#!/bin/bash

set -e

cd $(dirname "${0}")/../..

###############################################################################
# setup PawPaw (builds extra static libs)

export PAWPAW_FAST_MATH=1
export PAWPAW_SKIP_FFTW=1
export PAWPAW_SKIP_FLUIDSYNTH=1
export PAWPAW_SKIP_GLIB=1
export PAWPAW_SKIP_SAMPLERATE=1

mkdir -p build-wasm
[ -d build-wasm/PawPaw ] || git -C build-wasm clone https://github.com/DISTRHO/PawPaw.git

./build-wasm/PawPaw/bootstrap-plugins.sh wasm

###############################################################################
# setup build

source build-wasm/PawPaw/local.env wasm

if [ "$(uname -s)" = "Darwin" ]; then
    NUMJOBS=$(sysctl -n hw.logicalcpu)
else
    NUMJOBS=$(nproc)
fi

emcmake cmake -S src/wasm -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j ${NUMJOBS}

###############################################################################
# test build

emrun src/wasm/wasm-test.html

###############################################################################
