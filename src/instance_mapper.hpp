// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "config.h"

#include <array>

// --------------------------------------------------------------------------------------------------------------------

static constexpr const uint16_t kMaxHostInstances = NUM_BLOCKS_PER_PRESET * 2 /* dual-mono pair */
                                                  * NUM_BLOCK_CHAIN_ROWS
                                                  * NUM_PRESETS_PER_BANK
                                                  + 2 /* reserved space for block replacement */;
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
    [[nodiscard]] BlockPair get(uint8_t preset, uint8_t row, uint8_t block) const noexcept;
    [[nodiscard]] BlockAndRow get_block_with_id(uint8_t preset, uint16_t id) const noexcept;
    void reset() noexcept;
    void reorder(uint8_t preset, uint8_t row, uint8_t orig, uint8_t dest) noexcept;
    void swapPresets(uint8_t presetA, uint8_t presetB) noexcept;
    void swapBlocks(uint8_t preset, uint8_t rowA, uint8_t blockA, uint8_t rowB, uint8_t blockB) noexcept;

private:
    struct {
        struct PresetBlocks {
            std::array<BlockPair, NUM_BLOCKS_PER_PRESET * NUM_BLOCK_CHAIN_ROWS> blocks;
        };
        std::array<PresetBlocks, NUM_PRESETS_PER_BANK> presets;
    } map;

    std::array<bool, kMaxHostInstances> used;
};

using HostBlockAndRow = HostInstanceMapper::BlockAndRow;
using HostBlockPair = HostInstanceMapper::BlockPair;

// --------------------------------------------------------------------------------------------------------------------

