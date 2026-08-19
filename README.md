# Stealth — ESP32-S3 Touch AMOLED 1.8

A realtime-tactics stealth puzzler for the Waveshare ESP32-S3-Touch-AMOLED-1.8
(SKU 29957), inspired by [Stealth](https://store.steampowered.com/app/2168090/Stealth/).

Guards sweep vision cones across a tile maze. You tilt the board to move,
throw sound bombs to pull guards off their patrol, free the hostages, and
reach the exit without ever being fully seen. A heartbeat in the speaker rises
as a guard closes in on identifying you.

![preview](docs/preview.png)

## The idea

Sound is the currency. Tilt gently and you walk silently; tilt hard and you
run, which is fast and *loud*. Noise flood-fills through open tiles rather
than radiating in a circle, so it travels around corners but never through
walls — a bomb thrown down a side corridor genuinely pulls guards away from
the door they were watching.

The instant a guard resolves you from "something moved" into "someone is
there", their cone turns hot and an alarm stabs — the same `detecting` flag
drives both, so the sound always lands on the frame you see the colour change.

Detection is a meter, not a trigger. Being clipped by the edge of a cone for a
moment is survivable; standing in the middle of one is not. The meter climbs
two columns up the left and right edges of the screen — when they reach the
top you have been identified — so it reads in peripheral vision without
looking away from the guard about to see you. You can hear it happening too:
the heartbeat's rate and volume both track the closest guard's certainty.

Once you *are* seen, running is not an escape. A chasing guard moves at least
as fast as you are currently moving, so the only way out is to break line of
sight.

## Controls

| Action | Input |
|---|---|
| Move | **Tilt the board.** Speed rises continuously with the angle |
| Throw a sound bomb | Tap the bomb button, then tap where it should land |
| Reveal patrol routes | **Press and hold** the map — a hairline traces each guard's full circuit |
| Re-level the tilt neutral | Short tap on the map (when no bomb is armed) — confirmed by a centred panel |
| Menus | Tap |

Speed is a continuous function of tilt angle, not a walk/run toggle: a slight
lean creeps, a hard lean sprints at 205 px/s. Past `SPRINT_THRESHOLD` your
footsteps start carrying, so the fastest route is rarely the quiet one.

The IMU reports tilt relative to a captured neutral, so you can play at any
comfortable angle. Tap the map to rebase neutral to however you're holding it.

## Build and flash

Requires ESP-IDF v5.5+. The build directory is kept outside the project
because this tree lives in iCloud Drive and syncing thousands of object files
is miserable.

```bash
./flash.sh
```

Or manually:

```bash
source ~/esp/esp-idf/export.sh && idf.py -B /tmp/stealth-build -p /dev/cu.usbmodem1101 flash monitor
```

## Screen layout

The game uses the whole 368x448 panel — there is no HUD strip. The play field
is 23x28 tiles at 16px, which fills the panel exactly. The handful of things
that must stay visible float over the field instead of taking a band from it:
level and hostage tally top-left, tilt bubble bottom-left, bomb control
bottom-right, and the alert meter climbing the left and right edges.

## Sound

There are no audio samples in flash — everything is synthesised live by
[`main/synth.c`](main/synth.c) into the ES8311 codec at 22.05kHz mono.

- **Detection alarm** — the loudest thing in the game, a tritone stab with a
  noise transient on the front. It fires on the exact frame a guard's cone
  turns hot, and ducks the music under itself so it punches.
- **Heartbeat** — two thumps (a loud "lub", a softer "dub" 30% of a cycle
  later), ramping from 52 to 168 BPM as tension climbs.
- **Music bed** — a slow unresolved drone in D minor at 76 BPM, with a sparse
  pulse on the bar and an occasional bell. A minor second fades into the drone
  *only* as tension rises, which is where the unease comes from. It stays
  quiet: it raises the floor, it does not ask to be listened to.

**Everything is pitched for this speaker, not for headphones.** The board's
speaker rolls off hard below roughly 300-400Hz, so the obvious choices — a
36Hz drone, a 62Hz heartbeat thump — are inaudible on the hardware no matter
how loud you make them. The first version of this made exactly that mistake:
98% of the music's energy sat below 250Hz and nothing could be heard.

Two things fix it. Fundamentals live inside the passband (the drone is
A4/D5/A5 rather than D1/A1), and every low-pitched sound uses `V_HARM`, which
sums a fundamental with its 2nd and 3rd harmonics — the harmonics land where
the speaker can move air and the ear still infers the missing fundamental.
The result moved the bed from 8.6% to 68.7% of its energy above 300Hz.

`tools/host/synthwav.c` renders the real synth to a WAV, which is how that was
measured without a speaker in hand.

The inner loop uses a 1024-entry sine table and multiplicative envelopes
rather than `sinf`/`expf` per sample, which is what leaves room for a
continuous music bed under the one-shot effects.

`synth.c` deliberately has no platform dependencies, so the exact code driving
the speaker can be rendered to a WAV and inspected without hardware:

```bash
clang -O2 -std=c11 -I main tools/host/synthwav.c main/synth.c -lm -o /tmp/synthwav && /tmp/synthwav out.wav
```

The game thread and the audio task never share a lock — one-shot effects cross
between them through a single-producer/single-consumer ring, so a slow frame
can never stall audio and a busy synth can never stall a frame.

## Hardware notes

Both board revisions are detected at runtime by probing the touch controller,
so one binary covers either:

| | V1 | V2 |
|---|---|---|
| Panel | SH8601 | CO5300 (+16px column gap) |
| Touch | FT3168 @ 0x38 | CST816 @ 0x15 |

Both panels take the same command set, and both touch controllers are
FocalTech-derived with an identical register block for the first contact — so
a single 5-byte read at `0x02` serves either. Panel and touch reset lines hang
off a TCA9554-style IO expander at `0x20`, not off GPIO.

Three things about this board that cost real debugging time, recorded here so
they don't have to be rediscovered:

1. **DMA straight from PSRAM needs an opt-in.** The SPI driver only accepts an
   external-RAM source when the transaction carries `SPI_TRANS_DMA_USE_PSRAM`,
   which esp_lcd sets only if you enable `flags.psram_dma_direct`. Without it
   the driver tries to bounce the whole frame through a 322KB *internal*
   buffer, which cannot be allocated, and you get
   `setup_dma_priv_buffer: Failed to allocate priv TX buffer`.

2. **…and even then, don't.** With framebuffers in PSRAM, DMA reading one
   while the CPU rasterises into the other saturates the PSRAM bus and the SPI
   peripheral underruns (`DMA TX underflow detected`). The renderer instead
   draws into two small internal-SRAM bands that ping-pong, so drawing and
   transfer overlap without ever sharing a bus.

3. **If tilt comes out mirrored**, the IMU's physical orientation relative to
   the panel is the only variable. Flip `IMU_SWAP_XY` / `IMU_INVERT_X` /
   `IMU_INVERT_Y` in [`main/imu.h`](main/imu.h); everything downstream is
   already in screen space.

## Performance

368×448, fully redrawn every frame, on the busiest level (5 guards, 3
hostages), measured on device:

| | |
|---|---|
| Framerate | **34–38 FPS** (26–29ms/frame, with audio running) |
| Cone raycasting | 1.3 ms |
| Rasterising (14 bands) | 15.5 ms |
| QSPI DMA | 9.5 ms |
| Binary | 294 KB (81% of the partition free) |
| Internal heap free | 258 KB |

QSPI runs at the vendor-validated 40MHz. That is a 16.5ms floor for a full
frame, so the renderer and the bus are roughly balanced — raising the clock is
the first place to look for more headroom.

## Layout

```
main/
  board.c     panel bring-up, revision detect, IO expander
  touch.c     unified FT3168 / CST816 driver
  imu.c       QMI8658 tilt, filtering, levelling
  audio.c     ES8311 + I2S bring-up (thin platform shim)
  synth.c     portable synthesiser: heartbeat and effects
  gfx.c       RGB565 software rasteriser (device-order pixels)
  font.c      5x7 bitmap font
  game.c      state, player, noise flood-fill, bombs
  guard.c     patrol / investigate / look / chase, BFS pathing
  level.c     the six levels
  render.c    world and overlay drawing
  hud.c       tilt input mapping, HUD
tools/
  validate_levels.py   static checks on the maps
  host/preview.c       runs the real game code natively, writes frames
  host/synthwav.c      renders the real synth to a WAV
  ppm2png.py           PPM -> PNG, no dependencies
```

### Working on the levels

The maps are 23×25 ASCII grids in [`main/level.c`](main/level.c). A single
miscounted character is invisible by eye, so validate before flashing:

```bash
python3 tools/validate_levels.py
```

It checks row widths, sealed borders, exactly one spawn and exit, and — via
flood fill — that every hostage, the exit, and every guard waypoint is
actually reachable from the player's start.

The game core is plain C with no ESP dependencies, so it also runs natively.
This renders real frames from the real code without a flash cycle:

```bash
clang -O2 -std=c11 -I main -I tools/host tools/host/preview.c \
  main/gfx.c main/font.c main/game.c main/guard.c main/level.c \
  main/render.c main/hud.c -lm -o /tmp/stealth_preview && /tmp/stealth_preview /tmp/shots
```

## Possible next steps

- Save progress to NVS so level unlocks survive a reboot
- Guards that hear *each other* — a chasing guard alerting nearby patrols
- Use the gyro as well as the accelerometer, so quick flicks read as intent
