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
#define NUM_PRESETS_PER_BANK 12
#endif

#ifndef NUM_BLOCKS_PER_PRESET
#define NUM_BLOCKS_PER_PRESET 24
#endif

#ifndef MAX_PARAMS_PER_BLOCK
#define MAX_PARAMS_PER_BLOCK 12
#endif

// --------------------------------------------------------------------------------------------------------------------

struct HostConnector {
    // connection to mod-host, handled internally
    Host host;

    // lv2 world for getting information about plugins
    Lv2World lv2world;

    // whether the host connection is working
    bool ok = false;

    // current state, including all presets and banks
    struct {
        int bank = 0;
        int preset = 0; // NOTE resets to 0 on bank change
        struct {
            struct {
                std::string name;
                struct {
                    int binding = -1;
                    std::string uri;
                    struct {
                        std::string symbol;
                        float value;
                        // convenience meta-data, not stored in json state
                        uint32_t flags;
                        float minimum, maximum;
                    } parameters[MAX_PARAMS_PER_BLOCK];
                } blocks[NUM_BLOCKS_PER_PRESET];
            } presets[NUM_PRESETS_PER_BANK];
        } banks[NUM_BANKS];
    } current;

    // constructor, initializes connection to mod-host and sets `ok` to true if successful
    HostConnector();

    // load state from a file and store it in the `current` struct
    // automatically calls loadCurrent() if the file contains valid state, otherwise does nothing
    // returning false means the current chain was unchanged
    bool loadStateFromFile(const char* filename);

    // save host state as stored in the `current` struct into a file
    bool saveStateToFile(const char* filename) const;

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

    // set a block property
    void setBlockProperty(int block, const char* property_uri, const char* value);

    // update the host value of a block parameter
    void hostUpdateParameterValue(int block, int index);

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
};

// --------------------------------------------------------------------------------------------------------------------
