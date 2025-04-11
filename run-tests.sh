#!/bin/bash

cd $(dirname "${0}")

JACKD_CMD_ARGS="-r -d dummy -r 48000 -p 8192"

# build test blocks
make -C test-blocks
export LV2_PATH=$(pwd)/test-blocks/build/

# build mod-connector
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# run tests
echo $(which jackd) $JACKD_CMD_ARGS | tee ~/.jackdrc
./build/tests

# cleanup (in case of failure or crash)
rm -rf test-presets

killall -SIGKILL jackd || true
