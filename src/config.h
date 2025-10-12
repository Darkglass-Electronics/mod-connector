// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include <cstdint>

// --------------------------------------------------------------------------------------------------------------------
// default configuration

#ifndef NUM_BINDING_ACTUATORS
#define NUM_BINDING_ACTUATORS 6
#endif

#ifndef NUM_BINDING_PAGES
#define NUM_BINDING_PAGES 1
#endif

#ifndef NUM_PRESETS_PER_BANK
#define NUM_PRESETS_PER_BANK 3
#endif

#ifndef NUM_SCENES_PER_PRESET
#define NUM_SCENES_PER_PRESET 3
#endif

#ifndef NUM_BLOCKS_PER_PRESET
#define NUM_BLOCKS_PER_PRESET 6
#endif

#ifndef NUM_BLOCK_CHAIN_ROWS
#define NUM_BLOCK_CHAIN_ROWS 1
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

#ifndef MONITOR_AUDIO_LEVELS
#define MONITOR_AUDIO_LEVELS 0
#endif

#ifndef JACK_PLAYBACK_MONITOR_PORT_1
#define JACK_PLAYBACK_MONITOR_PORT_1 "mod-monitor:out_1"
#endif

#ifndef JACK_PLAYBACK_MONITOR_PORT_2
#define JACK_PLAYBACK_MONITOR_PORT_2 "mod-monitor:out_2"
#endif

#define UUID_SIZE 28

// --------------------------------------------------------------------------------------------------------------------
// check valid configuration

#if NUM_PRESETS_PER_BANK > UINT8_MAX
#error NUM_PRESETS_PER_BANK > UINT8_MAX, need to adjust data types
#endif

#if NUM_BLOCKS_PER_PRESET > UINT8_MAX
#error NUM_BLOCKS_PER_PRESET > UINT8_MAX, need to adjust data types
#endif

#if NUM_BLOCK_CHAIN_ROWS > UINT8_MAX
#error NUM_BLOCK_CHAIN_ROWS > UINT8_MAX, need to adjust data types
#endif

#if NUM_BLOCKS_PER_PRESET * NUM_BLOCK_CHAIN_ROWS > UINT16_MAX
#error NUM_BLOCKS_PER_PRESET * NUM_BLOCK_CHAIN_ROWS > UINT16_MAX, need to adjust data types
#endif

#if MAX_PARAMS_PER_BLOCK > UINT8_MAX
#error MAX_PARAMS_PER_BLOCK > UINT8_MAX, need to adjust data types
#endif

// --------------------------------------------------------------------------------------------------------------------
// mod-host definitions

#define MAX_MOD_HOST_PLUGIN_INSTANCES 9990
#define MAX_MOD_HOST_TOOL_INSTANCES   10
#define MAX_MOD_HOST_INSTANCES        (MAX_MOD_HOST_PLUGIN_INSTANCES + MAX_MOD_HOST_TOOL_INSTANCES)

#define MOD_HOST_EFFECT_PREFIX "effect_"
#define MOD_HOST_EFFECT_PREFIX_LEN 7
