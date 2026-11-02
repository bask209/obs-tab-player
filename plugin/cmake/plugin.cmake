# SPDX-License-Identifier: Apache-2.0
# The OBS plugin module. Needs libobs, obs-frontend-api and Qt6 (CMAKE_PREFIX_PATH on Windows and
# macOS, see tools/obs-sdk.*; on Linux the OBS package provides them).
if(APPLE)
  # On macOS libobs is a framework: look in <prefix>/Frameworks too.
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_FRAMEWORK_PATH "${prefix}/Frameworks")
  endforeach()
endif()
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
target_link_libraries(obs-tab-player PRIVATE tab-core tab-render OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets)

if(MSVC)
  # Hardening: warnings as errors, SDL checks, Control Flow Guard, CET shadow stack, ASLR, DEP.
  target_compile_options(obs-tab-player PRIVATE /WX /sdl /guard:cf)
  target_compile_options(tab-core PRIVATE /WX /sdl /guard:cf)
  target_compile_options(tab-render PRIVATE /WX /sdl /guard:cf)
  target_link_options(obs-tab-player PRIVATE /guard:cf /CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
else()
  # Hardening on GCC/Clang: stack protector, fortified libc calls, position-independent code.
  foreach(t obs-tab-player tab-core tab-render)
    target_compile_options(${t} PRIVATE -fstack-protector-strong -fvisibility=hidden $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>)
    set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endforeach()
endif()

if(APPLE)
  # A .plugin bundle: the module in Contents/MacOS, the plugin's data in Contents/Resources.
  set_target_properties(obs-tab-player PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_BUNDLE_NAME obs-tab-player
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.bask209.obs-tab-player
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  add_custom_command(TARGET obs-tab-player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_BUNDLE_CONTENT_DIR:obs-tab-player>/Resources)
  install(TARGETS obs-tab-player LIBRARY DESTINATION . BUNDLE DESTINATION .)
else()
  # The layout OBS 30+ loads from its plugins folders on Windows (%ProgramData%\obs-studio\plugins)
  # and Linux (~/.config/obs-studio/plugins): obs-tab-player/bin/64bit and obs-tab-player/data.
  install(TARGETS obs-tab-player LIBRARY DESTINATION obs-tab-player/bin/64bit RUNTIME DESTINATION obs-tab-player/bin/64bit)
  install(DIRECTORY data/ DESTINATION obs-tab-player/data)
endif()
