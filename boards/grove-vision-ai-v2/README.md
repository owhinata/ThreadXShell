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
  placement/budget (ITCM/DTCM headroom, vector table, static stacks,
  benchmark-buffer residency, no forbidden SDK symbols surviving), and an
  MVE-predication scan (the ThreadX M55 port does not save VPR).

## Time sources

Three different counters, because the three jobs have different requirements.
Getting these mixed up is the easiest way to produce numbers that look fine and
are wrong, so the split is spelled out here:

| Used by | Counter | Why that one |
|---|---|---|
| `thread` cpu% (ThreadX Execution Profile Kit) | **Himax TIMER2**, free-running | Must keep counting while the core is asleep -- with WFI the idle time would otherwise vanish and every busy thread's share would inflate toward 100% |
| `udelay()` (`sleep`/`usleep`) and `membench` | **DWT CYCCNT** | Cycle resolution, and both busy-wait in the foreground, so a counter that stops in WFI is not a problem |
| `coremark` | **`tx_time_get()`** (1 ms ThreadX tick) | Only needs seconds; leaves the other two counters undisturbed |

**TIMER2 is owned outright by `port/threadx/tx_glue.c`** and by nothing else.
It is programmed over MMIO -- clock gate, divider and CPU ownership through the
SCU, then RELOAD/VALUE/CTRL directly -- as a free-running counter with RELOAD
all-ones and its interrupt disabled.  Any vendor timer call would overwrite
RELOAD from a period in milliseconds and enable the timer interrupt, so the
placement gate bars **the entire `hx_drv_timer_*` API** from the image, with a
single exception for `hx_drv_timer_init` (which the SDK's platform init calls
for all nine timers and which only records a base address).  Barring the whole
prefix rather than the `hx_drv_timer_cm55m_*` wrappers is the point: a name
list would leave `hx_drv_timer_hw_start(TIMER_ID_2, ...)` open, and no
name-based check can tell which timer id an argument carries.

RELOAD all-ones is what makes the kit's time source work: the kit wants an
increasing counter, TIMER2 counts down, and the bitwise complement of a
down-counter is only a clean mod-2^32 up-counter when RELOAD covers the whole
range.  Boot runs a self-test with a small RELOAD first (the reload-at-zero
behaviour is inferred from CMSDK; the SVD does not specify it), measuring
progress *modulo* RELOAD+1 -- deliberately not as "the complement increases
smoothly", which is false for any small RELOAD.

The absolute TIMER2 rate is `SCU SB_APB1 clock / divider`, both read back at
boot.  `epk` prints them.

## cpu% and the vendor ISR wrapper

The UART0 interrupt handler lives inside the prebuilt `libdriver.a`: the driver
installs it into the vector table at `uart_open()` time and there is no source
to add the profile kit's enter/exit hooks to.  Without those hooks, every
microsecond spent servicing the console is billed to whichever thread happened
to be running.

Because the vector table is ordinary RAM in ITCM, `backend/cli_backend_uart.c`
reads the vendor entry back, installs a wrapper that brackets a call to it, and
verifies the swap landed -- all with the UART0 IRQ shut at the NVIC.

**If any of that fails the console still works.**  Every failure path restores
the vendor vector; what is lost is the cpu% accuracy, and that loss is
reported, not hidden: `thread` prints `--` in the cpu% column and a one-line
reason, and `epk` prints the same reason.

**The verdict is re-derived on every query, not latched at boot.**  "The
accounting was correct when it was armed" is a different claim from "it is
correct now", and the gap between them is where a plausible wrong number would
come from.  So `tx_glue_profile_ok()` re-reads, each time `thread` or `epk`
asks:

- TIMER2's CTRL and RELOAD are still the production values, and the counter is
  still counting down;
- the wrapped vector still holds our wrapper (a vendor call that reinstalled
  its own handler downgrades to `--`);
- **every interrupt enabled at the NVIC is one of the REGISTERED ones, and
  every registered one still carries the wrapper we installed** -- an unwrapped
  ISR does not corrupt anything, it just bills its own runtime to whichever
  thread it interrupted, which is exactly the misattribution this machinery
  exists to prevent;
- the profile kit's ISR nesting counter is zero when read from a thread, i.e.
  every enter had its exit.

### The accounted-interrupt registry

UART0 was the only interrupt in M-G2, so the check was "exactly this one line
is enabled".  From issue #30 the registry holds a **set**
(`tx_glue_profile_register_irq()`), because the LCD -- and the camera after it
-- add lines whose numbers are not known until the prebuilt driver has been
brought up.

There is deliberately **no "enabled but unwrapped" category.**  A line the
firmware enables is either wrapped and registered, or it is left disabled and
its status polled instead.  Anything else re-opens the fail-open that #25
closed: the interrupt bills its runtime to an innocent thread while
`tx_glue_profile_ok()` still answers 1.

For a peripheral whose interrupt set is a property of the vendor binary rather
than of any header, that set is **measured, not guessed**
(`port/sdk_seam/epk_irq_wrap.c`): snapshot `NVIC->ISER`, run the vendor
bring-up with interrupts masked, then wrap and register every line that
appeared.  A line that cannot be wrapped or registered stays disabled and the
whole bring-up is abandoned -- losing the peripheral is the cheap failure.

## The vendor timer API seam

The prebuilt camera archives (`libsensordp.a`, `libextdevice.a`, and
`libdriver.a`'s own PWM object) call four vendor timer entry points:
`hx_drv_timer_hw_start`, `hx_drv_timer_hw_stop`, and
`hx_drv_timer_cm55x_delay_ms`/`_us`.  This port bars the whole `hx_drv_timer_*`
prefix from the image -- TIMER2 is the profile-kit time source and no
name-based check can tell which timer id a generic call carries -- so linking
those archives unchanged would fail the build.

**This is a gate conflict, not a hardware conflict.**  Disassembly of
`sensor_dp_lib.o` shows all 41 `hw_start`/`hw_stop` call sites passing a
constant `TIMER_ID_0`, and the delay entry points resolve (via the SDK's
`interface/timer_interface.c`, under `CM55_BIG` + non-`TRUSTZONE_NS`) to the
`TIMER_ID_3` wrappers.  Nothing asks for TIMER2.

The fix is a board-owned seam rather than a weaker gate: `board.cmake` passes
`-Wl,--wrap` for the four symbols and `port/sdk_seam/timer_seam.c` implements
them, **never calling `__real_*`**.  After the wrap, no `hx_drv_timer_*` symbol
except the one permitted `hx_drv_timer_init` reaches the ELF, so the placement
gate and the `AGENTS.md` invariant stay exactly as they were.  (An
argument-inspecting gate was considered and rejected: `AGENTS.md` makes both the
blanket ban and "do not weaken the gate" invariants, and general argument
analysis would be a brittle whole-program binary pass -- tail calls,
address-taken relocations, function pointers, linker veneers.)

The seam's own rules:

- `hw_start()` is **thread context only** (it talks to the SCU and installs a
  vector), `hw_stop()` is **ISR-safe** -- the vendor reaches it from its own
  Timer0 callback, so it does MMIO and NVIC only: no logging, no mutex, no
  ThreadX call, no fail-stop loop.
- Any id other than `TIMER_ID_0`, and any configuration the seam does not
  reproduce exactly, is **refused without writing a single register**.  The
  reason is latched (a string literal) and `epk` prints it.
- A `hw_start()` whose interrupt cannot be registered with the accounting
  registry is refused outright rather than started with an unaccounted line.
- The delays spend DWT cycles (`udelay()`), not a Himax timer.

Three independent things hold this down:

| what | where |
|---|---|
| no vendor timer code survives the wrap | `cmake/check_timer_seam.py`, run on the `seam_probe` link (the firmware objects **plus** the camera archives) |
| the gate actually fails when it should | `cmake/fixtures/run_fixture_tests.py` -- F1 removes one `--wrap`, F2 leaves the archives out, F3 proves the pristine link passes |
| the refusal path writes nothing | `test/test_timer_seam.c`, a host test that compiles the real `timer_seam.c` against the real SDK headers with the register block pointed at an array |

```sh
python3 boards/grove-vision-ai-v2/cmake/fixtures/run_fixture_tests.py \
    --build-dir build/grove-vision-ai-v2 --board-dir boards/grove-vision-ai-v2
sh shell/test/run_host_tests.sh grove-vision-ai-v2
```

`seam_probe` is a build target, not an artifact -- nothing flashes it.  It
exists because M-G3a does not link the camera archives, so every wrapper is
garbage-collected out of `shell` and the claim would otherwise go unchecked
until M-G3b.

### [!] Carried forward to M-G3b: the Timer0 interrupt path is unverified

`hw_start()` proves the counter RUNS -- four windows timed by `udelay()` (DWT
cycles, independent of the timer under test), each required to show motion, and
the amount checked against the rate Timer0's own clock predicts wherever a
window cannot contain a whole cycle.  That closes "armed but stationary" and
"armed but running at some other rate".

It does **not** prove the interrupt gets delivered.  A Timer0 whose counter is
fine while `IRQEN` produces no `INTSTATUS`, or whose NVIC path is dead, would
still be accepted, and the datapath's 500 ms frame watchdog would then never
fire a callback -- armed, believed, and useless.  The registry cannot catch it
either, since registered-but-disabled is a state it deliberately permits.

Nothing in M-G3a executes this code (the camera archives are not linked, so the
whole seam is garbage-collected out of `shell`), which is exactly why it is left
here rather than guessed at: the probe wants designing against real behaviour.
**When M-G3b links the archives, verify before relying on the watchdog:** arm a
short reload, confirm `INTSTATUS` asserts on its own, restore the requested
reload, read `NVIC->ISER` back after `NVIC_EnableIRQ`, and ideally confirm the
seam's ISR is reached once before the vendor callback is exposed.  There is no
public TRM and the vendor's `CTRL` bit 2 is still unexplained (see
`timer_seam.c`), so this is not a hazard to reason away.

## WFI

Idle is `DSB; WFI; ISB` inside PendSV (`TX_ENABLE_WFI`).  Build with
`-DBSP_ENABLE_WFI=OFF` for a busy-idle variant, which is easier to attach to
over SWD -- a sleeping core needs connect-under-reset.

`TX_ENABLE_WFI` is compiled into the port's idle loop, so there is no runtime
path back to spinning.  Its two preconditions are therefore **enforced** before
the scheduler starts rather than merely checked: `tx_glue.c` clears
`SCB->SCR.SLEEPDEEP` and `.SLEEPONEXIT`, reads them back, and halts if they did
not take (the DTCM log ring survives a reset, so `dmesg` shows why after the
next boot).  With both clear, WFI gates only the CPU clock, which is what keeps
SysTick ticking and TIMER2 counting through idle.  The Himax PM library is
linked but unreferenced, and the placement gate fails the build if any
`hx_lib_pm_*` symbol reaches the image.

**Verifying that TIMER2 really survives WFI** is what `epk sleep <ms>` is for:
it samples the raw TIMER2 count either side of a `tx_thread_sleep()` and
compares the delta against what the SCU reference and divider predict.
Watching the `(idle)` row in `thread` is *not* a test of this -- a counter that
advanced only during ISRs would still produce a non-zero window.  Run it on a
WFI build and on a `-DBSP_ENABLE_WFI=OFF` build; the deltas should match each
other and the prediction.

## Benchmarks

```
coremark            EEMBC CoreMark, ~10-100 s, not interruptible
membench [region]   bandwidth + pointer-chase latency (itcm|dtcm|sram|all)
epk [sleep [ms]]    profile-kit time source status / WFI sleep measurement
```

Both benchmarks refuse to run unless the ThreadX tick is alive and the SCU
CM55M frequency agrees with `SystemCoreClock` (`cmds/bench_gate.c`); both
re-read the frequency after the run and warn if it moved, since a CoreMark run
is 10-100 s of not looking.  The SDK's own `platform_driver_init()` drops the
return value of the SCU frequency read, so a failed read is indistinguishable
from a good one there; the gate does the read itself and checks it.

**What the gate cannot do is confirm that the SCU's value is TRUE.**  Both
selectors resolve to the same `hsc_clk` read in this SDK, so they can agree on
the same wrong number; a real check needs a second time reference on its own
oscillator, and this board has none brought up.  Calibrating DWT against the
ThreadX tick would be circular -- the SysTick reload comes from the same value.
So treat every absolute figure here as **measured under a stated clock**, not
as verified: the commands print the frequency next to the numbers for exactly
that reason.

**A CoreMark score is not comparable on its own.**  Quote it with: `MEM_STATIC`
working set, code and data both in TCM (this app is not XIP), `-O3
-funroll-loops -fno-tree-vectorize`, and the core clock the run printed.  The
`-fno-tree-vectorize` is not tuning -- `-mcpu=cortex-m55` enables MVE and the
auto-vectoriser emits predicated MVE, which the ThreadX M55 port cannot carry
across a context switch (it does not save VPR).  The published score is a
**scalar** score.

`membench` measures ITCM (4 KB, read only -- it is the memory all the code
executes from, so the write/copy legs and the chase construction are not run
next to live code), DTCM (4 KB) and SRAM0 (4 KB and 64 KB).  Its buffers live
in NOLOAD linker sections, so nothing in the startup ever initialises them --
the command writes every buffer end to end before the first read, which also
avoids a first-ever read of ECC-protected TCM.

**Read the latency column for memory behaviour; treat the bandwidth column as a
floor set by the access loop.**  They do not measure the same thing, and only
one of them can see a cache miss:

- The latency walk is a *dependent* chain (`idx = buf[idx]`) in pseudo-random
  order, so load N+1's address is unknown until load N returns.  Nothing
  overlaps and nothing prefetches -- a rising stride would measure the
  prefetcher, which is why the order is scrambled.  Every miss costs its full
  refill, which is why the SRAM column steps at 16 KB (measured: 15.0 -> 19.8
  -> 25.8 ns, an L1 D-cache working-set cliff) while DTCM stays flat.
- The bandwidth scan has no dependencies, so misses are amortised over whole
  line refills, overlapped with each other, and fed by the prefetcher
  (`PFCR`).  A 64 KB set therefore reads at essentially the same rate as a
  4 KB one even though the latency column says it is missing.
- More fundamentally, ~715 MB/s at 400 MHz is 1.79 bytes/cycle, i.e. about 2.2
  cycles per 4-byte `volatile` load -- **that is the scalar loop's issue rate,
  not any of these memories.**  `volatile` is what stops the compiler deleting
  the loop, and it is also what caps it.  All four bandwidth rows land in the
  same band for that reason; the memories only have to keep up.  (ITCM reads
  come out ~10% lower because they contend with instruction fetch on the same
  TCM port, and `copy` is lower still because that loop, unlike read and write,
  is not 8-way unrolled.)

**Read the register dump `membench` prints above its numbers.**  This app never
programs the MPU or the caches -- both are inherited from the bootloader -- and
with no TRM the firmware cannot honestly claim what the SRAM rows' memory
attributes are.  So it dumps `SCB->CCR`, `MEMSYSCTL` (`MSCR` including FORCEWT
/ DCACTIVE / ICACTIVE / ECCEN, `PFCR`, `ITCMCR`, `DTCMCR`), the MPU type,
control, MAIR pair and every enabled region, and lets you decide.  The SRAM
rows are labelled "as the CPU sees it", never "cached".

## SPI LCD (Waveshare 2inch, ST7789VW)

`lcd` drives a 240x320 RGB565 panel on the 2x7 XIAO footprint on the board's
underside (issue #30, M-G3a).

```
lcd info               state, read-back SCLK, orientation, ideal frame time
lcd init               bring the panel up (the other subcommands do it for you)
lcd bar                colour bars -- the wiring AND byte-order test
lcd orient             origin/axis probe -- the orientation test (issue #31)
lcd rot [0|90|180|270] read or set the rotation via MADCTL
lcd madctl [byte]      read or set MADCTL raw, e.g. `lcd madctl 0x60`
lcd fill <colour>      one flat colour: a name (`red`, `cyan`, ...) or 0xF800
lcd clear              fill black
lcd loop [n]           repeated full-frame DMA writes, Ctrl+C to stop
lcd regs               the SSPI controller's registers, raw
lcd on | lcd off       the backlight (PA2)
```

`lcd` on its own is not the report -- it is a subcommand set (issue #33), like
`lcd` on the other two boards and like `devmem` here, so `lcd` alone answers
"missing or unknown subcommand" and the report is `lcd info`.  The point of the
form is that the subcommands are DATA: `lcd <TAB>` completes against them and
`help lcd` lists them, neither of which a chain of `strcmp`s inside one handler
can offer.

**`lcd on`/`lcd off` are the backlight and nothing else.**  The spelling is the
one the other two boards use, deliberately -- one vocabulary across the three
consoles -- but there `off` also stops the LTDC scanout, and this panel has no
scanout to stop: it is push-driven over SPI, so `off` drives PA2 low and leaves
the controller running with the last frame still in its GRAM.  The help text and
the command's own output name PA2 for that reason.

### [!] Wiring: count pads, do not read the silkscreen

**Wire by PAD NUMBER.**  The silkscreen on this footprint is the generic XIAO
pin-position labelling and does NOT name the HX6538 signal on the pad.  Two
traps, both of which have already cost a debugging session:

- The pads marked `CLK` / `MISO` / `MOSI` are **PB4 / PB3 / PB2 -- the microSD
  bus**, not a free SPI port.
- **`TXD` is pad 7 (PB6) and `RXD` is pad 8 (PB7)** -- the labels follow the
  XIAO convention (pin 7 = TX, pin 8 = RX) while this chip's own function names
  run the other way (PB6 is UART1_RX, PB7 is UART1_TX).  Wiring DIN to the pad
  that *says* `TXD` puts it on PB6, i.e. on DC, and swaps the two.  The failure
  is silent and total: the SPI controller clocks out every byte correctly, every
  call returns success, and the panel sits at its power-on sleep-in/display-off
  state with the backlight lit.

Pad numbers below are from the PCB netlist in
`_ref/grove-vision-ai-v2/Grove_Vision_AI_Module_V2_Layout`:

| LCD | pad | HX6538 | pinmux |
|---|---|---|---|
| RST | 1 | PA0 | 2 = `AON_GPIO0` |
| CS | 2 | PB11 | 2 = `GPIO2` (driven by hand) |
| CLK | 3 | PB8 | 8 = `SPI_M_CLK` |
| BL | 6 | PA2 | 2 = `SB_GPIO0` |
| DC | 7 | PB6 | 1 = `GPIO0` |
| DIN (MOSI) | 8 | PB7 | 8 = `SPI_M_DO` |
| VCC | 12 | - | |
| GND | 13 | - | |

Pads 4, 5, 9, 10, 11, 14 are not used by the panel: 4 is the **board's own
RESETN** (not the panel's), 5 is PA3, 9/10/11 are the microSD bus, and 14 is USB
VBUS -- not a supply for a 3.3 V panel.

**Finding pad 6 without counting:** `lcd off` / `lcd on`, which drive the
backlight.  PA2 is the backlight pin, so the panel responds only if that wire is
on the right pad -- which also confirms the numbering for every other wire.  It
is the one signal whose connectivity can be checked with no instrument at all,
and it is worth doing FIRST: a lit backlight alone proves nothing, because PA2
carries a 2.2k pull-up to 3V3 and the panel lights up from that whether or not
the wire exists.
- **The header has exactly one 3V3 pad and one GND pad.**  That is why `BL` is
  a GPIO rather than a second wire into 3V3: PA2 carries a 2.2k pull-up to 3V3
  (`R1`), so the backlight is on from power-up, and driving it low sinks about
  4 mA (2.2k on the 3V3 side plus the Grove connector's 2.2k to 5 V through
  `Q4`'s body diode).
- **[!] `lcd off` (backlight low) holds the Grove connector's I2C SCL low**,
  through that same level shifter.  A Grove I2C device and a dimmed backlight
  are mutually exclusive.
- **microSD and the LCD share the SSPI master** and cannot be used at once.
- **[!] While the LCD is enabled, SWD is not available on PB7/PB8** (their
  reset-default mux is SWCLK/SWDIO, and PB6 can be SRSTN).  Flashing and
  recovery are unaffected -- the pinmux returns to its defaults at reset, so
  attach-under-reset still works -- but the alternative SWD mux on PA2/PA3 is
  also gone once PA2 drives the backlight.

### Why CS is a GPIO

The ST7789 ends a memory write when CS goes high.  Nothing documents whether
this controller holds its hardware `SPI_M_CS` low across a DMA descriptor
chain, and the failure mode if it does not is a torn frame every time.  Driving
PB11 by hand makes the question irrelevant: CS goes low before `RAMWR` and high
after the last pixel.

At reset PB11 has no pull-up and PB pins come up low, i.e. CS reads *asserted*
before software runs.  That window is harmless only because the panel is held
in reset (PA0 is also low) and no clock is toggling; `lcd_pins_init()` is what
ends it, driving CS high before the reset pulse is released.

### One DMA burst per frame

A full frame is 153,600 bytes and the SSPI master's plain `spi_write_dma()`
tops out at 4095 -- 38 chunks if the chaining were done in this port.  It is
not: `spi_write_ptl()` lands on the DMA controller's
`dmac_peritransfer_prerolling()`, a **circular linked list walked by the
hardware** and refilled by the vendor's own `dw_spi_s_tx_ptl_isr`, with a
documented ceiling of 256*4095 bytes.  One call per frame, one completion, no
chunk-boundary state machine.  The vendor entry point also cleans the D-cache
over the buffer, so the framebuffer needs no maintenance here.

The framebuffer lives in **SRAM** (`.lcd_fb`, NOLOAD, 32-byte aligned): ITCM and
DTCM are CPU-private on this part, so a framebuffer placed there would not
fault -- the panel would just stay blank while every call reported success.
`check_placement_budget.py` pins symbol -> size -> section -> SRAM.

### Orientation: measure MADCTL, do not inherit the Wio's answer

The panel is 240x320 portrait and everything worth putting on it is landscape --
the camera (M-G3b) most of all.  Issue #31 asks whether the controller will
transpose for free.

**[!] The Wio port's "MADCTL does not rotate" finding does NOT carry over.**  That
was measured on the **RGB parallel interface** (`boards/wio-lite-ai/port/ltdc/
st7789_rgb.c`), where the scan is driven by the controller's own timing and has
nothing to do with the order pixels were written in.  This panel is **4-wire
SPI**, where the `RAMWR` write order *is* the address counter -- exactly what
MADCTL steers.  Different question, different answer expected, so it is measured
here rather than assumed either way.

`lcd rot` sets one of four bytes, `lcd madctl` sets any of the eight MY/MX/MV
combinations:

| rot | MADCTL | geometry | measured |
|---|---|---|---|
| 0 | `0x00` | 240x320 portrait (native, the default) | OK |
| 90 | `0x60` | 320x240 landscape (MX \| MV) | OK |
| 180 | `0xC0` | 240x320 portrait (MY \| MX) | OK |
| 270 | `0xA0` | 320x240 landscape (MY \| MV) | OK |

`lcd madctl` takes a **raw byte** on purpose: the retries are the whole cost of
this measurement, and compiling the value in would spend one flash cycle per
trial on an external NOR rated ~100k.

**MV is the only bit that changes the geometry.**  Set, the address counter is
transposed, so the window commands span 0..319 horizontally and 0..239
vertically and the framebuffer is walked 320 pixels to the row; MX/MY only
mirror within whichever shape MV picked.  The pixel *count* is identical either
way, so the same 153,600-byte buffer serves both and the placement gate never
moves.  `lcd_madctl_apply()` is the only writer of `lcd_w`/`lcd_h`, and it
updates them **after** the write succeeds -- a failed MADCTL that still moved
the geometry would leave the driver addressing a 320-wide window on a controller
still in 240-wide mode, with every layer reporting success.

#### Reading `lcd orient`

**The colour bars cannot answer this.**  Eight vertical stripes look the same
transposed as they do mirrored, and they say nothing about where (0,0) landed --
a MADCTL that half worked would read as success.  `lcd orient` is asymmetric in
every axis instead:

- a **white 32x32 square** marks the origin, pixel (0,0)
- a **red bar** runs from it along **+X** (row 0, full width)
- a **green bar** runs from it along **+Y** (column 0, full height)
- a **blue 24x24 square** marks the far corner, (w-1, h-1)

All eight combinations produce a visibly different picture, and the long side of
the image says directly whether the controller accepted a landscape window.

#### Measured: MADCTL rotates this panel

**All four rotations work.**  With MADCTL `0x60` the probe comes back **landscape
with the white square at the top left** -- a true 90-degree rotation, not a
mirror -- so the controller accepts a 320-wide window and transposes the address
counter itself.  `0xC0` and `0xA0` were checked the same way and are correct too.

That settles the question issue #31 asked: **landscape costs nothing here.**  The
CPU-side fallback (`svc/gfx_rot`, written for the Wio) is **not needed** on this
board; it would have added a 150 KB read plus a 150 KB write per frame, which no
frame time here can absorb.  The Wio's RGB-parallel result really was specific to
that interface.

The default stays **rotation 0**.  Which orientation becomes the final one is
deliberately not settled here -- it belongs with the M-G3b resolution spike,
since the camera's own 90-degree transpose may still make portrait the cheaper
target.  What is settled is that the choice is free either way.

**[!] Re-seat the wires before believing a negative result.**  This measurement
was first recorded as "MADCTL does nothing" with a jumper off the header.  A
rotation that appears inert looks like a panel property and is indistinguishable
from one at the console -- `lcd rot 180` is the cheap discriminator, because it
changes no geometry (240x320 either way, so no window can fall out of range) and
therefore isolates "does MADCTL reach the controller at all" from "is MV
specifically ignored".

### [!] A DMA completion is not a transfer completion

The completion callback fires when the last byte reaches the SPI **TX FIFO**,
not when it leaves the pin.  For a one-byte command that is effectively
immediate, so raising DC to "data" straight after the callback puts the command
byte on the wire *as data*: the panel then treats every command as data, nothing
is ever configured, and it sits in its power-on sleep-in/display-off state --
while the pixel DMA keeps reporting success and the timing keeps looking right.
The same hazard releases CS mid-pixel at the end of a frame.

`lcd_spi_wait_idle()` therefore polls `SR.BUSY == 0 && SR.TFE == 1` after every
burst, before any caller touches DC or CS.  This cost one debugging session; it
is not optional.

### Speed and interruption

The SSPI master's output is always the reference divided by an **even** divider,
so it is at most **reference/2**, and the controller caps at **50 MHz**.  The
reference is whatever the bootloader left selected -- the SDK default
`RC96M48M` at 96 MHz -- so the only outputs below the cap are 48, 24, 16 and
12 MHz.  **There is no step between 24 and 48.**

M-G3a shipped 24 MHz: **measured 19.5 fps, 100 frames in 5127 ms**, i.e. 51.3 ms
per frame against 51.2 ms of pure wire time -- the DMA adds essentially nothing,
because a whole frame is one descriptor chain.  #32 raised the request to
**48 MHz**: **measured 38.7 fps, 100 frames in 2583 ms** (`BAUDR` read back as 2,
`sclk` as 48000000), 25.8 ms per frame against 25.6 ms of wire time.  The same
result -- the wire is the whole cost, and the clock is the whole lever.

**That change costs no clock-tree work.** #32 was filed assuming SSPIM had to be
re-sourced from the PLL, which would collide with the rule that the app does not
reconfigure the inherited clock tree.  It does not: the source mux and its SCU
divider stay exactly as inherited and only the controller's own `BAUDR` moves,
from 4 to 2.  Going further -- PLL at 100 MHz for the full 50 MHz -- buys 4% and
was dropped.

The panel is out of spec above ~15 MHz at either setting (24-60 MHz is in
universal use for ST7789 writes); what actually limits this is signal quality on
the jumper wires, and at 48 MHz the flying-wire hookup in `Wiring` shows no
degradation -- colour bars and solid fills are clean.  The driver still **asks
the vendor driver for the fastest output this reference can give**
(`SPI_CMD_MST_UPDATE_SYSCLK`) and requests the lower of that and its own
constant, and the achieved clock is **read back from the controller** and printed
by `lcd`, never assumed.  The numbers `lcd` prints stay labelled as ceilings:
48 MHz is a ~39 fps ideal before any DMA turnaround, `RAMWR`, or inter-frame gap,
and the measurement lands 0.3 fps under it.

That clamp is verified against `libdriver.a`, not assumed, because it could
otherwise silently cost speed: `SPI_CMD_MST_UPDATE_SYSCLK` dispatches to
`dw_spi_update_system_clk()`, which for device id 1 (`USE_DW_SPI_MST_S`) reads
`SCU_CLK_FREQ_TYPE_LSC_SSPIM` and returns `freq >> 1` -- 96 MHz becomes 48 MHz
and the clamp is a no-op.  The `freq >> 3` branch in the same function is device
id 3, the SSPI **slave**.  The call is a register read plus a shift with no wait
in it, so it is safe under `PRIMASK`, and it belongs inside the bring-up's masked
window so that anything it might enable is still covered by the EPK accounting.

`lcd loop` waits in 1-tick slices rather than one long block so Ctrl+C aborts a
transfer that is still **on the wire** -- with one burst per frame, waiting the
burst out and only then noticing the cancel would never exercise the in-flight
path that M-G3b's Ctrl+C handling depends on.  An abort halts the chain and then
**drains the completion semaphore**: `spi_write_ptl_halt()` races the transfer it
stops, and a count left behind would make the next burst return before its data
had gone out, desynchronising every transfer after it.  Verified on hardware:
abort mid-burst, `lcd bar` immediately after, then `lcd loop 100` back at the
full rate.

A cancelled `lcd loop` prints nothing by design -- the shared core discards
output produced while `cancel_req` is set, so `^C` is the feedback.

An abort that cannot prove the controller stopped (`SR.BUSY` never clears) does
not gamble: the DMA lines stay **masked**, the panel latches faulted, and
`lcd init` refuses until the next reboot.  Clearing pending and re-enabling a
line while the controller might still be shifting is how a completion arrives
*after* the drain -- and a count left in the semaphore makes the next burst
report success before its data has gone out.

### The panel is single-owner

The shell runs any command as a background job, so `lcd loop &` is a real second
thread contending for the framebuffer, the command bounce buffer, the CS/DC
pins, the vendor SPI device and the completion semaphore.  The cheapest of those
collisions overwrites pixels a DMA is still reading; the worst has one caller
consume another's completion, or halt another's chain during its own
cancellation, which wedges the vendor driver's busy flag for good.

A ThreadX mutex guards the whole driver.  Contenders **fail rather than queue**
(`lcd: busy`): a background job silently blocking the console for a frame time
is not an improvement on being told the panel is taken.  The mutex is recursive,
so a command holding it across a whole loop still nests through
`lcd_fill()`/`lcd_blit()`, and each of those guards itself for callers that come
from elsewhere (M-G3b's frame sink will).

### Bring-up is a transaction

`lcd_teardown()` unwraps the accounted interrupts **before** closing the SPI
device, and every failure path calls it -- including ones that happen long after
`lcd_spi_open()` returned, such as the panel's init table failing.

The order matters and so does the completeness.  `grove_epk_irq_wrap_new()` can
wrap two lines and then fail on the third; leaving those two registered while
the caller closes the device underneath lets `spi_close()` move a vector out
from under a live registration, and `thread` then reads `--` until the next
reboot.  So the wrap returns an undo log (`struct epk_irq_wrapset`) and the
caller rolls the whole attempt back.  Wrapping a line that is already wrapped is
refused outright, because a duplicate entry would make the unwrap restore
whichever one it found first -- a stale vendor vector on a live interrupt.

`test/test_epk_irq_wrap.c` pins this: partial failure, rollback, nine failed
attempts followed by a successful one (more than the eight trampoline slots, so
a leak fails the test), and a sweep after every step asserting the invariant
`AGENTS.md` states -- **every line is either disabled, or wrapped AND
registered**, never a third thing.

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

- LED / button / GPIO commands, SD (SPI mode), PDM microphone.
- Camera (MIPI CSI) and Ethos-U55 inference.
- MVE: usable only once the ThreadX Cortex-M55 port preserves VPR across a
  context switch, or only from code that cannot be preempted.  Until then the
  predication scan keeps it out of the image and the benchmarks are scalar.
