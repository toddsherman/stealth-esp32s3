#!/usr/bin/env bash
# Records an autopilot run of a stage and renders it as the tilting-board video.
#
#   ./tools/tiltvideo/make.sh [STAGE] [OUT.mp4]      defaults: 100, stealth-tilt.mp4
#
# Needs clang, ffmpeg, node, python3 and Google Chrome (or CHROME=/path).
set -euo pipefail
cd "$(dirname "$0")/../.."
STAGE="${1:-100}"
OUT="${2:-stealth-tilt.mp4}"
HERE=tools/tiltvideo
WORK="$HERE/work"
PORT=8765

[ -d "$HERE/node_modules" ] || (cd "$HERE" && npm install --no-audit --no-fund)

echo "== recording stage $STAGE"
rm -rf "$WORK" && mkdir -p "$WORK/run" "$WORK/frames" "$WORK/out"
clang -O2 -std=c11 -I main -I tools/host tools/host/capture.c \
  main/gfx.c main/font.c main/game.c main/guard.c main/level.c \
  main/level_gen.c main/render.c main/hud.c main/synth.c -lm -o "$WORK/capture"
"$WORK/capture" record "$STAGE" "$WORK/run"
cp "$WORK/run/trace.csv" "$WORK/trace.csv"

# Nearest-neighbour 2x keeps the game's hairlines crisp as a texture.
ffmpeg -hide_banner -loglevel error -y -i "$WORK/run/f%05d.ppm" \
  -vf scale=736:896:flags=neighbor "$WORK/frames/f%05d.png"

echo "== rendering"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$HERE" >/dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
sleep 1
node "$HERE/render.mjs" "http://127.0.0.1:$PORT/scene.html" "$WORK/out" all

echo "== encoding $OUT"
ffmpeg -hide_banner -loglevel error -y -framerate 30 -i "$WORK/out/r%05d.jpg" \
  -i "$WORK/run/audio.wav" -af volume=5dB -c:v libx264 -profile:v high \
  -pix_fmt yuv420p -crf 19 -preset slow -c:a aac -b:a 128k -ac 1 \
  -movflags +faststart -shortest "$OUT"
echo "wrote $OUT"
