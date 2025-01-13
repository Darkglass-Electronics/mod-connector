// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "instance_mapper.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>

// --------------------------------------------------------------------------------------------------------------------

HostInstanceMapper::HostInstanceMapper() noexcept
{
    reset();
}

// --------------------------------------------------------------------------------------------------------------------

uint16_t HostInstanceMapper::add(const uint8_t preset, const uint8_t row, const uint8_t block) noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const uint16_t rblock = row * NUM_BLOCKS_PER_PRESET + block;
    assert(map.presets[preset].blocks[rblock].id == kMaxHostInstances);
    assert(map.presets[preset].blocks[rblock].pair == kMaxHostInstances);

    for (uint16_t id = 0; id < kMaxHostInstances; ++id)
    {
        if (used[id])
            continue;

        used[id] = true;
        map.presets[preset].blocks[rblock].id = id;

        return id;
    }

    // something went really wrong if we reach this, abort
    abort();
}

// --------------------------------------------------------------------------------------------------------------------

uint16_t HostInstanceMapper::add_pair(const uint8_t preset, const uint8_t row, const uint8_t block) noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const uint16_t rblock = row * NUM_BLOCKS_PER_PRESET + block;
    assert(map.presets[preset].blocks[rblock].id != kMaxHostInstances);
    assert(map.presets[preset].blocks[rblock].pair == kMaxHostInstances);

    for (uint16_t id2 = 0; id2 < kMaxHostInstances; ++id2)
    {
        if (used[id2])
            continue;

        used[id2] = true;
        map.presets[preset].blocks[rblock].pair = id2;

        return id2;
    }

    // something went really wrong if we reach this, abort
    abort();
}

// --------------------------------------------------------------------------------------------------------------------

HostInstanceMapper::BlockPair HostInstanceMapper::remove(const uint8_t preset,
                                                         const uint8_t row,
                                                         const uint8_t block) noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const uint16_t rblock = row * NUM_BLOCKS_PER_PRESET + block;
    assert(map.presets[preset].blocks[rblock].id != kMaxHostInstances);

    const uint16_t id = map.presets[preset].blocks[rblock].id;
    const uint16_t id2 = map.presets[preset].blocks[rblock].pair;

    map.presets[preset].blocks[rblock].id = kMaxHostInstances;
    used[id] = false;

    if (id2 != kMaxHostInstances)
    {
        map.presets[preset].blocks[rblock].pair = kMaxHostInstances;
        used[id2] = false;
    }

    return { id, id2 };
}

// --------------------------------------------------------------------------------------------------------------------

uint16_t HostInstanceMapper::remove_pair(const uint8_t preset, const uint8_t row, const uint8_t block) noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const uint16_t rblock = row * NUM_BLOCKS_PER_PRESET + block;
    assert(map.presets[preset].blocks[rblock].id != kMaxHostInstances);
    assert(map.presets[preset].blocks[rblock].pair != kMaxHostInstances);

    const uint16_t id2 = map.presets[preset].blocks[rblock].pair;

    map.presets[preset].blocks[rblock].pair = kMaxHostInstances;
    used[id2] = false;

    return id2;
}

// --------------------------------------------------------------------------------------------------------------------

HostInstanceMapper::BlockPair HostInstanceMapper::get(const uint8_t preset,
                                                      const uint8_t row,
                                                      const uint8_t block) const noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(block < NUM_BLOCKS_PER_PRESET);

    const uint16_t rblock = row * NUM_BLOCKS_PER_PRESET + block;
    return map.presets[preset].blocks[rblock];
}

// --------------------------------------------------------------------------------------------------------------------

HostInstanceMapper::BlockAndRow HostInstanceMapper::get_block_with_id(const uint8_t preset,
                                                                      const uint16_t id) const noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);

    for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET * NUM_BLOCK_CHAIN_ROWS; ++b)
    {
        if (map.presets[preset].blocks[b].id == id)
            return {
                static_cast<uint8_t>(b % NUM_BLOCKS_PER_PRESET),
                static_cast<uint8_t>(b / NUM_BLOCKS_PER_PRESET)
            };

        if (map.presets[preset].blocks[b].pair == id)
            break;
    }

    return { NUM_BLOCKS_PER_PRESET, NUM_BLOCK_CHAIN_ROWS };
}

// --------------------------------------------------------------------------------------------------------------------

void HostInstanceMapper::reset() noexcept
{
    for (auto& preset : map.presets)
        for (auto &block : preset.blocks)
            block.id = block.pair = kMaxHostInstances;

    used.fill(false);
}

// --------------------------------------------------------------------------------------------------------------------

void HostInstanceMapper::reorder(const uint8_t preset,
                                 const uint8_t row,
                                 const uint8_t orig,
                                 const uint8_t dest) noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(row < NUM_BLOCK_CHAIN_ROWS);
    assert(orig < NUM_BLOCKS_PER_PRESET);
    assert(dest < NUM_BLOCKS_PER_PRESET);
    assert(orig != dest);

    const uint16_t offset = row * NUM_BLOCKS_PER_PRESET;
    auto& mpreset = map.presets[preset];

    if (orig > dest)
    {
        for (int i = orig; i > dest; --i)
            std::swap(mpreset.blocks[offset + i], mpreset.blocks[offset + i - 1]);
    }
    else
    {
        for (int i = orig; i < dest; ++i)
            std::swap(mpreset.blocks[offset + i], mpreset.blocks[offset + i + 1]);
    }
}

// --------------------------------------------------------------------------------------------------------------------

void HostInstanceMapper::swap(const uint8_t preset,
                              const uint8_t rowA,
                              const uint8_t blockA,
                              const uint8_t rowB,
                              const uint8_t blockB) noexcept
{
    assert(preset < NUM_PRESETS_PER_BANK);
    assert(rowA < NUM_BLOCK_CHAIN_ROWS);
    assert(blockA < NUM_BLOCKS_PER_PRESET);
    assert(rowB < NUM_BLOCK_CHAIN_ROWS);
    assert(blockB < NUM_BLOCKS_PER_PRESET);
    assert(rowA != rowB);

    const uint16_t rblockA = rowA * NUM_BLOCKS_PER_PRESET + blockA;
    const uint16_t rblockB = rowB * NUM_BLOCKS_PER_PRESET + blockB;
    std::swap(map.presets[preset].blocks[rblockA], map.presets[preset].blocks[rblockB]);
}

// --------------------------------------------------------------------------------------------------------------------
