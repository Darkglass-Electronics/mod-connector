// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// --------------------------------------------------------------------------------------------------------------------

enum Lv2Category {
    kLv2CategoryNone = 0,
    kLv2CategoryFilter,
    kLv2CategoryReverb,
    kLv2CategoryUtility,
    kLv2CategoryCount
};

enum Lv2Designation {
    kLv2DesignationNone = 0,
    kLv2DesignationEnabled,
    kLv2DesignationQuickPot,
};

enum Lv2Flags {
    Lv2PortIsAudio      = 1 << 1,
    Lv2PortIsControl    = 1 << 2,
    Lv2PortIsOutput     = 1 << 3,
    Lv2ParameterToggled = 1 << 4,
    Lv2ParameterInteger = 1 << 5,
    Lv2ParameterHidden  = 1 << 6,
};

struct Lv2Port {
    std::string symbol;
    std::string name;
    uint32_t flags = 0;
    Lv2Designation designation = kLv2DesignationNone;
    float def = 0.f;
    float min = 0.f;
    float max = 1.f;
    std::string unit; // TODO
};

struct Lv2Plugin {
    std::string uri;
    std::string name;
    Lv2Category category = kLv2CategoryNone;
    std::vector<Lv2Port> ports;
};

struct Lv2World {
   /**
    * string describing the last error, in case any operation fails.
    */
    std::string last_error;

   /* get the amount of lv2 plugins
    */
    uint32_t get_plugin_count() const noexcept;
 
   /* get the plugin @a index
    * can return null in case of error or the plugin requires unsupported features
    */
    const Lv2Plugin* get_plugin_by_index(uint32_t index) const;

   /* get the plugin with a known uri
    * can return null in case of error or the plugin requires unsupported features
    */
    const Lv2Plugin* get_plugin_by_uri(const char* uri) const;

    Lv2World();
    ~Lv2World();

private:
    struct Impl;
    Impl* const impl;
};

// --------------------------------------------------------------------------------------------------------------------

static constexpr inline
const char* lv2_category_name(Lv2Category category)
{
    switch (category)
    {
    case kLv2CategoryNone:
        return "None";
    case kLv2CategoryFilter:
        return "Filter";
    case kLv2CategoryReverb:
        return "Reverb";
    case kLv2CategoryUtility:
        return "Utility";
    case kLv2CategoryCount:
        break;
    }

    return "";
}

// --------------------------------------------------------------------------------------------------------------------
