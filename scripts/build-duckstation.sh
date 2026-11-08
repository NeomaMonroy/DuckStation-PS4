#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
BUILD="$PS4_BUILD_ROOT/build/duckstation"
mkdir -p "$PS4_BUILD_ROOT/logs"
if [ -z "${JOBS:-}" ]; then
    mem_gib=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1048576 ))
    JOBS=$(nproc); [ $(( mem_gib / 2 )) -lt "$JOBS" ] && JOBS=$(( mem_gib / 2 )); [ "$JOBS" -lt 1 ] && JOBS=1
fi
set +o pipefail
ninja -C "$BUILD" -j"$JOBS" -k "${KEEP_GOING:-0}" "${@:-duckstation-nogui_eboot}" 2>&1 | tee "$PS4_BUILD_ROOT/logs/build.log" | grep -E "error:|FAILED|warning: .*undefined|^ninja: build stopped" | head -${ERRORS:-150}
status="${PIPESTATUS[0]}"
if [ "$status" != 0 ]; then
    awk '/^FAILED: / { show = ($0 !~ /\.(o|obj)( |$)/); n = 0; if (show) print "---- " $0; next }
         show && n < 40 && !/^\[[0-9]+\/[0-9]+\]/ { print "     " $0; n++ }' "$PS4_BUILD_ROOT/logs/build.log" | head -120
fi
exit "$status"
