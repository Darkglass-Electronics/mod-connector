#!/bin/bash

set -e

#build test program
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

rm -rf results
mkdir results

# start backends
jackd -ddummy > /dev/null 2>&1 &
JACKD_PID=$!
sleep .5
mod-host -n -p 5555 -f 5556 > /dev/null 2>&1 &
MOD_HOST_PID=$!
sleep .5

# export MOD_LOG=3

#run tests
./build/mod-connector-routings-test ./results

#stop backends
kill $JACKD_PID
kill $MOD_HOST_PID