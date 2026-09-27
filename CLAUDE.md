# Stealth — working notes for Claude Code

A tilt-controlled stealth game for the **Waveshare ESP32-S3-Touch-AMOLED-1.8**
(SKU 29957). Read [README.md](README.md) for what the game *is*; this file is
about how to work on it.

## First thing to do on a new machine

```bash
./tools/bootstrap.sh
```

It checks for ESP-IDF v5.5+, installs it if missing, fetches the pinned
components, builds, and runs the whole off-device test suite. Takes ~15 minutes
on a cold machine (the toolchain is ~2GB), seconds thereafter.

## The loop

```bash
./tools/check.sh                 # all off-device tests — no hardware needed
./flash.sh                       # validate levels, build, flash the board
python3 tools/gen_levels.py      # regenerate the 100 stages
```

**`./tools/check.sh` is the main safety net and needs no board.** It compiles
the game's own C sources natively and runs four suites: level validation, input
and menu geometry, patrol-route rendering, and a numerical soak. Run it before
every commit. It exits non-zero on failure.

## Layout

| | |
|---|---|
| `main/board.c` `touch.c` `imu.c` `audio.c` `button.c` `scores.c` | hardware: panel, touch, IMU, codec, button, NVS |
| `main/game.c` `guard.c` `level.c` `level_gen.c` | simulation and stage data |
| `main/gfx.c` `font.c` `render.c` `hud.c` `synth.c` | rasteriser, drawing, sound |
| `main/main.c` | init and the frame loop |
| `tools/host/` | native harnesses and tests |
| `tools/gen_levels.py` | the stage generator |

**The game core is deliberately free of ESP dependencies.** `game.c`, `guard.c`,
`gfx.c`, `render.c`, `hud.c`, `level*.c` and `synth.c` all compile natively
against a three-line `esp_err.h` stub in `tools/host/`. That is what makes the
whole test suite possible without hardware — keep it that way. Platform code
passes data *in* (tilt, touch, button) rather than being called *out* to.

## Hardware facts worth not rediscovering

- **Two board revisions**, detected at runtime by probing the touch I²C address:
  V1 is SH8601 + FT3168 (0x38), V2 is CO5300 + CST816 (0x15). Both panels take
  the same command set; V2 needs a 16px column gap. One binary covers both.
- **Panel/touch reset lines are on an I²C IO expander at 0x20**, not GPIO.
- **The panel is a rounded rectangle**, ~52px corner radius (`SCREEN_CORNER_R`).
  Anything drawn into the corners sits under the bezel and is never seen.
- **Pins**: LCD QSPI 4/5/6/7 + SCK 11 + CS 12, I²C 14/15, touch INT 21, audio
  MCLK 16 / BCLK 9 / WS 45 / DOUT 8 / DIN 10 / PA 46, BOOT button GPIO 0.

## Three findings that cost real time

**Render in bands out of internal SRAM, not a PSRAM framebuffer.** DMA reading
a PSRAM framebuffer while the CPU rasterises into the other one saturates the
PSRAM bus and the SPI peripheral underruns. Also: esp_lcd only accepts a PSRAM
source when `flags.psram_dma_direct` is set, otherwise it tries to bounce the
whole frame through a 322KB *internal* buffer that cannot be allocated.

**Band count dominates everything else.** Each band costs a synchronous
window-set round trip before its pixels can stream. Going from 14 bands to 4
was worth 34.5 → 50 FPS — more than every CPU-side optimisation combined. The
frame is now bus-bound: CPU is ~8.7ms of a 20ms frame. The only remaining lever
is the QSPI clock (40MHz, vendor default) in `board.c`, which needs eyes on the
panel to validate.

**Audio must be pitched for the speaker.** It rolls off hard below ~300–400Hz,
so a 36Hz drone or a 62Hz heartbeat thump is inaudible at any volume. Low
sounds use `V_HARM` (fundamental + 2nd + 3rd harmonics) so the harmonics land
where the speaker works and the ear infers the missing fundamental.

## Process notes

**Verify every edit landed.** Three separate bugs in this project's history were
string replacements that silently matched nothing because earlier changes had
altered the anchor text — the "fix" was never in the file. Prefer edits that
fail loudly on a non-match, and `grep` afterwards.

**Check flashes actually happened.** `idf.py flash` can print `Connecting...`
and stop without an error you would notice, usually because a stray serial
reader still holds the port. Grep the output for `Hash of data verified` (expect
3) rather than trusting exit status. If flashing fails, check
`lsof /dev/cu.usbmodem*` for a leftover reader.

**Measure before optimising.** The per-guard-per-frame BFS looked like an
obvious hotspot and turned out to be 200µs of a 20ms frame. Profile first.

**Write the failing test first.** Doing this caught that stage 1 ships zero
bombs, so a test for "starting a stage must not throw a bomb" passed against the
*broken* build. A test that cannot fail proves nothing.

**Regenerating stages invalidates saved records.** Records are keyed by stage
index, so `main.c` fingerprints the stage table (FNV-1a over every row) and
`scores.c` clears records when it changes. Expected, not a bug.

## Conventions

- Comments explain *why*, especially where something looks odd — a magic
  constant, a non-obvious ordering, a workaround for hardware behaviour. Don't
  narrate what the code plainly says.
- Stage difficulty is **measured, not assumed**. See the generator's docstring
  and the README section on it before touching stage generation.
- Diagnostics belong on the serial log (phase changes, menu taps with
  coordinates and resolved row). They exist because the board is often in hand
  and unreachable, and they have repeatedly been the thing that found the bug.

## Known environment quirks

- The project lives in iCloud Drive, so the build directory is kept **outside**
  the tree (`/tmp/stealth-build`, see `flash.sh`) — syncing thousands of object
  files is miserable.
- On this Mac, `/usr/bin/git` may refuse to run pending an Xcode licence
  agreement. Either `sudo xcodebuild -license accept`, or prefix commands with
  `DEVELOPER_DIR=/Library/Developer/CommandLineTools`, which uses the Command
  Line Tools git instead.
- `i2s_common: i2s_channel_disable ... not been enabled yet` is logged once at
  boot by the codec framework. It is benign.
