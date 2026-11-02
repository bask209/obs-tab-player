# SPDX-License-Identifier: Apache-2.0
# OBS Tab Player: native OBS plugin (Windows, macOS, Linux) + engine tools.
#
#   Engine and tools only (no OBS SDK needed):
#     cmake -S plugin -B build -DTAB_BUILD_PLUGIN=OFF && cmake --build build --config Release
#   Plugin (needs the OBS SDK, frontend API and Qt6, e.g. from obs-deps):
#     cmake -S plugin -B build -DCMAKE_PREFIX_PATH="<obs sdk>;<qt6>" && cmake --build build --config Release
cmake_minimum_required(VERSION 3.28)
project(obs-tab-player VERSION 0.4.0 LANGUAGES C CXX)

option(TAB_BUILD_PLUGIN "Build the OBS plugin" ON)
option(TAB_BUILD_TOOLS "Build tabtool (engine developer tool)" ON)
option(TAB_SANITIZE "Build engine and tools with AddressSanitizer" OFF)
option(TAB_FUZZ "Build the libFuzzer target (implies AddressSanitizer)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus /MP)
  if(TAB_SANITIZE OR TAB_FUZZ)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)
  if(TAB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# Portable engine: readers, timeline, layout, synth.
add_library(tab-core STATIC
  src/core/model.hpp src/core/model.cpp
  src/core/readers.hpp src/core/gp3to5.cpp src/core/containers.cpp src/core/gpif.cpp
  src/core/text.hpp src/core/text.cpp
  src/core/timeline.hpp src/core/timeline.cpp
  src/core/layout.hpp src/core/layout.cpp
  src/core/json.hpp src/core/json.cpp
  src/core/synth.hpp src/core/synth.cpp
  src/core/library.hpp src/core/library.cpp
)
target_include_directories(tab-core PUBLIC src)
set_target_properties(tab-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Renderer: Qt's painter (the plugin uses OBS's own Qt). Tools without Qt still build, without
# the commands that draw.
find_package(Qt6 QUIET COMPONENTS Gui)
if(Qt6Gui_FOUND)
  add_library(tab-render STATIC src/render/renderer.hpp src/render/themes.cpp src/render/qt_renderer.cpp)
  target_link_libraries(tab-render PUBLIC tab-core Qt6::Gui)
endif()

if(TAB_BUILD_TOOLS)
  add_executable(tabtool tests/tabtool.cpp)
  target_link_libraries(tabtool PRIVATE tab-core)
  if(TARGET tab-render)
    target_compile_definitions(tabtool PRIVATE TAB_WITH_RENDERER)
    target_link_libraries(tabtool PRIVATE tab-render)
  endif()
endif()

if(TAB_FUZZ AND MSVC)
  add_executable(tab-fuzz tests/fuzz_load.cpp)
  target_compile_options(tab-fuzz PRIVATE /fsanitize=fuzzer)
  target_link_libraries(tab-fuzz PRIVATE tab-core)
endif()

if(TAB_BUILD_PLUGIN)
  include(cmake/plugin.cmake)
endif()
