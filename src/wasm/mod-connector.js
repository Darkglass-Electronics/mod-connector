// global definitions
const kSceneModeClear = 1;

// create connector object with full API access
// requires wasm module to already be loaded and initialized
// also an IPC backend which implements `editorSend`, `editorSendMulti` and `editorPoll`
const connector_init = (backend, module) => {
    // collect all functions from the module
    const calls = {
        test_string_return: module.cwrap("test_string_return", "string", []),
        test_string_send: module.cwrap("test_string_send", "void", ["string"]),

        init: module.cwrap("init", "boolean", ["number", "number", "number", "number", "number"]),
        ok: module.cwrap("ok", "boolean", []),

        disconnect: module.cwrap("disconnect", "void", []),
        getLastError: module.cwrap("getLastError", "string", []),
        monitorBlocksCPULoad: module.cwrap("monitorBlocksCPULoad", "void", ["boolean"], {async: true}),
        monitorMidiControl: module.cwrap("monitorMidiControl", "boolean", ["number", "boolean"], {async: true}),
        monitorMidiProgram: module.cwrap("monitorMidiProgram", "boolean", ["number", "boolean"], {async: true}),
        midiOut: module.cwrap("midiOut", "boolean", ["number", "number"], {async: true}),
        pollHostUpdates: module.cwrap("pollHostUpdates", "void", [], {async: true}),
        requestHostUpdates: module.cwrap("requestHostUpdates", "void", [], {async: true}),
        waitAudioCycle: module.cwrap("waitAudioCycle", "void", [], {async: true}),

        getBlockId: module.cwrap("getBlockId", "string", ["number", "number"]),
        getBlockIdNoPair: module.cwrap("getBlockIdNoPair", "string", ["number", "number"]),
        getBlockIdPairOnly: module.cwrap("getBlockIdPairOnly", "string", ["number", "number"]),
        printStateForDebug: module.cwrap("printStateForDebug", "string", ["boolean", "boolean", "boolean"]),
        serializeCurrentPreset: module.cwrap("serializeCurrentPreset", "string", []),
        deserializeToCurrentPreset: module.cwrap("deserializeToCurrentPreset", "void", ["string"]),
    };

    // async handling, collecting data to be used later
    let _pendingFeedbackMessages = [];

    const connector = {
        // TESTING
        test_string_return: module.cwrap("test_string_return", "string", []),
        test_string_send: module.cwrap("test_string_send", "void", ["string"]),

        // wasm-specific async call: start connection, leaving remote in blocked state
        // returns remote preset state if successful
        connect: (hostCallback) => {
            return new Promise((success, reject) => {
                backend.editorOpen()
                .then(reply => {
                    const state = reply.payload.state;

                    // helper for allocating a string to give to the C/wasm side
                    let c_strdupLen = 0;
                    let c_strdupRet = 0;
                    const c_strdup = (str) => {
                        if (c_strdupRet != 0) {
                            module._free(c_strdupRet);
                        }
                        c_strdupLen = module.lengthBytesUTF8(str) + 1;
                        c_strdupRet = module._malloc(c_strdupLen);
                        if (c_strdupRet != 0) {
                            module.stringToUTF8(str, c_strdupRet, c_strdupLen);
                        } else {
                            c_strdupLen = 0;
                        }
                        return c_strdupRet;
                    };

                    // async handling, collecting data to be used later
                    let _pendingNonBlockingMessages = [];
                    let _pendingResponses = [];

                    /* leave it for now, useful if we run full simulator on web context
                    let _outputDataReady = false;
                    */

                    // C-compatible function to send a message over IPC
                    // If blocking is 1 we wait for a direct reply, otherwise collect messages to send later
                    const sendFn = module.addFunction((_, c_msg, blocking) => {
                        const msg = module.UTF8ToString(c_msg);
                        return module.Asyncify.handleAsync(() => {
                            return new Promise((successAsync, _) => {
                                /* intentionally keep out messages that might happen too fast
                                if (msg == "output_data_ready") {
                                    // assert(blocking);
                                    // assert(_pendingNonBlockingMessages.empty());
                                    // _outputDataReady = true;
                                    _pendingResponses.push("resp 0");
                                    successAsync(1);
                                    return;
                                }
                                */

                                if (blocking) {
                                    backend.editorSend(msg).then((reply) => {
                                        _pendingResponses.push(reply.payload.response);
                                        successAsync(1);
                                    }).catch(() => successAsync(0));
                                } else {
                                    _pendingNonBlockingMessages.push(msg);
                                    successAsync(1);
                                }
                            });
                        });
                    }, 'iiii');

                    // C-compatible function to fetch a reply over IPC
                    // To be compatible with our backend we already have a reply ready to go
                    // Exception for the non-blocking messages:
                    // we send all the pending messages together over IPC and get a single reply
                    const replyFn = module.addFunction((_, c_size) => {
                        return module.Asyncify.handleAsync(() => {
                            return new Promise((successAsync, _) => {
                                const replyWithFirstResponse = () => {
                                    if (_pendingResponses.length === 0) {
                                        module.HEAPU32[c_size / module.HEAPU32.BYTES_PER_ELEMENT] = 0;
                                        successAsync(0);
                                        return;
                                    }
                                    const resp = _pendingResponses[0];
                                    _pendingResponses = _pendingResponses.slice(1);
                                    c_strdup(resp);
                                    module.HEAPU32[c_size / module.HEAPU32.BYTES_PER_ELEMENT] = c_strdupLen;
                                    successAsync(c_strdupRet);
                                };

                                if (_pendingNonBlockingMessages.length != 0) {
                                    const messages = _pendingNonBlockingMessages;
                                    const numMessages = messages.length;
                                    _pendingNonBlockingMessages = [];
                                
                                    // NOTE needs a big timeout
                                    backend.editorSendMulti(messages).then(() => {
                                        for (let i = 0; i < numMessages; ++i) {
                                            _pendingResponses.push("resp 0");
                                        }
                                        replyWithFirstResponse();
                                    }).catch(() => successAsync(0));
                                    return;
                                }

                                replyWithFirstResponse();
                            });
                        });
                    }, 'iii');

                    const feedbackFn = module.addFunction((_, c_size) => {
                        if (_pendingFeedbackMessages.length === 0) {
                            module.HEAPU32[c_size / module.HEAPU32.BYTES_PER_ELEMENT] = 0;
                            return 0;
                        }
                        const msg = _pendingFeedbackMessages[0];
                        _pendingFeedbackMessages = _pendingFeedbackMessages.slice(1);
                        c_strdup(msg);
                        module.HEAPU32[c_size / module.HEAPU32.BYTES_PER_ELEMENT] = c_strdupLen;
                        return c_strdupRet;
                    }, 'iii');

                    const hostCallbackFn = module.addFunction(c_data => {
                        hostCallback(JSON.parse(module.UTF8ToString(c_data)));
                    }, 'vi');

                    module.__preventDeletion = [sendFn, replyFn, feedbackFn, hostCallbackFn];

                    if (! calls.init(sendFn, replyFn, feedbackFn, hostCallbackFn, 0)) {
                        reject('Failed to initialize virtual host');
                        return;
                    }

                    calls.deserializeToCurrentPreset(JSON.stringify(state));

                    success(state);
                })
                .catch(reject);
            });
        },

        // wasm-specific async call: stop connection, unblocking the remote
        // after being called the remote should be considered disconnected even if an error occurs
        disconnect: () => {
            return new Promise((success, reject) => {
                calls.disconnect();
                backend.editorClose().then(success).catch(reject);
            });
        },

        ok: calls.ok,
        getLastError: calls.getLastError,
        monitorBlocksCPULoad: calls.monitorBlocksCPULoad,
        monitorMidiControl: calls.monitorMidiControl,
        monitorMidiProgram: calls.monitorMidiProgram,
        midiOut: (data) => {
            // TODO
            // calls.midiOut(size, data);
        },
        pollHostUpdates: async () => {
            // await backend.editorSend("output_data_ready");
            const resp = await backend.editorPoll();
            _pendingFeedbackMessages = _pendingFeedbackMessages.concat(resp.payload.data);
            return calls.pollHostUpdates();
        },
        requestHostUpdates: calls.requestHostUpdates,
        waitAudioCycle: calls.waitAudioCycle,

        getBlockId: calls.getBlockId,
        getBlockIdNoPair: calls.getBlockIdNoPair,
        getBlockIdPairOnly: calls.getBlockIdPairOnly,
        printStateForDebug: calls.printStateForDebug,
        serializeCurrentPreset: calls.serializeCurrentPreset,
        deserializeToCurrentPreset: calls.deserializeToCurrentPreset,

        // TODO write them all!
        getAverageCpuLoad: module.cwrap("getAverageCpuLoad", "number", [], {async: true}),
        getMaximumCpuLoad: module.cwrap("getMaximumCpuLoad", "number", [], {async: true}),
        loadBankFromPresetFiles: (f1, f2, f3, init = 0) => module.ccall("loadBankFromPresetFiles", "boolean", ["string", "string", "string", "number"], [f1, f2, f3, init], {async: true}),
        clearCurrentPreset: module.cwrap("clearCurrentPreset", "void", [], {async: true}),
        switchPreset: module.cwrap("switchPreset", "boolean", ["number"], {async: true}),
        setJackPorts: module.cwrap("setJackPorts", "boolean", ["string", "string", "string", "string"], {async: true}),
        replaceBlock: (row, block, uri, clearBindingsForReplacementBlock = true, keepCurrentData = false) =>
            module.ccall("replaceBlock", "boolean", ["number", "number", "string", "boolean", "boolean"], [row, block, uri, clearBindingsForReplacementBlock, keepCurrentData], {async: true}),
        setBlockParameterBySymbol: (row, block, symbol, value, sceneMode = kSceneModeClear) =>
            module.ccall("setBlockParameterBySymbol", "void", ["number", "number", "string", "double", "number"], [row, block, symbol, value, sceneMode], {async: true}),
    };

    return connector;
};
