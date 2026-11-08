PS4_BUILD_ROOT="${PS4_BUILD_ROOT:-$HOME/dk64-build}"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
source "$OPENORBIS/ps4vars.sh"
if ldd "$OPENORBIS/bin/ld.lld" | grep -q "libxml2.so.2 => not found"; then
    compat=$(ls /usr/lib/x86_64-linux-gnu/libxml2.so.* 2>/dev/null | grep -E 'libxml2\.so\.[0-9]+$' | head -1)
    if [ -n "$compat" ]; then
        mkdir -p "$PS4_BUILD_ROOT/host-compat"
        ln -sf "$compat" "$PS4_BUILD_ROOT/host-compat/libxml2.so.2"
        export LD_LIBRARY_PATH="$PS4_BUILD_ROOT/host-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
fi
ps4_cmake() {
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake -DCMAKE_TOOLCHAIN_FILE="$OPENORBIS/cmake/ps4.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
}
