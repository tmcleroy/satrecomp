#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# Keeps a game repository's copy of the satrecomp toolkit and checks that nobody changed it there.
#
#   satrecomp/tools/sync-toolkit.sh           copy the toolkit as committed in its own repository (HEAD)
#                                             into satrecomp/ and record the commit
#   satrecomp/tools/sync-toolkit.sh --check   fail if satrecomp/ is not what was recorded (make check-toolkit)
#
# Run from the game repository's top. A clone of the toolkit is looked for beside the game's repository
# (../satrecomp), or where SATRECOMP says. The copy is committed in the game's repository, which so builds
# from a fresh clone on its own. Never change the copy by hand: change the toolkit in its repository (build
# and check a game against it with make SR=../satrecomp), commit there, and run this in each game.
#
# What is not the copy's is left alone and not counted: third_party/ (the reference emulator that make setup
# fetches there, which git ignores) and what running the tools leaves behind (__pycache__, .DS_Store).
set -eu

main() {
    dest=satrecomp
    [ -d games ] || { echo "sync-toolkit: run from a game repository's top (no games/ here)" >&2; exit 1; }
    src=${SATRECOMP:-$(pwd)/../satrecomp}
    record="$dest/COMMIT"
    if command -v shasum > /dev/null; then sum="shasum -a 256"; else sum="sha256sum"; fi
    # the copy's files
    files() {
        (cd "$dest" && find . -type f ! -path ./COMMIT ! -path './third_party/*' ! -path '*/__pycache__/*' \
            ! -name .DS_Store | sed 's|^\./||' | LC_ALL=C sort)
    }
    # ... and their hashes, one a line
    hashes() {
        files | while read -r f; do
            printf '%s  %s\n' "$($sum < "$dest/$f" | cut -d' ' -f1)" "$f"
        done
    }

    if [ "${1:-}" = "--check" ]; then
        [ -f "$record" ] || { echo "sync-toolkit: no $record; run satrecomp/tools/sync-toolkit.sh" >&2; exit 1; }
        commit=$(sed -n 's/^commit //p' "$record")
        if [ "$(hashes)" != "$(grep -v '^#' "$record" | grep -v '^commit ')" ]; then
            echo "sync-toolkit: satrecomp/ is not what satrecomp $commit gave (changed by hand?)." >&2
            echo "  Change the toolkit in its own repository and run satrecomp/tools/sync-toolkit.sh." >&2
            exit 1
        fi
        echo "satrecomp/: commit $commit"
        exit 0
    fi

    git -C "$src" rev-parse HEAD > /dev/null 2>&1 || { echo "sync-toolkit: no satrecomp at $src (set SATRECOMP)" >&2; exit 1; }
    commit=$(git -C "$src" rev-parse HEAD)
    if [ -n "$(git -C "$src" status --porcelain)" ]; then
        echo "sync-toolkit: note: $src has changes not committed; copying it as committed ($commit)" >&2
    fi
    # (only ever a copy this script made is replaced)
    if [ -e "$dest" ] && [ ! -f "$record" ] && [ "${2:-}" != "--first" ] && [ "${1:-}" != "--first" ]; then
        echo "sync-toolkit: $dest is there and is not a copy made by this script (--first to replace it)" >&2
        exit 1
    fi
    tmp=$(mktemp -d)
    git -C "$src" archive "$commit" | tar -x -C "$tmp"
    if [ -d "$dest" ]; then
        files | while read -r f; do rm -f "$dest/$f"; done
        find "$dest" -depth -type d -empty ! -path "$dest/third_party/*" -delete
    fi
    mkdir -p "$dest"
    (cd "$tmp" && tar -c .) | tar -x -C "$dest"
    rm -rf "$tmp"
    {
        echo "# A copy of the satrecomp toolkit, made by its tools/sync-toolkit.sh. Do not change it here."
        echo "commit $commit"
        hashes
    } > "$record"
    echo "satrecomp/: commit $commit"
}
# (the whole script is read before any of it runs: it replaces itself)
main "$@"; exit
