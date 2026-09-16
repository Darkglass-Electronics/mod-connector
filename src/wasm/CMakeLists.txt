cmake_minimum_required(VERSION 3.15)
project(mod-connector-wasm)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

set(MOD_CONNECTOR_LIB_ONLY TRUE)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt)

add_executable(mod-connector-wasm)

# set_target_properties(mod-connector-wasm
#     PROPERTIES
#         SUFFIX ".js"
# )

target_compile_definitions(mod-connector-wasm
    PRIVATE
        _DARKGLASS_DEVICE_PABLITO
        NUM_BINDING_ACTUATORS=10
        NUM_BLOCK_CHAIN_ROWS=2
        NUM_BLOCKS_PER_PRESET=12
        NUM_SCENES_PER_PRESET=126
)

target_compile_options(mod-connector-wasm
    PRIVATE
        -fexceptions
        -O3
)

target_link_libraries(mod-connector-wasm
    PRIVATE
        mod::connector
)

target_link_options(mod-connector-wasm
    PRIVATE
        # -Wl,--whole-archive
        # -Wl,--no-whole-archive
        --no-entry
        --preload-file=${CMAKE_SOURCE_DIR}/lv2@/usr/lib/lv2
        -fexceptions
        -lwebsocket.js
        -sAGGRESSIVE_VARIABLE_ELIMINATION=1
        -sALLOW_MEMORY_GROWTH
        # -sALLOW_TABLE_GROWTH
        # -sASSERTIONS
        -sASYNCIFY
        -sASYNCIFY_IMPORTS=do_sleep
        -sENVIRONMENT=web
        -sEXPORT_NAME="connector"
        -sEXPORTED_RUNTIME_METHODS=['ccall']
        -sINITIAL_MEMORY=8Mb
        -sLLD_REPORT_UNDEFINED
        -sMAIN_MODULE=2
        -sMODULARIZE=1
)

target_sources(mod-connector-wasm
    PRIVATE
        wasm-exports.cpp
)
