# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# The satrecomp launcher: the player's disc image, optional features, controllers; starts the game. Built
# beside the game program (it runs <id> from its own folder). Needs SDL3.
# The launcher itself is the one the recomp ports share (../recomp-common/launcher, a copy of recomp-common's;
# never changed here). What is this toolkit's is src/description.cpp: what it tells that launcher, and the
# check of the disc image. The game comes from these (satrecomp/build.mk passes them from games/<id>/game.cfg):
#   GAME_NAME GAME_EXE DISC_NAME DISC_PRODUCT DISC_VERSION APP_ID HOME_ENV DISCLAIMER, and optionally
#   GAME_VERSION (the game repository's VERSION) and UPDATE_REPO
cmake_minimum_required(VERSION 3.20)
project(satrecomp_launcher CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(LAUNCHER_GAME_NAME "${GAME_NAME}")
set(LAUNCHER_GAME_EXE "${GAME_EXE}")
set(LAUNCHER_APP_ID "${APP_ID}")
set(LAUNCHER_HOME_ENV "${HOME_ENV}")
set(LAUNCHER_DISCLAIMER "${DISCLAIMER}")
set(LAUNCHER_PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/description.cpp)
# The game's version (its repository's VERSION, shown under its name) and, when its game.cfg names one
# (update_repo = owner/name), the GitHub repository whose releases the launcher updates itself from
# (recomp-common/launcher/src/update.h: the release carries an update-<system>.txt for each download)
if(GAME_VERSION)
    set(LAUNCHER_VERSION "${GAME_VERSION}")
    if(UPDATE_REPO)
        set(LAUNCHER_UPDATE_REPO "${UPDATE_REPO}")
    endif()
endif()
include(../recomp-common/launcher/launcher.cmake)

target_compile_definitions(launcher PRIVATE
    LAUNCHER_DISC_NAME="${DISC_NAME}" LAUNCHER_DISC_PRODUCT="${DISC_PRODUCT}" LAUNCHER_DISC_VERSION="${DISC_VERSION}")
