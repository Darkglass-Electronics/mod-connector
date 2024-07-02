// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "utils.hpp"

#include <cstdarg>

// --------------------------------------------------------------------------------------------------------------------
// utility function that formats a std::string via `vsnprintf`

std::string format(const char* format, ...)
{
    std::string ret;

    va_list args, args2;
    va_start(args, format);
    va_copy(args2, args);

    const int size = std::vsnprintf(NULL, 0, format, args);
    if (size > 0)
    {
        ret.resize(size + 1);
        std::vsnprintf(&ret[0], size + 1, format, args2);
    }

    va_end(args);
    va_end(args2);

    return ret;
}

// --------------------------------------------------------------------------------------------------------------------
