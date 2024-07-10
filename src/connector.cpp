// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"
#include "json.hpp"
#include "utils.hpp"

#include <cstring>
#include <fstream>

#define JSON_STATE_VERSION_CURRENT 0
#define JSON_STATE_VERSION_MIN_SUPPORTED 0
#define JSON_STATE_VERSION_MAX_SUPPORTED 0

// --------------------------------------------------------------------------------------------------------------------

static bool isNullURI(const char* const uri)
{
    return uri == nullptr || uri[0] == '\0' || (uri[0] == '-' && uri[1] == '\0');
}

static bool isNullURI(const std::string& uri)
{
    return uri.empty() || uri == "-";
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
        for (int b = 0; b < NUM_BANKS; ++b)
            current.banks[b] = {};
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
            // reset bank
            bank = {};
            continue;
        }

        auto& jbank = jbanks[jbankid];
        if (! jbank.contains("presets"))
        {
            // reset bank
            bank = {};
            continue;
        }

        auto& jpresets = jbank["presets"];
        for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
        {
            auto& preset = bank.presets[pr];
            const std::string jpresetid = std::to_string(pr + 1);

            if (! jbanks.contains(jbankid))
            {
                // reset preset
                preset = {};
                continue;
            }

            auto& jpreset = jpresets[jpresetid];
            if (! jpreset.contains("blocks"))
            {
                // reset preset
                preset = {};
                continue;
            }

            auto& jblocks = jpreset["blocks"];
            for (int bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                auto& block = preset.blocks[bl];
                const std::string jblockid = std::to_string(bl + 1);

                if (! jblocks.contains(jblockid))
                {
                    // reset block
                    block = {};
                    continue;
                }

                auto& jblock = jblocks[jblockid];
                if (! jblock.contains("uri"))
                {
                    // reset block
                    block = {};
                    continue;
                }

                block.uri = jblock["uri"].get<std::string>();

                try {
                    block.binding = jblock["binding"].get<int>();
                } catch (...) {
                    block.binding = -1;
                }

                if (! jblock.contains("parameters"))
                {
                    // reset parameters
                    for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                        block.parameters[p] = {};
                    continue;
                }

                auto& jparams = jblock["parameters"];
                for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    auto& param = block.parameters[p];
                    const std::string jparamid = std::to_string(p + 1);

                    if (! jparams.contains(jparamid))
                    {
                        // reset param
                        param = {};
                        continue;
                    }

                    auto& jparam = jparams[jparamid];
                    if (! (jparam.contains("symbol") && jparam.contains("value")))
                    {
                        // reset param
                        param = {};
                        continue;
                    }

                    param.symbol = jparam["symbol"].get<std::string>();
                    param.value = jparam["value"].get<double>();
                }
            }
        }
    }

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
                        { "binding", block.binding },
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

bool HostConnector::reorderBlock(const int block, const int dest)
{
    if (block < 0 || block >= NUM_BLOCKS_PER_PRESET)
        return false;
    if (dest < 0 || dest >= NUM_BLOCKS_PER_PRESET)
        return false;
    if (block == dest)
        return false;

    // moving block backwards to the left
    if (block > dest)
    {
    }
    // moving block forward to the right
    else
    {
    }

    // TODO
    return false;
}

// --------------------------------------------------------------------------------------------------------------------
// replace a block with another lv2 plugin (referenced by its URI)
// passing null or empty string as the URI means clearing the block

bool HostConnector::replaceBlock(const int bank, const int preset, const int block, const char* const uri)
{
    if (bank < 0 || bank >= NUM_BANKS)
        return false;
    if (preset < 0 || preset >= NUM_PRESETS_PER_BANK)
        return false;
    if (block < 0 || block >= NUM_BLOCKS_PER_PRESET)
        return false;

    auto& blockdata(current.banks[bank].presets[preset].blocks[block]);
    const bool islive = current.bank == bank && current.preset == preset;

    if (! isNullURI(uri))
    {
        const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(uri);
        if (plugin == nullptr)
            return false;

        // we only do changes after verifying that the requested plugin exists
        if (islive)
            host.remove(block);

        blockdata.binding = -1;
        blockdata.uri = uri;

        int p = 0;
        for (size_t i = 0; i < plugin->ports.size(); ++i)
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
                blockdata.binding = p;
                break;
            }

            blockdata.parameters[p] = {
                .symbol = plugin->ports[i].symbol,
                .value = plugin->ports[i].def,
            };

            if (++p >= MAX_PARAMS_PER_BLOCK)
                break;
        }

        for (; p < MAX_PARAMS_PER_BLOCK; ++p)
            blockdata.parameters[p] = {};

        if (islive)
        {
            if (host.add(uri, block))
            {
                printf("DEBUG: block %d loaded plugin %s\n", block, uri);
            }
            else
            {
                printf("DEBUG: block %d failed to load plugin %s: %s\n", block, uri, host.last_error.c_str());
                blockdata = {};
            }

            // FIXME needed?
            hostDisconnectForNewBlock(block);
        }
    }
    else
    {
        if (islive)
            host.remove(block);

        blockdata = {};
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// convenience call to replace a block for the current bank + preset

bool HostConnector::replaceBlock(const int block, const char* const uri)
{
    return replaceBlock(current.bank, current.preset, block, uri);
}

// --------------------------------------------------------------------------------------------------------------------
// convenience method for quickly switching to another bank
// NOTE resets active preset to 0

bool HostConnector::switchBank(const int bank)
{
    if (current.bank == bank || bank < 0 || bank >= NUM_BANKS)
        return false;

    current.bank = bank;
    current.preset = 0;
    hostLoadCurrent();
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// convenience method for quickly switching to another preset within the current bank

bool HostConnector::switchPreset(const int preset)
{
    if (current.preset == preset || preset < 0 || preset >= NUM_PRESETS_PER_BANK)
        return false;

    current.preset = preset;
    hostLoadCurrent();
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// load host state as saved in the `current` struct

void HostConnector::hostLoadCurrent()
{
    host.remove(-1);

    const auto& bankdata(current.banks[current.bank]);
    const auto& presetdata(bankdata.presets[current.preset]);

    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        const auto& blockdata(presetdata.blocks[b]);
        if (isNullURI(blockdata.uri))
            continue;
        host.add(blockdata.uri.c_str(), b);

        for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        {
            const auto& parameterdata(blockdata.parameters[p]);
            if (isNullURI(parameterdata.symbol))
                continue;
            host.param_set(b, parameterdata.symbol.c_str(), parameterdata.value);
        }
    }

    hostConnectBetweenBlocks();
}

// --------------------------------------------------------------------------------------------------------------------
// common function to connect all the blocks as needed
// TODO cleanup duplicated code with function below

void HostConnector::hostConnectBetweenBlocks()
{
    const auto& bankdata(current.banks[current.bank]);
    const auto& presetdata(bankdata.presets[current.preset]);

    bool loaded[NUM_BLOCKS_PER_PRESET];
    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        loaded[b] = !isNullURI(presetdata.blocks[b].uri);

    // first plugin
    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        if (! loaded[b])
            continue;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
        {
            int srci = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                    continue;

                ++srci;
                const std::string origin(format("system:capture_%d", srci));
                const std::string target(format("effect_%d:%s", b, plugin->ports[i].symbol.c_str()));
                host.connect(origin.c_str(), target.c_str());
            }
        }

        break;
    }

    // last plugin
    for (int b = NUM_BLOCKS_PER_PRESET - 1; b >= 0; --b)
    {
        if (! loaded[b])
            continue;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
        {
            int dsti = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                    continue;

                ++dsti;
                const std::string origin(format("effect_%d:%s", b, plugin->ports[i].symbol.c_str()));
                const std::string target(format("mod-monitor:in_%d", dsti));
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

                        const std::string origin(format("effect_%d:%s", b1, plugin1->ports[i].symbol.c_str()));
                        const std::string target(format("effect_%d:%s", b2, plugin2->ports[j].symbol.c_str()));
                        host.connect(origin.c_str(), target.c_str());
                    }
                }
            }

            break;
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

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
        {
            int srci = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                    continue;

                ++srci;
                const std::string origin(format("system:capture_%d", srci));
                const std::string target(format("effect_%d:%s", b, plugin->ports[i].symbol.c_str()));
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

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(presetdata.blocks[b].uri.c_str()))
        {
            int dsti = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                    continue;

                ++dsti;
                const std::string origin(format("effect_%d:%s", b, plugin->ports[i].symbol.c_str()));
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

                        const std::string origin(format("effect_%d:%s", b1, plugin1->ports[i].symbol.c_str()));
                        const std::string target(format("effect_%d:%s", b2, plugin2->ports[j].symbol.c_str()));
                        host.disconnect(origin.c_str(), target.c_str());
                    }
                }
            }

            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------
