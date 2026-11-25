# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
#
# Switchres (GPL-2.0-or-later, Antonio Giner and others), for arcade_screen.h: the modes made for an arcade monitor at
# the board's exact rate. Downloaded (its release's archive, checked against its SHA-256), compiled as it is and linked
# in statically. Windows only for now (TODO: Linux, with its KMS and xrandr backends, which need libdrm's and Xrandr's
# headers in the build image). A port:
#
#   include(recomp-common/switchres.cmake)
#   recomp_switchres(<target>)      (every program that compiles recomp-common/arcade_screen.cpp; nothing off Windows)
include_guard(GLOBAL)

if (WIN32)
    include(FetchContent)
    FetchContent_Declare(switchres
        URL https://github.com/antonioginer/switchres/archive/refs/tags/v2.2.2.tar.gz
        URL_HASH SHA256=8abda61c21f375e08f0f2dcaaa105f072e892bdd639417316c0290d7b6b80027
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(switchres) # (its sources only: it has no CMakeLists.txt, and its own build, a makefile, is not used)
    set(RECOMP_SWITCHRES_SOURCES monitor modeline switchres display custom_video log edid switchres_wrapper display_windows
        custom_video_ati_family custom_video_ati custom_video_adl custom_video_pstrip resync_windows)
    list(TRANSFORM RECOMP_SWITCHRES_SOURCES PREPEND "${switchres_SOURCE_DIR}/")
    list(TRANSFORM RECOMP_SWITCHRES_SOURCES APPEND .cpp)
    add_library(switchres STATIC ${RECOMP_SWITCHRES_SOURCES})
    target_compile_definitions(switchres PRIVATE SR_WIN32_STATIC)
    target_compile_options(switchres PRIVATE -w)
    target_link_libraries(switchres PRIVATE gdi32 user32 advapi32)
    target_include_directories(switchres INTERFACE "${switchres_SOURCE_DIR}")
endif ()

function(recomp_switchres target)
    if (WIN32)
        target_link_libraries(${target} PRIVATE switchres)
    endif ()
endfunction()
