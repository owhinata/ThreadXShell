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

### [!] The top 32 KB of AXI-SRAM is the plugin reservation (issue #108)

`.plugin` is a fixed NOLOAD reservation at `0x24048000..0x24050000`, anchored at
the top of AXI-SRAM, for the load image of a model post-processing plugin
(#78; see [Plugin containers](#plugin-containers-issue-108--78-step-3a)).  A
plugin is **prelinked** for that address -- the loader services no relocations --
so it cannot drift the way a sequential section would the day `.bss` grew, and
moving it later invalidates every container built for this board.

**It is a stated exception to the placement policy above.**  That policy keeps
CPU-only *data* out of AXI-SRAM, and it can, because DTCM takes it.  Code cannot
follow: the Cortex-M7 does not fetch instructions from DTCM.  The alternatives
were measured and rejected -- the PSRAM window is execute-never in MPU regions 0
and 3, and ITCM is read-only in region 1, which is the NULL-write guard.
AXI-SRAM needs no MPU change: no region covers it, so the default map
(`PRIVDEFENA`) makes it Normal, cacheable and executable.  When Step 3b copies a
plugin there it must clean the D-cache and invalidate the I-cache over the range
first, because the M7's instruction fetch does not snoop the D-cache.

**The heap's ceiling moved below it.**  `_sbrk` (`src/retarget.c`) used to be
bounded by `__ram_end`, the top of AXI-SRAM -- now inside the reservation.  It is
bounded by `__heap_end` (= the reservation's base) instead, and `__ram_end` keeps
meaning only "where AXI-SRAM ends".  `free` reports AXI-SRAM as two rows, `RAM`
(up to the heap ceiling) and `Plugin` (the reservation), so the reservation is
never counted as spendable headroom.

The address is declared four times, independently, on purpose: the firmware's
linker script, the plugin `MEMORY` fragment (`ldscript/plugin_memory.ld`),
`board.cmake`'s statement to the shared image gate (`WIO_PLUGIN_GATE_BASE/END`),
and `cmake/check_plugin_reservation.py`, which checks the linked image: the
section at exactly that address and size, NOLOAD, nothing else allocated or
named inside it, the heap ceiling at or below its base -- and that the linked
`_sbrk` names that ceiling and no address above it, read from its own
constants, because a correct `__heap_end` beside an `_sbrk` still using
`__ram_end` would pass every symbol check.  Naming a constant is not using it as
the bound, so the behaviour is checked separately: `test/test_sbrk.c` compiles
the real `src/retarget.c` on the host, with `__ram_end` above the ceiling, and
requires the break to stop at the ceiling.  **Do not generate
any of them from another** -- a gate that reads its expected value out of what
it checks passes anything.  `cmake/fixtures/run_reservation_tests.py` watches
each refusal (one shape, a section overlapping the reservation, is refused by
ld itself in a normal link; the fixture records that rather than skipping it).

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

The `.plugin` ASSERTs (issue #108) are the exception that proves the rule: they
name section boundaries (`__ram_seq_end`, `__plugin_start`, `__heap_end`), not
functions, so LTO has nothing to rename.  They are still the linker checking its
own script, which is why `cmake/check_plugin_reservation.py` checks the image as
well.

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
| `null` (+ SD), at #98 | 305,584 B, 77.7% | 87,632 B |
| **`tflm`, SD dropped (the default)**, at #98 | **350,096 B, 89.0%** | **43,120 B** |
| `tflm` + SD, at #98 | 384,572 B, 97.8% | 8,644 B |
| **`tflm`, SD dropped (the default)**, at #108 | **358,664 B, 91.2%** | **34,552 B** |
| `tflm` + SD, at #108 | 393,124 B, 99.98% | **92 B** |

The `tflm` + SD row links and leaves no room to add anything, which is why the
tflm default also turns `BSP_ENABLE_SD` off.  `-DBSP_ENABLE_SD=ON` brings it
back, but wanting the SD back is a reason to audit the space first, not to pass
a flag -- and since #108 the plugin loader of #78 Step 3b will not fit beside
it at all.

[!] The switch only affects NEW build directories.  `CONFIG_NN_BACKEND` is a cache
variable, so a tree already configured as `null` stays `null` until it is
reconfigured -- which is also why the SD default, keyed on `NOT DEFINED
BSP_ENABLE_SD`, does not fire in one of those.  **This bit issue #108**: its
development tree had been configured with SD on, and the plugin work measured as
overflowing the partition when the default configuration had 34 KB free.  Since
then a tflm tree with SD on prints a configure WARNING saying so; reconfigure
with `-DBSP_ENABLE_SD=OFF`.

[!] **The tflm build has not been run on hardware.**  It builds and passes every
gate in both configurations, and `check_cxx_runtime.py` -- which only runs for
tflm -- is now part of the default build.  But no image from this configuration
has been flashed, so the first `--target flash` after this change ships something
new.

## Plugin containers (issues #108, #110 = #78 Steps 3a and 3b)

A **container** carries a model together with the code that interprets its
output (a *plugin*), so a new model family needs no firmware change.  The format,
the validator and the build rules are shared with Grove Vision AI V2 (its README
has the full design); this section is what is different here.

Step 3a delivered, validated and measured without ever executing one.  **Since
issue #110 a container's plugin RUNS**: it decodes, draws on the panel,
describes its result on the console and holds the threshold.  The resident
BlazeFace decoder is still here and still does all of that when no plugin is
loaded -- both arms exist on this board, unlike Grove, where issue #104 removed
the resident one.

`nn model load --slot <n>` reads the blob into the PSRAM staging buffer and
CRC-checks that copy exactly as before, then:

- a payload that is not a container is a bare model and takes the old path
  unchanged (models already in the store do not need re-sending);
- a container goes through `svc/plugin_load.c` -- the same validator the host
  build runs -- with this board's policy, and its **model section is handed to
  the backend in place**, at its offset inside the staged container.  The TFLM
  backend accepts any range inside the staging slot it handed out that starts
  on `NN_MODEL_ALIGN` (16 B); a container's model section is 16-aligned by the
  format's own rule, which `nn_svc_wio.c` asserts meets the backend's;
- the **plugin section is copied into `.plugin`, the caches are made to agree
  about it, the reservation is read back and its entry point is called** --
  but only after the backend has accepted the model.

**[!] The order matters and is not an implementation detail.**  Loading the
plugin first would destroy the previous one's state at the fixed reservation
before knowing whether the model that needs it can be built, and a backend that
then restored the PREVIOUS model would be left with no decoder for it.  So the
previous plugin is untouched while the backend works, and only the outcomes
that changed what is open change what decodes it:

| reload outcome | what happens to the decoder |
|---|---|
| a bare model loaded | whatever was loaded is unloaded -- a new model with an old model's decoder is the accident this ordering prevents |
| a container loaded | its plugin is loaded; if it refuses, the resident decoder reads the model and the loader logs why |
| the previous model was restored | nothing moved, so nothing moves here |
| nothing is open | the plugin is unloaded too |

All of it happens inside the claims window and before the session is given
back, so `nn info` on another console says *a model load is in progress* rather
than pairing one load's model with another's plugin.

`nn info` then shows what the container **claims** (`plugin : <name> (build
<id>, crc <digest>), running` -- or `validated, not loaded` when the plugin was
refused and the resident decoder is reading the model -- `from : slot <n>,
blob '<name>'`,
the image sizes and each slot's declared stack).  The claims follow the model
that is actually open: they are settled only after the backend adopted (or
refused) the model; a refused load that left the previous model active keeps the
previous claims; a bare load and `nn model unload` clear them.  `nn info` takes
no lock (it has to answer while a stream holds the NN session), so between the
backend adopting a model and the claims being settled it says `a model load is
in progress` rather than pairing one model with the other's claims -- and the
`from` line names the blob the claims came with, so a load that lands between
`nn info`'s model line and its plugin line shows up as a mismatch instead of
passing silently.  The *running* word comes out of that same masked section, so
it cannot describe a different load from the manifest printed beside it.

### [!] What decodes, draws and reports, and the lease that separates them

Grove needs no lock around a plugin's private result: its frame pipeline pins
one delivery per sink, so the producer's decode and the panel's draw
structurally cannot overlap.  **This board has no such exclusion.** The preview
(flip) thread runs at priority 12 and the inference worker at 18, with nothing
between them -- the worker is preemptible mid-decode and the panel is what
preempts it.  That was harmless while the worker handed over BOXES: it filled a
local array, took the detection mutex, copied and released, so a reader saw
either the old decode or the new one.  A plugin's result is private and stays
inside the plugin, so there is nothing to copy.

`port/plugin/plugin_lease.c` is the answer, and the rules are:

- **the order is always the lease, then the frame lock.**  The panel takes it
  first and the frame lock second; the worker takes it and then the detection
  mutex.  Nothing takes them the other way round;
- **the worker holds it across the decode AND the publish.**  Releasing between
  them would let the panel see new private state beside an old count;
- **the panel does not wait.**  A draw that blocked would hold a lock wider
  than what it protects for as long as a decode takes.  On a refusal it
  presents the picture unannotated -- the failure this pipeline already has for
  a `process()` that declines -- and the refusal is COUNTED, because the
  existing preview counters see a frame that was presented, not one presented
  bare;
- **a console asking for a report waits, but not forever.**  Measuring how long
  a wait took is not the same as bounding it.

One decoder annotates a frame, never two: with a plugin loaded the resident
overlay does not run.

### [!] Reporting: captured where the result still exists

`nn run` and `nn dets` print a plugin's account of its result from bytes
captured in the same breath as the snapshot, under the lease.  They do not ask
the plugin afterwards, because by then the session has been released and
another console may have replaced it -- the bytes would not have to describe
the count printed beside them.

**The capture buffer belongs to the shell command's own frame.**  A board-owned
slot does not fix that window, it moves it: console A captures, console B
captures over the top, A prints B's bytes.  Repairing that needs a reservation
with a lock order and a release on every path including a cancelled command;
putting the bytes where the caller already keeps its snapshot makes the
lifetime the C call stack instead.  512 B, and truncation is reported rather
than hidden -- as are "this decoder has no report to give", "it stopped part
way" and "it could not be reached in time", because length is not a status.

### [!] The painter draws with the CPU, fills included

The panel's own `ltdc_fill_rect()` runs on DMA2D and a plugin's `draw()` must
not.  The frame transaction ends when `ltdc_flip()` presents, and this port
cannot establish that an outstanding DMA2D transfer has stopped writing before
then: the small-transfer path polls with a HAL call that returns WITHOUT
aborting on timeout, `ltdc_dma2d_fill()` discards that result and
`ltdc_fill_rect()` returns void.  A timed-out fill can therefore still be in
flight when its destination becomes the displayed buffer, and suppressing that
one flip does not help because the next producer frame reuses the buffer.
**Repairing that is the port's own problem and its own issue.**  A painter that
never arms a transfer does not have it, and the rotating blit this panel
already uses writes the same framebuffer with the CPU, so this is an access the
panel already performs.

The budget is a quarter of the surface (19,200 pixels) plus 64 dispatches,
charged BEFORE the framebuffer is touched so a refused primitive leaves nothing
half-drawn.  An outline is charged for the pixels it WRITES, not the area it
encloses -- by area a 200x200 box costs 40,000 and vanishes silently, which is
how issue #105 found the rule; it actually costs 1,584.  **That cap is an
initial test ceiling, not a shipping one**: these pixels are CPU stores into
non-cacheable external PSRAM, and a pixel budget is not a bound on hold time.
See the acceptance criteria below.

### The target word

The plugin target word for this board is `0x1201` (Cortex-M7 / FPv5-D16 / hard
float / little-endian, no CMSE).  Until #108 nothing checked such a word on any
board -- the packer, the verifier and the firmware all read one CMake variable.
It is now checked at both ends: `nn_svc_wio.c` static-asserts it against this
firmware's own predefined macros (`svc/plugin_target.h`), and the shared image
gate derives the core, FPU, float ABI and endianness from each plugin ELF.
[!] An M7 object does not record its core's name -- GCC's `.arch armv7e-m`
overrides `.cpu cortex-m7`, leaving `Tag_CPU_name "7E-M"`, the same as a
Cortex-M4 -- so the gate knows an M7 by v7E-M **plus** an FPv5 FPU, which only
the M7 has.

### Sending an asset

```bash
cmake --build build/wio-lite-ai --target asset-blazeface
picocom -b 115200 --send-cmd "sb -k" --receive-cmd "rb" /dev/ttyACM0
```

The target fetches the pinned model (the same commit and SHA-256 Grove uses),
checks it with this board's `verify_tflite` (the operators THIS firmware
registers), packs it with the M7 blazeface plugin, runs the device's validator
over the packed file, and publishes only then.  It prints a receipt:

```
  on the board:  blob write 5   (erases the slot, then waits for the file)
  in picocom:    C-a C-s, then the path above
  afterwards:    `blob list` must show crc32 <CRC> -- ...
  then:          nn model load --slot 5
```

**Slot 5 is only the declaration in `board.cmake`.**  The build refuses a
container too large for it; nothing refuses the *wrong* slot, and `blob write`
erases the whole slot before the transfer.  Read `blob list` first.

The ingest is this board's: float32 I/O as the model zoo ships it (the shared
decoder reads float32), no boundary strip and no vela -- there is no NPU here.
No cifar10 asset exists yet (no CIFAR-10 model is established on this board);
the cifar10 plugin is built for M7 anyway, to prove the build and measure it.

### [!] What still catches what

The gates run at **build** time.  `sb -k` sends whatever path is typed, so
nothing checks that the file sent is the file built:

| what you could send by mistake | what catches it | when |
|---|---|---|
| a container built for another board | `plugin_load.c` (target word) | `nn model load` |
| a recognised container with malformed internals or a damaged plugin section | `plugin_load.c` | `nn model load` |
| a container whose magic is damaged | probes as unknown and falls through as a bare model, so the TFLM backend -- **not** `plugin_load.c` | `nn model load` |
| a malformed `.tflite` (bare, or as a container's model section) | the backend's parse only; `VerifyModelBuffer()` runs on the board only with `NN_TFLM_VERIFY=ON` (off by default) | `nn model load` |
| a stale artifact, or a model that never passed `verify_tflite` | **nothing** | -- |
| the wrong slot | **nothing** -- and the slot's previous contents are gone | `blob write` |

The **receipt CRC compared with `blob list`** is the one "built bytes = stored
bytes" check.  `nn info`'s CRC is the plugin-section digest and does not move
when a model changes.

### Measured sizes

| | measured |
|---|---|
| blazeface plugin (M7) | text 4,016 B, bss 4,816 B, load image 8,832 B; requirement with #110's veneer charge: decode 648, draw 588 B |
| cifar10 plugin (M7) | text 2,704 B, bss 8,852 B, load image 11,584 B; requirement: decode 688, draw 548, report 592 B |
| blazeface container | 194,120 B (model 189,816 B at +4,304) |
| `.plugin` reservation | 32 KB -- 2.8x the larger load image (cifar10's 11,584 B) |
| app flash, default build (SD off) | 363,620 B of 384 KB (92.5%), 29,596 B free -- #110's wiring cost 4,628 B over #108 |
| AXI-SRAM heap room | 103,200 B (`end` to `__heap_end`, SD off) |
| DTCM ceiling for growing `nn_work` | **4,544 B** (`_smsp_stack - _dtcm_used_end`; the 8 KB main-stack reservation is not free) |
| stack already spent at the decode / draw call sites | **609 B of 3,072 on `nn_work`** (decode), **105 B of 1,024 on `cam_prev`** (draw), measured on hardware at #108 -- see the caveat below |

The call-site depths are measured where a plugin will stand: on `nn_work`
immediately before the resident decoder is called, and on the preview thread
inside the frame lock just before the overlay is drawn (not in the per-box
helper, which is deeper than a plugin is ever called from).  There is a third
site since #110, on the shell thread, where four of the seven slots are
actually called.  The probe is out of line, so each figure includes its own few
bytes -- an over-report, the safe direction -- and it sits AT the call rather
than in the caller, so the number does not depend on whether the caller was
inlined this month.

**[!] The two figures above predate #110's call sites.**  They were taken when
nothing was called there; re-measure them.

### The stack budget, and what is still owed to it (issue #110)

There are **two quantities**, and Step 3a only had one of them:

1. what is free at the call site -- the probes above;
2. what the FIRMWARE spends below an outbound plugin veneer, which the image
   gate charges at every crossing because it cannot see across one.

The second was Grove's 256 B, carried over as an admitted placeholder.  It is
now derived from the base this board actually has, measured with
`-fstack-usage` over its callbacks and summed along the deepest chain a veneer
can reach:

```
nn_plugin_log 16 + log_write 16 + log_vwrite 192
  + fmt_vsnformat 32 + fmt_vformat 80 + fmt_utoa 64        = 400 B
```

(`fmt_utoa` and `fmt_padded` are called in sequence, not nested.  The painter
is far shallower -- `paint_rect` 64 + `rect_geom_norm` 16 -- and `to_frame` and
the report sink are leaves.)  **512 is declared**, because the measurement is
per-TU with LTO off and over-estimating is the safe direction here: the gate
charges it at every crossing, so a larger number makes a plugin's computed
requirement larger, not smaller.  **Re-derive it whenever the base gains a
callback** -- the old number would still pass, which is the shape of the
mistake it replaces.

With the real charge, the recomputed requirements are:

| | decode | draw | report |
|---|---:|---:|---:|
| blazeface | 648 B | 588 B | -- |
| cifar10 | 688 B | 548 B | 592 B |

**[!] Which thread each slot is called on was wrong until #110.**  Step 3a
declared the WORKER's allowance for `entry` and `shapes_ok`, on the reasoning
that a decoder's callbacks belong to the decoding thread.  They do not: `entry`
is called from `nn model load` and `shapes_ok` from the admission both `nn run`
and `nn stream start` pass through, and both are console commands.  A bound on
the wrong thread's stack is not a bound.  Four of the seven slots are on the
shell thread; `NNCAM_SITE_SHELL` measures it and `nn stream stats` prints it.

**[!] And the panel thread got a bigger stack rather than a smaller
allowance.**  `cam_prev` was 1,024 B when it did nothing but flip buffers and
draw rectangles.  At that size the derived allowance is
`1024 - 105 - 208 = 711`, and the detector's 588 fits by a margin thin enough
that any plugin with a label to draw would not.  Sizing an allowance to what
today's plugin happens to need is how a limit stops being one, so the thread is
1,536 B now -- 512 B out of the 4,544 DTCM has spare -- and the allowances are:

| thread | stack | at call | reserve | derived room | declared |
|---|---:|---:|---:|---:|---:|
| `nn_work` (decode) | 3,072 | 609 | 208 | 2,255 | 1,024 |
| `cam_prev` (draw) | 1,536 | 105 | 208 | 1,223 | 1,024 |
| shell (entry / shapes / report / params) | 4,096 | *to measure* | 208 | -- | 1,024 |

**[!] STILL OWED, and the biggest remaining way for this to fail open:** the
shell figure has no measurement behind it, the two that do were taken before
the call sites moved and before `nn run` started carrying a 512 B capture
buffer in the same frame, and **the container already stored in slot 5 has a
manifest packed with the OLD veneer cost** -- its declared stacks are smaller
than the requirements above.  Rebuild the asset and send it again; do not read
"it was accepted before" as "it still bounds execution".

### Acceptance criteria (to be met before this is called done)

A counter that goes up with no threshold beside it does not establish that
anything works.  Before the hardware run, freeze: the workload, how long it
runs, the acceptable overlay-miss fraction and maximum consecutive run, the
maximum draw and frame-transaction time, and the maximum lease wait a console
sees.  Require zero new DCMI errors, LTDC underruns or band corruption
attributable to the plugin, against a baseline taken with the resident decoder.
Define the miss denominator too, and count a refused or partly drawn overlay as
a miss, not a success -- and require useful progress (presented frames,
inferences, successful overlay opportunities), because a decode that stalls
improves the miss fraction.

A failed run does not become a pass by relaxing a threshold afterwards.

DTCM is the ceiling if 3b has to grow `nn_work`: `free` reports 12,736 B free,
but 8,192 of that is the main stack's reservation at the top of the region.
What the linker actually enforces is `_dtcm_used_end <= _smsp_stack`, which is
**4,544 B**.

### What the first hardware run established

The container path was exercised against the model this board was already
running, in slot 4, and two things came out of it:

- **The models are the same bytes.**  Slot 4's stored CRC-32 is `3A210F57`,
  which is exactly what `verify_tflite` reports for the pinned file this build
  fetches (189,816 B).  The plan deliberately did not claim that -- no size or
  hash for the board's existing model existed anywhere in the tree -- and
  planned a differential check ("both must find the same face") instead.  The
  blob store's own CRC turned out to record the identity all along, so the
  weaker check is not needed: the bare path and the container path run the same
  model, and the differences between two `nn run` outputs are just different
  camera frames.
- **The container costs 4,304 B of slot and nothing else.**  Loading it
  reported the same tensors, the same 470,352 B arena and the same threshold as
  the bare model, and `nn info`'s plugin lines match what the host's
  `verify_container` printed for the same file, field for field.

One operating note that is not a fault: `camera preview on` refuses with "the
display is down or its scanout is off" until `lcd on` has run.

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
