#include "hmi.hpp"
#include "utils.hpp"

// #include "mod-system-control/serial_io.h"
// #include "mod-system-control/serial_rw.h"

#include <libserialport.h>

#include <cassert>
#include <cstring>
#include <unistd.h>

// --------------------------------------------------------------------------------------------------------------------

namespace System {

bool reboot()
{
#ifdef LINUX_DEVICE
    static const char *const pathname = "/usr/sbin/reboot";
    static const char *argv[] = {pathname, nullptr};
    execv(pathname, const_cast<char *const *>(argv));

    // if we reach this point, the command failed
#endif

    return false;
}

bool rebootInRecoveryMode()
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
       #ifdef __EMSCRIPTEN__
        dummyDevMode = true;
        return;
       #endif

        if (const char* const dev = std::getenv("MOD_DEV_HOST"))
        {
            if (std::atoi(dev) != 0)
            {
                dummyDevMode = true;
                return;
            }
        }
    }

    ~Impl()
    {
        close();
    }

    void open(const char *serial, int baudrate)
    {
        if ((serialport = serial_open(serial, baudrate)) != nullptr)
            sp_flush(serialport, SP_BUF_BOTH);
    }

    void close()
    {
        if (serialport != nullptr)
        {
            serial_close(serialport);
            serialport = nullptr;
        }
    }

    bool poll()
    {
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

        return true;
    }

    bool writeMessageAndWait(const std::string& message)
    {
        if (dummyDevMode)
        {
            return true;
        }

        if (serialport == nullptr)
        {
            last_error = "serial port is not connected";
            return false;
        }

        return write_or_close(serialport, message.c_str());
    }

    // ----------------------------------------------------------------------------------------------------------------
    // message handling

private:
    bool parseAndReplyToMessage()
    {
        // ignore responses
        if (strncmp(buf, CMD_RESPONSE, 2) == 0)
            return true;

        // ignored calls, just return 0
        if (0
            || strcmp(buf, CMD_GUI_CONNECTED) == 0
            || strcmp(buf, CMD_GUI_DISCONNECTED) == 0
            || strcmp(buf, CMD_PING) == 0
            || strncmp(buf, CMD_DUO_BOOT, 5) == 0
            || strncmp(buf, CMD_INITIAL_STATE, 3) == 0
            || strncmp(buf, CMD_MENU_ITEM_CHANGE, 2) == 0
            || strncmp(buf, CMD_PEDALBOARD_NAME_SET, 3) == 0)
            return write_or_close(serialport, "r 0");

        // clear all addressings
        if (strcmp(buf, CMD_PEDALBOARD_CLEAR) == 0)
        {
            for (int i = 0; i < NUM_BINDING_ACTUATORS; ++i)
                actuators[i] = {};

            HMICallbackData d = { HMICallbackData::kPedalboardClear, {} };
            callback->hmiCallback(d);

            return write_or_close(serialport, "r 0");
        }

        // reboot into restore mode
        if (strcmp(buf, CMD_RESTORE) == 0)
        {
            System::rebootInRecoveryMode();
            return write_or_close(serialport, "r 0");
        }

        // this is ugly and hacky, I don't care for now

        if (strncmp(buf, CMD_CONTROL_ADD, 2) == 0)
        {
            buf[1] = buf[3] = '\0';

            const int hw_id = std::atoi(buf + 2);
            char *sep = buf + 4;
            char *name, *unit;

            if (*sep == '\"')
            {
                name = sep + 1;
                sep = std::strchr(name, '\"');
                *sep++ = '\0';
            }
            else
            {
                name = sep;
                sep = std::strchr(name, ' ');
            }
            *sep++ = '\0';
            actuators[hw_id].name = name;

            actuators[hw_id].flags = std::atoi(sep);
            sep = std::strchr(sep, ' ') + 1;

            if (*sep == '\"')
            {
                unit = sep + 1;
                sep = std::strchr(unit, '\"');
                *sep++ = '\0';
            }
            else
            {
                unit = sep;
                sep = std::strchr(unit, ' ');
            }
            *sep++ = '\0';
            actuators[hw_id].unit = unit;

            actuators[hw_id].current = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;

            actuators[hw_id].max = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;

            actuators[hw_id].min = std::atof(sep);
            sep = std::strchr(sep, ' ') + 1;

            actuators[hw_id].steps = std::atoi(sep);
            // sep = std::strchr(sep, ' ') + 1;

            LOG("HMI control add %d \"%s\" \"%s\" %d %f %f %f %d",
                hw_id, name, unit, actuators[hw_id].flags,
                actuators[hw_id].current, actuators[hw_id].max, actuators[hw_id].min, actuators[hw_id].steps);

            actuators[hw_id].assigned = true;

            HMICallbackData d = { HMICallbackData::kControlAdd, {} };
            d.controlAdd.hw_id = hw_id;
            callback->hmiCallback(d);

            return write_or_close(serialport, "r 0");
        }

        if (strncmp(buf, CMD_CONTROL_REMOVE, 2) == 0)
        {
            const int hw_id = std::atoi(buf + 2);

            LOG("HMI control remove %d", hw_id);

            actuators[hw_id] = {};

            HMICallbackData d = { HMICallbackData::kControlRemove, {} };
            d.controlRemove.hw_id = hw_id;
            callback->hmiCallback(d);

            return write_or_close(serialport, "r 0");
        }

        if (strncmp(buf, CMD_CONTROL_SET, 2) == 0)
        {
            buf[1] = buf[3] = '\0';

            const int hw_id = std::atoi(buf + 2);
            const float value = std::atof(buf + 4);

            LOG("HMI control set %d %f", hw_id, value);

            actuators[hw_id].current = value;

            HMICallbackData d = { HMICallbackData::kControlSet, {} };
            d.controlSet.hw_id = hw_id;
            callback->hmiCallback(d);

            return write_or_close(serialport, "r 0");
        }

        fprintf(stderr, "%s: unknown message '%s'\n", __func__, buf);
        return write_or_close(serialport, "r -1");
    }

    // ----------------------------------------------------------------------------------------------------------------

    char buf[0xff] = {};
    bool debug = true;
    bool dummyDevMode = false;
    bool nonBlockingMode = false;
    uint16_t numNonBlockingOps = 0;

    std::array<ActuatorPage, NUM_BINDING_PAGES> &actuatorPages;
    uint8_t &page;

    Callback *const callback;
    struct sp_port *serialport = nullptr;

    friend class NonBlockingScope;
};

// --------------------------------------------------------------------------------------------------------------------

HMI::HMI(Callback *callback, const char *serial, int baudrate)
    : impl(new Impl(_actuators, _page, callback, last_error))
{
    impl->open(serial, baudrate);
}

HMI::~HMI() { delete impl; }

// --------------------------------------------------------------------------------------------------------------------

bool HMI::control_set(uint8_t hw_id, float value)
{
    assert(hw_id < NUM_BINDING_ACTUATORS);
    assert(_actuators[hw_id].assigned);

    _actuators[hw_id].current = value;

    if (impl->writeOrClose(format(CMD_CONTROL_SET, hw_id, value)))
    {
        HMICallbackData d = { HMICallbackData::kControlSet, {} };
        d.controlSet.hw_id = hw_id;
        _callback->hmiCallback(d);
        return true;
    }

    return false;
}

bool HMI::poll()
{
    return impl->poll();
}
