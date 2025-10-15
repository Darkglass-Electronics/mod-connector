// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

// clang doesn't support constexpr string functions
#ifdef __clang__
#define constexprstr
#else
#define constexprstr constexpr
#endif

struct Lv2Plugin;

// --------------------------------------------------------------------------------------------------------------------
// always valid assert even in release builds, returns a value on failure

[[maybe_unused]]
static inline
void _assert_print(const char* const expr, const char* const file, const int line)
{
    fprintf(stderr, "assertion failure: \"%s\" in file %s line %d\n", expr, file, line);
}

#ifndef NDEBUG
#define assert_continue(expr) { assert(expr); }
#define assert_return(expr, ret) { assert(expr); }
#else
#define assert_continue(expr) \
    { if (__builtin_expect(!(expr),0)) { _assert_print(#expr, __FILE__, __LINE__); continue; } }
#define assert_return(expr, ret) \
    { if (__builtin_expect(!(expr),0)) { _assert_print(#expr, __FILE__, __LINE__); return ret; } }
#endif

// --------------------------------------------------------------------------------------------------------------------
// convert bool to string

[[maybe_unused]]
[[nodiscard]]
static inline constexpr
const char* bool2str(const bool b) noexcept
{
    return b ? "true" : "false";
}

// --------------------------------------------------------------------------------------------------------------------
// check if an URI is null

[[maybe_unused]]
[[nodiscard]]
static inline constexpr
bool isNullURI(const char* const uri) noexcept
{
    return uri == nullptr || uri[0] == '\0' || (uri[0] == '-' && uri[1] == '\0');
}

[[maybe_unused]]
[[nodiscard]]
static inline
bool isNullURI(const std::string& uri)
{
    return uri.empty() || uri == "-";
}

// --------------------------------------------------------------------------------------------------------------------
// safely check if values are equal

template<typename T>
[[maybe_unused]]
[[nodiscard]]
static inline constexpr
bool isEqual(const T v1, const T v2) noexcept
{
    return std::abs(v1 - v2) < std::numeric_limits<T>::epsilon();
}

template<typename T>
[[maybe_unused]]
[[nodiscard]]
static inline constexpr
bool isNotEqual(const T v1, const T v2) noexcept
{
    return std::abs(v1 - v2) >= std::numeric_limits<T>::epsilon();
}

// --------------------------------------------------------------------------------------------------------------------
// utilities for logging where levels 0:warn 1:info and 2+:debug, adjustable by "MOD_LOG" env var

[[nodiscard]]
int _mod_log_level();

#ifdef NDEBUG
#define mod_log_debug(MSG, ...)
#define mod_log_debug3(MSG, ...)
#else
#define mod_log_debug(MSG, ...) \
    { if (_mod_log_level() >= 2) fprintf(stderr, "[" MOD_LOG_GROUP "] " MSG "\n" __VA_OPT__(,) __VA_ARGS__); }
#define mod_log_debug3(MSG, ...) \
    { if (_mod_log_level() >= 3) fprintf(stderr, "[" MOD_LOG_GROUP "] " MSG "\n" __VA_OPT__(,) __VA_ARGS__); }
#endif

#define mod_log_info(MSG, ...) \
    { if (_mod_log_level() >= 1) fprintf(stderr, "[" MOD_LOG_GROUP "] " MSG "\n" __VA_OPT__(,) __VA_ARGS__); }
#define mod_log_warn(MSG, ...) \
    { if (_mod_log_level() >= 0) fprintf(stderr, "[" MOD_LOG_GROUP "] " MSG "\n" __VA_OPT__(,) __VA_ARGS__); }

// --------------------------------------------------------------------------------------------------------------------
// utility function that formats a std::string via `vsnprintf`

[[nodiscard]]
#ifdef __MINGW32__
__attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)))
#else
__attribute__((format(printf, 1, 2)))
#endif
std::string format(const char* format, ...);

// --------------------------------------------------------------------------------------------------------------------
// check if a plugin has compatible IO, while also filling info regarding IO

[[nodiscard]]
bool getSupportedPluginIO(const Lv2Plugin* plugin,
                          uint8_t& numInputs,
                          uint8_t& numOutputs,
                          uint8_t& numSideInputs,
                          uint8_t& numSideOutputs);

// --------------------------------------------------------------------------------------------------------------------
// get a monotonically-increasing time in nanoseconds

uint64_t getTimeNS() noexcept;

// --------------------------------------------------------------------------------------------------------------------
