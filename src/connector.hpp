// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "host.hpp"
#include "lv2.hpp"

// --------------------------------------------------------------------------------------------------------------------
// default configuration

#ifndef MAX_BANKS
#define MAX_BANKS 99
#endif

#ifndef NUM_PRESETS_PER_BANK
#define NUM_PRESETS_PER_BANK 3
#endif

#ifndef NUM_BLOCKS_PER_PRESET
#define NUM_BLOCKS_PER_PRESET 24
#endif

#ifndef MAX_PARAMS_PER_BLOCK
#define MAX_PARAMS_PER_BLOCK 60
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
        bool dirty = false;
        std::string filename;
    };

protected:
    // connection to mod-host, handled internally
    Host _host;

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
    // returns current state if preset is currently active, otherwise the default preset state
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

    // clear current preset
    void clearCurrentPreset();

    // return average dsp load
    float dspLoad();

    // poll for host updates (e.g. MIDI-mapped parameter changes, tempo changes)
    // NOTE make sure to call `requestHostUpdates()` after handling all updates
    void pollHostUpdates(Host::FeedbackCallback* callback);

    // request more host updates
    void requestHostUpdates();

    // set a block parameter value
    // NOTE value must already be sanitized!
    void setBlockParameterValue(uint8_t block, uint8_t paramIndex, float value);

    // set a block property
    void setBlockProperty(uint8_t block, const char* uri, const char* value);

protected:
    // load host state as stored in the `current` struct
    // also preloads the other presets in the bank
    void hostClearAndLoadCurrentBank();

    // common function to connect all the blocks as needed
    // TODO cleanup duplicated code with function below
    void hostConnectBetweenBlocks();

    // disconnect everything around the new plugin, to prevent double connections
    // TODO cleanup duplicated code with function above
    // FIXME this logic can be made much better, but this is for now just a testing tool anyhow
    void hostDisconnectForNewBlock(uint8_t blockidi);

    // internal feedback handling, for updating parameter values
    void hostFeedbackCallback(const HostFeedbackData& data) override;
};

// --------------------------------------------------------------------------------------------------------------------
