#!/bin/bash

set -e

cd $(dirname "${0}")/../..

mkdir -p deps

###############################################################################
# setup emscripten

# make emscripten quiet
export EMSDK_QUIET=1

# use a known good emscripten version
EMSDK_VERSION=6.0.13

# setup emscripten
if [ ! -e emsdk ]; then
    git clone --depth=1 https://github.com/emscripten-core/emsdk.git emsdk
fi

if [ ! -e "emsdk/upstream/bin" ]; then
    # --build="${CMAKE_BUILD_TYPE}"
    echo "Installing emsdk..."
    ./emsdk/emsdk install ${EMSDK_VERSION}
    echo "Activating emsdk..."
    ./emsdk/emsdk activate ${EMSDK_VERSION}
fi

# activate emscripten environment
source "${PWD}/emsdk/emsdk_env.sh"

###############################################################################
# setup build

mkdir -p build-wasm

if [ "$(uname -s)" = "Darwin" ]; then
    NUMJOBS=$(sysctl -n hw.logicalcpu)
else
    NUMJOBS=$(nproc)
fi

emcmake cmake -S src/wasm -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j ${NUMJOBS}

###############################################################################
# test build

# emrun src/wasm/wasm-test.html

###############################################################################
