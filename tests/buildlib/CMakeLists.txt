cmake_minimum_required(VERSION 3.15)
project(mod-connector-lib-test)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

set(MOD_CONNECTOR_TESTS TRUE)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt)

add_executable(mod-connector-lib-test main.cpp)

target_link_libraries(mod-connector-lib-test
    PRIVATE
        mod::connector
)

target_sources(mod-connector-lib-test
    PRIVATE
        main.cpp
)
