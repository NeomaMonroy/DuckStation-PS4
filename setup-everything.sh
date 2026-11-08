#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export PS4_BUILD_ROOT="${PS4_BUILD_ROOT:-$HOME/dk64-build}"
export PSB="$PS4_BUILD_ROOT"
export PS4_SYSROOT="$PS4_BUILD_ROOT/sysroot"
export OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
PKG_VERSION="${DS_VERSION:-00.03}"

step() { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf '\n\033[1;31mError: %s\033[0m\n' "$*" >&2; exit 1; }

chmod +x "$HERE"/scripts/*.sh "$HERE"/scripts/*.py "$HERE"/toolchain/*.sh "$HERE"/toolchain/bin/* 2>/dev/null || true
case "$HERE" in *" "*) fail "move this folder to a path without spaces (now: $HERE)" ;; esac
[ "$(id -u)" != 0 ] || fail "run this as your normal user, not with sudo (it asks for your password when it needs it)"
mkdir -p "$PS4_BUILD_ROOT/src" "$PS4_BUILD_ROOT/logs"

step "1/9  Linux packages"
OS_ID="$(. /etc/os-release 2>/dev/null; echo "${ID:-unknown}")"
OS_NAME="$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
[ "$OS_ID" = ubuntu ] || note "This script is made for Ubuntu (found $OS_NAME); trying anyway."
sudo apt-get update
sudo apt-get install -y git curl wget ca-certificates xz-utils unzip build-essential \
    cmake ninja-build pkg-config python3 python3-pip python3-venv python3-pil libzstd-dev \
    clang lld llvm zlib1g-dev libxml2 bison flex glslang-tools \
    gnupg lsb-release software-properties-common libarchive-tools autoconf libtool nasm

step "2/9  clang 21 (the PS4 toolchain's compiler)"
if ! command -v clang-21 >/dev/null || ! command -v ld.lld-21 >/dev/null || ! command -v llvm-ar-21 >/dev/null; then
    wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    sudo bash /tmp/llvm.sh 21
    sudo apt-get install -y lld-21 llvm-21 clang-21
else
    note "already installed"
fi

step "3/9  PacBrew's OpenOrbis PS4 toolchain"
if [ ! -f "$OPENORBIS/ps4vars.sh" ]; then
    if ! command -v pacman >/dev/null; then
        if apt-cache show pacman-package-manager >/dev/null 2>&1; then
            sudo apt-get install -y pacman-package-manager makepkg
        else
            d="$(mktemp -d)"
            ( cd "$d" &&
              wget -q http://launchpadlibrarian.net/635298936/libalpm13_13.0.2-3_amd64.deb &&
              wget -q http://launchpadlibrarian.net/635298938/pacman-package-manager_6.0.2-3_amd64.deb &&
              wget -q http://launchpadlibrarian.net/635298937/makepkg_6.0.2-3_amd64.deb &&
              sudo dpkg -i libalpm13_13.0.2-3_amd64.deb pacman-package-manager_6.0.2-3_amd64.deb makepkg_6.0.2-3_amd64.deb ) || true
            sudo apt-get -y -f install
        fi
    fi
    command -v pacman >/dev/null || fail "couldn't install pacman (PacBrew's package manager)"
    if ! grep -q '^\[pacbrew\]' /etc/pacman.conf 2>/dev/null; then
        printf '\n[pacbrew]\nSigLevel = Optional TrustAll\nServer = https://pacman.mydedibox.fr/pacbrew/packages/\n' | sudo tee -a /etc/pacman.conf >/dev/null
    fi
    sudo pacman -Sy --noconfirm
    sudo pacman -S --noconfirm --needed ps4-openorbis ps4-openorbis-portlibs
fi
[ -f "$OPENORBIS/ps4vars.sh" ] || fail "OpenOrbis isn't at $OPENORBIS after installing it"
note "OpenOrbis: $OPENORBIS"

step "4/9  PS4 sysroot (libc++ 21 and the C library fixes)"
if [ ! -f "$PS4_SYSROOT/lib/libc++.a" ]; then
    note "building libc++ (takes a while; log: logs/libcxx.log)"
    if ! "$HERE/toolchain/build-libcxx.sh" > "$PS4_BUILD_ROOT/logs/libcxx.log" 2>&1; then
        tail -30 "$PS4_BUILD_ROOT/logs/libcxx.log"; fail "libc++ didn't build (logs/libcxx.log)"
    fi
else
    note "libc++ already built"
fi
"$HERE/toolchain/build-runtime.sh" | tail -1
"$HERE/toolchain/ucontext-overlay.sh" >/dev/null
[ -f "$PS4_SYSROOT/lib/libc++.a" ] || fail "the sysroot isn't complete"

step "5/9  RADV, the Vulkan driver for the PS4's GPU (Mesa 26.0.8)"
MESA="$PS4_BUILD_ROOT/src/mesa"
if [ ! -f "$PS4_BUILD_ROOT/build/mesa/src/amd/vulkan/libvulkan_radeon.a" ]; then
    if [ ! -f "$MESA/meson.build" ]; then
        git clone --depth 1 --branch mesa-26.0.8 https://gitlab.freedesktop.org/mesa/mesa.git "$MESA" || {
            rm -rf "$MESA"; mkdir -p "$MESA"
            curl -fL https://archive.mesa3d.org/mesa-26.0.8.tar.xz | tar -xJ -C "$MESA" --strip-components=1; }
    fi
    if ( cd "$MESA" && git apply --reverse --check "$HERE/patches/mesa-ps4.patch" ) 2>/dev/null; then
        note "Mesa's PS4 patch already applied"
    else
        ( cd "$MESA" && git apply "$HERE/patches/mesa-ps4.patch" ) || fail "couldn't apply patches/mesa-ps4.patch to Mesa"
    fi
    if [ ! -f "$PS4_BUILD_ROOT/src/libdrm/amdgpu/amdgpu.h" ]; then
        git clone --depth 1 --branch libdrm-2.4.125 https://gitlab.freedesktop.org/mesa/drm.git "$PS4_BUILD_ROOT/src/libdrm" || {
            rm -rf "$PS4_BUILD_ROOT/src/libdrm"; mkdir -p "$PS4_BUILD_ROOT/src/libdrm"
            curl -fL https://dri.freedesktop.org/libdrm/libdrm-2.4.125.tar.xz | tar -xJ -C "$PS4_BUILD_ROOT/src/libdrm" --strip-components=1; }
    fi
    if [ ! -x "$PS4_BUILD_ROOT/mesa-venv/bin/meson" ]; then
        python3 -m venv "$PS4_BUILD_ROOT/mesa-venv"
        "$PS4_BUILD_ROOT/mesa-venv/bin/pip" install -q meson mako pyyaml packaging
    fi
    note "building RADV (takes a while; log: logs/mesa.log)"
    if ! "$HERE/scripts/build-mesa.sh" setup > "$PS4_BUILD_ROOT/logs/mesa.log" 2>&1; then
        grep -E "error|ERROR|FAILED|ninja: build stopped" "$PS4_BUILD_ROOT/logs/mesa.log" | head -30
        fail "Mesa didn't build (logs/mesa.log)"
    fi
else
    note "RADV already built"
fi

step "6/9  zlib, zstd, libpng, libjpeg-turbo, libwebp, FreeType and glslang for the PS4"
"$HERE/scripts/build-deps.sh"

step "7/9  DuckStation (RSDuck's Switch fork) and the PS4 patch"
"$HERE/scripts/prepare-source.sh"

step "8/9  Building DuckStation for PS4"
"$HERE/scripts/configure-duckstation.sh" > "$PS4_BUILD_ROOT/logs/configure.log" 2>&1 || {
    tail -40 "$PS4_BUILD_ROOT/logs/configure.log"; fail "configure failed (logs/configure.log)"; }
"$HERE/scripts/build-duckstation.sh" || fail "the build failed: the errors are above, the whole log is $PS4_BUILD_ROOT/logs/build.log"

step "9/9  Making the fake package (fpkg)"
DS_VERSION="$PKG_VERSION" "$HERE/scripts/package-duckstation.sh"
PKG="${PKG_OUT:-$HERE/out}/DuckStation-v$PKG_VERSION.pkg"
[ -f "$PKG" ] || fail "no package was made"

if [ -n "${PS4_HOST:-}" ]; then
    step "Sending it to the PS4 at $PS4_HOST"
    "$HERE/scripts/upload-to-console.sh" "$PKG"
fi

printf '\n\033[1;32mDone!\033[0m  Your package: %s\n\n' "$PKG"
cat <<EOF
On the PS4:
  /data/DuckStation/bios    your PS1 BIOS (scph5501.bin, scph1001.bin, ...)
  /data/DuckStation/games   your games (.cue/.bin, .chd, .pbp, .iso), folders are fine
  USB: /mnt/usb0/PS1 is scanned too
L3 + R3 opens the pause menu in a game.
EOF
