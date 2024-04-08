cmake_minimum_required(VERSION 3.15)
project(mod-connector)

set(CMAKE_POLICY_DEFAULT_CMP0025 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)

set_property(GLOBAL PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

#######################################################################################################################
# Find Qt

find_package(PkgConfig REQUIRED)
find_package(Qt6 COMPONENTS REQUIRED Core Network)

#######################################################################################################################
# Setup Chibi target

add_executable(mod-connector)

set_target_properties(mod-connector
  PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_compile_definitions(mod-connector
  PRIVATE
    BUILDING_CARLA
)

target_include_directories(mod-connector
  PRIVATE
    src
)

target_link_libraries(mod-connector
  PUBLIC
    Qt6::Core
    Qt6::Network
)

target_sources(mod-connector
  PRIVATE
    src/host.cpp
    src/main.cpp
)

#######################################################################################################################
