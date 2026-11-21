#!/usr/bin/env bash
# SPDX-FileCopyrightText: EmuSuNi (github.com/arturofuentes1988/EmuSuNi)
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
SRC_ROOT="${2:-$HOME/src}"
TARBALL="$SRC_ROOT/mbedtls-2.16.6-apache.tgz"
SHA256=66455e23a6190a30142cdc1113f7418158839331a9d8e6b0778631d077281770
LOG="$OUT/mbedtls.log"
LIBS="libmbedcrypto libmbedx509 libmbedtls"

if [ -f "$OUT/lib/libmbedcrypto.a" ] && [ -f "$OUT/lib/libmbedx509.a" ] && [ -f "$OUT/lib/libmbedtls.a" ] && [ -f "$OUT/.fixed" ]; then
  exit 0
fi

mkdir -p "$SRC_ROOT"
if ! echo "$SHA256  $TARBALL" | sha256sum -c --quiet - 2>/dev/null; then
  wget -q https://sources.buildroot.net/mbedtls/mbedtls-2.16.6-apache.tgz -O "$TARBALL.part"
  echo "$SHA256  $TARBALL.part" | sha256sum -c --quiet -
  mv "$TARBALL.part" "$TARBALL"
fi

AR=llvm-ar-21
command -v "$AR" > /dev/null || AR="$OPENORBIS/bin/llvm-ar"
NM=llvm-nm-21
command -v "$NM" > /dev/null || NM="$OPENORBIS/bin/llvm-nm"

rm -rf "$OUT/src" "$OUT/build" "$OUT/lib" "$OUT/objects" "$OUT/.fixed"
mkdir -p "$OUT/src" "$OUT/lib" "$OUT/objects/pacbrew" "$OUT/objects/new"
tar xzf "$TARBALL" -C "$OUT/src" --strip-components=1

python3 -I - "$OUT/src/include/mbedtls/bn_mul.h" <<'PATCH'
import sys
path = sys.argv[1]
text = open(path).read()
BS = "\\"
old = (
    "#define MULADDC_STOP                        " + BS + "\n"
    "        : \"+c\" (c), \"+D\" (d), \"+S\" (s)      " + BS + "\n"
    "        : \"b\" (b)                           " + BS + "\n"
    "        : \"rax\", \"rdx\", \"r8\"                " + BS + "\n"
    "    );\n"
    "\n"
    "#endif /* AMD64 */"
)
new = (
    "#define MULADDC_STOP                        " + BS + "\n"
    "        : \"+c\" (c), \"+D\" (d), \"+S\" (s), \"+m\" (*(uint64_t (*)[16]) d) " + BS + "\n"
    "        : \"b\" (b), \"m\" (*(const uint64_t (*)[16]) s)                 " + BS + "\n"
    "        : \"rax\", \"rdx\", \"r8\"                                         " + BS + "\n"
    "    );\n"
    "\n"
    "#endif /* AMD64 */"
)
if text.count(old) != 1:
    sys.exit("bn_mul.h: the AMD64 MULADDC_STOP block isn't 2.16.6's")
open(path, "w").write(text.replace(old, new))
PATCH

(
  source "$OPENORBIS/ps4vars.sh"
  mkdir -p "$OUT/build"
  cd "$OUT/build"
  if ! openorbis-cmake -Wno-dev -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_ZLIB_SUPPORT=TRUE \
       -DENABLE_TESTING=FALSE -DENABLE_PROGRAMS=FALSE "$OUT/src" > "$LOG" 2>&1; then
    tail -20 "$LOG"
    echo "Mbed TLS: configuring failed (see $LOG)"
    exit 1
  fi
  if ! make -j "${JOBS:-4}" mbedcrypto >> "$LOG" 2>&1; then
    grep -E "error" "$LOG" | head -20
    echo "Mbed TLS: compiling failed (see $LOG)"
    exit 1
  fi
)

for lib in $LIBS; do
  cp "$OPENORBIS/usr/lib/$lib.a" "$OUT/lib/$lib.a"
done
(cd "$OUT/objects/new" && "$AR" x "$OUT/build/library/libmbedcrypto.a" bignum.c.obj)
"$AR" rs "$OUT/lib/libmbedcrypto.a" "$OUT/objects/new/bignum.c.obj"

(cd "$OUT/objects/pacbrew" && "$AR" x "$OPENORBIS/usr/lib/libmbedcrypto.a")
rm -rf "$OUT/objects/new" && mkdir -p "$OUT/objects/new"
(cd "$OUT/objects/new" && "$AR" x "$OUT/lib/libmbedcrypto.a")
if [ "$("$AR" t "$OUT/lib/libmbedcrypto.a")" != "$("$AR" t "$OPENORBIS/usr/lib/libmbedcrypto.a")" ]; then
  echo "Mbed TLS: libmbedcrypto.a's objects aren't PacBrew's"
  exit 1
fi
for object in "$OUT"/objects/pacbrew/*; do
  name="$(basename "$object")"
  if [ "$name" = bignum.c.obj ]; then
    if cmp -s "$object" "$OUT/objects/new/$name"; then
      echo "Mbed TLS: bignum.c.obj is still PacBrew's"
      exit 1
    fi
  elif ! cmp -s "$object" "$OUT/objects/new/$name"; then
    echo "Mbed TLS: $name isn't PacBrew's"
    exit 1
  fi
done
for kind in --defined-only --undefined-only; do
  if ! cmp -s <("$NM" -g "$kind" -j "$OUT/lib/libmbedcrypto.a" 2>/dev/null | sort -u) \
              <("$NM" -g "$kind" -j "$OPENORBIS/usr/lib/libmbedcrypto.a" 2>/dev/null | sort -u); then
    echo "Mbed TLS: libmbedcrypto.a's symbols ($kind) differ from PacBrew's"
    exit 1
  fi
done
rm -rf "$OUT/objects" "$OUT/build"
touch "$OUT/.fixed"
echo "  Mbed TLS: PacBrew's 2.16.6 with bignum.c's x86-64 fix"
