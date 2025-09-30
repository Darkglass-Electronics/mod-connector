cmake_minimum_required(VERSION 3.15)
project(mod-connector-examples)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(MOD_CONNECTOR_TESTS TRUE)
include(${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt)

# Import example dependencies
include(FetchContent)
FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.3.0
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE
)
FetchContent_MakeAvailable(lvgl)

find_package(SDL2)

add_executable(lvgl-slider)

target_compile_definitions(lvgl-slider
    PUBLIC
       NUM_BLOCK_CHAIN_ROWS=1
)

target_link_libraries(lvgl-slider
    PRIVATE
        lvgl::lvgl
        mod::connector
        SDL2
)

target_sources(lvgl-slider
    PRIVATE
        lvgl-slider.cpp
)
