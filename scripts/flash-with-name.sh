#!/usr/bin/env bash
#
# flash-with-name.sh — build + flash clawstick firmware with a custom
# top-of-screen display name.
#
# Usage:
#   ./flash-with-name.sh                  # default "Claude"
#   ./flash-with-name.sh "MyBuddy"
#   ./flash-with-name.sh "桌面伙伴"        # UTF-8 OK if M5GFX font supports it
#
# Default lives in platformio.ini build_flags (-DBUDDY_DISPLAY_NAME). This
# script overrides via PIO --project-option for one-shot deploys.
#
# BLE advertising name (Claude-XXXX) is NOT affected by this script — that
# format is required by Hardware Buddy app filter (gotcha G12).

set -euo pipefail

NAME="${1:-Claude}"

# Sanitize: strip outer quotes if user accidentally passed them, escape inner "
NAME="${NAME%\"}"; NAME="${NAME#\"}"
ESCAPED="${NAME//\"/\\\"}"

cd "$(dirname "$0")/.."   # → code/

echo "=== flash-with-name: '$NAME' ==="
echo "(top-of-screen text; BLE name remains Claude-<MAC4>)"

# PIO build_flags additive: append override to whatever's in platformio.ini.
# The LAST -DBUDDY_DISPLAY_NAME wins because GCC processes flags left-to-right.
pio run -e clawstick-s3 -t upload \
    --project-option "build_flags=-DBUDDY_DISPLAY_NAME=\"\\\"$ESCAPED\\\"\""

echo "=== flashed with display name '$NAME' ==="
