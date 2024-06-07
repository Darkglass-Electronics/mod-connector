# mod-connector

This is a little project for a websocket server that:

- serves LV2 plugin information to clients
- stores received data from clients in a persistent way
- translates that data into [mod-host](https://github.com/moddevices/mod-host) commands

mod-connector uses `/websocket` entry point in port 13371.  
Connecting clients will receive the current state as json with this data model:

```
{
    "type": "state",
    "plugins": [
        {
            "uri": "",
            "name": "",
            "category": "",
            "ports": [
                {
                    "symbol": "in",
                    "name": "In",
                    "flag": ["audio"]
                },
                {
                    "symbol": "out",
                    "name": "Out",
                    "flag": ["audio","output"]
                },
                {
                    "symbol": "param",
                    "name": "Param",
                    "flag": ["control"],
                    "default": 0.0,
                    "minimum": 0.0,
                    "maximum": 1.0,
                }
            ]
        }
    ],
    "state": {
        "bank": 1,
        "banks": {
            "1": {
                "chain": {
                    "1": {
                        "binding": "1",
                        "name": "Gain",
                        "parameters": {
                            "1": {
                                "maximum": -20,
                                "minimum": 20,
                                "name": "Gain",
                                "symbol": "gain",
                                "value": 0
                            },
                            // ...
                        },
                        "uri": "http://kxstudio.sf.net/carla/plugins/audiogain"
                    }
                }
            }
        }
    }
}
```

## Building

This project uses cmake.
You can configure and build it in release mode by using:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build dependencies are:

- cmake >= 3.15
- lilv
- LV2
- Qt with QtWebsockets (either Qt5 or Qt6)
- systemd (optional, enables "notify" systemd event)

## Environment

The behaviour of this tool can be controlled with the following environment variables:

### `LV2_PATH`

Path to where LV2 plugins are stored.

### `MOD_DEV_HOST`

Do not create a connection to `mod-host`, simulate one by returning success to all host queries.  
Can be used during development or when just wanting to have the non-host functionality from this tool.

### `MOD_DEVICE_HOST_PORT`

The TCP port the tool expects `mod-host` to be using, defaults to 5555 if unset.
