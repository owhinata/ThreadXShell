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
- **no interrupt other than the accounted one is enabled at the NVIC** -- an
  unwrapped ISR does not corrupt anything, it just bills its own runtime to
  whichever thread it interrupted, which is exactly the misattribution this
  machinery exists to prevent;
- the profile kit's ISR nesting counter is zero when read from a thread, i.e.
  every enter had its exit.

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
