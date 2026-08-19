#!/usr/bin/env bash
# Build and flash. The build directory deliberately lives outside the project
# tree, which is in iCloud Drive.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-/tmp/stealth-build}"
PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

if [ -z "${PORT:-}" ]; then
  echo "No /dev/cu.usbmodem* found. Is the board plugged in?" >&2
  exit 1
fi

source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null

python3 tools/validate_levels.py
idf.py -B "$BUILD_DIR" -p "$PORT" build flash "$@"
