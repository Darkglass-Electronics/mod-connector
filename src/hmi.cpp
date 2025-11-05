// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#define MOD_LOG_GROUP "hmi"

#include "hmi.hpp"
#include "ipc.hpp"
#include "utils.hpp"

#include <cassert>
#include <cstring>
#include <memory>
#include <unistd.h>

// --------------------------------------------------------------------------------------------------------------------

namespace System {

#ifdef LINUX_DEVICE
static bool reboot()
{
    static const char *const pathname = "/usr/sbin/reboot";
    static const char *argv[] = {pathname, nullptr};
    execv(pathname, const_cast<char *const *>(argv));

    // if we reach this point, the command failed
    return false;
}
#endif

static bool rebootInRecoveryMode()
{
#ifdef LINUX_DEVICE
    if (FILE *f = std::fopen("/data/boot-restore", "w"))
    {
        std::fclose(f);
        return reboot();
    }
#endif

    return false;
}

}

// --------------------------------------------------------------------------------------------------------------------

struct HMIProto::Impl
{
    std::string& last_error;

    Impl(std::string& last_error_)
        : last_error(last_error_)
    {
    }

    ~Impl()
    {
        close();
    }

    bool open(const char* const serial, const int baudrate)
    {
        if (ipc == nullptr)
            ipc = std::make_unique<IPC>(serial, baudrate);

        last_error = ipc->last_error;

        if (last_error.empty())
            return true;

        ipc = nullptr;
        return false;
    }

    void close()
    {
        ipc.reset();
    }

    // ----------------------------------------------------------------------------------------------------------------
    // message handling

    void setWriteBlockingAndWait(const bool blocking)
    {
        ipc->setWriteBlockingAndWait(blocking);
    }

    bool writeMessageAndWait(const std::string& message,
                             const IPC::ResponseType respType = IPC::kResponseNone,
                             IPC::Response* const resp = nullptr)
    {
        if (ipc->writeMessage(message, respType, resp))
            return true;

        last_error = ipc->last_error;
        return false;
    }

    // ----------------------------------------------------------------------------------------------------------------

    bool poll(Callback* const callback)
    {
        std::string error;

        while (_poll(callback, error)) {}

        return error.empty();
    }

private:
    [[nodiscard]] bool _poll(Callback* const callback, std::string& error) const
    {
        uint32_t bytesRead;
        char* const buffer = ipc->readMessage(&bytesRead);

        if (buffer == nullptr)
        {
            error = ipc->last_error;
            return false;
        }

        if (std::strcmp(buffer, CMD_PING) == 0)
        {
            return _writeReply("r 0");
        }

        if (std::strcmp(buffer, CMD_GUI_CONNECTED) == 0)
        {
            HMICallbackData d = { HMICallbackData::kConnected, {} };
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strcmp(buffer, CMD_GUI_DISCONNECTED) == 0)
        {
            HMICallbackData d = { HMICallbackData::kDisconnected, {} };
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strncmp(buffer, CMD_CONTROL_ADD, 2) == 0)
        {
            buffer[1] = buffer[3] = '\0';

            const int hw_id = std::atoi(buffer + 2);
            assert(hw_id < NUM_BINDING_ACTUATORS);

            HMICallbackData d = { HMICallbackData::kControlAdd, {} };
            d.controlAdd.hw_id = hw_id;

            char *sep = buffer + 4;
            char *name, *unit;

            if (*sep == '\"')
            {
                name = sep + 1;
                sep = std::strchr(name, '\"');
                assert(sep != nullptr);
                *sep++ = '\0';
            }
            else
            {
                name = sep;
                sep = std::strchr(name, ' ');
                assert(sep != nullptr);
            }
            *sep++ = '\0';
            d.controlAdd.label = name;

            d.controlAdd.flags = std::atoi(sep);
            sep = std::strchr(sep, ' ');
            assert(sep != nullptr);
            ++sep;

            if (*sep == '\"')
            {
                unit = sep + 1;
                sep = std::strchr(unit, '\"');
                assert(sep != nullptr);
                *sep++ = '\0';
            }
            else
            {
                unit = sep;
                sep = std::strchr(unit, ' ');
                assert(sep != nullptr);
            }
            *sep++ = '\0';
            d.controlAdd.unit = unit;

            d.controlAdd.current = std::atof(sep);
            sep = std::strchr(sep, ' ');
            assert(sep != nullptr);
            ++sep;

            d.controlAdd.max = std::atof(sep);
            sep = std::strchr(sep, ' ');
            assert(sep != nullptr);
            ++sep;

            d.controlAdd.min = std::atof(sep);
            sep = std::strchr(sep, ' ');
            assert(sep != nullptr);
            ++sep;

            d.controlAdd.steps = std::atoi(sep);
            // sep = std::strchr(sep, ' ') + 1;

            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strncmp(buffer, CMD_CONTROL_REMOVE, 2) == 0)
        {
            const int hw_id = std::atoi(buffer + 2);
            assert(hw_id < NUM_BINDING_ACTUATORS);

            HMICallbackData d = { HMICallbackData::kControlRemove, {} };
            d.controlRemove.hw_id = hw_id;
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strncmp(buffer, CMD_CONTROL_SET, 2) == 0)
        {
            buffer[1] = buffer[3] = '\0';

            const int hw_id = std::atoi(buffer + 2);
            assert(hw_id < NUM_BINDING_ACTUATORS);

            HMICallbackData d = { HMICallbackData::kControlSet, {} };
            d.controlSet.hw_id = hw_id;
            d.controlSet.value = std::atof(buffer + 4);
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strncmp(buffer, CMD_INITIAL_STATE, 3) == 0)
        {
            char *sep = buffer + 3;

            mod_log_warn("TODO initialState '%s'", sep);

            HMICallbackData d = { HMICallbackData::kInitialState, {} };

            d.initialState.numPedalboards = std::atoi(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            d.initialState.paginationStart = std::atoi(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            d.initialState.paginationEnd = std::atoi(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            d.initialState.bankId = std::atoi(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            d.initialState.pedalboardId = std::atoi(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            // TODO pedalboard list

            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strncmp(buffer, CMD_PEDALBOARD_NAME_SET, 3) == 0)
        {
            char* name;
            char* sep = buffer + 3;

            if (*sep == '\"')
            {
                name = sep + 1;
                sep = std::strchr(name, '\"');
                assert(sep != nullptr);
                *sep++ = '\0';
            }
            else
            {
                name = sep;
            }

            HMICallbackData d = { HMICallbackData::kPedalboardNameSet, {} };
            d.pedalboardNameSet.name = name;
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strcmp(buffer, CMD_PEDALBOARD_CLEAR) == 0)
        {
            HMICallbackData d = { HMICallbackData::kPedalboardClear, {} };
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strcmp(buffer, CMD_RESTORE) == 0)
        {
            System::rebootInRecoveryMode();
            return _writeReply("r 0");
        }

        mod_log_warn("unknown mod-ui message '%s'", buffer);
        return _writeReply("r 0");
    }

    [[nodiscard]] bool _writeReply(const std::string& reply) const
    {
        if (ipc->writeMessageWithoutReply(reply))
            return true;

        last_error = ipc->last_error;
        return false;
    }

    // ----------------------------------------------------------------------------------------------------------------

    std::unique_ptr<IPC> ipc;

    friend class NonBlockingScope;
};

// --------------------------------------------------------------------------------------------------------------------

HMIProto::HMIProto(const char* const serial, const int baudrate)
    : impl(new Impl(last_error))
{
    impl->open(serial, baudrate);
}

HMIProto::~HMIProto() { delete impl; }

// --------------------------------------------------------------------------------------------------------------------

HMIProto::NonBlockingScope::NonBlockingScope(HMIProto& hmi_)
    : hmi(hmi_)
{
    hmi.impl->setWriteBlockingAndWait(false);
}

HMIProto::NonBlockingScope::~NonBlockingScope()
{
    hmi.impl->setWriteBlockingAndWait(true);
}

// --------------------------------------------------------------------------------------------------------------------

bool HMIProto::control_set(const uint8_t hw_id, const float value)
{
    return impl->writeMessageAndWait(format(CMD_CONTROL_SET, hw_id, value));
}

bool HMIProto::control_page(const uint8_t hw_id, const uint32_t prop_bitmask, const uint8_t page_index_id)
{
    return impl->writeMessageAndWait(format(CMD_CONTROL_PAGE, hw_id, prop_bitmask, page_index_id));
}

bool HMIProto::pedalboard_load(const uint32_t bank_id, const uint32_t pb_id)
{
    return impl->writeMessageAndWait(format("pb %d %d", bank_id, pb_id));
}

std::vector<std::string> HMIProto::pedalboards(const bool up_page,
                                               const uint32_t current_page_index,
                                               const uint32_t bank_uid)
{
    IPC::Response resp = {};
    if (impl->writeMessageAndWait(format(CMD_PEDALBOARDS, up_page ? 1 : 0, current_page_index, bank_uid),
                                  IPC::kResponseString,
                                  &resp))
    {
        fprintf(stderr, "pedalboards RESP: %s\n", resp.data.s);

        char* s = resp.data.s;
        char* sep = std::strchr(s, ' ');
        assert(sep != nullptr);
        ++sep;

        const int respcode = std::atoi(sep);
        fprintf(stderr, "pedalboards respcode: %d\n", respcode);
        if (respcode != 1)
            return {};

        sep = std::strchr(sep, ' ');
        assert(sep != nullptr);
        ++sep;

        const int numPedalboards = std::atoi(sep);
        sep = std::strchr(sep, ' ');
        assert(sep != nullptr);
        ++sep;

        const int paginationStart = std::atoi(sep);
        sep = std::strchr(sep, ' ');
        assert(sep != nullptr);
        ++sep;

        const int paginationEnd = std::atoi(sep);

        fprintf(stderr, "pedalboards numPedalboards: %d, paginationStart: %d, paginationEnd: %d\n",
                numPedalboards, paginationStart, paginationEnd);

        std::vector<std::string> pedalboards;
        pedalboards.reserve(numPedalboards);

        char* name;
        for (int i = paginationStart; i < paginationEnd; ++i)
        {
            sep = std::strchr(sep, ' ');
            assert(sep != nullptr);
            ++sep;

            if (*sep == '\"')
            {
                name = sep + 1;
                sep = std::strchr(name, '\"');
                assert(sep != nullptr);
                *sep++ = '\0';
            }
            else
            {
                name = sep;
                sep = std::strchr(name, ' ');
                assert(sep != nullptr);
            }
            *sep++ = '\0';

            pedalboards.push_back(name);

            sep = std::strchr(sep, ' ');
            if (sep == nullptr)
                break;
        }

        return pedalboards;
    }

    return {};
}

bool HMIProto::_poll(Callback* const callback)
{
    return impl->poll(callback);
}

// --------------------------------------------------------------------------------------------------------------------

HMI::HMI(Callback* const callback, const char* const serial, const int baudrate)
    : HMIProto(serial, baudrate),
      _callback(callback)
{
}

bool HMI::poll()
{
    return _poll(this);
}

bool HMI::control_set(const uint8_t hw_id, const float value)
{
    assert(hw_id < NUM_BINDING_ACTUATORS);

    ActuatorPage& actuatorPageH = _actuatorPages[_actuatorPage];
    assert(actuatorPageH.active);
    assert(hw_id < actuatorPageH.actuators.size());

    Actuator& actuator = actuatorPageH.actuators[hw_id];
    assert(actuator.assigned);

    if (! HMIProto::control_set(hw_id, value))
        return false;

    actuator.current = value;

    HMICallbackData data = { HMICallbackData::kControlSet, {} };
    data.controlSet.hw_id = hw_id;
    data.controlSet.value = value;
    _callback->hmiCallback(data);

    return true;
}

void HMI::hmiCallback(const Data &data)
{
    switch (data.type)
    {
    case HMICallbackData::kConnected:
        _webConnected = true;
        break;

    case HMICallbackData::kDisconnected:
        _webConnected = false;
        break;

    case HMICallbackData::kControlAdd:
    {
        const uint8_t hw_id = data.controlAdd.hw_id;
        assert(hw_id < NUM_BINDING_ACTUATORS);

        ActuatorPage& actuatorPageH = _actuatorPages[_actuatorPage];
        assert(hw_id < actuatorPageH.actuators.size());

        Actuator& actuator = actuatorPageH.actuators[hw_id];
        assert(!actuator.assigned);

        actuator.assigned = true;
        actuator.label = data.controlAdd.label;
        actuator.unit = data.controlAdd.unit;
        actuator.flags = data.controlAdd.flags;
        actuator.current = data.controlAdd.current;
        actuator.min = data.controlAdd.min;
        actuator.max = data.controlAdd.max;
        actuator.steps = data.controlAdd.steps;

        actuatorPageH.active = true;
        break;
    }

    case HMICallbackData::kControlRemove:
    {
        const uint8_t hw_id = data.controlRemove.hw_id;
        assert(hw_id < NUM_BINDING_ACTUATORS);

        ActuatorPage& actuatorPageH = _actuatorPages[_actuatorPage];
        assert(hw_id < actuatorPageH.actuators.size());

        Actuator& actuator = actuatorPageH.actuators[hw_id];
        assert(actuator.assigned);

        actuator = {};

        bool active = false;
        for (int i = 0; i < NUM_BINDING_ACTUATORS; ++i)
        {
            if (actuatorPageH.actuators[i].assigned)
            {
                active = true;
                break;
            }
        }

        actuatorPageH.active = active;
        break;
    }

    case HMICallbackData::kControlSet:
    {
        const uint8_t hw_id = data.controlSet.hw_id;
        assert(hw_id < NUM_BINDING_ACTUATORS);

        ActuatorPage& actuatorPageH = _actuatorPages[_actuatorPage];
        assert(actuatorPageH.active);
        assert(hw_id < actuatorPageH.actuators.size());

        Actuator& actuator = actuatorPageH.actuators[hw_id];
        assert(actuator.assigned);

        actuator.current = data.controlSet.value;
        break;
    }

    case HMICallbackData::kInitialState:
        _bankId = data.initialState.bankId;
        _numPedalboardsInBank = data.initialState.numPedalboards;
        _pedalboardId = data.initialState.pedalboardId;
        break;

    case HMICallbackData::kPedalboardNameSet:
        _pedalboardName = data.pedalboardNameSet.name;
        break;

    case HMICallbackData::kPedalboardClear:
        for (int i = 0; i < NUM_BINDING_PAGES; ++i)
        {
            _actuatorPages[i].active = false;
            for (int j = 0; j < NUM_BINDING_ACTUATORS; ++j)
                _actuatorPages[i].actuators[j] = {};
        }
        break;
    }

    _callback->hmiCallback(data);
}
