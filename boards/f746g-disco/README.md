# STM32F746G-DISCO (STM32F746NGH6, Cortex-M7)

The ST Discovery board this project's shell was originally written on.  It is
the one board here with everything attached: a 4.3in RGB panel on the LTDC, a
camera, Ethernet, QSPI NOR, a microSD slot and a resistive-free capacitive
touch controller -- so it is where a shared-core change is easiest to smoke
test.

Flashing is over the on-board ST-Link, which makes it the cheapest board to
iterate on: there is no erase-cycle budget to worry about and no bootloader to
protect.

## Quick start

```bash
cmake -B build/f746g-disco -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=f746g-disco
cmake --build build/f746g-disco
cmake --build build/f746g-disco --target flash   # ST-Link
picocom -b 115200 /dev/ttyACM0                   # the ST-Link VCP
```

The first configure downloads the pinned ARM GNU toolchain into `tools/` and
the submodules this board needs (`boards/f746g-disco/submodules.cmake`).

## The board at a glance

| | |
|---|---|
| MCU | STM32F746NGH6, Cortex-M7 with FPU, I-cache and D-cache on |
| Clock | 216 MHz: HSE 25 MHz -> PLL M25 N432 P2, VOS1 + over-drive, flash 7 WS |
| Console | ST-Link VCP: USART1, TX=PA9 / RX=PB7, 115200 8N1 -> `/dev/ttyACM0` |
| Second console | telnet over the on-board Ethernet (`net` + NetX Duo) |
| LED | LD1 (green) = PI1 |
| Flashing | ST-Link, `--target flash` |
| References | RM0385, UM1907, the ST demo sources under `_ref/f746g-disco/` |

### [!] The FPU is single precision only

`-mfpu=fpv5-sp-d16`.  Doubles are software, through `__aeabi_d*`, and printing
one needs the full float formatter -- which is why the link line carries
`-u _printf_float`.  CoreMark's score line prints a `%f`, so dropping that
option does not fail the link, it fails at runtime with an empty field.

## Memory map

| Region | Address | Size | Notes |
|---|---|---|---|
| Flash | `0x08000000` | 1 MB | whole device, no bootloader |
| ITCM | `0x00000000` | 16 KB | present, **not used by the linker script** |
| DTCM | `0x20000000` | 64 KB | |
| SRAM | `0x20010000` | 256 KB | SRAM1 240 KB + SRAM2 16 KB, contiguous |
| SDRAM | `0xC0000000` | 8 MB | FMC bank1, 16-bit bus |

ITCM placement was tried and dropped: with I-cache and D-cache enabled it was
worth about 0.6%, because the cache already hides the flash wait states.  The
region is left in the map as a fact about the part, not as a placement target.

### [!] The SDRAM's internal banks are assigned by use, and the linker enforces it

The 8 MB device has four 2 MB internal banks, and each one has a single owner:

| Bank | Address | Owner |
|---|---|---|
| 0 | `0xC0000000` | LTDC scan-out surface + the other fixed residents |
| 1 | `0xC0200000` | camera DMA arena, exactly 2 MB |
| 2 | `0xC0400000` | Ethernet descriptors and pool |
| 3 | `0xC0600000` | NN arena; the upper half at `0xC0700000` is the execution window for a relocatable model |

This is not tidiness.  Bank 0 is read continuously by the LTDC, bank 1 is
written continuously by the DCMI, and an FMC bank change costs a row activation
-- putting two continuous masters in one bank shows up as display tearing and
DMA FIFO errors.  Banks 0..2 are non-cacheable (bus masters write them); bank 3
is CPU-only and therefore cacheable, and its upper half is the only window
`bsp.c` makes instruction-fetchable.

The linker script asserts each boundary (`.sdram.cam` must start exactly at
bank1 and be exactly 2 MB, `.sdram.eth` must fit inside bank2, and so on), so a
placement mistake is a link error rather than a rendering artifact.

## Time

**The timebase is TIM2 at 108 MHz**, not the core clock.  APB1 runs at /4 with
TIMPRE=0, so the timer clock is 2x PCLK1 = 108 MHz (RM0385).  The execution
profile kit and `udelay` share that source, which is why the board sets
**`CLI_CPU_CYCLES_PER_US=108`** and not 216.  Getting this wrong does not fail
to build; it makes every measured microsecond off by 2x.

## Consoles

Two shell instances can be live at once: the ST-Link VCP and a telnet session
over Ethernet.  They are separate `cli_instance`s on the shared core, each with
its own line editor, history and output lock.

### [!] PA9 is shared with OTG_FS_VBUS

The VCP's TX pin is also the USB FS VBUS sense pin.  The default solder-bridge
configuration gives it to the VCP (UM1907), which is what this firmware
assumes.  If USB FS is ever brought up on this board, that bridge -- not the
firmware -- is the thing to change first.

### [!] `CLI_INSTANCE_TIME_SLICE` stays 0

Both console instances run at the same ThreadX priority, and the setting maps
to `TX_NO_TIME_SLICE`.  Round-robin between them would be wrong here:
`coremark`, `membench` and `nn run` share static state and the DWT cycle
counter, and are not re-entrant across instances (#4).  **A CPU-bound command
making the other console unresponsive is the expected behaviour**, not a bug to
fix by adding time slicing.

## Build options

The board's options live in `boards/f746g-disco/board.cmake`.  The ones worth
knowing:

| Option | Default | Effect |
|---|---|---|
| `BSP_ENABLE_IWDG` | ON | independent watchdog + the `wdt` command |
| `CLI_ENABLE_DANGEROUS_CMDS` | ON | builds `reboot` and `devmem` |
| `CLI_DEVMEM_DUMP_MAX_LEN` | 256 | bytes per `devmem` dump |
| `CONFIG_NN_BACKEND` | `null` | `null` / `stedgeai` / `stedgeai_reloc` / `tflm` |

### [!] LTO is refused on this board

`board.cmake` turns an attempt to enable it into a `FATAL_ERROR`, including the
per-configuration variants.  The reason is that the linker script's `ASSERT`s
are the placement invariant, and they are written against symbol and input
section names -- which LTO renames.  The asserts would still pass, against
different sections, and say nothing.

Two gates back that up:

- the `ASSERT`s in `ldscript/STM32F746NGHx_FLASH.ld` (bank boundaries, sizes)
- `cmake/check_f746_layout.py`, POST_BUILD, which checks the real image for
  symbol residency, the vector table and the float runtime

## Commands

```
ai        camera    console   coremark  crash     devmem
dmesg     echo      free      fs        gui       help
jobs      kill      lcd       membench  net       qspi
reboot    sd        sdram     sleep     thread    touch
uptime    usleep    version   watch     wdt       xfer
```

`help` lists them with one-line descriptions; `help <cmd>` and
`help <cmd> <sub>` walk the tree.

Subsystems behind them: LTDC + GUIX (`gui`, `lcd`), OV5640 over DCMI
(`camera`), QSPI NOR with LevelX + FileX (`fs`, `qspi`), microSD (`sd`),
FT5336 touch (`touch`), Ethernet with NetX Duo (`net`), the NN backends (`ai`),
and YMODEM transfer over the console (`xfer`).

This board runs two shell instances -- `sh>` on the VCP and `net>` on telnet --
each with its own line editor, history and RX/TX drop counters, so `console`
prints two lines here:

```
sh> console
console         rx_drop    tx_drop
sh>                   0          0
net>                  0          0
```

The `net>` row appears whether or not a telnet client is attached.  Its `rx_drop`
stays 0 by construction -- only a ring-buffered backend can overflow, and the TCP
one has no ring -- while `tx_drop` applies to both, because the no-progress
deadline that drops output lives in the shared output path.  What counts as a
console, and what the two columns mean, is in the root
[README](../../README.md#a-board-can-run-more-than-one-console).

### [!] Three subscribers share one capture, and each has to drain its sink

The GUI preview, `ai stream` and `net mjpeg` are all *subscribers* of one base
capture.  Stopping one of them detaches its sink while the base keeps running --
that is the whole point of a subscriber -- so a delivery can already be in
flight across the unlink: `frame_pipeline_publish()` copies the sinks it will
deliver to into a local array, drops the pipeline lock, and only then calls
`consume()` on each.

Two rules follow, and neither is optional (issue #72):

- **`camera_frame_put()` is the last statement of every `consume()` on this
  board.**  That is what makes one number -- the sink's pipeline pin count --
  answer "may I release what this sink reads".  It does not prove the callback
  RETURNED (there is still its epilogue, and the pipeline updates sink
  statistics afterwards); it proves the callback no longer touches anything the
  owner owns, which is enough only because every sink object here is static.
  Work moved back below the put becomes invisible to the count.
- **The owner enters its `DRAINING` state before `camera_unsubscribe()`, not
  after.**  A start walking into the drain would re-subscribe the sink, and
  `frame_pipeline_attach()` resets the pin count -- erasing the evidence the
  drain is waiting on.  The states are `port/camera/cam_own.h`; the drain
  decision is `port/camera/cam_drain.h`; both are pure functions with host tests
  because the branches that matter (a drain that spends its budget, two owner
  commands in flight at once) cannot be produced from the console.

When a drain does spend its budget, the owner refuses to release and says so:

| command | what you see | what it means |
|---|---|---|
| `gui stop` | `preview did not release the camera frame; display kept` | GUIX keeps the LCD and the preview stays armed.  Run `gui stop` again. |
| `ai stream stop` | `camera has not released the inference frame` | `ai stream start` is refused until a later `ai stream stop` finds it clear. |
| `net mjpeg stop` | `camera has not released the mjpeg frame` | likewise for `net mjpeg start`. |

Retrying is the recovery, and it is fail-closed in both directions: if the
callback never comes back, every retry keeps refusing; if it does, a retry can
prove it.  A `busy` message instead means another start/stop for that subsystem
is running right now and nothing was touched.

One behaviour changed with this: `gui start` while the UI is already up is now a
no-op instead of snapping the panel back to the preview screen.  The autostart it
used to re-post carries no claim on the preview lifecycle, so a slow one could
land after a `gui stop` had already drained and released the sink.  Use the
settings screen's **Back** button to return to the preview.

## Debugging

SWD is available through the same ST-Link.  Use the system `gdb-multiarch` --
the toolchain's own gdb is unusable here, it wants `libncursesw.so.5`.

```bash
openocd -f interface/stlink.cfg -f target/stm32f7x.cfg     # :3333
gdb-multiarch build/f746g-disco/shell.elf -ex 'target extended-remote :3333'
```

`st-util` on :4242 works too.  Note that a console program holding
`/dev/ttyACM0` and an `st-flash` read will fight over the device -- SWD and the
console are separate paths, but the VCP is not.

## Notes for changes that touch the shared core

This board and the Wio Lite AI both build `shell/` and `svc/`, so a change
there has to build for every board before it is committed (CLAUDE.md).  This is
the cheapest board to check a runtime effect on, because reflashing costs
nothing.
