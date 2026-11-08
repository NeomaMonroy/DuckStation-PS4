#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"

STAGE="$(readlink -f "$1")"; TITLE_ID="$2"; TITLE="$3"; VERSION="$4"; LABEL="$5"; OUT="$(readlink -f "$6")"
TOOLS="$OPENORBIS/bin/linux"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

[[ "$TITLE_ID" =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "TITLE_ID must look like ABCD12345" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]{2}\.[0-9]{2}$ ]] || { echo "version must look like 01.00" >&2; exit 1; }
[ -f "$STAGE/eboot.bin" ] || { echo "missing $STAGE/eboot.bin" >&2; exit 1; }
[ -f "$STAGE/sce_sys/icon0.png" ] || { echo "missing $STAGE/sce_sys/icon0.png" >&2; exit 1; }

mkdir -p "$STAGE/sce_module"
[ "${NO_RIGHT_SPRX:-0}" = 1 ] || mkdir -p "$STAGE/sce_sys/about"
[ "${NO_RIGHT_SPRX:-0}" = 1 ] || cp "$OPENORBIS/samples/piglet/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/"
cp "$OPENORBIS/samples/piglet/sce_module/"*.prx "$STAGE/sce_module/"
shopt -s nullglob
for m in "${DOLPHIN_PS4_MODULES_DIR:-$HERE/../modules}"/*.sprx; do
    echo "bundling module $(basename "$m")"
    cp "$m" "$STAGE/sce_module/"
done
shopt -u nullglob

LABEL="$(printf '%s' "$LABEL" | tr '[:lower:]' '[:upper:]' | tr -cd 'A-Z0-9')"
LABEL="$(printf '%-16s' "${LABEL:0:16}" | tr ' ' '0')"
CONTENT_ID="IV0000-${TITLE_ID}_00-${LABEL}"

SFO="$STAGE/sce_sys/param.sfo"
rm -f "$SFO"
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
set_entry() { "$TOOLS/PkgTool.Core" sfo_setentry "$SFO" "$1" --type "$2" --maxsize "$3" --value "$4" >/dev/null; }
if [ "${SFO_STYLE:-retroarch}" = retroarch ]; then
    set_entry APP_TYPE Integer 4 0
    set_entry ATTRIBUTE Integer 4 0x20814016
    set_entry ATTRIBUTE2 Integer 4 0x6
    set_entry CATEGORY Utf8 4 gde
    set_entry FORMAT Utf8 4 obs
    set_entry SYSTEM_VER Integer 4 0x3fc
else
    set_entry APP_TYPE Integer 4 1
    set_entry ATTRIBUTE Integer 4 0x2
    set_entry CATEGORY Utf8 4 gd
    set_entry SYSTEM_VER Integer 4 0
fi
set_entry APP_VER Utf8 8 "$VERSION"
set_entry CONTENT_ID Utf8 48 "$CONTENT_ID"
set_entry DOWNLOAD_DATA_SIZE Integer 4 0
set_entry TITLE Utf8 128 "$TITLE"
set_entry TITLE_ID Utf8 12 "$TITLE_ID"
set_entry VERSION Utf8 8 "$VERSION"

rm -f "$STAGE/pkg.gp4"
python3 "$(dirname "$0")/make-gp4.py" "$STAGE" "$CONTENT_ID" "$STAGE/pkg.gp4" >/dev/null
mkdir -p "$OUT"
( cd "$STAGE" && "$TOOLS/PkgTool.Core" pkg_build pkg.gp4 "$OUT" ) >/dev/null
rm -f "$STAGE/pkg.gp4"
echo "$OUT/$CONTENT_ID.pkg"
