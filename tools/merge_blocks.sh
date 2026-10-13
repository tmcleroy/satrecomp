#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
# Merges block databases: merge_blocks.sh OUT IN...   (OUT may also be one of the inputs)
set -e
OUT=$1
shift
TMP=$(mktemp)
(echo "# satrecomp block database: address, code bytes"; cat "$@" 2>/dev/null | grep -v '^#' | sort -u) > "$TMP"
mv "$TMP" "$OUT"
echo "$(grep -vc '^#' "$OUT") blocks in $OUT"
