# ThreadXShell

A multi-board firmware project providing an interactive shell console on top of
Eclipse ThreadX, built with CMake + Ninja (vendor layers: ST HAL on the STM32
boards, the Himax WiseEye2 SDK on the Grove Vision AI V2).

This repository unifies the shell implementations grown separately in
[stm32f746g-disco](https://github.com/owhinata/stm32f746g-disco) and
[wio-lite-ai](https://github.com/owhinata/wio-lite-ai) into a single
board-independent shell core with per-board ports.

## Supported boards

| Board | MCU | Clock | Console | Flashing |
|---|---|---|---|---|
| STM32F746G-DISCO | STM32F746NGH6 (Cortex-M7) | 216 MHz | VCP (USART1, 115200) | ST-Link |
| Wio Lite AI | STM32H725AEI6 (Cortex-M7) | 550 MHz (inherited from DFU bootloader) | USB CDC (TinyUSB) | DFU (`dfu-util`) |
| Grove Vision AI V2 | Himax HX6538 WiseEye2 (dual Cortex-M55 + Ethos-U55; app on CM55M) | 400 MHz (inherited from the Himax bootloader) | UART0 via CH343P bridge, 921600 | UART xmodem to the Himax bootloader |

More boards are planned.

## Architecture

One-way layering:

```
HAL / CMSIS / ThreadX (lib/, upstream submodules, read-only)
  <- port (per-board glue)
  <- shell (board-independent core, commands, transport backends)
  <- app
```

The Wio Lite AI DFU bootloader is an independent tree that shares no sources with
the app/shell layers and is treated as immutable.

## Repository layout

```
CMakeLists.txt        project, board selection, submodule bootstrap, cli_version.h
cmake/                toolchain file (fetches ARM GNU on first configure), version template
shell/                board-independent: core/ include/ backend/ cmds/ test/
svc/                  board-independent services (fmt, ymodem, frame pipeline, gfx)
lib/                  upstream mirror submodules (read-only)
boards/<board>/       board.cmake, src/ port/ cmds/ backend/ svc/ include/ ldscript/ cmake/
```

Anything that reaches for the HAL, a peripheral or a specific memory map lives
under `boards/<board>/`; `shell/` and `svc/` carry no board conditionals.

## Building

One build directory per board, `build/<board>/` by convention (variant trees of
the same board take a suffix, e.g. `build/wio-lite-ai-tflm`). The board is chosen
at configure time and there is no default. The first configure downloads the
pinned ARM GNU toolchain (~155 MB) into `tools/`, so it needs network access.

```bash
cmake -B build/wio-lite-ai -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=wio-lite-ai
cmake --build build/wio-lite-ai        # -> shell.{elf,bin,hex}, blink.{elf,bin,hex}
                                       #    + boot-reference/boot.{elf,bin,hex}

cmake -B build/f746g-disco -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=f746g-disco
cmake --build build/f746g-disco        # -> shell.{elf,bin,hex}
```

Configuring without `-DBOARD` fails with the list of available boards. Each board
declares the upstream mirrors it needs in `boards/<board>/submodules.cmake`, and
only those are fetched -- the first `f746g-disco` configure is heavy (GUIX alone
is about 1.5 GB) and none of it is pulled in for a `wio-lite-ai` build.

The Grove Vision AI V2 additionally fetches the Himax WiseEye2 SDK (~480 MB, a
pinned commit) into `boards/grove-vision-ai-v2/sdk/` on first configure -- not a
submodule, git-ignored, treated read-only; `-DGROVE_SDK_DIR=<path>` points at an
existing checkout instead:

```bash
cmake -B build/grove-vision-ai-v2 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=grove-vision-ai-v2
cmake --build build/grove-vision-ai-v2 # -> shell.elf + shell.img (signed flash image)
```

## Flashing

### STM32F746G-DISCO (ST-Link)

```bash
cmake --build build/f746g-disco --target flash
# or: st-flash --connect-under-reset --reset write build/f746g-disco/shell.bin 0x08000000
```

The console is the on-board ST-Link VCP: 115200 8N1 on `/dev/ttyACM0`.

### Wio Lite AI (DFU only)

Put the board in DFU mode by holding USER (PF1) during reset, then:

```bash
cmake --build build/wio-lite-ai --target flash
# `flash` is an alias for `dfu-shell`; `dfu-blink` programs the blink firmware
# or: dfu-util -d 0483:df11 -a 0 -D build/wio-lite-ai/shell.bin
```

### Grove Vision AI V2 (UART xmodem)

Close any terminal on the serial port, run the target, and press the board's
RESET button when the script asks:

```bash
cmake --build build/grove-vision-ai-v2 --target flash
# xmodem upload of shell.img (a FULL flash image incl. the Himax bootloader --
# the vendor-standard flow) at 921600 on /dev/ttyACM0 (-DGROVE_SERIAL_PORT=...)
```

The console is the same serial device at 921600 8N1. Recovery and endurance
notes are in
[`boards/grove-vision-ai-v2/README.md`](boards/grove-vision-ai-v2/README.md).

`--target flash` means the same thing on every board -- program THIS board's
shell firmware the only way this board can be programmed. On Wio Lite AI that is
DFU into the app partition; it has no ST-Link path and nothing here can write
sector 0.

The app is programmed to the internal flash app partition at `0x08020000`.
Internal flash endurance is about 10k cycles, so do not reflash in an automated
loop.

### The Wio Lite AI bootloader is built, never flashed

`boards/wio-lite-ai/boot/` is the DFU bootloader that owns internal flash sector
0 (`0x08000000`). It is an independent tree: it shares no source and no header
with the app, and it is the code that recovers a board from a bad app image.
This build produces it, into `build/wio-lite-ai/boot-reference/`, and **no target
in this repository can write it anywhere**:

- no `flash-boot`. Writing sector 0 is the one operation that can brick the
  board, and exactly one board is left. The recovery procedure stays prose, in
  [`boards/wio-lite-ai/boot/README.md`](boards/wio-lite-ai/boot/README.md).
- no `dfu-boot`. That would flash the bootloader into the *app* partition: it
  would erase sector 1 (only sector 1 -- a 30,524 B transfer only reaches offset
  0, and the erase runs on 128 KB boundaries), write the boot payload there minus
  its first 32 B, and then have the commit refused because a bootloader reset
  vector is outside the app window. The app would stop booting, DFU would still
  recover it, and one erase cycle of a ~10k budget would be gone.

The reason to build it at all is continuity: a HAL, TinyUSB, toolchain or
`board.cmake` change must not break the bootloader unnoticed. `boot.bin` is
therefore checked against a golden hash, and the linked image goes through
`boards/wio-lite-ai/cmake/check_boot_safety.py` on every build (vector placement,
sector-0 containment, no option-byte/RDP/DBGMCU path, the flash-writing call
graph, the DFU class, and the frozen source hashes). The image is a
*reproducibility baseline* for the bootloader's source commit -- it has never
been run on hardware from this repository, and nothing here claims otherwise.

## Host tests

The board-independent core has a host-gcc test suite that needs no hardware:

```bash
sh shell/test/run_host_tests.sh              # core + every board that pins tests
sh shell/test/run_host_tests.sh wio-lite-ai  # core + one board
```

`shell/test/` holds the board-independent tests and always runs them. A test that
compiles board-owned code against the board's real headers belongs to that board
and lives in `boards/<board>/test/host_tests.sh`, which the runner invokes with
the same toolchain flags (Wio Lite AI pins three: CRC-32, BlazeFace, MLPerf Tiny;
f746g-disco pins none).

## Status

All three boards are ported and build from the shared shell core, and the Wio
Lite AI DFU bootloader tree is in and building (reference build only -- see
above). Project rules are in `CLAUDE.md` / `AGENTS.md`.

## License

See [LICENSE](LICENSE).
