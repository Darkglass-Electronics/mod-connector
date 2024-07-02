// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <string>

// --------------------------------------------------------------------------------------------------------------------
// utility function that formats a std::string via `vsnprintf`

#ifdef __MINGW32__
__attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)))
#else
__attribute__((format(printf, 1, 2)))
#endif
std::string format(const char* format, ...);

// --------------------------------------------------------------------------------------------------------------------
