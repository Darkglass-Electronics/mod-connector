// SPDX-FileCopyrightText: 2026 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#define JSON_NO_IO

#include "connector.hpp"
#include "ipc.hpp"
#include "json.hpp"

#if NUM_BLOCK_CHAIN_ROWS != 2
#error this code expects NUM_BLOCK_CHAIN_ROWS == 2
#endif
#if NUM_PRESETS_PER_BANK != 3
#error this code expects NUM_PRESETS_PER_BANK == 3
#endif

#include <emscripten.h>

typedef void (*ExportedHostCallback)(const char* data);

class HostConnectorExport : public HostConnector,
                            private HostConnector::Callback
{
public:
    HostConnectorExport(IPC::SendCallback send,
                        IPC::RecvCallback reply,
                        IPC::RecvCallback feedback,
                        ExportedHostCallback callback,
                        void* userPtr = nullptr)
        : HostConnector(this, IPC::createDualCallbackIPC(send, reply, feedback, userPtr)),
          _callback(callback) {}

private:
    const ExportedHostCallback _callback;
    std::string _cbdata;

    static nlohmann::json HostPatchDataJSON(const char type, const HostPatchData& data)
    {
        nlohmann::json j;
        switch (type)
        {
        case 'b':
        case 'i':
            j["value"] = data.i;
            break;
        case 'l':
            j["value"] = data.l;
            break;
        case 'f':
            j["value"] = data.f;
            break;
        case 'g':
            j["value"] = data.g;
            break;
        case 's':
        case 'p':
        case 'u':
            j["value"] = data.s;
            break;
        case 'v':
            j["type"] = data.v.type;
            switch (data.v.type)
            {
            case 'b':
            case 'i':
                j["value"] = std::vector<int32_t>(data.v.data.i, data.v.data.i + data.v.num);
                break;
            case 'l':
                j["value"] = std::vector<int64_t>(data.v.data.l, data.v.data.l + data.v.num);
                break;
            case 'f':
                j["value"] = std::vector<float>(data.v.data.f, data.v.data.f + data.v.num);
                break;
            case 'g':
                j["value"] = std::vector<double>(data.v.data.g, data.v.data.g + data.v.num);
                break;
            }
            break;
        }
        return j;
    }

    void hostConnectorCallback(const HostCallbackData& data) final
    {
        nlohmann::json j;
        j["type"] = data.type;
        switch (data.type)
        {
        case HostCallbackData::kAudioMonitor:
            j["index"] = data.audioMonitor.index;
            j["value"] = data.audioMonitor.value;
            break;
        case HostCallbackData::kCpuLoad:
            j["avg"] = data.cpuLoad.avg;
            j["max"] = data.cpuLoad.max;
            j["xruns"] = data.cpuLoad.xruns;
            break;
        case HostCallbackData::kCpuMonitor:
            j["row"] = data.cpuMonitor.row;
            j["block"] = data.cpuMonitor.block;
            j["cpuLoad"] = data.cpuMonitor.cpuLoad;
            break;
        case HostCallbackData::kLog:
            j["type"] = data.log.type;
            j["msg"] = data.log.msg;
            break;
        case HostCallbackData::kParameterSet:
            j["row"] = data.parameterSet.row;
            j["block"] = data.parameterSet.block;
            j["index"] = data.parameterSet.index;
            j["symbol"] = data.parameterSet.symbol;
            j["value"] = data.parameterSet.value;
            break;
        case HostCallbackData::kParameterState:
            j["row"] = data.parameterState.row;
            j["block"] = data.parameterState.block;
            j["index"] = data.parameterState.index;
            j["symbol"] = data.parameterState.symbol;
            j["state"] = data.parameterState.state;
            break;
        case HostCallbackData::kPatchSet:
            j["row"] = data.patchSet.row;
            j["block"] = data.patchSet.block;
            j["key"] = data.patchSet.key;
            j["type"] = data.patchSet.type;
            j["data"] = HostPatchDataJSON(data.patchSet.type, data.patchSet.data);
            break;
        case HostCallbackData::kToolParameterSet:
            j["index"] = data.toolParameterSet.index;
            j["symbol"] = data.toolParameterSet.symbol;
            j["value"] = data.toolParameterSet.value;
            break;
        case HostCallbackData::kToolPatchSet:
            j["index"] = data.toolPatchSet.index;
            j["key"] = data.toolPatchSet.key;
            j["type"] = data.toolPatchSet.type;
            j["data"] = HostPatchDataJSON(data.toolPatchSet.type, data.toolPatchSet.data);
            break;
        case HostCallbackData::kMidiControlChange:
            j["channel"] = data.midiControlChange.channel;
            j["control"] = data.midiControlChange.control;
            j["value"] = data.midiControlChange.value;
            break;
        case HostCallbackData::kMidiProgramChange:
            j["channel"] = data.midiProgramChange.channel;
            j["program"] = data.midiProgramChange.program;
            break;
        }
        _cbdata = j.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace);
        _callback(_cbdata.c_str());
    }

    void hostDisconnectedCallback() final
    {
        fprintf(stderr, "hostDisconnectedCallback\n");
    }
};

static HostConnectorExport* conn;
static std::string ret;

// TODO apply "__attribute__((used))" to all functions at once

extern "C" {

// --------------------------------------------------------------------------------------------------------------------

__attribute__((used))
const char* test_string_return()
{
    return "This is a test string to verify that JS side can receive strings properly, does it work?";
}

__attribute__((used))
void test_string_send(const char* s)
{
    fprintf(stderr, "We are on C++ side now! String is: '%s'\n", s);
}

// --------------------------------------------------------------------------------------------------------------------

__attribute__((used))
bool init(IPC::SendCallback send, IPC::RecvCallback reply, IPC::RecvCallback feedback, ExportedHostCallback callback)
{
    assert_return(conn == nullptr, false);
    conn = new HostConnectorExport(send, reply, feedback, callback);
    return true;
}

__attribute__((used))
bool ok()
{
    return conn != nullptr && conn->ok;
}

// --------------------------------------------------------------------------------------------------------------------

__attribute__((used))
void disconnect()
{
    conn->disconnect();
}

__attribute__((used))
const char* getLastError()
{
    ret = conn->getLastError();
    return ret.c_str();
}

__attribute__((used))
void monitorBlocksCPULoad(bool enable)
{
    conn->monitorBlocksCPULoad(enable);
}

__attribute__((used))
bool monitorMidiControl(uint8_t midiChannel, bool enable)
{
    return conn->monitorMidiControl(midiChannel, enable);
}

__attribute__((used))
bool monitorMidiProgram(uint8_t midiChannel, bool enable)
{
    return conn->monitorMidiProgram(midiChannel, enable);
}

__attribute__((used))
bool midiOut(uint8_t size, const uint8_t* data)
{
    return conn->midiOut(size, data);
}

__attribute__((used))
void pollHostUpdates()
{
    conn->pollHostUpdates();
}

__attribute__((used))
void requestHostUpdates()
{
    conn->requestHostUpdates();
}

__attribute__((used))
void waitAudioCycle()
{
    conn->waitAudioCycle();
}

// --------------------------------------------------------------------------------------------------------------------
// debug helpers

__attribute__((used))
const char* getBlockId(uint8_t row, uint8_t block)
{
    ret = conn->getBlockId(row, block);
    return ret.c_str();
}

__attribute__((used))
const char* getBlockIdNoPair(uint8_t row, uint8_t block)
{
    ret = conn->getBlockIdNoPair(row, block);
    return ret.c_str();
}

__attribute__((used))
const char* getBlockIdPairOnly(uint8_t row, uint8_t block)
{
    ret = conn->getBlockIdPairOnly(row, block);
    return ret.c_str();
}

__attribute__((used))
void printStateForDebug(bool withBlocks, bool withParams, bool withBindings)
{
    conn->printStateForDebug(withBlocks, withParams, withBindings);
}

// --------------------------------------------------------------------------------------------------------------------
// wasm helpers

__attribute__((used))
const char* serializeCurrentPreset()
{
    ret = conn->serializeCurrentPreset();
    return ret.c_str();
}

__attribute__((used))
void deserializeToCurrentPreset(const char* data)
{
    conn->deserializeToCurrentPreset(nlohmann::json::parse(data));
}

// --------------------------------------------------------------------------------------------------------------------
// cpu load handling

__attribute__((used))
void enableCpuLoadUpdates(bool enable)
{
    conn->enableCpuLoadUpdates(enable);
}

__attribute__((used))
float getAverageCpuLoad()
{
    return conn->getAverageCpuLoad();
}

__attribute__((used))
float getMaximumCpuLoad()
{
    return conn->getMaximumCpuLoad();
}

// --------------------------------------------------------------------------------------------------------------------
// current state handling

// __attribute__((used))
// const Preset& getBankPreset(uint8_t preset)

// __attribute__((used))
// const Preset& getCurrentPreset(uint8_t preset)

__attribute__((used))
bool canAddSidechainInput(uint8_t row, uint8_t block)
{
    return conn->canAddSidechainInput(row, block);
}

__attribute__((used))
bool canAddSidechainOutput(uint8_t row, uint8_t block)
{
    return conn->canAddSidechainOutput(row, block);
}

__attribute__((used))
bool setJackPorts(const char* capture1, const char* capture2, const char* playback1, const char* playback2)
{
    const std::array<std::string, 2> capture = { capture1, capture2 };
    const std::array<std::string, 2> playback = { playback1, playback2 };
    return conn->setJackPorts(capture, playback);
}

__attribute__((used))
void hostReady()
{
    conn->hostReady();
}

__attribute__((used))
void enableAudioProcessing(bool enable)
{
    conn->enableAudioProcessing(enable);
}

__attribute__((used))
void setDirty(bool dirty = true)
{
    conn->setDirty(dirty);
}

// TODO
// __attribute__((used))
// void setMetadataValue(const std::string& key, const nlohmann::json& value)
// {
//     setMetadataValue(_current.preset, key, value);
// }

// TODO
// __attribute__((used))
// void setMetadataValue(uint8_t preset, const std::string& key, const nlohmann::json& value);

// --------------------------------------------------------------------------------------------------------------------
// bank handling

__attribute__((used))
void loadBankFromPresetFiles(const char* filename1,
                             const char* filename2,
                             const char* filename3,
                             uint8_t initialPresetToLoad = 0,
                             bool discardMetadata = false)
{
    const std::array<std::string, NUM_PRESETS_PER_BANK> filenames = { filename1, filename2, filename3 };
    conn->loadBankFromPresetFiles(filenames, initialPresetToLoad, discardMetadata);
}

// --------------------------------------------------------------------------------------------------------------------
// preset handling

__attribute__((used))
const char* getPresetNameFromFile(const char* filename)
{
    ret = HostConnector::getPresetNameFromFile(filename);
    return ret.c_str();
}

// TODO
// static std::optional<nlohmann::json> getPresetMetadataFromFile(const char* filename, const std::string& key = {});

// TODO
// static bool updatePresetMetadataInFile(const char* filename, const std::string& key, const nlohmann::json& value);

// TODO
__attribute__((used))
bool updatePresetNameInFile(const char* filename, const char* name)
{
    return HostConnector::updatePresetNameInFile(filename, name);
}

__attribute__((used))
bool loadCurrentPresetFromFile(const char* filename, bool replaceDefault)
{
    return conn->loadCurrentPresetFromFile(filename, replaceDefault);
}

__attribute__((used))
bool preloadPresetFromFile(uint8_t preset, const char* filename)
{
    return conn->preloadPresetFromFile(preset, filename);
}

__attribute__((used))
bool saveCurrentPresetToFile(const char* filename)
{
    return conn->saveCurrentPresetToFile(filename);
}

__attribute__((used))
bool reorderPresets(uint8_t orig, uint8_t dest)
{
    return conn->reorderPresets(orig, dest);
}

__attribute__((used))
void swapPresets(uint8_t presetA, uint8_t presetB, bool swapFiles = true)
{
    conn->swapPresets(presetA, presetB, swapFiles);
}

__attribute__((used))
bool saveCurrentPreset()
{
    return conn->saveCurrentPreset();
}

__attribute__((used))
void clearCurrentPreset()
{
    conn->clearCurrentPreset();
}

__attribute__((used))
void clearCurrentPresetBackground()
{
    conn->clearCurrentPresetBackground();
}

__attribute__((used))
void regenUUID()
{
    conn->regenUUID();
}

__attribute__((used))
void setPresetFilename(uint8_t preset, const char* filename)
{
    conn->setPresetFilename(preset, filename);
}

__attribute__((used))
void setPresetName(uint8_t preset, const char* name)
{
    conn->setPresetName(preset, name);
}

__attribute__((used))
void setCurrentPresetFilename(const char* filename)
{
    conn->setCurrentPresetFilename(filename);
}

__attribute__((used))
void setCurrentPresetName(const char* name)
{
    conn->setCurrentPresetName(name);
}

__attribute__((used))
bool switchPreset(uint8_t preset)
{
    return conn->switchPreset(preset);
}

__attribute__((used))
void renamePreset(uint8_t preset, const char* name)
{
    conn->renamePreset(preset, name);
}

// --------------------------------------------------------------------------------------------------------------------
// block handling

__attribute__((used))
bool enableBlock(uint8_t row, uint8_t block, bool enable, HostSceneMode sceneMode)
{
    return conn->enableBlock(row, block, enable, sceneMode);
}

__attribute__((used))
bool reorderBlock(uint8_t row, uint8_t orig, uint8_t dest)
{
    return conn->reorderBlock(row, orig, dest);
}

__attribute__((used))
bool replaceBlock(uint8_t row,
                  uint8_t block,
                  const char* uri,
                  bool clearBindingsForReplacementBlock = true,
                  bool keepCurrentData = false)
{
    return conn->replaceBlock(row, block, uri, clearBindingsForReplacementBlock, keepCurrentData);
}

__attribute__((used))
bool replaceBlockWhileKeepingCurrentData(uint8_t row, uint8_t block, const char* uri)
{
    return conn->replaceBlockWhileKeepingCurrentData(row, block, uri);
}

__attribute__((used))
bool resetBlock(uint8_t row, uint8_t block, bool resetUserDefaults = false)
{
    return conn->resetBlock(row, block, resetUserDefaults);
}

__attribute__((used))
bool saveBlockStateAsDefault(uint8_t row, uint8_t block)
{
    return conn->saveBlockStateAsDefault(row, block);
}

#if NUM_BLOCK_CHAIN_ROWS > 1
__attribute__((used))
bool swapBlockRow(uint8_t row, uint8_t block, uint8_t emptyRow, uint8_t emptyBlock)
{
    return conn->swapBlockRow(row, block, emptyRow, emptyBlock);
}
#endif

// --------------------------------------------------------------------------------------------------------------------
// scene handling (within the current preset)

__attribute__((used))
void clearAllScenes()
{
    conn->clearAllScenes();
}

__attribute__((used))
void clearScene(uint8_t scene)
{
    conn->clearScene(scene);
}

__attribute__((used))
bool copyScene(uint8_t orig, uint8_t dest)
{
    return conn->copyScene(orig, dest);
}

__attribute__((used))
bool reorderScenes(uint8_t orig, uint8_t dest)
{
    return conn->reorderScenes(orig, dest);
}

__attribute__((used))
void swapScenes(uint8_t sceneA, uint8_t sceneB)
{
    conn->swapScenes(sceneA, sceneB);
}

__attribute__((used))
bool switchScene(uint8_t scene, bool switchEvenIfSameScene = false, bool discardIfUnused = true)
{
    return conn->switchScene(scene, switchEvenIfSameScene, discardIfUnused);
}

__attribute__((used))
bool renameScene(uint8_t scene, const char* name)
{
    return conn->renameScene(scene, name);
}

__attribute__((used))
bool renameCurrentScene(const char* name)
{
    return conn->renameCurrentScene(name);
}

// --------------------------------------------------------------------------------------------------------------------
// bindings NOTICE WORK-IN-PROGRESS

__attribute__((used))
bool addBlockBinding(uint8_t hwid, uint8_t row, uint8_t block)
{
    return conn->addBlockBinding(hwid, row, block);
}

__attribute__((used))
bool addBlockParameterBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t paramIndex)
{
    return conn->addBlockParameterBinding(hwid, row, block, paramIndex);
}

__attribute__((used))
bool addBlockParameterBindingBySymbol(uint8_t hwid, uint8_t row, uint8_t block, const char* symbol)
{
    return conn->addBlockParameterBinding(hwid, row, block, symbol);
}

__attribute__((used))
bool editBlockBinding(uint8_t hwid, uint8_t row, uint8_t block, bool inverted)
{
    return conn->editBlockBinding(hwid, row, block, inverted);
}

__attribute__((used))
bool editBlockParameterBinding(uint8_t hwid,
                               uint8_t row,
                               uint8_t block,
                               uint8_t paramIndex,
                               float min,
                               float max)
{
    return conn->editBlockParameterBinding(hwid, row, block, paramIndex, min, max);
}

__attribute__((used))
bool editBlockParameterBindingBySymbol(uint8_t hwid,
                                       uint8_t row,
                                       uint8_t block,
                                       const char* symbol,
                                       float min,
                                       float max)
{
    return conn->editBlockParameterBinding(hwid, row, block, symbol, min, max);
}

__attribute__((used))
bool removeBindings(uint8_t hwid)
{
    return conn->removeBindings(hwid);
}

__attribute__((used))
bool removeBlockBinding(uint8_t hwid, uint8_t row, uint8_t block)
{
    return conn->removeBlockBinding(hwid, row, block);
}

__attribute__((used))
bool removeBlockParameterBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t paramIndex)
{
    return conn->removeBlockParameterBinding(hwid, row, block, paramIndex);
}

__attribute__((used))
bool removeBlockParameterBindingBySymbol(uint8_t hwid, uint8_t row, uint8_t block, const char* symbol)
{
    return conn->removeBlockParameterBinding(hwid, row, block, symbol);
}

__attribute__((used))
bool renameBinding(uint8_t hwid, const char* name)
{
    return conn->renameBinding(hwid, name);
}

__attribute__((used))
bool replaceBlockBinding(uint8_t hwid, uint8_t row, uint8_t block, uint8_t rowB, uint8_t blockB)
{
    return conn->replaceBlockBinding(hwid, row, block, rowB, blockB);
}

__attribute__((used))
bool replaceBlockParameterBinding(uint8_t hwid,
                                  uint8_t row,
                                  uint8_t block,
                                  uint8_t paramIndex,
                                  uint8_t rowB,
                                  uint8_t blockB,
                                  uint8_t paramIndexB)
{
    return conn->replaceBlockParameterBinding(hwid, row, block, paramIndex, rowB, blockB, paramIndexB);
}

__attribute__((used))
bool replaceBlockParameterBindingBySymbol(uint8_t hwid,
                                          uint8_t row,
                                          uint8_t block,
                                          const char* symbol,
                                          uint8_t rowB,
                                          uint8_t blockB,
                                          const char* symbolB)
{
    return conn->replaceBlockParameterBinding(hwid, row, block, symbol, rowB, blockB, symbolB);
}

__attribute__((used))
bool reorderBlockBinding(uint8_t hwid, uint8_t dest)
{
    return conn->reorderBlockBinding(hwid, dest);
}

__attribute__((used))
void setBindingValue(uint8_t hwid, double value, HostSceneMode sceneMode, bool updateBindings = true)
{
    conn->setBindingValue(hwid, value, sceneMode, updateBindings);
}

// --------------------------------------------------------------------------------------------------------------------
// parameters

__attribute__((used))
void setBlockParameter(uint8_t row,
                       uint8_t block,
                       uint8_t paramIndex,
                       float value,
                       HostSceneMode sceneMode = HostConnector::kSceneModeClear)
{
    conn->setBlockParameter(row, block, paramIndex, value, sceneMode);
}

__attribute__((used))
void setBlockParameterBySymbol(uint8_t row,
                               uint8_t block,
                               const char* symbol,
                               float value,
                               HostSceneMode sceneMode = HostConnector::kSceneModeClear)
{
    fprintf(stderr, "sending %u %u %s %f\n", row, block, symbol, value);
    conn->setBlockParameter(row, block, symbol, value, sceneMode);
    fprintf(stderr, "sending %u %u %s %f -> DONE\n", row, block, symbol, value);
}

__attribute__((used))
void setBlockQuickpot(uint8_t row, uint8_t block, uint8_t paramIndex)
{
    conn->setBlockQuickpot(row, block, paramIndex);
}

__attribute__((used))
void setBlockQuickpotBySymbol(uint8_t row, uint8_t block, const char* symbol)
{
    conn->setBlockQuickpot(row, block, symbol);
}

__attribute__((used))
bool monitorBlockOutputParameter(uint8_t row, uint8_t block, uint8_t paramIndex, bool enable = true)
{
    return conn->monitorBlockOutputParameter(row, block, paramIndex, enable);
}

// --------------------------------------------------------------------------------------------------------------------
// tempo handling NOTICE WORK-IN-PROGRESS

__attribute__((used))
bool setBeatsPerBar(double beatsPerBar)
{
    return conn->setBeatsPerBar(beatsPerBar);
}

__attribute__((used))
bool setBeatsPerMinute(double beatsPerMinute)
{
    return conn->setBeatsPerMinute(beatsPerMinute);
}

__attribute__((used))
bool transport(bool rolling, double beatsPerBar, double beatsPerMinute)
{
    return conn->transport(rolling, beatsPerBar, beatsPerMinute);
}

// --------------------------------------------------------------------------------------------------------------------
// tool handling NOTICE WORK-IN-PROGRESS

__attribute__((used))
bool enableTool(uint8_t toolIndex, const char* uri, bool prerun = false)
{
    return conn->enableTool(toolIndex, uri, prerun);
}

__attribute__((used))
void connectToolAudioInput(uint8_t toolIndex, const char* symbol, const char* jackPort, bool safe = false)
{
    conn->connectToolAudioInput(toolIndex, symbol, jackPort, safe);
}

__attribute__((used))
void connectToolAudioOutput(uint8_t toolIndex, const char* symbol, const char* jackPort)
{
    conn->connectToolAudioOutput(toolIndex, symbol, jackPort);
}

__attribute__((used))
void connectTool2Tool(uint8_t toolAIndex,
                      const char* toolAOutSymbol,
                      uint8_t toolBIndex,
                      const char* toolBInSymbol)
{
    conn->connectTool2Tool(toolAIndex, toolAOutSymbol, toolBIndex, toolBInSymbol);
}

__attribute__((used))
void connectBlock2Tool(uint8_t row,
                       uint8_t block,
                       uint8_t toolIndex,
                       const char* toolInSymbolL,
                       const char* toolInSymbolR = nullptr,
                       const char* toolInSymbolSidechainL = nullptr,
                       const char* toolInSymbolSidechainR = nullptr)
{
    conn->connectBlock2Tool(row, block, toolIndex, toolInSymbolL, toolInSymbolR, toolInSymbolSidechainL, toolInSymbolSidechainR);
}

__attribute__((used))
void connectBlockAudioInput2Tool(uint8_t row,
                                 uint8_t block,
                                 uint8_t toolIndex,
                                 const char* toolInSymbolL,
                                 const char* toolInSymbolR = nullptr,
                                 const char* toolInSymbolSidechainL = nullptr,
                                 const char* toolInSymbolSidechainR = nullptr)
{
    conn->connectBlockAudioInput2Tool(row, block, toolIndex, toolInSymbolL, toolInSymbolR, toolInSymbolSidechainL, toolInSymbolSidechainR);
}

__attribute__((used))
void connectJackPorts(const char* jackPortA, const char* jackPortB)
{
    conn->connectJackPorts(jackPortA, jackPortB);
}

__attribute__((used))
void disconnectToolAudioPort(uint8_t toolIndex, const char* symbol)
{
    conn->disconnectToolAudioPort(toolIndex, symbol);
}

__attribute__((used))
void disconnectJackPort(const char* jackPort)
{
    conn->disconnectJackPort(jackPort);
}

__attribute__((used))
void mapToolParameterToMIDICC(uint8_t toolIndex,
                              const char* symbol,
                              uint8_t channel,
                              uint8_t cc,
                              float minimum,
                              float maximum)
{
    conn->mapToolParameterToMIDICC(toolIndex, symbol, channel, cc, minimum, maximum);
}

__attribute__((used))
void unmapToolParameterFromMIDICC(uint8_t toolIndex, const char* symbol)
{
    conn->unmapToolParameterFromMIDICC(toolIndex, symbol);
}

__attribute__((used))
void setToolParameter(uint8_t toolIndex, const char* symbol, float value)
{
    conn->setToolParameter(toolIndex, symbol, value);
}

__attribute__((used))
void monitorToolOutputParameter(uint8_t toolIndex, const char* symbol, bool enable = true)
{
    conn->monitorToolOutputParameter(toolIndex, symbol, enable);
}

// --------------------------------------------------------------------------------------------------------------------
// properties

__attribute__((used))
void setBlockProperty(uint8_t row, uint8_t block, uint8_t propIndex, const char* value)
{
    conn->setBlockProperty(row, block, propIndex, value);
}

__attribute__((used))
void setBlockPropertyByURI(uint8_t row, uint8_t block, const char* uri, const char* value)
{
    conn->setBlockProperty(row, block, uri, value);
}

// --------------------------------------------------------------------------------------------------------------------
// wasm extras, not part of official API

__attribute__((used))
float getBlockParameter(uint8_t row, uint8_t block, uint8_t paramIndex)
{
    return conn->current.block(row, block).parameters[paramIndex].value;
}

__attribute__((used))
float getBlockParameterBySymbol(uint8_t row, uint8_t block, const char* symbol)
{
    const HostBlock& blockdata = conn->current.block(row, block);
    if (uint8_t paramIndex = blockdata.parameterIndexForSymbol(symbol); paramIndex != UINT8_MAX)
        return blockdata.parameters[paramIndex].value;
    return 0.f;
}

__attribute__((used))
uint8_t getBlockQuickPotIndex(uint8_t row, uint8_t block)
{
    return conn->current.block(row, block).meta.quickpotIndex;
}

__attribute__((used))
const char* getBlockQuickPotSymbol(uint8_t row, uint8_t block)
{
    return conn->current.block(row, block).quickpotSymbol.c_str();
}

// --------------------------------------------------------------------------------------------------------------------

} // extern "C"
