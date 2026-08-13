# Grove Vision AI V2 (Himax HX6538 WiseEye2)

ThreadX shell port for the Seeed Grove Vision AI V2: Himax HX6538 (WiseEye2),
dual Cortex-M55 (the app runs on the big CM55M core at 400 MHz) + Ethos-U55
NPU, external W25Q128JW 16 MB QSPI NOR flash, console and flashing on UART0
through the onboard CH343P USB-UART bridge (the chip has NO USB device
controller).

## Quick start

```bash
cmake -B build/grove-vision-ai-v2 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=grove-vision-ai-v2
cmake --build build/grove-vision-ai-v2
# close any terminal on the serial port first, then:
cmake --build build/grove-vision-ai-v2 --target flash
# the script prints "Please press reset button!!" -- press the board's RESET
# button if the transfer does not start on its own (opening the port already
# resets the board, see below, so it often does)
picocom -b 921600 /dev/ttyACM0
```

The first configure fetches the Himax SDK (~480 MB) into
`boards/grove-vision-ai-v2/sdk/` -- a pinned, git-ignored, read-only checkout
(see `cmake/himax_sdk.cmake`; `-DGROVE_SDK_DIR=<path>` points at an existing
checkout at the same commit instead).  The serial device defaults to
`/dev/ttyACM0` (`-DGROVE_SERIAL_PORT=` to override); note the CH343P can
enumerate as `/dev/ttyUSB*` depending on the host driver.

## [!] Opening the serial port RESETS the board

This is board wiring, not a bug in the firmware, and it surprises everyone
once: **every time a host opens the serial port, the board reboots.**  Connect
with `picocom` and you will see the Himax bootloader banner scroll past before
the shell prompt, and the shell's uptime starts from zero.

From the board schematic (`_ref/grove-vision-ai-v2/`), the reset path is:

```
CH343P RTS (pin 13) --| |-- RST ---- RESETN (HX6538 pin E5)
                      C16              |
                     220nF             +-- R7 10k --- VCC_3V3
```

`DTR` (pin 12) is **not connected at all**, and neither are DCD/RI/CTS/DSR/ACT
-- RTS is the only modem-control line that goes anywhere.  The coupling is
capacitive, so it is the *transition* that resets: any RTS edge is passed to
RESETN as a pulse (RC = 220 nF x 10 k = 2.2 ms, comfortably longer than a
reset needs).  The board enumerates through Linux's `cdc_acm` driver, which
asserts DTR and RTS when the port is opened, and CH343P's RTS# pin is
active-low -- so an open produces a falling edge, and the falling edge is the
reset.  (Closing produces the opposite edge, which is harmless.)

**Do not try to defeat this globally.**  It is also what makes flashing work:
the bootloader only listens for the xmodem handshake for about 100 ms after
reset, and the flash tool opening the port is what puts the board there.

If the reboot-on-connect is a nuisance for console work, the direction to try
is keeping RTS from ever changing state -- e.g. `stty -F <port> -hupcl` so
that closing the port stops dropping the line, after which the next open
re-asserts an already-asserted line and produces no edge.  **Untested here**;
the settings are also lost when the USB device re-enumerates.

## How this port works

- **Not XIP.**  The flashed `.img` is a full flash image (bootloader + 2nd
  bootloader + memory descriptors + signed app).  At boot the 2nd bootloader
  loads the ELF into ITCM 256 KB @`0x10000000` (vectors+code+rodata) and DTCM
  256 KB @`0x30000000` (data+stacks); the SRAM0 window from `0x3401F000` is
  explicit-placement only and empty in this milestone.
- **Clock inheritance.**  The app never configures PLLs; it reads the CM55M
  frequency back through the SCU driver (measured: 400 MHz) and derives
  SysTick from that value at runtime (`port/threadx/tx_glue.c` sanity-checks
  it before starting the tick, and the banner warns if the read-back ever
  stops matching `CLI_CPU_CYCLES_PER_US`).
- **All-Secure TrustZone.**  SDK "SEC_ONLY" configuration: SAU disabled,
  everything Secure.  ThreadX is built with `TX_SINGLE_MODE_SECURE`
  (`port/threadx/tx_user.h`) -- mandatory, or the first context switch returns
  into a non-existent Non-secure world.
- **Prebuilt drivers.**  Peripheral drivers exist only as
  `prebuilt_libs/gnu/libdriver.a`; they install their ISRs at runtime into the
  writable ITCM vector table.  `main.c` runs the SDK platform init under
  PRIMASK and then disables + clears every external IRQ before
  `tx_kernel_enter()`; the console UART IRQ is re-enabled by the backend's
  `enable()` on the shell thread.
- **Console backend** (`backend/cli_backend_uart.c`): interrupt-driven RX
  (`UART_CMD_SET_RXCB`, NULL buffer = DATA_AVAIL callback) + chunked
  interrupt TX.  The SDK documents this path but ships no example of it; it
  is verified working here.  The documented fallback, should a future SDK
  bump break it, is a 1-byte `uart_read_udma()` re-arm -- which additionally
  needs the DMA3 combined IRQ (69) enabled, not just UART0's (90).
- **SDK diagnostics** (`xprintf`) are satisfied by a board shim that routes
  into the `dmesg` RAM log; the SDK clib/console is not linked at all.
- **Post-build gates** (`cmake/check_*.py`): image coherence (every linker
  section present in the generated `.img`, command registry inside `.rodata`),
  placement/budget (ITCM/DTCM headroom, vector table, static stacks, no
  forbidden SDK symbols surviving), and an MVE-predication scan (the ThreadX
  M55 port does not save VPR).

## Flashing and recovery

Every flash rewrites the WHOLE image, bootloader region included -- that is
the vendor-standard flow, proven many times on this board by the donor build
environment.  The external NOR (W25Q128JW) is rated ~100k program/erase
cycles; do not script flashing in a loop.

If a flash goes wrong (power loss mid-transfer, corrupted image):

1. The HX6538 has a 64 KB in-chip boot ROM and a BOOT_OPT strap (the BOOT
   button on the board).  Hold BOOT while releasing RESET to force the ROM
   loader path, then retry the xmodem upload.
2. A known-good factory image ships in the donor environment
   (`Seeed_SenseCraft_AI_*.img`, from Seeed's SenseCraft release) and can be
   sent with the same `xmodem_send.py` flow to restore the retail firmware.
3. The SWD pads (CMSIS-DAP + Himax's pyocd fork) remain available for
   debugging; they are not needed for unbricking the flash.

The console and the flash channel are the same serial device: close the
terminal before running `--target flash`.

## Future work

- EPK (`thread` cpu%%): needs a wrapper around the vendor-installed UART ISR
  vector before its ISR accounting can be correct; deferred.
- LED / button / GPIO commands, SD (SPI mode), PDM microphone, CoreMark.
- Camera (MIPI CSI) and Ethos-U55 inference.
