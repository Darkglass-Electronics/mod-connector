
// globals
let ws;
let g_metadata = {
    categories: [],
    plugins: [],
};
let g_state = {
    bank: 1,
    preset: 1, // NOTE resets to 0 on bank change
    banks: {
        1: {
            presets: {
                1: {
                    blocks: {
                        "1": { "name": "-", "uri": "-" },
                        "2": { "name": "-", "uri": "-" },
                        "3": { "name": "-", "uri": "-" },
                        "4": { "name": "-", "uri": "-" },
                        "5": { "name": "-", "uri": "-" },
                        "6": { "name": "-", "uri": "-" },
                    },
                },
            },
        },
    },
};

function wsconnect() {
    ws = new WebSocket("ws://" + (window.location.hostname || "localhost") + ":13371/websocket")

    ws.onclose = function () {
        // console.log("onclose", ws)

        if (ws === false) {
            return;
        }

        // retry in 1s
        setTimeout(wsconnect, 1000)
    }

    ws.onerror = function (evt) {
        console.log("onerror", evt)
        // set as false to signal error
        ws = false;
    }

    ws.onmessage = function (evt) {
        // console.log("onmessage", evt)
        const data = JSON.parse(evt.data)
        // console.log("onmessage", data)

        switch (data.type) {
        case 'state':
            g_metadata.categories = data.categories;
            g_metadata.plugins = data.plugins;

            // websocket server state can be partial, so be careful when importing it!
            if (data.state) {
                if (data.state.banks !== undefined) {
                    for (var b in data.state.banks) {
                        const databank = data.state.banks[b];
                        if (databank.presets !== undefined) {
                            for (var pr in databank.presets) {
                                const datapreset = databank.presets[pr];
                                if (datapreset.blocks !== undefined) {
                                    for (var bl in datapreset.blocks) {
                                        const datablock = datapreset.blocks[bl];
                                        g_state.banks[b].presets[pr].blocks[bl] = datablock;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // update page to display retrieved content
            update_metadata()
            update_state()
            break;
        }
    }

    ws.onopen = function () {
        // ws.send("init");
    }
}

// utility functions
// NOTE partial/missing properties are ignored on webserver

// send full state
function ws_send_state(state) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({
            type: 'state',
            state: state,
        }));
        return true;
    }
    return false;
}

// send blocks
function ws_send_blocks(blocks) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        console.log("sending live blocks", blocks);
        ws.send(JSON.stringify({
            type: 'state',
            state: {
                "banks": {
                    [g_state.bank]: {
                        presets: {
                            [g_state.preset]: {
                                blocks: blocks
                            }
                        }
                    }
                }
            }
        }));
        return true;
    }
    return false;
}
