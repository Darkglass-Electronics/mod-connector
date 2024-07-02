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
# Find deps

find_package(PkgConfig REQUIRED)
pkg_check_modules(lilv REQUIRED IMPORTED_TARGET lilv-0)
pkg_check_modules(lv2_1_18 QUIET IMPORTED_TARGET lv2>=1.18)
if (lv2_1_18_FOUND)
  add_library(PkgConfig::lv2 ALIAS PkgConfig::lv2_1_18)
else()
  pkg_check_modules(lv2 REQUIRED IMPORTED_TARGET lv2)
endif()

#######################################################################################################################
# Setup connector target

# check if we are building from this project, or are imported by another
if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)

  # building as regular application, uses Qt for event loop and notifies startup via systemd
  pkg_check_modules(systemd IMPORTED_TARGET libsystemd)

  find_package(Qt6 QUIET COMPONENTS Core Network WebSockets)
  if (Qt6_FOUND)
    add_library(Qt::Core ALIAS Qt6::Core)
    add_library(Qt::Network ALIAS Qt6::Network)
    add_library(Qt::WebSockets ALIAS Qt6::WebSockets)
  else()
    find_package(Qt5 REQUIRED COMPONENTS Core Network WebSockets)
    add_library(Qt::Core ALIAS Qt5::Core)
    add_library(Qt::Network ALIAS Qt5::Network)
    add_library(Qt::WebSockets ALIAS Qt5::WebSockets)
  endif()

  add_executable(mod-connector)

  set_target_properties(mod-connector
    PROPERTIES
      AUTOMOC ON
      AUTOUIC ON
      AUTORCC ON
  )

  target_compile_definitions(mod-connector
    PRIVATE
      $<$<BOOL:${lv2_1_18_FOUND}>:HAVE_LV2_1_18>
      $<$<BOOL:${systemd_FOUND}>:HAVE_SYSTEMD>
      # TODO remove this once host.cpp does not need Qt anymore
      WITH_QT
  )

  target_include_directories(mod-connector
    PRIVATE
      src
  )

  target_link_libraries(mod-connector
    PRIVATE
      PkgConfig::lilv
      PkgConfig::lv2
      $<$<BOOL:${systemd_FOUND}>:PkgConfig::systemd>
      Qt::Core
      Qt::Network
      Qt::WebSockets
  )

  target_sources(mod-connector
    PRIVATE
      src/connector.cpp
      src/host.cpp
      src/lv2.cpp
      src/main.cpp
      src/utils.cpp
      src/websocket.cpp
  )

else()

  # building as interface library
  add_library(mod-connector INTERFACE)
  add_library(mod::connector ALIAS mod-connector)

  target_compile_definitions(mod-connector
    INTERFACE
      $<$<BOOL:${lv2_1_18_FOUND}>:HAVE_LV2_1_18>
  )

  target_include_directories(mod-connector
    INTERFACE
      src
  )

  target_link_libraries(mod-connector
    INTERFACE
      PkgConfig::lilv
      PkgConfig::lv2
  )

  target_sources(mod-connector
    INTERFACE
      src/connector.cpp
      src/host.cpp
      src/lv2.cpp
      src/utils.cpp
  )

endif()

#######################################################################################################################
