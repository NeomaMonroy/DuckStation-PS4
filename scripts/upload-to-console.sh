#!/usr/bin/env bash
set -euo pipefail
PKG="${1:?package path}"
: "${PS4_HOST:?set PS4_HOST to the IP address of your PS4 (Settings > Network > View Connection Status)}"
NAME="$(basename "$PKG")"
HOST="ftp://$PS4_HOST:${PS4_FTP_PORT:-2121}"
SIZE="$(stat -c %s "$PKG")"
export MSYS_NO_PATHCONV=1

listing() { curl -sS --list-only "$HOST/data/pkg/" | tr -d '\r'; }
present() { curl -sS "$HOST/data/pkg/" | tr -d '\r' | awk -v n="$NAME" -v s="$SIZE" '$NF == n && $5 == s { f = 1 } END { exit !f }'; }
settled() { for _ in 1 2 3; do sleep 10; present || return 1; done; }

curl -sS "$HOST/" -Q "MKD /data/pkg" >/dev/null 2>&1 || true
for try in 1 2 3; do
    curl -sS -T "$PKG" "$HOST/data/pkg/"
    echo "uploaded $NAME"
    settled && break
    echo "console: $NAME didn't stay, uploading again"
done
OLD_PACKAGES='^(DuckStation-v[0-9.]+|IV0000-DUCK00001_00-[A-Z0-9]+)[.]pkg$'
for old in $(listing | grep -E "$OLD_PACKAGES" | grep -vx "$NAME"); do
    if curl -sS "$HOST/" -Q "DELE /data/pkg/$old" > /dev/null; then echo "console: removed old $old"; else echo "console: could not remove $old"; fi
done
listing | grep -E '^(DuckStation-v|IV0000-DUCK00001)' | sed 's/^/console has: /'
