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
#define NUM_BLOCKS_PER_PRESET 6
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
                struct {
                    std::string uri;
                    struct {
                        std::string symbol;
                        float value;
                    } parameters[MAX_PARAMS_PER_BLOCK];
                } blocks[NUM_BLOCKS_PER_PRESET];
            } presets[NUM_PRESETS_PER_BANK];
        } banks[NUM_BANKS];
    } current;

    // constructor, initializes connection to mod-host and sets `ok` to true if successful
    HostConnector();

    // load host state as stored in the `current` struct
    void loadCurrent();

    // replace a block with another lv2 plugin (referenced by its URI)
    // passing null or empty string as the URI means clearing the block
    void replaceBlock(int bank, int preset, int block, const char* uri);

    // convenience call to replace a block for the current preset
    void replaceBlockInActivePreset(int block, const char* uri);

    // convenience method for quickly switching to another bank
    // NOTE resets active preset to 0
    void switchBank(int bank);

    // convenience method for quickly switching to another preset within the current bank
    void switchPreset(int preset);

protected:
    // common function to connect all the blocks as needed
    // TODO cleanup duplicated code with function below
    void hostConnectBetweenBlocks();

    // disconnect everything around the new plugin, to prevent double connections
    // TODO cleanup duplicated code with function above
    // FIXME this logic can be made much better, but this is for now just a testing tool anyhow
    void hostDisconnectForNewBlock(int blockidi);
};

// --------------------------------------------------------------------------------------------------------------------
