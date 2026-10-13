#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# Checks of the game on its own machine. Each compares a hash of the machine's state (work RAM, CPU registers,
# picture, sound) after every frame between two runs with the same input:
#   1. everything run by the SH-2 interpreter against the translated code (run block by block, as the
#      interpreter does): the two must give the same state, which checks each against the other;
#   2. the game as it is played, twice: every run must be the same.
#   check.sh [FRAMES] [MOVIE]
# Environment: GAME, DISC, B (the build's folder; defaults as in build.mk).
set -e
GAME=${GAME:-fighting_vipers}
FRAMES=${1:-3000}
MOVIE=$2
WORK=work/$GAME
PORT=${B:-build/$GAME}/$GAME
DISC=${DISC:-$(ls disc/*.cue disc/*.chd disc/*.iso 2>/dev/null | head -1)}
TAG=$(basename "${MOVIE:-boot}" .txt)
mkdir -p "$WORK/check"
run() { # name, then environment settings
    name=$1
    shift
    env "$@" "$PORT" --disc "$DISC" --headless --frames "$FRAMES" ${MOVIE:+--movie "$MOVIE"} \
        --hash-log "$WORK/check/${TAG}_$name.txt" > "$WORK/check/${TAG}_$name.log" 2>&1 &
}
run interpreter SATRECOMP_NO_BLOCKS=1 SATRECOMP_NO_CALLS=1
run blocks SATRECOMP_NO_FUNCTIONS=1 SATRECOMP_NO_CALLS=1
run first SATRECOMP_UNUSED=1
run second SATRECOMP_UNUSED=1
wait
status=0
for name in interpreter blocks first second; do
    if [ "$(wc -l < "$WORK/check/${TAG}_$name.txt")" -ne "$FRAMES" ]; then
        echo "FAILED: $TAG: the '$name' run stopped before $FRAMES frames (see $WORK/check/${TAG}_$name.log)"
        exit 1
    fi
done
if cmp -s "$WORK/check/${TAG}_interpreter.txt" "$WORK/check/${TAG}_blocks.txt"; then
    echo "OK: $TAG: the interpreter and the translated code give the same state for $FRAMES frames"
else
    echo "MISMATCH: $TAG: interpreter and translated code, first differing frame $(cmp "$WORK/check/${TAG}_interpreter.txt" "$WORK/check/${TAG}_blocks.txt" | awk '{print $NF}')"
    status=1
fi
if cmp -s "$WORK/check/${TAG}_first.txt" "$WORK/check/${TAG}_second.txt"; then
    echo "OK: $TAG: two runs are the same for $FRAMES frames"
else
    echo "MISMATCH: $TAG: two runs differ, first at frame $(cmp "$WORK/check/${TAG}_first.txt" "$WORK/check/${TAG}_second.txt" | awk '{print $NF}')"
    status=1
fi
exit $status
