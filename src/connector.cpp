// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
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

using BindingIterator = std::list<HostConnector::Binding>::iterator;
using BindingIteratorConst = std::list<HostConnector::Binding>::const_iterator;

// --------------------------------------------------------------------------------------------------------------------

static void resetParam(HostConnector::Parameter& paramdata)
{
    paramdata = {};
    paramdata.meta.max = 1.f;
}

static void resetBlock(HostConnector::Block& blockdata)
{
    blockdata.enabled = false;
   #if NUM_BLOCK_CHAIN_ROWS != 1
    blockdata.linkedRow = UINT8_MAX;
    blockdata.linkedBlock = UINT8_MAX;
   #endif
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

static void allocBlock(HostConnector::Block& blockdata)
{
    blockdata.parameters.resize(MAX_PARAMS_PER_BLOCK);

    for (uint8_t s = 0; s <= NUM_SCENES_PER_PRESET; ++s)
        blockdata.sceneValues[s].resize(MAX_PARAMS_PER_BLOCK);
}

// --------------------------------------------------------------------------------------------------------------------

static bool getSupportedPluginIO(const Lv2Plugin* const plugin, uint8_t& numInputs, uint8_t& numOutputs)
{
    numInputs = numOutputs = 0;
    for (const Lv2Port& port : plugin->ports)
    {
        if ((port.flags & Lv2PortIsAudio) == 0)
            continue;

        if ((port.flags & Lv2PortIsOutput) != 0)
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

static bool isNullBlock(const HostConnector::Block& blockdata)
{
    return isNullURI(blockdata.uri);
}

// --------------------------------------------------------------------------------------------------------------------

static bool shouldBlockBeStereo(const HostConnector::ChainRow& chaindata, const uint8_t block)
{
    assert(block < NUM_BLOCKS_PER_PRESET);

    if (chaindata.inputs[0] != chaindata.inputs[1])
        return true;

    for (uint8_t bl = block - 1; bl != UINT8_MAX; --bl)
    {
        if (isNullBlock(chaindata.blocks[bl]))
            continue;
        if (chaindata.blocks[bl].meta.isStereoOut)
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

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS && (withBlocks || withParams); ++row)
    {
       #if NUM_BLOCK_CHAIN_ROWS != 1
        fprintf(stderr, "\n\t--- Row %u\n", row);
       #endif

        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET && (withBlocks || withParams); ++bl)
        {
            const Block& blockdata(_current.chains[row].blocks[bl]);

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

        for (Binding& bindingdata : _current.bindings[hwid])
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
            for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
            {
                for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                {
                    Block& blockdata = preset.chains[row].blocks[bl];

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
                            continue;

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
                   #if NUM_BLOCK_CHAIN_ROWS != 1
                    blockdata.linkedRow = UINT8_MAX;
                    blockdata.linkedBlock = UINT8_MAX;
                   #endif
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
                        if ((plugin->ports[i].flags & Lv2ParameterHidden) != 0)
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

                        Parameter& paramdata = blockdata.parameters[numParams];

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
                        if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
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
                            if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
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
                    int row = 1;
                    if (jbinding.contains("row"))
                    {
                        row = jbinding["row"].get<int>();
                        if (row < 1 || row > NUM_BLOCK_CHAIN_ROWS)
                        {
                           #if NUM_BLOCK_CHAIN_ROWS != 1
                            printf("HostConnector::loadBankFromFile: binding has out of bounds block %d\n", block);
                           #endif
                            continue;
                        }
                    }
                    const Block& blockdata = preset.chains[row - 1].blocks[block - 1];

                    const std::string symbol = jbinding["symbol"].get<std::string>();

                    if (symbol == ":bypass")
                    {
                        preset.bindings[hwid].push_back({
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
                            const Parameter& paramdata = blockdata.parameters[p];

                            if (isNullURI(paramdata.symbol))
                                break;
                            if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                                continue;

                            if (paramdata.symbol == symbol)
                            {
                                preset.bindings[hwid].push_back({
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
    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            Block& blockdata = _presets[_current.preset].chains[row].blocks[bl];
            if (isNullURI(blockdata.uri))
                continue;

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                Parameter& paramdata = blockdata.parameters[p];
                if (isNullURI(paramdata.symbol))
                    break;
                if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                    continue;

                if (blockdata.sceneValues[0][p].used)
                    paramdata.value = blockdata.sceneValues[0][p].value;
            }
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
                    const Block& blockdata = preset.chains[row].blocks[bl];

                    if (isNullURI(blockdata.uri))
                        continue;

                   #if NUM_BLOCK_CHAIN_ROWS == 1
                    const std::string jblockid = std::to_string(bl + 1);
                   #else
                    const std::string jblockid = format("%u:%u", row + 1, bl + 1);
                   #endif

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
                        if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
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

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            if (!isNullURI(_current.chains[row].blocks[bl].uri))
                hostRemoveInstanceForBlock(row, bl);

            resetBlock(_current.chains[row].blocks[bl]);
        }
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

bool HostConnector::enableBlock(const uint8_t row, const uint8_t block, const bool enable)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
    {
        fprintf(stderr, "HostConnector::enableBlock(%u, %s) - block not in use, rejected\n", block, bool2str(enable));
        return false;
    }

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    if (hbp.id == kMaxHostInstances)
        return false;

    blockdata.enabled = enable;
    _current.dirty = true;
    _host.bypass(hbp.id, !enable);

    if (hbp.pair != kMaxHostInstances)
        _host.bypass(hbp.pair, !enable);

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::reorderBlock(const uint8_t row, const uint8_t orig, const uint8_t dest)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(orig < NUM_BLOCKS_PER_PRESET);
    assert(dest < NUM_BLOCKS_PER_PRESET);

    if (orig == dest)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - orig == dest, rejected\n", orig, dest);
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
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - there is nothing to reorder, rejected\n", orig, dest);
        return false;
    }

    printf("HostConnector::reorderBlock(%u, %u) - reconnect %s, blockIsEmpty %s, start %u, end %u\n",
           orig, dest, bool2str(reconnect), bool2str(blockIsEmpty), blockStart, blockEnd);

    const Host::NonBlockingScope hnbs(_host);

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
            if (reconnect)
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
            if (reconnect)
            {
                hostDisconnectAllBlockInputs(row, i + 1);
                hostDisconnectAllBlockOutputs(row, i + 1);
            }
            std::swap(chain.blocks[i], chain.blocks[i + 1]);
        }
    }

    _mapper.reorder(_current.preset, row, orig, dest);

    if (reconnect)
    {
        hostEnsureStereoChain(row, blockStart, blockEnd);
        hostConnectAll(row, left, right);
    }

    // update bindings
    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        for (Binding& bindingdata : _current.bindings[hwid])
        {
            if (bindingdata.row != row)
                continue;
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

bool HostConnector::replaceBlock(const uint8_t row, const uint8_t block, const char* const uri)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    Block& blockdata(_current.chains[row].blocks[block]);

    const Host::NonBlockingScope hnbs(_host);

    if (!isNullURI(uri))
    {
        const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(uri);
        assert_return(plugin != nullptr, false);

        // we only do changes after verifying that the requested plugin exists and is valid
        uint8_t numInputs, numOutputs;
        if (! getSupportedPluginIO(plugin, numInputs, numOutputs))
        {
            fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - unsupported IO, rejected\n", block, uri);
            return false;
        }

        if (!isNullURI(blockdata.uri))
        {
            --_current.numLoadedPlugins;
            hostRemoveAllBlockBindings(row, block);
            hostRemoveInstanceForBlock(row, block);
        }

        // activate dual mono if previous plugin is stereo or also dualmono
        bool dualmono = numInputs == 1 && shouldBlockBeStereo(_current.chains[row], block);

        const uint16_t instance = _mapper.add(_current.preset, row, block);

        bool added = _host.add(uri, instance);
        if (added)
        {
            printf("DEBUG: block %u loaded plugin %s\n", block, uri);

            if (dualmono)
            {
                const uint16_t pair = _mapper.add_pair(_current.preset, row, block);

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
           #if NUM_BLOCK_CHAIN_ROWS != 1
            blockdata.linkedRow = UINT8_MAX;
            blockdata.linkedBlock = UINT8_MAX;
           #endif
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
                if ((plugin->ports[i].flags & Lv2ParameterHidden) != 0)
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
            _mapper.remove(_current.preset, row, block);
        }
    }
    else if (!isNullURI(blockdata.uri))
    {
        --_current.numLoadedPlugins;
        hostRemoveAllBlockBindings(row, block);
        hostRemoveInstanceForBlock(row, block);
        resetBlock(blockdata);
    }
    else
    {
        fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - already empty, rejected\n", block, uri);
        return false;
    }

    if (!isNullURI(blockdata.uri))
    {
        ++_current.numLoadedPlugins;

        // replace old direct connections if this is the first plugin
        if (_current.numLoadedPlugins == 1)
        {
            assert(row == 0);
            _host.disconnect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
            _host.disconnect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
            hostConnectBlockToChainInput(row, block);
            hostConnectBlockToChainOutput(row, block);
        }
        // otherwise we need to add ourselves more carefully
        else
        {
            std::array<bool, NUM_BLOCKS_PER_PRESET> loaded;
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                loaded[bl] = !isNullURI(_current.chains[row].blocks[bl].uri);

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

            printf("replaceBlock add mode before: %u, after: %u | block: %u\n", before, after, block);

            if (after != NUM_BLOCKS_PER_PRESET)
                hostDisconnectAllBlockInputs(row, after);

            if (before != NUM_BLOCKS_PER_PRESET)
                hostDisconnectAllBlockOutputs(row, before);
            else
                before = 0;

            hostEnsureStereoChain(row, before, NUM_BLOCKS_PER_PRESET - 1);
            hostConnectAll(row, before, NUM_BLOCKS_PER_PRESET - 1);
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
            std::array<bool, NUM_BLOCKS_PER_PRESET> loaded;
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                loaded[bl] = !isNullURI(_current.chains[row].blocks[bl].uri);

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

            hostEnsureStereoChain(row, start, NUM_BLOCKS_PER_PRESET - 1);
            hostConnectAll(row, start, NUM_BLOCKS_PER_PRESET - 1);
        }
    }

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

#if NUM_BLOCK_CHAIN_ROWS != 1

bool HostConnector::swapBlockRow(const uint8_t row,
                                 const uint8_t block,
                                 const uint8_t emptyRow,
                                 const uint8_t emptyBlock)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(emptyRow < NUM_BLOCK_CHAIN_ROWS);
    assert(emptyBlock < NUM_BLOCKS_PER_PRESET);
    assert(row != emptyRow);
    assert(isNullURI(_current.chains[emptyRow].blocks[emptyBlock].uri));

    Block& blockdata(_current.chains[row].blocks[block]);

    // TODO reconnect ports

    std::swap(_current.chains[row].blocks[block], _current.chains[emptyRow].blocks[emptyBlock]);

    _mapper.swap(_current.preset, row, block, emptyRow, emptyBlock);

    // update bindings
    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        for (Binding& bindingdata : _current.bindings[hwid])
        {
            if (bindingdata.row != row)
                continue;
            if (bindingdata.block != block)
                continue;

            bindingdata.row = emptyRow;
            bindingdata.block = emptyBlock;
        }
    }

    _current.dirty = true;
    return true;
}

#endif // NUM_BLOCK_CHAIN_ROWS != 1

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::switchPreset(const uint8_t preset)
{
    if (_current.preset == preset || preset >= NUM_PRESETS_PER_BANK)
        return false;

    // store old active preset in memory before doing anything
    const Current old = _current;
    bool oldloaded[NUM_BLOCK_CHAIN_ROWS][NUM_BLOCKS_PER_PRESET];

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

            _host.disconnect(JACK_CAPTURE_PORT_1, JACK_PLAYBACK_PORT_1);
            _host.disconnect(JACK_CAPTURE_PORT_2, JACK_PLAYBACK_PORT_2);
        }
        else
        {
            for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
            {
                for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                {
                    if (! (oldloaded[row][bl] = !isNullBlock(old.chains[row].blocks[bl])))
                        continue;

                    hostDisconnectAllBlockInputs(row, bl);
                    hostDisconnectAllBlockOutputs(row, bl);

                    const HostBlockPair hbp = _mapper.get(old.preset, row, bl);

                    if (hbp.id != kMaxHostInstances)
                        _host.activate(hbp.id, false);

                    if (hbp.pair != kMaxHostInstances)
                        _host.activate(hbp.pair, false);
                }
            }
        }

        // step 3: activate and connect all plugins in new preset
        uint8_t lastBlockInRow0 = 0;
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

                if (++numLoadedPlugins == 1)
                    hostConnectBlockToChainInput(row, bl);
                else
                    hostConnectBlockToBlock(row, last, bl);

                last = bl;
            }

            if (numLoadedPlugins != 0)
            {
                _current.numLoadedPlugins += numLoadedPlugins;

               #if NUM_BLOCK_CHAIN_ROWS != 1
                if (row != 0)
                {
                    hostConnectBlockToChainOutput(row, last);
                }
                else
               #endif
                // handled outside the loop, so the last connection we do is for system audio output
                {
                    lastBlockInRow0 = last;
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
            hostConnectBlockToChainOutput(0, lastBlockInRow0);
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

        for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
        {
            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                const Block& defblockdata = defaults.chains[row].blocks[bl];
                const Block& oldblockdata = defaults.chains[row].blocks[bl];

                // using same plugin (or both empty)
                if (defblockdata.uri == old.chains[row].blocks[bl].uri)
                {
                    if (isNullBlock(defblockdata))
                        continue;

                    const HostBlockPair hbp = _mapper.get(old.preset, row, bl);
                    if (hbp.id == kMaxHostInstances)
                        continue;

                    if (defblockdata.enabled != oldblockdata.enabled)
                    {
                        _host.bypass(hbp.id, !defblockdata.enabled);

                        if (hbp.pair != kMaxHostInstances)
                            _host.bypass(hbp.pair, !defblockdata.enabled);
                    }

                    params.clear();

                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        const Parameter& defparameterdata(defblockdata.parameters[p]);
                        const Parameter& oldparameterdata(oldblockdata.parameters[p]);

                        if (isNullURI(defparameterdata.symbol))
                            break;
                        if (defparameterdata.value == oldparameterdata.value)
                            continue;

                        params.push_back({ defparameterdata.symbol.c_str(), defparameterdata.value });
                    }

                    _host.params_flush(hbp.id, KXSTUDIO__Reset_full, params.size(), params.data());

                    if (hbp.pair != kMaxHostInstances)
                        _host.params_flush(hbp.pair, KXSTUDIO__Reset_full, params.size(), params.data());

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
                if (isNullURI(defaults.chains[row].blocks[bl].uri))
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
                    const Parameter& defparameterdata(defblockdata.parameters[p]);
                    if (isNullURI(defparameterdata.symbol))
                        break;

                    params.push_back({ defparameterdata.symbol.c_str(), defparameterdata.value });
                }

                _host.params_flush(hbp.id, KXSTUDIO__Reset_full, params.size(), params.data());

                if (hbp.pair != kMaxHostInstances)
                    _host.params_flush(hbp.pair, KXSTUDIO__Reset_full, params.size(), params.data());
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

    // preallocating some data
    std::vector<flushed_param> params;
    params.reserve(MAX_PARAMS_PER_BLOCK);

    _current.scene = scene;

    const Host::NonBlockingScope hnbs(_host);

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            Block& blockdata(_current.chains[row].blocks[bl]);
            if (isNullURI(blockdata.uri))
                continue;
            if (! blockdata.meta.hasScenes)
                continue;

            const HostBlockPair hbp = _mapper.get(_current.preset, row, bl);
            if (hbp.id == kMaxHostInstances)
                continue;

            params.clear();

            for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                Parameter& paramdata(blockdata.parameters[p]);
                if (isNullURI(paramdata.symbol))
                    break;
                if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
                    continue;
                if (! blockdata.sceneValues[_current.scene][p].used)
                    continue;

                paramdata.value = blockdata.sceneValues[_current.scene][p].value;

                params.push_back({ paramdata.symbol.c_str(), paramdata.value });
            }

            _host.params_flush(hbp.id, KXSTUDIO__Reset_soft, params.size(), params.data());

            if (hbp.pair != kMaxHostInstances)
                _host.params_flush(hbp.pair, KXSTUDIO__Reset_soft, params.size(), params.data());
        }
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockBinding(const uint8_t hwid, const uint8_t row, const uint8_t block)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    _current.bindings[hwid].push_back({ row, block, ":bypass", { 0 } });
    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::addBlockParameterBinding(const uint8_t hwid,
                                             const uint8_t row,
                                             const uint8_t block,
                                             const uint8_t paramIndex)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    const Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    const Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return false;
    if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
        return false;

    _current.bindings[hwid].push_back({ row, block, paramdata.symbol, { paramIndex } });
    _current.dirty = true;
    return true;
}

bool HostConnector::removeBlockBinding(const uint8_t hwid, const uint8_t row, const uint8_t block)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    std::list<Binding>& bindings(_current.bindings[hwid]);
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

bool HostConnector::removeBlockParameterBinding(const uint8_t hwid,
                                                const uint8_t row,
                                                const uint8_t block,
                                                const uint8_t paramIndex)
{
    assert(hwid < NUM_BINDING_ACTUATORS);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    const Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
        return false;

    const Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return false;
    if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
        return false;

    std::list<Binding>& bindings(_current.bindings[hwid]);
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

void HostConnector::setBlockParameter(const uint8_t row,
                                      const uint8_t block,
                                      const uint8_t paramIndex,
                                      const float value)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
        return;

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    if (hbp.id == kMaxHostInstances)
        return;

    Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return;
    if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
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

    _host.param_set(hbp.id, paramdata.symbol.c_str(), value);

    if (hbp.pair != kMaxHostInstances)
        _host.param_set(hbp.pair, paramdata.symbol.c_str(), value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::monitorBlockOutputParameter(const uint8_t row, const uint8_t block, const uint8_t paramIndex)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(paramIndex < MAX_PARAMS_PER_BLOCK);

    const Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
        return;

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    if (hbp.id == kMaxHostInstances)
        return;

    const Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return;

    if ((paramdata.meta.flags & Lv2PortIsOutput) != 0)
        _host.monitor_output(hbp.id, paramdata.symbol.c_str());
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

void HostConnector::setBlockProperty(const uint8_t row,
                                     const uint8_t block,
                                     const char* const uri,
                                     const char* const value)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(uri != nullptr && *uri != '\0');
    assert(value != nullptr);

    const Block& blockdata(_current.chains[row].blocks[block]);
    if (isNullURI(blockdata.uri))
        return;

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    if (hbp.id == kMaxHostInstances)
        return;

    _current.dirty = true;

    _host.patch_set(hbp.id, uri, value);

    if (hbp.pair != kMaxHostInstances)
        _host.patch_set(hbp.pair, uri, value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectAll(const uint8_t row, uint8_t blockStart, uint8_t blockEnd)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(blockStart <= blockEnd);
    assert(blockEnd < NUM_BLOCKS_PER_PRESET);

    ChainRow& chain = _current.chains[row];

    if (_current.numLoadedPlugins == 0)
    {
        // direct connections
        _host.connect(chain.inputs[0].c_str(), chain.outputs[0].c_str());
        _host.connect(chain.inputs[1].c_str(), chain.outputs[1].c_str());
        return;
    }

    // direct connections
    _host.disconnect(chain.inputs[0].c_str(), chain.outputs[0].c_str());
    _host.disconnect(chain.inputs[1].c_str(), chain.outputs[1].c_str());

    std::array<bool, NUM_BLOCKS_PER_PRESET> loaded;
    for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        loaded[bl] = !isNullBlock(chain.blocks[bl]);

    // first blocks
    for (uint8_t bl = 0; bl <= blockEnd; ++bl)
    {
        if (loaded[bl])
        {
            if (bl >= blockStart && bl <= blockEnd)
                hostConnectBlockToChainInput(row, bl);
            break;
        }
    }

    // last blocks
    for (uint8_t bl = NUM_BLOCKS_PER_PRESET - 1; bl >= blockStart && bl != UINT8_MAX; --bl)
    {
        if (loaded[bl])
        {
            if (bl >= blockStart && bl <= blockEnd)
                hostConnectBlockToChainOutput(row, bl);
            break;
        }
    }

    // find connecting blocks
    if (blockStart != 0)
    {
        for (uint8_t bl = blockStart - 1; bl != UINT8_MAX; --bl)
        {
            if (loaded[bl])
            {
                blockStart = bl;
                break;
            }
        }
    }

    if (blockEnd != NUM_BLOCKS_PER_PRESET - 1)
    {
        for (uint8_t bl = blockEnd + 1; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            if (loaded[bl])
            {
                blockEnd = bl;
                break;
            }
        }
    }

    // now we can connect between blocks
    for (uint8_t bl1 = blockStart; bl1 < blockEnd; ++bl1)
    {
        if (! loaded[bl1])
            continue;

        for (uint8_t bl2 = bl1 + 1; bl2 <= blockEnd; ++bl2)
        {
            if (! loaded[bl2])
                continue;

            hostConnectBlockToBlock(row, bl1, bl2);
            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBlockToBlock(const uint8_t row, const uint8_t blockA, const uint8_t blockB)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(blockA < NUM_BLOCKS_PER_PRESET);
    assert(blockB < NUM_BLOCKS_PER_PRESET);

    const Lv2Plugin* const pluginA = lv2world.get_plugin_by_uri(_current.chains[row].blocks[blockA].uri.c_str());
    const Lv2Plugin* const pluginB = lv2world.get_plugin_by_uri(_current.chains[row].blocks[blockB].uri.c_str());

    if (pluginA == nullptr || pluginB == nullptr)
        return;

    const HostBlockPair hbpA = _mapper.get(_current.preset, row, blockA);
    const HostBlockPair hbpB = _mapper.get(_current.preset, row, blockB);

    if (hbpA.id == kMaxHostInstances || hbpB.id == kMaxHostInstances)
        return;

    std::string origin, target;

    // FIXME cleanup this mess, find a better way
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

            origin = format("effect_%d:%s", hbpA.id, pluginA->ports[a].symbol.c_str());
            target = format("effect_%d:%s", hbpB.id, pluginB->ports[b].symbol.c_str());
            _host.connect(origin.c_str(), target.c_str());
            aConnectedPort = a;
            bIsConnected = true;

            if (hbpA.pair != kMaxHostInstances && hbpB.pair != kMaxHostInstances)
            {
                origin = format("effect_%d:%s", hbpA.pair, pluginA->ports[a].symbol.c_str());
                target = format("effect_%d:%s", hbpB.pair, pluginB->ports[b].symbol.c_str());
                _host.connect(origin.c_str(), target.c_str());
                return;
            }

            if (hbpA.pair != kMaxHostInstances)
            {
                for (size_t b2 = b + 1; b2 < pluginB->ports.size(); ++b2)
                {
                    if ((pluginB->ports[b2].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                        continue;

                    origin = format("effect_%d:%s", hbpA.pair, pluginA->ports[a].symbol.c_str());
                    target = format("effect_%d:%s", hbpB.id, pluginB->ports[b2].symbol.c_str());
                    _host.connect(origin.c_str(), target.c_str());
                }
                return;
            }

            if (hbpB.pair != kMaxHostInstances)
            {
                for (size_t a2 = a + 1; a2 < pluginA->ports.size(); ++a2)
                {
                    if ((pluginA->ports[a2].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                        continue;

                    origin = format("effect_%d:%s", hbpA.id, pluginA->ports[a2].symbol.c_str());
                    target = format("effect_%d:%s", hbpB.pair, pluginB->ports[b].symbol.c_str());
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
            origin = format("effect_%d:%s", hbpA.id, pluginA->ports[aConnectedPort].symbol.c_str());
            target = format("effect_%d:%s", hbpB.id, pluginB->ports[b].symbol.c_str());
            _host.connect(origin.c_str(), target.c_str());
        }
    }
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

void HostConnector::hostDisconnectAll()
{
    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        {
            const Block& blockdata(_current.chains[row].blocks[bl]);
            if (isNullURI(blockdata.uri))
                continue;

            hostDisconnectAllBlockInputs(row, bl);
            hostDisconnectAllBlockOutputs(row, bl);
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockInputs(const uint8_t row, const uint8_t block)
{
    hostDisconnectBlockAction(row, block, false);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectAllBlockOutputs(const uint8_t row, const uint8_t block)
{
    hostDisconnectBlockAction(row, block, true);
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
    uint8_t lastBlockInRow0 = 0;

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        for (uint8_t pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
        {
            const bool active = _current.preset == pr;
            bool firstBlock = true;
            bool previousPluginStereoOut;

            // related to active preset only
            uint8_t last = 0;
            uint8_t numLoadedPlugins = 0;

            const ChainRow& chaindata(active ? _current.chains[row] : _presets[pr].chains[row]);

            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                const Block& blockdata(chaindata.blocks[bl]);
                if (isNullURI(blockdata.uri))
                    continue;

                if (firstBlock)
                {
                    firstBlock = false;
                    previousPluginStereoOut = chaindata.inputs[0] != chaindata.inputs[1];
                }

                const auto loadInstance = [=](const uint16_t instance)
                {
                    if (active ? _host.add(blockdata.uri.c_str(), instance)
                               : _host.preload(blockdata.uri.c_str(), instance))
                    {
                        if (!blockdata.enabled)
                            _host.bypass(instance, true);

                        for (const Parameter& parameterdata : blockdata.parameters)
                        {
                            if (isNullURI(parameterdata.symbol))
                                break;
                            _host.param_set(instance, parameterdata.symbol.c_str(), parameterdata.value);
                        }

                        return true;
                    }

                    return false;
                };

                const bool dualmono = previousPluginStereoOut && blockdata.meta.isMonoIn;
                const uint16_t instance = _mapper.add(pr, row, bl);

                bool added = loadInstance(instance);

                if (added)
                {
                    if (dualmono)
                    {
                        const uint16_t pair = _mapper.add_pair(pr, row, bl);

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
                        resetBlock(_current.chains[row].blocks[bl]);

                    _mapper.remove(pr, row, bl);
                    continue;
                }

                previousPluginStereoOut = blockdata.meta.isStereoOut || dualmono;

                // dealing with connections after this point, only valid if preset is the active one
                if (active)
                {
                    if (++numLoadedPlugins == 1)
                        hostConnectBlockToChainInput(row, bl);
                    else
                        hostConnectBlockToBlock(row, last, bl);

                    last = bl;
                }
            }

            if (active && numLoadedPlugins != 0)
            {
                _current.numLoadedPlugins += numLoadedPlugins;

               #if NUM_BLOCK_CHAIN_ROWS != 1
                if (row != 0)
                {
                    hostConnectBlockToChainOutput(row, last);
                }
                else
               #endif
                // handled outside the loop, so the last connection we do is for system audio output
                {
                    lastBlockInRow0 = last;
                }
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
        hostConnectBlockToChainOutput(0, lastBlockInRow0);
    }

    _host.feature_enable(Host::kFeatureProcessing, Host::kProcessingOnWithFadeIn);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectChainInputAction(const uint8_t row, const uint8_t block, const bool connect)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(!isNullURI(_current.chains[row].blocks[block].uri));

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

        origin = _current.chains[row].inputs[j++];
        target = format("effect_%d:%s", hbp.id, plugin->ports[i].symbol.c_str());
        assert_continue(!origin.empty());
        (_host.*call)(origin.c_str(), target.c_str());

        if (hbp.pair != kMaxHostInstances)
        {
            origin = _current.chains[row].inputs[j++];
            target = format("effect_%d:%s", hbp.pair, plugin->ports[i].symbol.c_str());
            assert_continue(!origin.empty());
            (_host.*call)(origin.c_str(), target.c_str());
            return;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectChainOutputAction(const uint8_t row, const uint8_t block, const bool connect)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(!isNullURI(_current.chains[row].blocks[block].uri));

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.chains[row].blocks[block].uri.c_str());
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

        origin = format("effect_%d:%s", hbp.id, plugin->ports[i].symbol.c_str());
        target = _current.chains[row].outputs[dsti++];
        assert_continue(!target.empty());
        (_host.*call)(origin.c_str(), target.c_str());

        if (hbp.pair != kMaxHostInstances)
        {
            origin = format("effect_%d:%s", hbp.pair, plugin->ports[i].symbol.c_str());
            target = _current.chains[row].outputs[dsti++];
            assert_continue(!target.empty());
            (_host.*call)(origin.c_str(), target.c_str());
            return;
        }
    }

    if (dsti == 1)
        (_host.*call)(origin.c_str(), _current.chains[row].outputs[1].c_str());
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectBlockAction(const uint8_t row, const uint8_t block, const bool outputs)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);
    assert(!isNullURI(_current.chains[row].blocks[block].uri));

    const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.chains[row].blocks[block].uri.c_str());
    assert_return(plugin != nullptr,);

    const HostBlockPair hbp = _mapper.get(_current.preset, row, block);
    assert_return(hbp.id != kMaxHostInstances,);

    const unsigned int ioflags = Lv2PortIsAudio | (outputs ? Lv2PortIsOutput : 0);
    std::string origin;

    for (const Lv2Port& port : plugin->ports)
    {
        if ((port.flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != ioflags)
            continue;

        origin = format("effect_%d:%s", hbp.id, port.symbol.c_str());
        _host.disconnect_all(origin.c_str());

        if (hbp.pair != kMaxHostInstances)
        {
            origin = format("effect_%d:%s", hbp.pair, port.symbol.c_str());
            _host.disconnect_all(origin.c_str());
            return;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostEnsureStereoChain(const uint8_t row, const uint8_t blockStart, const uint8_t blockEnd)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(blockStart <= blockEnd);
    assert(blockEnd < NUM_BLOCKS_PER_PRESET);

    bool previousPluginStereoOut = shouldBlockBeStereo(_current.chains[row], blockStart);

    for (uint8_t bl = blockStart; bl <= blockEnd; ++bl)
    {
        const Block& blockdata(_current.chains[row].blocks[bl]);
        if (isNullURI(blockdata.uri))
            continue;

        const bool oldDualmono = _mapper.get(_current.preset, row, bl).pair != kMaxHostInstances;
        const bool newDualmono = previousPluginStereoOut && blockdata.meta.isMonoIn;

        if (oldDualmono != newDualmono)
        {
            if (newDualmono)
            {
                const uint16_t pair = _mapper.add_pair(_current.preset, row, bl);

                if (_host.add(blockdata.uri.c_str(), pair))
                {
                    if (!blockdata.enabled)
                        _host.bypass(pair, true);

                    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        const Parameter& parameterdata(blockdata.parameters[p]);
                        if (isNullURI(parameterdata.symbol))
                            break;
                        _host.param_set(pair, parameterdata.symbol.c_str(), parameterdata.value);
                    }

                    // disconnect ports, we might have mono to stereo connections
                    hostDisconnectAllBlockOutputs(row, bl);
                }
            }
            else
            {
                _host.remove(_mapper.remove_pair(_current.preset, row, bl));
            }
        }

        previousPluginStereoOut = blockdata.meta.isStereoOut || newDualmono;
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostRemoveAllBlockBindings(const uint8_t row, const uint8_t block)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
    {
        std::list<Binding>& bindings(_current.bindings[hwid]);

    restart:
        for (BindingIteratorConst it = bindings.cbegin(), end = bindings.cend(); it != end; ++it)
        {
            if (it->row != row)
                continue;
            if (it->block != block)
                continue;

            bindings.erase(it);
            _current.dirty = true;
            goto restart;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostRemoveInstanceForBlock(const uint8_t row, const uint8_t block)
{
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const HostBlockPair hbp = _mapper.remove(_current.preset, row, block);

    if (hbp.id != kMaxHostInstances)
        _host.remove(hbp.id);

    if (hbp.pair != kMaxHostInstances)
        _host.remove(hbp.pair);
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

    case HostFeedbackData::kFeedbackLog:
        cdata.type = HostCallbackData::kLog;
        cdata.log.type = data.log.type;
        cdata.log.msg = data.log.msg;
        break;

    case HostFeedbackData::kFeedbackParameterSet:
    case HostFeedbackData::kFeedbackOutputMonitor:
        assert(data.paramSet.effect_id >= 0);
        assert(data.paramSet.effect_id < MAX_MOD_HOST_PLUGIN_INSTANCES + MAX_MOD_HOST_TOOL_INSTANCES);

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
        assert(data.patchSet.effect_id < MAX_MOD_HOST_PLUGIN_INSTANCES + MAX_MOD_HOST_TOOL_INSTANCES);

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

    // assume we dont want mod-host side monitoring if input is a plugin
    if constexprstr (std::strncmp(JACK_CAPTURE_PORT_1, MOD_HOST_EFFECT_PREFIX, MOD_HOST_EFFECT_PREFIX_LEN) != 0)
        _host.monitor_audio_levels(JACK_CAPTURE_PORT_1, true);

    if constexprstr (std::strncmp(JACK_CAPTURE_PORT_1, MOD_HOST_EFFECT_PREFIX, MOD_HOST_EFFECT_PREFIX_LEN) != 0 &&
                     std::strcmp(JACK_CAPTURE_PORT_1, JACK_CAPTURE_PORT_2) != 0)
        _host.monitor_audio_levels(JACK_CAPTURE_PORT_2, true);

    // assume we dont want mod-host side monitoring if output is a plugin
    if constexprstr (std::strncmp(JACK_PLAYBACK_MONITOR_PORT_1, MOD_HOST_EFFECT_PREFIX, MOD_HOST_EFFECT_PREFIX_LEN) != 0)
        _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_1, true);

    if constexprstr (std::strncmp(JACK_PLAYBACK_MONITOR_PORT_2, MOD_HOST_EFFECT_PREFIX, MOD_HOST_EFFECT_PREFIX_LEN) != 0 &&
                     std::strcmp(JACK_PLAYBACK_MONITOR_PORT_1, JACK_PLAYBACK_MONITOR_PORT_2) != 0)
        _host.monitor_audio_levels(JACK_PLAYBACK_MONITOR_PORT_2, true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::allocPreset(Preset& preset)
{
    for (ChainRow& chain : preset.chains)
    {
        chain.blocks.resize(NUM_BLOCKS_PER_PRESET);

        for (Block& block : chain.blocks)
            allocBlock(block);
    }
}

void HostConnector::resetPreset(Preset& preset)
{
    preset.name.clear();
    preset.chains[0].inputs[0] = JACK_CAPTURE_PORT_1;
    preset.chains[0].inputs[1] = JACK_CAPTURE_PORT_2;
    preset.chains[0].outputs[0] = JACK_PLAYBACK_PORT_1;
    preset.chains[0].outputs[1] = JACK_PLAYBACK_PORT_2;

    for (uint8_t row = 0; row < NUM_BLOCK_CHAIN_ROWS; ++row)
    {
        if (row != 0)
        {
            preset.chains[row].inputs.fill({});
            preset.chains[row].outputs.fill({});

            // FIXME REMOVE THIS
            preset.chains[row].inputs[0] = JACK_CAPTURE_PORT_1;
            preset.chains[row].inputs[1] = JACK_CAPTURE_PORT_2;
            preset.chains[row].outputs[0] = JACK_PLAYBACK_PORT_1;
            preset.chains[row].outputs[1] = JACK_PLAYBACK_PORT_2;
        }

        for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            resetBlock(preset.chains[row].blocks[bl]);
    }

    for (uint8_t hwid = 0; hwid < NUM_BINDING_ACTUATORS; ++hwid)
        preset.bindings[hwid].clear();
}

// --------------------------------------------------------------------------------------------------------------------
