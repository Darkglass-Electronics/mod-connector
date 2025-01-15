// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "utils.hpp"
#include "lv2.hpp"

#include <cstdarg>

// --------------------------------------------------------------------------------------------------------------------
// utilities for logging where levels 0:warn 1:info and 2:debug, adjustable by "MOD_LOG" env var

static int _get_mod_log_level()
{
    if (const char* const log = std::getenv("MOD_LOG"))
        if (*log != '\0')
            return std::atoi(log);
    return 0;
}

int _mod_log_level()
{
    static int level = _get_mod_log_level();
    return level;
}

// --------------------------------------------------------------------------------------------------------------------
// utility function that formats a std::string via `vsnprintf`

std::string format(const char* format, ...)
{
    std::string ret;

    va_list args, args2;
    va_start(args, format);
    va_copy(args2, args);

    const int size = std::vsnprintf(nullptr, 0, format, args);
    if (size > 0)
    {
        ret.resize(size);
        std::vsnprintf(ret.data(), size + 1, format, args2);
    }

    va_end(args);
    va_end(args2);

    return ret;
}

// --------------------------------------------------------------------------------------------------------------------
// check if a plugin has compatible IO, while also filling info regarding IO

bool getSupportedPluginIO(const Lv2Plugin* const plugin,
                          uint8_t& numInputs,
                          uint8_t& numOutputs,
                          uint8_t& numSideInputs,
                          uint8_t& numSideOutputs)
{
    assert(plugin != nullptr);

    numInputs = numOutputs = numSideInputs = numSideOutputs = 0;
    for (const Lv2Port& port : plugin->ports)
    {
        if ((port.flags & Lv2PortIsAudio) == 0)
            continue;

        if ((port.flags & Lv2PortIsSidechain) != 0)
        {
            if (++((port.flags & Lv2PortIsOutput) != 0 ? numSideOutputs : numSideInputs) > 1)
                break;
        }
        else
        {
            if (++((port.flags & Lv2PortIsOutput) != 0 ? numOutputs : numInputs) > 2)
                break;
        }
    }

    if (numInputs == 0 || numOutputs == 0)
        return false;
    if (numInputs > 2 || numOutputs > 2)
        return false;
    if (numSideInputs > 1 || numSideOutputs > 1)
        return false;

    return true;
}
// --------------------------------------------------------------------------------------------------------------------
