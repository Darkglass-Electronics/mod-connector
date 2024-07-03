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

#ifndef NUM_PARAMS_PER_BLOCK
#define NUM_PARAMS_PER_BLOCK 6
#endif

// --------------------------------------------------------------------------------------------------------------------

struct HostConnector {
    Host host;
    Lv2World lv2world;
    bool ok = false;

    struct {
        int bank = 0;
        int preset = 0; // NOTE resets to 0 on bank change
        struct {
            struct {
                struct {
                    std::string uri = "-";
                    struct {
                        std::string symbol = "-";
                        float value;
                    } parameters[NUM_PARAMS_PER_BLOCK];
                } blocks[NUM_BLOCKS_PER_PRESET];
            } presets[NUM_PRESETS_PER_BANK];
        } banks[NUM_BANKS];
    } current;

    HostConnector();

    // load state as saved in the `current` struct
    void loadCurrent();

    // common function to connect all the blocks as needed
    // TODO cleanup duplicated code with function below
    void hostConnectBetweenBlocks();

    // disconnect everything around the new plugin, to prevent double connections
    // TODO cleanup duplicated code with function above
    // FIXME this logic can be made much better, but this is for now just a testing tool anyhow
    void hostDisconnectForNewBlock(int blockidi);
};

// --------------------------------------------------------------------------------------------------------------------
