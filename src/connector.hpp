// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "host.hpp"
#include "lv2.hpp"

#include <cassert>

// --------------------------------------------------------------------------------------------------------------------
// default configuration

#ifndef MAX_BANKS
#define MAX_BANKS 99
#endif

#ifndef NUM_PRESETS_PER_BANK
#define NUM_PRESETS_PER_BANK 3
#endif

#ifndef NUM_BLOCKS_PER_PRESET
#define NUM_BLOCKS_PER_PRESET 6
#endif

#ifndef MAX_PARAMS_PER_BLOCK
#define MAX_PARAMS_PER_BLOCK 60
#endif

#ifndef JACK_CAPTURE_PORT_1
#define JACK_CAPTURE_PORT_1 "system:capture_1"
#endif

#ifndef JACK_CAPTURE_PORT_2
#define JACK_CAPTURE_PORT_2 "system:capture_2"
#endif

#ifndef JACK_PLAYBACK_PORT_1
#define JACK_PLAYBACK_PORT_1 "mod-monitor:in_1"
#endif

#ifndef JACK_PLAYBACK_PORT_2
#define JACK_PLAYBACK_PORT_2 "mod-monitor:in_2"
#endif

// --------------------------------------------------------------------------------------------------------------------
// check valid configuration

#if MAX_BANKS > UINT8_MAX
#error MAX_BANKS > UINT8_MAX, need to adjust data types
#endif

#if NUM_PRESETS_PER_BANK > UINT8_MAX
#error NUM_PRESETS_PER_BANK > UINT8_MAX, need to adjust data types
#endif

#if NUM_BLOCKS_PER_PRESET > UINT8_MAX
#error NUM_BLOCKS_PER_PRESET > UINT8_MAX, need to adjust data types
#endif

#if MAX_PARAMS_PER_BLOCK > UINT8_MAX
#error MAX_PARAMS_PER_BLOCK > UINT8_MAX, need to adjust data types
#endif

// --------------------------------------------------------------------------------------------------------------------

static constexpr const uint16_t kMaxInstances = NUM_PRESETS_PER_BANK * (NUM_BLOCKS_PER_PRESET * 4);
static_assert(kMaxInstances < 9990, "maximum amount of instances is bigger than what mod-host can do");

struct HostInstanceMapper {
    struct BlockPair {
        uint16_t id;
        uint16_t pair;
    };

    struct {
        struct {
            BlockPair blocks[NUM_BLOCKS_PER_PRESET];
        } presets[NUM_PRESETS_PER_BANK];
    } map;

    bool used[kMaxInstances];

    HostInstanceMapper()
    {
        reset();
    }

    uint16_t add(const uint8_t preset, const uint8_t block)
    {
        assert(map.presets[preset].blocks[block].id == UINT16_MAX);
        assert(map.presets[preset].blocks[block].pair == UINT16_MAX);

        for (uint16_t id = 0; id < kMaxInstances; ++id)
        {
            if (used[id])
                continue;

            used[id] = true;
            map.presets[preset].blocks[block].id = id;

            return id;
        }

        // something went really wrong if we reach this, abort
        abort();
    }

    uint16_t pair(const uint8_t preset, const uint8_t block)
    {
        assert(map.presets[preset].blocks[block].id != UINT16_MAX);
        assert(map.presets[preset].blocks[block].pair == UINT16_MAX);

        for (uint16_t id2 = 0; id2 < kMaxInstances; ++id2)
        {
            if (used[id2])
                continue;

            used[id2] = true;
            map.presets[preset].blocks[block].pair = id2;

            return id2;
        }

        // something went really wrong if we reach this, abort
        abort();
    }

    BlockPair remove(const uint8_t preset, const uint8_t block)
    {
        assert(map.presets[preset].blocks[block].id != UINT16_MAX);
        assert(map.presets[preset].blocks[block].pair != UINT16_MAX);

        const uint16_t id = map.presets[preset].blocks[block].id;
        const uint16_t id2 = map.presets[preset].blocks[block].pair;
        map.presets[preset].blocks[block].id = map.presets[preset].blocks[block].pair = UINT16_MAX;

        if (id < kMaxInstances)
            used[id] = false;

        if (id2 < kMaxInstances)
            used[id2] = false;

        return { id, id2 };
    }

    BlockPair get(const uint8_t preset, const uint8_t block) const
    {
        return map.presets[preset].blocks[block];
    }

    void reset()
    {
        for (uint8_t p = 0; p < NUM_PRESETS_PER_BANK; ++p)
            for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
                map.presets[p].blocks[b].id = map.presets[p].blocks[b].pair = UINT16_MAX;

        for (uint16_t id = 0; id < kMaxInstances; ++id)
            used[id] = false;
    }
};

// --------------------------------------------------------------------------------------------------------------------

struct HostConnector : Host::FeedbackCallback {
    struct Parameter {
        std::string symbol;
        float value;
        struct {
            // convenience meta-data, not stored in json state
            uint32_t flags;
            float def, min, max;
            std::string name;
            std::string unit;
            std::vector<Lv2ScalePoint> scalePoints;
        } meta;
    };

    struct Block {
        bool enabled = false;
        std::string bindingSymbol;
        std::string uri;
        struct {
            // convenience meta-data, not stored in json state
            int bindingIndex = -1;
            bool isMonoIn = false;
            bool isStereoOut = false;
            std::string name;
        } meta;
        std::vector<Parameter> parameters;
    };

    struct Preset {
        std::string name;
        std::vector<Block> blocks;
    };

    struct Current : Preset {
        uint8_t preset = 0;
        uint8_t numLoadedPlugins = 0;
        bool dirty = false;
        std::string filename;
    };

protected:
    // connection to mod-host, handled internally
    Host _host;

    // internal host instance mapper
    HostInstanceMapper _mapper;

    // internal current preset state
    Current _current;

    // default state for each preset
    Preset _presets[NUM_PRESETS_PER_BANK];

    // current feedback callback
    FeedbackCallback* _callback = nullptr;

public:
    // lv2 world for getting information about plugins
    const Lv2World lv2world;

    // whether the host connection is working
    bool ok = false;

    // public and read-only current preset state
    const Current& current = _current;

    // constructor, initializes connection to mod-host and sets `ok` to true if successful
    HostConnector();

    // try to reconnect host if it previously failed
    bool reconnect();

    // get the preset at @a index
    // returns current state if preset is currently active, otherwise the preset state from the current bank
    const Preset& getCurrentPreset(uint8_t index) const;

    // load bank from a file and store the first preset in the `current` struct
    // automatically calls loadCurrent() if the file contains valid state, otherwise does nothing
    // returning false means the current chain was unchanged
    bool loadBankFromFile(const char* filename);

    // save bank state as stored in the `current` struct
    // a bank must have been loaded or saved to a file before, so that `current.filename` is valid
    bool saveBank();

    // save bank state as stored in the `current` struct into a new file
    bool saveBankToFile(const char* filename);

    // clear current preset
    // sets dirty flag if any blocks were removed
    void clearCurrentPreset();

    // enable or disable/bypass a block
    // returning false means the block was unchanged
    bool enableBlock(uint8_t block, bool enable);

    // reorder a block into a new position
    // returning false means the current chain was unchanged
    bool reorderBlock(uint8_t block, uint8_t dest);

    // replace a block with another lv2 plugin (referenced by its URI)
    // passing null or empty string as the URI means clearing the block
    // returning false means the block was unchanged
    bool replaceBlock(uint8_t block, const char* uri);

    // switch to another preset within the current bank
    // returning false means the current chain was unchanged
    bool switchPreset(uint8_t preset);

    // WIP details below this point

    // return average dsp load
    float dspLoad();

    // poll for host updates (e.g. MIDI-mapped parameter changes, tempo changes)
    // NOTE make sure to call `requestHostUpdates()` after handling all updates
    void pollHostUpdates(Host::FeedbackCallback* callback);

    // request more host updates
    void requestHostUpdates();

    // set a block parameter value
    // NOTE value must already be sanitized!
    void setBlockParameter(uint8_t block, uint8_t paramIndex, float value);

    // set a block property
    void setBlockProperty(uint8_t block, const char* uri, const char* value);

protected:
    // load host state as stored in the `current` struct
    // also preloads the other presets in the bank
    void hostClearAndLoadCurrentBank();

    void hostConnectAll(uint8_t blockStart = 0, uint8_t blockEnd = NUM_BLOCKS_PER_PRESET - 1);
    void hostConnectBlockToBlock(uint8_t blockA, uint8_t blockB);
    void hostConnectBlockToSystemInput(uint8_t block);
    void hostConnectBlockToSystemOutput(uint8_t block);

    void hostDisconnectAll();
    void hostDisconnectAllBlockInputs(uint8_t block);
    void hostDisconnectAllBlockOutputs(uint8_t block);

    void hostRemoveInstanceForBlock(uint8_t block);

private:
    void hostConnectSystemInputAction(uint8_t block, bool connect);
    void hostConnectSystemOutputAction(uint8_t block, bool connect);
    void hostDisconnectBlockAction(uint8_t block, bool outputs);

    // internal feedback handling, for updating parameter values
    void hostFeedbackCallback(const HostFeedbackData& data) override;
};

// --------------------------------------------------------------------------------------------------------------------
