// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <string>

// --------------------------------------------------------------------------------------------------------------------
// check if an URI is null

static inline constexpr
bool isNullURI(const char* const uri) noexcept
{
    return uri == nullptr || uri[0] == '\0' || (uri[0] == '-' && uri[1] == '\0');
}

static inline
bool isNullURI(const std::string& uri)
{
    return uri.empty() || uri == "-";
}

// --------------------------------------------------------------------------------------------------------------------
// utility function that formats a std::string via `vsnprintf`

#ifdef __MINGW32__
__attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)))
#else
__attribute__((format(printf, 1, 2)))
#endif
std::string format(const char* format, ...);

// --------------------------------------------------------------------------------------------------------------------
