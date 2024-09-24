// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "host.hpp"
#include "lv2.hpp"

// --------------------------------------------------------------------------------------------------------------------
// default configuration

#ifndef NUM_BANKS
#define NUM_BANKS 6
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

    struct Bank {
        std::string name;
        std::vector<Preset> presets;
    };

    struct Current {
        int bank = 0;
        int preset = 0; // NOTE resets to 0 on bank change
        std::vector<Bank> banks;
    };

protected:
    // connection to mod-host, handled internally
    Host _host;

    // internal current state
    Current _current;

    // current feedback callback
    FeedbackCallback* _callback = nullptr;

public:
    // lv2 world for getting information about plugins
    const Lv2World lv2world;

    // whether the host connection is working
    bool ok = false;

    // public and read-only current state, including all presets and banks
    const Current& current = _current;

    // shortcut to active bank
    inline const Bank& currentBank() const
    {
        return current.banks[current.bank];
    }

    // shortcut to active preset
    inline const Preset& currentPreset() const
    {
        return current.banks[current.bank].presets[current.preset];
    }

    // constructor, initializes connection to mod-host and sets `ok` to true if successful
    HostConnector();

    // load state from a file and store it in the `current` struct
    // automatically calls loadCurrent() if the file contains valid state, otherwise does nothing
    // returning false means the current chain was unchanged
    bool loadStateFromFile(const char* filename);

    // save host state as stored in the `current` struct into a file
    bool saveStateToFile(const char* filename) const;

    // enable or disable/bypass a block
    // returning false means the block was unchanged
    bool enableBlock(int block, bool enable);

    // reorder a block into a new position
    // returning false means the current chain was unchanged
    bool reorderBlock(int block, int dest);

    // replace a block with another lv2 plugin (referenced by its URI)
    // passing null or empty string as the URI means clearing the block
    // returning false means the block was unchanged
    bool replaceBlock(int block, const char* uri);

    // switch to another bank
    // returning false means the current chain was unchanged
    // NOTE resets active preset to 0
    bool switchBank(int bank);

    // switch to another preset within the current bank
    // returning false means the current chain was unchanged
    bool switchPreset(int preset);

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
    void setBlockParameterValue(int block, int paramIndex, float value);

    // set a block property
    void setBlockProperty(int block, const char* uri, const char* value);

protected:
    // load host state as stored in the `current` struct
    void hostLoadCurrent();

    // common function to connect all the blocks as needed
    // TODO cleanup duplicated code with function below
    void hostConnectBetweenBlocks();

    // disconnect everything around the new plugin, to prevent double connections
    // TODO cleanup duplicated code with function above
    // FIXME this logic can be made much better, but this is for now just a testing tool anyhow
    void hostDisconnectForNewBlock(int blockidi);

    // internal feedback handling, for updating parameter values
    void hostFeedbackCallback(const HostFeedbackData& data) override;
};

// --------------------------------------------------------------------------------------------------------------------
