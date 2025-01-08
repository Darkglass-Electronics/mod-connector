// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <cassert>
#include <string>

// clang doesn't support constexpr string functions
#ifdef __clang__
#define constexprstr
#else
#define constexprstr constexpr
#endif

// --------------------------------------------------------------------------------------------------------------------
// always valid assert even in release builds, returns a value on failure

[[maybe_unused]]
static inline
void _assert_print(const char* const expr, const char* const file, const int line)
{
    fprintf(stderr, "assertion failure: \"%s\" in file %s line %d\n", expr, file, line);
}

#ifdef assert
#define assert_continue(expr) assert(expr);
#define assert_return(expr, ret) assert(expr);
#else
#define assert_continue(expr) \
    { if (__builtin_expect(!(expr),0)) { _assert_print(#expr, __FILE__, __LINE__); }
#define assert_return(expr, ret) \
    { if (__builtin_expect(!(expr),0)) { _assert_print(#expr, __FILE__, __LINE__); return ret; } }
#endif

// --------------------------------------------------------------------------------------------------------------------
// convert bool to string

[[maybe_unused]]
static inline constexpr
const char* bool2str(const bool b) noexcept
{
    return b ? "true" : "false";
}

// --------------------------------------------------------------------------------------------------------------------
// check if an URI is null

[[maybe_unused]]
static inline constexpr
bool isNullURI(const char* const uri) noexcept
{
    return uri == nullptr || uri[0] == '\0' || (uri[0] == '-' && uri[1] == '\0');
}

[[maybe_unused]]
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
