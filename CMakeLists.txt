cmake_minimum_required(VERSION 3.22)
project(raylyrics VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(LIBSOUP REQUIRED libsoup-3.0)
pkg_check_modules(CJSON REQUIRED libcjson)
pkg_check_modules(LUA REQUIRED lua5.4)

find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)

# --- Wayland protocol code generation -------------------------------------
set(WLR_LAYER_SHELL_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml)

pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(XDG_SHELL_XML ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)

set(WAYLAND_PROTOCOL_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${WAYLAND_PROTOCOL_DIR})

function(generate_wayland_protocol xml name)
  set(header ${WAYLAND_PROTOCOL_DIR}/${name}-client-protocol.h)
  set(code ${WAYLAND_PROTOCOL_DIR}/${name}-protocol.c)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${xml} ${header}
    DEPENDS ${xml}
    VERBATIM)
  add_custom_command(
    OUTPUT ${code}
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${xml} ${code}
    DEPENDS ${xml}
    VERBATIM)
  set(${name}_HEADER ${header} PARENT_SCOPE)
  set(${name}_CODE ${code} PARENT_SCOPE)
endfunction()

generate_wayland_protocol(${WLR_LAYER_SHELL_XML} wlr-layer-shell-unstable-v1)
generate_wayland_protocol(${XDG_SHELL_XML} xdg-shell)

add_custom_target(wayland_protocols DEPENDS
  ${wlr-layer-shell-unstable-v1_HEADER}
  ${wlr-layer-shell-unstable-v1_CODE}
  ${xdg-shell_HEADER}
  ${xdg-shell_CODE})

# --- raylib (forked) with the Wayland layer-shell platform -----------------
set(PLATFORM "Wayland" CACHE STRING "Platform to build for." FORCE)
add_subdirectory(vendors/raylib)

# raylib's platform file includes "wayland/wayland_glue.h"
target_include_directories(raylib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- raylyrics ------------------------------------------------------------
add_executable(raylyrics
  main.cpp
  src/config/config.cpp
  src/ipc/control.cpp
  src/lyrics/cache.cpp
  src/lyrics/encoding.cpp
  src/lyrics/lrc.cpp
  src/lyrics/lrclib.cpp
  src/lyrics/match.cpp
  src/lyrics/title_grammar.cpp
  src/media/mpris.cpp
  src/media/player_selection.cpp
  src/plugin/effects.cpp
  src/plugin/plugin.cpp
  src/render/text.cpp
  src/wayland/wayland_glue.c
  ${wlr-layer-shell-unstable-v1_CODE}
  ${xdg-shell_CODE}
)

add_dependencies(raylyrics wayland_protocols)

target_include_directories(raylyrics PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/vendors/slug-raylib
  ${CMAKE_CURRENT_SOURCE_DIR}/vendors/sol2
  ${WAYLAND_PROTOCOL_DIR}
  ${WAYLAND_CLIENT_INCLUDE_DIRS}
  ${WAYLAND_EGL_INCLUDE_DIRS}
  ${FONTCONFIG_INCLUDE_DIRS}
  ${GIO_INCLUDE_DIRS}
  ${LIBSOUP_INCLUDE_DIRS}
  ${CJSON_INCLUDE_DIRS}
  ${LUA_INCLUDE_DIRS}
)

target_compile_options(raylyrics PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)

target_link_libraries(raylyrics PRIVATE
  raylib
  ${WAYLAND_CLIENT_LIBRARIES}
  ${WAYLAND_EGL_LIBRARIES}
  ${FONTCONFIG_LIBRARIES}
  ${GIO_LIBRARIES}
  ${LIBSOUP_LIBRARIES}
  ${CJSON_LIBRARIES}
  ${LUA_LIBRARIES}
  EGL
  GL
  m
  dl
  pthread
)

# --- install ---------------------------------------------------------------
include(GNUInstallDirs)

# Where the system-wide presets live, searched after ~/.config/raylyrics/presets.
target_compile_definitions(raylyrics PRIVATE
  RAYLYRICS_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/raylyrics"
)

install(TARGETS raylyrics RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config.example.lua DESTINATION ${CMAKE_INSTALL_DATADIR}/raylyrics)
install(DIRECTORY presets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/raylyrics/presets
        PATTERN "README.md" EXCLUDE)
