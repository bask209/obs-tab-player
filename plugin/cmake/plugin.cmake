# SPDX-License-Identifier: Apache-2.0
# The OBS plugin module. Needs libobs, obs-frontend-api and Qt6 (CMAKE_PREFIX_PATH).
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(obs-tab-player MODULE
  src/plugin-main.cpp
  src/obs/engine.hpp src/obs/engine.cpp
  src/obs/tab_source.hpp src/obs/tab_source.cpp
  src/ui/dock.hpp src/ui/dock.cpp
)
set_target_properties(obs-tab-player PROPERTIES AUTOMOC ON PREFIX "")
target_link_libraries(obs-tab-player PRIVATE tab-core tab-render OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)

if(MSVC)
  # Hardening: warnings as errors, SDL checks, Control Flow Guard, CET shadow stack, ASLR, DEP.
  target_compile_options(obs-tab-player PRIVATE /WX /sdl /guard:cf)
  target_compile_options(tab-core PRIVATE /WX /sdl /guard:cf)
  target_compile_options(tab-render PRIVATE /WX /sdl /guard:cf)
  target_link_options(obs-tab-player PRIVATE /guard:cf /CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
endif()

# Install layout used by OBS 30+ (%ProgramData%\obs-studio\plugins\obs-tab-player).
install(TARGETS obs-tab-player LIBRARY DESTINATION obs-tab-player/bin/64bit RUNTIME DESTINATION obs-tab-player/bin/64bit)
install(DIRECTORY data/ DESTINATION obs-tab-player/data)
