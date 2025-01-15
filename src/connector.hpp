// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "host.hpp"
#include "instance_mapper.hpp"
#include "lv2.hpp"

#include <cassert>
#include <cstdint>
#include <array>
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

        virtual ~Callback() = default;
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
        std::string quickPotSymbol;
        std::string uri;
        struct {
            // convenience meta-data, not stored in json state
            uint8_t quickPotIndex;
            bool hasScenes = false;
            uint8_t numInputs = 0;
            uint8_t numOutputs = 0;
            uint8_t numSideInputs = 0;
            uint8_t numSideOutputs = 0;
            std::string name;
            std::string abbreviation;
        } meta;
        std::vector<Parameter> parameters;
        std::array<std::vector<SceneParameterValue>, NUM_SCENES_PER_PRESET + 1> sceneValues;
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

    struct ChainRow {
        std::vector<Block> blocks;
        std::array<std::string, 2> capture;
        std::array<std::string, 2> playback;
    };

    struct Preset {
        std::string name;
        std::string filename;
        std::array<std::list<Binding>, NUM_BINDING_ACTUATORS> bindings; // TODO private ?
    private:
        friend struct HostConnector;
        friend class WebSocketConnector;
        std::array<ChainRow, NUM_BLOCK_CHAIN_ROWS> chains;
    };

    struct Current : Preset {
        uint8_t preset = 0;
        uint8_t scene = 0;
        uint8_t numLoadedPlugins = 0;
        bool dirty = false;

       #if NUM_BLOCK_CHAIN_ROWS != 1
        [[nodiscard]] inline const Block& block(const uint8_t row, const uint8_t block) const noexcept
        {
            assert(row < NUM_BLOCK_CHAIN_ROWS);
            assert(block < NUM_BLOCKS_PER_PRESET);
            return chains[row].blocks[block];
        }
       #else
        [[nodiscard]] inline const Block& block(const uint8_t block) const noexcept
        {
            assert(block < NUM_BLOCKS_PER_PRESET);
            return chains[0].blocks[block];
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
    std::array<Preset, NUM_PRESETS_PER_BANK> _presets;

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
    // debug helpers

    // get internal Id(s) used for debugging
    [[nodiscard]] std::string getBlockId(uint8_t row, uint8_t block) const;

    // print current state for debugging
    void printStateForDebug(bool withBlocks, bool withParams, bool withBindings) const;

    // ----------------------------------------------------------------------------------------------------------------
    // current state handling

    // public and read-only current preset state
    const Current& current = _current;

    // get the preset at @a index
    // returns the preset state from the current bank (which might be different from the current state)
    [[nodiscard]] const Preset& getBankPreset(uint8_t preset) const;

    // get the current preset at @a index
    // returns current state if preset is currently active, otherwise the preset state from the current bank
    [[nodiscard]] const Preset& getCurrentPreset(uint8_t preset) const;

    // check if possible to add a sidechan input/playback/sink block
    // requires an output/capture/source to be present first, or have an unmatched pair
    [[nodiscard]] bool canAddSidechainInput(uint8_t row, uint8_t block) const;

    // check if possible to add a sidechan output/capture/source block
    // requires no sidechain blocks to be present or having matched pairs
    [[nodiscard]] bool canAddSidechainOutput(uint8_t row, uint8_t block) const;

    void hostReady();

    // ----------------------------------------------------------------------------------------------------------------
    // bank handling

    // load bank from a set of preset files and activate the first
    void loadBankFromPresetFiles(const std::array<std::string, NUM_PRESETS_PER_BANK>& filenames);

    // save all presets (from the non-current data)
    bool saveBankToPresetFiles(const std::array<std::string, NUM_PRESETS_PER_BANK>& filenames);

    // ----------------------------------------------------------------------------------------------------------------
    // preset handling

    // load preset from a file, automatically replacing the current preset and optionally the default too
    // returning false means the current chain was unchanged, likely because the file contains invalid state
    bool loadCurrentPresetFromFile(const char* filename, bool replaceDefault);

    // save current preset to a file
    bool saveCurrentPresetToFile(const char* filename);

    // save current preset
    // a preset must have been loaded or saved to a file before, so that `current.filename` is valid
    bool saveCurrentPreset();

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
   // #if NUM_BLOCK_CHAIN_ROWS == 1
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
   // #else
    // move a block into a new row, by swapping position with an empty block
    // returning false means the current chain was unchanged
    bool swapBlockRow(uint8_t row, uint8_t block, uint8_t emptyRow, uint8_t emptyBlock);
   // #endif

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

    // connect a tool audio output port to another tool's input port
    void connectTool2Tool(uint8_t toolAIndex, const char* toolAOutSymbol, uint8_t toolBIndex, const char* toolBInSymbol);

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
    void hostConnectBlockToChainInput(uint8_t row, uint8_t block);
    void hostConnectBlockToChainOutput(uint8_t row, uint8_t block);
    void hostConnectChainEndpoints(uint8_t row);

    void hostDisconnectAll();
    void hostDisconnectAllBlockInputs(uint8_t row, uint8_t block);
    void hostDisconnectAllBlockOutputs(uint8_t row, uint8_t block);
    void hostDisconnectAllBlockInputs(const Block& blockdata, const HostBlockPair& hbp);
    void hostDisconnectAllBlockOutputs(const Block& blockdata, const HostBlockPair& hbp);
    void hostDisconnectChainEndpoints(uint8_t row);

    void hostEnsureStereoChain(uint8_t row, uint8_t blockStart);

    void hostSetupSideIO(uint8_t row, uint8_t block, HostBlockPair hbp, const Lv2Plugin* plugin);

    // remove all bindings related to a block
    void hostRemoveAllBlockBindings(uint8_t row, uint8_t block);

    void hostRemoveInstanceForBlock(uint8_t row, uint8_t block);

private:
    void hostConnectChainEndpointsAction(uint8_t row, bool connect);
    void hostConnectChainInputAction(uint8_t row, uint8_t block, bool connect);
    void hostConnectChainOutputAction(uint8_t row, uint8_t block, bool connect);
    void hostDisconnectBlockAction(const Block& blockdata, const HostBlockPair& hbp, bool outputs);

    template<class nlohmann_json>
    uint8_t hostLoadPreset(Preset& presetdata, nlohmann_json& json);

    template<class nlohmann_json>
    void hostSavePreset(const Preset& presetdata, nlohmann_json& json) const;

    void hostSwitchPreset(const Current& old);

    // internal feedback handling, for updating parameter values
    void hostFeedbackCallback(const HostFeedbackData& data) override;

    static void allocPreset(Preset& preset);
    static void resetPreset(Preset& preset);
};

using HostBlock = HostConnector::Block;
using HostParameter = HostConnector::Parameter;
using HostCallbackData = HostConnector::Callback::Data;

// --------------------------------------------------------------------------------------------------------------------
