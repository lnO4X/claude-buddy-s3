#!/usr/bin/env bash
#
# check-toolchain.sh — Phase B2 acceptance per PLAN §3.
#
# Read-only deterministic check (per P16 + P18). Exit 0 = PASS.
#
# Verifies:
#   1. PlatformIO Core in PATH
#   2. espressif32 platform installed (S3 support comes with it)
#   3. esptool installed (needed for D6 erase_flash + flash sanity)
#
# Tool versions captured to stdout for the baseline report.

set -euo pipefail

echo "=== check-toolchain.sh — clawstick Phase B2 ==="

# 1. PlatformIO Core
echo "--- 1/3 PlatformIO Core ---"
pio --version

# 2. espressif32 platform.
#    Capture to a variable first; piping into `grep -q` would SIGPIPE pio
#    and pipefail would surface that as a false negative.
echo "--- 2/3 espressif32 platform ---"
platforms="$(pio pkg list -g --only-platforms 2>/dev/null)"
if ! grep -q ' espressif32 @' <<<"$platforms"; then
    echo "FAIL: espressif32 platform not installed" >&2
    echo "$platforms" >&2
    exit 1
fi
grep ' espressif32 @' <<<"$platforms"

# 3. esptool (binary; the .py shim is deprecated upstream as of 5.x).
echo "--- 3/3 esptool ---"
esptool version | tail -1

echo "=== toolchain OK ==="
