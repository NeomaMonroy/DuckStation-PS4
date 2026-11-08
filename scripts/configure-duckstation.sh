#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
PSB="$PS4_BUILD_ROOT"
SRC="${DS_SRC:-$PSB/src/duckstation-nx}"
BUILD="$PSB/build/duckstation"
PORT="$(cd "$HERE/.." && pwd)"
export PS4_SYSROOT="$PSB/sysroot"
unset PKG_CONFIG_PATH
[ -f "$SRC/src/common/ps4_host.h" ] || { echo "run scripts/prepare-source.sh first" >&2; exit 1; }
[ -f "$PSB/duckstation-sysroot/lib/libglslang.a" ] || { echo "run scripts/build-deps.sh first" >&2; exit 1; }
MESA_VK="$PSB/build/mesa/src/amd/vulkan/libvulkan_radeon.a"
[ -f "$MESA_VK" ] || { echo "RADV isn't built ($MESA_VK): run setup-everything.sh" >&2; exit 1; }
ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PORT/toolchain/ps4-duckstation.cmake" \
    "-DCMAKE_PROJECT_duckstation_INCLUDE=$PORT/cmake/ps4-project.cmake" \
    -DPS4_DEPS_FILE="$PORT/cmake/ps4-deps.cmake" \
    -DPS4_DEPS_ROOT="$PSB/duckstation-sysroot" \
    -DPS4_PAID="${PS4_PAID:-0x3800000000000035}" \
    -DPS4_MESA_BUILD="$PSB/build/mesa" \
    -DPS4_SYSROOT="$PS4_SYSROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NOGUI_FRONTEND=ON -DBUILD_QT_FRONTEND=OFF -DBUILD_REGTEST=OFF -DBUILD_TESTS=OFF \
    -DENABLE_OPENGL=OFF -DENABLE_VULKAN=ON -DHOST_PAGE_SIZE=16384 -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
    -DCMAKE_HAVE_LIBC_PTHREAD=1 -DCMAKE_THREAD_LIBS_INIT= -DThreads_FOUND=TRUE \
    "$@"
