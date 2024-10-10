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

                block.bindingSymbol.clear();
                block.enabled = true;
                block.uri = uri;

                block.meta.bindingIndex = -1;
                block.meta.name = plugin->name;

                if (jblock.contains("enabled"))
                    block.enabled = jblock["enabled"].get<bool>();

                // parameters are always filled from lv2 metadata first, then overriden with json data
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
    const Host::NonBlockingScope hnbs(_host);

    for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        const int instance = 100 * _current.preset + b;

        if (! isNullURI(_current.blocks[b].uri))
        {
            _current.dirty = true;
            _host.remove(instance);
        }

        resetBlock(_current.blocks[b]);
    }

    hostConnectBetweenBlocks();
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::enableBlock(const uint8_t block, const bool enable)
{
    if (block >= NUM_BLOCKS_PER_PRESET)
    {
        fprintf(stderr, "HostConnector::enableBlock(%u, %d) - out of bounds block, rejected\n", block, enable);
        return false;
    }

    const int instance = 100 * current.preset + block;
    HostConnector::Block& blockdata(_presets[current.preset].blocks[block]);

    if (isNullURI(blockdata.uri))
    {
        fprintf(stderr, "HostConnector::enableBlock(%u, %d) - block not in use, rejected\n", block, enable);
        return false;
    }

    blockdata.enabled = enable;

    _current.dirty = true;
    _host.bypass(instance, !enable);
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

    const Host::NonBlockingScope hnbs(_host);

    HostConnector::Preset& preset(_presets[_current.preset]);

    // NOTE this is a very crude quick implementation
    // just removing all plugins and adding them again after reordering
    // it is not meant to be the final implementation, just something quick for experimentation
    _host.remove(-1);

    // moving block backwards to the left
    // a b c d e! f
    // a b c e! d f
    // a b e! c d f
    // a e! b c d f
    if (block > dest)
    {
        for (int i = block; i > dest; --i)
            std::swap(preset.blocks[i], preset.blocks[i - 1]);
    }
    // moving block forward to the right
    // a b! c d e f
    // a c b! d e f
    // a c d b! e f
    // a c d e b! f
    else
    {
        for (int i = block; i < dest; ++i)
            std::swap(preset.blocks[i], preset.blocks[i + 1]);
    }

    hostClearAndLoadCurrentBank();

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

    const int instance = 100 * current.preset + block;
    HostConnector::Block& blockdata(_presets[_current.preset].blocks[block]);

    const Host::NonBlockingScope hnbs(_host);

    if (! isNullURI(uri))
    {
        const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(uri);
        if (plugin == nullptr)
        {
            fprintf(stderr, "HostConnector::replaceBlock(%u, %s) - plugin not available, rejected\n", block, uri);
            return false;
        }

        // we only do changes after verifying that the requested plugin exists
        _host.remove(instance);

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

        if (_host.add(uri, instance))
        {
            printf("DEBUG: block %u loaded plugin %s\n", block, uri);
        }
        else
        {
            printf("DEBUG: block %u failed to load plugin %s: %s\n", block, uri, _host.last_error.c_str());
            resetBlock(blockdata);
        }

        hostDisconnectForNewBlock(block);
    }
    else
    {
        _host.remove(instance);
        resetBlock(blockdata);
    }

    hostConnectBetweenBlocks();

    _current.dirty = true;
    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool HostConnector::switchPreset(const uint8_t preset)
{
    if (_current.preset == preset || preset >= NUM_PRESETS_PER_BANK)
        return false;

    const int oldpreset = _current.preset;

    static_cast<Preset&>(_current) = _presets[preset];
    _current.preset = preset;
    _current.dirty = false;

    const Host::NonBlockingScope hnbs(_host);

    // step 1: fade out
    // TODO

    // step 2: deactivate all plugins in old preset
#if 1
    _host.activate(100 * oldpreset, 100 * oldpreset + NUM_BLOCKS_PER_PRESET, 0);
#else
    {
        const HostConnector::Preset& presetdata(bankdata.presets[oldpreset]);

        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const HostConnector::Block& blockdata(presetdata.blocks[b]);
            if (isNullURI(blockdata.uri))
                continue;

            const int instance = 100 * oldpreset + b;
            _host.activate(instance, 0);
        }
    }
#endif

    // step 3: activate all plugins in new preset
#if 1
    _host.activate(100 * preset, 100 * preset + NUM_BLOCKS_PER_PRESET, 1);
#else
    {
        const HostConnector::Preset& presetdata(bankdata.presets[preset]);

        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const HostConnector::Block& blockdata(presetdata.blocks[b]);
            if (isNullURI(blockdata.uri))
                continue;

            const int instance = 100 * preset + b;
            _host.activate(instance, 1);
        }
    }
#endif

    hostConnectBetweenBlocks();

    // step 4: fade in
    // TODO

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

void HostConnector::setBlockParameterValue(const uint8_t block, const uint8_t paramIndex, const float value)
{
    if (block >= NUM_BLOCKS_PER_PRESET)
        return;
    if (paramIndex >= MAX_PARAMS_PER_BLOCK)
        return;

    const int instance = 100 * _current.preset + block;

    HostConnector::Block& blockdata(_presets[_current.preset].blocks[block]);
    if (isNullURI(blockdata.uri))
        return;

    HostConnector::Parameter& paramdata(blockdata.parameters[paramIndex]);
    if (isNullURI(paramdata.symbol))
        return;

    paramdata.value = value;
    _current.dirty = true;
    _host.param_set(instance, paramdata.symbol.c_str(), value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::setBlockProperty(const uint8_t block, const char* const uri, const char* const value)
{
    if (block >= NUM_BLOCKS_PER_PRESET)
        return;
    if (uri == nullptr || *uri == '\0')
        return;
    if (value == nullptr || *value == '\0')
        return;

    const int instance = 100 * current.preset + block;
    const HostConnector::Block& blockdata(_presets[_current.preset].blocks[block]);

    if (isNullURI(blockdata.uri))
        return;

    _current.dirty = true;
    _host.patch_set(instance, uri, value);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostClearAndLoadCurrentBank()
{
    _host.feature_enable("processing", false);
    _host.remove(-1);

    for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
    {
        const bool active = _current.preset == pr;
        const HostConnector::Preset& presetdata = active ? _current : _presets[pr];

        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            const HostConnector::Block& blockdata(presetdata.blocks[b]);
            if (isNullURI(blockdata.uri))
                continue;

            const int instance = 100 * pr + b;

            if (active)
                _host.add(blockdata.uri.c_str(), instance);
            else
                _host.preload(blockdata.uri.c_str(), instance);

            if (!blockdata.enabled)
                _host.bypass(instance, true);

            for (int p = 0; p < MAX_PARAMS_PER_BLOCK; ++p)
            {
                const HostConnector::Parameter& parameterdata(blockdata.parameters[p]);
                if (isNullURI(parameterdata.symbol))
                    continue;
                _host.param_set(instance, parameterdata.symbol.c_str(), parameterdata.value);
            }
        }
    }

    hostConnectBetweenBlocks();
    _host.feature_enable("processing", true);
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostConnectBetweenBlocks()
{
    // for (int pr = 0; pr < NUM_PRESETS_PER_BANK; ++pr)
    {
        const int pr = _current.preset;

        bool loaded[NUM_BLOCKS_PER_PRESET];
        int numLoaded = 0;
        bool hasMeter = false;
        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            if (_current.blocks[b].uri == "http://gareus.org/oss/lv2/modspectre")
                hasMeter = true;
            else if ((loaded[b] = !isNullURI(_current.blocks[b].uri)))
                ++numLoaded;
        }

        if (numLoaded == 0)
        {
            // direct connections
            _host.connect("system:capture_1", "mod-monitor:in_1");
            _host.connect("system:capture_2", "mod-monitor:in_2");
            // connect to extra inputs just in case
            _host.connect("system:capture_3", "mod-monitor:in_1");
            _host.connect("system:capture_4", "mod-monitor:in_2");

            if (hasMeter)
            {
                _host.connect("system:capture_1", "effect_23:in");
                _host.connect("system:capture_3", "effect_23:in");
            }
            return;
        }

        // first plugin
        for (int b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            if (! loaded[b])
                continue;

            const int instance = 100 * pr + b;

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.blocks[b].uri.c_str()))
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
                    _host.connect(origin.c_str(), target.c_str());
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
                        _host.connect(origin.c_str(), target.c_str());
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

            if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.blocks[b].uri.c_str()))
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
                    _host.connect(origin.c_str(), target.c_str());

                    if (hasMeter)
                        _host.connect(origin.c_str(), "effect_23:in");
                }

                // connect to stereo output if chain is mono
                if (dsti == 1)
                {
                    const std::string origin(format("effect_%d:%s", instance, plugin->ports[lasti].symbol.c_str()));
                    const std::string target(format("mod-monitor:in_%d", dsti + 1));
                    _host.connect(origin.c_str(), target.c_str());
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

                const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(_current.blocks[b1].uri.c_str());
                const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(_current.blocks[b2].uri.c_str());

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
                            _host.connect(origin.c_str(), target.c_str());
                        }
                    }
                }

                break;
            }
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostDisconnectForNewBlock(const uint8_t blockidi)
{
    bool loaded[NUM_BLOCKS_PER_PRESET];
    for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        loaded[b] = !isNullURI(_current.blocks[b].uri);
    loaded[blockidi] = false;

    // direct connections
    _host.disconnect("system:capture_1", "mod-monitor:in_1");
    _host.disconnect("system:capture_2", "mod-monitor:in_2");

    // first plugin
    for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
    {
        if (! loaded[b])
            continue;

        const int instance = 100 * current.preset + b;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.blocks[b].uri.c_str()))
        {
            int srci = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != Lv2PortIsAudio)
                    continue;

                ++srci;
                const std::string origin(format("system:capture_%d", srci));
                const std::string target(format("effect_%d:%s", instance, plugin->ports[i].symbol.c_str()));
                _host.disconnect(origin.c_str(), target.c_str());
            }
        }

        break;
    }

    // last plugin
    for (int8_t b = NUM_BLOCKS_PER_PRESET - 1; b >= 0; --b)
    {
        if (! loaded[b])
            continue;

        const int instance = 100 * current.preset + b;

        if (const Lv2Plugin* const plugin = lv2world.get_plugin_by_uri(_current.blocks[b].uri.c_str()))
        {
            int dsti = 0;
            for (size_t i = 0; i < plugin->ports.size(); ++i)
            {
                if ((plugin->ports[i].flags & (Lv2PortIsAudio|Lv2PortIsOutput)) != (Lv2PortIsAudio|Lv2PortIsOutput))
                    continue;

                ++dsti;
                const std::string origin(format("effect_%d:%s", instance, plugin->ports[i].symbol.c_str()));
                const std::string target(format("mod-monitor:in_%d", dsti));
                _host.disconnect(origin.c_str(), target.c_str());
            }
        }

        break;
    }

    // between plugins
    for (uint8_t b1 = 0; b1 < NUM_BLOCKS_PER_PRESET - 1; ++b1)
    {
        if (! loaded[b1])
            continue;

        for (uint8_t b2 = b1 + 1; b2 < NUM_BLOCKS_PER_PRESET; ++b2)
        {
            if (! loaded[b2])
                continue;

            const int instance1 = 100 * current.preset + b1;
            const int instance2 = 100 * current.preset + b2;

            const Lv2Plugin* const plugin1 = lv2world.get_plugin_by_uri(_current.blocks[b1].uri.c_str());
            const Lv2Plugin* const plugin2 = lv2world.get_plugin_by_uri(_current.blocks[b2].uri.c_str());

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
                        _host.disconnect(origin.c_str(), target.c_str());
                    }
                }
            }

            break;
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostConnector::hostFeedbackCallback(const HostFeedbackData& data)
{
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
                Block& blockdata = _presets[preset].blocks[block];

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

    _callback->hostFeedbackCallback(data);
}

// --------------------------------------------------------------------------------------------------------------------
