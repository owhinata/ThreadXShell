# ThreadXShell

A multi-board firmware project providing an interactive shell console on top of
Eclipse ThreadX, using the ST HAL and built with CMake + Ninja.

This repository unifies the shell implementations grown separately in
[stm32f746g-disco](https://github.com/owhinata/stm32f746g-disco) and
[wio-lite-ai](https://github.com/owhinata/wio-lite-ai) into a single
board-independent shell core with per-board ports.

## Supported boards

| Board | MCU | Clock | Console | Flashing |
|---|---|---|---|---|
| STM32F746G-DISCO | STM32F746NGH6 (Cortex-M7) | 216 MHz | VCP (USART1, 115200) | ST-Link |
| Wio Lite AI | STM32H725AEI6 (Cortex-M7) | 550 MHz (inherited from DFU bootloader) | USB CDC (TinyUSB) | DFU (`dfu-util`) |

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

cmake -B build/f746g-disco -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=f746g-disco
cmake --build build/f746g-disco        # -> shell.{elf,bin,hex}
```

Configuring without `-DBOARD` fails with the list of available boards. Each board
declares the upstream mirrors it needs in `boards/<board>/submodules.cmake`, and
only those are fetched -- the first `f746g-disco` configure is heavy (GUIX alone
is about 1.5 GB) and none of it is pulled in for a `wio-lite-ai` build.

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
cmake --build build/wio-lite-ai --target dfu-shell
# or: dfu-util -d 0483:df11 -a 0 -D build/wio-lite-ai/shell.bin
```

The app is programmed to the internal flash app partition at `0x08020000`. The
bootloader in sector 0 is never written by this build. Internal flash endurance
is about 10k cycles, so do not reflash in an automated loop.

## Host tests

The board-independent core has a host-gcc test suite that needs no hardware:

```bash
sh shell/test/run_host_tests.sh
```

## Status

Both boards are ported and build from the shared shell core. The Wio Lite AI DFU
bootloader tree is next. Project rules are in `CLAUDE.md` / `AGENTS.md`.

## License

See [LICENSE](LICENSE).
