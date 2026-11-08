#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
PSB="$PS4_BUILD_ROOT"
BUILD="$PSB/build/duckstation"
SRC="${DS_SRC:-$PSB/src/duckstation-nx}"
EBOOT="$BUILD/src/duckstation-nogui/duckstation-nogui_eboot/eboot.bin"
ELF="$BUILD/bin/duckstation-nogui"
VERSION="${DS_VERSION:-00.03}"

[ -f "$EBOOT" ] || { echo "missing $EBOOT: build first" >&2; exit 1; }
[ "$EBOOT" -nt "$ELF" ] || { echo "$EBOOT is older than the ELF: the last build failed" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9][0-9]\.[0-9][0-9]$ ]] || { echo "version must look like 01.00" >&2; exit 1; }
[ -d "$BUILD/pkgroot/resources" ] || { echo "no resources in $BUILD/pkgroot: build first" >&2; exit 1; }

STAGE="$PSB/build/duckstation-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$EBOOT" "$STAGE/eboot.bin"
cp -r "$BUILD/pkgroot/resources" "$STAGE/resources"
python3 "$HERE/make-art.py" "$STAGE/sce_sys" "$SRC/data/resources/images/duck.png"
echo "$VERSION" > "$STAGE/version.txt"
cp "$SRC/LICENSE" "$STAGE/LICENSE.txt" 2>/dev/null || true
find "$STAGE" -type d -empty -delete

PKG="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR=/nonexistent "$HERE/make-pkg.sh" "$STAGE" DUCK00001 "DuckStation" "$VERSION" DUCKSTATIONPS4 "$PSB/out" | tail -1)"
NAME="DuckStation-v$VERSION.pkg"
DEST="${PKG_OUT:-$HERE/../out}"
mkdir -p "$DEST"
mv -f "$PKG" "$DEST/$NAME" && sync

mkdir -p "$PSB/oelf"
cp "$BUILD/src/duckstation-nogui/duckstation-nogui_eboot/duckstation-nogui.oelf" "$PSB/oelf/duckstation-v$VERSION.oelf" 2>/dev/null || true
cp "$ELF" "$PSB/oelf/duckstation-v$VERSION.elf"
echo "package: $DEST/$NAME"
