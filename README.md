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

The Wio Lite AI DFU bootloader (`boot/`) is an independent tree that shares no
sources with the app/shell layers and is treated as immutable.

## Status

Early stage: project rules are in place (see `CLAUDE.md` / `AGENTS.md`); the
shell core and board ports are being migrated from the source repositories.
Build instructions will be added once the CMake configuration lands.

## License

See [LICENSE](LICENSE).
