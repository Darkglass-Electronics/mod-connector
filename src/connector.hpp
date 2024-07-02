// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "host.hpp"
#include "lv2.hpp"

// --------------------------------------------------------------------------------------------------------------------
// default configuration

#ifndef NUM_BANKS
#define NUM_BANKS 6
#endif

#ifndef NUM_BLOCKS_IN_BANK
#define NUM_BLOCKS_IN_BANK 6
#endif

#ifndef NUM_PARAMS_PER_BLOCK
#define NUM_PARAMS_PER_BLOCK 6
#endif

// --------------------------------------------------------------------------------------------------------------------

struct Connector {
    Host host;
    Lv2World lv2world;
    bool ok = false;

    struct {
        int bank = 0;
        struct {
            struct {
                std::string uri = "-";
                struct {
                    std::string symbol = "-";
                    float value;
                } parameters[NUM_PARAMS_PER_BLOCK];
            } blocks[NUM_BLOCKS_IN_BANK];
        } banks[NUM_BANKS];
    } current;

    Connector();

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
