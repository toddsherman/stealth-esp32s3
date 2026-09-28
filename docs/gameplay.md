# README gameplay animation

`gameplay.gif` is the tilting-board video from
[todd.sh/StealthGame](https://www.todd.sh/StealthGame), without its sound. It is
a render, not a camera recording of the hardware:

1. [`tools/host/capture.c`](../tools/host/capture.c) plays stage 100 through the
   actual game code with an autopilot, and records every frame, the
   synthesised audio, and the tilt and touch it applied on each frame.
2. [`tools/tiltvideo`](../tools/tiltvideo) replays that run on a 3D model of the
   board: the screen shows the recorded frames, and the board leans by the
   recorded tilt, reversed through the game's own response curve (a ~3 degree
   deadzone, ~29 degrees at full tilt). Taps show as rings on the glass.

Rebuild the video, then the GIF (360 × 450 at 12 FPS):

```bash
./tools/tiltvideo/make.sh 100 stealth-tilt.mp4
ffmpeg -i stealth-tilt.mp4 -vf "fps=12,scale=360:-1:flags=lanczos,palettegen=max_colors=128:stats_mode=diff" pal.png
ffmpeg -i stealth-tilt.mp4 -i pal.png \
  -lavfi "fps=12,scale=360:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" \
  docs/gameplay.gif
```
