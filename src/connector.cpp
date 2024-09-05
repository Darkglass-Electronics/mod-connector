// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"
#include "json.hpp"
#include "utils.hpp"

#include <cstring>
#include <fstream>
#include <map>

#define JSON_STATE_VERSION_CURRENT 0
#define JSON_STATE_VERSION_MIN_SUPPORTED 0
#define JSON_STATE_VERSION_MAX_SUPPORTED 0

// --------------------------------------------------------------------------------------------------------------------

template <class Param>
static void resetParam(Param& param)
{
    param = {};
    param.meta.max = 1.f;
}

template <class Block>
static void resetBlock(Block& block)
{
    block = {};
    block.meta.bindingIndex = -1;

    for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetParam(block.parameters[p]);
}

template <class Preset>
static void resetPreset(Preset& preset)
{
    preset = {};

    for (int bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
        resetBlock(preset.blocks[bl]);
}

template <class Bank>
static void resetBank(Bank& bank)
{
    bank = {};

    for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
        resetPreset(bank.presets[pr]);
}

// --------------------------------------------------------------------------------------------------------------------

HostConnector::HostConnector()
{
    if (! host.last_error.empty())
    {
        fprintf(stderr, "Failed to initialize host connection: %s\n", host.last_error.c_str());
        return;
    }

    ok = true;
}

// --------------------------------------------------------------------------------------------------------------------
// load state from a file and store it in the `current` struct
// returning false means the current chain was unchanged

bool HostConnector::loadStateFromFile(const char* const filename)
{
    std::ifstream f(filename);
    nlohmann::json j;

    try {
        j = nlohmann::json::parse(f);
    } catch (...) {
        return false;
    }

    if (! (j.contains("state") && j.contains("type") && j.contains("version")))
        return false;

    try {
        if (j["type"].get<std::string>() != "state")
        {
            fprintf(stderr, "HostConnector::loadStateFromFile failed: file is not state type\n");
            return false;
        }

        const int version = j["version"].get<int>();
        if (version < JSON_STATE_VERSION_MIN_SUPPORTED || version > JSON_STATE_VERSION_MAX_SUPPORTED)
        {
            fprintf(stderr, "HostConnector::loadStateFromFile failed: version mismatch\n");
            return false;
        }

        j = j["state"].get<nlohmann::json>();
    } catch (...) {
        return false;
    }

    if (j.contains("bank"))
    {
        current.bank = j["bank"].get<int>() - 1;

        if (current.bank < 0 || current.bank >= NUM_BANKS)
            current.bank = 0;
    }
    else
    {
        current.bank = 0;
    }

    // always start with the first preset
    current.preset = 0;

    if (! j.contains("banks"))
    {
        // full reset
        printf("HostConnector::loadStateFromFile: no banks in file, using empty state\n");
        for (int b = 0; b < NUM_BANKS; ++b)
            resetBank(current.banks[b]);
        hostLoadCurrent();
        return true;
    }

    auto& jbanks = j["banks"];
    for (int b = 0; b < NUM_BANKS; ++b)
    {
        auto& bank = current.banks[b];
        const std::string jbankid = std::to_string(b + 1);

        if (! jbanks.contains(jbankid))
        {
            resetBank(bank);
            continue;
        }

        auto& jbank = jbanks[jbankid];
        if (! jbank.contains("presets"))
        {
            printf("HostConnector::loadStateFromFile: bank #%d does not include presets, using empty bank\n", b + 1);
            resetBank(bank);
            continue;
        }

        auto& jpresets = jbank["presets"];
        for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
        {
            auto& preset = bank.presets[pr];
            const std::string jpresetid = std::to_string(pr + 1);

            if (! jbanks.contains(jbankid))
            {
                printf("HostConnector::loadStateFromFile: bank #%d does not include preset #%d, using empty preset\n",
                       b + 1, pr + 1);
                resetPreset(preset);
                continue;
            }

            auto& jpreset = jpresets[jpresetid];
            if (! jpreset.contains("blocks"))
            {
                printf("HostConnector::loadStateFromFile: bank #%d / preset #%d does not include blocks, using empty preset\n",
                       b + 1, pr + 1);
                resetPreset(preset);
                continue;
            }

            auto& jblocks = jpreset["blocks"];
            for (int bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                auto& block = preset.blocks[bl];
                const std::string jblockid = std::to_string(bl + 1);

                if (! jblocks.contains(jblockid))
                {
                    resetBlock(block);
                    continue;
                }

                auto& jblock = jblocks[jblockid];
                if (! jblock.contains("uri"))
                {
                    printf("HostConnector::loadStateFromFile: bank #%d / preset #%d / block #%d does not include uri, using empty block\n",
                           b + 1, pr + 1, bl + 1);
                    resetBlock(block);
                    continue;
                }

                const std::string uri = jblock["uri"].get<std::string>();

                const Lv2Plugin* const plugin = !isNullURI(uri)
                                              ? lv2world.get_plugin_by_uri(uri.c_str())
                                              : nullptr;

                if (plugin == nullptr)
                {
                    printf("HostConnector::loadStateFromFile: plugin with uri '%s' not available, using empty block\n",
                           uri.c_str());
                    resetBlock(block);
                    continue;
                }

                block.bindingSymbol.clear();
                block.enabled = true;
                block.uri = uri;

                block.meta.bindingIndex = -1;
                block.meta.name = plugin->name;

                if (jblock.contains("enabled"))
                    block.enabled = jblock["enabled"].get<bool>();

                // parameters are always filled in lv2 metadata first, then overriden with json data
                int numParams = 0;
                std::map<std::string, int> symbolToIndexMap;
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
                        block.bindingSymbol = plugin->ports[i].symbol;
                        block.meta.bindingIndex = numParams;
                        break;
                    }

                    auto& param = block.parameters[numParams];

                    param.symbol = plugin->ports[i].symbol;
                    param.value = plugin->ports[i].def;

                    param.meta.flags = plugin->ports[i].flags;
                    param.meta.min = plugin->ports[i].min;
                    param.meta.max = plugin->ports[i].max;
                    param.meta.name = plugin->ports[i].name;
                    param.meta.unit = plugin->ports[i].unit;
                    param.meta.scalePoints = plugin->ports[i].scalePoints;

                    symbolToIndexMap[param.symbol] = numParams++;
                }

                for (int p = numParams; p < MAX_PARAMS_PER_BLOCK; ++p)
                    resetParam(block.parameters[p]);

                try {
                    const std::string binding = jblock["binding"].get<std::string>();

                    for (int p = 0; p < numParams; ++p)
                    {
                        if (block.parameters[p].symbol == binding)
                        {
                            block.bindingSymbol = binding;
                            block.meta.bindingIndex = p;
                            break;
                        }
                    }

                } catch (...) {}

                if (! jblock.contains("parameters"))
                    continue;

                auto& jparams = jblock["parameters"];
                for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const std::string jparamid = std::to_string(p + 1);

                    if (! jparams.contains(jparamid))
                        continue;

                    auto& jparam = jparams[jparamid];
                    if (! (jparam.contains("symbol") && jparam.contains("value")))
                    {
                        printf("HostConnector::loadStateFromFile: param #%d is missing symbol and/or value\n",
                               p + 1);
                        continue;
                    }

                    const std::string symbol = jparam["symbol"].get<std::string>();

                    if (symbolToIndexMap.find(symbol) == symbolToIndexMap.end())
                    {
                        printf("HostConnector::loadStateFromFile: param with '%s' symbol does not exist in plugin\n",
                               symbol.c_str());
                        continue;
                    }

                    block.parameters[p].value = std::max(block.parameters[p].meta.min,
                                                         std::min<float>(block.parameters[p].meta.max,
                                                                         jparam["value"].get<double>()));
                }
            }
        }
    }

    const Host::NonBlockingScope hnbs(host);
    hostLoadCurrent();
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// save host state as stored in the `current` struct into a file

bool HostConnector::saveStateToFile(const char* const filename) const
{
    nlohmann::json j;
    try {
        j["version"] = JSON_STATE_VERSION_CURRENT;
        j["type"] = "state";
        j["state"] = {
            { "bank", current.bank + 1 },
            { "banks", nlohmann::json::object({}) },
        };

        auto& jbanks = j["state"]["banks"];
        for (int b = 0; b < NUM_BANKS; ++b)
        {
            auto& bank = current.banks[b];
            const std::string jbankid = std::to_string(b + 1);
            jbanks[jbankid]["presets"] = nlohmann::json::object({});

            auto& jpresets = jbanks[jbankid]["presets"];
            for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
            {
                auto& preset = bank.presets[pr];
                const std::string jpresetid = std::to_string(pr + 1);
                jpresets[jpresetid]["blocks"] = nlohmann::json::object({});

                auto& jblocks = jpresets[jpresetid]["blocks"];
                for (int bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
                {
                    auto& block = preset.blocks[bl];

                    if (isNullURI(block.uri))
                        continue;

                    const std::string jblockid = std::to_string(bl + 1);
                    jblocks[jblockid] = {
                        { "binding", block.bindingSymbol },
                        { "enabled", block.enabled },
                        { "uri", isNullURI(block.uri) ? "-" : block.uri },
                        { "parameters", nlohmann::json::object({}) },
                    };

                    auto& jparams = jblocks[jblockid]["parameters"];
                    for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                    {
                        auto& param = block.parameters[p];

                        if (isNullURI(param.symbol))
                            continue;

                        const std::string jparamid = std::to_string(p + 1);
                        jparams[jparamid] = {
                            { "symbol", param.symbol },
                            { "value", param.value },
                        };
                    }
                }
            }
        }
    } catch (...) {
        return false;
    }

    std::ofstream o(filename);
    o << std::setw(2) << j << std::endl;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// reorder a block into a new position
// returning false means the current chain was unchanged

bool HostConnector::reorderBlock(const int block, const int dest)
{
    if (block < 0 || block >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%d, %d) - out of bounds block, rejected\n", block, dest);
        return false;
    }
    if (dest < 0 || dest >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%d, %d) - out of bounds dest, rejected\n", block, dest);
        return false;
    }
    if (block == dest)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%d, %d) - block == dest, rejected\n", block, dest);
        return false;
    }

    const Host::NonBlockingScope hnbs(host);

    auto& blocks(current.banks[current.bank].presets[current.preset].blocks);

    // NOTE this is a very crude quick implementation
    // just removing all plugins and adding them again after reordering
    // it is not meant to be the final implementation, just something quick for experimentation
    host.remove(-1);

    // moving block backwards to the left
    // a b c d e! f
    // a b c e! d f
    // a b e! c d f
    // a e! b c d f
    if (block > dest)
    {
        for (int i = block; i > dest; --i)
            std::swap(blocks[i], blocks[i - 1]);
    }
    // moving block forward to the right
    // a b! c d e f
    // a c b! d e f
    // a c d b! e f
    // a c d e b! f
    else
    {
        for (int i = block; i < dest; ++i)
            std::swap(blocks[i], blocks[i + 1]);
    }

    hostLoadCurrent();
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// replace a block with another lv2 plugin (referenced by its URI)
// passing null or empty string as the URI means clearing the block
// returning false means the block was unchanged

bool HostConnector::replaceBlock(const int block, const char* const uri)
{
    if (block < 0 || block >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::replaceBlock(%d, %s) - out of bounds block, rejected\n", block, uri);
        return false;
    }

    const int instance = 100 * current.preset + block;
    auto& blockdata(current.banks[current.bank].presets[current.preset].blocks[block]);

    const Host::NonBlockingScope hnbs(host);

    if (! isNullURI(uri))
    {
        const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(uri);
        if (plugin == nullptr)
        {
            fprintf(stderr, "HostConnector::replaceBlock(%d, %s) - plugin not available, rejected\n", block, uri);
            return false;
        }

        // we only do changes after verifying that the requested plugin exists
        host.remove(instance);

        blockdata.bindingSymbol.clear();
        blockdata.enabled = true;
        blockdata.uri = uri;

        blockdata.meta.bindingIndex = -1;
        blockdata.meta.name = plugin->name;

        int p = 0;
        for (size_t i = 0; i < plugin->ports.size() && p < MAX_PARAMS_PER_BLOCK; ++i)
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
                blockdata.bindingSymbol = plugin->ports[i].symbol;
                blockdata.meta.bindingIndex = p;
                break;
            }

            blockdata.parameters[p++] = {
                .symbol = plugin->ports[i].symbol,
                .value = plugin->ports[i].def,
                .meta = {
                    .flags = plugin->ports[i].flags,
                    .min = plugin->ports[i].min,
                    .max = plugin->ports[i].max,
                    .name = plugin->ports[i].name,
                    .unit = plugin->ports[i].unit,
                    .scalePoints = plugin->ports[i].scalePoints,
                },
            };
        }

        for (; p < MAX_PARAMS_PER_BLOCK; ++p)
            resetParam(blockdata.parameters[p]);

        if (host.add(uri, instance))
        {
            printf("DEBUG: block %d loaded plugin %s\n", block, uri);
        }
        else
        {
            printf("DEBUG: block %d failed to load plugin %s: %s\n", block, uri, host.last_error.c_str());
            resetBlock(blockdata);
        }

        hostDisconnectForNewBlock(block);
    }
    else
    {
        host.remove(instance);
        resetBlock(blockdata);
    }

    hostConnectBetweenBlocks();
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// convenience method for quickly switching to another bank
// returning false means the current chain was unchanged
// NOTE resets active preset to 0

bool HostConnector::switchBank(const int bank)
{
    if (current.bank == bank || bank < 0 || bank >= NUM_BANKS)
        return false;

    current.bank = bank;
    current.preset = 0;

    const Host::NonBlockingScope hnbs(host);
    hostLoadCurrent();
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// convenience method for quickly switching to another preset within the current bank
// returning false means the current chain was unchanged

bool HostConnector::switchPreset(const int preset)
{
    if (current.preset == preset || preset < 0 || preset >= NUM_PRESETS_PER_BANK)
        return false;

    // const auto& bankdata(current.banks[current.bank]);
    const int oldpreset = current.preset;
    current.preset = preset;

    const Host::NonBlockingScope hnbs(host);

    // step 1: fade out
    // TODO

    // step 2: deactivate all plugins in old preset
#if 1
    host.activate(100 * oldpreset, 100 * oldpreset + NUM_BLOCKS_PER_PRESET, 0);
#else
    {
        const auto& presetdata(bankdata.presets[oldpreset]);

        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const auto& blockdata(presetdata.blocks[b]);
            if (isNullURI(blockdata.uri))
                continue;

            const int instance = 100 * oldpreset + b;
            host.activate(instance, 0);
        }
    }
#endif

    // step 3: activate all plugins in new preset
#if 1
    host.activate(100 * preset, 100 * preset + NUM_BLOCKS_PER_PRESET, 1);
#else
    {
        const auto& presetdata(bankdata.presets[preset]);

        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const auto& blockdata(presetdata.blocks[b]);
            if (isNullURI(blockdata.uri))
                continue;

            const int instance = 100 * preset + b;
            host.activate(instance, 1);
        }
    }
#endif

    hostConnectBetweenBlocks();

    // step 4: fade in
    // TODO

    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// clear current preset

void HostConnector::clearCurrentPreset()
{
    auto& blocks(current.banks[current.bank].presets[current.preset].blocks);

    for (int i = 0; i < NUM_BLOCKS_PER_PRESET; ++i)
        resetBlock(blocks[i]);

    const Host::NonBlockingScope hnbs(host);
    hostLoadCurrent();
}

// --------------------------------------------------------------------------------------------------------------------
// set a block property

void HostConnector::setBlockProperty(int block, const char* property_uri, const char* value)
{
    if (block < 0 || block >= NUM_BLOCKS_PER_PRESET)
        return;
    if (property_uri == nullptr || *property_uri == '\0')
        return;
    if (value == nullptr || *value == '\0')
        return;

    const int instance = 100 * current.preset + block;
    auto& blockdata(current.banks[current.bank].presets[current.preset].blocks[block]);

    if (isNullURI(blockdata.uri))
        return;

    host.patch_set(instance, property_uri, value);
}

// --------------------------------------------------------------------------------------------------------------------
// set the value of a block parameter

void HostConnector::hostUpdateParameterValue(int block, int index)
{
    if (block < 0 || block >= NUM_BLOCKS_PER_PRESET)
        return;
    if (index < 0 || index >= MAX_PARAMS_PER_BLOCK)
        return;

    const int instance = 100 * current.preset + block;
    auto& blockdata(current.banks[current.bank].presets[current.preset].blocks[block]);

    if (isNullURI(blockdata.uri))
        return;

    auto& paramdata(blockdata.parameters[index]);

    host.param_set(instance, paramdata.symbol.c_str(), paramdata.value);
}

// --------------------------------------------------------------------------------------------------------------------
// load host state as saved in the `current` struct

void HostConnector::hostLoadCurrent()
{
    host.feature_enable("processing", false);
    host.remove(-1);

    const auto& bankdata(current.banks[current.bank]);

    for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
    {
        const auto& presetdata(bankdata.presets[pr]);
        const bool active = current.preset == pr;

        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const auto& blockdata(presetdata.blocks[b]);
            if (isNullURI(blockdata.uri))
                continue;

            const int instance = 100 * pr + b;

            if (active)
                host.add(blockdata.uri.c_str(), instance);
            else
                host.preload(blockdata.uri.c_str(), instance);

            for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                const auto& parameterdata(blockdata.parameters[p]);
                if (isNullURI(parameterdata.symbol))
                    continue;
                host.param_set(instance, parameterdata.symbol.c_str(), parameterdata.value);
            }
        }
    }

    hostConnectBetweenBlocks();
    host.feature_enable("processing", true);
}

// --------------------------------------------------------------------------------------------------------------------
// common function to connect all the blocks as needed
// TODO cleanup duplicated code with function below

void HostConnector::hostConnectBetweenBlocks()
{
    const auto& bankdata(current.banks[current.bank]);

    // for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
    {
        const int pr = current.preset;
        const auto& presetdata(bankdata.presets[pr]);

        bool loaded[NUM_BLOCKS_PER_PRESET];
        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
            loaded[b] = !isNullURI(presetdata.blocks[b].uri);

        // first plugin
        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            if (! loaded[b])
                continue;

            const int instance = 100 * pr + b;

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
            {
                int srci = 0, firsti = -1;
                for (size_t i = 0; i < plugin->ports.size(); ++i)
                {
                    if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                        continue;

                    if (firsti == -1)
                        firsti = i;

                    ++srci;
                    const std::string origin(format("system:capture_%d", srci));
                    const std::string target(format("effect_%d:%s", instance, plugin->ports[i].symbol.c_str()));
                    host.connect(origin.c_str(), target.c_str());
                }

                // connect to extra inputs
                if (firsti != -1)
                {
                    srci = 2;
                    for (size_t i = firsti; i < plugin->ports.size(); ++i)
                    {
                        if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                            continue;

                        ++srci;
                        const std::string origin(format("system:capture_%d", srci));
                        const std::string target(format("effect_%d:%s", instance, plugin->ports[i].symbol.c_str()));
                        host.connect(origin.c_str(), target.c_str());
                    }
                }
            }

            break;
        }

        // last plugin
        for (int b = NUM_BLOCKS_PER_PRESET - 1; b >= 0; --b)
        {
            if (! loaded[b])
                continue;

            const int instance = 100 * pr + b;

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
            {
                int dsti = 0, lasti = 0;
                for (size_t i = 0; i < plugin->ports.size(); ++i)
                {
                    if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                        continue;

                    ++dsti;
                    lasti = i;
                    const std::string origin(format("effect_%d:%s", instance, plugin->ports[i].symbol.c_str()));
                    const std::string target(format("mod-monitor:in_%d", dsti));
                    host.connect(origin.c_str(), target.c_str());
                }

                // connect to stereo output if chain is mono
                if (dsti == 1)
                {
                    const std::string origin(format("effect_%d:%s", instance, plugin->ports[lasti].symbol.c_str()));
                    const std::string target(format("mod-monitor:in_%d", dsti + 1));
                    host.connect(origin.c_str(), target.c_str());
                }
            }

            break;
        }

        // between plugins
        for (int b1 = 0; b1 < NUM_BLOCKS_PER_PRESET - 1; ++b1)
        {
            if (! loaded[b1])
                continue;

            for (int b2 = b1 + 1; b2 < NUM_BLOCKS_PER_PRESET; ++b2)
            {
                if (! loaded[b2])
                    continue;

                const int instance1 = 100 * pr + b1;
                const int instance2 = 100 * pr + b2;

                const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(presetdata.blocks[b1].uri.c_str());
                const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(presetdata.blocks[b2].uri.c_str());

                if (plugin1 != nullptr && plugin2 != nullptr)
                {
                    int srci = 0;
                    for (size_t i = 0; i < plugin1->ports.size(); ++i)
                    {
                        if ((plugin1->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                            continue;

                        ++srci;
                        size_t dstj = 0;
                        for (size_t j = 0; j < plugin2->ports.size(); ++j)
                        {
                            if (plugin2->ports[j].flags & Lv2PortIsOutput)
                                continue;
                            if ((plugin2->ports[j].flags & Lv2PortIsAudio) == 0)
                                continue;

                            if (srci != ++dstj)
                                continue;

                            const std::string origin(format("effect_%d:%s", instance1, plugin1->ports[i].symbol.c_str()));
                            const std::string target(format("effect_%d:%s", instance2, plugin2->ports[j].symbol.c_str()));
                            host.connect(origin.c_str(), target.c_str());
                        }
                    }
                }

                break;
            }
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------
// disconnect everything around the new plugin, to prevent double connections
// TODO cleanup duplicated code with function above
// FIXME this logic can be made much better, but this is for now just a testing tool anyhow

void HostConnector::hostDisconnectForNewBlock(const int blockidi)
{
    const auto& bankdata(current.banks[current.bank]);
    const auto& presetdata(bankdata.presets[current.preset]);

    bool loaded[NUM_BLOCKS_PER_PRESET];
    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        loaded[b] = !isNullURI(presetdata.blocks[b].uri);
    loaded[blockidi] = false;

    // first plugin
    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        if (! loaded[b])
            continue;

        const int instance = 100 * current.preset + b;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
        {
            int srci = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                    continue;

                ++srci;
                const std::string origin(format("system:capture_%d", srci));
                const std::string target(format("effect_%d:%s", instance, plugin->ports[i].symbol.c_str()));
                host.disconnect(origin.c_str(), target.c_str());
            }
        }

        break;
    }

    // last plugin
    for (int b = NUM_BLOCKS_PER_PRESET - 1; b >= 0; --b)
    {
        if (! loaded[b])
            continue;

        const int instance = 100 * current.preset + b;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
        {
            int dsti = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                    continue;

                ++dsti;
                const std::string origin(format("effect_%d:%s", instance, plugin->ports[i].symbol.c_str()));
                const std::string target(format("mod-monitor:in_%d", dsti));
                host.disconnect(origin.c_str(), target.c_str());
            }
        }

        break;
    }

    // between plugins
    for (int b1 = 0; b1 < NUM_BLOCKS_PER_PRESET - 1; ++b1)
    {
        if (! loaded[b1])
            continue;

        for (int b2 = b1 + 1; b2 < NUM_BLOCKS_PER_PRESET; ++b2)
        {
            if (! loaded[b2])
                continue;

            const int instance1 = 100 * current.preset + b1;
            const int instance2 = 100 * current.preset + b2;

            const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(presetdata.blocks[b1].uri.c_str());
            const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(presetdata.blocks[b2].uri.c_str());

            if (plugin1 != nullptr && plugin2 != nullptr)
            {
                int srci = 0;
                for (size_t i = 0; i < plugin1->ports.size(); ++i)
                {
                    if ((plugin1->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                        continue;

                    ++srci;
                    size_t dstj = 0;
                    for (size_t j = 0; j < plugin2->ports.size(); ++j)
                    {
                        if (plugin2->ports[j].flags & Lv2PortIsOutput)
                            continue;
                        if ((plugin2->ports[j].flags & Lv2PortIsAudio) == 0)
                            continue;

                        if (srci != ++dstj)
                            continue;

                        const std::string origin(format("effect_%d:%s", instance1, plugin1->ports[i].symbol.c_str()));
                        const std::string target(format("effect_%d:%s", instance2, plugin2->ports[j].symbol.c_str()));
                        host.disconnect(origin.c_str(), target.c_str());
                    }
                }
            }

            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------
