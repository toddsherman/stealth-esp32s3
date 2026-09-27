# Contributing to Stealth Game

Thanks for helping make this useful to other ESP32 and Waveshare developers.
Small, focused changes are easiest to review. For a large feature or new board
port, open an issue first to describe the approach.

## Start without a board

Install Git, Python 3, Bash and Clang, clone the repository, then run:

```bash
./tools/check.sh
```

All six suites run on a computer without ESP-IDF or hardware. GitHub Actions
runs them on Linux and macOS and builds the firmware with ESP-IDF v5.5.5.
For device setup, see the [quick start](README.md#quick-start).

## Useful contributions

- Test the game on your exact board revision and report display, touch, tilt
  and audio behaviour, including any serial logs.
- Add a regression test for an input, guard, rendering or audio bug.
- Document a port to another ESP32 board, including its pin map and which
  features were tested on hardware.
- Improve setup instructions using a fresh Linux, macOS or Windows machine.
- Measure frame time on the heaviest eight-guard stages; include the board
  revision, firmware commit and measurement method.

The [engineering reference](docs/engineering.md) explains the existing design
and [possible next steps](docs/engineering.md#possible-next-steps).

## Code and validation

- Keep the game core independent of ESP-IDF. Hardware code passes input into
  the simulation; the simulation must still compile with the host harnesses.
- Explain hardware constraints and non-obvious choices in comments.
- For a bug fix, add a regression check that fails before the fix where practical.
- Run `./tools/check.sh` before submitting a change. For firmware changes, also
  build with ESP-IDF v5.5.5 and report whether you tested on a board.
- Edit `tools/gen_levels.py` to change stage generation; regenerate
  `main/level_gen.c` rather than editing the generated table by hand. Changes
  to that table invalidate saved stage records.
- Do not commit SDK installations, `managed_components/`, generated
  `sdkconfig`, build directories or serial logs containing personal data.

A successful host test or firmware build does not validate a panel, speaker or
sensor on a physical board. State exactly what you tested and what still needs
hardware verification.

## Report an issue or open a pull request

For a hardware issue, include the board model/revision, ESP-IDF version, commit,
steps to reproduce, expected/actual behaviour and relevant serial output. For a
portable-core issue, include the host OS and compiler version.

In a pull request, explain the problem, what changed, and the checks you ran.
Include screenshots or a short clip for rendering changes. Changes are
contributed under the repository's [MIT license](LICENSE).
