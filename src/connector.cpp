// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"
#include "json.hpp"
#include "utils.hpp"

#include <cstring>
#include <fstream>
#include <map>

#define JSON_BANK_VERSION_CURRENT 0
#define JSON_BANK_VERSION_MIN_SUPPORTED 0
#define JSON_BANK_VERSION_MAX_SUPPORTED 0

#ifdef BINDING_ACTUATOR_IDS
static constexpr const char* kBindingActuatorIDs[NUM_BINDING_ACTUATORS] = { BINDING_ACTUATOR_IDS };
#endif

// --------------------------------------------------------------------------------------------------------------------

static void resetParam(HostConnector::Parameter& paramdata)
{
    paramdata = {};
    paramdata.meta.max = 1.f;
}

static void resetBlock(HostConnector::Block& blockdata)
{
    blockdata = {};
    blockdata.meta.quickPotIndex = -1;
    blockdata.parameters.resize(MAX_PARAMS_PER_BLOCK);

    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetParam(blockdata.parameters[p]);

    for (uint8_t s = 0; s <= NUM_SCENES_PER_PRESET; ++s)
    {
        blockdata.sceneValues[s].resize(MAX_PARAMS_PER_BLOCK);

        for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            blockdata.sceneValues[s][p].used = false;
    }
}

static void resetPreset(HostConnector::Preset& preset)
{
    preset = {};
    preset.blocks.resize(NUM_BLOCKS_PER_PRESET);

    for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        resetBlock(preset.blocks[bl]);
}

// --------------------------------------------------------------------------------------------------------------------

HostConnector::HostConnector()
{
    for (uint8_t p = 0; p < NUM_PRESETS_PER_BANK; ++p)
        resetPreset(_presets[p]);

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

                uint8_t numInputs = 0;
                uint8_t numOutputs = 0;
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

                if (numInputs > 2 || numOutputs > 2)
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
                    if (plugin->ports[i].flags & (Lv2PortIsOutput|Lv2ParameterHidden))
                        continue;

                    switch (plugin->ports[i].designation)
                    {
                    case kLv2DesignationNone:
                        break;
                    case kLv2DesignationEnabled:
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
    _host.connect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
    _host.connect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);

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

bool HostConnector::reorderBlock(const uint8_t block, const uint8_t dest)
{
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(dest < NUM_BLOCKS_PER_PRESET);

    if (block == dest)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - block == dest, rejected\n", block, dest);
        return false;
    }
    if (isNullURI(_current.blocks[block].uri))
    {
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - block is empty, rejected\n", block, dest);
        return false;
    }

    // check if we need to re-do any connections
    bool reconnect = false;
    const uint8_t blockStart = std::max(0, std::min<int>(block, dest) - 1);
    const uint8_t blockEnd = std::min(NUM_BLOCKS_PER_PRESET - 1, std::max(block, dest) + 1);

    for (uint8_t i = blockStart; i <= blockEnd; ++i)
    {
        if (block == i)
            continue;
        if (isNullURI(_current.blocks[i].uri))
            continue;
        reconnect = true;
        break;
    }

    printf("HostConnector::reorderBlock(%u, %u) - reconnect %d, start %u, end %u\n",
           block, dest, reconnect, blockStart, blockEnd);

    auto& mpreset = _mapper.map.presets[_current.preset];

    const Host::NonBlockingScope hnbs(_host);

    if (reconnect)
    {
        hostDisconnectAllBlockInputs(block);
        hostDisconnectAllBlockOutputs(block);
    }

    // moving block backwards to the left
    // a b c d e! f
    // a b c e! d f
    // a b e! c d f
    // a e! b c d f
    if (block > dest)
    {
        for (int i = block; i > dest; --i)
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
            hostConnectAll(dest, block);
        }
    }

    // moving block forward to the right
    // a b! c d e f
    // a c b! d e f
    // a c d b! e f
    // a c d e b! f
    else
    {
        for (int i = block; i < dest; ++i)
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
            hostConnectAll(block, dest);
        }
    }

    // update bindings
    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        for (size_t bi = 0; bi < _current.bindings[hwid].size(); ++bi)
        {
            HostConnector::Binding& bindingdata = _current.bindings[hwid][bi];

            if (bindingdata.block < blockStart || bindingdata.block > blockEnd)
                continue;

            if (bindingdata.block == block)
                bindingdata.block = dest;
            else if (block > dest)
                ++bindingdata.block;
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
        uint8_t numInputs = 0;
        uint8_t numOutputs = 0;
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

        if (numInputs > 2 || numOutputs > 2)
        {
            fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - unsupported IO, rejected\n", block, uri);
            return false;
        }

        if (! isNullURI(blockdata.uri))
        {
            --_current.numLoadedPlugins;
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

            uint8_t numParams = 0;
            for (size_t i = 0; i < plugin->ports.size() && numParams < MAX_PARAMS_PER_BLOCK; ++i)
            {
                if ((plugin->ports[i].flags & Lv2PortIsControl) == 0)
                    continue;
                if (plugin->ports[i].flags & (Lv2PortIsOutput|Lv2ParameterHidden))
                    continue;

                switch (plugin->ports[i].designation)
                {
                case kLv2DesignationNone:
                    break;
                case kLv2DesignationEnabled:
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
            _host.disconnect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
            _host.disconnect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
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

            hostEnsureStereoChain(before, NUM_BLOCKS_PER_PRESET - 1);
            hostConnectAll(before, NUM_BLOCKS_PER_PRESET - 1);
        }
    }
    else
    {
        // use direct connections if there are no plugins
        if (_current.numLoadedPlugins == 0)
        {
            _host.connect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
            _host.connect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
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

            _host.disconnect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
            _host.disconnect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
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
            _host.connect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
            _host.connect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
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

                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const HostConnector::Parameter& defparameterdata(defblockdata.parameters[p]);
                    const HostConnector::Parameter& oldparameterdata(oldblockdata.parameters[p]);

                    if (isNullURI(defparameterdata.symbol))
                        break;
                    if (defparameterdata.value == oldparameterdata.value)
                        continue;

                    _host.param_set(bp.id, defparameterdata.symbol.c_str(), defparameterdata.value);

                    if (bp.pair != kMaxHostInstances)
                        _host.param_set(bp.pair, defparameterdata.symbol.c_str(), defparameterdata.value);
                }

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

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                const HostConnector::Parameter& defparameterdata(defblockdata.parameters[p]);
                if (isNullURI(defparameterdata.symbol))
                    break;

                _host.param_set(bp.id, defparameterdata.symbol.c_str(), defparameterdata.value);

                if (bp.pair != kMaxHostInstances)
                    _host.param_set(bp.pair, defparameterdata.symbol.c_str(), defparameterdata.value);
            }
        }
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::switchScene(const uint8_t scene)
{
    if (_current.scene == scene || scene > NUM_SCENES_PER_PRESET)
        return false;

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

        for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        {
            HostConnector::Parameter& paramdata(blockdata.parameters[p]);
            if (isNullURI(paramdata.symbol))
                break;
            if (! blockdata.sceneValues[_current.scene][p].used)
                continue;

            paramdata.value = blockdata.sceneValues[_current.scene][p].value;

            _host.param_set(bp.id, paramdata.symbol.c_str(), paramdata.value);

            if (bp.pair != kMaxHostInstances)
                _host.param_set(bp.pair, paramdata.symbol.c_str(), paramdata.value);
        }
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockBinding(const uint8_t hwid, const uint8_t block)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    _current.bindings[hwid].push_back({ block, ":bypass", { 0 } });
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockParameterBinding(const uint8_t hwid, const uint8_t block, const uint8_t paramIndex)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    HostConnector::Block& blockdata(_current.blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    HostConnector::Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return false;

    _current.bindings[hwid].push_back({ block, paramdata.symbol, { paramIndex } });
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
        _host.connect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
        _host.connect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
        return;
    }

    // direct connections
    // _host.disconnect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
    // _host.disconnect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);

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

    for (size_t a = 0, astart = 0, b = 0; b < pluginB->ports.size(); ++b)
    {
        if ((pluginB->ports[b].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
            continue;

        for (a = astart; a < pluginA->ports.size(); ++a)
        {
            if ((pluginA->ports[a].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                continue;

            origin = format("effect_%d:%s", bpA.id, pluginA->ports[a].symbol.c_str());
            target = format("effect_%d:%s", bpB.id, pluginB->ports[b].symbol.c_str());
            _host.connect(origin.c_str(), target.c_str());

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

            ++a;
            ++astart;
            break;
        }

        if (astart == 1)
            astart = 0;
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
        _host.connect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
        _host.connect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
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

        origin = j++ == 0 ? JACK_CAPTURE_PORT_1 : JACK_CAPTURE_PORT_2;
        target = format("effect_%d:%s", bp.id, plugin->ports[i].symbol.c_str());
        (_host.*call)(origin, target.c_str());

        if (bp.pair != kMaxHostInstances)
        {
            target = format("effect_%d:%s", bp.pair, plugin->ports[i].symbol.c_str());
            (_host.*call)(JACK_CAPTURE_PORT_2, target.c_str());
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
        target = dsti++ == 0 ? JACK_PLAYBACK_PORT_1 : JACK_PLAYBACK_PORT_2;
        (_host.*call)(origin.c_str(), target);

        if (bp.pair != kMaxHostInstances)
        {
            origin = format("effect_%d:%s", bp.pair, plugin->ports[i].symbol.c_str());
            (_host.*call)(origin.c_str(), JACK_PLAYBACK_PORT_2);
            return;
        }
    }

    if (dsti == 1)
        (_host.*call)(origin.c_str(), JACK_PLAYBACK_PORT_2);
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

            _current.dirty = true;
            blockdata.parameters[p].value = data.paramSet.value;

            cdata.type = HostCallbackData::kPatchSet;
            cdata.parameterSet.block = block;
            cdata.parameterSet.index = p;
            cdata.parameterSet.symbol = data.paramSet.symbol;
            cdata.parameterSet.value = data.paramSet.value;
        }

        break;

    case HostFeedbackData::kFeedbackPatchSet:
        // if ((block = _mapper.get_block_with_id(_current.preset, data.patchSet.effect_id)) == NUM_BLOCKS_PER_PRESET)
        //    return;

        cdata.type = HostCallbackData::kPatchSet;
        cdata.patchSet.block = 0; // TESTING
        cdata.patchSet.key = data.patchSet.key;
        cdata.patchSet.type = data.patchSet.type;
        std::memcpy(&cdata.patchSet.data, &data.patchSet.data, sizeof(data.patchSet.data));
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
    _host.monitor_audio_levels(JACK_CAPTURE_PORT_2, true);
    _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_1, true);
    _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_2, true);
}

// --------------------------------------------------------------------------------------------------------------------
