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

# (the game's combined buttons, game.cfg's `combos`: on the Controllers page as the game reads them, port/bindings.h)
set(GAME_COMBOS "")
set(GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../games/${GAME_EXE}" CACHE PATH "The game's directory (game.cfg)")
set(GAME_CFG "${GAME_DIR}/game.cfg")
if (EXISTS "${GAME_CFG}")
    file(STRINGS "${GAME_CFG}" GAME_COMBOS REGEX "^combos[ \t]*=")
    string(REGEX REPLACE "^combos[ \t]*=[ \t]*" "" GAME_COMBOS "${GAME_COMBOS}")
endif ()
# (a second disc of the game whose files the game may read too, game.cfg's `extra_disc_product` and
# `extra_disc_name`: the launcher looks for it beside the player's disc and starts the game with --extra-disc)
set(GAME_EXTRA_DISC_PRODUCT "")
set(GAME_EXTRA_DISC_NAME "")
if (EXISTS "${GAME_CFG}")
    file(STRINGS "${GAME_CFG}" GAME_EXTRA_DISC_PRODUCT REGEX "^extra_disc_product[ \t]*=")
    string(REGEX REPLACE "^extra_disc_product[ \t]*=[ \t]*" "" GAME_EXTRA_DISC_PRODUCT "${GAME_EXTRA_DISC_PRODUCT}")
    file(STRINGS "${GAME_CFG}" GAME_EXTRA_DISC_NAME REGEX "^extra_disc_name[ \t]*=")
    string(REGEX REPLACE "^extra_disc_name[ \t]*=[ \t]*" "" GAME_EXTRA_DISC_NAME "${GAME_EXTRA_DISC_NAME}")
endif ()
# (the other games' discs a custom soundtrack knows the music of, game.cfg's `soundtrack_discs`: named on the Sound
# card's soundtrack row, port/soundtrack.h)
set(GAME_SOUNDTRACK_DISCS "")
# (whether the game has one at all, game.cfg's `custom_soundtrack = yes`: a game whose sources name its music,
# Game::songs; without, the launcher shows no soundtrack row)
set(GAME_CUSTOM_SOUNDTRACK 0)
if (EXISTS "${GAME_CFG}")
    file(STRINGS "${GAME_CFG}" GAME_SOUNDTRACK_DISCS REGEX "^soundtrack_discs[ \t]*=")
    string(REGEX REPLACE "^soundtrack_discs[ \t]*=[ \t]*" "" GAME_SOUNDTRACK_DISCS "${GAME_SOUNDTRACK_DISCS}")
    file(STRINGS "${GAME_CFG}" custom_soundtrack_line REGEX "^custom_soundtrack[ \t]*=[ \t]*yes")
    if (custom_soundtrack_line)
        set(GAME_CUSTOM_SOUNDTRACK 1)
    endif ()
endif ()
# (where the game's players talk, game.cfg's `community_url` (a Discord invite, say), `community_title`,
# `community_detail` and `community_button`: a card on the launcher's Game page under Play,
# recomp-common's Description::community_url; no url: none)
foreach (key url title detail button)
    string(TOUPPER "${key}" KEY)
    set(GAME_COMMUNITY_${KEY} "")
    if (EXISTS "${GAME_CFG}")
        file(STRINGS "${GAME_CFG}" GAME_COMMUNITY_${KEY} REGEX "^community_${key}[ \t]*=")
        string(REGEX REPLACE "^community_${key}[ \t]*=[ \t]*" "" GAME_COMMUNITY_${KEY} "${GAME_COMMUNITY_${KEY}}")
    endif ()
endforeach ()
target_compile_definitions(launcher PRIVATE
    "LAUNCHER_SOUNDTRACK_DISCS=\"${GAME_SOUNDTRACK_DISCS}\"" "LAUNCHER_CUSTOM_SOUNDTRACK=${GAME_CUSTOM_SOUNDTRACK}"
    LAUNCHER_DISC_NAME="${DISC_NAME}" LAUNCHER_DISC_PRODUCT="${DISC_PRODUCT}" LAUNCHER_DISC_VERSION="${DISC_VERSION}"
    "SATRECOMP_COMBOS=\"${GAME_COMBOS}\""
    "LAUNCHER_EXTRA_DISC_PRODUCT=\"${GAME_EXTRA_DISC_PRODUCT}\"" "LAUNCHER_EXTRA_DISC_NAME=\"${GAME_EXTRA_DISC_NAME}\""
    "LAUNCHER_COMMUNITY_URL=\"${GAME_COMMUNITY_URL}\"" "LAUNCHER_COMMUNITY_TITLE=\"${GAME_COMMUNITY_TITLE}\""
    "LAUNCHER_COMMUNITY_DETAIL=\"${GAME_COMMUNITY_DETAIL}\"" "LAUNCHER_COMMUNITY_BUTTON=\"${GAME_COMMUNITY_BUTTON}\"")
# (the disc check reads the disc image as the game does, .chd too: port/disc_image.cpp, with libchdr)
target_sources(launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../port/disc_image.cpp)
target_include_directories(launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../port)
set(CHDR_USE_SYSTEM_ZLIB OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/../port/libchdr.cmake)
target_link_libraries(launcher PRIVATE chdr-static)
