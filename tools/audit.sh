#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# Checks the repository's tracked files for things that must never be committed (CONTRIBUTING.md):
# game files, anything made from a game, an emulator's sources, personal paths and addresses,
# large files; and that every file marked as another project's is listed in the notices. Run it
# before pushing (make audit); CI runs it on every push and pull request. Exits non-zero on a finding.
set -eu
cd "$(git rev-parse --show-toplevel)"
LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT
git ls-files > "$LIST"

fail=0
check() { if [ -n "$2" ]; then echo "AUDIT FAIL: $1"; echo "$2" | head -10; fail=1; else echo "ok: $1"; fi; }
# (this script names the things it looks for, so it is not searched itself)
grep_files() { grep -v '^tools/audit\.sh$' "$LIST" | tr '\n' '\0' | xargs -0 grep -nIE "$@" 2>/dev/null || true; }

check "no game, disc, BIOS, save or archive file types" "$(grep -iE '\.(bin|cue|iso|chd|ccd|img|sub|mdf|mds|gdi|rom|bkr|sav|srm|state|zip|7z|rar|key)$' "$LIST" || true)"
check "nothing made from a game (block lists, generated code, memory dumps, input movies)" "$(grep -E '(^|/)(work|build)/|(^|/)blocks[^/]*\.txt|blocks_[0-9]+\.cpp$|decode\.bin$|(^|/)gen(-ref)?/|\.(dump|ram)$|(^|/)movies/' "$LIST" || true)"
check "no game but the stand-in" "$(grep -E '(^|/)games/' "$LIST" | grep -v '^example/games/example/game\.cfg$' || true)"
check "no pictures, sound or video" "$(grep -iE '\.(png|jpe?g|gif|bmp|ppm|webp|mp4|mov|webm|wav|ogg|mp3|flac)$' "$LIST" || true)"
# (the published checksums of the libraries the builds download, and the hashes of the copied files, are not keys)
check "no key-like hex strings (32 digits or more)" "$(grep_files '(^|[^0-9a-fA-F.])[0-9a-fA-F]{32,}([^0-9a-fA-F]|$)' | grep -vE 'YMIR_COMMIT|URL_HASH SHA256=[0-9a-f]{64}|SHA256=[0-9a-f]{64}|= [0-9a-f]{64} \]|^recomp-common/COMMIT:|^\.github/workflows/' || true)"
check "no home folder paths" "$(grep_files -e '/Users/[a-z]' -e '/home/[a-z]+/' -e 'C:\\\\Users\\\\[a-z]' || true)"
check "no private session links or private network addresses" "$(grep_files -e 'claude\.ai/code/session' -e '\b192\.168\.[0-9]+\.[0-9]+' -e '\b10\.[0-9]+\.[0-9]+\.[0-9]+\b' || true)"
check "no file over 512 KB" "$(tr '\n' '\0' < "$LIST" | xargs -0 find 2>/dev/null -prune -type f -size +512k || true)"
check "no words that point to where game files are found" "$(grep_files -i -e 'rom ?site' -e 'emuparadise' -e 'vimm' -e 'cdromance' -e 'romsfun' -e 'torrent' -e 'download (the|a|an) (rom|iso|bios|disc image)' || true)"
check "no console maker's SDK files or names" "$(grep_files -e '\bsl(Init|Print|PutPolygon|DispSprite)[A-Za-z]*\b' -e '\b(GFS|SPR|SCL|PER|CDC|CSH|STM)_[A-Z][A-Za-z0-9]+\(' -e 'sgl\.h' -e 'sega_(sys|gfs|spr|scl|per|bup|cdc)\.h' || true)"
# An emulator is a development reference here, never part of the tree: its sources are fetched by
# 'make setup', and the patch to it (which quotes its source) is kept outside this repository.
check "no submodules" "$(git ls-files -s | awk '$1 == "160000" {print $4}'; [ ! -f .gitmodules ] || echo .gitmodules)"
check "no patch files" "$(grep -iE '\.(patch|diff)$|(^|/)patches/' "$LIST" || true)"
check "no emulator's sources in the tree" "$(grep -iE '(^|/)(third_party|vendor|external|deps)/.+/.+|(^|/)(ymir|mednafen|yabause|kronos|mame|musashi|libretro)[^/]*/' "$LIST" || true)"
check "files of another project's are only the ones the notices list" "$(grep_files -l -i -e 'adapted from' -e 'ported from' -e 'SPDX-License-Identifier: GPL-3.0-or-later' | grep -vE '^recomp-common/launcher/src/(ui\.cpp|ui\.h|embedded\.h)$|^(THIRD_PARTY_NOTICES|CONTRIBUTING)\.md$' || true)"
check "every source file has a license line" "$(grep -E '\.(h|c|cpp|inc|frag|vert|py|sh|mk|cmake|cfg)$|CMakeLists\.txt$|(^|/)Makefile$' "$LIST" | tr '\n' '\0' | xargs -0 grep -LE 'SPDX-License-Identifier: GPL-3\.0-(only|or-later)' 2>/dev/null || true)"
[ "$fail" = 0 ] || { echo "audit failed"; exit 1; }
echo "audit passed ($(wc -l < "$LIST" | tr -d ' ') files)"
