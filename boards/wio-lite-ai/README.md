# Wio Lite AI (STM32H725AEI6, Cortex-M7)

A Seeed board with no debugger on it, no reset button worth the name, and a DFU
bootloader in the first flash sector that is the only way back if an app image
is bad.  Everything below follows from that.

> **[!] Read the safety rules before changing anything here.**  They are in
> `CLAUDE.md` ("Wio Lite AI" under the per-board rules) and `AGENTS.md`, not in
> this file: this README explains *how the board works*, those two say *what an
> agent must not do*.  The bootloader's own recovery runbook is
> [`boot/README.md`](boot/README.md).
>
> The short version: **one board exists** (board #2; board #1 is a permanent
> paperweight), the internal flash is good for roughly 10k erase cycles, and
> flash sector 0 holds the bootloader.  Do not reflash in a loop, and do not
> write sector 0.

## Quick start

```bash
cmake -B build/wio-lite-ai -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=wio-lite-ai
cmake --build build/wio-lite-ai
```

To flash the app: **hold PF1 (USER) while resetting** to enter the bootloader's
DFU mode, check that `0483:df11` appears, then

```bash
cmake --build build/wio-lite-ai --target flash   # = dfu-shell
picocom -b 115200 /dev/ttyACM0                   # the app's own USB CDC
```

`flash` is an alias for `dfu-shell`, which is exactly
`dfu-util -d 0483:df11 -a 0 -D shell.bin` -- it writes the **app partition
only** and the board reboots into the new image by itself.  It cannot reach
sector 0; the two boards use the same command name for convenience, not because
this one grew an ST-Link path.

If `dfu-util` reports `LIBUSB_ERROR_BUSY`, nothing was written -- something else
has the device open.  Close it and retry.

## The board at a glance

| | |
|---|---|
| MCU | STM32H725AEI6, Cortex-M7, 550 MHz **inherited from the bootloader** |
| Console | USB CDC (USB1_OTG_HS driven at FS through the internal PHY, TinyUSB) |
| USB ID | app = `0483:5740` (ST generic VCP) -> `/dev/ttyACM0`; bootloader = `0483:df11` |
| LEDs | LED0 (red) = PC13, LED1 (yellow) = PF0 |
| Button | USER = PF1, active low; held across reset it selects DFU |
| App | internal flash `0x08020000`, sectors 1-3, 384 KB |
| Bootloader | internal flash `0x08000000`, sector 0, 128 KB -- **immutable** |
| References | RM0468, PM0253, H72x/H73x errata, board schematic (`_ref/wio-lite-ai/`) |

## Boot, and why the app does so little at startup

The bootloader in sector 0 has already configured the clock tree, the power
supply and the flash wait states by the time the app runs.  The app **inherits
all of it** and reconfigures none of it: 550 MHz core, PLL3Q at 48 MHz for USB,
flash latency 3.

`SystemInit` here is a custom one that does FPU, VTOR and TCM initialisation
only -- ECC enable, zero fill, the `.itcm` load and the MSP fill.  It does not
touch RCC, PWR or FLASH ACR, and the stock CMSIS `SystemInit` /
`SystemClock_Config` are never called.  VTOR comes from the linker's
`g_pfnVectors`, and the SysTick reload is computed from the inherited
`SystemCoreClock` without reading RCC at all.

There are exactly **two** writes into the inherited configuration, both
documented as exceptions in CLAUDE.md:

1. `ltdc_clock_init()` retunes PLL3's R output before USB is clocked -- four
   writes on the success path (`RCC_CR.PLL3ON` clear, `RCC_PLL3DIVR.DIVR3`,
   `RCC_PLLCFGR.DIVR3EN` set, `RCC_CR.PLL3ON` set; RM0468 8.7.1 / 8.7.11 /
   8.7.16).
2. `HAL_PWREx_EnableUSBVoltageDetector()` sets `PWR_CR3.USB33DEN`
   (RM0468 6.8.4).

Peripheral bus clock gates and kernel clock muxes are ordinary app business and
are not covered by the rule.

### The DFU fallback is the safety net

An erased or invalid app always lands in DFU mode -- including a DFU transfer
that was interrupted, because the bootloader writes the first 32 bytes (the
vector table) last.  That "vector-last commit" is what makes a half-written
image indistinguishable from an erased one, and therefore recoverable.  Nothing
on the app side may change the conditions the bootloader tests.

## Memory map

| Region | Address | Size | Reachable by |
|---|---|---|---|
| ITCM | `0x00000000` | 64 KB | CPU only -- ISR code lives here |
| DTCM | `0x20000000` | 128 KB | CPU only -- hot, CPU-private data |
| AXI-SRAM | `0x24000000` | 320 KB | CPU and bus masters |
| Flash (app) | `0x08020000` | 384 KB | sectors 1-3 |
| PSRAM | `0x90000000` | 8 MB | external APS6408 on OCTOSPI1 |

ITCM is 64 KB rather than 192 KB because `TCM_AXI_SHARED` is at its default,
which is also why AXI-SRAM is 320 KB.  That is an option byte, and **option
bytes are never touched here**, so both numbers are fixed.

### [!] DMA cannot see the TCMs, and it fails silently

DMA1, DMA2 and the SDMMC1 IDMA cannot reach ITCM or DTCM (RM0468 2.1.2 / 2.1.5
/ 2.1.6).  A DMA buffer placed in DTCM does not fault -- the transfer simply
does not happen, and the buffer keeps whatever was in it.  That is the whole
reason the placement policy exists:

- **AXI-SRAM**: anything a bus master must see
- **DTCM**: CPU-private hot data (stacks, the ISR ring)
- **ITCM**: interrupt service code

### [!] The linker script's `ASSERT`s do not hold here

This board builds with LTO on by default (`BSP_ENABLE_LTO`), and LTO renames
the sections and symbols those asserts are written against, so they pass
without checking anything.  The real guard is post-link:
`cmake/check_itcm_residency.py` and `cmake/check_dtcm_residency.py` read the
finished ELF.  Keep them, and keep their resident lists current -- they carry a
maintenance obligation that the asserts did not.

`cmake/check_psram_ai_residency.py` does the same job for the cacheable PSRAM
carve-out, and `cmake/check_cxx_runtime.py` bounds what the C++ (TFLM) backend
is allowed to drag in.

Since issue #97 there is one more, and it protects something less obvious.  The
BlazeFace decoder is shared by all three boards (`svc/blazeface.c`), and sharing
it is only possible because it owns NO storage: this board passes in its own
candidate scratch, which is how that buffer stays in `.psram_ai` and how
`check_psram_ai_residency.py` keeps naming a symbol this board owns
(`nn_dec_scratch`, in `port/nn/nn_decoder.c`).  A static added to the shared file
would become state nobody placed and no gate mentions.
`cmake/check_no_mutable_storage.py` refuses that, by compiling the shared file
with this board's real definitions and requiring the object to have no allocated,
writable section.  It measures sections rather than symbols because thread-local
storage is not an `STT_OBJECT` and inline asm can place anonymous writable bytes;
`cmake/fixtures/run_storage_gate_tests.py` demonstrates both, and demonstrates
why the check runs against the cross compiler rather than on the host.

[!] Only the SCRATCH is placed.  The decoder's state -- the threshold -- stays in
ordinary internal RAM, and that is deliberate twice over: `.psram_ai` is NOLOAD,
so an initialised field there would never be loaded (and NOLOAD keeps the
previous run's bytes, so it would fail by appearing to work), and the PSRAM
bring-up is fail-soft, so `nn thresh` has to keep answering on a board whose
external memory did not come up.

## Console

The console is USB CDC on USB1_OTG_HS, driven at full speed through the
internal PHY.  TinyUSB's dwc2 driver is pointed at rhport0 with the OTG_HS base
address, and `OTG_HS_IRQHandler` calls `tud_int_handler(0)`.  The pins are
PA11/PA12 as `GPIO_AF10_OTG1_FS`, and the USB clock is PLL3Q at 48 MHz.

The app enumerates as `0483:5740`, the ST generic VCP, so it appears at
`/dev/ttyACM0` exactly where the bootloader's `0483:df11` was.

## The bootloader tree

`boot/` is an independent tree.  It shares no source with the app or the shell,
and it is built here **only as a reference image** (`boot` target, output under
`boot-reference/`) so that a HAL, TinyUSB, toolchain or `board.cmake` change
cannot break it silently.  There is deliberately no target that can write it.

The gate around it is `cmake/check_boot_safety.py`:

- **precheck**: a sha256 manifest over every bootloader source *and*
  `ldscript/STM32H725AEIx_ROM.ld`, plus an audit of the compile commands
- **POST_BUILD**: vector placement, sector-0 containment, absence of any
  option-byte or DBGMCU path, the call graph of the flash-write API, the DFU
  class, ELF/bin agreement, absence of LTO IR in every object, and a golden
  image hash

Two things about it are worth knowing before touching anything nearby.  The
LTO check reads the objects (`.gnu.lto_*` sections) rather than the command
line, because a specs file or a compiler launcher leaves no trace on the
command line.  And the flash-write API is detected as `movw`/`movt` pairs as
well as data words, because an indirect call leaves no edge in the call graph.

The golden hash is a reproducibility baseline against the donor commit
`09468bb`.  It is not proof about what is on the board.

Negative tests for the gate live in `cmake/fixtures/run_fixture_tests.py`.

## Build options

| Option | Default | Effect |
|---|---|---|
| `CONFIG_NN_BACKEND` | **tflm** | TensorFlow Lite Micro inference; `null` for the stub |
| `BSP_ENABLE_LTO` | ON | link-time optimisation for the shell firmware |
| `BSP_ENABLE_WFI` | ON | ThreadX idle WFI |
| `BSP_ENABLE_IWDG` | ON | watchdog + `wdt` |
| `BSP_ENABLE_PSRAM` | ON | OCTOSPI1 APS6408 window + `psram` |
| `BSP_ENABLE_SD` | **OFF under tflm**, else ON | SDMMC1 + `sd` |
| `BSP_ENABLE_LCD` | ON | FPC-40 RGB panel on the LTDC + `lcd` |
| `BSP_ENABLE_KV` | ON | external NOR on OCTOSPI2 + the config KV store |
| `BSP_ENABLE_CAMERA` | ON | FPC-24 DVP camera on the DCMI + `camera` |
| `CONFIG_MLPERF_TINY` | OFF | MLPerf Tiny v1.4 harness + `mlperf` |

### Why tflm is the default here and nowhere else (issue #98)

This is the only board that can afford it without asking anything of whoever
configures the tree.  Its models arrive at RUNTIME, from the NOR blob region, so
nothing is baked into the image and `cmake -DBOARD=wio-lite-ai` needs no arguments.
f746g-disco's tflm bakes a `.tflite` in and this repo ships none -- defaulting it
would make a clean tree fail to configure -- so that board stays `null`.

[!] **It costs most of what is left of the 384 KB app partition**, and the microSD
is what pays for it.  Measured:

| build | app partition | free |
|---|---|---|
| `null` (+ SD) | 305,584 B, 77.7% | 87,632 B |
| **`tflm`, SD dropped (the default)** | **350,096 B, 89.0%** | **43,120 B** |
| `tflm` + SD | 384,572 B, 97.8% | 8,644 B |

The last row links and leaves no room to add anything, which is why the tflm
default also turns `BSP_ENABLE_SD` off.  `-DBSP_ENABLE_SD=ON` brings it back, but
wanting the SD back is a reason to audit the space first, not to pass a flag.

[!] The switch only affects NEW build directories.  `CONFIG_NN_BACKEND` is a cache
variable, so a tree already configured as `null` stays `null` until it is
reconfigured -- which is also why the SD default, keyed on `NOT DEFINED
BSP_ENABLE_SD`, does not fire in one of those.

[!] **The tflm build has not been run on hardware.**  It builds and passes every
gate in both configurations, and `check_cxx_runtime.py` -- which only runs for
tflm -- is now part of the default build.  But no image from this configuration
has been flashed, so the first `--target flash` after this change ships something
new.

## Commands

```
ai        blob      camera    console   coremark  crash
devmem    dmesg     echo      free      help      jobs
kill      kv        lcd       membench  mlperf    net
nor       psram     reboot    sd        sleep     thread
uptime    usleep    version   watch     wdt       wifi
```

Behind them: the RTL8720DN companion for WiFi and the telnet console (`wifi`,
`net`), the external NOR with a key-value store and a blob region (`kv`,
`blob`, `nor`), the 8 MB PSRAM (`psram`), the ST7789 panel over the LTDC's RGB
interface (`lcd`), a DVP camera (`camera`), and the TFLM inference backend
(`nn`).

This board runs two shell instances -- `wio>` on the USB CDC and `wio-net>` on
telnet -- each with its own line editor, history and RX/TX drop counters.
`console` prints those counters, one line per console; what counts as a console
is in the root
[README](../../README.md#a-board-can-run-more-than-one-console).  `wio-net>`'s
`rx_drop` stays 0 by construction -- only a ring-buffered backend can overflow,
and the TCP one has no ring -- while `tx_drop` applies to both.  The boot-time
KV shell (`src/kv_boot.c`) is an instance too, but it is never started as a
console -- its output goes to `dmesg` -- so it is not listed.

## Flashing and recovery

Normal flow is the DFU one at the top of this file.  If the app is bad, the
bootloader takes over by itself -- that is the design.  If the *bootloader* is
bad, the recovery procedure (ST-Link in UR mode, which programmer, what to back
up first) is in [`boot/README.md`](boot/README.md), and it is not something to
attempt without the review path CLAUDE.md describes.

Debugging over SWD uses the system `gdb-multiarch` with OpenOCD
(`target/stm32h7x.cfg`).  **The SWD pins PA13/PA14, the option bytes, RDP and
DBGMCU are never reconfigured by this firmware.**
