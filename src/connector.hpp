// SPDX-FileCopyrightText: 2024 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "host.hpp"
#include "lv2.hpp"

#include <cassert>
#include <cstring>
#include <list>

// --------------------------------------------------------------------------------------------------------------------
// default configuration

#ifndef NUM_BINDING_ACTUATORS
#define NUM_BINDING_ACTUATORS 6
#endif

#ifndef NUM_PRESETS_PER_BANK
#define NUM_PRESETS_PER_BANK 3
#endif

#ifndef NUM_SCENES_PER_PRESET
#define NUM_SCENES_PER_PRESET 2
#endif

#ifndef NUM_BLOCKS_PER_PRESET
#define NUM_BLOCKS_PER_PRESET 6
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

#ifndef JACK_PLAYBACK_MONITOR_PORT_1
#define JACK_PLAYBACK_MONITOR_PORT_1 "mod-monitor:out_1"
#endif

#ifndef JACK_PLAYBACK_MONITOR_PORT_2
#define JACK_PLAYBACK_MONITOR_PORT_2 "mod-monitor:out_2"
#endif

// --------------------------------------------------------------------------------------------------------------------
// check valid configuration

#if NUM_PRESETS_PER_BANK > UINT8_MAX
#error NUM_PRESETS_PER_BANK > UINT8_MAX, need to adjust data types
#endif

#if NUM_BLOCKS_PER_PRESET > UINT8_MAX
#error NUM_BLOCKS_PER_PRESET > UINT8_MAX, need to adjust data types
#endif

#if MAX_PARAMS_PER_BLOCK > UINT8_MAX
#error MAX_PARAMS_PER_BLOCK > UINT8_MAX, need to adjust data types
#endif

// --------------------------------------------------------------------------------------------------------------------

static constexpr const uint16_t kMaxHostInstances = NUM_PRESETS_PER_BANK * (NUM_BLOCKS_PER_PRESET * 4);
static_assert(kMaxHostInstances < 9990, "maximum amount of instances is bigger than what mod-host can do");

struct HostInstanceMapper {
    struct BlockPair {
        uint16_t id;
        uint16_t pair;
    };

    struct {
        struct {
            BlockPair blocks[NUM_BLOCKS_PER_PRESET];
        } presets[NUM_PRESETS_PER_BANK];
    } map;

    bool used[kMaxHostInstances];

    HostInstanceMapper()
    {
        reset();
    }

    uint16_t add(const uint8_t preset, const uint8_t block)
    {
        assert(map.presets[preset].blocks[block].id == kMaxHostInstances);
        assert(map.presets[preset].blocks[block].pair == kMaxHostInstances);

        for (uint16_t id = 0; id < kMaxHostInstances; ++id)
        {
            if (used[id])
                continue;

            used[id] = true;
            map.presets[preset].blocks[block].id = id;

            return id;
        }

        // something went really wrong if we reach this, abort
        abort();
    }

    uint16_t add_pair(const uint8_t preset, const uint8_t block)
    {
        assert(map.presets[preset].blocks[block].id != kMaxHostInstances);
        assert(map.presets[preset].blocks[block].pair == kMaxHostInstances);

        for (uint16_t id2 = 0; id2 < kMaxHostInstances; ++id2)
        {
            if (used[id2])
                continue;

            used[id2] = true;
            map.presets[preset].blocks[block].pair = id2;

            return id2;
        }

        // something went really wrong if we reach this, abort
        abort();
    }

    BlockPair remove(const uint8_t preset, const uint8_t block)
    {
        assert(map.presets[preset].blocks[block].id != kMaxHostInstances);

        const uint16_t id = map.presets[preset].blocks[block].id;
        const uint16_t id2 = map.presets[preset].blocks[block].pair;

        map.presets[preset].blocks[block].id = kMaxHostInstances;
        used[id] = false;

        if (id2 != kMaxHostInstances)
        {
            map.presets[preset].blocks[block].pair = kMaxHostInstances;
            used[id2] = false;
        }

        return { id, id2 };
    }

    uint16_t remove_pair(const uint8_t preset, const uint8_t block)
    {
        assert(map.presets[preset].blocks[block].id != kMaxHostInstances);
        assert(map.presets[preset].blocks[block].pair != kMaxHostInstances);

        const uint16_t id2 = map.presets[preset].blocks[block].pair;

        map.presets[preset].blocks[block].pair = kMaxHostInstances;
        used[id2] = false;

        return id2;
    }

    BlockPair get(const uint8_t preset, const uint8_t block) const
    {
        return map.presets[preset].blocks[block];
    }

    uint8_t get_block_with_id(const uint8_t preset, const uint16_t id) const
    {
        for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
        {
            if (map.presets[preset].blocks[b].id == id)
                return b;
            if (map.presets[preset].blocks[b].pair == id)
                return NUM_BLOCKS_PER_PRESET;
        }

        return NUM_BLOCKS_PER_PRESET;
    }

    void reset()
    {
        for (uint8_t p = 0; p < NUM_PRESETS_PER_BANK; ++p)
            for (uint8_t b = 0; b < NUM_BLOCKS_PER_PRESET; ++b)
                map.presets[p].blocks[b].id = map.presets[p].blocks[b].pair = kMaxHostInstances;

        std::memset(used, 0, sizeof(bool) * kMaxHostInstances);
    }
};

// --------------------------------------------------------------------------------------------------------------------

struct HostConnector : Host::FeedbackCallback {
    struct Callback {
        struct Data {
            enum {
                kAudioMonitor,
                kLog,
                kParameterSet,
                kPatchSet,
            } type;
            union {
                struct {
                    int index;
                    float value;
                } audioMonitor;
                struct {
                    char type;
                    const char* msg;
                } log;
                struct {
                    uint8_t block;
                    uint8_t index;
                    const char* symbol;
                    float value;
                } parameterSet;
                struct {
                    uint8_t block;
                    const char* key;
                    char type;
                    union {
                        int32_t b;
                        int32_t i;
                        int64_t l;
                        float f;
                        double g;
                        const char* s;
                        const char* p;
                        const char* u;
                        struct {
                            uint32_t num;
                            char type;
                            union {
                                const int32_t* b;
                                const int32_t* i;
                                const int64_t* l;
                                const float* f;
                                const double* g;
                            } data;
                        } v;
                    } data;
                } patchSet;
            };
        };

        virtual ~Callback() {};
        virtual void hostConnectorCallback(const Data& data) = 0;
    };

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

    struct SceneParameterValue {
        bool used;
        float value;
    };

    struct Block {
        bool enabled = false;
        std::string quickPotSymbol;
        std::string uri;
        struct {
            // convenience meta-data, not stored in json state
            uint8_t quickPotIndex;
            bool hasScenes = false;
            bool isChainPoint = false;
            bool isMonoIn = false;
            bool isStereoOut = false;
            std::string name;
        } meta;
        std::vector<Parameter> parameters;
        std::vector<SceneParameterValue> sceneValues[NUM_SCENES_PER_PRESET + 1];
    };

    struct Binding {
        uint8_t block;
        std::string parameterSymbol;
        struct {
            // convenience meta-data, not stored in json state
            uint8_t parameterIndex;
        } meta;
    };

    struct Preset {
        std::string name;
        std::vector<Block> blocks;
        std::list<Binding> bindings[NUM_BINDING_ACTUATORS];
    };

    struct Current : Preset {
        uint8_t preset = 0;
        uint8_t scene = 0;
        uint8_t numLoadedPlugins = 0;
        bool dirty = false;
        std::string filename;
    };

    // connection to mod-host, handled internally
    Host _host;

protected:
    // internal host instance mapper
    HostInstanceMapper _mapper;

    // internal current preset state
    Current _current;

    // default state for each preset
    Preset _presets[NUM_PRESETS_PER_BANK];

    // current connector callback
    Callback* _callback = nullptr;

    // first time booting up
    bool _firstboot = true;

public:
    // lv2 world for getting information about plugins
    const Lv2World lv2world;

    // constructor, initializes connection to mod-host and sets `ok` to true if successful
    HostConnector();

    // ----------------------------------------------------------------------------------------------------------------
    // check valid configuration

    // whether the host connection is working
    bool ok = false;

    // try to reconnect host if it previously failed
    bool reconnect();

    // return average dsp load
    float dspLoad();

    // poll for host updates (e.g. MIDI-mapped parameter changes, tempo changes)
    // NOTE make sure to call `requestHostUpdates()` after handling all updates
    void pollHostUpdates(Callback* callback);

    // request more host updates
    void requestHostUpdates();

    // ----------------------------------------------------------------------------------------------------------------
    // check valid configuration

    // public and read-only current preset state
    const Current& current = _current;

    // get the preset at @a index
    // returns the preset state from the current bank (which might be different from the current state)
    const Preset& getBankPreset(uint8_t preset) const;

    // get the current preset at @a index
    // returns current state if preset is currently active, otherwise the preset state from the current bank
    const Preset& getCurrentPreset(uint8_t preset) const;

    // ----------------------------------------------------------------------------------------------------------------
    // file handling

    // load bank from a file and store the first preset in the `current` struct
    // automatically calls loadCurrent() if the file contains valid state, otherwise does nothing
    // returning false means the current chain was unchanged
    bool loadBankFromFile(const char* filename);

    // save bank state as stored in the `current` struct into a new file
    bool saveBankToFile(const char* filename);

    // ----------------------------------------------------------------------------------------------------------------
    // file handling

    // save bank state as stored in the `current` struct
    // a bank must have been loaded or saved to a file before, so that `current.filename` is valid
    bool saveBank();

    // ----------------------------------------------------------------------------------------------------------------
    // preset handling

    // clear current preset
    // sets dirty flag if any blocks were removed
    void clearCurrentPreset();

    // set the name of the current preset
    void setCurrentPresetName(const char* name);

    // switch to another preset within the current bank
    // returning false means the current chain was unchanged
    bool switchPreset(uint8_t preset);

    // ----------------------------------------------------------------------------------------------------------------
    // block handling

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

    // ----------------------------------------------------------------------------------------------------------------
    // scene handling

    // switch to another scene within the current preset
    // returning false means the current chain was unchanged
    bool switchScene(uint8_t scene);

    // ----------------------------------------------------------------------------------------------------------------
    // bindings NOTICE WORK-IN-PROGRESS

    // add a block binding (for enable/disable control)
    bool addBlockBinding(uint8_t hwid, uint8_t block);

    // add a block parameter binding
    bool addBlockParameterBinding(uint8_t hwid, uint8_t block, uint8_t paramIndex);

    // remove a block binding (for enable/disable control)
    bool removeBlockBinding(uint8_t hwid, uint8_t block);

    // remove a block parameter binding
    bool removeBlockParameterBinding(uint8_t hwid, uint8_t block, uint8_t paramIndex);

    // reorder bindings
    bool reorderBlockBinding(uint8_t hwid, uint8_t dest);

    // ----------------------------------------------------------------------------------------------------------------
    // parameters

    // set a block parameter value
    // NOTE value must already be sanitized!
    void setBlockParameter(uint8_t block, uint8_t paramIndex, float value);

    // enable monitoring for block output parameter
    void monitorBlockOutputParameter(uint8_t block, uint8_t paramIndex);

    // ----------------------------------------------------------------------------------------------------------------
    // properties

    // WIP details below this point

    // set a block property
    void setBlockProperty(uint8_t block, const char* uri, const char* value);

protected:
    // load host state as stored in the `current` struct
    // also preloads the other presets in the bank
    void hostClearAndLoadCurrentBank();

    void hostConnectAll(uint8_t blockStart = 0, uint8_t blockEnd = NUM_BLOCKS_PER_PRESET - 1);
    void hostConnectBlockToBlock(uint8_t blockA, uint8_t blockB);
    void hostConnectBlockToSystemInput(uint8_t block);
    void hostConnectBlockToSystemOutput(uint8_t block);

    void hostDisconnectAll();
    void hostDisconnectAllBlockInputs(uint8_t block);
    void hostDisconnectAllBlockOutputs(uint8_t block);

    void hostEnsureStereoChain(uint8_t blockStart, uint8_t blockEnd);

    bool hostPresetBlockShouldBeStereo(const Preset& presetdata, uint8_t block);

    void hostRemoveInstanceForBlock(uint8_t block);

private:
    void hostConnectSystemInputAction(uint8_t block, bool connect);
    void hostConnectSystemOutputAction(uint8_t block, bool connect);
    void hostDisconnectBlockAction(uint8_t block, bool outputs);

    // internal feedback handling, for updating parameter values
    void hostFeedbackCallback(const HostFeedbackData& data) override;

    void hostReady();
};

typedef HostConnector::Callback::Data HostCallbackData;

// --------------------------------------------------------------------------------------------------------------------
