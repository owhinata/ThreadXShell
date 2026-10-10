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
the submodules this board needs (`boards/f746g-disco/submodules.cmake`).  The
default NN backend (`tflm`) also fetches tflite-micro at configure time and the
pinned model at build time; `-DCONFIG_NN_BACKEND=null` builds offline (see
[Build options](#build-options)).

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
| `CONFIG_NN_BACKEND` | `tflm` | `tflm` / `null` / `stedgeai` / `stedgeai_reloc` |
| `NN_TFLM_MODEL_OVERRIDE` | empty | a local `.tflite` baked in instead of the pinned model (tflm only) |

### The NN backend: `tflm` by default (issue #130)

`tflm` runs tflite-micro with CMSIS-NN kernels and bakes one model into Flash
as the built-in. That model is the same pinned file wio-lite-ai and
grove-vision-ai-v2 ship as their `blazeface` asset: BlazeFace front 128 int8
from the ST model zoo (`STMicroelectronics/stm32ai-modelzoo`, commit
`1423c78953a830903485135febe1dd98ff31aed8`,
`face_detection/facedetect_front/.../blazeface_front_128_int8.tflite`, SHA256
`e803bb4e93b10f7a19d4243bcc39698599a723f3128e19d3f90e0b1c0bc88dd8`, 189,816 B). It is fetched at BUILD time by the shared
`cmake/fetch_model.cmake` into `build/<dir>/tflm-model/pinned.tflite`, and the
C array is generated from that fetched file, so a pin that can no longer be
fetched -- a vanished commit, a Git LFS pointer instead of the file, different
bytes -- fails the build rather than shipping an older array.  The hash is
checked again at use: `cmake/gen_model_array.py` hashes the exact bytes it
emits and refuses (leaving no array behind) if the file changed after the
fetch verified it.

[!] **The default build needs the network**, once per build directory: for the
tflite-micro tree at configure time (this was already true of any tflm build)
and for the model at build time. This is a deliberate difference from the
`asset-*` rule (issue #94), whose fetch stays outside ALL because a container
is not part of the firmware; here the model is. A `null` build needs neither and
configures and builds offline.

To try another model, pass `-DNN_TFLM_MODEL_OVERRIDE=<path to .tflite>` (int8
weights, float32 I/O). An override is not hash-checked. The old variable,
`NN_TFLM_MODEL`, is IGNORED with a configure warning: build trees from before
#130 hold it pointing into the git-ignored `_ref/` tree, and reading it would
keep building from a file no clone has. `-UNN_TFLM_MODEL` silences the warning.

`null` stays available: no runtime, a BlazeFace-shaped stub whose inference
does nothing, and no network or C++ needed. It was the default until #130
(issue #98 kept it because `tflm` then needed a `.tflite` the repo does not
ship; the pin removes that reason). `stedgeai` and `stedgeai_reloc` still need a
local ST Edge AI Core install (ST-SLA) and do not configure in a clean tree.

[!] **A changed default does not reach an existing build directory.** CMake
keeps the cached `CONFIG_NN_BACKEND`, so a tree first configured while `null`
was the default stays `null`. Such a tree prints a warning on its first
configure after #130, and every `null` configure prints a status line. To move
it: `cmake -B <dir> -DCONFIG_NN_BACKEND=tflm`, or use a fresh build directory.
A fresh `-DCONFIG_NN_BACKEND=null` is not warned about.

Measured on the default (`tflm`, CMSIS-NN) build at #130, fresh configure:

| | null | tflm |
|---|---|---|
| `shell.bin` | 402,932 B | 777,280 B (74.1% of 1 MB) |
| `size` text / data | 398,560 / 4,360 | 772,900 / 4,372 |

Of the 374 KB difference, 189,816 B is the model array (`.rodata`); the rest
is the interpreter, the kernels and the C++ runtime.

SDRAM bank3 (`.sdram.ai`, `0xC0600000`, 2 MB) under `tflm`, after #130 step 6b
removed the camera staging buffers (`nncam_stage`, 393,216 B):

| Object | Address | Size | Owner |
|---|---|---|---|
| `nn_dec_scratch` | `0xC0600000` | 1,536 B | `port/nn/nn_decoder.c` |
| `g_sd_model_buf` | `0xC0600600` | 1,048,576 B (2 x 512 KB) | `port/nn/tflm/nn_tflm.cc`, SD model slots |
| `g_arena` | `0xC0700600` | 524,288 B | `port/nn/tflm/nn_tflm.cc`, activation arena (holds the input tensor) |
| free | `0xC0780600` | 522,752 B | |

`check_f746_layout.py` requires `g_arena` and `g_sd_model_buf` in bank3 in a
`tflm` build only (they are in an anonymous namespace, so board.cmake names them
by their mangled local symbols). A `null` build is not asked for them. The
negative tests are `cmake/fixtures/run_layout_tests.py`, run by the host test
suite.

**What the camera producer writes is covered by the same requires.** Since #130
the producer preprocesses each frame straight into the model's input tensor,
and that tensor is not an object of its own: TFLM allocates it inside `g_arena`
(and the `null` backend's is `null_in_buf`, required in a `null` build). So the
`g_arena` require is also the placement guarantee for the producer's writes --
bank3, CPU-only, never a DMA target. There was no require for the removed
`nncam_stage`, so no gate changed.

#### Hardware baseline for the tflm default (issue #130)

Measured on the board on 2026-10-04 with firmware `5e676ec-dirty` (the #130
step T working tree): the default `tflm` build above, built-in BlazeFace pin,
216 MHz. This log is the reference the later f746 steps of #130 (the shared
stream lifecycle, the producer writing the input tensor directly, and the
per-board error classification) are compared against; the Flash and bank3
figures are the tables above.

| Measurement | Value |
|---|---|
| `nn bench 10` | min 628,234 / avg 631,212 / max 633,281 us |
| `nn stream start --frames 300` | 300 frames in, 266 skipped, 0 errors; 30 inferences in 21,036 ms = 1.42 inf/s; latency 659,658 us (last) |
| `nn info` arena line | `470352 B reserved` -- [!] labelled "reserved" but it is the arena TFLM actually USES; the reservation is `g_arena`, 524,288 B (to be corrected under #131) |

Stack high-water marks from `thread` after `nn bench 10`, two `nn run` and the
300-frame stream (`peak` / size):

| Thread | Before any inference | After | Notes |
|---|---|---|---|
| `nn-worker` | (not created yet) | 1,808 / 4,096 B (44%) | `NNCAM_WORKER_STACK` stays 4096, ~2.3x margin |
| `cli` | 1,724 / 4,096 B | 2,156 / 4,096 B (52%) | `nn bench` infers on the console thread |
| `cam_prod` | 364 / 1,024 B | 580 / 1,024 B (56%) | the rise is the preprocessing step |
| `GUIX System Thread` | 816 / 4,096 B | 816 / 4,096 B | |

After #130 step 6b (the producer preprocesses straight into the input tensor;
measured 2026-10-10, firmware `34422df-dirty`, same procedure, with the #137
overrun resync in place):

| Measurement | Value |
|---|---|
| `nn bench 10` | min 623,232 / avg 629,820 / max 632,799 us (unchanged) |
| `nn stream start --frames 300` | 300 in, 268 skipped, 0 errors; 32 inferences in 21,523 ms = 1.48 inf/s; latency 636,898 us -- `in - skipped` now equals the inferences |
| `nn-worker` peak | 1,512 / 4,096 B (36%): the frame copy is gone |
| `cam_prod` peak | 508 / 1,024 B (49%) before and after the stream |
| `camera stream stats` after 145 s | 12 DCMI overruns (3 of them at 15.9-16.4 s after boot), all resynced in place, `ovr ring` 0, base capture never stopped; `dma fe/s` about 2,350 (up from about 1,500: the producer now writes the input tensor in bank3 as well) |

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

### [!] Three interrupt handlers have to be STRONG symbols

`PendSV_Handler`, `SysTick_Handler` and `USART1_IRQHandler`.  The stock CMSIS
startup supplies all three as `.weak` aliases of `Default_Handler`, an infinite
loop -- so losing an implementation still links, `nm` still shows the name
defined, and the vector table still points at something.  The board stops
scheduling, or the console stops answering, with no build diagnostic at all.
`check_f746_layout.py` therefore tests three things per handler: the symbol is a
strong `T`, its address is NOT `Default_Handler`'s, and the matching
`.isr_vector` slot holds that address.

[!] The ASSERTs are weaker than they look for `.sdram.ai` in particular: they
bound where the section STARTS, so an EMPTY one satisfies all of them, and the
section uses `KEEP` so `--gc-sections` cannot even produce the "no such object"
hint the other boards get.  Until issue #97 nothing named the BlazeFace decoder's
candidate scratch, and dropping its section attribute would have moved it into
internal SRAM with every check still green.  `check_f746_layout.py` now requires
`nn_dec_scratch` unconditionally -- unconditionally because the decoder is
compiled in every `CONFIG_NN_BACKEND` configuration.

A third gate arrived with it, and it protects the arrangement that made the
require possible.  The decoder itself is shared by all three boards
(`svc/blazeface.c`) and owns NO storage: this board passes in its own scratch,
which is how that buffer keeps `.sdram.ai` and how the layout gate can name a
symbol this board owns (`port/nn/nn_decoder.c`).
`cmake/check_no_mutable_storage.py` refuses a static added to the shared file, by
compiling it with this board's real definitions and requiring the object to have
no allocated, writable section.  Only the SCRATCH is placed -- the decoder's
threshold stays in ordinary internal RAM, because `.sdram` is NOLOAD and an
initialised field there would never be loaded (and NOLOAD keeps the previous
run's bytes, so it would fail by appearing to work).

Since issue #117, coverage is derived across the actual `svc_obj` and `shell`
producers. `cmd_nn.c`, `nn_cmd_core.c` and the resident decoder compile in
`shell`. Each audit replays its producer's compile command. The resident
decoder remains covered in every backend configuration. See the
[shared storage gate contract](../../cmake/README.md).

## Commands

```
camera    console   coremark  crash     devmem    dmesg
echo      free      fs        gui       help      jobs
kill      lcd       membench  net       nn        qspi
reboot    sd        sdram     sleep     thread    touch
uptime    usleep    version   watch     wdt       xfer
```

`help` lists them with one-line descriptions; `help <cmd>` and
`help <cmd> <sub>` walk the tree.

Subsystems behind them: LTDC + GUIX (`gui`, `lcd`), OV5640 over DCMI
(`camera`), QSPI NOR with LevelX + FileX (`fs`, `qspi`), microSD (`sd`),
FT5336 touch (`touch`), Ethernet with NetX Duo (`net`), the NN backends (`nn`),
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

The GUI preview, `nn stream` and `net mjpeg` are all *subscribers* of one base
capture.  When the LCD comes up at boot, the GUI preview subscribes and starts
the base capture once (`ui/guix_camera_ui.c`, on the GUIX thread), so on a board
with a working sensor the base is already running after boot: `camera stream
start` then answers `busy (streaming or preview active)`, and `nn run` / `nn
stream start` need no start of their own.

> **`nn stream` is a subscriber, and `--frames` waits on that** (issue #99).
> `nn stream start` enables inference whether or not the base capture is running:
> with it stopped the stream stays enabled and idle, and attaches at the next
> `camera stream start`. So `nn stream start --frames <n>` waits indefinitely
> until the base is started, which is not a hung command. The start says so when
> the base is not running. (Issue #99 also removed the `[qqvga|qvga]` word this
> command used to take -- the port discarded it and derived the geometry from
> whatever the base publishes, so it had not selected anything for some time.)
>
> **The last result outlives the session** (issue #118). `nn dets` reads the
> last published decode after a `nn run`, during a stream and after its stop;
> only a `nn model load` that changed what is open (a new model, or a refusal
> whose rollback left nothing -- a refusal that kept the previous model keeps its
> result; the reload reports which, issue #122 P1) and `nn model unload` clear it. `nn run` waits on
> the record's accepted-publish count, not the worker's inference counter, which
> this board bumps before the decode is published. `nn stream stats` reads
> `infers` and `last` in separate critical sections, so on a running stream the
> two can be one frame apart. The GUI boxes are drawn only
> from a result of the session in force, so they still clear on `nn stream stop`.
> For a model the decoder does not recognise, the worker takes the top 5 classes
> of output 0 before its next inference and publishes them with the result
> (issue #121, `svc/nn_top.c`); `nn run` and `nn dets` print those, never the
> output tensor as it is at print time.
>
> **A frame that arrives during an inference is skipped, not staged** (issue
> #130 step 6b). The camera producer preprocesses a frame straight into the
> model's input tensor, and only while the worker wants one; the shared
> hand-over word (`svc/nn_handoff.h`, stepped by `svc/nn_core_frame.c`, the same
> one grove-vision-ai-v2 and wio-lite-ai use) says who owns the input. In `nn
> stream stats`, `frames in` counts every frame offered, `skipped` the ones that
> arrived while the worker was busy (or before it armed), and the rest were
> handed over and inferred -- so `in - skipped` equals the inferences, give or
> take the one in flight when the stream stopped, and less any inference whose
> `nn_run()` failed (that one is counted in `errors`, not in `infers`). A frame
> whose preparation was abandoned is in neither `in` nor `skipped`: that
> happens when its session ended part way through (a base detach or a stop
> moved the epoch) -- such a frame belongs to no session, which is how the
> staging buffers treated it too -- or when the input tensor was missing,
> which is counted in `errors`. Before step 6b the producer
> filled two SDRAM staging buffers and the worker copied one into the input:
> frames kept arriving into a free buffer during an inference, so `in -
> skipped` ran ahead of the inferences (34 vs 30 in the baseline below) and
> the frame inferred had usually sat in its buffer for most of an inference.

Stopping one of them detaches its sink while the base keeps running --
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
| `nn stream stop` | `camera has not released the inference frame` | `nn stream start` is refused until a later `nn stream stop` finds it clear.  `nn run` reports the same condition rather than swallowing it (issue #50). |
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

### A DCMI overrun is resynced in place, not torn down (issue #137)

The DCMI FIFO overruns when a frame start arrives while the DMA cannot reach
SDRAM in time -- a GUIX full-screen DMA2D repaint lined up with the frame start
is enough, and moving code around shifts that alignment.  RM0385 17.3.10: the
DCMI resets its FIFO and waits for the next frame start, so the capture
survives; only the DMA's position inside the frame is lost.

For the raster base stream the producer thread therefore restarts DCMI + DMA in
place (`cam_stream_resync()` in `port/camera/camera.c`), under the camera lock
with the DCMI and DMA2 Stream1 interrupts masked at the NVIC: stop CAPTURE and
wait for it to read 0, make sure the stream's EN bit is 0, classify, empty the
DCMI FIFO, re-initialise the DMA stream, and restart double buffering on the
same two ring slots.  Nothing is published from the interrupted frame and no
subscriber sees a close()/open(), so the GUI preview does not blink.  Each
overrun costs at least 1-2 frames (the one being captured, plus a completed one
the producer had not serviced yet; more if the producer runs late) and logs one
line,
`DCMI overrun: capture re-armed in place (ovr dcmi=<n>)`.

`ovr dcmi` in `camera stream stats` counts overruns: one per in-place resync
(however many OVR interrupts the DCMI raised before the producer got to it),
plus the overruns that ended the stream.  A DMA transfer error or a DCMI sync
error that ends the stream is not an overrun and is not counted.  It restarts from 0 with every stream
start, an auto-recovery included.

These are still terminal -- the stream tears down (`state: stopped (overrun)`)
and the escalating auto-recovery of owhinata/stm32f746g-disco#100 takes over:

- an overrun on the JPEG stream (snapshot-per-frame, not this path);
- a DMA transfer error, or a DCMI synchronisation error or stop timeout, seen
  either in the HAL error codes or in the raw DMA / DCMI flags;
- an error indication without an overrun in it;
- CAPTURE not clearing within 500 ms, EN not clearing within 10 ms, the FIFO not
  draining, or a DMA re-init / restart failure;
- the 30th resync in a row with no frame published between them (so at most 29
  in-place restarts, about one second at 30 fps) -- a persistent overrun falls
  back to the escalating path rather than resyncing forever.

The terminals decided inside the resync (the last two items, and an error code
or raw flag found there) log `DCMI overrun: in-place resync failed (<reason>);
stopping` first.  Those decided in the error callbacks -- a JPEG-stream overrun,
a DMA transfer error, a DCMI sync error -- do not.
A stop, `--frames` or `--secs` that is due by the time the teardown decides
wins: the stream is not auto-recovered, even when a resync (or any other
terminal error, JPEG included) failed after the target was reached.  A terminal
error that was latched still shows as `stopped (overrun)` in `stats`.

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
