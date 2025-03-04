// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// --------------------------------------------------------------------------------------------------------------------

enum Lv2Category {
    kLv2CategoryNone = 0,
    kLv2CategoryDelay,
    kLv2CategoryDistortion,
    kLv2CategoryDistortionWaveshaper,
    kLv2CategoryDynamics,
    kLv2CategoryDynamicsAmplifier,
    kLv2CategoryDynamicsCompressor,
    kLv2CategoryDynamicsExpander,
    kLv2CategoryDynamicsGate,
    kLv2CategoryDynamicsLimiter,
    kLv2CategoryFilter,
    kLv2CategoryFilterAllpass,
    kLv2CategoryFilterBandpass,
    kLv2CategoryFilterComb,
    kLv2CategoryFilterEqualiser,
    kLv2CategoryFilterEqualiserMultiband,
    kLv2CategoryFilterEqualiserParametric,
    kLv2CategoryFilterHighpass,
    kLv2CategoryFilterLowpass,
    kLv2CategoryGenerator,
    kLv2CategoryGeneratorConstant,
    kLv2CategoryGeneratorInstrument,
    kLv2CategoryGeneratorOscillator,
    kLv2CategoryMIDI,
    kLv2CategoryModulator,
    kLv2CategoryModulatorChorus,
    kLv2CategoryModulatorFlanger,
    kLv2CategoryModulatorPhaser,
    kLv2CategoryReverb,
    kLv2CategorySimulator,
    kLv2CategorySpatial,
    kLv2CategorySpectral,
    kLv2CategorySpectralPitchShifter,
    kLv2CategoryUtility,
    kLv2CategoryUtilityAnalyser,
    kLv2CategoryUtilityConverter,
    kLv2CategoryUtilityFunction,
    kLv2CategoryUtilityMixer,
    kLv2CategoryCount
};

enum Lv2Designation {
    kLv2DesignationNone = 0,
    kLv2DesignationEnabled,
    kLv2DesignationBPM,
    kLv2DesignationReset,
    kLv2DesignationQuickPot,
};

enum Lv2Flags {
    // port flags
    Lv2PortIsAudio          = 1 << 0,
    Lv2PortIsControl        = 1 << 1,
    Lv2PortIsOutput         = 1 << 2,
    Lv2PortIsSidechain      = 1 << 3,
    // property flags
    Lv2PropertyIsPath       = 1 << 0,
    Lv2PropertyIsParameter  = 1 << 1,
    Lv2PropertyIsReadOnly   = 1 << 2,
    // common flags
    Lv2ParameterToggled     = 1 << 4,
    Lv2ParameterInteger     = 1 << 5,
    Lv2ParameterEnumerated  = 1 << 6,
    Lv2ParameterLogarithmic = 1 << 7,
    Lv2ParameterHidden      = 1 << 8,
};

struct Lv2ScalePoint {
    std::string label;
    float value = 0.f;
};

struct Lv2Port {
    std::string symbol;
    std::string name;
    std::string shortname;
    uint32_t flags = 0;
    Lv2Designation designation = kLv2DesignationNone;
    float def = 0.f;
    float min = 0.f;
    float max = 1.f;
    std::string unit;
    std::vector<Lv2ScalePoint> scalePoints;
};

struct Lv2Property {
    std::string uri;
    std::string name;
    std::string shortname;
    uint32_t flags = 0;
    // used for Lv2PropertyIsPath
    std::string defpath;
    // used for Lv2PropertyIsParameter
    float def = 0.f;
    float min = 0.f;
    float max = 1.f;
};

struct Lv2Plugin {
    std::string uri;
    std::string name;
    std::string abbreviation;
    Lv2Category category = kLv2CategoryNone;
    std::vector<Lv2Port> ports;
    std::vector<Lv2Property> properties;
};

struct Lv2World {
   /**
    * string describing the last error, in case any operation fails.
    */
    std::string last_error;

   /* get the amount of lv2 plugins
    */
    [[nodiscard]] uint32_t getPluginCount() const noexcept;
    [[deprecated]] inline uint32_t get_plugin_count() const noexcept
    {
        return getPluginCount();
    }

   /* get the plugin @a index
    * can return null in case of error or the plugin requires unsupported features
    */
    [[nodiscard]] const Lv2Plugin* getPluginByIndex(uint32_t index) const;
    [[deprecated]] inline const Lv2Plugin* get_plugin_by_index(uint32_t index) const
    {
        return getPluginByIndex(index);
    }

   /* get the plugin with a known uri
    * can return null in case of error or the plugin requires unsupported features
    */
    [[nodiscard]] const Lv2Plugin* getPluginByURI(const char* uri) const;
    [[deprecated]] inline const Lv2Plugin* get_plugin_by_uri(const char* uri) const
    {
        return getPluginByURI(uri);
    }

   /* get the plugin port with a known symbol
    */
    [[nodiscard]] const Lv2Port& getPluginPort(const char* uri, const char* symbol) const;

   /* load a plugin state from disk and return a symbol -> value map
    */
    [[nodiscard]] std::unordered_map<std::string, float> loadPluginState(const char* path) const;
    [[deprecated]] inline std::unordered_map<std::string, float> load_plugin_state(const char* path) const
    {
        return loadPluginState(path);
    }

    Lv2World();
    ~Lv2World();

private:
    struct Impl;
    Impl* const impl;
};

// --------------------------------------------------------------------------------------------------------------------

[[maybe_unused]]
static constexpr inline
const char* lv2_category_name(Lv2Category category)
{
    switch (category)
    {
    case kLv2CategoryNone:
        return "None";
    case kLv2CategoryDelay:
        return "Delay";
    case kLv2CategoryDistortion:
        return "Distortion";
    case kLv2CategoryDistortionWaveshaper:
        return "Distortion, Waveshaper";
    case kLv2CategoryDynamics:
        return "Dynamics";
    case kLv2CategoryDynamicsAmplifier:
        return "Dynamics, Amplifier";
    case kLv2CategoryDynamicsCompressor:
        return "Dynamics, Compressor";
    case kLv2CategoryDynamicsExpander:
        return "Dynamics, Expander";
    case kLv2CategoryDynamicsGate:
        return "Dynamics, Gate";
    case kLv2CategoryDynamicsLimiter:
        return "Dynamics, Limiter";
    case kLv2CategoryFilter:
        return "Filter";
    case kLv2CategoryFilterAllpass:
        return "Filter, Allpass";
    case kLv2CategoryFilterBandpass:
        return "Filter, Bandpass";
    case kLv2CategoryFilterComb:
        return "Filter, Comb";
    case kLv2CategoryFilterEqualiser:
        return "Filter, Equaliser";
    case kLv2CategoryFilterEqualiserMultiband:
        return "Filter, Equaliser, Multiband";
    case kLv2CategoryFilterEqualiserParametric:
        return "Filter, Equaliser, Parametric";
    case kLv2CategoryFilterHighpass:
        return "Filter, Highpass";
    case kLv2CategoryFilterLowpass:
        return "Filter, Lowpass";
    case kLv2CategoryGenerator:
        return "Generator";
    case kLv2CategoryGeneratorConstant:
        return "Generator, Constant";
    case kLv2CategoryGeneratorInstrument:
        return "Generator, Instrument";
    case kLv2CategoryGeneratorOscillator:
        return "Generator, Oscillator";
    case kLv2CategoryMIDI:
        return "MIDI";
    case kLv2CategoryModulator:
        return "Modulator";
    case kLv2CategoryModulatorChorus:
        return "Modulator, Chorus";
    case kLv2CategoryModulatorFlanger:
        return "Modulator, Flanger";
    case kLv2CategoryModulatorPhaser:
        return "Modulator, Phaser";
    case kLv2CategoryReverb:
        return "Reverb";
    case kLv2CategorySimulator:
        return "Simulator";
    case kLv2CategorySpatial:
        return "Spatial";
    case kLv2CategorySpectral:
        return "Spectral";
    case kLv2CategorySpectralPitchShifter:
        return "Spectral, Pitch Shifter";
    case kLv2CategoryUtility:
        return "Utility";
    case kLv2CategoryUtilityAnalyser:
        return "Utility, Analyser";
    case kLv2CategoryUtilityConverter:
        return "Utility, Converter";
    case kLv2CategoryUtilityFunction:
        return "Utility, Function";
    case kLv2CategoryUtilityMixer:
        return "Utility, Mixer";
    case kLv2CategoryCount:
        break;
    }

    return "";
}

// --------------------------------------------------------------------------------------------------------------------
