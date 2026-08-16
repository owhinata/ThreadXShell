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

### Carried forward to M-G3b, and since closed: the Timer0 interrupt path

`hw_start()` proves the counter RUNS -- four windows timed by `udelay()` (DWT
cycles, independent of the timer under test), each required to show motion, and
the amount checked against the rate Timer0's own clock predicts wherever a
window cannot contain a whole cycle.  That closes "armed but stationary" and
"armed but running at some other rate".

It does **not** prove the interrupt gets delivered.  A Timer0 whose counter is
fine while `IRQEN` produces no `INTSTATUS`, or whose NVIC path is dead, would
still be accepted, and the datapath's frame pacing would then never fire a
callback -- armed, believed, and useless.  The registry cannot catch it
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
auto-vectoriser emits predicated MVE, which `check_mve_predication.py` fails the
build on.  [!] That gate's premise is wrong (the hardware does stack VPR, see
issue #42 and the Future work note below); it is kept because it is
fail-closed, so today's published score is a **scalar** score.

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

## Camera (OV5647 over MIPI CSI)

```
camera probe             power the module and read its sensor ID
camera capture           one frame + per-channel statistics
camera preview [frames]  live preview on the panel, Ctrl+C to stop
camera raw               capture the Bayer mosaic and name the phase
camera stats             producer and sink counters
camera exposure [lines]  read/set the sensor's exposure
camera gain [a [d]]      read/set analogue / digital gain
camera wb [r g b]        read/set the software white balance (256 = unity)
camera black [n]         black level subtracted before the gain (pedestal 16)
camera sat [n]           saturation, standing in for a colour matrix
camera auto [on|off]     the sensor's own AEC + this port's white balance
camera gamma [on|off]    sRGB encode (off by default)
camera bayer [mode]      demosaic phase
```

An OV5647 module in the board's MIPI CSI connector.  The datapath is fixed, and
it is the donor's shipping OV5647 configuration rather than one invented here:

```
OV5647 640x480 RAW10, 2 MIPI lanes (the sensor bins on chip)
  -> no INP crop -> 4:2 binning -> 320x240
  -> HW5x5 demosaic (BGGR) -> WDMA3
  -> software pack to RGB565 -> svc/frame_pipeline -> ST7789
```

**[!] The IMX219 was supported and was removed (issue #54).**  Both modules fit
the same connector, but they were never a like-for-like choice: the IMX219
streams its full 3280x2464 and makes the INP do a single 10.25x reduction, which
is soft and aliases, and it has no auto-exposure of its own -- so the port
carried a software AE loop purely for it.  That loop, `camera depth`, the frame
length argument to `camera exposure` and the IMX219 register tables all went
with it.  The history is in git if a part without on-chip AE ever arrives.

`camera preview` rotates the panel to landscape first.  320x240 then maps 1:1
with no CPU-side transpose, because MADCTL really rotates this panel over 4-wire
SPI (issue #31).

### What the hardware gives you, and what it does not

**WDMA3 output is PLANAR B/G/R**, three 320x240 byte planes back to back, B
first -- not interleaved RGB565.  There is no hardware packer for this path:
HXCSC, which looks like one, is an input *un*packer for already-packed data and
has zero uses anywhere in the SDK.  So the interleave is software
(`port/camera/cam_convert.c`), and `camera capture` reports the three planes as
the hardware produced them rather than the packed result -- the packer has a
host test, the camera cannot have one.

**The CSI FIFO fill level is computed, not tabulated.**  The SDK writes that
formula in `double` and finishes with `ceil()`; this firmware links no libm, so
`port/camera/cam_mipi_calc.c` is exact integer arithmetic over the same
rational.  `test/test_cam_mipi_calc.c` checks it against the original formula --
transcribed into the test and swept over the whole parameter space -- rather than
against numbers worked out by hand.

**There is no way to read D-PHY lock back.**  Every SDK helper involved returns
void.  So the rev-C workaround below cannot verify what it would like to; see
what it settles for instead.

### [!] There is no auto-exposure and no auto white balance

Neither exists anywhere in this datapath.  The donor applications feed the raw
output straight to a neural network, which does not care what it looks like, so
the sensor simply sits at whatever fixed exposure and gain it was programmed
with -- the donor's constants, tuned for the donor's scene.

Two consequences, both of which look like bugs and are not:

- **Frames come out dark** unless the exposure suits the room.
- **Frames come out green.** A Bayer array has twice as many green photosites
  as red or blue and its green filters pass more light, so an uncorrected frame
  is green-heavy.  Measured here in room light: R 58 / G 66 / B 54, i.e. about
  1.2x green -- which is exactly what the eye reads as a cast.

The white balance is in SOFTWARE because it has to be: the sensor exposes only
a global analogue gain and a global digital gain, and both move all three
channels together, so neither can correct a cast.

**`camera exposure` and `camera gain` also switch the sensor to manual.**  The
OV5647's on-chip AEC/AGC would otherwise write over the value on its next frame
and the command would appear to do nothing, so both setters write 0x3503 as
well.  The console's `again` is a 0..232 curve inherited from the IMX219 this
port also drove; on this part it is mapped onto the sensor's linear
16-means-1x, so one number means one thing in the command and in the report.

`camera exposure` / `camera gain` / `camera wb` set all of this at runtime, and
that is the point of them -- finding good values by editing a `#define` costs
one flash cycle per guess on a part rated ~100k of them, with a manual
press-the-button flow.  Once a set is known good, bake it into the defaults in
`camera.c` (the white balance).  `camera capture` deliberately reports the RAW
planes, before the white balance, so its statistics stay evidence about the
sensor rather than about the gains.

### `camera auto`: the sensor's exposure, this port's white balance

The datapath provides NEITHER an exposure loop nor a white balance, because the
applications it was built for feed the output to a neural network which does not
care what the picture looks like.  A human preview does.  The two halves come
from different places:

- **Exposure** is the OV5647's own on-chip AEC/AGC.  `camera auto on/off` writes
  0x3503 to hand it over or take it back, and the producer thread reads the
  sensor's current values back every few frames so the console reports what is
  really in the part rather than what was last written by hand.
- **White balance** is this port's, in software: grey world, green held as the
  reference so the other two move toward it rather than all three drifting in
  brightness.  Damped and clamped, because grey world is simply wrong for a
  frame filled with one colour and the clamp is what stops being wrong from
  being catastrophic.

**[!] There is no software exposure loop any more (issue #54).**  There was one,
for the IMX219, which has no AEC of its own; it went with that sensor.  Two
exposure loops on one part do not average out -- ours would measure a mean the
sensor had already corrected and correct it again -- so on a part that has one,
standing down was always the behaviour.

**[!] `camera exposure` and `camera gain` turn auto OFF (issue #39).**  Writing
a value by hand IS taking manual control, and the sensor's AEC/AGC is already
switched to manual by the write itself (0x3503) -- so leaving the flag set meant
`camera auto` reported "on" while the sensor's loop was stopped.  Note that the
flag is shared, so this freezes the white balance too; the commands say so when
it happens, and `camera auto on` hands both back.  One flag is deliberate:
reporting "auto: on" while only one of the two loops runs is exactly the
confusion this fixes.

`camera auto off` before any measurement that assumes the sensor is holding
still -- comparing Bayer phases, or reading `camera capture` twice -- since a
loop adjusting the exposure in between is a variable nobody asked for.

`test/test_cam_auto.c` runs the white balance against simulated means for a few
hundred iterations: it settles, the corrected channels end up close to equal,
the gains stay inside their clamps, and a black frame does not move them.  Those
are failure modes that look fine in a single capture and are miserable in a live
preview.

### The sensor seam

The port asks over I2C which part is fitted rather than taking a build option --
a build flag would cost a flash cycle to swap camera, on a board rated ~100k of
them.  With one part in the table that is an identity check rather than a
choice, and it stays a table because that is the shape a second part arrives in.

`cam_sensor.c` dispatches exposure, gain, auto and read-back through per-sensor
function pointers, and that seam stays even at one entry: what differs between
parts is more than a register table.  The OV5647's exposure is 20 bits in
SIXTEENTHS of a line across 0x3500..0x3502 and its gain is 10 bits where 16
means 1x; the IMX219 kept them at 0x015a/0x015b and 0x0157..0x0159.  Writing one
part's addresses to another does not fail -- SCCB acknowledges a write to an
unimplemented register -- so an unguarded call would report success and change
something else.  That is why the frame-length argument and `camera depth` were
removed with the IMX219 rather than being re-aimed at the OV5647's VTS pair at
0x380e/0x380f.

**[!] The auto mode is re-applied at every bring-up, not just when asked for.**
A bring-up writes the mode table again, and on the OV5647 that table is what
switches its AEC back ON -- so without this an earlier `camera auto off` is
silently undone by the next `camera preview` and manual exposure stops holding.
The requested mode is also kept from before the first probe, when "the sensor"
is still a default descriptor and not the fitted part.

### The sensor is linear; the panel is not

This was the single biggest thing wrong with the picture, and it is not a
tuning matter -- it is units.  A sensor measures light, so its samples are
LINEAR.  Displays, this panel included (its init table programs gamma curves),
expect values already encoded with roughly a 1/2.2 power law.  Sending linear
samples to one does not fail; it produces a dark, flat picture with the
midtones crushed toward black.  Measured here: an ordinary indoor scene
averaged 49, which as a linear value is a normal midtone and as a display value
is nearly black.  Through the sRGB curve it becomes 121.

`port/camera/cam_convert.c` carries a 256-byte sRGB encode table (generated
from the standard transfer function; no libm in this image) and applies it
LAST -- after the black level and the gain, both of which are corrections to a
linear measurement.  Encoding first would turn the gain into a contrast control
and the black level into a crush.

It is ON by default, but it took a detour to get there: an earlier build
defaulted it OFF because the picture looked washed out encoded.  That was the
black-level interaction below -- with the pedestal still in the data the curve
lifts black to grey -- not a fault of the curve.  With both in place it settled
on the bench.  `camera gamma off` compares, and **the auto-exposure target
follows the switch**
(CAM_AE_TARGET_ENCODED_X100 vs CAM_AE_TARGET_LINEAR_X100): the loop aims at
what the panel shows, and the curve is the transfer function in between, so
wiring the loop to one and the converter to the other gives a picture that is
simply dark or simply blown while the loop reports success.

**[!] Gamma and the black level are a PAIR.**  The sRGB curve has a slope of
12.92 at the origin, so it multiplies whatever pedestal is left in the data: a
black level of 16 encodes to 71, and the picture comes out visibly washed out
with its blacks lifted to grey.  That is exactly what happened on hardware when
gamma was enabled on its own.  So the black level defaults to 16 alongside it --
and 16 is not a scene-tuned guess: the datasheet fixes the sensor's pedestal at
64 in RAW10 (16 after the receiver takes the top eight bits) and at 16 natively
in RAW8.

### [!] There is no colour correction matrix, and it shows

The single biggest reason the picture looks pale, and the one that survives
every amount of exposure and white-balance work -- including swapping the
sensor, which is why it took so long to name.

A sensor's colour filters have broad, heavily OVERLAPPING spectral responses:
the red pixels see a good deal of green and blue light, and so on.  A real
imaging pipeline undoes that with a 3x3 colour correction matrix between the
demosaic and the display encode.  This datapath has no such stage, so what
reaches the panel is raw sensor RGB, which is inherently desaturated.  No
exposure setting and no white balance can put the colour back, because neither
is the missing operation.

A correct matrix is per-sensor measured data this project does not have, so
`camera sat` is the honest stand-in: it pulls each channel away from the
pixel's own luma, restoring the LOOK of saturation without claiming to be
colorimetric.  Default 600 (2.3x), settled on the bench and within the range a real CCM
works out to for sensors of this class.  Applied after the white balance and before the
gamma -- saturation is a linear-light operation, and doing it on encoded values
distorts hue as well as amount.

**It also makes phase errors obvious**, which is how the OV5647's swapped red
and blue were caught: at unity saturation a swap is a slightly odd tint, and at
1.8x it is a blue object rendered yellow.

### Bit depth: RAW8 was tried, and is worse

The 10 -> 8 reduction inside the MIPI receiver is undocumented and
unconfigurable, so the natural suspicion is that it damages the image.  It does
not.  **This was measured on the IMX219, with a `camera depth` command that no
longer exists** (both went in issue #54) -- switching that sensor to RAW8, one
byte per pixel with no CSI-2 packing at all and the reduction done inside the
sensor as its own designed "10b-8b compress", made the result measurably WORSE:

| | RAW10 | RAW8 |
|---|---|---|
| means B/G/R | 43.7 / 49.6 / 49.0 | 18.0 / 22.0 / 21.5 |
| colour separation | 6.53 | 4.42 |
| blue minimum | 26 | 0 (clipped) |

Same Bayer phase, same `mosaic` figures, about 2.3x less signal and less colour.
If the receiver's RAW10 unpacking were mangling anything, removing the packing
entirely would have improved matters dramatically; it did the opposite.  **The
RAW10 path is not the problem**, and that is the part of this worth keeping:
RAW10 is what the port sends, and there is now nothing to switch.

Recorded because it cost a flash cycle to learn, and because of the trap in
doing it: switching depth took THREE registers, not two -- `CSI_DATA_FORMAT`
(0x018C/0x018D) **and `OPPXCK_DIV` (0x0309)**, which the datasheet's clock tree
requires to equal the bits per pixel.  Changing the format alone left the sensor
clocking pixels at the 10-bit rate while announcing 8-bit ones, and the datapath
simply failed to start.  (The upstream Linux imx219 RAW8 patch has the same
omission; its review raised 0x0309 for this reason.)

### [!] Is the demosaic right?  Two different questions

`camera capture` prints two figures, and they answer different things.  Reading
the first as an answer to the second is a mistake this port made once:

| figure | question | reading |
|---|---|---|
| `mosaic` | did a demosaic run **at all**? | single digits yes, tens+ = still raw Bayer |
| `colour` | was the **phase** right? | bigger is better, on a coloured scene |

Neither figure is sufficient alone, and the exact limits are worth stating,
because both were mis-stated once during bring-up:

- **`mosaic` sees a MISALIGNED phase but not a swapped one.**  Getting the grid
  alignment wrong (gbrg/grbg here) leaves phase residue and the figure jumps by
  an order of magnitude.  Swapping red and blue (bggr vs rggb) preserves the
  alignment perfectly, so `mosaic` cannot see it at all.
- **`colour` sees a misaligned phase but not a swapped one either**, for the
  same reason: an R/B swap changes which channel is which, not how far apart
  they are.

What a MISALIGNED phase does is interpolate red and blue from positions where
they do not exist, dragging both toward the green level -- so the frame
desaturates, and R and B collapse onto nearly the same mean.

Measured on this board, one indoor scene, all four phases:

| phase | mosaic | colour | B mean | G mean | R mean |
|---|---|---|---|---|---|
| bggr | 0.45-0.50 | 12.56 | 54.10 | 66.25 | 58.22 |
| gbrg | 4.63-4.65 | 10.55 | 65.67 | 56.07 | 65.71 |
| grbg | 4.66-4.68 | 10.52 | 65.70 | 56.08 | 65.66 |
| rggb | 0.46-0.50 | 12.29 | 57.73 | 65.75 | 53.88 |

gbrg and grbg are out on both counts -- and note how completely their R and B
means collapse together (65.67 vs 65.71), which is the sharpest signature of a
misaligned phase there is.  bggr and rggb are the R/B swap of each other and
the metrics cannot separate them.

**`camera raw` settles the family without any theory.**  It takes the demosaic
out of the path (INP -> WDMA2) so the buffer holds the Bayer mosaic itself, and
reads the phase off it: a Bayer tile has two green photosites on a diagonal,
green is the most sensitive channel and carries the most light in nearly any
scene, so the two greens come out highest and nearly equal -- and which
diagonal they occupy names the family.  It also shows what the MIPI receiver
made of the sensor's RAW10, which is the only direct look at that data this
port has.

**Red versus blue still needs a scene of known colour.**  Point the camera at
something strongly RED and see which `camera bayer` setting reports the red
mean as the high one; no amount of arithmetic on an unknown scene can do it.

#### Blue coming out YELLOW means red and blue are swapped

Worth knowing because it is not the symptom people expect.  A swapped R/B does
not turn blue into red: the blue photosites read as red, green survives, and
red plus green is **yellow**.  Seen on the bench with the OV5647 the moment
saturation was turned up -- the phase default had come from the donor cfg's
mirror-to-phase table and was RGGB, where the part is natively BGGR (Linux's
ov5647 driver reports SBGGR10 for the same reason).

The sensor defaults to BGGR, and `camera bayer` overrides stick across a
re-bring-up so a bench measurement is not quietly undone by a fault recovery.

### Bit depth: there is nothing to configure

The sensor sends RAW10 and the datapath is 8-bit from the INP onward.  That is
architecture, not a setting left at a default: `INP_IOBIT_E` offers 8/4/1 bit
and `INP_DATADEPTH_E` offers 8/4 bit -- **neither has a 10-bit value**.  The
only 10-bit knobs in the whole SDK surface are the CSI receiver's and
transmitter's `pixel_depth`, both set to 10 here (and the FIFO-fill computation
uses the same 10).  The 10 -> 8 reduction happens inside the MIPI receiver;
`hx_drv_csirx_set_default_tuncatecfg()` exists but **no application in the SDK
calls it**, and the truncate register is `#define`d in four donor files and
written in none.

If that reduction ever took the wrong end of the word, `camera raw` is what
would show it: a smooth scene would come back as noise instead of a mosaic with
two clearly-highest green positions.

The default phase (BGGR) is the part's native order with the mirror left off,
and it still deserves a way to be checked: the INP bins 4:2 **before** the
demosaic, which can move the effective phase.  So the phase is runtime-settable:

```
camera bayer bggr|gbrg|grbg|rggb
```

To settle it: point the camera at something strongly coloured, try all four,
and discard the two that show a high `mosaic` and R/B means collapsed together.
That leaves an R/B-swapped pair, which only a scene of KNOWN colour can
separate -- see the table above.  On a grey scene every phase scores low and
the measurement means nothing at all.

### [!] Four things that produce a plausible, wrong picture

1. **The DMA'd frame must be cache-invalidated before the CPU reads it.**  The
   shipping vendor glue invalidates only the 32-byte JPEG size word and never
   the 225 KB WDMA3 buffer, so this omission is easy to inherit.  The result is
   a frame that is mostly right and partly the previous one.  Invalidate after
   frame-ready (which is the statement that the write-DMA has stopped) and
   before reading.
2. **`lcd_blit()` wants wire order (big-endian) and the pipeline carries
   little-endian.**  `lcd_blit_le()` exists so the driver owns that swap.
   Publishing wire-order pixels under a `FRAME_FMT_RGB565` tag would be cheaper
   and would be a lie the next sink discovers.
3. **Buffers a DMA touches cannot live in TCM.**  Same rule as the framebuffer,
   same failure: no fault, the transfer simply never happens.  `.cam_raw` and
   `.cam_slots` are SRAM NOLOAD sections and `check_placement_budget.py` pins
   both.
4. **The demosaic's Bayer phase follows the sensor's mirror setting.**  BGGR
   here because the sensor is programmed HV-mirrored; a `_Static_assert` ties
   the two together so changing one alone is a compile error and not a colour
   swap that looks like a software bug in the packer.

### [!] Unwrap before the vendor teardown, not after

`cam_dp_full_stop()` IS the vendor's "close the device", and it moves
interrupt vectors.  Running it while this port's wrappers are still installed
and registered leaves the accounting registry holding pointers to vectors that
no longer exist -- and the failure is permanent and silent until you look:

```
grove> camera preview
^C
grove> thread
thread: cpu% unavailable (an accounted interrupt vector was replaced)
```

That was observed on hardware, and it is the same lesson `lcd_teardown()`
already carried ("unwrap the accounted interrupts BEFORE closing the device").
`cam_quiesce()` therefore masks the lines, hands the vectors back, and only then
lets the vendor tear the datapath down.  The lines are left DOWN afterwards --
the next start re-enables them inside a measure-then-wrap round, which is the
only way any line may come back up.

### Restarting is a barrier, not a flag

The datapath callback carries a status code and nothing else -- no frame number,
no handle, no generation.  A late event from a stream that has already been
stopped is therefore indistinguishable at the callback from a fresh one, and a
generation counter in the driver cannot fix that, because the event carries no
generation to compare.

So every restart quiesces first: full stop, mask this port's interrupt lines,
*then* clear the latched peripheral status and the NVIC pending bits, drain the
semaphore, and only then re-arm.  **Stop before clear**, exactly as the panel's
abort path had to learn (see "A DMA completion is not a transfer completion"):
clearing while the hardware is still running just means the next event lands
after the clear, and on a restart it arrives as the new stream's first frame.

There is one stop sequence (`cam_dp_full_stop`) and all four ways of ending
a stream go through it: the user's stop, a frame-timeout, a terminal datapath
event, and a bring-up that failed part way.  The datapath configuration is
deliberately NOT kept across it -- the stop performs a datapath software reset
and nothing in the SDK says which registers survive one, so the next start
reconfigures from scratch.

Errors have priority over frames: the callback's error latch is sticky and is
checked before the frame-ready flag, so a wakeup carrying both never publishes a
frame produced by a datapath that has already failed.  Any negative status the
port does not recognise is treated as terminal.

### Chip revision C needs a per-frame MIPI bounce

_(rev-C bounce)_  **The board here reports `0x8538000d` (revision D), so this path does not run
on it and is untested on this hardware.**  It is carried because the donor
carries it and rev-C parts exist in the field.

On `0x8538000c` the receiver loses D-PHY lock across a frame boundary unless it
and the sensor stream are cycled between frames; every donor application carries
the same workaround gated on the same version word.  It is expensive -- a full
receiver bring-up, including a PLL reselect and both PHY resets, per frame -- and
it freezes the sensor's own exposure and gain loops, so it runs only where the
version word says it is needed.  `camera probe` prints whether it applies.

Since D-PHY lock itself cannot be read back, "it came back" is established from
three weaker things, and the port says so rather than claiming otherwise:

1. the sensor's I2C writes returned success (the donor's helpers are all void;
   these are checked),
2. the receiver's latched error status is clean under a **clear -> enable ->
   poll** protocol -- it latches, so a poll without the clear reports the
   *previous* link,
3. the next frame arrives inside the bounded wait.  This is the real evidence,
   and it is established after the fact by the producer loop.

### The Timer0 interrupt path is now verified

M-G3a left this open (the archives were not linked, so the seam was
garbage-collected out of the image).  `hw_start()` proves the counter RUNS,
which is a different claim from "the interrupt is delivered": a Timer0 whose
IRQEN produces no INTSTATUS, or whose NVIC path is dead, passes every check it
makes, and the datapath library paces capture with this timer.

`grove_timer_seam_probe_delivery()` closes it.  It runs once, from the camera
bring-up, in **thread context with interrupts enabled** -- outside the
measure-then-wrap PRIMASK window, because it waits for its own interrupt and
under PRIMASK could only ever time out.  It arms a short periodic Timer0 through
the real wrapped entry point, reads the NVIC enable bit back, waits a bounded
100 ms, and then stops, unregisters and checks the line went back down.  A
failure classifies itself (never enabled / INTSTATUS asserts but nothing is
delivered / the timer never raises it), latches the reason for `epk`, and
**refuses the whole camera bring-up**.

`test/test_timer_probe.c` pins both directions, since the failing one cannot be
produced on demand on hardware: the mock NVIC calls the installed vector from
inside `udelay()`, which is where the firmware waits.  It is compiled twice
because the probe latches its answer on the first call.

### Interrupt accounting: 32 wrap slots

The camera brings up interrupts by the dozen and which ones is not knowable
before the measurement, so `GROVE_EPK_WRAP_MAX` and `TX_GLUE_EPK_MAX_IRQ` are
both 32 (they must move together; a `_Static_assert` says so).  The ceiling
comes from the candidate set -- sensor control 20/84/85, Timer0 34, and
INP/DP/EDM/WDMA 136..157, at most 26 lines -- plus the console UART and the
panel's SPI/DMA.  CSIRX and CSITX are not NVIC-registered at all on this path;
their status is polled.

The measure-then-wrap protocol runs in **several rounds**, each under PRIMASK,
with the sensor's I2C traffic in between with interrupts enabled.  One round
would mean masking interrupts across roughly ten milliseconds of mode-table
writes, which stalls the console and drops ThreadX ticks.  Everything that can
turn a line on has to be inside a round -- not just the CSI receiver, but the
INP/WDMA configuration and the sensor controller's start, which enable lines of
their own.

#### [!] The vendor re-registers ISRs on lines it already has running

Measured on hardware, and it breaks the protocol's central assumption:

```
grove> epk
  displaced irq : 143  wrapper 100019a5  now 10010d59
```

irq 143 is wrapped at start-up with one vendor handler and is carrying a
DIFFERENT one a frame later -- the datapath's per-frame retrigger re-registers
its ISR.  Measure-then-wrap cannot see that, because the line never changes its
ENABLED state: the ISER diff is empty and the wrap has nothing to do, while the
vector table quietly stops pointing at the trampoline.  The line keeps working;
it just stops being accounted, and `thread` blanks the whole cpu% column for as
long as the stream runs.

So the wrap is **defended, not just established**: `grove_epk_irq_reassert()`
runs once per frame and, for any line whose vector is no longer the trampoline,
adopts the NEW vendor handler as the one the trampoline calls and puts the
trampoline back.  Re-installing the *original* saved handler would be wrong --
the vendor changed it for a reason.  `epk` reports how often this has been
needed; a count that tracks the frame count means it is happening every frame.
Measured here: 118 re-wraps in 12.2 s of preview, i.e. once per frame, and with
the defence in place `epk` reports `cpu% accounting : live` throughout.

The rounds have **two different lifetimes**, and that distinction is load
bearing:

| group | what it wraps | lifetime |
|---|---|---|
| bring-up | module power, the CIS layer | as long as the port is up |
| datapath | CSI receiver, INP/WDMA, capture start | **one stream** |

The datapath rounds are rebuilt by every start and every timeout retry, so they
are unwound first -- a wrap refuses a line that is already wrapped, so without
the unwind the second start finds no free round and the retry budget is
decorative.  Recovering from a fault likewise unwinds everything before
rebuilding: dropping the undo log instead would leave those lines enabled and
registered, absent from the next ISER delta (they are no longer "fresh"), and
impossible to unwind ever again.

### Not yet answered

- ~~Frame rate~~ -- **measured, see "Where the preview's time goes" below**
  (issue #38).  It is 15.8 fps, not the ~10 fps recorded here earlier, and the
  leading explanation on file was wrong.
- The producer's stack high-water mark (4 KB allocated; `thread` reported a
  568 B peak on the first run, so there is room to trim once the sink work is
  settled).
- Whether the baked-in defaults are the RIGHT ones.  Values were found on the
  bench and are now the defaults (exposure 1000, again 0x40, black level 16,
  saturation 600, gamma on), but they were chosen by looking at the panel, not
  by measuring a colour target -- so they are a starting point, not a
  calibration.  The runtime commands exist so that changing them costs no flash
  cycles.  For reference, the donor's own defaults measured in room light:
  means R 58 / G 66 / B 54, and `mosaic` under 0.5 on every plane -- i.e. the
  demosaic was working and the frame was simply under-exposed and unbalanced.
  A colorimetric answer needs the colour correction matrix, tracked in issue
  #37.
- The rev-C bounce, which this board (rev D) does not exercise.  Deliberately
  not tracked as an issue: it is a condition this board cannot reach, not a
  piece of work.  This section is where it lives until a rev-C board turns up.

### Where the preview's time goes (issue #38)

`camera stats` prints the producer's own per-stage timing, since the last
stream start.  It is measured on TIMER2 (the EPK's free-running source) and not
on DWT CYCCNT, because the largest candidate stage is the producer ASLEEP
waiting for a frame and CYCCNT stops in WFI -- a cycle counter would report that
stage as free and produce exactly the wrong picture.

Four runs, same scene, ~80-150 frames each (microseconds per frame):

| run | total | wait | work | pack | sink | fps |
|---|---|---|---|---|---|---|
| baseline (auto on) | 63,244 | 19,261 | 43,983 | 17,186 | 26,452 | 15.8 |
| `camera exposure 200` | 62,892 | 19,059 | 43,833 | 17,209 | 26,445 | 15.9 |
| `camera exposure 1500` | 62,503 | 18,784 | 43,719 | 17,257 | 26,447 | 16.0 |
| `camera gamma off` + `camera sat 256` | 62,972 | 26,800 | 36,172 | 9,546 | 26,447 | 15.9 |

`work` is `total - wait`, i.e. everything the CPU does.  `invald` is 145 us and
`tune` is 221 us with auto on (0 with it off); neither is a factor.

**The exposure does nothing.**  200 against 1500 lines is a factor of 7.5 and
moved the total by 0.6%.  The hypothesis that a stretched frame length paces the
loop is refuted: whatever sets the pace, it is not integration time.

**`sink` is the SPI DMA, and it is a hard floor.**  153,600 bytes at 48 MHz is
25.60 ms of wire time; measured 26.45 ms, and across four runs it varied by
7 us -- 0.03%.  So the byte-swap copy inside `lcd_blit_le()` costs about 0.85 ms,
**1.3% of the frame**.  Removing it would mean teaching `svc/frame.h` a
big-endian RGB565 format, which reaches every board, for 1.3%.

**`pack` is 44% arithmetic, not pixel traffic.**  Turning off gamma and
saturation took it from 17.2 ms to 9.5 ms.

**[!] AND NONE OF IT MADE THE PREVIEW FASTER.**  That last run saved 7,811 us of
CPU work; `wait` grew by 7,539 us and the total moved 272 us -- 0.4%.  The loop
is paced by something outside the CPU work, at about 63 ms, and time saved in
`pack` is simply spent in `wait` instead.

That is the finding, and it retires two of the three things #38 proposed:
removing the byte swap and moving the blit to its own thread both make the CPU
faster, and the CPU is not what is holding the frame rate.

**The pacer is the datapath, and its period is quantised.**  `camera bench`
takes the 26.4 ms blit out of the loop by running the producer with no sink, so
the work can be varied over a 4.6x range; `camera vts` moves the sensor's frame
length.  Six runs:

| run | VTS | work | predicted | measured | err |
|---|---:|---:|---:|---:|---:|
| bench, gamma on | 504 | 17,618 | 47,639 | 47,778 | -0.3% |
| bench, gamma on | 984 | 17,611 | 62,006 | 62,006 | 0.0% |
| bench, gamma on | 1968 | 17,619 | 62,006 | 62,729 | -1.2% |
| bench, gamma off | 984 | 9,777 | 31,003 | 31,254 | -0.8% |
| preview | 984 | 43,478 | 62,006 | 62,625 | -1.0% |
| preview | 1968 | 43,540 | 62,006 | 62,722 | -1.1% |

The model behind the predicted column, and it holds to 1.2% across periods from
31 to 63 ms:

```
period = T_s * ceil((W + T_active) / T_s)

  T_s      = VTS * 31.507 us          (HTS 1852 at a PCLK of 58.8 MHz)
  T_active = 480 * 31.507 = 15.1 ms   (one active frame on the wire)
  W        = the producer's own work
```

**The datapath is one-shot.**  The producer arms it only after consuming the
last frame, so every period contains the wait for the next frame start plus a
whole active frame -- and the total is then rounded UP to a multiple of the
sensor's period.  That single `ceil` explains everything the earlier runs made
look mysterious: why exposure changed nothing (it never touched VTS), why 984
and 1968 measure the same 62 ms (N=2 against N=1), and why 504 gave 47.8 ms
rather than the 16 ms its frame length alone implies (N=3).

**[!] The optimum is a cliff edge.**  With today's producer, W + T_active is
58.7 ms.  VTS 1968 clears it with margin (N=1, 62 ms).  VTS **1860** gives
T_s = 58,603 us -- **60 us short** -- so N becomes 2 and the frame rate HALVES
to 8.5 fps.  Any VTS tuned to sit just above the current work is one `camera
gamma` away from falling off that edge, which is why the default is not tuned
tight.

**What actually raises it.**  Not the frame length by itself: at W = 43.5 ms
every VTS from 1864 up gives the same ~62 ms, and everything below is equal or
worse.  Not the CPU by itself either: at VTS 1968 the period is one sensor frame
whatever the CPU does.  It is BOTH, and the numbers say which way round:

| | W | best VTS | period | fps |
|---|---:|---:|---:|---:|
| today | 43.5 ms | 1968 | 62.0 ms | 16.1 |
| blit off the producer | 17.6 ms | ~1100 | ~34.7 ms | ~29 |

So the three things this section used to list as "deliberately left out" --
removing the byte swap, giving the sink its own thread, overlapping capture with
conversion -- are not pointless after all, which an earlier revision of this
file wrongly concluded.  They were simply not sufficient on their own: they cut
W, and W only matters once VTS is chosen to let it.

`sink` remains 26.45 ms of SPI wire time (153,600 bytes at 48 MHz is 25.60 ms in
theory, and it varied by 7 us across four runs), so taking the blit off this
thread means overlapping it, not making it cheaper.  The byte swap inside it is
~0.85 ms, 1.3% of a frame, and still not worth teaching `svc/frame.h` a
big-endian RGB565 format for.

## Ethos-U55 inference (`nn`)

Capture a frame, run it on the NPU, print the result.  Classification (top 5,
92 ms) since issue #44; BlazeFace face detection (13 ms) since issue #45; face
boxes on the LIVE preview (`nn preview`) since issue #48.  The console stays
usable while any of them runs.

```
nn open [cls|det|<addr>]  # QSPI XIP on, NPU out of reset, model parsed in place
nn info                   # tensors, arena use, which interrupts got wrapped
nn run                    # one frame -> classification, top 5
nn detect                 # one frame -> face boxes
nn thresh [<1..999>]      # the detector's score threshold, milli-probability
nn run &                  # either, in the background; the prompt comes straight back
nn close
```

`cls` and `det` are the two model partitions below; the raw address form stays
for checking a model somebody put somewhere else.

The models are SEPARATE flash partitions from the firmware:

```
cmake --build build/grove-vision-ai-v2 --target flash-model-cls   # MobileNet
cmake --build build/grove-vision-ai-v2 --target flash-model-det   # BlazeFace
```

which means iterating on firmware never disturbs them, and swapping models never
rewrites the bootloader -- worth having on a part with ~100k NOR cycles.

### The flash partition map, and the checks over it

| partition | write address | reserved blocks | today's artifact | flashed by |
|---|---|---|---|---|
| firmware | `0x000000` | `0x000000..0xB70000` | 405,504 B image | `--target flash` |
| model-cls | `0xB7B000` | `0xB70000..0xD20000` | MobileNet, 1,704,672 B | `--target flash-model-cls` |
| model-det | `0xD20000` | `0xD20000..0xD50000` | BlazeFace, 164,512 B | `--target flash-model-det` |

Addresses live in ONE place: `GROVE_MODEL_CLS_ADDR` / `GROVE_MODEL_DET_ADDR` in
`board.cmake`.  They are compiled into `cmd_nn.c` as well, so `nn open det` and
`--target flash-model-det` cannot drift apart.

**The layout is reservations, not files.**  `cmake/check_flash_partitions.py`
checks that the reservations are disjoint and fit the part *with no artifacts
present at all* -- a layout is a property of addresses.  Only the artifact
actually being written has to exist, so updating the firmware never depends on
having built a model.  Any other artifact that happens to be present is measured
too, which catches a model that outgrew its slot before the flash that would
have proved it the destructive way.

[!] The first version of this required every declared file, which made plain
`--target flash` refuse on any clean tree -- the detection model cannot be
committed, so it is never there until somebody builds it by hand.  A gate that
blocks the operation it is not protecting is a gate that gets deleted.

It compares **destroyed footprints, not file extents** -- xmodem pads its last
packet, and the flash is erased in blocks -- so two regions whose bytes do not
touch can still be found to collide.  The erase block size the resident Himax
bootloader uses is not known (it is closed, and was not disassembled to a
conclusion), so the check rounds to **64 KB**, the largest this NOR offers: a
conservative bound that contains whatever the bootloader actually does.

`0xD20000` is the first 64 KB boundary above the classification model's extent
(`0xB7B000 + 1,704,672 = 0xD1B2E0`), so the two reservations abut exactly.
Host tests: `test/test_flash_partitions.py`.

**Both model targets verify before they transmit.**  `flash-model-cls` and
`flash-model-det` stage a copy, run the partition check and `verify_vela_model`
**on that staged copy**, and then send **that same file** -- so nothing can be
swapped in between, and a stale or malformed model never costs an erase cycle.
With no host C++ compiler the verifier cannot be built and the targets refuse;
skipping the check would be the fail-open the arrangement exists to prevent.

The one thing no static check can bound is a receiver that erases the whole chip
before writing.  That evidence is empirical: flash one model, then read the
other one back with `nn open` + a run.

### One operator, and therefore no CMSIS-NN

`MicroMutableOpResolver<1>` with `AddEthosU()` and nothing else.  A Vela-compiled
model folds its whole graph into the single `ethos-u` custom operator, so there
is no CPU kernel to register and no reason for CMSIS-NN -- which matters here
because CMSIS-NN is Helium, and Helium is what the predication scan keeps out of
the image.  A model Vela did NOT fully offload fails at `AllocateTensors` with a
missing-operator error rather than quietly falling back to a scalar reference
kernel; a silent fallback would be slow in a way nobody would think to look for.

This is a **policy**, not a cache constraint.  Before issue #46 a CPU operator
running after the `ethos-u` one inside `Invoke()` would have read the NPU's
output through a stale D-cache; since #46 the arena is invalidated before the
kernel returns, so it would not.  Keeping the resolver at one operator is a
choice about Helium and about failing loudly, and should be argued on those
grounds.

TFLM is built from source for the same reason: the only 2412-tag archive the SDK
ships is the CMSIS-NN one.  Linked cost is **15,360 B** -- an order of magnitude
under the 59 KB that summing the archive's object sizes suggests, because
`--gc-sections` drops everything this narrow configuration cannot reach.

### [!] The flash read window is not live at reset

`nn` parses the flatbuffer in place through the memory-mapped read alias at
`0x3A000000`, and that window does not exist until the application opens the
QSPI master and enables XIP.  The bootloader reads the firmware through the
controller's register interface, so nothing before us leaves XIP on.

The symptom is not what unmapped memory usually looks like.  Reads do not fault
and do not return 0xFF:

```
devmem dump 0x3AB7B000 32   ->  07 0c 40 00 00 00 00 00 ...
devmem dump 0x3A000000 32   ->  07 0c 40 00 00 00 00 00 ...
```

Identical bytes 11 MB apart -- a controller register block aliased across the
whole window.  `nn open` brings XIP up and the same dump then reads
`24 00 00 00 54 46 4c 33`.  `devmem`'s flash region is listed read-only for
exactly this: without being able to dump the window, the difference between
"nothing was flashed" and "the window is shut" is guesswork.

### `nn info`'s interrupt list shrinks after the first open, and that is correct

The first `nn open` after a reset reports `irq 133, 192`; every later one
reports `irq 192`.  Nothing is being lost:

- `npu_flash_xip_init()` is one-shot (`xip_ready`).  The first open calls into
  the QSPI library, which enables IRQ 133 as a side effect (below); later opens
  skip it because the read window is already up.
- `nn close` unwraps, and unwrapping DISABLES -- the EPK rule is that a line is
  either disabled or wrapped-and-registered, never a third thing.

So on the second open 133 is disabled and stays disabled, which is why the
snapshot does not see it become enabled and why nothing is "enabled but
unaccounted".  Reads keep working because the model is fetched through the
memory-mapped alias, not through the library's DMA path.

### [!] Bringing up the flash enables an interrupt nobody named

`hx_lib_spi_eeprom_open()` turns on DMAC1's combined interrupt (133) -- the QSPI
library moves flash data with DMA.  Nothing predicted that line.  The EPK
snapshot wrap caught it because it measures what got enabled rather than listing
what we expect, which is the whole argument for doing accounting that way.
`nn info` prints the wrapped set and it reads `irq 133, 192`.

### The NPU was never brought up before this

`TZ_Set_ALL_Secure()` -- what SEC_ONLY runs -- configures the U55's APB side and
stops.  The PORSL/PORPL, per-master MSC and reset->normal sequence lives in
`TZ_Set_Secure_ByCFG()`, which this build never reaches, so the NPU sat in
whatever state the bootloader left it in.

The sequence is the donor's secure branch with every write read back; a mismatch
aborts the bring-up and leaves a working shell without inference.  The MSC
filters are set to ERROR responses rather than the RAZ/WI default: the SDK
exposes no violation-status read, and zeros would reach TFLM as data whereas a
bus error becomes a recorded fault.

### [!] The arena changes hands twice, and both points are inside `Invoke()`

The U55 is an AXI bus master and is not coherent with the CM55's D-cache, so
every handover needs explicit maintenance.  Issue #44 did it per range from the
shell command.  That is wrong three ways, and issue #46 replaced it.

**Per-range maintenance cannot be made correct here.**  TFLM aligns arena
buffers to 16 bytes; this core's cache line is 32.  Any per-range operation
therefore rounds outward across a neighbour's half-line -- an invalidate discards
what the CPU wrote there, a clean writes a stale half back over what the NPU
wrote.  Measuring the model's own I/O does not help, because the boundaries that
matter are internal: the NPU writes intermediate feature maps throughout the
arena while the CPU writes the `ethos-u` kernel's `base_addrs` scratch (which is
`RequestScratchBufferInArena`, i.e. in the arena) and TFLM's persistent
allocator at the tail.

**Maintenance from the command is always mistimed.**  Cleaning before the call
is too early -- the kernel writes that scratch after it.  Invalidating after the
call is too late -- TFLM runs `ResetTempAllocations()` immediately after the
kernel returns and *before* it checks the kernel's status, which writes the
arena-resident allocator.

**Only one of the two vendor hooks was mistimed.**  `ethosu_flush_dcache()` is
called after the kernel has built its base-address arrays and immediately before
the power request and the launch -- exactly the last-writer boundary.  It was
neutered along with `ethosu_invalidate_dcache()`, whose call site really is
wrong (`ethosu_wait()` runs it before taking the completion semaphore, while the
NPU may still be writing).

So the port maintains the **whole arena** at two instants, which makes alignment
structurally irrelevant -- the arena is 32-byte aligned and a whole number of
lines by `_Static_assert`, so no partial line can be involved:

| point | owner | maintenance |
|---|---|---|
| `ethosu_flush_dcache()` | CPU -> NPU | clean the whole arena |
| `ethosu_inference_begin()` | NPU | arm |
| NPU running | NPU | caller suspended on the semaphore; other threads excluded by `nn`'s ownership gate |
| `ethosu_inference_end()`, `DONE` **and** `OK` | NPU -> CPU | invalidate the whole arena |
| `ethosu_inference_end()`, otherwise | NPU -> CPU | soft reset, **checked**, then invalidate |
| after `Invoke()` returns | CPU | nothing |

`ethosu_inference_begin` / `_end` are weak by design, receive the driver, and sit
at the right instants -- begin is reachable only after the power request
succeeded, and the driver's own comment says end is "always called even in case
of timeout".  The generic `ethosu_semaphore_take()` hook is NOT used: it is not
inference-specific (driver reservation takes the same path) and it is never
reached on a power-acquisition failure or a timeout, so a flag raised beside it
would be left standing.

Two details that look like paranoia and are not.  `DONE` is not success: the
interrupt handler sets `state = DONE` and *then* sets `result`, so a fault also
lands as `DONE`, while `result` initialises to `OK` and so does not by itself
prove the job ran -- both are required.  And the reset is checked: on timeout or
error the driver calls `(void)ethosu_soft_reset(drv)` and discards the result,
but "the NPU has stopped writing" is the precondition for invalidating at all,
so the port resets first and verifies.  If that reset fails there is no way to
establish the precondition and the firmware halts rather than corrupt the arena
intermittently.

### [!] A payload whose actions continue past the command stream

`ethosu_invoke_async()` parses driver actions in order, and the command-stream
action *launches the NPU* in the middle of that loop.  If a later action is
malformed the parser takes `goto err` and returns without ever calling
`ethosu_wait()` -- so `ethosu_inference_end()` never runs, the arena is never
handed back, and TFLM writes it while the NPU may still be running.

The SDK is read-only, so this is closed at the other end: `npu_open()` refuses a
model whose payload is not **exactly one `COMMAND_STREAM`, as the last action**.
With nothing left to parse after the launch there is nothing left to fail on.
Vela emits exactly this shape, but `nn open <addr>` takes an arbitrary address,
so it is checked rather than assumed.  `npu_cache_after_invoke()` is the
backstop: if an inference ever does return with the arena still owed, it halts.

Two things about that check are easy to get wrong, and both were:

- **The payload is not `custom_options`.**  The kernel reads
  `custom_initial_data` only for a CO_TYPE marker -- 3 bytes in the models here
  -- and hands the driver **input tensor 0** (`cms_data` from its buffer,
  `cms_data_size` from its `bytes`).  A check on `custom_options` inspects the
  marker and proves nothing.
- **A variable tensor is not the buffer you validated.**
  `MicroAllocator::AllocateVariables()` overwrites `eval_tensors[i].data.data`
  with an arena allocation for any tensor marked `is_variable()`, *even when it
  has a serialized buffer*.  The driver would then parse arena bytes -- which
  another operator can write at run time -- while the check inspected the
  constant in flash.  Variable tensors are refused.

The arm is one-shot and a second one is fatal rather than counted: the driver
runs `handle_command_stream()` once per command-stream action but waits once for
the whole walk, so two launches would arm twice and disarm once, and a counter
would let the backstop pass with a launch unaccounted for.

### Inference does not block the shell

The driver's default semaphore `malloc`s its objects and spins on `__WFE()`,
which would burn the core for the length of a run -- the console would stop
answering and Ctrl+C would not land.  A static ThreadX pool replaces it, so the
inference thread suspends instead.  `nn run &` returns the prompt immediately.

`ETHOSU_SEMAPHORE_WAIT_INFERENCE` is set to a finite 5000 ticks; the header
would otherwise default it to "wait forever", and a lost NPU interrupt would
suspend the calling job with no way back.

### [!] The QSPI archive carries an erase path

`lib_spi_eeprom.a` is linked for one reason -- enabling the read window -- but it
also defines `erase_all`, `erase_sector`, `write` and `clear_write_protect`, on
the flash that holds the bootloader.  All of them are on
`check_placement_budget.py`'s forbidden list and verified absent: with
`--gc-sections`, presence would mean a caller.

`setWriteEnable` IS present and is deliberately allowed.  Putting the part into
QUAD mode writes the QE bit in its status register and that write needs the WEL
latch first, so it is part of configuring the read path.  On its own a latch
cannot modify anything -- it has to be followed by a program or erase opcode, and
every entry point that issues one is barred and gone.

## Face detection (`nn detect`, BlazeFace-front 128)

Issue #45.  ST model zoo's BlazeFace Front 128x128 -- a MediaPipe-derived SSD
face detector -- on the NPU, decoded to boxes on the CPU.

```
nn open det
nn detect
nn thresh 500          # more sensitive; 1..999, milli-probability
```

### Building the model (it cannot be committed)

The weights are model-zoo licensed, so `*.tflite` is gitignored and the pipeline
is documented instead.  Everything it needs is in the build tree: `vela` is
pinned in `requirements.txt` and installed into `build/<board>/venv`, and the two
host tools come from `--target model-tools`.

```
cmake --build build/grove-vision-ai-v2 --target model-tools
cd build/grove-vision-ai-v2

./tflite_strip_boundary <path>/blazeface_front_128_int8.tflite \
                        model/blazeface_stripped.tflite
./venv/bin/vela --accelerator-config ethos-u55-64 \
                --output-dir model model/blazeface_stripped.tflite
mv model/blazeface_stripped_vela.tflite model/blazeface_vela.tflite
./verify_vela_model model/blazeface_vela.tflite --blazeface
```

`GROVE_MODEL_DET_FILE` already points at that last path, so
`--target flash-model-det` picks it up -- and runs `verify_vela_model` itself on
the staged copy before transmitting, so the manual run above is for reading the
report, not for safety.

**Why the stripping step.** "An int8 model" from the model zoo means int8
WEIGHTS and float32 I/O: the graph starts with a `QUANTIZE` and ends with four
`DEQUANTIZE`s.  Vela offloads 93.8% of the network (75 operators) and leaves
exactly those five on the CPU -- they are not arithmetic, they are the graph's
edges changing type.  This port registers ONE operator and intends to keep it
that way, so removing them from the FILE is what makes the model fit.  Nothing
is lost: the firmware already fills the input as int8 (`nn_fill_input()`'s
`pixel - 128` IS this model's scale 1/255, zero point -128) and the decoder reads
int8 outputs through their own quantisation.  After stripping, Vela reports
**CPU operators = 0**.

`tflite_strip_boundary` deletes no tensors -- the ones it orphans stay in the
table, so no index is renumbered and the tool stays short.  Vela rebuilds the
graph and prunes them, which `verify_vela_model` checks rather than assumes.

### What `verify_vela_model` proves, and what it does not

Vela's "CPU operators = 0" says the graph was offloaded.  It says nothing about
whether this firmware can run the result, so the gate answers the rest on the
host instead of on a board at the far end of a flash cycle:

- the payload passes **the firmware's own** `npu_payload.c` walk -- the file is
  compiled into the tool, not reimplemented, so it cannot agree with itself and
  disagree with `npu_open()`
- `AllocateTensors()` fits **the board's own** `npu_arena.c` -- likewise compiled
  in, so the size cannot drift from the reservation
- one subgraph, every operator `ethos-u`, int8 in and out, no unreachable
  tensors, an `OfflineMemoryAllocation` whose tensor count matches the subgraph
- `--blazeface` additionally pins the four output shapes the decoder looks for

Measured on the model built by the commands above:

| | value |
|---|---|
| model size | 164,512 B |
| arena used | 395,328 B of 460,800 (65,472 spare) |
| optimizer config word | `0x00001006` -- 64 MACs/cc, cmd stream v0, 16 KB SHRAM |
| arch version | 1.0.6 (vela 5.1.0) |
| Vela cycle estimate | 4,445,266 -> 8.9 ms at 500 MHz, ~11.1 ms at 400 MHz |
| **measured on hardware** | **13 ms**, arena **394,800 B** |

[!] The host's arena figure is ~0.1% HIGHER than the board's (395,328 against
394,800; 386,096 against 385,748 for the classification model).  The host is
64-bit, so TFLM's persistent allocations -- which are full of pointers -- come
out larger there.  The error is in the safe direction and the gate is left to
over-estimate rather than corrected with a fudge factor: a model that fits on
the host fits on the board, which is the property worth having.

The config word is **bit-identical to the MobileNet** that already runs on this
board, which is the strongest pre-hardware evidence available that the command
stream will be accepted; the arch version differs only in the patch field, which
`ethosu_dev_verify_optimizer_config` does not compare.  It is still only
evidence -- the driver checks compatibility against registers at INVOKE time, so
the first `nn detect` on hardware is the proof.

### The decoder

`port/npu/models/blazeface.c`: SSD anchor decode over 896 anchors (16x16 grid
with 2 per cell, 8x8 with 6), then hard NMS.  No libm -- the threshold is
compared as a pre-sigmoid logit and the reported confidence uses an algebraic
sigmoid.  It takes tensor DESCRIPTORS rather than reaching into the NPU
singleton, which is what lets `test/test_blazeface.c` compile the real decoder
and drive it with synthetic tensors.

Two things differ from the Wio/F746 implementation of the same model:

- **The anchor centres are computed, not tabulated.**  The donor fills two
  `float[896]` tables on first use behind a ready flag; the centres are
  `(cell + 0.5) / grid`, so computing them removes 7 KB of DTCM and a piece of
  lazily-initialised static state.
- **The scan is never cut short.**  The donor stops decoding when its
  64-candidate buffer fills, which makes two things wrong at once: the reported
  peak score becomes the maximum of a PREFIX, and NMS sees the FIRST 64
  candidates rather than the best 64 -- so a busy frame drops the strongest face
  if it lands in the 8x8 group, which is scanned last.  Here every anchor is
  visited and the candidate list is a bounded top-N with deterministic
  tie-breaking.  (The same bug is still in the other two boards.)

`nn detect` always prints the peak raw score, the number of anchors over the
threshold, the number kept, and the number after NMS.  "No faces" and "the
threshold is above everything the model produced" are different states and the
detection count cannot tell them apart.

### [!] The 8x8 group's scores are quantised very coarsely

The four outputs carry four different quantisations, and they are not
comparable:

| output | shape | scale | zero point |
|---|---|---|---|
| box regressors, 16x16 | 1x512x16 | 0.306708 | -47 |
| scores, 16x16 | 1x512x1 | 0.036937 | 49 |
| box regressors, 8x8 | 1x384x16 | 1.202013 | -47 |
| scores, 8x8 | 1x384x1 | 1.224698 | 126 |

The 8x8 score tensor has zero point 126 and scale 1.2247, so of the whole int8
range only `127` dequantises above zero -- its scores are effectively
three-valued around the threshold, and the 8x8 layer is all-or-nothing.  The
16x16 layer has a threshold step of about 0.037 and behaves normally.  This is
ST's quantisation, not a decoder property, but it explains a peak score that
jumps rather than sliding as `nn thresh` moves.

**[!] SINCE ISSUE #48 THIS IS USUALLY NOT WHAT YOU SEE.**  The wider field of
view makes a face smaller in the input, so it is picked up by the FINE 16x16
grid rather than the coarse 8x8 one -- and that tensor's scale is 0.036937 with
zero point 49, giving a ceiling of 2,881/1000 and a threshold step of about
0.037.  Measured after #48, at a normal working distance:

```
peak raw 1292/1000    <- above the 8x8 group's 1224 ceiling, so it is 16x16
score     781/1000    <- sigmoid(1.2919), and it MOVES between faces
```

So the confidence discriminates again.  What follows is what the 8x8 group does
when a face is large enough to reach it, which the old 128x128 centre crop made
the common case:

**Measured before #48.**  A face filling the centre crop is detected by the 8x8
group at its saturated value, so:

```
peak raw 1224/1000    <- (127 - 126) * 1.2247, the largest value that tensor holds
score     775/1000    <- sigmoid(1.2247)
```

and the reported confidence reads **775 for every such detection**, across
different faces and positions.  That is the quantisation ceiling, not a stuck
value: a detection from the 16x16 group can report up to sigmoid(2.88) = 871,
and the boxes themselves move normally.  If a confidence that discriminates
between faces is ever needed, it has to come from a differently quantised
model -- no amount of decoder work can recover a distinction the tensor cannot
represent.

It is also why a decoder that shared one dequantisation constant across the four
tensors would look almost right: the boxes would still be boxes.
`test_blazeface.c` writes the same quantised bytes into both groups and requires
the decoded sizes to differ by the ratio of the scales.

### Limits worth stating with any result

- **Field of view.**  The largest centred square of the frame -- 240x240 at
  +40+0 -- scaled to the model's 128x128 (issue #48).  Every result prints it.
  Until #48 it was a 128x128 CROP at +96+56, a field of view so narrow the
  detector was nearly useless at any normal working distance.
- **The model sees the RAW frame, not the one on the panel.**  The preview
  carries white balance, gamma and saturation tuned by eye for the glass; the
  detector reads the planar B/G/R the datapath wrote.  Same frame, different
  pixels.  Feeding it the gamma-encoded image is plausibly BETTER -- the model
  was trained on sRGB photographs -- but it is a separate experiment, and doing
  it in the same change as the field of view would have left nothing to compare
  against.
- **NMS is capped.**  At most 64 candidates survive to NMS and at most 8
  detections come out.  Both counts are printed, so the cap is never a surprise.
- **The threshold is refused, not clamped, outside 1..999.**  0 and 1000 are the
  poles of the inverse sigmoid; a silently clamped threshold is a setting that
  does not do what it says.

### The resize, and why it is host-tested

`port/npu/nn_preproc.c`: crop the largest centred rectangle with the INPUT's
aspect ratio, then bilinear-scale it in, in fixed point, with no libm and no
vectorisation.  It depends on nothing -- no hardware, no ThreadX, no camera or
NPU singleton -- so `test/test_nn_preproc.c` compiles the real thing and drives
it with synthetic frames.

That is not ceremony.  Every bug this file can have arrives on the board as a
plausible box in the wrong place, and each hypothesis otherwise costs a flash
cycle of a NOR rated ~100k of them.  Three choices are pinned by tests because
all three are invisible by eye:

- **Half-pixel sample centres.**  `src = origin + (dst + 0.5) * extent /
  dst_extent - 0.5`.  An align-corners implementation differs by half a source
  pixel, which against a face is nothing and against a test ramp is exact.
- **Box edges carry NO half-pixel term.**  The `- 0.5` converts a continuous
  coordinate to a sample INDEX; an edge is already continuous.  Applying the
  sampling convention to edges biases every box by half a source pixel.  The
  two mappings are one transform, and `nn detect` now prints boxes in FRAME
  pixels through the same function the overlay draws with -- so the console and
  the panel cannot disagree.
- **Mathematical floor, not a C cast.**  Truncation toward zero is off by one
  for negative coordinates, and the `- 0.5` makes the first destination pixel
  negative on any upscale.

The four taps accumulate in one signed 32-bit accumulator: the Q8 weights are
built as `w1 = f`, `w0 = 256 - f`, so they sum to exactly 256 per axis and the
whole sum is bounded by `255 * 256 * 256` = 2^24.  The bound holds because of
how the weights are constructed, not because the pixels happened to be small.

### Not yet answered

- Whether feeding the model the gamma-encoded preview pixels detects better than
  the raw frame (see the limits above).
- Whether the U55 can reach CM55M TCM.  UNVERIFIED -- the Ethos-U55 TRM does not
  describe this SoC's interconnect and the SVD describes registers, not routing.
  The arena is in SRAM because the donor puts it there, which is evidence and not
  proof.

## Face boxes on the live preview (`nn preview`)

Issue #48.  The camera on the panel with the detections drawn on it:

```
nn open det
nn preview             # Ctrl+C to stop
nn preview 30          # or a frame count
```

`camera preview` is unchanged and still shows a plain picture.

**Measured on the board:** 100 frames in 12,500 ms = **8.0 fps**, with **100
inferences for 100 frames** -- every published frame is inferred and shown, none
dropped and none refused.  Against the 15.8 fps of a plain preview that is about
25 ms per frame for inference (12-13 ms), the resize and the decode together.
The producer thread's stack high-water came out at **964 B of 8,192**: a single
custom-operator graph really does keep the CPU-side call chain shallow, since
the work is in the NPU.  8 KiB stays as the allocation anyway -- the margin is
the point, and the measurement is what says it is not needed rather than an
argument that it might be.

### Inference runs on the camera producer thread

Not on a worker.  The producer already serialises capture -> pack -> blit
(issue #38), and it re-arms WDMA3 only AFTER every sink has consumed -- the SDK
gates the next frame on `sensordplib_retrigger_capture()`.  So the raw frame is
stable for the whole of `consume()`, and the boxes are guaranteed to belong to
the frame being displayed.

A worker thread would decouple the frame rate, at the price of drawing last
frame's boxes on this frame.  That is the harder failure to see: at 8 fps a
one-frame lag looks like a slightly slow tracker, not like a bug.

The order inside `consume()` is the design:

1. **inference, with NO panel guard held.**  It can take the whole NPU timeout
   if an interrupt is lost, and holding the panel across that would block every
   other `lcd` command for the duration.
2. **acquire the panel once**, then stage, draw and present without releasing.
   The overlay hook runs between the staging copy and the DMA
   (`lcd_blit_le_overlay()`), so a box is never half-transferred.

A failed inference means no boxes on that frame, not a blank preview -- the
picture is worth more than the annotation.  `camera stats` counts those frames
separately from sink errors.

### [!] A stop that is not confirmed poisons the camera

`camera_stream_stop()` is documented as synchronous so that callers may detach a
sink afterwards, but its wait is BOUNDED.  Once a sink can run an inference
inside `consume()`, that matters: the ethos-u driver waits
`ETHOSU_SEMAPHORE_WAIT_INFERENCE` and takes the semaphore a SECOND time on its
timeout/interrupt race path, then resets.  A Ctrl+C landing on a lost NPU
interrupt could therefore return `CAM_ERR_TIMEOUT` while the producer was still
inside `consume()` -- and both previews then detached the sink unconditionally,
which is the one thing `frame_pipeline` cannot survive (publish() has already
pre-pinned the sink and called it with the lock released).

The fix is not a bigger number.  A hard timing proof is not available here: the
tails include two NPU soft resets bounded only by iteration counts, the panel's
abort-and-poll recovery, and a camera quiesce that ends in sensor I2C.  A design
whose memory safety rests on arithmetic nobody can complete is wrong the first
time a vendor path is slower than assumed.  So:

- **The join is success-only.**  `CAM_OK` proves the producer is idle; anything
  else proves nothing.  Every caller detaches only on `CAM_OK`.  The same rule
  covers the SETUP path: `camera_stream_start()` returning `CAM_ERR_BUSY` means
  a stream is already running with this sink attached, so that failure does not
  detach either.  (Today the sink's exclusive attach makes that unreachable --
  it is handled because "unreachable" rests on another module's exclusivity
  rather than on anything checked at that line.)
- **`cam_lcd_sink_detach()` clears its overlay only when the unsubscribe
  succeeded.**  A refused unsubscribe means the sink is still attached and a
  `consume()` may still be running in it; mutating overlay state there would be
  the very race the refusal exists to prevent.
- **An unconfirmed join enters `CAM_ST_LOST`**, which refuses every camera entry
  point that touches hardware, for good.  `CAM_ST_FAULTED` could not express
  this -- it is explicitly the recoverable state, and the next bring-up rebuilds
  from it, which is exactly the catastrophic action here.
- **Nothing is detached, torn down or released** on that path, and `nn` stays
  leased.  It is safe only because all overlay and sink state is static.
- `camera stats` still answers (it reads no hardware and takes no mutex), so the
  refusal always comes with an explanation.

The overlay also carries a stop-pending flag, set BEFORE the join is asked for
and checked three times -- before preprocessing, immediately before the invoke,
and after inference before the panel is touched.  It skips work not yet begun,
which is what keeps an ordinary Ctrl+C from ever waiting on the NPU.  It cannot
cancel an invoke already waiting; nothing can.  It narrows the window, and
`CAM_ST_LOST` makes the remainder safe.

The inference timeout came down from 5,000 ticks to **1,000** as part of this,
and now has ONE definition -- `NPU_INFERENCE_TIMEOUT_TICKS` in
`port/npu/npu_hw.h`, which `board.cmake` parses.  It used to be written in both
places with only the CMake literal live, so the header's constant was dead and
would have drifted the first time anyone tuned it.

### The producer thread's stack

Raised from 4 KiB to 8 KiB: it now carries TFLM's `Invoke()`, the ethos-u driver
and the BlazeFace decoder, a call chain that had only ever run on a 4 KiB shell
or background-job stack.  The producer's own measured peak before this was
568 B, so the old allocation was sized for a thread that did almost nothing.
8 KiB is a starting allocation, not a proof -- `thread` reports the high-water
mark, and that is the number to check.

### [!] This is the first configuration that uses five interrupt consumers

Camera (up to 26 lines) + UART0 (1) + LCD SPI/DMA (2) + QSPI and U55 (2) = 31 of
the 32 EPK wrap slots, reaching exactly 32 if the UART's DMA fallback is ever
used.  `nn run` never brings the panel up, so nothing before `nn preview` had
all five at once.

There is no room for an unexpected vendor-enabled line.  The wrap is
fail-closed, so the symptom would be the camera refusing to come up during
`nn preview` rather than silent mis-accounting -- but it is worth running
`thread` once after the first `nn open det`, when IRQ 133 is also present, to
see the cpu% column still reported as trustworthy.

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
- General object detection.  Classification landed in #44, face detection in
  #45 and boxes on the live preview in #48; the camera's output is already in
  the shape the donor's `tflm_yolov8_od` expects, but that model wants a
  1,053 KB arena against the 450 KB reserved here.
- MVE.  [!] The reason it is barred is WRONG: the Armv8-M ARM's PushStack /
  PopStack save and restore VPR under `HaveMve()`, and rule RZWQX makes MVE
  execution set `CONTROL.FPCA`, so the hardware preserves it across a context
  switch and the ThreadX port only has to save the callee-saved s16-s31, which it
  does.  The scan is fail-closed, so nothing unsafe shipped -- but CoreMark and
  the camera's pixel loops are scalar for no reason.  Tracked in issue #42, which
  also covers what removing the scan must not skip (enforcing and reading back
  `FPCCR.ASPEN`, since this app inherits its state from a bootloader).
