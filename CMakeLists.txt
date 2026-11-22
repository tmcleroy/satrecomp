# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
cmake_minimum_required(VERSION 3.28)
project(satrecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# Game selection: the game id names a directory under games/ and the generated code under work/
set(GAME "fighting_vipers" CACHE STRING "Game id (directory under games/)")
set(GAME_DIR "${CMAKE_SOURCE_DIR}/../games/${GAME}" CACHE PATH "The game's directory (game.cfg, src/)")
set(WORK_DIR "${CMAKE_SOURCE_DIR}/../work/${GAME}" CACHE PATH "Generated code directory")
# The game's combined buttons (game.cfg's `combos`, port/bindings.h), for the game and the launcher alike
set(GAME_COMBOS "")
if (EXISTS "${GAME_DIR}/game.cfg")
    file(STRINGS "${GAME_DIR}/game.cfg" GAME_COMBOS REGEX "^combos[ \t]*=")
    string(REGEX REPLACE "^combos[ \t]*=[ \t]*" "" GAME_COMBOS "${GAME_COMBOS}")
endif ()
# The game's buttons on a touch screen (game.cfg's `touch_buttons`, named as `combos` names them: port/touch.h);
# without, the Saturn's own
set(GAME_TOUCH "")
if (EXISTS "${GAME_DIR}/game.cfg")
    file(STRINGS "${GAME_DIR}/game.cfg" GAME_TOUCH REGEX "^touch_buttons[ \t]*=")
    string(REGEX REPLACE "^touch_buttons[ \t]*=[ \t]*" "" GAME_TOUCH "${GAME_TOUCH}")
endif ()

find_package(SDL3 REQUIRED CONFIG)
find_package(ZLIB REQUIRED)

# Translated game code: work/<game>/gen/blocks_*.cpp, written by tools/recomp.py.
# Without it the game still builds and runs everything in the interpreter.
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${WORK_DIR}/gen/blocks_*.cpp")
if (GEN_SOURCES)
    message(STATUS "satrecomp: ${GAME}: translated code found in ${WORK_DIR}/gen")
else ()
    message(STATUS "satrecomp: ${GAME}: no translated code yet (interpreter only)")
    set(GEN_SOURCES runtime/no_blocks.cpp)
endif ()

# The game's own sources (lifted routines); without any, nothing is lifted
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS "${GAME_DIR}/src/*.cpp")
if (NOT GAME_SOURCES)
    set(GAME_SOURCES runtime/no_game.cpp)
endif ()

# The game, on satrecomp's own machine (machine/). No emulator is compiled or linked into it.
# (Android: a library, which the game's activity loads and calls the main of: android/, port/android.cpp)
set(PLATFORM_SOURCES "")
if (ANDROID)
    add_library(${GAME} SHARED)
    set(PLATFORM_SOURCES port/android.cpp)
else ()
    add_executable(${GAME})
endif ()
target_sources(${GAME} PRIVATE
    ${GAME_SOURCES}
    ${PLATFORM_SOURCES}
    port/main.cpp port/touch.cpp
    port/disc_files.cpp port/disc_image.cpp port/soundtrack.cpp port/decoders.cpp
    port/frontend_sdl.cpp port/shading.cpp port/menu.cpp port/pictures.cpp port/hud.cpp recomp-common/menu.cpp recomp-common/settings.cpp
    video/renderer.cpp video/regs.cpp video/layers.cpp video/vdp1.cpp video/raster.cpp video/gpu.cpp
    sound/chip.cpp sound/m68k.cpp sound/state.cpp
    machine/sh2.cpp machine/machine.cpp machine/vdp.cpp machine/cd.cpp machine/state.cpp
    runtime/music.cpp runtime/shared_lifts.cpp runtime/backup.cpp runtime/game.cpp
    runtime/recomp.cpp
    runtime/hle_bios.cpp
    ${GEN_SOURCES}
)
target_compile_definitions(${GAME} PRIVATE SATRECOMP_OWN_MACHINE "SATRECOMP_COMBOS=\"${GAME_COMBOS}\"" "SATRECOMP_TOUCH=\"${GAME_TOUCH}\"")
target_include_directories(${GAME} PRIVATE runtime port "${WORK_DIR}/gen")
target_link_libraries(${GAME} PRIVATE SDL3::SDL3 ZLIB::ZLIB)
if (ANDROID)
    # (the system's log; and calls of the library's own functions kept inside it, not through the PLT: as
    # Soulcalibur II's Android build found, that is several per cent of a frame)
    target_link_libraries(${GAME} PRIVATE log android)
    target_compile_options(${GAME} PRIVATE -fno-semantic-interposition)
    target_link_options(${GAME} PRIVATE -Wl,-Bsymbolic)
endif ()
target_compile_options(${GAME} PRIVATE -Wno-unused-parameter)

# The reference build (-DSATRECOMP_REFERENCE=ON, 'make ref'): the same game code on the Ymir hardware model,
# a development tool to compare with. It has Ymir's interpreters and renderers, which the checks of the
# translated code, the video renderer, the sound chip and the sound CPU run side by side with ours.
option(SATRECOMP_REFERENCE "Also build <game>-ref, on the Ymir hardware model" OFF)
if (SATRECOMP_REFERENCE)
# Saturn hardware (and the reference interpreter): Ymir's core library, with the reference patch applied (build.mk, 'setup').
# Its top-level CMakeLists needs Xcode's Metal compiler for GPU shaders this port doesn't use, so the
# core library and its vendored dependencies are added directly, with shader compilation stubbed out.
set(YMIR_DIR "${CMAKE_SOURCE_DIR}/third_party/Ymir")
set(Ymir_LIBRARY_ONLY ON)
set(Ymir_ENABLE_DEVLOG OFF)
set(Ymir_ENABLE_DEV_ASSERTIONS OFF)
set(Ymir_ENABLE_IPO OFF)
set(Ymir_DEV_BUILD OFF)
set(Ymir_EXTRA_INLINING ON)
set(Ymir_FF_HOST_CD_DRIVES OFF)
set(Ymir_VERSION "0.4.0")
set(Ymir_VERSION_MAJOR 0)
set(Ymir_VERSION_MINOR 4)
set(Ymir_VERSION_PATCH 0)
set(BUILD_SHARED_LIBS OFF)
set(ARCHITECTURES "${CMAKE_SYSTEM_PROCESSOR}")
include("${YMIR_DIR}/cmake/CMakeRC.cmake")
include("${YMIR_DIR}/cmake/TargetConditionalSources.cmake" OPTIONAL)
function(compile_shader)
    cmake_parse_arguments(ARG "" "OUT_PATHS" "" ${ARGN})
    set(${ARG_OUT_PATHS} "" PARENT_SCOPE)
endfunction()
add_subdirectory("${YMIR_DIR}/vendor" ymir-vendor EXCLUDE_FROM_ALL)
add_subdirectory("${YMIR_DIR}/libs/ymir-core" ymir-core EXCLUDE_FROM_ALL)


    file(GLOB REF_GEN_SOURCES CONFIGURE_DEPENDS "${WORK_DIR}/gen-ref/blocks_*.cpp")
    if (NOT REF_GEN_SOURCES)
        set(REF_GEN_SOURCES runtime/no_blocks.cpp)
    endif ()
    # The sound driver as blocks of Ymir's 68000 handlers: work/<game>/gen-ref/m68k_*.cpp (tools/recomp68k.py)
    file(GLOB REF_GEN_M68K_SOURCES CONFIGURE_DEPENDS "${WORK_DIR}/gen-ref/m68k_*.cpp")
    if (NOT REF_GEN_M68K_SOURCES)
        set(REF_GEN_M68K_SOURCES runtime/no_m68k_blocks.cpp)
    endif ()
    add_executable(${GAME}-ref
        ${GAME_SOURCES}
        port/main.cpp port/touch.cpp
        port/disc_files.cpp port/disc_image.cpp port/soundtrack.cpp port/decoders.cpp
        port/video_stats.cpp
        port/frontend_sdl.cpp port/shading.cpp port/menu.cpp port/pictures.cpp port/hud.cpp recomp-common/menu.cpp recomp-common/settings.cpp
        video/renderer.cpp video/regs.cpp video/ymir_adapter.cpp video/layers.cpp video/vdp1.cpp video/raster.cpp video/gpu.cpp
        sound/generator.cpp sound/chip.cpp sound/m68k.cpp sound/state.cpp
        runtime/music.cpp runtime/shared_lifts.cpp runtime/game.cpp
        runtime/recomp.cpp
        runtime/hle_bios.cpp
        runtime/m68k_recomp.cpp
        ${REF_GEN_M68K_SOURCES}
        ${REF_GEN_SOURCES}
    )
    target_include_directories(${GAME}-ref PRIVATE runtime port "${WORK_DIR}/gen-ref" third_party/Ymir/libs/ymir-core/src)
    target_link_libraries(${GAME}-ref PRIVATE ymir::ymir-core SDL3::SDL3 ZLIB::ZLIB)
    target_compile_options(${GAME}-ref PRIVATE -Wno-unused-parameter)
endif ()

# .chd disc images (port/disc_image.cpp): libchdr, with the system's zlib, which the game links already. (Added
# after the reference build's emulator, which may bring its own copy: then that one is used.)
set(CHDR_USE_SYSTEM_ZLIB ON)
include(port/libchdr.cmake)
target_link_libraries(${GAME} PRIVATE chdr-static)
if (TARGET ${GAME}-ref)
    target_link_libraries(${GAME}-ref PRIVATE chdr-static)
endif ()
