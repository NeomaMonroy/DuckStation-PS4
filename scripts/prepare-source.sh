#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PSB="${PS4_BUILD_ROOT:-$HOME/dk64-build}"
SRC="${DS_SRC:-$PSB/src/duckstation-nx}"
REPO="https://github.com/RSDuck/duckstation.git"
BRANCH="switch-port"
COMMIT="a7feccf54eadf6a54bebbf90ff1b4f327716c9ec"
PATCH="$HERE/../patches/duckstation-ps4.patch"

mkdir -p "$PSB/src"
if [ ! -d "$SRC/.git" ]; then
    rm -rf "$SRC"
    git clone -q --branch "$BRANCH" "$REPO" "$SRC"
fi
if [ "$(git -C "$SRC" rev-parse HEAD)" != "$COMMIT" ]; then
    git -C "$SRC" fetch -q origin "$BRANCH" || true
    git -C "$SRC" reset -q --hard "$COMMIT"
    git -C "$SRC" clean -qfd
fi

if git -C "$SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "  DuckStation: PS4 patch already applied"
else
    git -C "$SRC" reset -q --hard "$COMMIT"
    git -C "$SRC" clean -qfd
    git -C "$SRC" apply --whitespace=nowarn "$PATCH"
    echo "  DuckStation: PS4 patch applied"
fi
