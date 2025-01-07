// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "host.hpp"
#include "instance_mapper.hpp"
#include "lv2.hpp"

#include <cassert>
#include <list>

// --------------------------------------------------------------------------------------------------------------------

struct HostConnector : Host::FeedbackCallback {
    struct Callback {
        struct Data {
            enum {
                kAudioMonitor,
                kLog,
                kParameterSet,
                kPatchSet,
                kToolParameterSet,
                kToolPatchSet,
            } type;
            union {
                // kAudioMonitor
                struct {
                    int index;
                    float value;
                } audioMonitor;
                // kLog
                struct {
                    char type;
                    const char* msg;
                } log;
                // kParameterSet
                struct {
                    uint8_t row;
                    uint8_t block;
                    uint8_t index;
                    const char* symbol;
                    float value;
                } parameterSet;
                // kPatchSet
                struct {
                    uint8_t row;
                    uint8_t block;
                    const char* key;
                    char type;
                    HostPatchData data;
                } patchSet;
                // kToolParameterSet
                struct {
                    uint8_t index;
                    const char* symbol;
                    float value;
                } toolParameterSet;
                // kToolPatchSet
                struct {
                    uint8_t index;
                    const char* key;
                    char type;
                    HostPatchData data;
                } toolPatchSet;
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
            std::string shortname;
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
       #if NUM_BLOCK_CHAIN_ROWS != 1
        uint8_t linkedRow = UINT8_MAX;
        uint8_t linkedBlock = UINT8_MAX;
       #endif
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
            std::string abbreviation;
        } meta;
        std::vector<Parameter> parameters;
        std::vector<SceneParameterValue> sceneValues[NUM_SCENES_PER_PRESET + 1];
    };

    struct Binding {
        uint8_t row;
        uint8_t block;
        std::string parameterSymbol;
        struct {
            // convenience meta-data, not stored in json state
            uint8_t parameterIndex;
        } meta;
    };

    struct Preset {
        std::string name;
        std::list<Binding> bindings[NUM_BINDING_ACTUATORS]; // TODO private ?
    private:
        friend class HostConnector;
        friend class WebSocketConnector;
        std::vector<Block> blocks[NUM_BLOCK_CHAIN_ROWS];
    };

    struct Current : Preset {
        uint8_t preset = 0;
        uint8_t scene = 0;
        uint8_t numLoadedPlugins = 0;
        bool dirty = false;
        std::string filename;

        inline const Block& block(const uint8_t row, const uint8_t block) const noexcept
        {
            assert(row < NUM_BLOCK_CHAIN_ROWS);
            assert(block < NUM_BLOCKS_PER_PRESET);
            return blocks[row][block];
        }

       #if NUM_BLOCK_CHAIN_ROWS == 1
        inline const Block& block(const uint8_t block) const noexcept
        {
            assert(block < NUM_BLOCKS_PER_PRESET);
            return blocks[0][block];
        }
       #endif
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

    // print current state for debugging
    void printStateForDebug(bool withBlocks, bool withParams, bool withBindings);

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
    bool enableBlock(uint8_t row, uint8_t block, bool enable);

    // reorder a block into aconst  new position
    // returning false means the current chain was unchanged
    bool reorderBlock(uint8_t row, uint8_t orig, uint8_t dest);

    // replace a block with another lv2 plugin (referenced by its URI)
    // passing null or empty string as the URI means clearing the block
    // returning false means the block was unchanged
    bool replaceBlock(uint8_t row, uint8_t block, const char* uri);

    // convenience calls for single-chain builds
   #if NUM_BLOCK_CHAIN_ROWS == 1
    inline bool enableBlock(const uint8_t block, const bool enable)
    {
        return enableBlock(0, block, enable);
    }

    inline bool reorderBlock(const uint8_t orig, const uint8_t dest)
    {
        return reorderBlock(0, orig, dest);
    }

    inline bool replaceBlock(const uint8_t block, const char* const uri)
    {
        return replaceBlock(0, block, uri);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // scene handling

    // switch to another scene within the current preset
    // returning false means the current chain was unchanged
    bool switchScene(uint8_t scene);

    // ----------------------------------------------------------------------------------------------------------------
    // bindings NOTICE WORK-IN-PROGRESS

    // add a block binding (for enable/disable control)
    bool addBlockBinding(uint8_t hwid, uint8_t row, uint8_t block);

    // add a block parameter binding
    bool addBlockParameterBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t paramIndex);

    // remove a block binding (for enable/disable control)
    bool removeBlockBinding(uint8_t hwid, uint8_t row, uint8_t block);

    // remove a block parameter binding
    bool removeBlockParameterBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t paramIndex);

    // reorder bindings
    bool reorderBlockBinding(uint8_t hwid, uint8_t dest);

    // convenience calls for single-chain builds
   #if NUM_BLOCK_CHAIN_ROWS == 1
    inline bool addBlockBinding(const uint8_t hwid, const uint8_t block)
    {
        return addBlockBinding(hwid, 0, block);
    }

    inline bool addBlockParameterBinding(const uint8_t hwid, const uint8_t block, const uint8_t paramIndex)
    {
        return addBlockParameterBinding(hwid, 0, block, paramIndex);
    }

    inline bool removeBlockBinding(const uint8_t hwid, const uint8_t block)
    {
        return removeBlockBinding(hwid, 0, block);
    }

    inline bool removeBlockParameterBinding(const uint8_t hwid, const uint8_t block, const uint8_t paramIndex)
    {
        return removeBlockParameterBinding(hwid, 0, block, paramIndex);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // parameters

    // set a block parameter value
    // NOTE value must already be sanitized!
    void setBlockParameter(uint8_t row, uint8_t block, uint8_t paramIndex, float value);

    // enable monitoring for block output parameter
    void monitorBlockOutputParameter(uint8_t row, uint8_t block, uint8_t paramIndex);

    // convenience calls for single-chain builds
   #if NUM_BLOCK_CHAIN_ROWS == 1
    inline void setBlockParameter(const uint8_t block, const uint8_t paramIndex, const float value)
    {
        setBlockParameter(0, block, paramIndex, value);
    }

    inline void monitorBlockOutputParameter(const uint8_t block, const uint8_t paramIndex)
    {
        monitorBlockOutputParameter(0, block, paramIndex);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // tool handling NOTICE WORK-IN-PROGRESS

    // enable a "system tool" lv2 plugin (referenced by its URI)
    // passing null or empty string as the URI means disabling the tool
    // NOTE toolIndex must be < 10
    bool enableTool(uint8_t toolIndex, const char* uri);

    // connect a tool audio input port to an arbitrary jack output port
    void connectToolAudioInput(uint8_t toolIndex, const char* symbol, const char* jackPort);

    // connect a tool audio output port to an arbitrary jack input port
    void connectToolAudioOutput(uint8_t toolIndex, const char* symbol, const char* jackPort);

    // set a block parameter value
    // NOTE value must already be sanitized!
    void setToolParameter(uint8_t toolIndex, const char* symbol, float value);

    // enable monitoring for tool output parameter
    void monitorToolOutputParameter(uint8_t toolIndex, const char* symbol);

    // ----------------------------------------------------------------------------------------------------------------
    // properties

    // WIP details below this point

    // set a block property
    void setBlockProperty(uint8_t row, uint8_t block, const char* uri, const char* value);

    // convenience calls for single-chain builds
   #if NUM_BLOCK_CHAIN_ROWS == 1
    inline void setBlockProperty(const uint8_t block, const char* uri, const char* value)
    {
        setBlockProperty(0, block, uri, value);
    }
   #endif

protected:
    // load host state as stored in the `current` struct
    // also preloads the other presets in the bank
    void hostClearAndLoadCurrentBank();

    void hostConnectAll(uint8_t row, uint8_t blockStart = 0, uint8_t blockEnd = NUM_BLOCKS_PER_PRESET - 1);
    void hostConnectBlockToBlock(uint8_t row, uint8_t blockA, uint8_t blockB);
   #if NUM_BLOCK_CHAIN_ROWS != 1
    void hostConnectBlockToChainInput(uint8_t row, uint8_t block);
    void hostConnectBlockToChainOutput(uint8_t row, uint8_t block);
   #endif
    void hostConnectBlockToSystemInput(uint8_t block);
    void hostConnectBlockToSystemOutput(uint8_t block);

    void hostDisconnectAll();
    void hostDisconnectAllBlockInputs(uint8_t row, uint8_t block);
    void hostDisconnectAllBlockOutputs(uint8_t row, uint8_t block);

    void hostEnsureStereoChain(uint8_t row, uint8_t blockStart, uint8_t blockEnd);

    bool hostPresetBlockShouldBeStereo(const Preset& presetdata, uint8_t row, uint8_t block);

    // remove all bindings related to a block
    void hostRemoveAllBlockBindings(uint8_t row, uint8_t block);

    void hostRemoveInstanceForBlock(uint8_t row, uint8_t block);

private:
    void hostConnectSystemInputAction(uint8_t block, bool connect);
    void hostConnectSystemOutputAction(uint8_t block, bool connect);
    void hostDisconnectBlockAction(uint8_t row, uint8_t block, bool outputs);

    // internal feedback handling, for updating parameter values
    void hostFeedbackCallback(const HostFeedbackData& data) override;

    void hostReady();

    static void allocPreset(Preset& preset);
    static void resetPreset(Preset& preset);
};

typedef HostConnector::Callback::Data HostCallbackData;

// --------------------------------------------------------------------------------------------------------------------
