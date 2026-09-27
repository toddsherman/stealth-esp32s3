#!/usr/bin/env bash
# Builds and runs every off-device check. No hardware needed.
set -uo pipefail
cd "$(dirname "$0")/.."

CORE=(main/gfx.c main/font.c main/game.c main/guard.c main/level.c
      main/level_gen.c main/render.c main/hud.c main/synth.c main/tilt.c)
OUT=${TMPDIR:-/tmp}
fails=0

echo "== level tables =="
python3 tools/validate_levels.py | tail -2 || fails=$((fails+1))

for t in inputtest routecheck smoke soak tilttest; do
  echo
  echo "== $t =="
  if ! clang -O2 -std=c11 -Wall -Wextra -I main -I tools/host \
       "tools/host/$t.c" "${CORE[@]}" -lm -o "$OUT/$t"; then
    echo "  build failed"; fails=$((fails+1)); continue
  fi
  "$OUT/$t" | tail -3 || fails=$((fails+1))
  [ "${PIPESTATUS[0]}" -eq 0 ] || fails=$((fails+1))
done

echo
if [ "$fails" -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$fails CHECK(S) FAILED"; fi
exit "$fails"
