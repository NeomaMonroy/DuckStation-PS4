#!/bin/sh
set -e
in=$1
out=$2
heap_mib=$3

data=$(mktemp)
python3 - "$data" "$heap_mib" <<'EOF'
import struct, sys
path, mib = sys.argv[1], int(sys.argv[2])
blob = struct.pack("<IIQII", 1, 0, mib * 1024 * 1024, 0, 0) + bytes(0x28 - 24)
open(path, "wb").write(blob)
EOF
llvm-objcopy-21 --update-section .data="$data" "$in" "$out"
rm -f "$data"
