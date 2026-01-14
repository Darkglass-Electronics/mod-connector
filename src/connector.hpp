// SPDX-FileCopyrightText: 2024-2025 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

#include "host.hpp"
#include "json_fwd.hpp"
#include "instance_mapper.hpp"
#include "lv2.hpp"

#include <cassert>
#include <cstdint>
#include <array>
#include <list>
#include <unordered_map>

enum ExtraLv2Flags {
    Lv2ParameterVirtual = 1 << 12,
    Lv2ParameterInScene = 1 << 13,
    Lv2ParameterNotInQuickPot = 1 << 14,
    Lv2ParameterValueChangesNotSaved = 1 << 15, // not from lv2, can be added/removed in runtime
};

enum Lv2ParameterState {
    Lv2ParameterStateNone = 0,
    Lv2ParameterStateInactive,
    Lv2ParameterStateBlocked,
};

// --------------------------------------------------------------------------------------------------------------------

struct HostConnector : Host::FeedbackCallback {
    struct Callback {
        struct Data {
            enum {
                kAudioMonitor,
                kCpuLoad,
                kLog,
                kParameterSet,
                kParameterState,
                kPatchSet,
                kToolParameterSet,
                kToolPatchSet,
                // TODO rename Patch to Property
                kMidiControlChange,
                kMidiProgramChange,
            } type;
            union {
                // kAudioMonitor
                struct {
                    int index;
                    float value;
                } audioMonitor;
                // kCpuLoad
                struct {
                    float avg;
                    float max;
                    uint32_t xruns;
                } cpuLoad;
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
                // kParameterState
                struct {
                    uint8_t row;
                    uint8_t block;
                    uint8_t index;
                    const char* symbol;
                    Lv2ParameterState state;
                } parameterState;
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
                // kMidiControlChange
                struct {
                    uint8_t channel;
                    uint8_t control;
                    uint16_t value;
                } midiControlChange;
                // kMidiProgramChange
                struct {
                    uint8_t channel;
                    uint8_t program;
                } midiProgramChange;
            };
        };

        virtual ~Callback() = default;
        virtual void hostConnectorCallback(const Data& data) = 0;
    };

    enum TemporarySceneState : uint8_t {
        kTemporarySceneNone = 0,
        kTemporarySceneActivate,
        kTemporarySceneClear,
    };

    struct Parameter {
        std::string symbol;
        float value;
        struct {
            // convenience meta-data, not stored in json state
            uint32_t flags;
            uint32_t designation;
            uint8_t hwbinding;
            TemporarySceneState tempSceneState;
            Lv2ParameterState state;
            float def, min, max;
            float defttl; // default from plugin ttl, which might not match initial state (default preset override)
            std::string name;
            std::string shortname;
            std::string unit;
            std::vector<Lv2ScalePoint> scalePoints;
        } meta;
    };

    struct Property {
        std::string uri;
        std::string value; // TODO float or string
        struct {
            // convenience meta-data, not stored in json state
            uint32_t flags;
            uint8_t hwbinding;
            TemporarySceneState tempSceneState;
            float def, min, max; // used for Lv2PropertyIsParameter
            std::string defpath; // used for Lv2PropertyIsPath
            std::string name;
            std::string shortname;
        } meta;
    };

    enum SceneMode {
        // enable scenes if not active yet
        SceneModeActivate,
        // sync all parameter values in a scene, same as clearing it
        SceneModeClear,
        // only update value, do not activate any scenes
        SceneModeUpdate,
        // enable scenes if not active yet, but only temporarily
        // scene value is discarded if preset is not saved
        SceneModeActivateTemporarily,
        // sync all parameter values in a scene, same as clearing it, but only temporarily
        // scene value is "uncleared" if preset is not saved
        SceneModeClearTemporarily,
        // only update value, do not activate any scenes, but only temporarily
        // scene value is discarded if preset is not saved
        SceneModeUpdateTemporarily,
    };

    struct SceneValues {
        bool enabled;
        std::vector<float> parameters;
        std::vector<std::string> properties;
    };

    struct Block {
        bool enabled;
        std::string quickPotSymbol;
        std::string uri;
        struct {
            // convenience meta-data, not stored in json state
            struct {
                bool hasScenes;
                uint8_t hwbinding;
                TemporarySceneState tempSceneState;
            } enable;
            uint8_t quickPotIndex;
            uint8_t numParametersInScenes;
            uint8_t numPropertiesInScenes;
            uint8_t numInputs;
            uint8_t numOutputs;
            uint8_t numSideInputs;
            uint8_t numSideOutputs;
            std::string name;
            std::string abbreviation;
            std::string brand;
        } meta;
        std::vector<Parameter> parameters;
        std::vector<Property> properties;
        std::array<SceneValues, NUM_SCENES_PER_PRESET> sceneValues;

        inline uint8_t parameterIndexForSymbol(const std::string& parameterSymbol) const
        {
            try {
                return parameterSymbolToIndexMap.at(parameterSymbol);
            } catch (...) {
                return UINT8_MAX;
            }
        }

        inline uint8_t propertyIndexForURI(const std::string& propertyURI) const
        {
            try {
                return propertyURIToIndexMap.at(propertyURI);
            } catch (...) {
                return UINT8_MAX;
            }
        }

    private:
        // extra details, not stored in json state
        friend struct HostConnector;
        std::array<SceneValues, NUM_SCENES_PER_PRESET> lastSavedSceneValues;
        std::unordered_map<std::string, uint8_t> parameterSymbolToIndexMap;
        std::unordered_map<std::string, uint8_t> propertyURIToIndexMap;
    };

    struct ParameterBinding {
        uint8_t row;
        uint8_t block;
        float min;
        float max;
        std::string parameterSymbol;
        struct {
            // convenience meta-data, not stored in json state
            uint8_t parameterIndex;
        } meta;
        bool bindingValueChangesNotSaved = false;
    };

    struct PropertyBinding {
        uint8_t row;
        uint8_t block;
        std::string propertyURI;
        struct {
            // convenience meta-data, not stored in json state
            uint8_t propertyIndex;
        } meta;
    };

    struct Bindings {
        std::string name;
        std::list<ParameterBinding> parameters;
        std::list<PropertyBinding> properties;
        double value; // NOTE normalized 0-1, updated automatically if single binding
    };

    struct ChainRow {
        std::vector<Block> blocks;
        std::array<std::string, 2> capture;
        std::array<std::string, 2> playback;
        std::array<uint16_t, 2> captureId;
        std::array<uint16_t, 2> playbackId;
    };

    struct Preset {
        uint8_t scene;
        std::string name;
        std::string filename;
        std::array<Bindings, NUM_BINDING_ACTUATORS> bindings;
        struct {
            uint32_t color;
            std::string style;
        } background;
        std::array<std::string, NUM_SCENES_PER_PRESET> sceneNames;
        std::array<unsigned char, UUID_SIZE> uuid;
    private:
        friend struct HostConnector;
        friend class WebSocketConnector;
        std::array<ChainRow, NUM_BLOCK_CHAIN_ROWS> chains;
    };

    struct Current : Preset {
        uint8_t defaultScene = 0;
        uint8_t preset = 0;
        uint8_t numLoadedPlugins = 0;
        int dirty = 0; // 0|false for clean, 1|true for dirty, < 0 for scene only change (also dirty)

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

    // list of virtual parameters per plugin
    // NOTE symbol MUST start with ":" and not be ":bypass"
    std::unordered_map<std::string, std::vector<Lv2Port>> virtualParameters;

    // constructor, initializes connection to mod-host and sets `ok` to true if successful
    HostConnector();

    // ----------------------------------------------------------------------------------------------------------------

    // whether the host connection is working
    bool ok = false;

    // try to reconnect host if it previously failed
    bool reconnect();

    // get last error from host in case something failed
    [[nodiscard]] const std::string& getLastError() const;

    // listen to MIDI control change messages
    bool monitorMidiControl(uint8_t midiChannel, bool enable);

    // listen to MIDI program change messages
    bool monitorMidiProgram(uint8_t midiChannel, bool enable);

    // poll for host updates (e.g. MIDI-mapped parameter changes, tempo changes)
    // NOTE make sure to call `requestHostUpdates()` after handling all updates
    void pollHostUpdates(Callback* callback);

    // request more host updates
    void requestHostUpdates();

    // ----------------------------------------------------------------------------------------------------------------
    // debug helpers

    // get internal Id(s) used for debugging
    [[nodiscard]] std::string getBlockId(uint8_t row, uint8_t block) const;

    // get internal Id without pair's id used for debugging
    [[nodiscard]] std::string getBlockIdNoPair(uint8_t row, uint8_t block) const;

    // get internal Id of pair used for debugging
    [[nodiscard]] std::string getBlockIdPairOnly(uint8_t row, uint8_t block) const;

    // print current state for debugging
    void printStateForDebug(bool withBlocks, bool withParams, bool withBindings) const;

    // ----------------------------------------------------------------------------------------------------------------
    // cpu load handling

    void enableCpuLoadUpdates(bool enable);

    // return average cpu load
    float getAverageCpuLoad();

    // return maximum cpu load
    float getMaximumCpuLoad();

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

    // set new custom ports to be used as chain capture can playback ports
    bool setJackPorts(const std::array<std::string, 2>& capture, const std::array<std::string, 2>& playback);

    void hostReady();

    // turn audio processing on / off (with fade in / fade out)
    // many other functions do this within preset handling, this is for special cases
    void enableAudioProcessing(bool enable);

    // set current preset dirty state
    void setDirty(bool dirty = true);

    // ----------------------------------------------------------------------------------------------------------------
    // bank handling

    // load bank from a set of preset files and activate the first
    void loadBankFromPresetFiles(const std::array<std::string, NUM_PRESETS_PER_BANK>& filenames,
                                 uint8_t initialPresetToLoad = 0);

    // ----------------------------------------------------------------------------------------------------------------
    // preset handling

    // get the name of an arbitrary preset file
    static std::string getPresetNameFromFile(const char* filename);

    // load preset from a file, automatically replacing the current preset and optionally the default too
    // returning false means the current chain was unchanged, likely because the file contains invalid state
    bool loadCurrentPresetFromFile(const char* filename, bool replaceDefault);

    // preload a preset from a file, preset **must not be the current one**
    bool preloadPresetFromFile(uint8_t preset, const char* filename);

    // save current preset to a file
    bool saveCurrentPresetToFile(const char* filename);

    // reorder/move a preset into a new position (within the current bank)
    bool reorderPresets(uint8_t orig, uint8_t dest);

    // swap 2 presets within the current bank
    void swapPresets(uint8_t presetA, uint8_t presetB);

    // save current preset
    // a preset must have been loaded or saved to a file before, so that `current.filename` is valid
    bool saveCurrentPreset();

    // clear current preset
    // sets dirty flag if any blocks were removed
    // uuid is also regenerated
    void clearCurrentPreset();

    // clear current preset background (color and style)
    void clearCurrentPresetBackground();

    // regenerate current preset uuid
    void regenUUID();

    // set the filename of a preset
    void setPresetFilename(uint8_t preset, const char* filename);

    // set the name of the current preset
    void setCurrentPresetName(const char* name);

    // convenience call for setting current preset filename
    void setCurrentPresetFilename(const char* filename)
    {
        setPresetFilename(_current.preset, filename);
    }

    // switch to another preset within the current bank
    // returning false means the current chain was unchanged
    bool switchPreset(uint8_t preset);

    // rename a preset within the current bank
    void renamePreset(uint8_t preset, const char* name);

    // ----------------------------------------------------------------------------------------------------------------
    // block handling

    // enable or disable/bypass a block
    // returning false means the block was unchanged
    bool enableBlock(uint8_t row, uint8_t block, bool enable, SceneMode sceneMode, bool markPresetDirty = true);

    // reorder/move a block into a new position
    // returning false means the current chain was unchanged
    bool reorderBlock(uint8_t row, uint8_t orig, uint8_t dest);

    // replace a block with another lv2 plugin (referenced by its URI)
    // passing null or empty string as the URI means clearing the block
    // returning false means the block was unchanged
    // use clearBindingsForReplacementBlock=false only when making sure the new plugin has same control inputs
    bool replaceBlock(uint8_t row, uint8_t block, const char* uri, bool clearBindingsForReplacementBlock = true);

    // replace a block with another lv2 plugin that matches current one (referenced by its URI)
    // the current and new plugin must have the exact same parameters and properties
    // returning false means the block was unchanged
    bool replaceBlockWhileKeepingCurrentData(uint8_t row, uint8_t block, const char* uri);

    // reset a block to default settings, optionally deleting custom user setting
    bool resetBlock(uint8_t row, uint8_t block, bool resetUserDefaults = false);

    // save a current block state as the default state for next time the same block is loaded
    // this is done by saving an lv2 preset of the plugin inside the block
    bool saveBlockStateAsDefault(uint8_t row, uint8_t block);

    // convenience calls for single-chain builds
   #if NUM_BLOCK_CHAIN_ROWS == 1
    inline bool enableBlock(const uint8_t block, const bool enable, const SceneMode sceneMode)
    {
        return enableBlock(0, block, enable, sceneMode);
    }

    inline bool reorderBlock(const uint8_t orig, const uint8_t dest)
    {
        return reorderBlock(0, orig, dest);
    }

    inline bool replaceBlock(const uint8_t block, const char* const uri)
    {
        return replaceBlock(0, block, uri);
    }
   #else
    // move a block into a new row, by swapping position with an empty block
    // returning false means the current chain was unchanged
    bool swapBlockRow(uint8_t row, uint8_t block, uint8_t emptyRow, uint8_t emptyBlock);
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // scene handling

    // reorder/move a scene into a new position (within the current preset)
    bool reorderScenes(uint8_t orig, uint8_t dest);

    // swap 2 scenes within the current preset
    void swapScenes(uint8_t sceneA, uint8_t sceneB);

    // switch to another scene within the current preset
    // returning false means the current chain was unchanged
    bool switchScene(uint8_t scene);

    // rename a scene within the current preset
    bool renameScene(uint8_t scene, const char* name);

    // convenience call for renaming current scene name
    inline bool renameCurrentScene(const char* name)
    {
        return renameScene(_current.scene, name);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // bindings NOTICE WORK-IN-PROGRESS

    // add a block binding (for enable/disable control)
    bool addBlockBinding(uint8_t hwid, uint8_t row, uint8_t block);

    // add a block parameter binding
    bool addBlockParameterBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t paramIndex, bool bindingValueChangesNotSaved = false);

    // add a block property binding
    bool addBlockPropertyBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t propIndex);

    // edit a block parameter binding (change normal or inverted operation)
    bool editBlockBinding(uint8_t hwid, uint8_t row, uint8_t block, bool inverted);

    // edit a block parameter binding (change min and max range)
    bool editBlockParameterBinding(uint8_t hwid,
                                   uint8_t row,
                                   uint8_t block,
                                   uint8_t paramIndex,
                                   float min,
                                   float max);

    // remove all binds for a specific actuator
    bool removeBindings(uint8_t hwid);

    // remove a block binding (for enable/disable control)
    bool removeBlockBinding(uint8_t hwid, uint8_t row, uint8_t block);

    // remove a block parameter binding
    bool removeBlockParameterBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t paramIndex);

    // remove a block parameter binding
    bool removeBlockPropertyBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t propIndex);

    // rename a binding
    bool renameBinding(uint8_t hwid, const char* name);

    // replace a block binding with another (for enable/disable control)
    // the binding to be replaced must already exist
    bool replaceBlockBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t rowB, uint8_t blockB);

    // replace a block parameter binding with another
    // the binding to be replaced must already exist
    bool replaceBlockParameterBinding(uint8_t hwid,
                                      uint8_t row,
                                      uint8_t block,
                                      uint8_t paramIndex,
                                      uint8_t rowB,
                                      uint8_t blockB,
                                      uint8_t paramIndexB,
                                      bool bindingValueChangesNotSaved = false);

    // replace a block property binding with another
    // the binding to be replaced must already exist
    bool replaceBlockPropertyBinding(uint8_t hwid,
                                     uint8_t row,
                                     uint8_t block,
                                     uint8_t propIndex,
                                     uint8_t rowB,
                                     uint8_t blockB,
                                     uint8_t propIndexB);

    // reorder bindings
    bool reorderBlockBinding(uint8_t hwid, uint8_t dest);

    // set all block and parameter bindings to a normalized value
    void setBindingValue(uint8_t hwid, double value, SceneMode sceneMode, bool updateBindings = true, bool markPresetDirty = true);

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

    // set a block parameter value, based on parameter index
    // NOTE value must already be sanitized!
    void setBlockParameter(uint8_t row,
                           uint8_t block,
                           uint8_t paramIndex,
                           float value,
                           SceneMode sceneMode = SceneModeClear,
                           bool markPresetDirty = true);

    // set a block parameter value, based on port symbol
    // NOTE value must already be sanitized!
    void setBlockParameter(uint8_t row,
                           uint8_t block,
                           const char* symbol,
                           float value,
                           SceneMode sceneMode = SceneModeClear,
                           bool markPresetDirty = true);

    // set a block quickpot
    void setBlockQuickPot(uint8_t row, uint8_t block, uint8_t paramIndex);

    // enable monitoring for block output parameter
    bool monitorBlockOutputParameter(uint8_t row, uint8_t block, uint8_t paramIndex, bool enable = true);

    // convenience calls for single-chain builds
   #if NUM_BLOCK_CHAIN_ROWS == 1
    inline void setBlockParameter(const uint8_t block,
                                  const uint8_t paramIndex,
                                  const float value,
                                  const SceneMode sceneMode)
    {
        setBlockParameter(0, block, paramIndex, value, sceneMode);
    }

    inline bool monitorBlockOutputParameter(const uint8_t block, const uint8_t paramIndex, const bool enable = true)
    {
        return monitorBlockOutputParameter(0, block, paramIndex, enable);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // tempo handling NOTICE WORK-IN-PROGRESS

    // set the global beats per bar transport value
    bool setBeatsPerBar(double beatsPerBar);

    // set the global beats per minute transport value
    bool setBeatsPerMinute(double beatsPerMinute);

    // change the global transport state
    bool transport(bool rolling, double beatsPerBar, double beatsPerMinute);

    // ----------------------------------------------------------------------------------------------------------------
    // tool handling NOTICE WORK-IN-PROGRESS

    // enable a "system tool" lv2 plugin (referenced by its URI)
    // passing null or empty string as the URI means disabling the tool
    // NOTE toolIndex must be < 10 and != 5
    bool enableTool(uint8_t toolIndex, const char* uri);

    // connect a tool audio input port to an arbitrary jack output port
    void connectToolAudioInput(uint8_t toolIndex, const char* symbol, const char* jackPort, bool safe = false);

    // connect a tool audio output port to an arbitrary jack input port
    void connectToolAudioOutput(uint8_t toolIndex, const char* symbol, const char* jackPort);

    // connect a tool audio output port to another tool's input port
    void connectTool2Tool(uint8_t toolAIndex,
                          const char* toolAOutSymbol,
                          uint8_t toolBIndex,
                          const char* toolBInSymbol);

    // connect a block output port to a tool input port
    void connectBlock2Tool(uint8_t row,
                           uint8_t block,
                           uint8_t toolIndex,
                           const char* toolInSymbolL,
                           const char* toolInSymbolR = nullptr,
                           const char* toolInSymbolSidechainL = nullptr,
                           const char* toolInSymbolSidechainR = nullptr);

    // connect a block input port to a tool input port
    // this is done by querying what is connected to the block input port,
    // and then making matching connections on the tool input port
    void connectBlockAudioInput2Tool(uint8_t row,
                                     uint8_t block,
                                     uint8_t toolIndex,
                                     const char* toolInSymbolL,
                                     const char* toolInSymbolR = nullptr,
                                     const char* toolInSymbolSidechainL = nullptr,
                                     const char* toolInSymbolSidechainR = nullptr);

    // disconnect all ports from a tool audio port
    void disconnectToolAudioPort(uint8_t toolIndex, const char* symbol);

    // map a tool parameter to a specific MIDI CC
    void mapToolParameterToMIDICC(uint8_t toolIndex,
                                  const char* symbol,
                                  uint8_t channel,
                                  uint8_t cc,
                                  float minimum,
                                  float maximum);

    // unmap a tool parameter from MIDI CC
    void unmapToolParameterFromMIDICC(uint8_t toolIndex, const char* symbol);

    // set a block parameter value
    // NOTE value must already be sanitized!
    void setToolParameter(uint8_t toolIndex, const char* symbol, float value);

    // enable monitoring for tool output parameter
    void monitorToolOutputParameter(uint8_t toolIndex, const char* symbol, bool enable = true);

    // ----------------------------------------------------------------------------------------------------------------
    // properties

    // WIP details below this point

    // set a block property, based on property index
    void setBlockProperty(uint8_t row,
                          uint8_t block,
                          uint8_t propIndex,
                          const char* value,
                          SceneMode sceneMode = SceneModeClear,
                          bool markPresetDirty = true);

    // set a block property, based on property URI
    void setBlockProperty(uint8_t row,
                          uint8_t block,
                          const char* uri,
                          const char* value,
                          SceneMode sceneMode = SceneModeClear,
                          bool markPresetDirty = true);

    // convenience calls for single-chain builds
   #if NUM_BLOCK_CHAIN_ROWS == 1
    inline void setBlockProperty(const uint8_t block,
                                 const uint8_t propIndex,
                                 const char* const value,
                                 const SceneMode sceneMode)
    {
        setBlockProperty(0, block, propIndex, value, sceneMode);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------

    // class to activate non-blocking mode during a function scope, same as Host::NonBlockingScope.
    // NOTE must be used with care! A lot of HostConnector calls already use this and recursion is not supported!
    class NonBlockingScope {
        const Host::NonBlockingScope hnbs;
    public:
        NonBlockingScope(HostConnector& hostconn);
    };

    // same as above, but with audio fade-in and fade-out
    class NonBlockingScopeWithAudioFades {
        const Host::NonBlockingScopeWithAudioFades hnbs;
    public:
        NonBlockingScopeWithAudioFades(HostConnector& hostconn);
    };

    // ----------------------------------------------------------------------------------------------------------------

protected:
    // load host state as stored in the `current` struct
    // also preloads the other presets in the bank
    void hostClearAndLoadCurrentBank();

    void hostConnectAll(uint8_t row, uint8_t blockStart = 0, uint8_t blockEnd = NUM_BLOCKS_PER_PRESET - 1);
    void hostConnectBlockToBlock(uint8_t row, uint8_t blockA, uint8_t blockB);
    void hostConnectBlockToChainInput(uint8_t row, uint8_t block);
    void hostConnectBlockToChainOutput(uint8_t row, uint8_t block);
    void hostConnectChainEndpoints(uint8_t row);

    void hostDisconnectAll(bool disconnectSideChains = false);
    void hostDisconnectAllBlockInputs(uint8_t row, uint8_t block, bool disconnectSideChains = false);
    void hostDisconnectAllBlockOutputs(uint8_t row, uint8_t block, bool disconnectSideChains = false);
    void hostDisconnectAllBlockInputs(const Block& blockdata, const HostBlockPair& hbp, bool disconnectSideChains = false);
    void hostDisconnectAllBlockOutputs(const Block& blockdata, const HostBlockPair& hbp, bool disconnectSideChains = false);
    void hostDisconnectChainEndpoints(uint8_t row);

    void hostEnsureStereoChain(uint8_t preset, uint8_t row, uint8_t blockStart = 0, bool recursive = false);

    void hostSetupSideIO(uint8_t preset, uint8_t row, uint8_t block, HostBlockPair hbp, const Lv2Plugin* plugin);

    // remove all bindings related to a block
    void hostRemoveAllBlockBindings(uint8_t row, uint8_t block);

    void hostRemoveInstanceForBlock(uint8_t row, uint8_t block);

private:
    void hostConnectChainEndpointsAction(uint8_t row, bool connect);
    void hostConnectChainInputAction(uint8_t row, uint8_t block, bool connect);
    void hostConnectChainOutputAction(uint8_t row, uint8_t block, bool connect);
    void hostDisconnectBlockAction(const Block& blockdata, const HostBlockPair& hbp, bool outputs, bool disconnectSideChains);

    // loads preset data, does not trigger host commands
    void jsonPresetLoad(Preset& presetdata, const nlohmann::json& json) const;

    // saves preset data, also no host commands
    void jsonPresetSave(const Preset& presetdata, nlohmann::json& json) const;

    // load preset data from the current bank, only does host commands
    void hostLoadPreset(uint8_t preset);

    // unload "old" and load current preset, only does host commands
    void hostSwitchPreset(const Current& old);

    // add (active==true) or preload block defined by blockdata to instance_number
    bool hostLoadInstance(const Block& blockdata, uint16_t instance_number, bool active);
    // called inside hostLoadInstance
    void hostSetupInstance(const Block& blockdata, uint16_t instance_number);

    // set bypass state of block and its pair if exists
    void hostBypassBlockPair(const HostBlockPair& hbp, bool bypass);

    // remove block instance and its pair if exists
    void hostRemoveBlockPair(const HostBlockPair& hbp);

    // patch_set for block and its pair if exists
    void hostPatchSetBlockPair(const HostBlockPair& hbp, const Property& propdata);

    // params_flush for block and its pair if exists
    void hostParamsFlushBlockPair(const HostBlockPair& hbp, uint8_t reset_value, const std::vector<flushed_param>& params);

    // internal feedback handling, for updating parameter values
    void hostFeedbackCallback(const HostFeedbackData& data) override;

    // init block using plugin default values, optionally fill index maps
    void initBlock(Block& blockdata,
                   const Lv2Plugin* plugin,
                   uint8_t numInputs,
                   uint8_t numOutputs,
                   uint8_t numSideInputs,
                   uint8_t numSideOutputs) const;

    void allocBlock(Block& blockdata) const;
    void resetBlock(Block& blockdata) const;

    void allocPreset(Preset& preset, bool init = true) const;
    void resetPreset(Preset& preset) const;
};

using HostBindings = HostConnector::Bindings;
using HostBlock = HostConnector::Block;
using HostParameter = HostConnector::Parameter;
using HostParameterBinding = HostConnector::ParameterBinding;
using HostProperty = HostConnector::Property;
using HostPropertyBinding = HostConnector::PropertyBinding;
using HostSceneMode = HostConnector::SceneMode;
using HostCallbackData = HostConnector::Callback::Data;
using HostNonBlockingScope = HostConnector::NonBlockingScope;
using HostNonBlockingScopeWithAudioFades = HostConnector::NonBlockingScopeWithAudioFades;

// --------------------------------------------------------------------------------------------------------------------

static inline constexpr bool hasScenes(const HostParameter& param)
{
    return (param.meta.flags & Lv2ParameterInScene) != 0;
}

static inline constexpr bool hasScenes(const HostProperty& prop)
{
    return (prop.meta.flags & Lv2ParameterInScene) != 0;
}

static inline constexpr bool hasScenes(const HostBlock& block)
{
    return block.meta.enable.hasScenes;
}

// --------------------------------------------------------------------------------------------------------------------
