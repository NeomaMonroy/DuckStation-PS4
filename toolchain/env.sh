export PSB="${PSB:-$HOME/dk64-build}"
export OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
export PS4_SYSROOT="${PS4_SYSROOT:-$PSB/sysroot}"
export PS4_TARGET="x86_64-pc-freebsd12-elf"
export CLANG_VER="${CLANG_VER:-21}"
export PS4_CC="clang-$CLANG_VER"
export PS4_CXX="clang++-$CLANG_VER"
export PS4_LD="ld.lld-$CLANG_VER"
export PS4_AR="llvm-ar-$CLANG_VER"
export PS4_RANLIB="llvm-ranlib-$CLANG_VER"

export PS4_BASE_FLAGS="--target=$PS4_TARGET -D_GNU_SOURCE -D__PS4__ -D__ORBIS__ -D__OPENORBIS__ -fPIC -funwind-tables \
 --sysroot=$OPENORBIS -isystem $PS4_SYSROOT/libc-overlay -isystem $OPENORBIS/include"
export PS4_CXX_FLAGS="-nostdinc++ -isystem $PS4_SYSROOT/include/c++/v1"

export JOBS="${JOBS:-$(nproc)}"

need() {
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 || { echo "missing tool: $t (see toolchain/README.md)" >&2; exit 1; }
    done
}
