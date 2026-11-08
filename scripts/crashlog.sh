#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
ELF="${1:-$(ls -t "$PS4_BUILD_ROOT"/oelf/duckstation-v*.elf 2>/dev/null | head -1)}"
HOST="ftp://${PS4_HOST:?set PS4_HOST to your PS4 IP}:${PS4_FTP_PORT:-2121}"
OUT="$PS4_BUILD_ROOT/logs"
mkdir -p "$OUT"
for f in boot-trace.log crash.log freeze.log ps4.log duckstation.log; do
    curl -s --max-time 30 -o "$OUT/$(basename "$f")" "$HOST/data/DuckStation/$f" || rm -f "$OUT/$(basename "$f")"
done
if [ -z "${1:-}" ]; then
    ran="$(grep -oE "for PS4 v[0-9]{2}\.[0-9]{2}" "$OUT/ps4.log" 2>/dev/null | head -1 | grep -oE "[0-9]{2}\.[0-9]{2}" || true)"
    [ -n "$ran" ] && [ -f "$PS4_BUILD_ROOT/oelf/duckstation-v$ran.elf" ] && ELF="$PS4_BUILD_ROOT/oelf/duckstation-v$ran.elf"
fi
echo "== symbols from $(basename "$ELF")"
symbolize() {
    llvm-symbolizer-21 --obj="$ELF" --functions=linkage --demangle --no-inlines "$1" | head -2 | paste -sd' '
}
print_thread_dump() {
    while IFS= read -r line; do
        case "$line" in
        "-- "*)
            echo "$line"
            rip="$(echo "$line" | grep -oE "rip elf 0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" || true)"
            [ -n "$rip" ] && echo "     rip  $(symbolize "$rip")"
            ;;
        *"stack:"*)
            for addr in $(echo "$line" | grep -oE "0x[0-9a-f]+" | head -14); do
                echo "     $addr  $(symbolize "$addr")"
            done
            ;;
        *) echo "$line" ;;
        esac
    done
}
echo "== boot-trace.log"; cat "$OUT/boot-trace.log" 2>/dev/null || echo "(none)"
echo "== ps4.log (last 60 lines)"; tail -60 "$OUT/ps4.log" 2>/dev/null || echo "(none)"
echo "== duckstation.log (last 40 lines)"; tail -40 "$OUT/duckstation.log" 2>/dev/null || echo "(none)"
if [ -s "$OUT/crash.log" ] && grep -q "crashed" "$OUT/boot-trace.log" 2>/dev/null; then
    echo "== crash.log (last crash)"
    last="$(awk '/^=== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/crash.log")"
    crashed="$(printf '%s\n' "$last" | sed '/^==== /,$d')"
    printf '%s\n' "$crashed" | grep -v "^  \[rsp"
    echo "== symbolized"
    printf '%s\n' "$crashed" | grep -oE "(rip 0x[0-9a-f]+ elf |^  \[rsp\+0x[0-9a-f]+\] )0x[0-9a-f]+" | grep -oE "0x[0-9a-f]+$" |
        while read -r addr; do
            printf '%s  ' "$addr"
            symbolize "$addr"
        done
    if printf '%s\n' "$last" | grep -q "^==== "; then
        echo "== the other threads at the crash"
        printf '%s\n' "$last" | sed -n '/^==== /,$p' | print_thread_dump
    fi
else
    echo "== crash.log: no crash in this run"
fi
if [ -s "$OUT/freeze.log" ]; then
    echo "== freeze.log (last dump, symbolized; stacks may include stale return addresses)"
    awk '/^==== /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$OUT/freeze.log" | print_thread_dump
fi
