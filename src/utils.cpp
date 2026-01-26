// SPDX-FileCopyrightText: 2024-2026 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "utils.hpp"
#include "lv2.hpp"

#include <cstdarg>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <ctime>
#include <pwd.h>
#include <unistd.h>
#endif

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
// get home directory, return value will be cached

std::string homedir()
{
    static std::string _homedir;
    static bool _once = true;

    if (_once)
    {
        _once = false;
       #ifdef _WIN32
        WCHAR wpath[MAX_PATH + 256];

        if (SHGetSpecialFolderPathW(nullptr, wpath, CSIDL_MYDOCUMENTS, FALSE))
        {
            CHAR apath[MAX_PATH + 256];

            if (WideCharToMultiByte(CP_UTF8, 0, wpath, -1, apath, MAX_PATH + 256, nullptr, nullptr))
                _homedir = apath;
        }
       #else
        if (const char* const home = std::getenv("HOME"))
            _homedir = home;
        else if (struct passwd* const pwd = getpwuid(getuid()))
            _homedir = pwd->pw_dir;
       #endif
    }

    return _homedir;
}

// --------------------------------------------------------------------------------------------------------------------
// check if a file path resides inside a known directory

bool path_contains(const std::string& path, const std::string& dir)
{
    return !dir.empty() &&
        path.length() > dir.length() &&
        path.at(dir.length() - 1) == PATH_SEP_CHAR &&
        std::strncmp(path.c_str(), dir.c_str(), dir.length()) == 0;
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

#ifndef MOD_CONNECTOR_MINIMAL_LV2_WORLD
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
#endif

// --------------------------------------------------------------------------------------------------------------------
// get a monotonically-increasing time in nanoseconds

uint64_t getTimeNS() noexcept
{
   #if defined(_WIN32)
    static struct {
      LARGE_INTEGER freq;
      LARGE_INTEGER counter;
      BOOL r1, r2;
    } s = { {}, {}, QueryPerformanceFrequency(&s.freq), QueryPerformanceCounter(&s.counter) };

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart - s.counter.QuadPart) * 1000000000ULL / s.freq.QuadPart;
   #elif defined(__APPLE__)
    static const uint64_t s = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - s;
   #else
    static struct {
        timespec ts;
        int r;
        uint64_t ns;
    } s = { {}, clock_gettime(CLOCK_MONOTONIC, &s.ts), static_cast<uint64_t>(s.ts.tv_sec * 1000000000ULL +
                                                                             s.ts.tv_nsec) };
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000000000ULL + ts.tv_nsec) - s.ns;
   #endif
}

// --------------------------------------------------------------------------------------------------------------------
