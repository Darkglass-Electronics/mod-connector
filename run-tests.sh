JACKD_CMD_ARGS="-r -d dummy -r 48000 -p 8192"

# build test blocks
pushd test-blocks
make
ln -s $(pwd)/build/*.lv2 ~/.lv2/
popd

# build mod-connector
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# run tests
echo $(which jackd) $JACKD_CMD_ARGS | tee ~/.jackdrc
./build/tests