#!/usr/bin/env bash
# One-shot setup for a machine that has never built this project.
#
# Installs ESP-IDF if it is missing, fetches the pinned components, builds the
# firmware, and runs the whole off-device test suite. Idempotent - safe to run
# again any time. Needs no sudo.
#
#   IDF_PATH=/somewhere/esp-idf ./tools/bootstrap.sh    # use an existing install
set -uo pipefail
cd "$(dirname "$0")/.."

IDF_VERSION="v5.5.5"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
BUILD_DIR="${BUILD_DIR:-/tmp/stealth-build}"

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
fail() { printf '\033[31mFAILED: %s\033[0m\n' "$*" >&2; exit 1; }

# --- prerequisites --------------------------------------------------------
say "prerequisites"
command -v python3 >/dev/null || fail "python3 not found"
command -v git     >/dev/null || fail "git not found"
echo "  python3 $(python3 --version 2>&1 | awk '{print $2}')"

# Apple's git refuses to run until the Xcode licence is accepted. The Command
# Line Tools copy does not, so prefer it rather than demanding a sudo step.
if ! git --version >/dev/null 2>&1; then
  if [ -x /Library/Developer/CommandLineTools/usr/bin/git ]; then
    export DEVELOPER_DIR=/Library/Developer/CommandLineTools
    echo "  using Command Line Tools git (Xcode licence not accepted)"
  else
    fail "git is present but not runnable - try: sudo xcodebuild -license accept"
  fi
fi
echo "  git $(git --version | awk '{print $3}')"

# --- ESP-IDF --------------------------------------------------------------
say "ESP-IDF $IDF_VERSION"
if [ -f "$IDF_PATH/export.sh" ]; then
  have="$(cat "$IDF_PATH/version.txt" 2>/dev/null || echo unknown)"
  echo "  found $have at $IDF_PATH"
  case "$have" in
    v5.5*|v5.6*|v6.*) ;;
    *) echo "  WARNING: this project targets $IDF_VERSION; $have may not build" ;;
  esac
else
  echo "  not found - cloning into $IDF_PATH (this is ~2GB and takes a while)"
  mkdir -p "$(dirname "$IDF_PATH")" || fail "cannot create $(dirname "$IDF_PATH")"
  git clone -b "$IDF_VERSION" --depth 1 --recursive \
      https://github.com/espressif/esp-idf.git "$IDF_PATH" \
      || fail "esp-idf clone failed"
  echo "  installing the esp32s3 toolchain"
  (cd "$IDF_PATH" && ./install.sh esp32s3) || fail "install.sh failed"
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1 || fail "could not source export.sh"
command -v idf.py >/dev/null || fail "idf.py not on PATH after export.sh"
echo "  idf.py $(idf.py --version 2>&1 | tail -1)"

# --- off-device checks ----------------------------------------------------
# Run these before building firmware: they need only a host compiler, so a
# failure here is a real problem rather than a toolchain problem.
say "off-device test suite"
./tools/check.sh || fail "off-device checks did not pass"

# --- firmware -------------------------------------------------------------
say "building firmware"
echo "  build directory: $BUILD_DIR (kept outside the tree; this repo may live in iCloud)"
idf.py -B "$BUILD_DIR" build >/tmp/stealth-bootstrap-build.log 2>&1 \
  || { tail -30 /tmp/stealth-bootstrap-build.log; fail "build failed"; }
grep -E "binary size" /tmp/stealth-bootstrap-build.log | sed 's/^/  /'

# --- done -----------------------------------------------------------------
say "ready"
port="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
if [ -n "$port" ]; then
  echo "  board detected on $port"
  echo "  flash it with:   ./flash.sh"
else
  echo "  no board detected (looked for /dev/cu.usbmodem*)"
  echo "  plug one in, then:   ./flash.sh"
fi
echo "  run the tests any time with:   ./tools/check.sh"
echo "  read CLAUDE.md for the project's conventions and gotchas"
