// SPDX-FileCopyrightText: 2026 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#include "connector.hpp"

#include <emscripten.h>

class HostConnectorExport : public HostConnector,
                            private HostConnector::Callback
{
public:
    HostConnectorExport()
        : HostConnector(this) {}

private:
    void hostConnectorCallback(const HostCallbackData& data) final
    {
        fprintf(stderr, "hostConnectorCallback %d\n", data.type);
    }

    void hostDisconnectedCallback() final
    {
        fprintf(stderr, "hostDisconnectedCallback\n");
    }
};

static HostConnectorExport* conn = new HostConnectorExport;

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
bool ok()
{
    return conn->ok;
}

__attribute__((used))
void disconnect()
{
    conn->disconnect();
}

__attribute__((used))
bool reconnect()
{
    return conn->reconnect();
}

__attribute__((used))
const char* getLastError()
{
    static std::string ret;
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
    static std::string ret;
    ret = conn->getBlockId(row, block);
    return ret.c_str();
}

__attribute__((used))
const char* getBlockIdNoPair(uint8_t row, uint8_t block)
{
    static std::string ret;
    ret = conn->getBlockIdNoPair(row, block);
    return ret.c_str();
}

__attribute__((used))
const char* getBlockIdPairOnly(uint8_t row, uint8_t block)
{
    static std::string ret;
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
    static std::string ret;
    ret = conn->serializeCurrentPreset();
    return ret.c_str();
}

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
    return conn->current.block(row, block).meta.quickPotIndex;
}

__attribute__((used))
const char* getBlockQuickPotSymbol(uint8_t row, uint8_t block)
{
    return conn->current.block(row, block).quickPotSymbol.c_str();
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

// --------------------------------------------------------------------------------------------------------------------
// bank handling

__attribute__((used))
void loadBankFromPresetFiles(const char* filename1,
                             const char* filename2,
                             const char* filename3,
                             uint8_t initialPresetToLoad = 0)
{
    const std::array<std::string, NUM_PRESETS_PER_BANK> filenames = { filename1, filename2, filename3 };
    conn->loadBankFromPresetFiles(filenames, initialPresetToLoad);
}

// --------------------------------------------------------------------------------------------------------------------
// preset handling

__attribute__((used))
const char* getPresetNameFromFile(const char* filename)
{
    static std::string ret;
    ret = ::getPresetNameFromFile(filename);
    return ret.c_str();
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
void setCurrentPresetName(const char* name)
{
    conn->setCurrentPresetName(name);
}

// __attribute__((used))
// void setCurrentPresetFilename(const char* filename)
// {
//     setPresetFilename(_current.preset, filename);
// }

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

// __attribute__((used))
// inline bool renameCurrentScene(const char* name)
// {
//     return renameScene(_current.scene, name);
// }

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
    conn->setBlockParameter(row, block, symbol, value, sceneMode);
}

__attribute__((used))
void setBlockQuickPot(uint8_t row, uint8_t block, uint8_t paramIndex)
{
    conn->setBlockQuickPot(row, block, paramIndex);
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
void disconnectToolAudioPort(uint8_t toolIndex, const char* symbol)
{
    conn->disconnectToolAudioPort(toolIndex, symbol);
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

} // extern "C"
