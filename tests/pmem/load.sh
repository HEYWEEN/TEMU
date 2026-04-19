#!/usr/bin/env bash
#
# load.sh — smoke test for Stage 2 pmem + image loading.
#
# Write a 16-byte image with a distinctive byte pattern, start TEMU
# with that image as the positional argument, and use the REPL's
# `x 4 $pc` command to dump the first four 32-bit words. Verify the
# output matches the expected little-endian packing. Also verify that
# an out-of-bounds read triggers the pmem assertion.

set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF_DIR/../.." && pwd)
TEMU=$ROOT/build/temu

if [[ ! -x $TEMU ]]; then
    echo "temu binary missing. Run: make" >&2
    exit 2
fi

TMPIMG=$(mktemp)
trap 'rm -f "$TMPIMG"' EXIT

# 16 predictable bytes. Little-endian 4-byte reads at offset 0, 4, 8, 12:
#   0x44332211  0x88776655  0xccbbaa99  0x00ffeedd
printf '\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff\x00' > "$TMPIMG"

# --- case 1: loaded image, little-endian read ---
EXPECTED='0x80000000: 0x44332211 0x88776655 0xccbbaa99 0x00ffeedd'
OUTPUT=$(printf 'x 4 $pc\nq\n' | "$TEMU" "$TMPIMG" 2>/dev/null)

if ! grep -qF "$EXPECTED" <<< "$OUTPUT"; then
    echo "pmem load test: FAIL"
    echo "--- expected:"
    echo "$EXPECTED"
    echo "--- got:"
    echo "$OUTPUT"
    exit 1
fi
echo "pmem load test: OK (image loaded, little-endian read matches)"

# --- case 2: out-of-bounds read should abort ---
OOB_OUT=$(printf 'x 1 0x40000000\nq\n' | "$TEMU" 2>&1 || true)

if ! grep -q 'out of bound' <<< "$OOB_OUT"; then
    echo "pmem OOB test: FAIL (no assertion message)"
    echo "--- got:"
    echo "$OOB_OUT"
    exit 1
fi
echo "pmem OOB test: OK (assertion fired on out-of-range read)"
