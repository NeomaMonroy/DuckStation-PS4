#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
PSB="$PS4_BUILD_ROOT"
export PS4_SYSROOT="$PSB/sysroot"
DEPS="$PSB/duckstation-sysroot"
mkdir -p "$PSB/logs" "$PSB/src" "$DEPS/lib" "$DEPS/include"

deps_cmake() {
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-deps.cmake" -DPS4_TRY_LINK=ON \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF "$@"
}

fetch() {
    local dir="$1" url="$2" tag="$3"
    if [ ! -d "$PSB/src/$dir/.git" ]; then
        rm -rf "$PSB/src/$dir"
        git clone -q --depth 1 --branch "$tag" "$url" "$PSB/src/$dir"
    fi
}

configure_build() {
    local name="$1" src="$2"
    shift 2
    local B="$PSB/build/ds-$name"
    rm -rf "$B"
    deps_cmake -S "$src" -B "$B" "$@" > "$PSB/logs/ds-$name-configure.log" 2>&1 || { tail -40 "$PSB/logs/ds-$name-configure.log"; exit 1; }
    ninja -C "$B" > "$PSB/logs/ds-$name-build.log" 2>&1 || { grep -E "error|undefined" "$PSB/logs/ds-$name-build.log" | head -40; exit 1; }
    ninja -C "$B" install > /dev/null
}

done_already() {
    [ -f "$DEPS/lib/$1" ] && { echo "  $2: already built"; return 0; }
    return 1
}

build_zlib() {
    done_already libz.a zlib && return
    fetch zlib https://github.com/madler/zlib.git v1.3.1
    local B="$PSB/build/ds-zlib"
    rm -rf "$B"
    deps_cmake -S "$PSB/src/zlib" -B "$B" -DZLIB_BUILD_EXAMPLES=OFF > "$PSB/logs/ds-zlib-configure.log" 2>&1 || { tail -40 "$PSB/logs/ds-zlib-configure.log"; exit 1; }
    ninja -C "$B" zlibstatic > "$PSB/logs/ds-zlib-build.log" 2>&1 || { grep -E "error|undefined" "$PSB/logs/ds-zlib-build.log" | head -40; exit 1; }
    cp "$B/libz.a" "$DEPS/lib/libz.a"
    cp "$PSB/src/zlib/zlib.h" "$B/zconf.h" "$DEPS/include/"
    echo "  zlib: built"
}

build_zstd() {
    done_already libzstd.a zstd && return
    fetch zstd https://github.com/facebook/zstd.git v1.5.7
    configure_build zstd "$PSB/src/zstd/build/cmake" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
        -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_DICTBUILDER=OFF -DZSTD_LEGACY_SUPPORT=OFF -DZSTD_MULTITHREAD_SUPPORT=ON -DCMAKE_INSTALL_LIBDIR=lib
    echo "  zstd: built"
}

build_png() {
    done_already libpng16.a libpng && return
    fetch libpng https://github.com/pnggroup/libpng.git v1.6.47
    configure_build png "$PSB/src/libpng" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
        -DPNG_FRAMEWORK=OFF -DPNG_HARDWARE_OPTIMIZATIONS=ON -DCMAKE_INSTALL_LIBDIR=lib -DZLIB_ROOT="$DEPS" \
        -DZLIB_INCLUDE_DIR="$DEPS/include" -DZLIB_LIBRARY="$DEPS/lib/libz.a"
    echo "  libpng: built"
}

build_jpeg() {
    done_already libjpeg.a libjpeg-turbo && return
    fetch libjpeg-turbo https://github.com/libjpeg-turbo/libjpeg-turbo.git 3.1.0
    configure_build jpeg "$PSB/src/libjpeg-turbo" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF \
        -DWITH_SIMD=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DCMAKE_INSTALL_LIBDIR=lib
    echo "  libjpeg-turbo: built"
}

build_webp() {
    done_already libwebp.a libwebp && return
    fetch libwebp https://github.com/webmproject/libwebp.git v1.4.0
    configure_build webp "$PSB/src/libwebp" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF \
        -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
        -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_USE_THREAD=ON \
        -DWEBP_ENABLE_SIMD=ON -DCMAKE_INSTALL_LIBDIR=lib
    echo "  libwebp: built"
}

build_freetype() {
    done_already libfreetype.a freetype && return
    fetch freetype https://github.com/freetype/freetype.git VER-2-13-3
    configure_build freetype "$PSB/src/freetype" -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
        -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON -DCMAKE_INSTALL_LIBDIR=lib
    echo "  freetype: built"
}

build_glslang() {
    done_already libglslang.a glslang && return
    fetch glslang https://github.com/KhronosGroup/glslang.git 16.0.0
    configure_build glslang "$PSB/src/glslang" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF \
        -DENABLE_SPVREMAPPER=OFF -DENABLE_HLSL=OFF -DGLSLANG_ENABLE_INSTALL=ON -DCMAKE_INSTALL_LIBDIR=lib
    echo "  glslang: built"
}

for dep in "${@:-zlib zstd png jpeg webp freetype glslang}"; do
    for d in $dep; do "build_$d"; done
done
ls "$DEPS/lib"/*.a | sed 's#.*/#  #'
