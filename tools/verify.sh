#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# Checks translated code against the interpreter (on the reference build, 'make ref'): runs it twice with the same input and compares a hash
# of the machine state (work RAM, CPU registers, picture) after every frame. The two must be identical.
#   verify.sh [FRAMES] [MOVIE] [native|exact]
# native (default): --cpu native (satrecomp's own translation) against --cpu free (the interpreter, free running)
# exact:            --cpu recomp (cycle-exact blocks) against --cpu pure (the unmodified interpreter)
# A movie only replays in the timing it was recorded in: movies/native_* for native, movies/explore_* for exact.
# Environment: GAME, DISC, BIOS, B_REF (the reference build's folder) (defaults as in build.mk; native runs without a BIOS image unless BIOS is set).
set -e
GAME=${GAME:-fighting_vipers}
FRAMES=${1:-3000}
MOVIE=$2
KIND=${3:-native}
if [ "$KIND" = exact ]; then REF=pure; NEW=recomp; else REF=free; NEW=native; fi
WORK=work/$GAME
PORT=${B_REF:-build/$GAME-ref}/$GAME-ref
DISC=${DISC:-$(ls disc/*.cue disc/*.iso 2>/dev/null | head -1)}
# The cycle-exact pair runs through a BIOS image (the unmodified interpreter cannot use the native BIOS)
if [ "$KIND" = exact ]; then BIOS=${BIOS:-$(ls bios/*.bin 2>/dev/null | head -1)}; fi
TAG=$(basename "${MOVIE:-boot}" .txt)
mkdir -p "$WORK/verify"
PIDS=""
for mode in $REF $NEW; do
    "$PORT" ${BIOS:+--bios "$BIOS"} --disc "$DISC" --headless --frames "$FRAMES" --cpu $mode --stats \
        ${MOVIE:+--movie "$MOVIE"} --hash-log "$WORK/verify/${TAG}_$mode.txt" 2> "$WORK/verify/${TAG}_$mode.log" &
    PIDS="$PIDS $!"
done
for pid in $PIDS; do
    if ! wait $pid; then
        echo "FAILED: $TAG: a run did not finish (see $WORK/verify/${TAG}_*.log)"
        exit 1
    fi
done
if [ "$(wc -l < "$WORK/verify/${TAG}_$NEW.txt")" -ne "$FRAMES" ]; then
    echo "FAILED: $TAG: the run stopped before $FRAMES frames"
    exit 1
fi
grep "block runs" "$WORK/verify/${TAG}_$NEW.log" || true
if cmp -s "$WORK/verify/${TAG}_$REF.txt" "$WORK/verify/${TAG}_$NEW.txt"; then
    echo "OK: $TAG: --cpu $NEW matches --cpu $REF for $FRAMES frames"
else
    FRAME=$(cmp "$WORK/verify/${TAG}_$REF.txt" "$WORK/verify/${TAG}_$NEW.txt" | awk '{print $NF}')
    echo "MISMATCH: $TAG: first differing frame $FRAME (see satrecomp/docs/VERIFYING.md)"
    exit 1
fi
