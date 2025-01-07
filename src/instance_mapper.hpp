// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "config.h"

// --------------------------------------------------------------------------------------------------------------------

static constexpr const uint16_t kMaxHostInstances = NUM_BLOCKS_PER_PRESET * 2 /* dual-mono pair */
                                                  * NUM_BLOCK_CHAIN_ROWS
                                                  * NUM_PRESETS_PER_BANK;
static_assert(kMaxHostInstances < MAX_MOD_HOST_PLUGIN_INSTANCES,
              "maximum amount of instances is bigger than what mod-host can do");

// --------------------------------------------------------------------------------------------------------------------

struct HostInstanceMapper {
    struct BlockAndRow {
        uint8_t block;
        uint8_t row;
    };

    struct BlockPair {
        uint16_t id;
        uint16_t pair;
    };

    HostInstanceMapper() noexcept;
    uint16_t add(uint8_t preset, uint8_t row, uint8_t block) noexcept;
    uint16_t add_pair(uint8_t preset, uint8_t row, uint8_t block) noexcept;
    BlockPair remove(uint8_t preset, uint8_t row, uint8_t block) noexcept;
    uint16_t remove_pair(uint8_t preset, uint8_t row, uint8_t block) noexcept;
    BlockPair get(uint8_t preset, uint8_t row, uint8_t block) const noexcept;
    BlockAndRow get_block_with_id(uint8_t preset, uint16_t id) const noexcept;
    void reset() noexcept;
    void reorder(uint8_t preset, uint8_t row, uint8_t orig, uint8_t dest) noexcept;

private:
    struct {
        struct {
            BlockPair blocks[NUM_BLOCKS_PER_PRESET * NUM_BLOCK_CHAIN_ROWS];
        } presets[NUM_PRESETS_PER_BANK];
    } map;

    bool used[kMaxHostInstances];
};

typedef HostInstanceMapper::BlockAndRow HostBlockAndRow;
typedef HostInstanceMapper::BlockPair HostBlockPair;

// --------------------------------------------------------------------------------------------------------------------

