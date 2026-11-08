#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PSB="${PS4_BUILD_ROOT:-$HOME/dk64-build}"
SRC="${DS_SRC:-$PSB/src/duckstation-nx}"
OUT="$HERE/../patches"
git -C "$SRC" add -A -N .
git -C "$SRC" diff --binary HEAD -- . > "$OUT/duckstation-ps4.patch"
printf '%-28s %s lines\n' duckstation-ps4.patch "$(wc -l < "$OUT/duckstation-ps4.patch")"
