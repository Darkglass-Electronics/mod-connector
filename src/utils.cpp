// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "utils.hpp"

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

int _mod_log()
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
        std::vsnprintf(&ret[0], size + 1, format, args2);
    }

    va_end(args);
    va_end(args2);

    return ret;
}

// --------------------------------------------------------------------------------------------------------------------
