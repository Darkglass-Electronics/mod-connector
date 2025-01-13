// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"
#include "json.hpp"
#include "utils.hpp"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <map>

#define KXSTUDIO__Reset_full 1
#define KXSTUDIO__Reset_soft 2

#define JSON_BANK_VERSION_CURRENT 0
#define JSON_BANK_VERSION_MIN_SUPPORTED 0
#define JSON_BANK_VERSION_MAX_SUPPORTED 0

#ifdef BINDING_ACTUATOR_IDS
static constexpr const char* kBindingActuatorIDs[NUM_BINDING_ACTUATORS] = { BINDING_ACTUATOR_IDS };
#endif

typedef std::list<HostConnector::Binding>::iterator BindingIterator;
typedef std::list<HostConnector::Binding>::const_iterator BindingIteratorConst;

// --------------------------------------------------------------------------------------------------------------------

static void resetParam(HostConnector::Parameter& paramdata)
{
    paramdata = {};
    paramdata.meta.max = 1.f;
}

static void resetBlock(HostConnector::Block& blockdata)
{
    blockdata.enabled = false;
    blockdata.quickPotSymbol.clear();
    blockdata.uri.clear();
    blockdata.meta.quickPotIndex = 0;
    blockdata.meta.hasScenes = false;
    blockdata.meta.isChainPoint = false;
    blockdata.meta.isMonoIn = false;
    blockdata.meta.isStereoOut = false;
    blockdata.meta.name.clear();

    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetParam(blockdata.parameters[p]);

    for (uint8_t s = 0; s <= NUM_SCENES_PER_PRESET; ++s)
        for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            blockdata.sceneValues[s][p].used = false;
}

static void resetPreset(HostConnector::Preset& preset)
{
    preset.name.clear();

    for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        resetBlock(preset.blocks[bl]);

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
        preset.bindings[hwid].clear();
}

// --------------------------------------------------------------------------------------------------------------------

static void allocBlock(HostConnector::Block& blockdata)
{
    blockdata.parameters.resize(MAX_PARAMS_PER_BLOCK);

    for (uint8_t s = 0; s <= NUM_SCENES_PER_PRESET; ++s)
        blockdata.sceneValues[s].resize(MAX_PARAMS_PER_BLOCK);
}

static void allocPreset(HostConnector::Preset& preset)
{
    preset.blocks.resize(NUM_BLOCKS_PER_PRESET);

    for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        allocBlock(preset.blocks[bl]);
}

static bool getSupportedPluginIO(const Lv2Plugin* const plugin, uint8_t& numInputs, uint8_t& numOutputs)
{
    numInputs = numOutputs = 0;
    for (size_t i = 0; i < plugin->ports.size(); ++i)
    {
        if ((plugin->ports[i].flags & Lv2PortIsAudio) == 0)
            continue;

        if (plugin->ports[i].flags & Lv2PortIsOutput)
        {
            if (++numOutputs > 2)
                break;
        }
        else
        {
            if (++numInputs > 2)
                break;
        }
    }

    return numInputs <= 2 && numOutputs <= 2;
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

    if (ok)
        hostReady();
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::reconnect()
{
    if (_host.reconnect())
    {
        hostReady();
        return true;
    }

    return false;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::printStateForDebug(const bool withBlocks, const bool withParams, const bool withBindings)
{
    fprintf(stderr, "------------------------------------------------------------------\n");
    fprintf(stderr, "Dumping current state:\n");
    fprintf(stderr, "\tPreset: %u\n", _current.preset);
    fprintf(stderr, "\tScene: %u\n", _current.scene);
    fprintf(stderr, "\tNum loaded plugins: %u\n", _current.numLoadedPlugins);
    fprintf(stderr, "\tDirty: %s\n", bool2str(_current.dirty));
    fprintf(stderr, "\tFilename: %s\n", _current.filename.c_str());
    fprintf(stderr, "\tName: %s\n", _current.name.c_str());

    for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET && (withBlocks || withParams); ++bl)
    {
        const Block& blockdata(_current.blocks[bl]);

        if (isNullURI(blockdata.uri))
        {
            fprintf(stderr, "\n\tBlock %u: (empty)\n", bl);
            continue;
        }

        fprintf(stderr, "\n\tBlock %u: %s | %s\n", bl, blockdata.uri.c_str(), blockdata.meta.name.c_str());

        if (withBlocks)
        {
            fprintf(stderr, "\t\tQuick Pot: '%s' | %u\n", blockdata.quickPotSymbol.c_str(), blockdata.meta.quickPotIndex);
            fprintf(stderr, "\t\tHas scenes: %s\n", bool2str(blockdata.meta.hasScenes));
            fprintf(stderr, "\t\tIs chain point: %s\n", bool2str(blockdata.meta.isChainPoint));
            fprintf(stderr, "\t\tIs mono in: %s\n", bool2str(blockdata.meta.isMonoIn));
            fprintf(stderr, "\t\tIs stereo out: %s\n", bool2str(blockdata.meta.isStereoOut));
        }

        for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK && withParams; ++p)
        {
            const Parameter& paramdata(blockdata.parameters[p]);

            fprintf(stderr, "\t\tParameter %u: '%s' | '%s'\n", p, paramdata.symbol.c_str(), paramdata.meta.name.c_str());
            fprintf(stderr, "\t\t\tFlags: %x\n", paramdata.meta.flags);
            fprintf(stderr, "\t\t\tDefault: %f\n", paramdata.meta.def);
            fprintf(stderr, "\t\t\tMinimum: %f\n", paramdata.meta.min);
            fprintf(stderr, "\t\t\tMaximum: %f\n", paramdata.meta.max);
            fprintf(stderr, "\t\t\tUnit: %s\n", paramdata.meta.unit.c_str());
        }
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS && withBindings; ++hwid)
    {
       #ifdef BINDING_ACTUATOR_IDS
        const std::string hwname = kBindingActuatorIDs[hwid];
       #else
        const std::string hwname = std::to_string(hwid + 1);
       #endif
        fprintf(stderr, "\n\tBindings for '%s':\n", hwname.c_str());

        if (_current.bindings[hwid].empty())
        {
            fprintf(stderr, "\t\t(empty)\n");
            continue;
        }

        for (HostConnector::Binding& bindingdata : _current.bindings[hwid])
        {
            fprintf(stderr, "\t\t- Block %u, Parameter '%s' | %u\n",
                    bindingdata.block,
                    bindingdata.parameterSymbol.c_str(),
                    bindingdata.meta.parameterIndex);
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

bool HostConnector::loadBankFromFile(const char* const filename)
{
    std::ifstream f(filename);
    nlohmann::json j;

    try {
        j = nlohmann::json::parse(f);
    } catch (...) {
        return false;
    }

    if (! (j.contains("bank") && j.contains("type") && j.contains("version")))
        return false;

    try {
        if (j["type"].get<std::string>() != "bank")
        {
            fprintf(stderr, "HostConnector::loadBankFromFile failed: file is not bank type\n");
            return false;
        }

        const int version = j["version"].get<int>();
        if (version < JSON_BANK_VERSION_MIN_SUPPORTED || version > JSON_BANK_VERSION_MAX_SUPPORTED)
        {
            fprintf(stderr, "HostConnector::loadBankFromFile failed: version mismatch\n");
            return false;
        }

        j = j["bank"].get<nlohmann::json>();
    } catch (...) {
        return false;
    }

    if (! j.contains("presets"))
    {
        fprintf(stderr, "HostConnector::loadBankFromFile: bank does not include presets\n");
        return false;
    }

    uint8_t numLoadedPlugins = 0;

    do {
        auto& jpresets = j["presets"];
        for (uint8_t pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
        {
            Preset& preset = _presets[pr];
            const std::string jpresetid = std::to_string(pr + 1);

            if (! jpresets.contains(jpresetid))
            {
                printf("HostConnector::loadBankFromFile: missing preset #%u, loading empty\n", pr + 1);
                resetPreset(preset);
                continue;
            }

            std::string name;

            auto& jpreset = jpresets[jpresetid];
            if (jpreset.contains("name"))
            {
                try {
                    name = jpreset["name"].get<std::string>();
                } catch (...) {}
            }

            if (! jpreset.contains("blocks"))
            {
                printf("HostConnector::loadBankFromFile: preset #%u does not include blocks, loading empty\n", pr + 1);
                resetPreset(preset);
                preset.name = name;
                continue;
            }

            preset.name = name;

            auto& jblocks = jpreset["blocks"];
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                Block& blockdata = preset.blocks[bl];
                const std::string jblockid = std::to_string(bl + 1);

                if (! jblocks.contains(jblockid))
                {
                    resetBlock(blockdata);
                    continue;
                }

                auto& jblock = jblocks[jblockid];
                if (! jblock.contains("uri"))
                {
                    printf("HostConnector::loadBankFromFile: preset #%u / block #%u does not include uri, loading empty\n",
                           pr + 1, bl + 1);
                    resetBlock(blockdata);
                    continue;
                }

                const std::string uri = jblock["uri"].get<std::string>();

                const Lv2Plugin* const plugin = !isNullURI(uri)
                                              ? lv2world.get_plugin_by_uri(uri.c_str())
                                              : nullptr;

                if (plugin == nullptr)
                {
                    printf("HostConnector::loadBankFromFile: plugin with uri '%s' not available, using empty block\n",
                           uri.c_str());
                    resetBlock(blockdata);
                    continue;
                }

                uint8_t numInputs, numOutputs;
                if (! getSupportedPluginIO(plugin, numInputs, numOutputs))
                {
                    printf("HostConnector::loadBankFromFile: plugin with uri '%s' has invalid IO, using empty block\n",
                           uri.c_str());
                    resetBlock(blockdata);
                    continue;
                }

                blockdata.enabled = true;
                blockdata.uri = uri;
                blockdata.quickPotSymbol.clear();

                blockdata.meta.quickPotIndex = 0;
                blockdata.meta.hasScenes = false;
                blockdata.meta.isChainPoint = false;
                blockdata.meta.isMonoIn = numInputs == 1;
                blockdata.meta.isStereoOut = numOutputs == 2;
                blockdata.meta.name = plugin->name;
                blockdata.meta.abbreviation = plugin->abbreviation;

                if (jblock.contains("enabled"))
                    blockdata.enabled = jblock["enabled"].get<bool>();

                if (pr == 0)
                    ++numLoadedPlugins;

                // parameters are always filled from lv2 metadata first, then overriden with json data
                uint8_t numParams = 0;
                std::map<std::string, uint8_t> symbolToIndexMap;
                for (size_t i = 0; i < plugin->ports.size() && numParams < MAX_PARAMS_PER_BLOCK; ++i)
                {
                    if ((plugin->ports[i].flags & Lv2PortIsControl) == 0)
                        continue;
                    if (plugin->ports[i].flags & Lv2ParameterHidden)
                        continue;

                    switch (plugin->ports[i].designation)
                    {
                    case kLv2DesignationNone:
                        break;
                    case kLv2DesignationEnabled:
                    case kLv2DesignationReset:
                        // skip parameter
                        continue;
                    case kLv2DesignationQuickPot:
                        blockdata.quickPotSymbol = plugin->ports[i].symbol;
                        blockdata.meta.quickPotIndex = numParams;
                        break;
                    }

                    HostConnector::Parameter& paramdata = blockdata.parameters[numParams];

                    paramdata.symbol = plugin->ports[i].symbol;
                    paramdata.value = plugin->ports[i].def;

                    paramdata.meta.flags = plugin->ports[i].flags;
                    paramdata.meta.def = plugin->ports[i].def;
                    paramdata.meta.min = plugin->ports[i].min;
                    paramdata.meta.max = plugin->ports[i].max;
                    paramdata.meta.name = plugin->ports[i].name;
                    paramdata.meta.shortname = plugin->ports[i].shortname;
                    paramdata.meta.unit = plugin->ports[i].unit;
                    paramdata.meta.scalePoints = plugin->ports[i].scalePoints;

                    symbolToIndexMap[paramdata.symbol] = numParams++;
                }

                for (uint8_t p = numParams; p < MAX_PARAMS_PER_BLOCK; ++p)
                    resetParam(blockdata.parameters[p]);

                for (uint8_t s = 0; s <= NUM_SCENES_PER_PRESET; ++s)
                {
                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                        blockdata.sceneValues[s][p].used = false;
                }

                try {
                    const std::string quickpot = jblock["quickpot"].get<std::string>();

                    for (uint8_t p = 0; p < numParams; ++p)
                    {
                        if (blockdata.parameters[p].symbol == quickpot)
                        {
                            blockdata.quickPotSymbol = quickpot;
                            blockdata.meta.quickPotIndex = p;
                            break;
                        }
                    }

                } catch (...) {}

                if (blockdata.quickPotSymbol.empty() && numParams != 0)
                    blockdata.quickPotSymbol = blockdata.parameters[0].symbol;

                if (! jblock.contains("parameters"))
                    continue;

                auto& jparams = jblock["parameters"];
                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const std::string jparamid = std::to_string(p + 1);

                    if (! jparams.contains(jparamid))
                        continue;

                    auto& jparam = jparams[jparamid];
                    if (! (jparam.contains("symbol") && jparam.contains("value")))
                    {
                        printf("HostConnector::loadBankFromFile: param #%u is missing symbol and/or value\n", p + 1);
                        continue;
                    }

                    const std::string symbol = jparam["symbol"].get<std::string>();

                    if (symbolToIndexMap.find(symbol) == symbolToIndexMap.end())
                    {
                        printf("HostConnector::loadBankFromFile: param with '%s' symbol does not exist in plugin\n",
                               symbol.c_str());
                        continue;
                    }

                    const uint8_t parameterIndex = symbolToIndexMap[symbol];
                    Parameter& paramdata = blockdata.parameters[parameterIndex];

                    if (isNullURI(paramdata.symbol))
                        continue;
                    if (paramdata.meta.flags & Lv2PortIsOutput)
                        continue;

                    paramdata.value = std::max(paramdata.meta.min,
                                               std::min<float>(paramdata.meta.max,
                                                               jparam["value"].get<double>()));
                }

                if (! jblock.contains("scenes"))
                    continue;

                auto& jallscenes = jblock["scenes"];
                for (uint8_t sid = 1; sid <= NUM_SCENES_PER_PRESET; ++sid)
                {
                    const std::string jsceneid = std::to_string(sid);

                    if (! jallscenes.contains(jsceneid))
                        continue;

                    auto& jscenes = jallscenes[jsceneid];
                    if (! jscenes.is_array())
                    {
                        printf("HostConnector::loadBankFromFile: preset #%u scenes are not arrays\n", pr + 1);
                        continue;
                    }

                    for (auto& jscene : jscenes)
                    {
                        if (! (jscene.contains("symbol") && jscene.contains("value")))
                        {
                            printf("HostConnector::loadBankFromFile: scene param is missing symbol and/or value\n");
                            continue;
                        }

                        const std::string symbol = jscene["symbol"].get<std::string>();

                        if (symbolToIndexMap.find(symbol) == symbolToIndexMap.end())
                        {
                            printf("HostConnector::loadBankFromFile: scene param with '%s' symbol does not exist\n",
                                   symbol.c_str());
                            continue;
                        }

                        const uint8_t parameterIndex = symbolToIndexMap[symbol];
                        const Parameter& paramdata = blockdata.parameters[parameterIndex];

                        if (isNullURI(paramdata.symbol))
                            continue;
                        if (paramdata.meta.flags & Lv2PortIsOutput)
                            continue;

                        SceneParameterValue& sceneparamdata = blockdata.sceneValues[sid][parameterIndex];

                        sceneparamdata.used = true;
                        sceneparamdata.value = std::max(paramdata.meta.min,
                                                        std::min<float>(paramdata.meta.max,
                                                                        jscene["value"].get<double>()));

                        // extra data for when scenes are in use
                        blockdata.meta.hasScenes = true;
                        blockdata.sceneValues[0][parameterIndex].used = true;
                        blockdata.sceneValues[0][parameterIndex].value = paramdata.value;
                    }
                }
            }

            for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
                preset.bindings[hwid].clear();

            if (! jpreset.contains("bindings"))
            {
                printf("HostConnector::loadBankFromFile: preset #%u does not include any bindings\n", pr + 1);
                continue;
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
                    printf("HostConnector::loadBankFromFile: preset #%u does not include bindings for hw '%s'\n",
                           pr + 1, jbindingsid.c_str());
                    continue;
                }

                auto& jbindings = jallbindings[jbindingsid];
                if (! jbindings.is_array())
                {
                    printf("HostConnector::loadBankFromFile: preset #%u bindings are not arrays\n", pr + 1);
                    continue;
                }

                for (auto& jbinding : jbindings)
                {
                    if (! (jbinding.contains("block") && jbinding.contains("symbol")))
                    {
                        printf("HostConnector::loadBankFromFile: binding is missing block and/or symbol\n");
                        continue;
                    }

                    const int block = jbinding["block"].get<int>();
                    if (block < 1 || block > NUM_BLOCKS_PER_PRESET)
                    {
                        printf("HostConnector::loadBankFromFile: binding has out of bounds block %d\n", block);
                        continue;
                    }
                    const Block& blockdata = preset.blocks[block - 1];

                    const std::string symbol = jbinding["symbol"].get<std::string>();

                    if (symbol == ":bypass")
                    {
                        preset.bindings[hwid].push_back({
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
                            const Parameter& paramdata = blockdata.parameters[p];

                            if (isNullURI(paramdata.symbol))
                                break;
                            if (paramdata.meta.flags & Lv2PortIsOutput)
                                continue;

                            if (paramdata.symbol == symbol)
                            {
                                preset.bindings[hwid].push_back({
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
            }
        }
    } while(false);

    // always start with the first preset and scene
    static_cast<Preset&>(_current) = _presets[0];
    _current.preset = 0;
    _current.scene = 0;
    _current.numLoadedPlugins = numLoadedPlugins;
    _current.dirty = false;
    _current.filename = filename;

    const Host::NonBlockingScope hnbs(_host);
    hostClearAndLoadCurrentBank();
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::saveBank()
{
    if (_current.filename.empty())
        return false;

    return saveBankToFile(_current.filename.c_str());
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::saveBankToFile(const char* const filename)
{
    // copy current data into preset data
    _presets[_current.preset] = static_cast<Preset&>(_current);

    // store parameter values from default scene, if in use
    for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
    {
        HostConnector::Block& blockdata = _presets[_current.preset].blocks[bl];
        if (isNullURI(blockdata.uri))
            continue;

        for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        {
            Parameter& paramdata = blockdata.parameters[p];
            if (isNullURI(paramdata.symbol))
                break;
            if (paramdata.meta.flags & Lv2PortIsOutput)
                continue;

            if (blockdata.sceneValues[0][p].used)
                paramdata.value = blockdata.sceneValues[0][p].value;
        }
    }

    nlohmann::json j;
    try {
        j["version"] = JSON_BANK_VERSION_CURRENT;
        j["type"] = "bank";

        auto& jbank = j["bank"] = nlohmann::json::object({
            { "presets", nlohmann::json::object({}) },
        });

        auto& jpresets = jbank["presets"];

        for (uint8_t pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
        {
            const Preset& preset = _presets[pr];
            const std::string jpresetid = std::to_string(pr + 1);

            auto& jpreset = jpresets[jpresetid] = nlohmann::json::object({
                { "bindings", nlohmann::json::object({}) },
                { "blocks", nlohmann::json::object({}) },
                { "name", preset.name },
            });

            auto& jallbindings = jpreset["bindings"];

            for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
            {
               #ifdef BINDING_ACTUATOR_IDS
                const std::string jbindingsid = kBindingActuatorIDs[hwid];
               #else
                const std::string jbindingsid = std::to_string(hwid + 1);
               #endif
                auto& jbindings = jallbindings[jbindingsid] = nlohmann::json::array();

                for (const Binding& bindingdata : preset.bindings[hwid])
                {
                    jbindings.push_back({
                        { "block", bindingdata.block + 1 },
                        { "symbol", bindingdata.parameterSymbol },
                    });
                }
            }

            auto& jblocks = jpreset["blocks"];

            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                const HostConnector::Block& blockdata = preset.blocks[bl];

                if (isNullURI(blockdata.uri))
                    continue;

                const std::string jblockid = std::to_string(bl + 1);
                auto& jblock = jblocks[jblockid] = {
                    { "enabled", blockdata.enabled },
                    { "parameters", nlohmann::json::object({}) },
                    { "quickpot", blockdata.quickPotSymbol },
                    { "scenes", nlohmann::json::object({}) },
                    { "uri", isNullURI(blockdata.uri) ? "-" : blockdata.uri },
                };

                auto& jparams = jblock["parameters"];
                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const Parameter& paramdata = blockdata.parameters[p];

                    if (isNullURI(paramdata.symbol))
                        break;
                    if (paramdata.meta.flags & Lv2PortIsOutput)
                        continue;

                    const std::string jparamid = std::to_string(p + 1);
                    jparams[jparamid] = {
                        { "symbol", paramdata.symbol },
                        { "value", paramdata.value },
                    };
                }

                if (blockdata.meta.hasScenes)
                {
                    auto& jallscenes = jblock["scenes"];

                    for (uint8_t sid = 1; sid <= NUM_SCENES_PER_PRESET; ++sid)
                    {
                        const std::string jsceneid = std::to_string(sid);
                        auto& jscenes = jallscenes[jsceneid] = nlohmann::json::array();

                        for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                        {
                            const SceneParameterValue& sceneparamdata = blockdata.sceneValues[sid][p];

                            if (! sceneparamdata.used)
                                continue;

                            jscenes.push_back({
                                { "symbol", blockdata.parameters[p].symbol },
                                { "value", sceneparamdata.value },
                            });
                        }
                    }
                }
            }
        }
    } catch (...) {
        return false;
    }

    {
        std::ofstream o(filename);
        o << std::setw(2) << j << std::endl;
    }

    _current.dirty = false;
    _current.filename = filename;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::clearCurrentPreset()
{
    if (_current.numLoadedPlugins == 0)
        return;

    const Host::NonBlockingScope hnbs(_host);

    _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOffWithFadeOut);

    for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        if (! isNullURI(_current.blocks[b].uri))
            hostRemoveInstanceForBlock(b);

        resetBlock(_current.blocks[b]);
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
        _current.bindings[hwid].clear();

    _current.scene = 0;
    _current.numLoadedPlugins = 0;
    _current.dirty = true;

    // direct connections
    _host.connect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
    _host.connect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());

    _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOnWithFadeIn);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setCurrentPresetName(const char* const name)
{
    _current.name = name;
    _current.dirty = true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::enableBlock(const uint8_t block, const bool enable)
{
    assert(block < NUM_BLOCKS_PER_PRESET);

    HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
    {
        fprintf(stderr, "HostConnector::enableBlock(%u, %d) - block not in use, rejected\n", block, enable);
        return false;
    }

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return false;

    blockdata.enabled = enable;
    _current.dirty = true;
    _host.bypass(bp.id, !enable);

    if (bp.pair != kMaxHostInstances)
        _host.bypass(bp.pair, !enable);

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::reorderBlock(const uint8_t orig, const uint8_t dest)
{
    assert(orig < NUM_BLOCKS_PER_PRESET);
    assert(dest < NUM_BLOCKS_PER_PRESET);

    if (orig == dest)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - orig == dest, rejected\n", orig, dest);
        return false;
    }

    // check if we need to re-do any connections
    bool reconnect = false;
    const bool blockIsEmpty = isNullURI(_current.blocks[orig].uri);
    const uint8_t left = std::min(orig, dest);
    const uint8_t right = std::max(orig, dest);
    const uint8_t blockStart = std::max(0, left - 1);
    const uint8_t blockEnd = std::min(NUM_BLOCKS_PER_PRESET - 1, right + 1);

    for (uint8_t i = blockStart; i <= blockEnd; ++i)
    {
        if (orig == i)
            continue;
        if (isNullURI(_current.blocks[i].uri))
            continue;
        reconnect = true;
        break;
    }

    if (blockIsEmpty && ! reconnect)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - there is nothing to reorder, rejected\n", orig, dest);
        return false;
    }

    printf("HostConnector::reorderBlock(%u, %u) - reconnect %d, start %u, end %u\n",
           orig, dest, reconnect, blockStart, blockEnd);

    auto& mpreset = _mapper.map.presets[_current.preset];

    const Host::NonBlockingScope hnbs(_host);

    if (reconnect && ! blockIsEmpty)
    {
        hostDisconnectAllBlockInputs(orig);
        hostDisconnectAllBlockOutputs(orig);
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
            if (reconnect)
            {
                hostDisconnectAllBlockInputs(i - 1);
                hostDisconnectAllBlockOutputs(i - 1);
            }

            std::swap(_current.blocks[i], _current.blocks[i - 1]);
            std::swap(mpreset.blocks[i], mpreset.blocks[i - 1]);
        }

        if (reconnect)
        {
            hostEnsureStereoChain(blockStart, blockEnd);
            hostConnectAll(dest, orig);
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
            if (reconnect)
            {
                hostDisconnectAllBlockInputs(i + 1);
                hostDisconnectAllBlockOutputs(i + 1);
            }
            std::swap(_current.blocks[i], _current.blocks[i + 1]);
            std::swap(mpreset.blocks[i], mpreset.blocks[i + 1]);
        }

        if (reconnect)
        {
            hostEnsureStereoChain(blockStart, blockEnd);
            hostConnectAll(orig, dest);
        }
    }

    // update bindings
    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        for (HostConnector::Binding& bindingdata : _current.bindings[hwid])
        {
            if (bindingdata.block < left || bindingdata.block > right)
                continue;

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
        }
    }

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::replaceBlock(const uint8_t block, const char* const uri)
{
    if (block >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - out of bounds block, rejected\n", block, uri);
        return false;
    }

    HostConnector::Block& blockdata(_current.blocks[block]);

    const Host::NonBlockingScope hnbs(_host);

    if (! isNullURI(uri))
    {
        const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(uri);
        if (plugin == nullptr)
        {
            fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - plugin not available, rejected\n", block, uri);
            return false;
        }

        // we only do changes after verifying that the requested plugin exists and is valid
        uint8_t numInputs, numOutputs;
        if (! getSupportedPluginIO(plugin, numInputs, numOutputs))
        {
            fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - unsupported IO, rejected\n", block, uri);
            return false;
        }

        if (! isNullURI(blockdata.uri))
        {
            --_current.numLoadedPlugins;
            hostRemoveAllBlockBindings(block);
            hostRemoveInstanceForBlock(block);
        }

        // activate dual mono if previous plugin is stereo or also dualmono
        bool dualmono = numInputs == 1 && hostPresetBlockShouldBeStereo(_current, block);

        const uint16_t instance = _mapper.add(_current.preset, block);

        bool added = _host.add(uri, instance);
        if (added)
        {
            printf("DEBUG: block %u loaded plugin %s\n", block, uri);

            if (dualmono)
            {
                const uint16_t pair = _mapper.add_pair(_current.preset, block);

                if (! _host.add(uri, pair))
                {
                    printf("DEBUG: block %u failed to load dual-mono plugin %s: %s\n",
                        block, uri, _host.last_error.c_str());

                    added = false;
                    _host.remove(instance);
                }
            }
        }
        else
        {
            printf("DEBUG: block %u failed to load plugin %s: %s\n", block, uri, _host.last_error.c_str());
        }

        if (added)
        {
            blockdata.enabled = true;
            blockdata.uri = uri;
            blockdata.quickPotSymbol.clear();

            blockdata.meta.quickPotIndex = 0;
            blockdata.meta.hasScenes = false;
            blockdata.meta.isChainPoint = false;
            blockdata.meta.isMonoIn = numInputs == 1;
            blockdata.meta.isStereoOut = numOutputs == 2;
            blockdata.meta.name = plugin->name;
            blockdata.meta.abbreviation = plugin->abbreviation;

            uint8_t numParams = 0;
            for (size_t i = 0; i < plugin->ports.size() && numParams < MAX_PARAMS_PER_BLOCK; ++i)
            {
                if ((plugin->ports[i].flags & Lv2PortIsControl) == 0)
                    continue;
                if (plugin->ports[i].flags & Lv2ParameterHidden)
                    continue;

                switch (plugin->ports[i].designation)
                {
                case kLv2DesignationNone:
                    break;
                case kLv2DesignationEnabled:
                case kLv2DesignationReset:
                    // skip parameter
                    continue;
                case kLv2DesignationQuickPot:
                    blockdata.quickPotSymbol = plugin->ports[i].symbol;
                    blockdata.meta.quickPotIndex = numParams;
                    break;
                }

                blockdata.parameters[numParams++] = {
                    .symbol = plugin->ports[i].symbol,
                    .value = plugin->ports[i].def,
                    .meta = {
                        .flags = plugin->ports[i].flags,
                        .def = plugin->ports[i].def,
                        .min = plugin->ports[i].min,
                        .max = plugin->ports[i].max,
                        .name = plugin->ports[i].name,
                        .shortname = plugin->ports[i].shortname,
                        .unit = plugin->ports[i].unit,
                        .scalePoints = plugin->ports[i].scalePoints,
                    },
                };
            }

            if (blockdata.quickPotSymbol.empty() && numParams != 0)
                blockdata.quickPotSymbol = blockdata.parameters[0].symbol;

            for (uint8_t p = numParams; p < MAX_PARAMS_PER_BLOCK; ++p)
                resetParam(blockdata.parameters[p]);

            for (uint8_t s = 0; s <= NUM_SCENES_PER_PRESET; ++s)
            {
                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    blockdata.sceneValues[s][p].used = false;
            }
        }
        else
        {
            resetBlock(blockdata);
            _mapper.remove(_current.preset, block);
        }
    }
    else if (! isNullURI(blockdata.uri))
    {
        --_current.numLoadedPlugins;
        hostRemoveAllBlockBindings(block);
        hostRemoveInstanceForBlock(block);
        resetBlock(blockdata);
    }
    else
    {
        fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - already empty, rejected\n", block, uri);
        return false;
    }

    if (! isNullURI(blockdata.uri))
    {
        ++_current.numLoadedPlugins;

        // replace old direct connections if this is the first plugin
        if (_current.numLoadedPlugins == 1)
        {
            _host.disconnect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
            _host.disconnect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());
            hostConnectBlockToSystemInput(block);
            hostConnectBlockToSystemOutput(block);
        }
        // otherwise we need to add ourselves more carefully
        else
        {
            bool loaded[NUM_BLOCKS_PER_PRESET];
            for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
                loaded[b] = !isNullURI(_current.blocks[b].uri);

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
                for (uint8_t b = block + 1; b < NUM_BLOCKS_PER_PRESET; ++b)
                {
                    if (loaded[b])
                    {
                        after = b;
                        break;
                    }
                }
            }

            printf("replaceBlock add mode before: %u, after: %u | block: %u\n", before, after, block);

            if (after != NUM_BLOCKS_PER_PRESET)
                hostDisconnectAllBlockInputs(after);

            if (before != NUM_BLOCKS_PER_PRESET)
                hostDisconnectAllBlockOutputs(before);
            else
                before = 0;

            hostEnsureStereoChain(before, NUM_BLOCKS_PER_PRESET - 1);
            hostConnectAll(before, NUM_BLOCKS_PER_PRESET - 1);
        }
    }
    else
    {
        // use direct connections if there are no plugins
        if (_current.numLoadedPlugins == 0)
        {
            _host.connect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
            _host.connect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());
        }
        else
        {
            bool loaded[NUM_BLOCKS_PER_PRESET];
            for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
                loaded[b] = !isNullURI(_current.blocks[b].uri);

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

            hostEnsureStereoChain(start, NUM_BLOCKS_PER_PRESET - 1);
            hostConnectAll(start, NUM_BLOCKS_PER_PRESET - 1);
        }
    }

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::switchPreset(const uint8_t preset)
{
    if (_current.preset == preset || preset >= NUM_PRESETS_PER_BANK)
        return false;

    // store old active preset in memory before doing anything
    const Current old = _current;
    bool oldloaded[NUM_BLOCKS_PER_PRESET];

    // preallocating some data
    std::vector<flushed_param> params;
    params.reserve(MAX_PARAMS_PER_BLOCK);

    // copy new preset to current data
    static_cast<Preset&>(_current) = _presets[preset];
    _current.preset = preset;
    _current.scene = 0;
    _current.dirty = false;
    _current.numLoadedPlugins = 0;

    // scope for fade-out, old deactivate, new activate, fade-in
    {
        const Host::NonBlockingScope hnbs(_host);

        // step 1: fade out
        _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOffWithFadeOut);

        // step 2: disconnect and deactivate all plugins in old preset
        // NOTE not removing plugins, done after processing is reenabled
        if (old.numLoadedPlugins == 0)
        {
            std::memset(oldloaded, 0, sizeof(oldloaded));

            _host.disconnect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
            _host.disconnect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());
        }
        else
        {
            for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
            {
                if (! (oldloaded[b] = !isNullURI(old.blocks[b].uri)))
                    continue;

                hostDisconnectAllBlockInputs(b);
                hostDisconnectAllBlockOutputs(b);

                const HostInstanceMapper::BlockPair bp = _mapper.get(old.preset, b);

                if (bp.id != kMaxHostInstances)
                    _host.activate(bp.id, 0);

                if (bp.pair != kMaxHostInstances)
                    _host.activate(bp.pair, 0);
            }
        }

        // step 3: activate and connect all plugins in new preset
        uint8_t last = 0;
        for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            if (isNullURI(_current.blocks[b].uri))
                continue;

            const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, b);

            if (bp.id != kMaxHostInstances)
                _host.activate(bp.id, 1);

            if (bp.pair != kMaxHostInstances)
                _host.activate(bp.pair, 1);

            if (++_current.numLoadedPlugins == 1)
                hostConnectBlockToSystemInput(b);
            else
                hostConnectBlockToBlock(last, b);

            last = b;
        }

        if (_current.numLoadedPlugins == 0)
        {
            _host.connect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
            _host.connect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());
        }
        else
        {
            hostConnectBlockToSystemOutput(last);
        }

        // step 3: fade in
        _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOnWithFadeIn);
    }

    // audio is now processing new preset

    // scope for preloading default state on old preset
    {
        const Preset& defaults = _presets[old.preset];
        // bool defloaded[NUM_BLOCKS_PER_PRESET];

        const Host::NonBlockingScope hnbs(_host);

        for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const Block& defblockdata = defaults.blocks[b];
            const Block& oldblockdata = defaults.blocks[b];

            // using same plugin (or both empty)
            if (defblockdata.uri == old.blocks[b].uri)
            {
                if (isNullURI(defblockdata.uri))
                    continue;

                const HostInstanceMapper::BlockPair bp = _mapper.get(old.preset, b);
                if (bp.id == kMaxHostInstances)
                    continue;

                if (defblockdata.enabled != oldblockdata.enabled)
                {
                    _host.bypass(bp.id, !defblockdata.enabled);

                    if (bp.pair != kMaxHostInstances)
                        _host.bypass(bp.pair, !defblockdata.enabled);
                }

                params.clear();

                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const HostConnector::Parameter& defparameterdata(defblockdata.parameters[p]);
                    const HostConnector::Parameter& oldparameterdata(oldblockdata.parameters[p]);

                    if (isNullURI(defparameterdata.symbol))
                        break;
                    if (defparameterdata.value == oldparameterdata.value)
                        continue;

                    params.push_back({ defparameterdata.symbol.c_str(), defparameterdata.value });
                }

                _host.params_flush(bp.id, KXSTUDIO__Reset_full, params.size(), params.data());

                if (bp.pair != kMaxHostInstances)
                    _host.params_flush(bp.pair, KXSTUDIO__Reset_full, params.size(), params.data());

                continue;
            }

            // different plugin, unload old one if there is any
            if (oldloaded[b])
            {
                const HostInstanceMapper::BlockPair bp = _mapper.remove(old.preset, b);

                if (bp.id != kMaxHostInstances)
                    _host.remove(bp.id);

                if (bp.pair != kMaxHostInstances)
                    _host.remove(bp.pair);
            }

            // nothing else to do if block is empty
            if (isNullURI(defaults.blocks[b].uri))
                continue;

            // otherwise load default plugin
            HostInstanceMapper::BlockPair bp = { _mapper.add(old.preset, b), kMaxHostInstances };
            _host.preload(defblockdata.uri.c_str(), bp.id);

            if (hostPresetBlockShouldBeStereo(defaults, b))
            {
                bp.pair = _mapper.add_pair(old.preset, b);
                _host.preload(defblockdata.uri.c_str(), bp.pair);
            }

            if (!defblockdata.enabled)
            {
                _host.bypass(bp.id, true);

                if (bp.pair != kMaxHostInstances)
                    _host.bypass(bp.pair, true);
            }

            params.clear();

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                const HostConnector::Parameter& defparameterdata(defblockdata.parameters[p]);
                if (isNullURI(defparameterdata.symbol))
                    break;

                params.push_back({ defparameterdata.symbol.c_str(), defparameterdata.value });
            }

            _host.params_flush(bp.id, KXSTUDIO__Reset_full, params.size(), params.data());

            if (bp.pair != kMaxHostInstances)
                _host.params_flush(bp.pair, KXSTUDIO__Reset_full, params.size(), params.data());
        }
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::switchScene(const uint8_t scene)
{
    if (_current.scene == scene || scene > NUM_SCENES_PER_PRESET)
        return false;

    // preallocating some data
    std::vector<flushed_param> params;
    params.reserve(MAX_PARAMS_PER_BLOCK);

    _current.scene = scene;

    const Host::NonBlockingScope hnbs(_host);

    for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        HostConnector::Block& blockdata(_current.blocks[b]);
        if (isNullURI(blockdata.uri))
            continue;
        if (! blockdata.meta.hasScenes)
            continue;

        const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, b);
        if (bp.id == kMaxHostInstances)
            continue;

        params.clear();

        for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        {
            HostConnector::Parameter& paramdata(blockdata.parameters[p]);
            if (isNullURI(paramdata.symbol))
                break;
            if (paramdata.meta.flags & Lv2PortIsOutput)
                continue;
            if (! blockdata.sceneValues[_current.scene][p].used)
                continue;

            paramdata.value = blockdata.sceneValues[_current.scene][p].value;

            params.push_back({ paramdata.symbol.c_str(), paramdata.value });
        }

        _host.params_flush(bp.id, KXSTUDIO__Reset_soft, params.size(), params.data());

        if (bp.pair != kMaxHostInstances)
            _host.params_flush(bp.pair, KXSTUDIO__Reset_soft, params.size(), params.data());
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockBinding(const uint8_t hwid, const uint8_t block)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    _current.bindings[hwid].push_back({ block, ":bypass", { 0 } });
    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockParameterBinding(const uint8_t hwid, const uint8_t block, const uint8_t paramIndex)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    const HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    const HostConnector::Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return false;
    if (paramdata.meta.flags & Lv2PortIsOutput)
        return false;

    _current.bindings[hwid].push_back({ block, paramdata.symbol, { paramIndex } });
    _current.dirty = true;
    return true;
}

bool HostConnector::removeBlockBinding(const uint8_t hwid, const uint8_t block)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    std::list<HostConnector::Binding>& bindings(_current.bindings[hwid]);
    for (BindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
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

bool HostConnector::removeBlockParameterBinding(const uint8_t hwid, const uint8_t block, const uint8_t paramIndex)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    const HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    const HostConnector::Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return false;
    if (paramdata.meta.flags & Lv2PortIsOutput)
        return false;

    std::list<HostConnector::Binding>& bindings(_current.bindings[hwid]);
    for (BindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
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

bool HostConnector::reorderBlockBinding(const uint8_t hwid, const uint8_t dest)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(dest < NUM_BINDING_ACTUATORS);

    if (hwid == dest)
    {
        fprintf(stderr, "HostConnector::reorderBlockBinding(%u, %u) - hwid == dest, rejected\n", hwid, dest);
        return false;
    }

    // moving hwid backwards to the left
    // a b c d e! f
    // a b c e! d f
    // a b e! c d f
    // a e! b c d f
    if (hwid > dest)
    {
        for (int i = hwid; i > dest; --i)
            std::swap(_current.bindings[i], _current.bindings[i - 1]);
    }

    // moving hwid forward to the right
    // a b! c d e f
    // a c b! d e f
    // a c d b! e f
    // a c d e b! f
    else
    {
        for (int i = hwid; i < dest; ++i)
            std::swap(_current.bindings[i], _current.bindings[i + 1]);
    }

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

float HostConnector::dspLoad()
{
    return _host.cpu_load();
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

void HostConnector::setBlockParameter(const uint8_t block, const uint8_t paramIndex, const float value)
{
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return;

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return;

    HostConnector::Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return;
    if (paramdata.meta.flags & Lv2PortIsOutput)
        return;

    _current.dirty = true;

    if (_current.scene != 0 && ! blockdata.sceneValues[_current.scene][paramIndex].used)
    {
        blockdata.meta.hasScenes = true;
        blockdata.sceneValues[_current.scene][paramIndex].used = true;

        // if this is the first time for this scene parameter, set original value for default scene
        if (! blockdata.sceneValues[0][paramIndex].used)
        {
            blockdata.sceneValues[0][paramIndex].used = true;
            blockdata.sceneValues[0][paramIndex].value = paramdata.value;
        }
    }

    paramdata.value = value;
    blockdata.sceneValues[_current.scene][paramIndex].value = value;

    _host.param_set(bp.id, paramdata.symbol.c_str(), value);

    if (bp.pair != kMaxHostInstances)
        _host.param_set(bp.pair, paramdata.symbol.c_str(), value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::monitorBlockOutputParameter(const uint8_t block, const uint8_t paramIndex)
{
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    const HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return;

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return;

    const HostConnector::Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return;

    if (paramdata.meta.flags & Lv2PortIsOutput)
        _host.monitor_output(bp.id, paramdata.symbol.c_str());
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::enableTool(const uint8_t toolIndex, const char* const uri)
{
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);

    return isNullURI(uri) ? _host.remove(MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex)
                          : _host.add(uri, MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::connectToolAudioInput(const uint8_t toolIndex,
                                          const char* const symbol,
                                          const char* const jackPort)
{
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');
    assert(jackPort != nullptr && *jackPort != '\0');

    _host.connect(jackPort, format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol).c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::connectToolAudioOutput(const uint8_t toolIndex,
                                           const char* const symbol,
                                           const char* const jackPort)
{
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');
    assert(jackPort != nullptr && *jackPort != '\0');

    _host.connect(format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol).c_str(), jackPort);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::connectTool2Tool(uint8_t toolAIndex, 
                      const char* toolAOutSymbol, 
                      uint8_t toolBIndex, 
                      const char* toolBInSymbol)
{
    assert(toolAIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(toolBIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(toolAOutSymbol != nullptr && *toolAOutSymbol != '\0');
    assert(toolBInSymbol != nullptr && *toolBInSymbol != '\0');

    _host.connect(format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolAIndex, toolAOutSymbol).c_str(), 
                  format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolBIndex, toolBInSymbol).c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::toolOutAsCapturePort(uint8_t toolIndex, 
                          const char* symbol, 
                          uint8_t capturePortIndex)
{
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');
    assert(capturePortIndex < kNCapturePorts);
    
    switch (capturePortIndex) {
        case 0:
            jackCapturePort1 = format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol);
            break;
        case 1:
            jackCapturePort2 = format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol);
            break;
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::toolInAsPlaybackPort(uint8_t toolIndex, 
                          const char* symbol, 
                          uint8_t playbackPortIndex)
{
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');
    assert(playbackPortIndex < kNPlaybackPorts);

    switch (playbackPortIndex) {
        case 0:
            jackPlaybackPort1 = format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol);
            break;
        case 1:
            jackPlaybackPort2 = format("effect_%d:%s", MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol);
            break;
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setToolParameter(const uint8_t toolIndex, const char* const symbol, const float value)
{
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');

    _host.param_set(MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol, value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::monitorToolOutputParameter(const uint8_t toolIndex, const char* const symbol)
{
    assert(toolIndex < MAX_MOD_HOST_TOOL_INSTANCES);
    assert(symbol != nullptr && *symbol != '\0');

    _host.monitor_output(MAX_MOD_HOST_PLUGIN_INSTANCES + toolIndex, symbol);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setBlockProperty(const uint8_t block, const char* const uri, const char* const value)
{
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(uri != nullptr && *uri != '\0');
    assert(value != nullptr);

    const HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return;

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return;

    _current.dirty = true;

    _host.patch_set(bp.id, uri, value);

    if (bp.pair != kMaxHostInstances)
        _host.patch_set(bp.pair, uri, value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectAll(uint8_t blockStart, uint8_t blockEnd)
{
    assert(blockStart <= blockEnd);
    assert(blockEnd < NUM_BLOCKS_PER_PRESET);

    if (_current.numLoadedPlugins == 0)
    {
        // direct connections
        _host.connect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
        _host.connect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());
        return;
    }

    // direct connections
    // _host.disconnect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
    // _host.disconnect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());

    bool loaded[NUM_BLOCKS_PER_PRESET];
    for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        loaded[b] = !isNullURI(_current.blocks[b].uri);

    // first plugin
    for (uint8_t b = 0; b <= blockEnd; ++b)
    {
        if (loaded[b])
        {
            if (b >= blockStart && b <= blockEnd)
                hostConnectBlockToSystemInput(b);
            break;
        }
    }

    // last plugin
    for (uint8_t b = NUM_BLOCKS_PER_PRESET - 1; b >= blockStart && b != UINT8_MAX; --b)
    {
        if (loaded[b])
        {
            if (b >= blockStart && b <= blockEnd)
                hostConnectBlockToSystemOutput(b);
            break;
        }
    }

    // find connecting blocks
    if (blockStart != 0)
    {
        for (uint8_t b = blockStart - 1; b != UINT8_MAX; --b)
        {
            if (loaded[b])
            {
                blockStart = b;
                break;
            }
        }
    }

    if (blockEnd != NUM_BLOCKS_PER_PRESET - 1)
    {
        for (uint8_t b = blockEnd + 1; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            if (loaded[b])
            {
                blockEnd = b;
                break;
            }
        }
    }

    // now we can connect between plugins
    for (uint8_t b1 = blockStart; b1 < blockEnd; ++b1)
    {
        if (! loaded[b1])
            continue;

        for (uint8_t b2 = b1 + 1; b2 <= blockEnd; ++b2)
        {
            if (! loaded[b2])
                continue;

            hostConnectBlockToBlock(b1, b2);
            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBlockToBlock(const uint8_t blockA, const uint8_t blockB)
{
    assert(blockA < NUM_BLOCKS_PER_PRESET);
    assert(blockB < NUM_BLOCKS_PER_PRESET);

    const Lv2Plugin* const pluginA = lv2world.get_plugin_by_uri(_current.blocks[blockA].uri.c_str());
    const Lv2Plugin* const pluginB = lv2world.get_plugin_by_uri(_current.blocks[blockB].uri.c_str());

    if (pluginA == nullptr || pluginB == nullptr)
        return;

    const HostInstanceMapper::BlockPair bpA = _mapper.get(_current.preset, blockA);
    const HostInstanceMapper::BlockPair bpB = _mapper.get(_current.preset, blockB);

    if (bpA.id == kMaxHostInstances || bpB.id == kMaxHostInstances)
        return;

    std::string origin, target;

    size_t aConnectedPort = pluginA->ports.size();

    for (size_t a = 0, astart = 0, b = 0; b < pluginB->ports.size(); ++b)
    {
        if ((pluginB->ports[b].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
            continue;

        bool bIsConnected = false;

        for (a = astart; a < pluginA->ports.size(); ++a)
        {
            if ((pluginA->ports[a].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                continue;

            origin = format("effect_%d:%s", bpA.id, pluginA->ports[a].symbol.c_str());
            target = format("effect_%d:%s", bpB.id, pluginB->ports[b].symbol.c_str());
            _host.connect(origin.c_str(), target.c_str());
            aConnectedPort = a;
            bIsConnected = true;

            if (bpA.pair != kMaxHostInstances && bpB.pair != kMaxHostInstances)
            {
                origin = format("effect_%d:%s", bpA.pair, pluginA->ports[a].symbol.c_str());
                target = format("effect_%d:%s", bpB.pair, pluginB->ports[b].symbol.c_str());
                _host.connect(origin.c_str(), target.c_str());
                return;
            }

            if (bpA.pair != kMaxHostInstances)
            {
                for (size_t b2 = b + 1; b2 < pluginB->ports.size(); ++b2)
                {
                    if ((pluginB->ports[b2].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                        continue;

                    origin = format("effect_%d:%s", bpA.pair, pluginA->ports[a].symbol.c_str());
                    target = format("effect_%d:%s", bpB.id, pluginB->ports[b2].symbol.c_str());
                    _host.connect(origin.c_str(), target.c_str());
                }
                return;
            }

            if (bpB.pair != kMaxHostInstances)
            {
                for (size_t a2 = a + 1; a2 < pluginA->ports.size(); ++a2)
                {
                    if ((pluginA->ports[a2].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                        continue;

                    origin = format("effect_%d:%s", bpA.id, pluginA->ports[a2].symbol.c_str());
                    target = format("effect_%d:%s", bpB.pair, pluginB->ports[b].symbol.c_str());
                    _host.connect(origin.c_str(), target.c_str());
                    return;
                }
            }

            // try finding another a input port for next b output port
            astart = a + 1;
            break;
        }

        if (!bIsConnected && (aConnectedPort != pluginA->ports.size())) {
            // didn't find a new a port to connect from -> connect latest found a port to b port
            origin = format("effect_%d:%s", bpA.id, pluginA->ports[aConnectedPort].symbol.c_str());
            target = format("effect_%d:%s", bpB.id, pluginB->ports[b].symbol.c_str());
            _host.connect(origin.c_str(), target.c_str());
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBlockToSystemInput(const uint8_t block)
{
    hostConnectSystemInputAction(block, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBlockToSystemOutput(const uint8_t block)
{
    hostConnectSystemOutputAction(block, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAll()
{
    for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        const HostConnector::Block& blockdata(_current.blocks[b]);
        if (isNullURI(blockdata.uri))
            continue;

        hostDisconnectAllBlockInputs(b);
        hostDisconnectAllBlockOutputs(b);
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockInputs(const uint8_t block)
{
    hostDisconnectBlockAction(block, false);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockOutputs(const uint8_t block)
{
    hostDisconnectBlockAction(block, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostClearAndLoadCurrentBank()
{
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
    uint8_t last = 0;

    for (uint8_t pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
    {
        const bool active = _current.preset == pr;
        bool previousPluginStereoOut = false;

        for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const HostConnector::Block& blockdata(active ? _current.blocks[b] : _presets[pr].blocks[b]);
            if (isNullURI(blockdata.uri))
                continue;

            const auto loadInstance = [=](const uint16_t instance)
            {
                if (active ? _host.add(blockdata.uri.c_str(), instance)
                           : _host.preload(blockdata.uri.c_str(), instance))
                {
                    if (!blockdata.enabled)
                        _host.bypass(instance, true);

                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        const HostConnector::Parameter& parameterdata(blockdata.parameters[p]);
                        if (isNullURI(parameterdata.symbol))
                            break;
                        _host.param_set(instance, parameterdata.symbol.c_str(), parameterdata.value);
                    }

                    return true;
                }

                return false;
            };

            const bool dualmono = previousPluginStereoOut && blockdata.meta.isMonoIn;
            const uint16_t instance = _mapper.add(pr, b);

            bool added = loadInstance(instance);

            if (added)
            {
                if (dualmono)
                {
                    const uint16_t pair = _mapper.add_pair(pr, b);

                    if (! loadInstance(pair))
                    {
                        added = false;
                        _host.remove(instance);
                    }
                }
            }

            if (! added)
            {
                if (active)
                    resetBlock(_current.blocks[b]);

                _mapper.remove(pr, b);
                continue;
            }

            previousPluginStereoOut = blockdata.meta.isStereoOut || dualmono;

            if (active)
            {
                if (++_current.numLoadedPlugins == 1)
                    hostConnectBlockToSystemInput(b);
                else
                    hostConnectBlockToBlock(last, b);

                last = b;
            }
        }
    }

    if (_current.numLoadedPlugins == 0)
    {
        _host.connect(jackCapturePort1.c_str(), jackPlaybackPort1.c_str());
        _host.connect(jackCapturePort2.c_str(), jackPlaybackPort2.c_str());
    }
    else
    {
        hostConnectBlockToSystemOutput(last);
    }

    _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOnWithFadeIn);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectSystemInputAction(const uint8_t block, const bool connect)
{
    assert(block < NUM_BLOCKS_PER_PRESET);

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.blocks[block].uri.c_str());
    if (plugin == nullptr)
        return;

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return;

    bool (Host::*call)(const char*, const char*) = connect ? &Host::connect : &Host::disconnect;
    const char* origin;
    std::string target;

    for (size_t i = 0, j = 0; i < plugin->ports.size() && j < 2; ++i)
    {
        if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
            continue;

        origin = j++ == 0 ? jackCapturePort1.c_str() : jackCapturePort2.c_str();
        target = format("effect_%d:%s", bp.id, plugin->ports[i].symbol.c_str());
        (_host.*call)(origin, target.c_str());

        if (bp.pair != kMaxHostInstances)
        {
            target = format("effect_%d:%s", bp.pair, plugin->ports[i].symbol.c_str());
            (_host.*call)(jackCapturePort2.c_str(), target.c_str());
            return;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectSystemOutputAction(const uint8_t block, const bool connect)
{
    assert(block < NUM_BLOCKS_PER_PRESET);

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.blocks[block].uri.c_str());
    if (plugin == nullptr)
        return;

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return;

    bool (Host::*call)(const char*, const char*) = connect ? &Host::connect : &Host::disconnect;
    std::string origin;
    const char* target;
    int dsti = 0;

    for (size_t i = 0; i < plugin->ports.size() && dsti < 2; ++i)
    {
        if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
            continue;

        origin = format("effect_%d:%s", bp.id, plugin->ports[i].symbol.c_str());
        target = dsti++ == 0 ? jackPlaybackPort1.c_str() : jackPlaybackPort2.c_str();
        (_host.*call)(origin.c_str(), target);

        if (bp.pair != kMaxHostInstances)
        {
            origin = format("effect_%d:%s", bp.pair, plugin->ports[i].symbol.c_str());
            (_host.*call)(origin.c_str(), jackPlaybackPort2.c_str());
            return;
        }
    }

    if (dsti == 1)
        (_host.*call)(origin.c_str(), jackPlaybackPort2.c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectBlockAction(const uint8_t block, const bool outputs)
{
    assert(block < NUM_BLOCKS_PER_PRESET);

    if (isNullURI(_current.blocks[block].uri))
        return;

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.blocks[block].uri.c_str());
    if (plugin == nullptr)
        return;

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return;

    const unsigned int ioflags = Lv2PortIsAudio | (outputs ? Lv2PortIsOutput : 0);
    std::string origin;

    for (size_t i = 0; i < plugin->ports.size(); ++i)
    {
        if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != ioflags)
            continue;

        origin = format("effect_%d:%s", bp.id, plugin->ports[i].symbol.c_str());
        _host.disconnect_all(origin.c_str());

        if (bp.pair != kMaxHostInstances)
        {
            origin = format("effect_%d:%s", bp.pair, plugin->ports[i].symbol.c_str());
            _host.disconnect_all(origin.c_str());
            return;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostEnsureStereoChain(const uint8_t blockStart, const uint8_t blockEnd)
{
    assert(blockStart <= blockEnd);
    assert(blockEnd < NUM_BLOCKS_PER_PRESET);

    bool previousPluginStereoOut = hostPresetBlockShouldBeStereo(_current, blockStart);

    for (uint8_t b = blockStart; b <= blockEnd; ++b)
    {
        const HostConnector::Block& blockdata(_current.blocks[b]);
        if (isNullURI(blockdata.uri))
            continue;

        const bool oldDualmono = _mapper.get(_current.preset, b).pair != kMaxHostInstances;
        const bool newDualmono = previousPluginStereoOut && blockdata.meta.isMonoIn;

        if (oldDualmono != newDualmono)
        {
            if (newDualmono)
            {
                const uint16_t pair = _mapper.add_pair(_current.preset, b);

                if (_host.add(blockdata.uri.c_str(), pair))
                {
                    if (!blockdata.enabled)
                        _host.bypass(pair, true);

                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        const HostConnector::Parameter& parameterdata(blockdata.parameters[p]);
                        if (isNullURI(parameterdata.symbol))
                            break;
                        _host.param_set(pair, parameterdata.symbol.c_str(), parameterdata.value);
                    }

                    // disconnect ports, we might have mono to stereo connections
                    hostDisconnectAllBlockOutputs(b);
                }
            }
            else
            {
                _host.remove(_mapper.remove_pair(_current.preset, b));
            }
        }

        previousPluginStereoOut = blockdata.meta.isStereoOut || newDualmono;
    }
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::hostPresetBlockShouldBeStereo(const Preset& presetdata, const uint8_t block)
{
    if (block == 0)
        return false;

    for (uint8_t b = block - 1; b != UINT8_MAX; --b)
    {
        if (isNullURI(presetdata.blocks[b].uri))
            continue;
        if (presetdata.blocks[b].meta.isStereoOut)
            return true;
    }

    return false;
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostRemoveAllBlockBindings(const uint8_t block)
{
    assert(block < NUM_BLOCKS_PER_PRESET);

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        std::list<HostConnector::Binding>& bindings(_current.bindings[hwid]);

    restart:
        for (BindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
        {
            if (it->block != block)
                continue;

            bindings.erase(it);
            _current.dirty = true;
            goto restart;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostRemoveInstanceForBlock(const uint8_t block)
{
    assert(block < NUM_BLOCKS_PER_PRESET);

    const HostInstanceMapper::BlockPair bp = _mapper.remove(_current.preset, block);

    if (bp.id != kMaxHostInstances)
        _host.remove(bp.id);

    if (bp.pair != kMaxHostInstances)
        _host.remove(bp.pair);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostFeedbackCallback(const HostFeedbackData& data)
{
    if (_callback == nullptr)
        return;

    uint8_t block;
    HostCallbackData cdata = {};

    switch (data.type)
    {
    case HostFeedbackData::kFeedbackAudioMonitor:
        cdata.type = HostCallbackData::kAudioMonitor;
        cdata.audioMonitor.index = data.audioMonitor.index;
        cdata.audioMonitor.value = data.audioMonitor.value;
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
            if ((block = _mapper.get_block_with_id(_current.preset, data.paramSet.effect_id)) == NUM_BLOCKS_PER_PRESET)
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
                Block& blockdata = _current.blocks[block];

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
                cdata.parameterSet.block = block;
                cdata.parameterSet.index = p;
                cdata.parameterSet.symbol = data.paramSet.symbol;
                cdata.parameterSet.value = data.paramSet.value;
            }
        }
        break;

    case HostFeedbackData::kFeedbackPatchSet:
        assert(data.patchSet.effect_id >= 0);
        assert(data.patchSet.effect_id < MAX_MOD_HOST_INSTANCES);
        static_assert(sizeof(cdata.toolPatchSet.data) == sizeof(data.patchSet.data), "data size mismatch");

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
            if ((block = _mapper.get_block_with_id(_current.preset, data.patchSet.effect_id)) == NUM_BLOCKS_PER_PRESET)
                return;

            cdata.type = HostCallbackData::kPatchSet;
            cdata.patchSet.block = block;
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

    _host.monitor_audio_levels(jackCapturePort1.c_str(), true);
    _host.monitor_audio_levels(jackCapturePort2.c_str(), true);
    _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_1, true);
    _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_2, true);
}

// --------------------------------------------------------------------------------------------------------------------
