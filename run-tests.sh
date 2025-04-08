#!/bin/bash

JACKD_CMD_ARGS="-r -d dummy -r 48000 -p 8192"

# build test blocks
make -C test-blocks
export LV2_PATH=$(pwd)/test-blocks/build/

# build mod-connector
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# temp directory for preset files
mkdir test-presets

# run tests
echo $(which jackd) $JACKD_CMD_ARGS | tee ~/.jackdrc
./build/tests

rm -rf test-presets