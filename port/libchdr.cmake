# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# libchdr (BSD-3-Clause, THIRD_PARTY_NOTICES.md): reads .chd disc images (port/disc_image.cpp). Downloaded at
# configure time at a pinned commit, checked against its hash, and built as a static library: chdr-static.
# Include once; a build that has it already (the reference build's emulator brings its own) keeps that one.
# CHDR_USE_SYSTEM_ZLIB: link the system's zlib (the game links it anyway) rather than libchdr's own copy.
include_guard(GLOBAL)
include(FetchContent)

function(satrecomp_libchdr)
    if (TARGET chdr-static)
        return()
    endif ()
    # (libchdr's options as plain variables, only in this function's scope: not its tests, not a shared library)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(CHDR_WANT_TESTS OFF)
    set(INSTALL_STATIC_LIBS OFF)
    set(WITH_SYSTEM_ZLIB ${CHDR_USE_SYSTEM_ZLIB})
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    FetchContent_Declare(libchdr
        URL https://github.com/rtissera/libchdr/archive/47d794151b2b64f6aba60304e6480f4f5a2bc24b.tar.gz
        URL_HASH SHA256=ee1131e748a2c7d7559b7054540ebe49637433901f92071560df52e369f7c7af
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(libchdr)
endfunction()
satrecomp_libchdr()
