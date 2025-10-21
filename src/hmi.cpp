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

static bool reboot()
{
#ifdef LINUX_DEVICE
    static const char *const pathname = "/usr/sbin/reboot";
    static const char *argv[] = {pathname, nullptr};
    execv(pathname, const_cast<char *const *>(argv));

    // if we reach this point, the command failed
#endif

    return false;
}

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

struct HMI::Impl
{
    std::string& last_error;

    Impl(std::array<ActuatorPage, NUM_BINDING_PAGES> &actuatorPages_,
         uint8_t &page_,
         Callback *callback_,
         std::string& last_error_)
        : last_error(last_error_),
          actuatorPages(actuatorPages_),
          page(page_),
          callback(callback_)
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
        return ipc->last_error.empty();
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

    bool poll()
    {
        std::string error;

        while (_poll(error)) {}

        return error.empty();
#if 0
        // poll serial port
        while (serialport != nullptr)
        {
            bool reading = true;
            switch (serial_read_msg_until_zero(serialport, buf, debug))
            {
                case SP_READ_ERROR_NO_DATA:
                    reading = false;
                    break;

                case SP_READ_ERROR_INVALID_DATA:
                    reading = false;
                    serial_read_ignore_until_zero(serialport);
                    break;

                case SP_READ_ERROR_IO:
                    return false;
            }

            if (! reading)
                break;
            if (! parseAndReplyToMessage())
                return false;
        }
#endif
    }

private:
    [[nodiscard]] bool _poll(std::string& error) const
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

        // clear all addressings
        if (std::strcmp(buffer, CMD_PEDALBOARD_CLEAR) == 0)
        {
            for (int i = 0; i < NUM_BINDING_PAGES; ++i)
            {
                actuatorPages[i].active = false;
                for (int j = 0; j < NUM_BINDING_ACTUATORS; ++j)
                    actuatorPages[i].actuators[j] = {};
            }

            HMICallbackData d = { HMICallbackData::kPedalboardClear, {} };
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strncmp(buffer, CMD_CONTROL_ADD, 2) == 0)
        {
            buffer[1] = buffer[3] = '\0';

            const int hw_id = std::atoi(buffer + 2);
            assert(hw_id < NUM_BINDING_ACTUATORS);

            Actuator& actuator = actuatorPages[page].actuators[hw_id];

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
            actuator.name = name;

            actuator.flags = std::atoi(sep);
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
            actuator.unit = unit;

            actuator.current = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            actuator.max = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            actuator.min = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;
            assert(sep != nullptr);

            actuator.steps = std::atoi(sep);
            // sep = std::strchr(sep, ' ') + 1;

            mod_log_warn("HMI control add %d \"%s\" \"%s\" %d %f %f %f %d",
                         hw_id, name, unit, actuator.flags,
                         actuator.current, actuator.max, actuator.min, actuator.steps);

            actuator.assigned = true;

            HMICallbackData d = { HMICallbackData::kControlAdd, {} };
            d.controlAdd.hw_id = hw_id;
            callback->hmiCallback(d);

            return _writeReply("r 0");
        }

        if (std::strncmp(buffer, CMD_CONTROL_REMOVE, 2) == 0)
        {
            const int hw_id = std::atoi(buffer + 2);
            assert(hw_id < NUM_BINDING_ACTUATORS);

            Actuator& actuator = actuatorPages[page].actuators[hw_id];

            mod_log_warn("HMI control remove %d", hw_id);

            actuator = {};

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

            Actuator& actuator = actuatorPages[page].actuators[hw_id];

            const float value = std::atof(buffer + 4);

            mod_log_warn("HMI control set %d %f", hw_id, value);

            actuator.current = value;

            HMICallbackData d = { HMICallbackData::kControlSet, {} };
            d.controlSet.hw_id = hw_id;
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

    std::array<ActuatorPage, NUM_BINDING_PAGES> &actuatorPages;
    uint8_t &page;

    Callback *const callback;

    // ----------------------------------------------------------------------------------------------------------------

    std::unique_ptr<IPC> ipc;

    friend class NonBlockingScope;
};

// --------------------------------------------------------------------------------------------------------------------

HMI::HMI(Callback *callback, const char* const serial, const int baudrate)
    : impl(new Impl(_actuatorPages, _page, callback, last_error))
{
    impl->open(serial, baudrate);
}

HMI::~HMI() { delete impl; }

// --------------------------------------------------------------------------------------------------------------------

HMI::NonBlockingScope::NonBlockingScope(HMI& hmi_)
    : hmi(hmi_)
{
    hmi.impl->setWriteBlockingAndWait(false);
}

HMI::NonBlockingScope::~NonBlockingScope()
{
    hmi.impl->setWriteBlockingAndWait(true);
}

// --------------------------------------------------------------------------------------------------------------------

bool HMI::control_set(uint8_t hw_id, float value)
{
    assert(hw_id < NUM_BINDING_ACTUATORS);
    assert(_actuatorPages[hw_id].active);
    assert(_page < _actuatorPages[hw_id].actuators.size());

    Actuator& actuator = _actuatorPages[hw_id].actuators[_page];
    assert(actuator.assigned);

    actuator.current = value;

    if (impl->writeMessageAndWait(format(CMD_CONTROL_SET, hw_id, value)))
    {
        // HMICallbackData d = { HMICallbackData::kControlSet, {} };
        // d.controlSet.hw_id = hw_id;
        // _callback->hmiCallback(d);
        return true;
    }

    return false;
}

bool HMI::poll()
{
    return impl->poll();
}
