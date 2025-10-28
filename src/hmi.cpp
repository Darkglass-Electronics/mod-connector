// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#define MOD_LOG_GROUP "hmi"

#include "hmi.hpp"
#include "ipc.hpp"
#include "utils.hpp"

// #include "mod-system-control/serial_io.h"
// #include "mod-system-control/serial_rw.h"

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

        // web-ui is connected
        if (std::strcmp(buffer, CMD_GUI_CONNECTED) == 0)
        {
            HMICallbackData d = { HMICallbackData::kConnected, {} };
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        // web-ui is disconnected
        if (std::strcmp(buffer, CMD_GUI_DISCONNECTED) == 0)
        {
            HMICallbackData d = { HMICallbackData::kDisconnected, {} };
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        // assign a new hw control
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
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

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
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            d.controlAdd.max = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            d.controlAdd.min = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            d.controlAdd.steps = std::atoi(sep);
            // sep = std::strchr(sep, ' ') + 1;

            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        // unassign a hw control
        if (std::strncmp(buffer, CMD_CONTROL_REMOVE, 2) == 0)
        {
            const int hw_id = std::atoi(buffer + 2);
            assert(hw_id < NUM_BINDING_ACTUATORS);

            HMICallbackData d = { HMICallbackData::kControlRemove, {} };
            d.controlRemove.hw_id = hw_id;
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        // send assigned control data
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

        // clear all addressings
        if (std::strcmp(buffer, CMD_PEDALBOARD_CLEAR) == 0)
        {
            HMICallbackData d = { HMICallbackData::kPedalboardClear, {} };
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        // reboot into restore mode
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

bool HMIProto::control_set(uint8_t hw_id, float value)
{
    return impl->writeMessageAndWait(format(CMD_CONTROL_SET, hw_id, value));
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

        ActuatorPage& actuatorPage = _actuatorPages[_page];
        assert(hw_id < actuatorPage.actuators.size());

        Actuator& actuator = actuatorPage.actuators[hw_id];
        assert(!actuator.assigned);

        actuator.assigned = true;
        actuator.label = data.controlAdd.label;
        actuator.unit = data.controlAdd.unit;
        actuator.flags = data.controlAdd.flags;
        actuator.current = data.controlAdd.current;
        actuator.min = data.controlAdd.min;
        actuator.max = data.controlAdd.max;
        actuator.steps = data.controlAdd.steps;

        actuatorPage.active = true;
        break;
    }

    case HMICallbackData::kControlRemove:
    {
        const uint8_t hw_id = data.controlRemove.hw_id;
        assert(hw_id < NUM_BINDING_ACTUATORS);

        ActuatorPage& actuatorPage = _actuatorPages[_page];
        assert(hw_id < actuatorPage.actuators.size());

        Actuator& actuator = actuatorPage.actuators[hw_id];
        assert(actuator.assigned);

        actuator = {};

        bool active = false;
        for (int i = 0; i < NUM_BINDING_ACTUATORS; ++i)
        {
            if (actuatorPage.actuators[i].assigned)
            {
                active = true;
                break;
            }
        }

        actuatorPage.active = active;
        break;
    }

    case HMICallbackData::kControlSet:
    {
        const uint8_t hw_id = data.controlSet.hw_id;
        assert(hw_id < NUM_BINDING_ACTUATORS);

        ActuatorPage& actuatorPage = _actuatorPages[_page];
        assert(actuatorPage.active);
        assert(hw_id < actuatorPage.actuators.size());

        Actuator& actuator = actuatorPage.actuators[hw_id];
        assert(actuator.assigned);

        actuator.current = data.controlSet.value;
        break;
    }

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
