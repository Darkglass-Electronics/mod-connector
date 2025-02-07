// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#define MOD_LOG_GROUP "connector"

#include "connector.hpp"
#include "json.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

// Possible values for LV2_KXSTUDIO_PROPERTIES__Reset
typedef enum {
    LV2_KXSTUDIO_PROPERTIES_RESET_NONE = 0, // No reset
    LV2_KXSTUDIO_PROPERTIES_RESET_FULL = 1, // Full reset
    LV2_KXSTUDIO_PROPERTIES_RESET_SOFT = 2  // Soft reset, e.g. reset filter state but do not clear audio buffers
} LV2_KXStudio_Properties_Reset;

#define JSON_PRESET_VERSION_CURRENT 0
#define JSON_PRESET_VERSION_MIN_SUPPORTED 0
#define JSON_PRESET_VERSION_MAX_SUPPORTED 0

#ifdef BINDING_ACTUATOR_IDS
static constexpr const char* kBindingActuatorIDs[NUM_BINDING_ACTUATORS] = { BINDING_ACTUATOR_IDS };
#endif

using ParameterBindingIterator = std::list<HostParameterBinding>::iterator;
using ParameterBindingIteratorConst = std::list<HostParameterBinding>::const_iterator;

using PropertyBindingIterator = std::list<HostPropertyBinding>::iterator;
using PropertyBindingIteratorConst = std::list<HostPropertyBinding>::const_iterator;

// --------------------------------------------------------------------------------------------------------------------

#ifndef _WIN32
static const char* getHomeDir()
{
    static std::string home;
    if (home.empty())
    {
        /**/ if (const char* const envhome = getenv("HOME"))
            home = envhome;
        else if (struct passwd* const pwd = getpwuid(getuid()))
            home = pwd->pw_dir;
    }
    return home.c_str();
}
#endif

static std::string getDefaultPluginBundleForBlock(const HostBlock& blockdata)
{
   #if defined(_WIN32)
    WCHAR wpath[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, wpath) == S_OK)
        return format("%ls\\LV2\\default-%s.lv2", wpath, blockdata.meta.abbreviation.c_str());
    return {};
   #elif defined(__APPLE__)
    return format("%s/Library/Audio/Plug-Ins/LV2/default-%s.lv2", getHomeDir(), blockdata.meta.abbreviation.c_str());
   #else
    return format("%s/.lv2/default-%s.lv2", getHomeDir(), blockdata.meta.abbreviation.c_str());
   #endif
}

// --------------------------------------------------------------------------------------------------------------------

static std::array<unsigned char, UUID_SIZE> generateUUID()
{
    std::array<unsigned char, UUID_SIZE> uuid;

    for (int i = 0; i < UUID_SIZE / 2; ++i)
        *reinterpret_cast<uint16_t*>(uuid.data() + i * 2) = rand();

    // make it standards compliant
    uuid[6] = 0x40 | (uuid[6] & 0x0f);
    uuid[8] = 0x80 | (uuid[8] & 0x3f);

    return uuid;
}

static std::array<unsigned char, UUID_SIZE> str2uuid(const std::string& uuidstr)
{
    assert_return(uuidstr.length() == UUID_SIZE * 2 + 3, generateUUID());

    std::array<unsigned char, UUID_SIZE> uuid;
    unsigned char* const uuidptr = uuid.data();

    if (std::sscanf(uuidstr.c_str(),
                    "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx-",
                    uuidptr,
                    uuidptr + 1,
                    uuidptr + 2,
                    uuidptr + 3,
                    uuidptr + 4,
                    uuidptr + 5,
                    uuidptr + 6,
                    uuidptr + 7) != 8)
    {
        mod_log_warn("failed to read uuid1: %s", uuidstr.c_str());
        return generateUUID();
    }

    if (std::sscanf(uuidstr.c_str() + (8 * 2 + 1),
                    "%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx-",
                    uuidptr + 8,
                    uuidptr + 9,
                    uuidptr + 10,
                    uuidptr + 11,
                    uuidptr + 12,
                    uuidptr + 13,
                    uuidptr + 14,
                    uuidptr + 15) != 8)
    {
        mod_log_warn("failed to read uuid2: %s", uuidstr.c_str() + (8 * 2 + 1));
        return generateUUID();
    }

    if (std::sscanf(uuidstr.c_str() + (16 * 2 + 3),
                    "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
                    uuidptr + 16,
                    uuidptr + 17,
                    uuidptr + 18,
                    uuidptr + 19,
                    uuidptr + 20,
                    uuidptr + 21,
                    uuidptr + 22,
                    uuidptr + 23,
                    uuidptr + 24,
                    uuidptr + 25,
                    uuidptr + 26,
                    uuidptr + 27) != 12)
    {
        mod_log_warn("failed to read uuid3: %s", uuidstr.c_str() + (16 * 2 + 3));
        return generateUUID();
    }

    return uuid;
}

static std::string uuid2str(const std::array<unsigned char, UUID_SIZE>& uuid)
{
    return format("%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx-",
                  uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7])
        + format("%02hhx%02hhx%02hhx%02hhx-", uuid[8], uuid[9], uuid[10], uuid[11])
        + format("%02hhx%02hhx%02hhx%02hhx-", uuid[12], uuid[13], uuid[14], uuid[15])
        + format("%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
                 uuid[16], uuid[17], uuid[18], uuid[19], uuid[20], uuid[21],
                 uuid[22], uuid[23], uuid[24], uuid[25], uuid[26], uuid[27]);
}

// --------------------------------------------------------------------------------------------------------------------

static void resetParameter(HostConnector::Parameter& paramdata)
{
    paramdata = {};
    paramdata.meta.hwbinding = UINT8_MAX;
    paramdata.meta.max = 1.f;
}

static void resetProperty(HostConnector::Property& propdata)
{
    propdata = {};
    propdata.meta.hwbinding = UINT8_MAX;
}

static void resetBlock(HostConnector::Block& blockdata)
{
    blockdata.enabled = false;
    blockdata.uri.clear();
    blockdata.quickPotSymbol.clear();
    blockdata.meta.enable.hasScenes = false;
    blockdata.meta.enable.hwbinding = UINT8_MAX;
    blockdata.meta.quickPotIndex = 0;
    blockdata.meta.numParamsInScenes = 0;
    blockdata.meta.numPropertiesInScenes = 0;
    blockdata.meta.numInputs = 0;
    blockdata.meta.numOutputs = 0;
    blockdata.meta.numSideInputs = 0;
    blockdata.meta.numSideOutputs = 0;
    blockdata.meta.name.clear();
    blockdata.meta.abbreviation.clear();

    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetParameter(blockdata.parameters[p]);

    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetProperty(blockdata.properties[p]);

    for (uint8_t s = 0; s < NUM_SCENES_PER_PRESET; ++s)
        blockdata.sceneValues[s].enabled = false;
}

static void allocBlock(HostConnector::Block& blockdata)
{
    blockdata.parameters.resize(MAX_PARAMS_PER_BLOCK);
    blockdata.properties.resize(MAX_PARAMS_PER_BLOCK);

    for (uint8_t s = 0; s < NUM_SCENES_PER_PRESET; ++s)
    {
        blockdata.sceneValues[s].params.resize(MAX_PARAMS_PER_BLOCK);
        blockdata.sceneValues[s].properties.resize(MAX_PARAMS_PER_BLOCK);
    }
}

// --------------------------------------------------------------------------------------------------------------------

static bool isNullBlock(const HostConnector::Block& blockdata)
{
    return isNullURI(blockdata.uri);
}

// --------------------------------------------------------------------------------------------------------------------

static constexpr const char* SceneMode2Str(const HostSceneMode sceneMode)
{
    switch (sceneMode)
    {
    case HostConnector::SceneModeNone:
        return "SceneModeNone";
    case HostConnector::SceneModeActivate:
        return "SceneModeActivate";
    case HostConnector::SceneModeClear:
        return "SceneModeClear";
    }

    return "";
}

// --------------------------------------------------------------------------------------------------------------------

template <class Meta>
static inline constexpr float normalized(const Meta& meta, float value)
{
    return value <= meta.min ? 0.f
        : value >= meta.max ? 1.f
        : (value - meta.min) / (meta.max - meta.min);
}

static bool shouldBlockBeStereo(const HostConnector::ChainRow& chaindata, const uint8_t block)
{
    assert(block <= NUM_BLOCKS_PER_PRESET);

    if (chaindata.capture[0] != chaindata.capture[1])
        return true;

    for (uint8_t bl = block - 1; bl != UINT8_MAX; --bl)
    {
        if (isNullBlock(chaindata.blocks[bl]))
            continue;
        if (chaindata.blocks[bl].meta.numOutputs == 2)
            return true;
    }

    return false;
}

// --------------------------------------------------------------------------------------------------------------------

static bool loadPresetFromFile(const char* const filename, nlohmann::json& j)
{
    std::ifstream f(filename);
    if (f.fail())
        return false;

    try {
        j = nlohmann::json::parse(f);
    } catch (...) {
        return false;
    }

    if (! j.contains("preset"))
    {
        mod_log_warn("failed to load \"%s\": missing required fields 'preset'", filename);
        return false;
    }

    if (! j.contains("type"))
    {
        mod_log_warn("failed to load \"%s\": missing required field 'type'", filename);
        return false;
    }

    if (! j.contains("version"))
    {
        mod_log_warn("failed to load \"%s\": missing required field 'version'", filename);
        return false;
    }

    try {
        if (j["type"].get<std::string>() != "preset")
        {
            mod_log_warn("loadPresetFromFile(\"%s\"): failed, file is not preset type", filename);
            return false;
        }

        const int version = j["version"].get<int>();
        if (version < JSON_PRESET_VERSION_MIN_SUPPORTED || version > JSON_PRESET_VERSION_MAX_SUPPORTED)
        {
            mod_log_warn("loadPresetFromFile(\"%s\"): failed, version mismatch", filename);
            return false;
        }

        j = j["preset"].get<nlohmann::json>();
    } catch (const std::exception& e) {
        mod_log_warn("failed to parse \"%s\": %s", filename, e.what());
        return false;
    } catch (...) {
        mod_log_warn("failed to parse \"%s\": unknown exception", filename);
        return false;
    }

    return true;
}

static bool safeJsonSave(const nlohmann::json& json, const std::string& filename)
{
    if (FILE* const fd = std::fopen((filename + ".tmp").c_str(), "w"))
    {
        const std::string jsonstr = json.dump(2, ' ', false, nlohmann::detail::error_handler_t::replace);

        std::fwrite(jsonstr.c_str(), 1, jsonstr.length(), fd);
        std::fflush(fd);
       #ifndef _WIN32
        fsync(fileno(fd));
        // syncfs(fileno(fd));
       #endif
        std::fclose(fd);
        std::rename((filename + ".tmp").c_str(), filename.c_str());
        return true;
    }

    return false;
}

// --------------------------------------------------------------------------------------------------------------------

HostConnector::HostConnector()
{
    for (uint8_t p = 0; p < NUM_PRESETS_PER_BANK; ++p)
    {
        allocPreset(_presets[p]);
        resetPreset(_presets[p]);
    }

    allocPreset(_current);
    resetPreset(_current);

    ok = _host.last_error.empty();
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::reconnect()
{
    return _host.reconnect();
}

// --------------------------------------------------------------------------------------------------------------------

std::string HostConnector::getBlockId(const uint8_t row, const uint8_t block) const
{
    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);

    if (hbp.id == kMaxHostInstances)
        return {};

    if (hbp.pair == kMaxHostInstances)
        return format("effect_%u", hbp.id);

    return format("effect_%u + effect_%u", hbp.id, hbp.pair);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::printStateForDebug(const bool withBlocks, const bool withParams, const bool withBindings) const
{
    if (_mod_log_level() < 3)
        return;

    fprintf(stderr, "------------------------------------------------------------------\n");
    fprintf(stderr, "Dumping current state:\n");
    fprintf(stderr, "\tPreset: %u\n", _current.preset);
    fprintf(stderr, "\tScene: %u\n", _current.scene);
    fprintf(stderr, "\tNum loaded plugins: %u\n", _current.numLoadedPlugins);
    fprintf(stderr, "\tDirty: %s\n", bool2str(_current.dirty));
    fprintf(stderr, "\tFilename: %s\n", _current.filename.c_str());
    fprintf(stderr, "\tName: %s\n", _current.name.c_str());

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS && (withBlocks || withParams); ++row)
    {
       #if NUM_BLOCK_CHAIN_ROWS != 1
        fprintf(stderr, "\n\t--- Row %u\n", row);
       #endif

        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET && (withBlocks || withParams); ++bl)
        {
            const Block& blockdata(_current.chains[row].blocks[bl]);

            if (isNullBlock(blockdata))
            {
                fprintf(stderr, "\n\tBlock %u: (empty)\n", bl);
                continue;
            }

            fprintf(stderr, "\n\tBlock %u: %s | %s\n", bl, blockdata.uri.c_str(), blockdata.meta.name.c_str());

            if (withBlocks)
            {
                fprintf(stderr, "\t\tQuick Pot: '%s' | %u\n", blockdata.quickPotSymbol.c_str(), blockdata.meta.quickPotIndex);
                fprintf(stderr, "\t\tnumParamsInScenes: %u\n", blockdata.meta.numParamsInScenes);
                fprintf(stderr, "\t\tnumPropertiesInScenes: %u\n", blockdata.meta.numPropertiesInScenes);
                fprintf(stderr, "\t\tnumInputs: %u\n", blockdata.meta.numInputs);
                fprintf(stderr, "\t\tnumOutputs: %u\n", blockdata.meta.numOutputs);
                fprintf(stderr, "\t\tnumSideInputs: %u\n", blockdata.meta.numSideInputs);
                fprintf(stderr, "\t\tnumSideOutputs: %u\n", blockdata.meta.numSideOutputs);
            }

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK && withParams; ++p)
            {
                const Parameter& paramdata(blockdata.parameters[p]);

                fprintf(stderr, "\t\tParameter %u: '%s' | '%s'\n",
                        p, paramdata.symbol.c_str(), paramdata.meta.name.c_str());
                fprintf(stderr, "\t\t\tFlags: %x\n", paramdata.meta.flags);
                if (paramdata.meta.hwbinding != UINT8_MAX)
                {
                   #ifdef BINDING_ACTUATOR_IDS
                    fprintf(stderr, "\t\t\tHwBinding: %s\n", kBindingActuatorIDs[paramdata.meta.hwbinding]);
                   #else
                    fprintf(stderr, "\t\t\tHwBinding: %d\n", paramdata.meta.hwbinding);
                   #endif
                }
                else
                {
                    fprintf(stderr, "\t\t\tHwBinding: (none)\n");
                }
                fprintf(stderr, "\t\t\tDefault: %f\n", paramdata.meta.def);
                fprintf(stderr, "\t\t\tMinimum: %f\n", paramdata.meta.min);
                fprintf(stderr, "\t\t\tMaximum: %f\n", paramdata.meta.max);
                fprintf(stderr, "\t\t\tUnit: %s\n", paramdata.meta.unit.c_str());
            }

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK && withParams; ++p)
            {
                const Property& propdata(blockdata.properties[p]);

                fprintf(stderr, "\t\tProperty %u: '%s' | '%s'\n",
                        p, propdata.uri.c_str(), propdata.meta.name.c_str());
                fprintf(stderr, "\t\t\tFlags: %x\n", propdata.meta.flags);
                if (propdata.meta.hwbinding != UINT8_MAX)
                {
                   #ifdef BINDING_ACTUATOR_IDS
                    fprintf(stderr, "\t\t\tHwBinding: %s\n", kBindingActuatorIDs[propdata.meta.hwbinding]);
                   #else
                    fprintf(stderr, "\t\t\tHwBinding: %d\n", propdata.meta.hwbinding);
                   #endif
                }
                else
                {
                    fprintf(stderr, "\t\t\tHwBinding: (none)\n");
                }
            }
        }
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS && withBindings; ++hwid)
    {
       #ifdef BINDING_ACTUATOR_IDS
        const std::string hwname = kBindingActuatorIDs[hwid];
       #else
        const std::string hwname = std::to_string(hwid + 1);
       #endif
        fprintf(stderr, "\n\tBindings for '%s', value: %f:\n", hwname.c_str(), _current.bindings[hwid].value);

        if (_current.bindings[hwid].params.empty() && _current.bindings[hwid].properties.empty())
        {
            fprintf(stderr, "\t\t(empty)\n");
            continue;
        }

        for (const ParameterBinding& bindingdata : _current.bindings[hwid].params)
        {
            fprintf(stderr, "\t\t- Block %u, Parameter '%s' | %u\n",
                    bindingdata.block,
                    bindingdata.parameterSymbol.c_str(),
                    bindingdata.meta.parameterIndex);
        }

        for (const PropertyBinding& bindingdata : _current.bindings[hwid].properties)
        {
            fprintf(stderr, "\t\t- Block %u, Property '%s' | %u\n",
                    bindingdata.block,
                    bindingdata.propertyURI.c_str(),
                    bindingdata.meta.propertyIndex);
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

const HostConnector::Preset& HostConnector::getBankPreset(const uint8_t preset) const
{
    assert(preset < NUM_PRESETS_PER_BANK);

    return _presets[preset];
}

// --------------------------------------------------------------------------------------------------------------------

const HostConnector::Preset& HostConnector::getCurrentPreset(const uint8_t preset) const
{
    assert(preset < NUM_PRESETS_PER_BANK);

    return _current.preset != preset ? _presets[preset] : _current;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::canAddSidechainInput(const uint8_t row, const uint8_t block) const
{
    assert_return(row == 0, false);
    assert(block < NUM_BLOCKS_PER_PRESET);

    if (_current.numLoadedPlugins == 0)
        return false;

    // cannot add input/playback/sink on first block
    if (block == 0)
        return false;

    const ChainRow& chaindata(_current.chains[0]);

    // must have a matching sidechain output/capture/source before
    bool hasMatchingSource = false;
    for (uint8_t bl = block - 1; bl != UINT8_MAX; --bl)
    {
        const Block& blockdata(chaindata.blocks[bl]);
        if (isNullBlock(blockdata))
            continue;
        if (blockdata.meta.numSideInputs != 0)
            return false;
        if (blockdata.meta.numSideOutputs != 0)
        {
            hasMatchingSource = true;
            break;
        }
    }

    if (!hasMatchingSource)
        return false;

    // must NOT have sidechain input after
    for (uint8_t bl = block + 1; bl < NUM_BLOCKS_PER_PRESET; ++bl)
    {
        const Block& blockdata(chaindata.blocks[bl]);
        if (isNullBlock(blockdata))
            continue;
        if (blockdata.meta.numSideInputs != 0)
            return false;
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::canAddSidechainOutput(const uint8_t row, const uint8_t block) const
{
    assert_return(row == 0, false);
    assert(block < NUM_BLOCKS_PER_PRESET);

    if (_current.numLoadedPlugins == 0)
        return true;

    const ChainRow& chaindata(_current.chains[0]);

    // TODO limit amount of next chains, check for enough space, etc

    for (const Block& blockdata : chaindata.blocks)
    {
        if (isNullBlock(blockdata))
            continue;

        // TODO for now we only allow 1 sidechain output, because WIP development
        if (blockdata.meta.numSideOutputs != 0)
            return false;
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::loadBankFromPresetFiles(const std::array<std::string, NUM_PRESETS_PER_BANK>& filenames,
                                            const uint8_t initialPresetToLoad)
{
    assert(initialPresetToLoad < NUM_PRESETS_PER_BANK);
    mod_log_debug("loadBankFromPresetFiles(..., %u)", initialPresetToLoad);

    for (uint8_t pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
    {
        Preset& presetdata = _presets[pr];
        presetdata.filename = filenames[pr];

        nlohmann::json j;
        if (! loadPresetFromFile(filenames[pr].c_str(), j))
        {
            resetPreset(presetdata);
            continue;
        }

        jsonPresetLoad(presetdata, j);
    }

    // create current preset data from selected initial preset
    static_cast<Preset&>(_current) = _presets[initialPresetToLoad];
    _current.preset = initialPresetToLoad;

    const Host::NonBlockingScope hnbs(_host);
    hostClearAndLoadCurrentBank();
}

// --------------------------------------------------------------------------------------------------------------------

std::string HostConnector::getPresetNameFromFile(const char* const filename)
{
    mod_log_debug("getPresetNameFromFile(\"%s\")", filename);

    nlohmann::json j;
    if (! loadPresetFromFile(filename, j))
        return {};

    try {
        return j["name"].get<std::string>();
    } catch (...) {}

    return {};
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::loadCurrentPresetFromFile(const char* const filename, const bool replaceDefault)
{
    mod_log_debug("loadCurrentPresetFromFile(\"%s\")", filename);

    nlohmann::json j;
    if (! loadPresetFromFile(filename, j))
        return false;

    // store old active preset in memory before doing anything
    const Current old = _current;

    // load new preset data
    jsonPresetLoad(_current, j);
    _current.filename = filename;

    // switch old preset with new one
    hostSwitchPreset(old);

    if (replaceDefault)
        _presets[_current.preset] = _current;

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::preloadPresetFromFile(const uint8_t preset, const char* const filename)
{
    mod_log_debug("preloadPresetFromFile(%u, \"%s\")", preset, filename);
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(preset != _current.preset);

    // load initial json object
    nlohmann::json j;
    if (! loadPresetFromFile(filename, j))
        return false;

    // load preset data
    Preset presetdata;
    allocPreset(presetdata);
    jsonPresetLoad(presetdata, j);
    presetdata.filename = filename;

    // unload old preset
    {
        const Preset& oldpreset = _presets[preset];

        const Host::NonBlockingScope hnbs(_host);

        for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
        {
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                if (isNullBlock(oldpreset.chains[row].blocks[bl]))
                    continue;

                const HostBlockPair hbp = _mapper.remove(preset, row, bl);

                if (hbp.id != kMaxHostInstances)
                    _host.remove(hbp.id);

                if (hbp.pair != kMaxHostInstances)
                    _host.remove(hbp.pair);
            }
        }
    }

    // assign and preload new preset
    _presets[preset] = presetdata;

    {
        const Host::NonBlockingScope hnbs(_host);
        hostLoadPreset(preset);
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::saveCurrentPresetToFile(const char* const filename)
{
    mod_log_debug("saveCurrentPresetToFile(\"%s\")", filename);

    nlohmann::json j;
    try {
        j["version"] = JSON_PRESET_VERSION_CURRENT;
        j["type"] = "preset";
        j["preset"] = nlohmann::json::object({});
    } catch (...) {
        return false;
    }

    if (_current.dirty)
    {
        _current.dirty = false;
        _current.uuid = generateUUID();
    }

    // copy current data into preset data
    _presets[_current.preset] = static_cast<Preset&>(_current);

    jsonPresetSave(_current, j["preset"]);

    safeJsonSave(j, filename);
   #ifndef _WIN32
    sync();
   #endif

    _current.filename = filename;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::swapPresets(const uint8_t presetA, const uint8_t presetB)
{
    mod_log_debug("swapPresets(%u, %u)", presetA, presetB);
    assert(presetA < NUM_PRESETS_PER_BANK);
    assert(presetB < NUM_PRESETS_PER_BANK);
    assert(presetA != presetB);

    // swap data first
    std::swap(_presets[presetA], _presets[presetB]);
    _mapper.swapPresets(presetA, presetB);

    // swap filenames again so the keep the originals
    std::swap(_presets[presetA].filename, _presets[presetB].filename);

    // adjust data for current preset, if matching
    if (_current.preset == presetA)
    {
        _current.preset = presetB;
        _current.filename = _presets[presetB].filename;
    }
    else if (_current.preset == presetB)
    {
        _current.preset = presetA;
        _current.filename = _presets[presetA].filename;
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::saveCurrentPreset()
{
    mod_log_debug("saveCurrentPreset()");

    if (_current.filename.empty())
        return false;

    return saveCurrentPresetToFile(_current.filename.c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::clearCurrentPreset()
{
    mod_log_debug("clearCurrentPreset()");

    _current.uuid = generateUUID();

    if (_current.numLoadedPlugins == 0)
        return;

    const Host::NonBlockingScopeWithAudioFades hnbs(_host);

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            if (!isNullBlock(_current.chains[row].blocks[bl]))
                hostRemoveInstanceForBlock(row, bl);

            resetBlock(_current.chains[row].blocks[bl]);
        }
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        _current.bindings[hwid].value = 0.f;
        _current.bindings[hwid].params.clear();
        _current.bindings[hwid].properties.clear();
    }

    _current.scene = 0;
    _current.numLoadedPlugins = 0;
    _current.dirty = true;

    // direct connections
    hostConnectChainEndpoints(0);

}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::regenUUID()
{
    mod_log_debug("regenUUID()");

    _current.uuid = generateUUID();
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setCurrentPresetName(const char* const name)
{
    mod_log_debug("setCurrentPresetName(\"%s\")", name);

    if (_current.name == name)
        return;

    _current.name = name;
    _current.dirty = true;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setCurrentPresetFilename(const char* const filename)
{
    mod_log_debug("setCurrentPresetFilename(\"%s\")", filename);

    _presets[_current.preset].filename = _current.filename = filename;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::enableBlock(const uint8_t row, const uint8_t block, const bool enable, const SceneMode sceneMode)
{
    mod_log_debug("enableBlock(%u, %u, %s, %d:%s)", row, block, bool2str(enable), sceneMode, SceneMode2Str(sceneMode));
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata), false);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert_return(hbp.id != kMaxHostInstances, false);

    _current.dirty = true;

    switch (sceneMode)
    {
    case SceneModeNone:
        blockdata.sceneValues[_current.scene].enabled = enable;
        break;

    case SceneModeActivate:
        if (! blockdata.meta.enable.hasScenes)
        {
            ++blockdata.meta.numParamsInScenes;
            blockdata.meta.enable.hasScenes = true;

            // set original value for all other scenes
            for (uint8_t scene = 0; scene < NUM_SCENES_PER_PRESET; ++scene)
            {
                if (_current.scene == scene)
                    continue;
                blockdata.sceneValues[scene].enabled = blockdata.enabled;
            }
        }
        blockdata.sceneValues[_current.scene].enabled = enable;
        break;

    case SceneModeClear:
        if (blockdata.meta.enable.hasScenes)
        {
            --blockdata.meta.numParamsInScenes;
            blockdata.meta.enable.hasScenes = false;
        }
        break;
    }

    if (blockdata.meta.enable.hwbinding != UINT8_MAX)
    {
        Bindings& bindings(_current.bindings[blockdata.meta.enable.hwbinding]);
        assert(!bindings.params.empty());

        bindings.value = enable ? 1.f : 0.f;
    }

    blockdata.enabled = enable;
    blockdata.sceneValues[_current.scene].enabled = enable;

    _host.bypass(hbp.id, !enable);

    if (hbp.pair != kMaxHostInstances)
        _host.bypass(hbp.pair, !enable);

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::reorderBlock(const uint8_t row, const uint8_t orig, const uint8_t dest)
{
    mod_log_debug("reorderBlock(%u, %u, %u)", row, orig, dest);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(orig < NUM_BLOCKS_PER_PRESET);
    assert(dest < NUM_BLOCKS_PER_PRESET);

    if (orig == dest)
    {
        mod_log_warn("reorderBlock(%u, %u, %u) - orig == dest, rejected", row, orig, dest);
        return false;
    }

    ChainRow& chain = _current.chains[row];

    // check if we need to re-do any connections
    bool reconnect = false;
    const bool blockIsEmpty = isNullBlock(chain.blocks[orig]);
    const uint8_t left = std::min(orig, dest);
    const uint8_t right = std::max(orig, dest);
    const uint8_t blockStart = std::max(0, left - 1);
    const uint8_t blockEnd = std::min(NUM_BLOCKS_PER_PRESET - 1, right + 1);

    for (uint8_t i = blockStart; i <= blockEnd; ++i)
    {
        if (orig == i)
            continue;
        if (isNullBlock(chain.blocks[i]))
            continue;
        reconnect = true;
        break;
    }

    if (blockIsEmpty && ! reconnect)
    {
        mod_log_warn("reorderBlock(%u, %u, %u) - there is nothing to reorder, rejected", row, orig, dest);
        return false;
    }

    mod_log_info("reorderBlock(%u, %u, %u) - reconnect %s, blockIsEmpty %s, start %u, end %u",
                 row, orig, dest, bool2str(reconnect), bool2str(blockIsEmpty), blockStart, blockEnd);

    const Host::NonBlockingScopeWithAudioFades hnbs(_host);

    if (reconnect && ! blockIsEmpty)
    {
        hostDisconnectAllBlockInputs(row, orig);
        hostDisconnectAllBlockOutputs(row, orig);
    }

    // moving block backwards to the left
    // a b c d e! f
    // a b c e! d f
    // a b e! c d f
    // a e! b c d f
    if (orig > dest)
    {
        for (int i = orig; i > dest; --i)
        {
            if (reconnect && !isNullBlock(chain.blocks[i - 1]))
            {
                hostDisconnectAllBlockInputs(row, i - 1);
                hostDisconnectAllBlockOutputs(row, i - 1);
            }
            std::swap(chain.blocks[i], chain.blocks[i - 1]);
        }
    }

    // moving block forward to the right
    // a b! c d e f
    // a c b! d e f
    // a c d b! e f
    // a c d e b! f
    else
    {
        for (int i = orig; i < dest; ++i)
        {
            if (reconnect && !isNullBlock(chain.blocks[i + 1]))
            {
                hostDisconnectAllBlockInputs(row, i + 1);
                hostDisconnectAllBlockOutputs(row, i + 1);
            }
            std::swap(chain.blocks[i], chain.blocks[i + 1]);
        }
    }

    _mapper.reorder(_current.preset, row, orig, dest);

    if (reconnect)
        hostEnsureStereoChain(row, blockStart);

    // update bindings
    const auto updateBinding = [=](auto& bindingdata) {
        if (bindingdata.row != row)
            return;
        if (bindingdata.block < left || bindingdata.block > right)
            return;

        // block matches orig, moving it to dest
        if (bindingdata.block == orig)
            bindingdata.block = dest;

        // block matches dest, moving it by +1 or -1 accordingly
        else if (bindingdata.block == dest)
            bindingdata.block += orig > dest ? 1 : -1;

        // block > dest, moving +1
        else if (bindingdata.block > dest)
            ++bindingdata.block;

        // block < dest, moving -1
        else
            --bindingdata.block;
    };

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        for (ParameterBinding& bindingdata : _current.bindings[hwid].params)
            updateBinding(bindingdata);

        for (PropertyBinding& bindingdata : _current.bindings[hwid].properties)
            updateBinding(bindingdata);
    }

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::replaceBlock(const uint8_t row, const uint8_t block, const char* const uri)
{
    mod_log_debug("replaceBlock(%u, %u, \"%s\")", row, block, uri);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    ChainRow& chaindata(_current.chains[row]);
    assert_return(!chaindata.capture[0].empty(), false);

    Block& blockdata(chaindata.blocks[block]);

    // do not change blocks if attempting to replace plugin with itself
    if (blockdata.uri == uri)
    {
        mod_log_debug("replaceBlock(%u, %u, \"%s\"): uri matches old block, will not replace plugin",
                      row, block, uri);

        // reset plugin to defaults
        if (!isNullURI(uri))
        {
            std::vector<flushed_param> params;
            params.reserve(MAX_PARAMS_PER_BLOCK);

            const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
            assert_return(hbp.id != kMaxHostInstances, false);

            blockdata.meta.enable.hasScenes = false;
            blockdata.meta.enable.hwbinding = UINT8_MAX;
            blockdata.meta.numParamsInScenes = 0;
            blockdata.meta.numPropertiesInScenes = 0;

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                Parameter& paramdata(blockdata.parameters[p]);
                if (isNullURI(paramdata.symbol))
                    break;
                if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                    continue;

                paramdata.meta.flags &= ~Lv2ParameterInScene;

                if (paramdata.value != paramdata.meta.def)
                {
                    paramdata.value = paramdata.meta.def;
                    params.push_back({ paramdata.symbol.c_str(), paramdata.meta.def });
                }
            }

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                Property& propdata(blockdata.properties[p]);
                if (isNullURI(propdata.uri))
                    break;
                if ((propdata.meta.flags & Lv2PropertyIsReadOnly) != 0)
                    continue;

                propdata.meta.flags &= ~Lv2ParameterInScene;

                // TODO reset properties too
            }

            const Host::NonBlockingScopeWithAudioFades hnbs(_host);

            if (!blockdata.enabled)
            {
                blockdata.enabled = true;

                _host.bypass(hbp.id, false);

                if (hbp.pair != kMaxHostInstances)
                    _host.bypass(hbp.pair, false);
            }

            _host.params_flush(hbp.id, LV2_KXSTUDIO_PROPERTIES_RESET_FULL, params.size(), params.data());

            if (hbp.pair != kMaxHostInstances)
                _host.params_flush(hbp.pair, LV2_KXSTUDIO_PROPERTIES_RESET_FULL, params.size(), params.data());
        }

        return true;
    }

    // check if we can remove the block, might be refused due to sidechain setup
    if (blockdata.meta.numSideOutputs != 0)
    {
        for (uint8_t bl = block + 1; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            if (chaindata.blocks[bl].meta.numSideInputs != 0)
            {
                mod_log_warn("replaceBlock(%u, %u, \"%s\"): cannot remove block, paired with a sidechain input",
                             row, block, uri);
                return false;
            }
        }
    }

    // store for later use after we change change blockdata
    const uint8_t oldNumSideInputs = blockdata.meta.numSideInputs;

    const Host::NonBlockingScopeWithAudioFades hnbs(_host);

    if (!isNullURI(uri))
    {
        const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(uri);
        assert_return(plugin != nullptr, false);

        // we only do changes after verifying that the requested plugin exists and is valid
        uint8_t numInputs, numOutputs, numSideInputs, numSideOutputs;
        if (!getSupportedPluginIO(plugin, numInputs, numOutputs, numSideInputs, numSideOutputs))
        {
            mod_log_warn("replaceBlock(%u, %u, %s): unsupported IO, rejected", row, block, uri);
            return false;
        }

        if (!isNullBlock(blockdata))
        {
            --_current.numLoadedPlugins;
            hostRemoveAllBlockBindings(row, block);
            hostRemoveInstanceForBlock(row, block);
        }

        // activate dual mono if previous plugin is stereo or also dualmono
        bool dualmono = false;
        if (numInputs == 1) {
            dualmono = shouldBlockBeStereo(chaindata, block);
            if ((numSideInputs != 0) && 
                !dualmono && 
                (row + 1 < NUM_BLOCK_CHAIN_ROWS)) {
                // if dual mono wasn't enforced by current row, it might be enforced by next row
                ChainRow& chain2data(_current.chains[row + 1]);
                dualmono = shouldBlockBeStereo(chain2data, NUM_BLOCKS_PER_PRESET);
            }
        }

        HostBlockPair hbp = { _mapper.add(_current.preset, row, block), kMaxHostInstances };

        bool added = _host.add(uri, hbp.id);
        if (added)
        {
            mod_log_debug("block %u loaded plugin %s", block, uri);

            if (dualmono)
            {
                hbp.pair = _mapper.add_pair(_current.preset, row, block);

                if (! _host.add(uri, hbp.pair))
                {
                    mod_log_warn("block %u failed to load dual-mono plugin %s: %s",
                                 block, uri, _host.last_error.c_str());

                    added = false;
                    _host.remove(hbp.id);
                }
            }
        }
        else
        {
            mod_log_warn("block %u failed to load plugin %s: %s", block, uri, _host.last_error.c_str());
        }

        if (added)
        {
            ++_current.numLoadedPlugins;
            initBlock(blockdata, plugin, numInputs, numOutputs, numSideInputs, numSideOutputs);
            hostSetupSideIO(row, block, hbp, plugin);
        }
        else
        {
            resetBlock(blockdata);
            _mapper.remove(_current.preset, row, block);
        }
    }
    else if (!isNullBlock(blockdata))
    {
        --_current.numLoadedPlugins;
        hostRemoveAllBlockBindings(row, block);
        hostRemoveInstanceForBlock(row, block);
        resetBlock(blockdata);
    }
    else
    {
        mod_log_warn("replaceBlock(%u, %u, %s): already empty, rejected", row, block, uri);
        return false;
    }

    if (!isNullBlock(blockdata))
    {
        // replace old direct connections if this is the first plugin
        if (_current.numLoadedPlugins == 1)
        {
            assert(row == 0);
            hostDisconnectChainEndpoints(0);
            hostConnectBlockToChainInput(row, block);
            hostConnectBlockToChainOutput(row, block);
        }
        // otherwise we need to add ourselves more carefully
        else
        {
            std::array<bool, NUM_BLOCKS_PER_PRESET> loaded;
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                loaded[bl] = !isNullBlock(chaindata.blocks[bl]);

            // find surrounding plugins
            uint8_t before = NUM_BLOCKS_PER_PRESET;
            if (block != 0)
            {
                for (uint8_t b = block - 1; b != UINT8_MAX; --b)
                {
                    if (loaded[b])
                    {
                        before = b;
                        break;
                    }
                }
            }

            uint8_t after = NUM_BLOCKS_PER_PRESET;
            if (block != NUM_BLOCKS_PER_PRESET - 1)
            {
                for (uint8_t bl = block + 1; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                {
                    if (loaded[bl])
                    {
                        after = bl;
                        break;
                    }
                }
            }

            mod_log_debug("replaceBlock add mode before: %u, after: %u | block: %u", before, after, block);

            if (after != NUM_BLOCKS_PER_PRESET)
                hostDisconnectAllBlockInputs(row, after);

            if (before != NUM_BLOCKS_PER_PRESET)
                hostDisconnectAllBlockOutputs(row, before);
            else
                before = 0;

            // disconnect end of next chain
            if (blockdata.meta.numSideInputs != 0)
            {
                assert(row + 1 < NUM_BLOCK_CHAIN_ROWS);

                ChainRow& chain2data(_current.chains[row + 1]);
                assert(!chain2data.capture[0].empty());

                uint8_t last = NUM_BLOCKS_PER_PRESET;
                for (uint8_t b = NUM_BLOCKS_PER_PRESET - 1; b != UINT8_MAX; --b)
                {
                    if (!isNullBlock(chain2data.blocks[b]))
                    {
                        last = b;
                        hostDisconnectAllBlockOutputs(row + 1, b);
                        break;
                    }
                }

                if (last == NUM_BLOCKS_PER_PRESET)
                {
                    _host.disconnect_all(chain2data.capture[0].c_str());

                    if (chain2data.capture[0] != chain2data.capture[1])
                        _host.disconnect_all(chain2data.capture[1].c_str());
                }

                hostEnsureStereoChain(row + 1, 0);
            }

            hostEnsureStereoChain(row, before);
        }

        if (blockdata.meta.numSideOutputs != 0)
        {
            assert(row + 1 < NUM_BLOCK_CHAIN_ROWS);
            hostEnsureStereoChain(row + 1, 0);
        }
    }
    else
    {
        // use direct connections if there are no plugins
        if (_current.numLoadedPlugins == 0)
        {
            hostConnectChainEndpoints(0);
        }
        else
        {
            std::array<bool, NUM_BLOCKS_PER_PRESET> loaded;
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                loaded[bl] = !isNullBlock(chaindata.blocks[bl]);

            // find previous plugin
            uint8_t start = 0;
            if (block != 0)
            {
                for (uint8_t b = block - 1; b != UINT8_MAX; --b)
                {
                    if (loaded[b])
                    {
                        start = b;
                        break;
                    }
                }
            }

            // connect end of next chain
            if (oldNumSideInputs != 0)
            {
                assert(row + 1 < NUM_BLOCK_CHAIN_ROWS);
                hostEnsureStereoChain(row + 1, NUM_BLOCK_CHAIN_ROWS - 1);
            }

            hostEnsureStereoChain(row, start);
        }
    }

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::saveBlockStateAsDefault(const uint8_t row, const uint8_t block)
{
    mod_log_debug("saveBlockStateAsDefault(%u, %u)", row, block);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert(hbp.id != kMaxHostInstances);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert(!isNullBlock(blockdata));

    // save live default values
    for (uint8_t rowB = 0; rowB < NUM_BLOCK_CHAIN_ROWS; ++rowB)
    {
        for (uint8_t blB = 0; blB < NUM_BLOCKS_PER_PRESET; ++blB)
        {
            Block& blockdataB(_current.chains[rowB].blocks[blB]);
            if (isNullBlock(blockdataB))
                continue;
            if (blockdataB.uri != blockdata.uri)
                continue;

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                Parameter& paramdata(blockdata.parameters[p]);
                if (isNullURI(paramdata.symbol))
                    break;
                if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                    continue;

                blockdataB.parameters[p].meta.def = paramdata.value;
            }

            // TODO update default properties
        }
    }

    // save default preset for next load
    assert_return(!blockdata.meta.abbreviation.empty(), false);

    const std::string defdir = getDefaultPluginBundleForBlock(blockdata);

    if (! _host.preset_save(hbp.id, "Default", defdir.c_str(), "default.ttl"))
        return false;

    // save any extra details in separate file
    nlohmann::json j;
    j["quickpot"] = blockdata.quickPotSymbol;
    safeJsonSave(j, defdir + "/defaults.json");

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

// #if NUM_BLOCK_CHAIN_ROWS != 1

bool HostConnector::swapBlockRow(const uint8_t row,
                                 const uint8_t block,
                                 const uint8_t emptyRow,
                                 const uint8_t emptyBlock)
{
    mod_log_debug("swapBlockRow(%u, %u, %u, %u)", row, block, emptyRow, emptyBlock);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(emptyRow < NUM_BLOCK_CHAIN_ROWS);
    assert(emptyBlock < NUM_BLOCKS_PER_PRESET);
    assert(row != emptyRow);
    assert(!_current.chains[row].capture[0].empty());
    assert(!_current.chains[emptyRow].capture[0].empty());
    assert(!isNullBlock(_current.chains[row].blocks[block]));
    assert(isNullBlock(_current.chains[emptyRow].blocks[emptyBlock]));

    // scope for fade-out, disconnect, reconnect, fade-in
    {
        const Host::NonBlockingScopeWithAudioFades hnbs(_host);

        // step 1: disconnect all ports from moving block
        hostDisconnectAllBlockInputs(row, block);
        hostDisconnectAllBlockOutputs(row, block);

        // step 2: disconnect sides of chain that gets new block
        if (emptyBlock != 0)
        {
            for (uint8_t bl = emptyBlock - 1; bl != 0; --bl)
            {
                if (isNullBlock(_current.chains[emptyRow].blocks[bl]))
                    continue;

                hostDisconnectAllBlockOutputs(emptyRow, bl);
                break;
            }
        }

        if (emptyBlock != NUM_BLOCKS_PER_PRESET - 1)
        {
            for (uint8_t bl = emptyBlock + 1; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                if (isNullBlock(_current.chains[emptyRow].blocks[bl]))
                    continue;

                hostDisconnectAllBlockInputs(emptyRow, bl);
                break;
            }
        }

        // step 3: swap data
        std::swap(_current.chains[row].blocks[block], _current.chains[emptyRow].blocks[emptyBlock]);

        for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
        {
            for (ParameterBinding& bindingdata : _current.bindings[hwid].params)
            {
                if (bindingdata.row == row)
                    bindingdata.row = emptyRow;
                else if (bindingdata.row == emptyRow)
                    bindingdata.row = row;
            }

            for (PropertyBinding& bindingdata : _current.bindings[hwid].properties)
            {
                if (bindingdata.row == row)
                    bindingdata.row = emptyRow;
                else if (bindingdata.row == emptyRow)
                    bindingdata.row = row;
            }
        }

        _mapper.swapBlocks(_current.preset, row, block, emptyRow, emptyBlock);

        for (Bindings& bindings : _current.bindings)
        {
            for (ParameterBinding& bindingdata : bindings.params)
            {
                if (bindingdata.row == row && bindingdata.block == block)
                {
                    bindingdata.row = emptyRow;
                    bindingdata.block = emptyBlock;
                }
            }

            for (PropertyBinding& bindingdata : bindings.properties)
            {
                if (bindingdata.row == row && bindingdata.block == block)
                {
                    bindingdata.row = emptyRow;
                    bindingdata.block = emptyBlock;
                }
            }
        }

        // step 4: reconnect ports
        hostEnsureStereoChain(row, emptyBlock - (emptyBlock != 0 ? 1 : 0));

        // NOTE previous call already handles sidechain connections
        if (_current.chains[row].blocks[emptyBlock].meta.numSideOutputs == 0)
            hostEnsureStereoChain(emptyRow, block - (block != 0 ? 1 : 0));
    }

    _current.dirty = true;
    return true;
}

// #endif // NUM_BLOCK_CHAIN_ROWS != 1

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::switchPreset(const uint8_t preset)
{
    mod_log_debug("switchPreset(%u)", preset);
    assert(preset < NUM_PRESETS_PER_BANK);

    if (_current.preset == preset)
        return false;

    // store old active preset in memory before doing anything
    const Current old = _current;

    // copy new preset to current data
    static_cast<Preset&>(_current) = _presets[preset];
    _current.preset = preset;

    // switch old preset with new one
    hostSwitchPreset(old);
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::switchScene(const uint8_t scene)
{
    mod_log_debug("switchScene(%u)", scene);
    assert(scene < NUM_SCENES_PER_PRESET);

    if (_current.scene == scene)
        return false;

    // preallocating some data
    std::vector<flushed_param> params;
    params.reserve(MAX_PARAMS_PER_BLOCK);

    _current.scene = scene;

    const Host::NonBlockingScopeWithAudioFades hnbs(_host);

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            Block& blockdata(_current.chains[row].blocks[bl]);
            if (isNullBlock(blockdata))
                continue;
            if (blockdata.meta.numParamsInScenes == 0 && blockdata.meta.numPropertiesInScenes == 0)
                continue;

            const HostBlockPair hbp = _mapper.get(_current.preset, row, bl);
            if (hbp.id == kMaxHostInstances)
                continue;

            params.clear();

            const SceneValues& sceneValues(blockdata.sceneValues[_current.scene]);

            // bypass/disable first if relevant
            if (blockdata.meta.enable.hasScenes && !blockdata.sceneValues[_current.scene].enabled)
            {
                blockdata.enabled = false;
                _host.bypass(hbp.id, true);

                if (hbp.pair != kMaxHostInstances)
                    _host.bypass(hbp.id, true);
            }

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                Parameter& paramdata(blockdata.parameters[p]);
                if (isNullURI(paramdata.symbol))
                    break;
                if ((paramdata.meta.flags & (Lv2PortIsOutput|Lv2ParameterInScene)) != Lv2ParameterInScene)
                    continue;

                paramdata.value = sceneValues.params[p];

                params.push_back({ paramdata.symbol.c_str(), paramdata.value });
            }

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                Property& propdata(blockdata.properties[p]);
                if (isNullURI(propdata.uri))
                    break;
                if ((propdata.meta.flags & (Lv2PropertyIsReadOnly|Lv2ParameterInScene)) != Lv2ParameterInScene)
                    continue;

                propdata.value = sceneValues.properties[p];

                _host.patch_set(hbp.id, propdata.uri.c_str(), propdata.value.c_str());

                if (hbp.pair != kMaxHostInstances)
                    _host.patch_set(hbp.pair, propdata.uri.c_str(), propdata.value.c_str());
            }

            _host.params_flush(hbp.id, LV2_KXSTUDIO_PROPERTIES_RESET_NONE, params.size(), params.data());

            if (hbp.pair != kMaxHostInstances)
                _host.params_flush(hbp.pair, LV2_KXSTUDIO_PROPERTIES_RESET_NONE, params.size(), params.data());

            // unbypass/enable last if relevant
            if (blockdata.meta.enable.hasScenes && blockdata.sceneValues[_current.scene].enabled)
            {
                blockdata.enabled = true;
                _host.bypass(hbp.id, false);

                if (hbp.pair != kMaxHostInstances)
                    _host.bypass(hbp.id, false);
            }
        }
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockBinding(const uint8_t hwid, const uint8_t row, const uint8_t block)
{
    mod_log_debug("addBlockBinding(%u, %u, %u)", hwid, row, block);
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata), false);
    assert_return(blockdata.meta.enable.hwbinding == UINT8_MAX, false);

    blockdata.meta.enable.hwbinding = hwid;

    if (_current.bindings[hwid].params.empty())
        _current.bindings[hwid].value = blockdata.enabled ? 1.f : 0.f;

    _current.bindings[hwid].params.push_back({ row, block, ":bypass", { 0 } });
    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockParameterBinding(const uint8_t hwid,
                                             const uint8_t row,
                                             const uint8_t block,
                                             const uint8_t paramIndex)
{
    mod_log_debug("addBlockParameterBinding(%u, %u, %u, %u)", hwid, row, block, paramIndex);
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata), false);

    Parameter& paramdata(blockdata.parameters[paramIndex]);
    assert_return(!isNullURI(paramdata.symbol), false);
    assert_return((paramdata.meta.flags & Lv2PortIsOutput) == 0, false);
    assert_return(paramdata.meta.hwbinding == UINT8_MAX, false);

    paramdata.meta.hwbinding = hwid;

    if (_current.bindings[hwid].params.empty())
        _current.bindings[hwid].value = paramdata.value;

    _current.bindings[hwid].params.push_back({ row, block, paramdata.symbol, { paramIndex } });
    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockPropertyBinding(const uint8_t hwid,
                                            const uint8_t row,
                                            const uint8_t block,
                                            const uint8_t propIndex)
{
    mod_log_debug("addBlockPropertyBinding(%u, %u, %u, %u)", hwid, row, block, propIndex);
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(propIndex < MAX_PARAMS_PER_BLOCK);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata), false);

    Property& propdata(blockdata.properties[propIndex]);
    assert_return(!isNullURI(propdata.uri), false);
    assert_return((propdata.meta.flags & Lv2PropertyIsReadOnly) == 0, false);
    assert_return(propdata.meta.hwbinding == UINT8_MAX, false);

    propdata.meta.hwbinding = hwid;

    if (_current.bindings[hwid].properties.empty())
    {
        assert(! propdata.meta.scalePoints.empty());

        bool found = false;
        for (uint32_t i = 0, numScalePoints = propdata.meta.scalePoints.size(); i < numScalePoints; ++i)
        {
            if (propdata.meta.scalePoints[i].value == propdata.value)
            {
                _current.bindings[hwid].value = i;
                break;
            }
        }

        assert(found);
        if (! found)
            _current.bindings[hwid].value = 0.0;
    }

    _current.bindings[hwid].properties.push_back({ row, block, propdata.uri, { propIndex } });
    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::removeBlockBinding(const uint8_t hwid, const uint8_t row, const uint8_t block)
{
    mod_log_debug("removeBlockBinding(%u, %u, %u)", hwid, row, block);
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata), false);
    assert_return(blockdata.meta.enable.hwbinding != UINT8_MAX, false);

    blockdata.meta.enable.hwbinding = UINT8_MAX;

    std::list<ParameterBinding>& bindings(_current.bindings[hwid].params);
    for (ParameterBindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
    {
        if (it->block != block)
            continue;
        if (it->parameterSymbol != ":bypass")
            continue;

        bindings.erase(it);
        _current.dirty = true;
        return true;
    }

    return false;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::removeBlockParameterBinding(const uint8_t hwid,
                                                const uint8_t row,
                                                const uint8_t block,
                                                const uint8_t paramIndex)
{
    mod_log_debug("removeBlockParameterBinding(%u, %u, %u, %u)", hwid, row, block, paramIndex);
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata), false);

    Parameter& paramdata(blockdata.parameters[paramIndex]);
    assert_return(!isNullURI(paramdata.symbol), false);
    assert_return((paramdata.meta.flags & Lv2PortIsOutput) == 0, false);
    assert_return(paramdata.meta.hwbinding != UINT8_MAX, false);

    paramdata.meta.hwbinding = UINT8_MAX;

    std::list<ParameterBinding>& bindings(_current.bindings[hwid].params);
    for (ParameterBindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
    {
        if (it->block != block)
            continue;
        if (it->meta.parameterIndex != paramIndex)
            continue;

        bindings.erase(it);
        _current.dirty = true;
        return true;
    }

    return false;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::removeBlockPropertyBinding(const uint8_t hwid,
                                               const uint8_t row,
                                               const uint8_t block,
                                               const uint8_t propIndex)
{
    mod_log_debug("removeBlockPropertyBinding(%u, %u, %u, %u)", hwid, row, block, propIndex);
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(propIndex < MAX_PARAMS_PER_BLOCK);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata), false);

    Property& propdata(blockdata.properties[propIndex]);
    assert_return(!isNullURI(propdata.uri), false);
    assert_return((propdata.meta.flags & Lv2PropertyIsReadOnly) == 0, false);
    assert_return(propdata.meta.hwbinding != UINT8_MAX, false);

    propdata.meta.hwbinding = UINT8_MAX;

    std::list<PropertyBinding>& bindings(_current.bindings[hwid].properties);
    for (PropertyBindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
    {
        if (it->block != block)
            continue;
        if (it->meta.propertyIndex != propIndex)
            continue;

        bindings.erase(it);
        _current.dirty = true;
        return true;
    }

    return false;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::reorderBlockBinding(const uint8_t hwid, const uint8_t dest)
{
    mod_log_debug("reorderBlockBinding(%u, %u)", hwid, dest);
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(dest < NUM_BINDING_ACTUATORS);

    if (hwid == dest)
    {
        mod_log_warn("reorderBlockBinding(%u, %u): hwid == dest, rejected", hwid, dest);
        return false;
    }

    const auto swapBindingHwId = [=](const uint8_t hwidA, const uint8_t hwidB)
    {
        std::swap(_current.bindings[hwidA], _current.bindings[hwidB]);

        for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
        {
            ChainRow& chaindata(_current.chains[row]);

            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                Block& blockdata(chaindata.blocks[bl]);
                if (isNullBlock(blockdata))
                    continue;

                if (blockdata.meta.enable.hwbinding == hwidA)
                    blockdata.meta.enable.hwbinding = hwidB;
                else if (blockdata.meta.enable.hwbinding == hwidB)
                    blockdata.meta.enable.hwbinding = hwidA;

                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    Parameter& paramdata(blockdata.parameters[p]);
                    if (isNullURI(paramdata.symbol))
                        break;

                    if (paramdata.meta.hwbinding == hwidA)
                        paramdata.meta.hwbinding = hwidB;
                    else if (paramdata.meta.hwbinding == hwidB)
                        paramdata.meta.hwbinding = hwidA;
                }

                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    Property& propdata(blockdata.properties[p]);
                    if (isNullURI(propdata.uri))
                        break;

                    if (propdata.meta.hwbinding == hwidA)
                        propdata.meta.hwbinding = hwidB;
                    else if (propdata.meta.hwbinding == hwidB)
                        propdata.meta.hwbinding = hwidA;
                }
            }
        }
    };

    // moving hwid backwards to the left
    // a b c d e! f
    // a b c e! d f
    // a b e! c d f
    // a e! b c d f
    if (hwid > dest)
    {
        for (int i = hwid; i > dest; --i)
            swapBindingHwId(i, i - 1);
    }

    // moving hwid forward to the right
    // a b! c d e f
    // a c b! d e f
    // a c d b! e f
    // a c d e b! f
    else
    {
        for (int i = hwid; i < dest; ++i)
            swapBindingHwId(i, i + 1);
    }

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::pollHostUpdates(Callback* const callback)
{
    _callback = callback;
    _host.poll_feedback(this);
    _callback = nullptr;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::requestHostUpdates()
{
    _host.output_data_ready();
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::enableCpuLoadUpdates(const bool enable)
{
    _host.feature_enable(Host::kFeatureCpuLoad, enable ? 1 : 0);
}

float HostConnector::getAverageCpuLoad()
{
    return _host.cpu_load();
}

float HostConnector::getMaximumCpuLoad()
{
    return _host.max_cpu_load();
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setBlockParameter(const uint8_t row,
                                      const uint8_t block,
                                      const uint8_t paramIndex,
                                      const float value,
                                      const SceneMode sceneMode)
{
    mod_log_debug("setBlockParameter(%u, %u, %u, %f, %d:%s)",
                  row, block, paramIndex, value, sceneMode, SceneMode2Str(sceneMode));
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata),);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert_return(hbp.id != kMaxHostInstances,);

    Parameter& paramdata(blockdata.parameters[paramIndex]);
    assert_return(!isNullURI(paramdata.symbol),);
    assert_return((paramdata.meta.flags & Lv2PortIsOutput) == 0,);

    _current.dirty = true;

    switch (sceneMode)
    {
    case SceneModeNone:
        blockdata.sceneValues[_current.scene].params[paramIndex] = value;
        break;

    case SceneModeActivate:
        if ((paramdata.meta.flags & Lv2ParameterInScene) == 0)
        {
            ++blockdata.meta.numParamsInScenes;
            paramdata.meta.flags |= Lv2ParameterInScene;

            // set original value for all other scenes
            for (uint8_t scene = 0; scene < NUM_SCENES_PER_PRESET; ++scene)
            {
                if (_current.scene == scene)
                    continue;
                blockdata.sceneValues[scene].params[paramIndex] = paramdata.value;
            }
        }
        blockdata.sceneValues[_current.scene].params[paramIndex] = value;
        break;

    case SceneModeClear:
        if ((paramdata.meta.flags & Lv2ParameterInScene) != 0)
        {
            --blockdata.meta.numParamsInScenes;
            paramdata.meta.flags &= ~Lv2ParameterInScene;
        }
        break;
    }

    if (paramdata.meta.hwbinding != UINT8_MAX)
    {
        Bindings& bindings(_current.bindings[paramdata.meta.hwbinding]);
        assert(!bindings.params.empty());

        if (bindings.params.size() == 1)
            bindings.value = value;
        else
            bindings.value = normalized(paramdata.meta, value);
    }

    paramdata.value = value;

    _host.param_set(hbp.id, paramdata.symbol.c_str(), value);

    if (hbp.pair != kMaxHostInstances)
        _host.param_set(hbp.pair, paramdata.symbol.c_str(), value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setBlockQuickPot(const uint8_t row, const uint8_t block, const uint8_t paramIndex)
{
    mod_log_debug("setBlockQuickPot(%u, %u, %u)", row, block, paramIndex);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata),);

    const Parameter& paramdata(blockdata.parameters[paramIndex]);
    assert_return(!isNullURI(paramdata.symbol),);
    assert_return((paramdata.meta.flags & Lv2PortIsOutput) == 0,);

    blockdata.quickPotSymbol = paramdata.symbol;
    blockdata.meta.quickPotIndex = paramIndex;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::monitorBlockOutputParameter(const uint8_t row, const uint8_t block, const uint8_t paramIndex)
{
    mod_log_debug("monitorBlockOutputParameter(%u, %u, %u)", row, block, paramIndex);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    const Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata),);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert_return(hbp.id != kMaxHostInstances,);

    const Parameter& paramdata(blockdata.parameters[paramIndex]);
    assert_return(!isNullURI(paramdata.symbol),);
    assert_return((paramdata.meta.flags & Lv2PortIsOutput) != 0,);

    _host.monitor_output(hbp.id, paramdata.symbol.c_str());
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::enableTool(const uint8_t toolIndex, const char* const uri)
{
    mod_log_debug("enableTool(%u, \"%s\")", toolIndex, uri);
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);

    return isNullURI(uri) ? _host.remove(MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex)
                          : _host.add(uri, MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::connectToolAudioInput(const uint8_t toolIndex,
                                          const char* const symbol,
                                          const char* const jackPort)
{
    mod_log_debug("connectToolAudioInput(%u, \"%s\", \"%s\")", toolIndex, symbol, jackPort);
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');
    assert(jackPort != nullptr && *jackPort != '\0');

    _host.connect(jackPort, format(MOD_HOST_EFFECT_PREFIX "%d:%s", 
                                   MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex,
                                   symbol).c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::connectToolAudioOutput(const uint8_t toolIndex,
                                           const char* const symbol,
                                           const char* const jackPort)
{
    mod_log_debug("connectToolAudioOutput(%u, \"%s\", \"%s\")", toolIndex, symbol, jackPort);
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');
    assert(jackPort != nullptr && *jackPort != '\0');

    _host.connect(format(MOD_HOST_EFFECT_PREFIX "%d:%s",
                         MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex,
                         symbol).c_str(), jackPort);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::connectTool2Tool(const uint8_t toolAIndex,
                                     const char* const toolAOutSymbol,
                                     const uint8_t toolBIndex, 
                                     const char* const toolBInSymbol)
{
    mod_log_debug("connectTool2Tool(%u, \"%s\", %u, \"%s\")", toolAIndex, toolAOutSymbol, toolBIndex, toolBInSymbol);
    assert(toolAIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(toolBIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(toolAOutSymbol != nullptr && *toolAOutSymbol != '\0');
    assert(toolBInSymbol != nullptr && *toolBInSymbol != '\0');

    _host.connect(format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolAIndex, toolAOutSymbol).c_str(), 
                  format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolBIndex, toolBInSymbol).c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setToolParameter(const uint8_t toolIndex, const char* const symbol, const float value)
{
    mod_log_debug("setToolParameter(%u, \"%s\", %f)", toolIndex, symbol, value);
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');

    _host.param_set(MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol, value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::monitorToolOutputParameter(const uint8_t toolIndex, const char* const symbol)
{
    mod_log_debug("monitorToolOutputParameter(%u, \"%s\")", toolIndex, symbol);
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');

    _host.monitor_output(MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setBlockProperty(const uint8_t row,
                                     const uint8_t block,
                                     const uint8_t propIndex,
                                     const char* const value,
                                     const SceneMode sceneMode)
{
    mod_log_debug("setBlockProperty(%u, %u, %u, \"%s\", %d:%s)",
                  row, block, propIndex, value, sceneMode, SceneMode2Str(sceneMode));
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(propIndex < MAX_PARAMS_PER_BLOCK);
    assert(value != nullptr);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert_return(!isNullBlock(blockdata),);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert_return(hbp.id != kMaxHostInstances,);

    Property& propdata(blockdata.properties[propIndex]);
    assert_return(!isNullURI(propdata.uri),);
    assert_return((propdata.meta.flags & Lv2PropertyIsReadOnly) == 0,);

    _current.dirty = true;

    switch (sceneMode)
    {
    case SceneModeNone:
        blockdata.sceneValues[_current.scene].properties[propIndex] = value;
        break;

    case SceneModeActivate:
        if ((propdata.meta.flags & Lv2ParameterInScene) == 0)
        {
            ++blockdata.meta.numPropertiesInScenes;
            propdata.meta.flags |= Lv2ParameterInScene;

            // set original value for all other scenes
            for (uint8_t scene = 0; scene < NUM_SCENES_PER_PRESET; ++scene)
            {
                if (_current.scene == scene)
                    continue;
                blockdata.sceneValues[scene].properties[propIndex] = propdata.value;
            }
        }
        blockdata.sceneValues[_current.scene].properties[propIndex] = value;
        break;

    case SceneModeClear:
        if ((propdata.meta.flags & Lv2ParameterInScene) != 0)
        {
            --blockdata.meta.numPropertiesInScenes;
            propdata.meta.flags &= ~Lv2ParameterInScene;
        }
        break;
    }

    if (propdata.meta.hwbinding != UINT8_MAX)
    {
        Bindings& bindings(_current.bindings[propdata.meta.hwbinding]);
        assert(!bindings.properties.empty());

        // TODO
        // bindings.value = normalized(propdata.meta, value);
    }

    propdata.value = value;

    _host.patch_set(hbp.id, propdata.uri.c_str(), value);

    if (hbp.pair != kMaxHostInstances)
        _host.patch_set(hbp.pair, propdata.uri.c_str(), value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBlockToBlock(const uint8_t row, const uint8_t blockA, const uint8_t blockB)
{
    mod_log_debug("hostConnectBlockToBlock(%u, %u, %u)", row, blockA, blockB);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(blockA < NUM_BLOCKS_PER_PRESET);
    assert(blockB < NUM_BLOCKS_PER_PRESET);

    Block& blockdataA(_current.chains[row].blocks[blockA]);
    Block& blockdataB(_current.chains[row].blocks[blockB]);

    const Lv2Plugin* const pluginA = lv2world.get_plugin_by_uri(blockdataA.uri.c_str());
    assert_return(pluginA != nullptr,);

    const Lv2Plugin* const pluginB = lv2world.get_plugin_by_uri(blockdataB.uri.c_str());
    assert_return(pluginB != nullptr,);

    const HostBlockPair hbpA = _mapper.get(_current.preset, row, blockA);
    assert_return(hbpA.id != kMaxHostInstances,);

    const HostBlockPair hbpB = _mapper.get(_current.preset, row, blockB);
    assert_return(hbpB.id != kMaxHostInstances,);

    std::string origin, target;

    // collect audio ports from each block
    std::vector<std::string> portsA;
    std::vector<std::string> portsB;
    portsA.reserve(2);
    portsB.reserve(2);

    constexpr uint32_t testFlags = Lv2PortIsAudio|Lv2PortIsOutput|Lv2PortIsSidechain;
    for (const Lv2Port& port : pluginA->ports)
    {
        if ((port.flags & testFlags) != (Lv2PortIsAudio|Lv2PortIsOutput))
            continue;

        portsA.push_back(format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbpA.id, port.symbol.c_str()));

        if (hbpA.pair != kMaxHostInstances)
        {
            portsA.push_back(format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbpA.pair, port.symbol.c_str()));
            break;
        }
    }

    for (const Lv2Port& port : pluginB->ports)
    {
        if ((port.flags & testFlags) != Lv2PortIsAudio)
            continue;

        portsB.push_back(format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbpB.id, port.symbol.c_str()));

        if (hbpB.pair != kMaxHostInstances)
        {
            portsB.push_back(format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbpB.pair, port.symbol.c_str()));
            break;
        }
    }

    assert(!portsA.empty());
    assert(!portsB.empty());

    _host.connect(portsA[0].c_str(), portsB[0].c_str());

    /**/ if (portsA.size() > portsB.size())
        _host.connect(portsA[1].c_str(), portsB[0].c_str());
    else if (portsA.size() < portsB.size())
        _host.connect(portsA[0].c_str(), portsB[1].c_str());
    else if (portsA.size() == 2)
        _host.connect(portsA[1].c_str(), portsB[1].c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBlockToChainInput(const uint8_t row, const uint8_t block)
{
    hostConnectChainInputAction(row, block, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBlockToChainOutput(const uint8_t row, const uint8_t block)
{
    hostConnectChainOutputAction(row, block, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectChainEndpoints(const uint8_t row)
{
    hostConnectChainEndpointsAction(row, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAll(const bool disconnectSideChains)
{
    mod_log_debug("hostDisconnectAll(%s)", bool2str(disconnectSideChains));

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            if (isNullBlock(_current.chains[row].blocks[bl]))
                continue;

            hostDisconnectAllBlockInputs(row, bl, disconnectSideChains);
            hostDisconnectAllBlockOutputs(row, bl, disconnectSideChains);
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockInputs(const uint8_t row,
                                                 const uint8_t block,
                                                 const bool disconnectSideChains)
{
    hostDisconnectBlockAction(_current.chains[row].blocks[block],
                              _mapper.get(_current.preset, row, block),
                              false,
                              disconnectSideChains);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockOutputs(const uint8_t row,
                                                  const uint8_t block,
                                                  const bool disconnectSideChains)
{
    hostDisconnectBlockAction(_current.chains[row].blocks[block],
                              _mapper.get(_current.preset, row, block),
                              true,
                              disconnectSideChains);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockInputs(const Block& blockdata,
                                                 const HostBlockPair& hbp,
                                                 const bool disconnectSideChains)
{
    hostDisconnectBlockAction(blockdata, hbp, true, disconnectSideChains);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockOutputs(const Block& blockdata,
                                                  const HostBlockPair& hbp,
                                                  const bool disconnectSideChains)
{
    hostDisconnectBlockAction(blockdata, hbp, false, disconnectSideChains);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectChainEndpoints(const uint8_t row)
{
    hostConnectChainEndpointsAction(row, false);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostClearAndLoadCurrentBank()
{
    mod_log_debug("hostClearAndLoadCurrentBank()");

    if (_firstboot)
    {
        _firstboot = false;
        _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOffWithoutFadeOut);
    }
    else
    {
        _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOffWithFadeOut);
    }

    _host.remove(-1);
    _mapper.reset();
    _current.numLoadedPlugins = 0;
    _current.dirty = false;

    for (uint8_t row = 1; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        _current.chains[row].capture.fill({});
        _current.chains[row].playback.fill({});
        _current.chains[row].captureId.fill(kMaxHostInstances);
        _current.chains[row].playbackId.fill(kMaxHostInstances);
    }

    for (uint8_t pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
        hostLoadPreset(pr);

    _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOnWithFadeIn);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectChainEndpointsAction(const uint8_t row, const bool connect)
{
    mod_log_debug("hostConnectChainInputAction(%u, %s)", row, bool2str(connect));
    assert(row < NUM_BLOCK_CHAIN_ROWS);

    const ChainRow& chain(_current.chains[row]);
    assert(!chain.capture[0].empty());
    assert(!chain.capture[1].empty());

    // playback side is allowed to be empty
    if (chain.playback[0].empty())
        return;

    assert(!chain.playback[1].empty());

    if (connect)
    {
        _host.connect(chain.capture[0].c_str(), chain.playback[0].c_str());
        _host.connect(chain.capture[1].c_str(), chain.playback[1].c_str());
    }
    else
    {
        _host.disconnect(chain.capture[0].c_str(), chain.playback[0].c_str());
        _host.disconnect(chain.capture[1].c_str(), chain.playback[1].c_str());
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectChainInputAction(const uint8_t row, const uint8_t block, const bool connect)
{
    mod_log_debug("hostConnectChainInputAction(%u, %u, %s)", row, block, bool2str(connect));
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(!isNullBlock(_current.chains[row].blocks[block]));

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.chains[row].blocks[block].uri.c_str());
    assert_return(plugin != nullptr,);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert_return(hbp.id != kMaxHostInstances,);

    bool (Host::*call)(const char*, const char*) = connect ? &Host::connect : &Host::disconnect;
    std::string origin, target;

    for (size_t i = 0, j = 0; i < plugin->ports.size() && j < 2; ++i)
    {
        if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
            continue;
        if ((plugin->ports[i].flags & Lv2PortIsSidechain) != 0)
            continue;

        origin = _current.chains[row].capture[j++];
        target = format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbp.id, plugin->ports[i].symbol.c_str());
        assert_continue(!origin.empty());
        (_host.*call)(origin.c_str(), target.c_str());

        if (hbp.pair != kMaxHostInstances)
        {
            origin = _current.chains[row].capture[j++];
            target = format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbp.pair, plugin->ports[i].symbol.c_str());
            assert_continue(!origin.empty());
            (_host.*call)(origin.c_str(), target.c_str());
            return;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectChainOutputAction(const uint8_t row, const uint8_t block, const bool connect)
{
    mod_log_debug("hostConnectChainOutputAction(%u, %u, %s)", row, block, bool2str(connect));
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const ChainRow& chain(_current.chains[row]);
    assert(!isNullBlock(chain.blocks[block]));

    // playback side is allowed to be empty
    if (chain.playback[0].empty())
        return;

    assert(!chain.playback[1].empty());

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(chain.blocks[block].uri.c_str());
    assert_return(plugin != nullptr,);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert_return(hbp.id != kMaxHostInstances,);

    bool (Host::*call)(const char*, const char*) = connect ? &Host::connect : &Host::disconnect;
    std::string origin, target;
    int dsti = 0;

    for (size_t i = 0; i < plugin->ports.size() && dsti < 2; ++i)
    {
        if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
            continue;
        if ((plugin->ports[i].flags & Lv2PortIsSidechain) != 0)
            continue;

        origin = format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbp.id, plugin->ports[i].symbol.c_str());
        target = chain.playback[dsti++];
        (_host.*call)(origin.c_str(), target.c_str());

        if (hbp.pair != kMaxHostInstances)
        {
            origin = format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbp.pair, plugin->ports[i].symbol.c_str());
            target = chain.playback[dsti++];
            (_host.*call)(origin.c_str(), target.c_str());
            return;
        }
    }

    if (dsti == 1)
        (_host.*call)(origin.c_str(), chain.playback[1].c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectBlockAction(const Block& blockdata,
                                              const HostBlockPair& hbp,
                                              const bool outputs,
                                              const bool disconnectSideChains)
{
    mod_log_debug("hostDisconnectBlockAction(..., {%u, %u}, %s, %s)",
                  hbp.id, hbp.pair, bool2str(outputs), bool2str(disconnectSideChains));
    assert(!isNullBlock(blockdata));
    assert(hbp.id != kMaxHostInstances);

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(blockdata.uri.c_str());
    assert_return(plugin != nullptr,);

    const unsigned int ioflags = Lv2PortIsAudio | (outputs ? Lv2PortIsOutput : 0);
    unsigned int flagsToCheck = Lv2PortIsAudio | Lv2PortIsOutput;
    if (!disconnectSideChains)
        flagsToCheck |= Lv2PortIsSidechain;
    std::string origin;

    for (const Lv2Port& port : plugin->ports)
    {
        if ((port.flags & flagsToCheck) != ioflags)
            continue;

        origin = format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbp.id, port.symbol.c_str());
        _host.disconnect_all(origin.c_str());

        if (hbp.pair != kMaxHostInstances)
        {
            origin = format(MOD_HOST_EFFECT_PREFIX "%d:%s", hbp.pair, port.symbol.c_str());
            _host.disconnect_all(origin.c_str());
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostEnsureStereoChain(const uint8_t row, const uint8_t blockStart, const bool recursive)
{
    mod_log_debug("hostEnsureStereoChain(%u, %u, %s)", row, blockStart, bool2str(recursive));
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(blockStart < NUM_BLOCKS_PER_PRESET);

    ChainRow& chain = _current.chains[row];
    assert(!chain.capture[0].empty());
    assert(!chain.capture[1].empty());

    // bool changed = false;
    bool previousPluginStereoOut = shouldBlockBeStereo(chain, blockStart);

    // ----------------------------------------------------------------------------------------------------------------
    // part 1: deal with dual-mono and disconnect blocks where needed

    bool sideChainToBeUpdated = false;

    for (uint8_t bl = blockStart; bl < NUM_BLOCKS_PER_PRESET; ++bl)
    {
        const Block& blockdata(chain.blocks[bl]);
        if (isNullBlock(blockdata))
            continue;

        const bool oldDualmono = _mapper.get(_current.preset, row, bl).pair != kMaxHostInstances;
        // activate dual mono if previous plugin is stereo or also dualmono
        bool newDualmono = false;
        if (blockdata.meta.numInputs == 1) 
        {
            newDualmono = previousPluginStereoOut;
            if ((blockdata.meta.numSideInputs != 0) && 
                !newDualmono && 
                (row + 1 < NUM_BLOCK_CHAIN_ROWS))
            {
                // if dual mono wasn't enforced by current row, it might be enforced by next row
                ChainRow& chain2data(_current.chains[row + 1]);
                newDualmono = shouldBlockBeStereo(chain2data, NUM_BLOCKS_PER_PRESET);
            }
        }

        previousPluginStereoOut = blockdata.meta.numOutputs == 2 || newDualmono;

        if (oldDualmono == newDualmono)
            continue;

        // changed = true;

        if (newDualmono)
        {
            const uint16_t pair = _mapper.add_pair(_current.preset, row, bl);

            // NOTE this does not take into account our custom defaults, we set all params
            if (_host.add(blockdata.uri.c_str(), pair))
            {
                if (!blockdata.enabled)
                    _host.bypass(pair, true);

                for (const Parameter& paramdata : blockdata.parameters)
                {
                    if (isNullURI(paramdata.symbol))
                        break;
                    _host.param_set(pair, paramdata.symbol.c_str(), paramdata.value);
                }

                for (const Property& propdata : blockdata.properties)
                {
                    if (isNullURI(propdata.uri))
                        break;
                    _host.patch_set(pair, propdata.uri.c_str(), propdata.value.c_str());
                }
            }
        }
        else
        {
            _host.remove(_mapper.remove_pair(_current.preset, row, bl));
        }

        // disconnect also sidechain outputs in case of outdated connection (will be reconnected hostEnsureStereoChain(row + 1, ...))
        hostDisconnectAllBlockOutputs(row, bl, true);

        // redo sideIO (if applicable) due to add/remove
        const HostBlockPair hbp = _mapper.get(_current.preset, row, bl);
        hostSetupSideIO(row, bl, hbp, nullptr);

        // NOTE: sidechain update not needed if mono -> dual mono update of sidechain playback target block
        // was triggered from sidechain (recursive == true)
        if ((blockdata.meta.numSideOutputs != 0) ||
            (blockdata.meta.numSideInputs != 0 && newDualmono && !oldDualmono && !recursive))
        {
            assert_continue(row + 1 < NUM_BLOCK_CHAIN_ROWS);
            sideChainToBeUpdated = true;
        }
    }

    // after all required dual mono blocks have been created above, update "sidechain" mono/stereo setup
    if (sideChainToBeUpdated) {
        hostEnsureStereoChain(row + 1, 0, true);
    }

    // ensure stereo / dual mono for possible other rows serving as playback targets
    if (row > 0 && chain.playbackId[0] != kMaxHostInstances) {
        HostInstanceMapper::BlockAndRow blockRow = _mapper.get_block_with_id(_current.preset, chain.playbackId[0]);
        hostEnsureStereoChain(blockRow.row, blockRow.block, true);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // part 2: handle connections

    std::array<bool, NUM_BLOCKS_PER_PRESET> loaded;
    uint8_t firstBlock = UINT8_MAX;
    uint8_t lastBlock = UINT8_MAX;
    uint8_t numLoadedPlugins = 0;
    for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
    {
        if ((loaded[bl] = !isNullBlock(chain.blocks[bl])))
        {
            ++numLoadedPlugins;

            if (firstBlock == UINT8_MAX)
                firstBlock = bl;

            lastBlock = bl;
        }
    }

    if (numLoadedPlugins == 0)
    {
        // direct connections
        hostConnectChainEndpoints(row);
        return;
    }

    assert(firstBlock != UINT8_MAX);
    assert(lastBlock != UINT8_MAX);

    // direct connections
    hostDisconnectChainEndpoints(row);

    // first and last blocks
    hostConnectBlockToChainInput(row, firstBlock);
    hostConnectBlockToChainOutput(row, lastBlock);

    // in between blocks
    for (uint8_t bl1 = firstBlock; bl1 <= lastBlock && bl1 < NUM_BLOCKS_PER_PRESET; ++bl1)
    {
        if (! loaded[bl1])
            continue;

        for (uint8_t bl2 = bl1 + 1; bl2 < NUM_BLOCKS_PER_PRESET; ++bl2)
        {
            if (! loaded[bl2])
                continue;

            hostConnectBlockToBlock(row, bl1, bl2);
            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostSetupSideIO(const uint8_t row,
                                    const uint8_t block,
                                    const HostBlockPair hbp,
                                    const Lv2Plugin* plugin)
{
    mod_log_debug("hostSetupSideIO(%u, %u, {%u, %u}, %p)", row, block, hbp.id, hbp.pair, plugin);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(hbp.id != kMaxHostInstances);

    const Block& blockdata(_current.chains[row].blocks[block]);
    assert(!isNullBlock(blockdata));

    if (blockdata.meta.numSideInputs == 0 && blockdata.meta.numSideOutputs == 0)
        return;

    // next row must be available for use if adding side IO
    assert_return(row + 1 < NUM_BLOCK_CHAIN_ROWS,);

    if (plugin == nullptr)
        plugin = lv2world.get_plugin_by_uri(blockdata.uri.c_str());
    assert_return(plugin != nullptr,);

    // side input requires something to connect from
    if (blockdata.meta.numSideInputs != 0)
    {
        // must have matching capture side
        assert_return(!_current.chains[row + 1].capture[0].empty(),);
        assert_return(!_current.chains[row + 1].capture[1].empty(),);

        constexpr uint32_t flagsToCheck = Lv2PortIsAudio|Lv2PortIsSidechain|Lv2PortIsOutput;
        constexpr uint32_t flagsWanted = Lv2PortIsAudio|Lv2PortIsSidechain;

        for (const Lv2Port& port : plugin->ports)
        {
            if ((port.flags & flagsToCheck) != flagsWanted)
                continue;

            _current.chains[row + 1].playback[0] = format(MOD_HOST_EFFECT_PREFIX "%d:%s",
                                                          hbp.id,
                                                          port.symbol.c_str());
            _current.chains[row + 1].playbackId[0] = hbp.id;

            if (hbp.pair != kMaxHostInstances)
            {
                _current.chains[row + 1].playback[1] = format(MOD_HOST_EFFECT_PREFIX "%d:%s",
                                                              hbp.pair,
                                                              port.symbol.c_str());
                _current.chains[row + 1].playbackId[1] = hbp.pair;
            }
            else
            {
                _current.chains[row + 1].playback[1] = _current.chains[row + 1].playback[0];
                _current.chains[row + 1].playbackId[1] = _current.chains[row + 1].playbackId[0];
            }
            break;
        }
    }

    if (blockdata.meta.numSideOutputs != 0)
    {
        constexpr uint32_t flags = Lv2PortIsAudio|Lv2PortIsSidechain|Lv2PortIsOutput;

        for (const Lv2Port& port : plugin->ports)
        {
            if ((port.flags & flags) != flags)
                continue;

            _current.chains[row + 1].capture[0] = format(MOD_HOST_EFFECT_PREFIX "%d:%s",
                                                         hbp.id,
                                                         port.symbol.c_str());
            _current.chains[row + 1].captureId[0] = hbp.id;

            if (hbp.pair != kMaxHostInstances)
            {
                _current.chains[row + 1].capture[1] = format(MOD_HOST_EFFECT_PREFIX "%d:%s",
                                                             hbp.pair,
                                                             port.symbol.c_str());
                _current.chains[row + 1].captureId[1] = hbp.pair;
            }
            else
            {
                _current.chains[row + 1].capture[1] = _current.chains[row + 1].capture[0];
                _current.chains[row + 1].captureId[1] = _current.chains[row + 1].captureId[0];
            }

            break;
        }
    }
}
// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostRemoveAllBlockBindings(const uint8_t row, const uint8_t block)
{
    mod_log_debug("hostRemoveAllBlockBindings(%u, %u)", row, block);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    Block& blockdata(_current.chains[row].blocks[block]);
    assert(!isNullBlock(blockdata));

    blockdata.meta.enable.hwbinding = UINT8_MAX;

    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
    {
        Parameter& paramdata(blockdata.parameters[p]);
        if (isNullURI(paramdata.symbol))
            break;

        paramdata.meta.hwbinding = UINT8_MAX;
    }

    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
    {
        Property& propdata(blockdata.properties[p]);
        if (isNullURI(propdata.uri))
            break;

        propdata.meta.hwbinding = UINT8_MAX;
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        _current.bindings[hwid].value = 0.f;

        std::list<ParameterBinding>& bindings(_current.bindings[hwid].params);

    restartParameter:
        for (ParameterBindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
        {
            if (it->row != row)
                continue;
            if (it->block != block)
                continue;

            bindings.erase(it);
            _current.dirty = true;
            goto restartParameter;
        }
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        _current.bindings[hwid].value = 0.f;

        std::list<PropertyBinding>& bindings(_current.bindings[hwid].properties);

    restartProperty:
        for (PropertyBindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
        {
            if (it->row != row)
                continue;
            if (it->block != block)
                continue;

            bindings.erase(it);
            _current.dirty = true;
            goto restartProperty;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostRemoveInstanceForBlock(const uint8_t row, const uint8_t block)
{
    mod_log_debug("hostRemoveInstanceForBlock(%u, %u)", row, block);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const HostBlockPair hbp = _mapper.remove(_current.preset, row, block);

    if (hbp.id != kMaxHostInstances)
        _host.remove(hbp.id);

    if (hbp.pair != kMaxHostInstances)
        _host.remove(hbp.pair);

   #if NUM_BLOCK_CHAIN_ROWS != 1
    if (row == 0)
    {
        const Block& blockdata(_current.chains[row].blocks[block]);
        assert(!isNullBlock(blockdata));

        if (blockdata.meta.numSideInputs != 0)
        {
            _current.chains[row + 1].playback.fill({});
            _current.chains[row + 1].playbackId.fill(kMaxHostInstances);
        }

        if (blockdata.meta.numSideOutputs != 0)
        {
            _current.chains[row + 1].capture.fill({});
            _current.chains[row + 1].captureId.fill(kMaxHostInstances);
        }
    }
   #endif
}

// --------------------------------------------------------------------------------------------------------------------

template<class nlohmann_json>
uint8_t HostConnector::jsonPresetLoad(Preset& presetdata, nlohmann_json& json) const
{
    nlohmann::json& jpreset = static_cast<nlohmann::json&>(json);

    if (!jpreset.contains("blocks"))
    {
        mod_log_info("jsonPresetLoad(): preset does not include blocks, loading empty");
        resetPreset(presetdata);
        return 0;
    }

    {
        std::string name;

        if (jpreset.contains("name"))
        {
            try {
                name = jpreset["name"].get<std::string>();
            } catch (...) {}
        }

        presetdata.name = name;
    }

    {
        uint32_t color = 0;
        std::string style;

        if (jpreset.contains("background"))
        {
            const auto& jbackground = jpreset["background"];

            try {
                color = jbackground["color"].get<uint32_t>();
                style = jbackground["style"].get<std::string>();
            } catch (...) {}
        }

        presetdata.background.color = color;
        presetdata.background.style = style;
    }

    {
        std::string uuid;

        if (jpreset.contains("uuid"))
        {
            try {
                uuid = jpreset["uuid"].get<std::string>();
            } catch (...) {}
        }

        if (!uuid.empty())
            presetdata.uuid = str2uuid(uuid);
        else
            presetdata.uuid = generateUUID();
    }

    if (jpreset.contains("scene"))
    {
        try {
            presetdata.scene = jpreset["scene"].get<int>();
        } catch (...) {}

        if (presetdata.scene >= NUM_SCENES_PER_PRESET)
            presetdata.scene = 0;
    }
    else
    {
        presetdata.scene = 0;
    }

    uint8_t numLoadedPlugins = 0;

    auto& jblocks = jpreset["blocks"];
    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        if (row != 0)
        {
            presetdata.chains[row].capture.fill({});
            presetdata.chains[row].playback.fill({});
        }

        presetdata.chains[row].captureId.fill(kMaxHostInstances);
        presetdata.chains[row].playbackId.fill(kMaxHostInstances);

        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            Block& blockdata = presetdata.chains[row].blocks[bl];

           #if NUM_BLOCK_CHAIN_ROWS == 1
            // single row, load direct block id if available
            std::string jblockid = std::to_string(bl + 1);

            if (! jblocks.contains(jblockid))
            {
                // fallback: try loading multi-row file with 1st row
                jblockid = format("1:%u", bl + 1);

                if (! jblocks.contains(jblockid))
                {
                    resetBlock(blockdata);
                    continue;
                }
            }
           #else
            // multiple rows, load row + block id if available
            std::string jblockid = format("%u:%u", row + 1, bl + 1);

            if (! jblocks.contains(jblockid))
            {
                // fallback only valid for first row
                if (row != 0)
                {
                    resetBlock(blockdata);
                    continue;
                }

                // fallback: try loading single-row file
                jblockid = std::to_string(bl + 1);

                if (! jblocks.contains(jblockid))
                {
                    resetBlock(blockdata);
                    continue;
                }
            }
           #endif

            auto& jblock = jblocks[jblockid];
            if (! jblock.contains("uri"))
            {
                mod_log_info("jsonPresetLoad(): block %u does not include uri, loading empty", bl);
                resetBlock(blockdata);
                continue;
            }

            const std::string uri = jblock["uri"].get<std::string>();

            const Lv2Plugin* const plugin = !isNullURI(uri)
                                            ? lv2world.get_plugin_by_uri(uri.c_str())
                                            : nullptr;

            if (plugin == nullptr)
            {
                mod_log_info("jsonPresetLoad(): plugin with uri '%s' not available, using empty block", uri.c_str());
                resetBlock(blockdata);
                continue;
            }

            uint8_t numInputs, numOutputs, numSideInputs, numSideOutputs;
            if (!getSupportedPluginIO(plugin, numInputs, numOutputs, numSideInputs, numSideOutputs))
            {
                mod_log_info("jsonPresetLoad(): plugin with uri '%s' has invalid IO, using empty block", uri.c_str());
                resetBlock(blockdata);
                continue;
            }

            std::unordered_map<std::string, uint8_t> paramToIndexMap, propToIndexMap;
            initBlock(blockdata, plugin, numInputs, numOutputs, numSideInputs, numSideOutputs,
                      &paramToIndexMap, &propToIndexMap);

            if (jblock.contains("enabled"))
                blockdata.enabled = jblock["enabled"].get<bool>();

            ++numLoadedPlugins;

            try {
                const std::string quickpot = jblock["quickpot"].get<std::string>();

                if (!quickpot.empty())
                {
                    if (const auto it = paramToIndexMap.find(quickpot); it != paramToIndexMap.end())
                    {
                        blockdata.quickPotSymbol = quickpot;
                        blockdata.meta.quickPotIndex = it->second;
                    }
                }

            } catch (...) {}

            if (jblock.contains("parameters"))
            {
                auto& jparams = jblock["parameters"];
                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const std::string jparamid = std::to_string(p + 1);

                    if (! jparams.contains(jparamid))
                        continue;

                    auto& jparam = jparams[jparamid];
                    if (! (jparam.contains("symbol") && jparam.contains("value")))
                    {
                        mod_log_info("jsonPresetLoad(): parameter %u is missing symbol and/or value", p);
                        continue;
                    }

                    const std::string symbol = jparam["symbol"].get<std::string>();

                    if (paramToIndexMap.find(symbol) == paramToIndexMap.end())
                    {
                        mod_log_info("jsonPresetLoad(): parameter with '%s' symbol does not exist in plugin", symbol.c_str());
                        continue;
                    }

                    const uint8_t paramIndex = paramToIndexMap[symbol];
                    Parameter& paramdata = blockdata.parameters[paramIndex];

                    if (isNullURI(paramdata.symbol))
                        continue;
                    if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                        continue;

                    paramdata.value = std::max(paramdata.meta.min,
                                                std::min<float>(paramdata.meta.max,
                                                                jparam["value"].get<double>()));
                }
            }

            if (jblock.contains("properties"))
            {
                auto& jprops = jblock["properties"];
                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const std::string jpropid = std::to_string(p + 1);

                    if (! jprops.contains(jpropid))
                        continue;

                    auto& jprop = jprops[jpropid];
                    if (! (jprop.contains("uri") && jprop.contains("value")))
                    {
                        mod_log_info("jsonPresetLoad(): property %u is missing uri and/or value", p);
                        continue;
                    }

                    const std::string propuri = jprop["uri"].get<std::string>();

                    if (propToIndexMap.find(propuri) == propToIndexMap.end())
                    {
                        mod_log_info("jsonPresetLoad(): property with '%s' uri does not exist in plugin", uri.c_str());
                        continue;
                    }

                    const uint8_t propIndex = propToIndexMap[propuri];
                    Property& propdata = blockdata.properties[propIndex];

                    if (isNullURI(propdata.uri))
                        continue;
                    if ((propdata.meta.flags & Lv2PropertyIsReadOnly) != 0)
                        continue;

                    propdata.value = jprop["value"].get<std::string>();
                }
            }

            if (! jblock.contains("scenes"))
                continue;

            auto& jallscenes = jblock["scenes"];
            for (uint8_t sid = 0; sid < NUM_SCENES_PER_PRESET; ++sid)
            {
                const std::string jsceneid = std::to_string(sid + 1);

                if (! jallscenes.contains(jsceneid))
                    continue;

                auto& jscenes = jallscenes[jsceneid];
                if (! jscenes.is_array())
                {
                    mod_log_info("jsonPresetLoad(): preset scenes are not arrays");
                    continue;
                }

                // TODO handle properties
                for (auto& jscene : jscenes)
                {
                    if (! (jscene.contains("symbol") && jscene.contains("value")))
                    {
                        mod_log_info("jsonPresetLoad(): scene param is missing symbol and/or value");
                        continue;
                    }

                    const std::string symbol = jscene["symbol"].get<std::string>();

                    if (symbol == ":bypass")
                    {
                        if (! blockdata.meta.enable.hasScenes)
                        {
                            blockdata.meta.enable.hasScenes = true;
                            ++blockdata.meta.numParamsInScenes;
                        }

                        blockdata.sceneValues[sid].enabled = jscene["value"].get<bool>();
                        continue;
                    }

                    if (paramToIndexMap.find(symbol) == paramToIndexMap.end())
                    {
                        mod_log_info("jsonPresetLoad(): scene param with '%s' symbol does not exist", symbol.c_str());
                        continue;
                    }

                    const uint8_t paramIndex = paramToIndexMap[symbol];
                    Parameter& paramdata = blockdata.parameters[paramIndex];

                    if (isNullURI(paramdata.symbol))
                        continue;
                    if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                        continue;

                    if ((paramdata.meta.flags & Lv2ParameterInScene) == 0)
                    {
                        paramdata.meta.flags |= Lv2ParameterInScene;
                        ++blockdata.meta.numParamsInScenes;
                    }

                    blockdata.sceneValues[sid].params[paramIndex] =
                        std::max(paramdata.meta.min,
                                 std::min<float>(paramdata.meta.max,
                                                 jscene["value"].get<double>()));
                }
            }
        }
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        presetdata.bindings[hwid].params.clear();
        presetdata.bindings[hwid].properties.clear();
    }

    if (! jpreset.contains("bindings"))
    {
        mod_log_info("jsonPresetLoad(): preset does not include any bindings");
        return numLoadedPlugins;
    }

    auto& jallbindings = jpreset["bindings"];
    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
       #ifdef BINDING_ACTUATOR_IDS
        const std::string jbindingsid = kBindingActuatorIDs[hwid];
       #else
        const std::string jbindingsid = std::to_string(hwid + 1);
       #endif

        if (! jallbindings.contains(jbindingsid))
        {
            mod_log_info("jsonPresetLoad(): preset does not include bindings for hw '%s'", jbindingsid.c_str());
            continue;
        }

        // TODO handle properties
        auto& jbindings = jallbindings[jbindingsid];
        if (! (jbindings.contains("params") && jbindings.contains("value")))
        {
            mod_log_info("jsonPresetLoad(): bindings is missing params and/or value");
            continue;
        }

        auto& jbindingparams = jbindings["params"];
        if (! jbindingparams.is_array())
        {
            mod_log_info("jsonPresetLoad(): preset binding params is not an array");
            continue;
        }

        Bindings& bindings(presetdata.bindings[hwid]);

        for (auto& jbindingparam : jbindingparams)
        {
            if (! (jbindingparam.contains("block") && jbindingparam.contains("symbol")))
            {
                mod_log_info("jsonPresetLoad(): binding is missing block and/or symbol");
                continue;
            }

            const int block = jbindingparam["block"].get<int>();
            if (block < 1 || block > NUM_BLOCKS_PER_PRESET)
            {
                mod_log_info("jsonPresetLoad(): binding has out of bounds block %d", block);
                continue;
            }
            int row = 1;
            if (jbindingparam.contains("row"))
            {
                row = jbindingparam["row"].get<int>();
                if (row < 1 || row > NUM_BLOCK_CHAIN_ROWS)
                {
                   #if NUM_BLOCK_CHAIN_ROWS != 1
                    mod_log_info("jsonPresetLoad(): binding has out of bounds block %d", block);
                   #endif
                    continue;
                }
            }

            Block& blockdata = presetdata.chains[row - 1].blocks[block - 1];

            const std::string symbol = jbindingparam["symbol"].get<std::string>();

            if (symbol == ":bypass")
            {
                blockdata.meta.enable.hwbinding = hwid;

                bindings.params.push_back({
                    .row = static_cast<uint8_t>(row - 1),
                    .block = static_cast<uint8_t>(block - 1),
                    .parameterSymbol = ":bypass",
                    .meta = {
                        .parameterIndex = 0,
                    },
                });
            }
            else
            {
                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    Parameter& paramdata = blockdata.parameters[p];

                    if (isNullURI(paramdata.symbol))
                        break;
                    if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                        continue;

                    if (paramdata.symbol == symbol)
                    {
                        paramdata.meta.hwbinding = hwid;

                        bindings.params.push_back({
                            .row = static_cast<uint8_t>(row - 1),
                            .block = static_cast<uint8_t>(block - 1),
                            .parameterSymbol = symbol,
                            .meta = {
                                .parameterIndex = p,
                            },
                        });
                        break;
                    }
                }
            }
        }

        const double jvalue = jbindings["value"].get<double>();
        if (bindings.params.size() == 1)
        {
            const ParameterBinding& binding = bindings.params.front();
            const Block& blockdata = presetdata.chains[binding.row].blocks[binding.block];
            const Parameter& paramdata = blockdata.parameters[binding.meta.parameterIndex];
            bindings.value = std::max(paramdata.meta.min, std::min(paramdata.meta.max, static_cast<float>(jvalue)));
        }
        else
        {
            bindings.value = std::max(0.0, std::min(1.0, jvalue));
        }
    }

    return numLoadedPlugins;
}

// --------------------------------------------------------------------------------------------------------------------

template<class nlohmann_json>
void HostConnector::jsonPresetSave(const Preset& presetdata, nlohmann_json& json) const
{
    nlohmann::json& jpreset = static_cast<nlohmann::json&>(json);

    jpreset = nlohmann::json::object({
        { "bindings", nlohmann::json::object({}) },
        { "blocks", nlohmann::json::object({}) },
        { "name", presetdata.name },
        { "scene", presetdata.scene },
        { "uuid", uuid2str(presetdata.uuid) },
    });

    if (! presetdata.background.style.empty())
    {
        auto& jbackground = jpreset["background"] = nlohmann::json::object({});
        jbackground["color"] = presetdata.background.color;
        jbackground["style"] = presetdata.background.style;
    }

    auto& jallbindings = jpreset["bindings"];

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        const Bindings& bindings(presetdata.bindings[hwid]);

       #ifdef BINDING_ACTUATOR_IDS
        const std::string jbindingsid = kBindingActuatorIDs[hwid];
       #else
        const std::string jbindingsid = std::to_string(hwid + 1);
       #endif
        // TODO handle properties
        auto& jbindings = jallbindings[jbindingsid] = nlohmann::json::object({
            { "params", nlohmann::json::array() },
            { "value", bindings.value },
        });

        auto& jbindingparams = jbindings["params"];

        for (const ParameterBinding& bindingdata : presetdata.bindings[hwid].params)
        {
            jbindingparams.push_back({
               #if NUM_BLOCK_CHAIN_ROWS != 1
                { "row", bindingdata.row + 1 },
               #endif
                { "block", bindingdata.block + 1 },
                { "symbol", bindingdata.parameterSymbol },
            });
        }
    }

    auto& jblocks = jpreset["blocks"];

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            const Block& blockdata = presetdata.chains[row].blocks[bl];

            if (isNullBlock(blockdata))
                continue;

           #if NUM_BLOCK_CHAIN_ROWS == 1
            const std::string jblockid = std::to_string(bl + 1);
           #else
            const std::string jblockid = format("%u:%u", row + 1, bl + 1);
           #endif

            // TODO handle properties
            auto& jblock = jblocks[jblockid] = {
                { "enabled", blockdata.enabled },
                { "parameters", nlohmann::json::object({}) },
                { "quickpot", blockdata.quickPotSymbol },
                { "scenes", nlohmann::json::object({}) },
                { "uri", /*isNullBlock(blockdata) ? "-" :*/ blockdata.uri },
            };

            auto& jparams = jblock["parameters"];
            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                const Parameter& paramdata = blockdata.parameters[p];

                if (isNullURI(paramdata.symbol))
                    break;
                if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                    continue;

                const std::string jparamid = std::to_string(p + 1);
                jparams[jparamid] = {
                    { "symbol", paramdata.symbol },
                    { "value", paramdata.value },
                };
            }

            if (blockdata.meta.numParamsInScenes != 0)
            {
                auto& jallscenes = jblock["scenes"];

                for (uint8_t sid = 0; sid < NUM_SCENES_PER_PRESET; ++sid)
                {
                    const std::string jsceneid = std::to_string(sid + 1);
                    auto& jscenes = jallscenes[jsceneid] = nlohmann::json::array();

                    if (blockdata.meta.enable.hasScenes)
                    {
                        jscenes.push_back({
                            { "symbol", ":bypass" },
                            { "value", blockdata.sceneValues[sid].enabled },
                        });
                    }

                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        const Parameter& paramdata = blockdata.parameters[p];

                        if (isNullURI(paramdata.symbol))
                            break;
                        if ((paramdata.meta.flags & (Lv2PortIsOutput|Lv2ParameterInScene)) != Lv2ParameterInScene)
                            continue;

                        jscenes.push_back({
                            { "symbol", blockdata.parameters[p].symbol },
                            { "value", blockdata.sceneValues[sid].params[p] },
                        });
                    }
                }
            }
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostLoadPreset(const uint8_t preset)
{
    mod_log_debug("hostLoadPreset(%u)", preset);

    if (_current.preset == preset) {
        assert(_current.numLoadedPlugins == 0);
    }

    const bool active = _current.preset == preset;

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        const ChainRow& chaindata(active ? _current.chains[row] : _presets[preset].chains[row]);

        bool firstBlock = true;
        bool previousPluginStereoOut;

        // related to active preset only
        uint8_t last = 0;
        uint8_t numLoadedPlugins = 0;

        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            const Block& blockdata(chaindata.blocks[bl]);
            if (isNullBlock(blockdata))
                continue;

            if (firstBlock)
            {
                firstBlock = false;
                previousPluginStereoOut = chaindata.capture[0] != chaindata.capture[1];
            }

            const auto loadInstance = [=, &blockdata](const uint16_t instance)
            {
                if (active ? _host.add(blockdata.uri.c_str(), instance)
                           : _host.preload(blockdata.uri.c_str(), instance))
                {
                    if (!blockdata.enabled)
                        _host.bypass(instance, true);

                    for (const Parameter& paramdata : blockdata.parameters)
                    {
                        if (isNullURI(paramdata.symbol))
                            break;
                        // TODO safe float comparison
                        if (paramdata.value != paramdata.meta.def)
                            _host.param_set(instance, paramdata.symbol.c_str(), paramdata.value);
                    }

                    for (const Property& propdata : blockdata.properties)
                    {
                        if (isNullURI(propdata.uri))
                            break;
                        if (propdata.value != propdata.meta.def)
                            _host.patch_set(instance, propdata.uri.c_str(), propdata.value.c_str());
                    }

                    return true;
                }

                return false;
            };

            const bool dualmono = previousPluginStereoOut && blockdata.meta.numInputs == 1;
            const HostBlockPair hbp = { _mapper.add(preset, row, bl), kMaxHostInstances };

            bool added = loadInstance(hbp.id);

            if (added)
            {
                if (dualmono)
                {
                    const uint16_t pair = _mapper.add_pair(preset, row, bl);

                    if (! loadInstance(pair))
                    {
                        added = false;
                        _host.remove(hbp.pair);
                    }
                }
            }

            if (! added)
            {
                if (active)
                    resetBlock(_current.chains[row].blocks[bl]);

                _mapper.remove(preset, row, bl);
                continue;
            }

            previousPluginStereoOut = blockdata.meta.numOutputs == 2 || dualmono;

            // dealing with connections after this point, only valid if preset is the active one
            if (active)
            {
                if (++numLoadedPlugins == 1)
                    hostConnectBlockToChainInput(row, bl);
                else
                    hostConnectBlockToBlock(row, last, bl);

                hostSetupSideIO(row, bl, hbp, nullptr);
                last = bl;
            }
        }

        if (active)
        {
            if (numLoadedPlugins != 0)
                hostConnectBlockToChainOutput(row, last);
            else if (!_current.chains[row].capture[0].empty())
                hostConnectChainEndpoints(row);

            _current.numLoadedPlugins += numLoadedPlugins;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostSwitchPreset(const Current& old)
{
    mod_log_debug("hostSwitchPreset(...)");

    bool oldloaded[NUM_BLOCK_CHAIN_ROWS][NUM_BLOCKS_PER_PRESET];

    // preallocating some data
    std::vector<flushed_param> params;
    params.reserve(MAX_PARAMS_PER_BLOCK);

    _current.dirty = false;
    _current.numLoadedPlugins = 0;

    // scope for fade-out, old deactivate, new activate, fade-in
    {
        const Host::NonBlockingScopeWithAudioFades hnbs(_host);

        // step 1: disconnect and deactivate all plugins in old preset
        // NOTE not removing plugins, done after processing is reenabled
        if (old.numLoadedPlugins == 0)
        {
            std::memset(oldloaded, 0, sizeof(oldloaded));
            hostDisconnectChainEndpoints(0);
        }
        else
        {
            for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
            {
                uint8_t numLoadedPlugins = 0;

                for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                {
                    const Block& blockdata(old.chains[row].blocks[bl]);

                    if (! (oldloaded[row][bl] = !isNullBlock(blockdata)))
                        continue;

                    const HostBlockPair hbp = _mapper.get(old.preset, row, bl);
                    hostDisconnectAllBlockInputs(blockdata, hbp);
                    hostDisconnectAllBlockOutputs(blockdata, hbp);

                    if (hbp.id != kMaxHostInstances)
                        _host.activate(hbp.id, false);

                    if (hbp.pair != kMaxHostInstances)
                        _host.activate(hbp.pair, false);

                    ++numLoadedPlugins;
                }

                if (numLoadedPlugins == 0 && !old.chains[row].capture[0].empty())
                    hostDisconnectChainEndpoints(row);
            }
        }

        // step 2: activate and connect all plugins in new preset
        for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
        {
            uint8_t last = 0;
            uint8_t numLoadedPlugins = 0;

            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                if (isNullBlock(_current.chains[row].blocks[bl]))
                    continue;

                const HostBlockPair hbp = _mapper.get(_current.preset, row, bl);

                if (hbp.id != kMaxHostInstances)
                    _host.activate(hbp.id, true);

                if (hbp.pair != kMaxHostInstances)
                    _host.activate(hbp.pair, true);

                if (numLoadedPlugins == 0)
                    hostConnectBlockToChainInput(row, bl);
                else
                    hostConnectBlockToBlock(row, last, bl);

                hostSetupSideIO(row, bl, hbp, nullptr);

                last = bl;
                ++numLoadedPlugins;
            }

            if (numLoadedPlugins != 0)
                hostConnectBlockToChainOutput(row, last);
            else if (!_current.chains[row].capture[0].empty())
                hostConnectChainEndpoints(row);

            _current.numLoadedPlugins += numLoadedPlugins;
        }
    }

    // audio is now processing new preset

    // scope for preloading default preset state
    {
        const Preset& defaults = _presets[old.preset];
        // bool defloaded[NUM_BLOCKS_PER_PRESET];

        const Host::NonBlockingScope hnbs(_host);

        for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
        {
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                const Block& defblockdata = defaults.chains[row].blocks[bl];
                const Block& oldblockdata = old.chains[row].blocks[bl];

                // using same plugin (or both empty)
                if (defblockdata.uri == oldblockdata.uri)
                {
                    if (isNullBlock(defblockdata))
                        continue;

                    const HostBlockPair hbp = _mapper.get(old.preset, row, bl);
                    assert_continue(hbp.id != kMaxHostInstances);

                    if (defblockdata.enabled != oldblockdata.enabled)
                    {
                        _host.bypass(hbp.id, !defblockdata.enabled);

                        if (hbp.pair != kMaxHostInstances)
                            _host.bypass(hbp.pair, !defblockdata.enabled);
                    }

                    params.clear();

                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        const Parameter& defparamdata(defblockdata.parameters[p]);
                        const Parameter& oldparamdata(oldblockdata.parameters[p]);

                        if (isNullURI(defparamdata.symbol))
                            break;
                        if (defparamdata.value == oldparamdata.value)
                            continue;

                        params.push_back({ defparamdata.symbol.c_str(), defparamdata.value });
                    }

                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        const Property& defpropdata(defblockdata.properties[p]);
                        const Property& oldpropdata(oldblockdata.properties[p]);

                        if (isNullURI(defpropdata.uri))
                            break;
                        if (defpropdata.value == oldpropdata.value)
                            continue;

                        _host.patch_set(hbp.id, defpropdata.uri.c_str(), defpropdata.value.c_str());

                        if (hbp.pair != kMaxHostInstances)
                            _host.patch_set(hbp.pair, defpropdata.uri.c_str(), defpropdata.value.c_str());
                    }

                    _host.params_flush(hbp.id, LV2_KXSTUDIO_PROPERTIES_RESET_FULL, params.size(), params.data());

                    if (hbp.pair != kMaxHostInstances)
                        _host.params_flush(hbp.pair, LV2_KXSTUDIO_PROPERTIES_RESET_FULL, params.size(), params.data());

                    continue;
                }

                // different plugin, unload old one if there is any
                if (oldloaded[row][bl])
                {
                    const HostBlockPair hbp = _mapper.remove(old.preset, row, bl);

                    if (hbp.id != kMaxHostInstances)
                        _host.remove(hbp.id);

                    if (hbp.pair != kMaxHostInstances)
                        _host.remove(hbp.pair);
                }

                // nothing else to do if block is empty
                if (isNullBlock(defaults.chains[row].blocks[bl]))
                    continue;

                // otherwise load default plugin
                HostBlockPair hbp = { _mapper.add(old.preset, row, bl), kMaxHostInstances };
                _host.preload(defblockdata.uri.c_str(), hbp.id);

                if (shouldBlockBeStereo(defaults.chains[row], bl))
                {
                    hbp.pair = _mapper.add_pair(old.preset, row, bl);
                    _host.preload(defblockdata.uri.c_str(), hbp.pair);
                }

                if (!defblockdata.enabled)
                {
                    _host.bypass(hbp.id, true);

                    if (hbp.pair != kMaxHostInstances)
                        _host.bypass(hbp.pair, true);
                }

                params.clear();

                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const Parameter& defparamdata(defblockdata.parameters[p]);
                    if (isNullURI(defparamdata.symbol))
                        break;

                    params.push_back({ defparamdata.symbol.c_str(), defparamdata.value });
                }

                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const Property& defpropdata(defblockdata.properties[p]);
                    if (isNullURI(defpropdata.uri))
                        break;

                    _host.patch_set(hbp.id, defpropdata.uri.c_str(), defpropdata.value.c_str());

                    if (hbp.pair != kMaxHostInstances)
                        _host.patch_set(hbp.pair, defpropdata.uri.c_str(), defpropdata.value.c_str());
                }

                _host.params_flush(hbp.id, LV2_KXSTUDIO_PROPERTIES_RESET_FULL, params.size(), params.data());

                if (hbp.pair != kMaxHostInstances)
                    _host.params_flush(hbp.pair, LV2_KXSTUDIO_PROPERTIES_RESET_FULL, params.size(), params.data());
            }
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostFeedbackCallback(const HostFeedbackData& data)
{
    if (_callback == nullptr)
        return;

    HostCallbackData cdata = {};

    switch (data.type)
    {
    case HostFeedbackData::kFeedbackAudioMonitor:
        cdata.type = HostCallbackData::kAudioMonitor;
        cdata.audioMonitor.index = data.audioMonitor.index;
        cdata.audioMonitor.value = data.audioMonitor.value;
        break;

    case HostFeedbackData::kFeedbackCpuLoad:
        cdata.type = HostCallbackData::kCpuLoad;
        cdata.cpuLoad.avg = data.cpuLoad.avg;
        cdata.cpuLoad.max = data.cpuLoad.max;
        cdata.cpuLoad.xruns = data.cpuLoad.xruns;
        break;

    case HostFeedbackData::kFeedbackLog:
        cdata.type = HostCallbackData::kLog;
        cdata.log.type = data.log.type;
        cdata.log.msg = data.log.msg;
        break;

    case HostFeedbackData::kFeedbackParameterSet:
    case HostFeedbackData::kFeedbackOutputMonitor:
        assert(data.paramSet.effect_id >= 0);
        assert(data.paramSet.effect_id < MAX_MOD_HOST_INSTANCES);

        if (data.paramSet.effect_id >= MAX_MOD_HOST_PLUGIN_INSTANCES)
        {
            cdata.type = HostCallbackData::kToolParameterSet;
            cdata.toolParameterSet.index = data.paramSet.effect_id - MAX_MOD_HOST_PLUGIN_INSTANCES;
            cdata.toolParameterSet.symbol = data.paramSet.symbol;
            cdata.toolParameterSet.value = data.paramSet.value;
        }
        else
        {
            const HostBlockAndRow hbar = _mapper.get_block_with_id(_current.preset, data.paramSet.effect_id);
            if (hbar.row == NUM_BLOCK_CHAIN_ROWS || hbar.block == NUM_BLOCKS_PER_PRESET)
                return;

            if (data.paramSet.symbol[0] == ':')
            {
                // _current.dirty = true;
                // blockdata.enabled = data.paramSet.value < 0.5f;

                // TODO special mod-host values here
                return;
            }
            else
            {
                Block& blockdata = _current.chains[hbar.row].blocks[hbar.block];

                uint8_t p = 0;
                for (; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    if (isNullURI(blockdata.parameters[p].symbol))
                        return;
                    if (blockdata.parameters[p].symbol == data.paramSet.symbol)
                        break;
                }

                if (p == MAX_PARAMS_PER_BLOCK)
                    return;

                if (data.type == HostFeedbackData::kFeedbackParameterSet)
                    _current.dirty = true;

                blockdata.parameters[p].value = data.paramSet.value;

                cdata.type = HostCallbackData::kParameterSet;
                cdata.parameterSet.row = hbar.row;
                cdata.parameterSet.block = hbar.block;
                cdata.parameterSet.index = p;
                cdata.parameterSet.symbol = data.paramSet.symbol;
                cdata.parameterSet.value = data.paramSet.value;
            }
        }
        break;

    case HostFeedbackData::kFeedbackPatchSet:
        assert(data.patchSet.effect_id >= 0);
        assert(data.patchSet.effect_id < MAX_MOD_HOST_INSTANCES);

        if (data.patchSet.effect_id >= MAX_MOD_HOST_PLUGIN_INSTANCES)
        {
            cdata.type = HostCallbackData::kToolPatchSet;
            cdata.toolPatchSet.index = data.patchSet.effect_id - MAX_MOD_HOST_PLUGIN_INSTANCES;
            cdata.toolPatchSet.key = data.patchSet.key;
            cdata.toolPatchSet.type = data.patchSet.type;
            std::memcpy(&cdata.toolPatchSet.data, &data.patchSet.data, sizeof(data.patchSet.data));
        }
        else
        {
            const HostBlockAndRow hbar = _mapper.get_block_with_id(_current.preset, data.paramSet.effect_id);
            if (hbar.row == NUM_BLOCK_CHAIN_ROWS || hbar.block == NUM_BLOCKS_PER_PRESET)
                return;

            cdata.type = HostCallbackData::kPatchSet;
            cdata.patchSet.row = hbar.row;
            cdata.patchSet.block = hbar.block;
            cdata.patchSet.key = data.patchSet.key;
            cdata.patchSet.type = data.patchSet.type;
            std::memcpy(&cdata.patchSet.data, &data.patchSet.data, sizeof(data.patchSet.data));
        }
        break;

    default:
        return;
    }

    _callback->hostConnectorCallback(cdata);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostReady()
{
    const Host::NonBlockingScope hnbs(_host);

    _host.monitor_audio_levels(JACK_CAPTURE_PORT_1, true);

    if constexprstr (std::strcmp(JACK_CAPTURE_PORT_1, JACK_CAPTURE_PORT_2) != 0)
        _host.monitor_audio_levels(JACK_CAPTURE_PORT_2, true);

    _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_1, true);

    if constexprstr (std::strcmp(JACK_PLAYBACK_MONITOR_PORT_1, JACK_PLAYBACK_MONITOR_PORT_2) != 0)
        _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_2, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::initBlock(HostConnector::Block& blockdata,
                              const Lv2Plugin* const plugin,
                              const uint8_t numInputs,
                              const uint8_t numOutputs,
                              const uint8_t numSideInputs,
                              const uint8_t numSideOutputs,
                              std::unordered_map<std::string, uint8_t>* paramToIndexMapOpt,
                              std::unordered_map<std::string, uint8_t>* propToIndexMapOpt) const
{
    assert(plugin != nullptr);

    blockdata.enabled = true;
    blockdata.uri = plugin->uri;
    blockdata.quickPotSymbol.clear();

    blockdata.meta.enable.hasScenes = false;
    blockdata.meta.enable.hwbinding = UINT8_MAX;
    blockdata.meta.quickPotIndex = 0;
    blockdata.meta.numParamsInScenes = 0;
    blockdata.meta.numPropertiesInScenes = 0;
    blockdata.meta.numInputs = numInputs;
    blockdata.meta.numOutputs = numOutputs;
    blockdata.meta.numSideInputs = numSideInputs;
    blockdata.meta.numSideOutputs = numSideOutputs;
    blockdata.meta.name = plugin->name;
    blockdata.meta.abbreviation = plugin->abbreviation;

    std::unordered_map<std::string, uint8_t> paramToIndexMapLocal;
    std::unordered_map<std::string, uint8_t>& paramToIndexMap = paramToIndexMapOpt != nullptr
                                                              ? *paramToIndexMapOpt
                                                              : paramToIndexMapLocal;

    std::unordered_map<std::string, uint8_t> propToIndexMapLocal;
    std::unordered_map<std::string, uint8_t>& propToIndexMap = propToIndexMapOpt != nullptr
                                                             ? *propToIndexMapOpt
                                                             : propToIndexMapLocal;

    uint8_t numParams = 0;
    for (const Lv2Port& port : plugin->ports)
    {
        if ((port.flags & (Lv2PortIsControl|Lv2ParameterHidden)) != Lv2PortIsControl)
            continue;

        switch (port.designation)
        {
        case kLv2DesignationNone:
            break;
        case kLv2DesignationEnabled:
        case kLv2DesignationReset:
            // skip parameter
            continue;
        case kLv2DesignationQuickPot:
            blockdata.quickPotSymbol = port.symbol;
            blockdata.meta.quickPotIndex = numParams;
            break;
        }

        paramToIndexMap[port.symbol] = numParams;

        blockdata.parameters[numParams++] = {
            .symbol = port.symbol,
            .value = port.def,
            .meta = {
                .flags = port.flags,
                .hwbinding = UINT8_MAX,
                .def = port.def,
                .min = port.min,
                .max = port.max,
                .name = port.name,
                .shortname = port.shortname,
                .unit = port.unit,
                .scalePoints = port.scalePoints,
            },
        };

        if (numParams == MAX_PARAMS_PER_BLOCK)
            break;
    }

    uint8_t numProps = 0;
    for (const Lv2Property& prop : plugin->properties)
    {
        if ((prop.flags & Lv2ParameterHidden) != 0)
            continue;

        propToIndexMap[prop.uri] = numProps;

        blockdata.properties[numProps++] = {
            .uri = prop.uri,
            .value = {},
            .meta = {
                .flags = prop.flags,
                .hwbinding = UINT8_MAX,
                .name = prop.name,
                .shortname = prop.shortname,
                .scalePoints = prop.scalePoints,
            },
        };

        if (numProps == MAX_PARAMS_PER_BLOCK)
            break;
    }

    if (blockdata.quickPotSymbol.empty() && numParams != 0)
        blockdata.quickPotSymbol = blockdata.parameters[0].symbol;

    for (uint8_t p = numParams; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetParameter(blockdata.parameters[p]);

    for (uint8_t p = numProps; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetProperty(blockdata.properties[p]);

    for (uint8_t s = 0; s < NUM_SCENES_PER_PRESET; ++s)
        blockdata.sceneValues[s].enabled = false;

    // override defaults from user
    const std::string defdir = getDefaultPluginBundleForBlock(blockdata);

    if (! std::filesystem::exists(defdir))
        return;

    const std::unordered_map<std::string, float> statemap
        = lv2world.load_plugin_state((defdir + "/default.ttl").c_str());

    for (const auto& state : statemap)
    {
        const std::string symbol = state.first;
        const float value = state.second;

        if (paramToIndexMap.find(symbol) == paramToIndexMap.end())
        {
            mod_log_warn("initBlock(): state param with '%s' symbol does not exist in plugin", symbol.c_str());
            continue;
        }

        const uint8_t paramIndex = paramToIndexMap[symbol];
        Parameter& paramdata = blockdata.parameters[paramIndex];

        if (isNullURI(paramdata.symbol))
            continue;
        if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
            continue;

        paramdata.meta.def = paramdata.value = value;
    }

    // TODO handle properties

    std::ifstream f(defdir + "/defaults.json");
    nlohmann::json j;
    std::string jquickpot;

    try {
        j = nlohmann::json::parse(f);
        jquickpot = j["quickpot"].get<std::string>();
    } catch (const std::exception& e) {
        mod_log_warn("failed to parse block defaults: %s", e.what());
        return;
    } catch (...) {
        mod_log_warn("failed to parse block defaults: unknown exception");
        return;
    }

    for (uint8_t p = 0; p < numParams; ++p)
    {
        if (blockdata.parameters[p].symbol == jquickpot)
        {
            blockdata.quickPotSymbol = jquickpot;
            blockdata.meta.quickPotIndex = p;
            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::allocPreset(Preset& preset)
{
    preset.chains[0].capture[0] = JACK_CAPTURE_PORT_1;
    preset.chains[0].capture[1] = JACK_CAPTURE_PORT_2;
    preset.chains[0].playback[0] = JACK_PLAYBACK_PORT_1;
    preset.chains[0].playback[1] = JACK_PLAYBACK_PORT_2;

    for (ChainRow& chain : preset.chains)
    {
        chain.blocks.resize(NUM_BLOCKS_PER_PRESET);

        for (Block& block : chain.blocks)
            allocBlock(block);
    }
}

void HostConnector::resetPreset(Preset& preset)
{
    preset.uuid = generateUUID();
    preset.scene = 0;
    preset.name.clear();
    preset.background.color = 0;
    preset.background.style.clear();

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        if (row != 0)
        {
            preset.chains[row].capture.fill({});
            preset.chains[row].playback.fill({});
        }

        preset.chains[row].captureId.fill(kMaxHostInstances);
        preset.chains[row].playbackId.fill(kMaxHostInstances);

        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            resetBlock(preset.chains[row].blocks[bl]);
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        preset.bindings[hwid].value = 0.f;
        preset.bindings[hwid].params.clear();
        preset.bindings[hwid].properties.clear();
    }
}

// --------------------------------------------------------------------------------------------------------------------
