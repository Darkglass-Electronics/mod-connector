// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"
#include "utils.hpp"

#include <cstring>

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
// load host state as saved in the `current` struct

void HostConnector::loadCurrent()
{
    host.remove(-1);

    const auto& bankdata(current.banks[current.bank]);
    const auto& presetdata(bankdata.presets[current.preset]);

    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        const auto& blockdata(presetdata.blocks[b]);
        if (blockdata.uri.empty() || blockdata.uri == "-")
            continue;
        host.add(blockdata.uri.c_str(), b);

        for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        {
            const auto& parameterdata(blockdata.parameters[p]);
            if (parameterdata.symbol.empty() || parameterdata.symbol == "-")
                continue;
            host.param_set(b, parameterdata.symbol.c_str(), parameterdata.value);
        }
    }

    hostConnectBetweenBlocks();
}

// --------------------------------------------------------------------------------------------------------------------
// replace a block with another lv2 plugin (referenced by its URI)
// passing null or empty string as the URI means clearing the block

void HostConnector::replaceBlock(const int bank, const int preset, const int block, const char* const uri)
{
    if (bank < 0 || bank >= NUM_BANKS)
        return;
    if (preset < 0 || preset >= NUM_PRESETS_PER_BANK)
        return;
    if (block < 0 || block >= NUM_BLOCKS_PER_PRESET)
        return;

    auto& blockdata(current.banks[bank].presets[preset].blocks[block]);
    const bool islive = current.bank == bank && current.preset == preset;

    if (uri != nullptr && *uri != '\0' && std::strcmp(uri, "-") != 0)
    {
        blockdata.uri = uri;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(uri))
        {
            int p = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & Lv2PortIsControl) == 0)
                    continue;
                if (plugin->ports[i].flags & (Lv2PortIsOutput|Lv2ParameterHidden))
                    continue;

                blockdata.parameters[p] = {
                    .symbol = plugin->ports[i].symbol,
                    .value = plugin->ports[i].def,
                };

                if (++p >= MAX_PARAMS_PER_BLOCK)
                    break;
            }

            for (; p < MAX_PARAMS_PER_BLOCK; ++p)
                blockdata.parameters[p] = {};
        }
        else
        {
            for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                blockdata.parameters[p] = {};
        }

        if (islive)
        {
            // TODO mod-host replace command, reducing 1 roundtrip
            host.remove(block);

            if (host.add(uri, block))
                printf("DEBUG: block %d loaded plugin %s\n", block, uri);
            else
                printf("DEBUG: block %d failed loaded plugin %s: %s\n", block, uri, host.last_error.c_str());

            hostDisconnectForNewBlock(block);
        }
    }
    else
    {
        if (islive)
            host.remove(block);

        blockdata = {};
    }
}

// --------------------------------------------------------------------------------------------------------------------
// convenience call to replace a block for the current preset

void HostConnector::replaceBlockInActivePreset(const int block, const char* const uri)
{
    replaceBlock(current.bank, current.preset, block, uri);
}

// --------------------------------------------------------------------------------------------------------------------
// convenience method for quickly switching to another bank
// NOTE resets active preset to 0

void HostConnector::switchBank(const int bank)
{
    if (current.bank == bank || bank < 0 || bank >= NUM_BANKS)
        return;

    current.bank = bank;
    current.preset = 0;
    loadCurrent();
}

// --------------------------------------------------------------------------------------------------------------------
// convenience method for quickly switching to another preset within the current bank

void HostConnector::switchPreset(const int preset)
{
    if (current.preset == preset || preset < 0 || preset >= NUM_PRESETS_PER_BANK)
        return;

    current.preset = preset;
    loadCurrent();
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
        loaded[b] = !presetdata.blocks[b].uri.empty() && presetdata.blocks[b].uri != "-";

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
        loaded[b] = !presetdata.blocks[b].uri.empty() && presetdata.blocks[b].uri != "-";
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
