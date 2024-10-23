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

// --------------------------------------------------------------------------------------------------------------------

static void resetParam(HostConnector::Parameter& param)
{
    param = {};
    param.meta.max = 1.f;
}

static void resetBlock(HostConnector::Block& block)
{
    block = {};
    block.meta.bindingIndex = -1;
    block.parameters.resize(MAX_PARAMS_PER_BLOCK);

    for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
        resetParam(block.parameters[p]);
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
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::reconnect()
{
    return _host.reconnect();
}

// --------------------------------------------------------------------------------------------------------------------

const HostConnector::Preset& HostConnector::getCurrentPreset(const uint8_t preset) const
{
    return _current.preset != preset && preset < NUM_PRESETS_PER_BANK ? _presets[preset] : _current;
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

    uint8_t numLoadedPlugins = 0;

    do {
        if (! j.contains("presets"))
        {
            printf("HostConnector::loadBankFromFile: bank does not include presets, loading empty\n");
            for (uint8_t pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
                resetPreset(_presets[pr]);
            break;
        }

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
                Block& block = preset.blocks[bl];
                const std::string jblockid = std::to_string(bl + 1);

                if (! jblocks.contains(jblockid))
                {
                    resetBlock(block);
                    continue;
                }

                auto& jblock = jblocks[jblockid];
                if (! jblock.contains("uri"))
                {
                    printf("HostConnector::loadBankFromFile: preset #%u / block #%u does not include uri, loading empty\n",
                           pr + 1, bl + 1);
                    resetBlock(block);
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
                    resetBlock(block);
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
                    resetBlock(block);
                    continue;
                }

                block.bindingSymbol.clear();
                block.enabled = true;
                block.uri = uri;

                block.meta.bindingIndex = -1;
                block.meta.isMonoIn = numInputs == 1;
                block.meta.isStereoOut = numOutputs == 2;
                block.meta.name = plugin->name;

                if (jblock.contains("enabled"))
                    block.enabled = jblock["enabled"].get<bool>();

                if (bl == 0)
                    ++numLoadedPlugins;

                // parameters are always filled from lv2 metadata first, then overriden with json data
                uint8_t numParams = 0;
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

                    HostConnector::Parameter& param = block.parameters[numParams];

                    param.symbol = plugin->ports[i].symbol;
                    param.value = plugin->ports[i].def;

                    param.meta.flags = plugin->ports[i].flags;
                    param.meta.def = plugin->ports[i].def;
                    param.meta.min = plugin->ports[i].min;
                    param.meta.max = plugin->ports[i].max;
                    param.meta.name = plugin->ports[i].name;
                    param.meta.unit = plugin->ports[i].unit;
                    param.meta.scalePoints = plugin->ports[i].scalePoints;

                    symbolToIndexMap[param.symbol] = numParams++;
                }

                for (uint8_t p = numParams; p < MAX_PARAMS_PER_BLOCK; ++p)
                    resetParam(block.parameters[p]);

                try {
                    const std::string binding = jblock["binding"].get<std::string>();

                    for (uint8_t p = 0; p < numParams; ++p)
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

                    block.parameters[p].value = std::max(block.parameters[p].meta.min,
                                                         std::min<float>(block.parameters[p].meta.max,
                                                                         jparam["value"].get<double>()));
                }
            }
        }
    } while(false);

    // always start with the first preset
    static_cast<Preset&>(_current) = _presets[0];
    _current.preset = 0;
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
    _presets[_current.preset] = static_cast<Preset&>(_current);

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
                { "name", preset.name },
                { "blocks", nlohmann::json::object({}) },
            });

            auto& jblocks = jpreset["blocks"];

            for (uint8_t bl = 0; bl < NUM_BLOCKS_PER_PRESET; ++bl)
            {
                const HostConnector::Block& block = preset.blocks[bl];

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
                for (uint8_t p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    const Parameter& param = block.parameters[p];

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
    _host.feature_enable("processing", false);

    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        if (! isNullURI(_current.blocks[b].uri))
            hostRemoveInstanceForBlock(b);

        resetBlock(_current.blocks[b]);
    }

    _current.numLoadedPlugins = 0;
    _current.dirty = true;

    hostConnectAll();
    _host.feature_enable("processing", true);
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::enableBlock(const uint8_t block, const bool enable)
{
    if (block >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::enableBlock(%u, %d) - out of bounds block, rejected\n", block, enable);
        return false;
    }

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
    if (block >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - out of bounds block, rejected\n", block, dest);
        return false;
    }
    if (dest >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::reorderBlock(%u, %u) - out of bounds dest, rejected\n", block, dest);
        return false;
    }
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
            blockdata.bindingSymbol.clear();
            blockdata.enabled = true;
            blockdata.uri = uri;

            blockdata.meta.bindingIndex = -1;
            blockdata.meta.isMonoIn = numInputs == 1;
            blockdata.meta.isStereoOut = numOutputs == 2;
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
                        .def = plugin->ports[i].def,
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

            // TODO take care to handle new stereo plugin before mono chain
            // TODO take care to disconnect mono -> stereo?

            if (before != NUM_BLOCKS_PER_PRESET && after != NUM_BLOCKS_PER_PRESET)
            {
                hostDisconnectAllBlockInputs(after);
                hostDisconnectAllBlockOutputs(before);
                hostConnectBlockToBlock(before, block);
                hostConnectBlockToBlock(block, after);
            }
            else if (before != NUM_BLOCKS_PER_PRESET)
            {
                hostDisconnectAllBlockOutputs(before);
                hostConnectBlockToSystemOutput(block);
                hostConnectBlockToBlock(before, block);
            }
            else if (after != NUM_BLOCKS_PER_PRESET)
            {
                hostDisconnectAllBlockInputs(after);
                hostConnectBlockToSystemInput(block);
                hostConnectBlockToBlock(block, after);
            }
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

            // find surrounding plugins
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

            uint8_t end = NUM_BLOCKS_PER_PRESET - 1;
            if (block != NUM_BLOCKS_PER_PRESET - 1)
            {
                for (uint8_t b = block + 1; b < NUM_BLOCKS_PER_PRESET; ++b)
                {
                    if (loaded[b])
                    {
                        end = b;
                        break;
                    }
                }
            }

            // TODO take care to disconnect mono -> stereo
            hostEnsureStereoChain(start, end);
            hostConnectAll(start, end);
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
    _current.dirty = false;
    _current.numLoadedPlugins = 0;

    // scope for fade-out, old deactivate, new activate, fade-in
    {
        const Host::NonBlockingScope hnbs(_host);

        // step 1: fade out
        // TODO
        _host.feature_enable("processing", false);

        // step 2: disconnect and deactivate all plugins in old preset
        // NOTE not removing plugins, done after processing is reenabled
        if (old.numLoadedPlugins == 0)
        {
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
        // TODO
        _host.feature_enable("processing", true);
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

void HostConnector::pollHostUpdates(Host::FeedbackCallback* const callback)
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

    HostConnector::Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return;

    const HostInstanceMapper::BlockPair bp = _mapper.get(_current.preset, block);
    if (bp.id == kMaxHostInstances)
        return;

    paramdata.value = value;
    _current.dirty = true;
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

    for (size_t a = 0, b = 0; b < pluginB->ports.size(); ++b)
    {
        if ((pluginB->ports[b].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
            continue;

        for (; a < pluginA->ports.size(); ++a)
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
            break;
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
    _host.feature_enable("processing", false);
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

    hostConnectAll();
    _host.feature_enable("processing", true);
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
            if (oldDualmono)
            {
                _host.remove(_mapper.remove_pair(_current.preset, b));
            }
            else
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
                }
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
#if 0
    // TODO
    if (data.type == HostFeedbackData::kFeedbackParameterSet)
    {
        const int preset = data.paramSet.effect_id / 100;
        const int block = data.paramSet.effect_id % 100;

        if (preset >= 0 && preset < NUM_PRESETS_PER_BANK && block >= 0 && block < NUM_BLOCKS_PER_PRESET)
        {
            if (data.paramSet.symbol[0] == ':')
            {
                // special mod-host values here
            }
            else
            {
                Block& blockdata = _current.blocks[block];

                for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
                {
                    if (isNullURI(blockdata.parameters[p].symbol))
                        break;

                    if (blockdata.parameters[p].symbol == data.paramSet.symbol)
                    {
                        _current.dirty = true;
                        blockdata.parameters[p].value = data.paramSet.value;
                        break;
                    }
                }
            }
        }
    }
#endif

    _callback->hostFeedbackCallback(data);
}

// --------------------------------------------------------------------------------------------------------------------
