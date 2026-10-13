#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# Starts what 'make example' built, with no game and no window: the program lists its features (the
# stand-in game has none) and the launcher draws its pages to pictures. Then tries tools/sync-toolkit.sh
# in a game repository made for the purpose: a copy is taken, checked, and a change to it is caught.
# Used by CI and by 'make smoke'.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/example/build/example-dev"
EXE=""
[ -f "$B/example.exe" ] && EXE=.exe
"$B/example$EXE" --list-features > /dev/null
echo "ok: the program starts"
OUT="$B/smoke"
rm -rf "$OUT" && mkdir -p "$OUT/home"
env SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy "EXAMPLE_HOME=$OUT/home" \
    "$B/example-launcher$EXE" --screenshot "$OUT/page"
pages="$(ls "$OUT"/page*.png | wc -l | tr -d ' ')"
[ "$pages" -ge 3 ] || { echo "the launcher drew $pages pages, not 3"; exit 1; }
echo "ok: the launcher starts and draws its pages ($pages)"

GAME_REPO="$OUT/game"
mkdir -p "$GAME_REPO/games/example"
cd "$GAME_REPO"
SATRECOMP="$ROOT" "$ROOT/tools/sync-toolkit.sh" > /dev/null
satrecomp/tools/sync-toolkit.sh --check > /dev/null
satrecomp/tools/sync-common.sh --check > /dev/null
[ -f satrecomp/build.mk ] && [ ! -e satrecomp/example ] && [ ! -e satrecomp/.github ] || { echo "the copy is not what a game needs"; exit 1; }
mkdir -p satrecomp/third_party/Ymir satrecomp/tools/__pycache__
touch satrecomp/third_party/Ymir/file satrecomp/tools/__pycache__/file.pyc
satrecomp/tools/sync-toolkit.sh --check > /dev/null
echo "// changed by hand" >> satrecomp/runtime/lift.h
if satrecomp/tools/sync-toolkit.sh --check > /dev/null 2>&1; then echo "a changed copy passed the check"; exit 1; fi
SATRECOMP="$ROOT" satrecomp/tools/sync-toolkit.sh > /dev/null
[ -f satrecomp/third_party/Ymir/file ] || { echo "a sync removed third_party/"; exit 1; }
touch satrecomp/port/added.cpp
if satrecomp/tools/sync-toolkit.sh --check > /dev/null 2>&1; then echo "a copy with a file added passed the check"; exit 1; fi
echo "ok: sync-toolkit.sh takes a copy, checks it, and catches a changed one"
