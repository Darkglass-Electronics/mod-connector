cmake_minimum_required(VERSION 3.15)

set(MAIN_PROJECT_NAME "mod-connector-routings-test")
project(${MAIN_PROJECT_NAME})

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

set(MOD_CONNECTOR_LIB_TEST TRUE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt)

add_executable(${MAIN_PROJECT_NAME} main.cpp)

target_link_libraries(${MAIN_PROJECT_NAME}
    PRIVATE
        mod::connector
)

target_include_directories( ${MAIN_PROJECT_NAME} 
    PRIVATE 
        ../../src    
)

target_sources(${MAIN_PROJECT_NAME}
    PRIVATE
        main.cpp
)
