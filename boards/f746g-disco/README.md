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
default NN backend (`tflm`) also fetches the `lib/tflite-micro` submodule on
first use, generates the tflite-micro tree from it at configure time (which
downloads its third-party dependencies) and fetches the pinned model at build
time; `-DCONFIG_NN_BACKEND=null` touches none of these and builds offline (see
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
tflite-micro tree at configure time (this was already true of any tflm build;
since issue #134 tflite-micro itself is the `lib/tflite-micro` submodule, which
the tflm configure initialises on first use, and generating the tree still
downloads its third-party dependencies) and for the model at build time. This is a deliberate difference from the
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

### The nn files

The command, its contract and the shared core are `svc/`'s (see
[svc/README.md](../../svc/README.md)).  What this board still owns, one line per
file (each `.c` with its `.h` unless only a header is listed):

| File | Thread or context | Hardware reason |
|---|---|---|
| `port/nn/nn_svc_f746.c` | console | The thin adapter over this board's `nn.h` API; it adds no state. |
| `port/nn/nn_svc_f746_model.c` | console (`nn info`, `nn model load` / `unload`) | The steps of the shared load order on this board: the built-in model in Flash and the SD-card model slots in SDRAM. |
| `port/nn/nn_camera.c`, `nn_camera_priv.h` | console (start / stop / stats / record reads) and the camera sink's open / close of the nn step | Binds the shared frame path to this board's camera as the nn step of the one camera sink, and holds nn's place among the camera's holders (issue #142); owns the glue's state (the private header lists each counter's writers and resets) and the frame path's ops table. |
| `port/nn/nn_producer.c` | camera producer (the nn step of the camera sink's `consume()`) | Prepares a frame straight into the model's input tensor (nearest-neighbour resize + RGB565 conversion on the CPU) and hands it over; it does not put the frame -- the sink does, last. |
| `port/camera/cam_sink.c` | camera producer (`consume()`), console (step enable / disable / drain) | Not an nn file, but nn runs inside it: the one RGB565 subscriber of the base capture, running the GUIX display step and then the nn step, with `camera_frame_put()` last. Each step's armed mark and busy count are `svc/camera/cam_step.h`'s. |
| `port/nn/nn_worker.c` | inference worker (priority 18) | Inference runs on the CPU, so the worker sits below every other thread; it runs the model, the resident decoder's publish and the counting. |
| `port/nn/nn_decoder.c` | inference worker | Resident BlazeFace decoder state, with its candidate scratch placed in `.sdram.ai`, and `nn_tensor` -> `tensor_desc`. |
| `port/nn/nn_decode_count.c` | inference worker | No hardware reason: this board's table for counting the decoder's answer in `nn stream stats` (pure, host-tested). |
| `port/nn/nn_sess_release.c` | console (stop), inference worker | No hardware reason: when the stream's hold on the nn session may be given back, as a pure function of the sink drain and the worker. |
| `port/nn/nn.c`, `nn_backend.h` | worker (run), console (open / bench / info) | The backend-agnostic model API and session guard, with DWT cycle-counter timing around `nn_run()`. |
| `port/nn/nn_null.c` | as `nn.c` | The `null` backend: a BlazeFace-shaped stub with no runtime. |
| `port/nn/nn_stedgeai.c`, `nn_stedgeai_reloc.c` | as `nn.c` | The non-default ST Edge AI Core backends (a model baked into Flash, or a relocatable one); they need a local ST install. |
| `port/nn/tflm/` | worker (invoke), console (load) | The default tflite-micro backend with CMSIS-NN kernels; arena and SD model slots in SDRAM. |

There is no `nn_panel.c` here: this board has no panel thread.  The GUIX thread
draws the boxes itself, reading the record through `nn_camera_dets_get()`
(`ui/guix_camera_ui.c`).

### The model: built in, loaded from SD, or nothing (issue #131)

The backend can hold no model at all, and `nn model unload` leaves it that way:
`nn info` then prints `model : (none)` and, in place of the tensor lines,
`note : nothing loaded -- `nn model load ...``; `nn run`, `nn stream start` and
`nn bench` are refused (`nn bench` with `no model is loaded`). Until #131 the
`tflm` unload went back to the built-in model, so the board could never be
emptied.

- [!] **The built-in model is adopted implicitly once, by the first open after
  boot** -- whichever of `nn info`, `nn run`, `nn bench`, `nn model load` or
  `nn model unload` comes first. After that, nothing puts it back except an
  explicit `nn model load builtin`: an open, an `nn info` from the other
  console or an `nn run` on an empty backend builds nothing. The first open is
  serialised by a latch and does not return until the build is done, so an
  unload typed right after boot -- even while the other console's `nn info` is
  doing that first open -- empties a finished model.
- A refused `nn model load --path` restores the previous model; from empty it
  stays empty and says `nothing is loaded`. The answer comes from the reload
  itself (whether a model is left), not from whether the handle is open.
- `stedgeai_reloc` has no built-in model: it starts empty, and `nn model load
  builtin` is refused as a source this backend does not have (it used to mean
  "unload"; use `nn model unload`). Its `nn info` shows `(none)` and the note
  after boot and after an unload.
- `null` and `stedgeai` cannot swap models: `nn model load` is unsupported and
  `nn model unload` answers as before without changing anything.

Since #131 step 7d the order of a load and an unload is the one every board
shares (`svc/nn/nn_core_model.c`, walked by `shell/test/test_nn_core_model.c`);
this board's half is `port/nn/nn_svc_f746_model.c`, and
`test/test_nn_model_life.c` builds it against the real `nn.c` over a stub
backend. What that changed here:

- [!] **`nn info` from the other console while a load or an unload is
  replacing the model answers busy** on the model, `used` and tensor lines
  (`-- another nn command is using the model; ask again when it returns`),
  instead of copying a name and a size that may come from
  two different models. The same transition counter as wio-lite-ai and
  grove-vision-ai-v2: it is odd only while the backend is being changed --
  the SD read happens before it -- and a stream, which holds the NN session
  but never moves it, still gets the full report. Not exercised on hardware:
  the two-console race cannot be hit by typing, so
  `test/test_nn_model_life.c` holds it (its stub reload asks `nn info` in the
  middle of the swap).
- Whether a model was open is read under the NN session, so a backend that
  says it restored a model when none was open ends `nothing is loaded` and is
  emptied, rather than `the previous model is still active`. Every load that
  ends with no model empties the backend explicitly.
- A reload that returns success with no model left (unreachable on these
  backends) is reported as a failure, not `loaded`.
- Statuses and wording are otherwise unchanged; the refusals before the
  session (stream running, SDRAM down, the first open) come in the same order.

`nn info` prints two arena figures: `arena : N B reserved` is the activation
arena the backend reserves (`tflm`: `g_arena`, 524,288 B; `stedgeai_reloc`:
`g_acts`, 393,216 B; `stedgeai`: the generated activations size; `null`: 0),
and `used : N B (activations)` is what the open model's activations actually
take of it (absent with no model, and on `null`). The same two lines as
wio-lite-ai and grove-vision-ai-v2.

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
| `nn info` arena line | `470352 B reserved` -- [!] labelled "reserved" but it was the arena TFLM actually USES; since #131 that figure is the `used` line and `arena` is the reservation, `g_arena`, 524,288 B |

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

After issue #142 (the GUIX and nn subscribers folded into one camera sink with
two steps, and the holder table; 2026-10-11, firmware `8baed71b`):
`nn stream start --frames 300` gave 31 inferences, 1.48 inf/s, with `in -
skipped` still equal to the inferences, and 1.47 inf/s across `lcd off` / `lcd
on`.  The DCMI overrun count moved with the build's layout, not with the change
(issue #144, below).

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
(`svc/nn/blazeface.c`) and owns NO storage: this board passes in its own scratch,
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

### The camera streams while somebody holds it (issue #142)

```
camera stream start [test]   take hold of the base capture (test = colour bar)
camera stream stop           let go; refused while nn holds it
camera stream stats          counters, subscribers and the holders
```

The base capture runs while, and only while, somebody holds it: the operator
(`camera stream start`) or nn (`nn stream start`, `nn run`).  The table is
`svc/camera/cam_holders` (the shared rules are in
[svc/README.md](../../svc/README.md#the-camera-streams-ownership-issue-142)),
kept next to the base in `port/camera/camera.c` under the camera lock.  When the
LCD comes up at boot, GUIX takes hold in the operator's place
(`ui/guix_camera_ui.c`), so on a board with a working sensor the base is already
running after boot and `camera stream start` answers `camera: streaming live
already running`.

- **`camera stream start` has no `--frames` / `--secs` any more.**  The stream
  runs until the last holder lets go or an error ends it; `nn stream start
  --frames <n>` is the only bound.  `test` takes effect only when this call
  starts the base; on a running stream the same setting is accepted and a
  different one is refused (`the stream is running with a different test
  setting`).
- **`camera stream stop` while nn holds the stream** answers `camera: still in
  use: nn` and changes nothing -- the operator stays too.  Otherwise it answers
  `camera: stream stop requested`: the stop is asynchronous, and the producer's
  teardown looks at the holders again under the camera lock and withdraws the
  stop if a holder came back meanwhile.  `camera off` is refused the same way
  while nn holds the stream.
- **`camera stream stats` prints `holders:`**, e.g. `holders:   manual, nn`, or
  `none`, followed by `(<n> lost when the stream ended)` after a confirmed end
  (the auto-recovery gave up or could not re-arm, or a power-off cancelled it).
- **`nn stream start` and `nn run` join a running stream, or start it** holding
  the operator's place too, so the camera keeps streaming after nn stops.  They
  start it in the current mode, which must be a streamable RGB565 mode (QQVGA
  or QVGA); any other mode is refused, as is a running base of another format
  (a JPEG stream for `net mjpeg`).  (Issue #99 removed the `[qqvga|qvga]` word
  `nn stream start` used to take -- the geometry is whatever the base
  publishes.)
- **`camera res` / `camera format` / `camera fps` are refused while anyone holds
  the stream** -- including between an overrun and its auto-recovery, which
  re-arms in the mode the holders joined.  The GUI's resolution button stops and
  restarts the base only while the operator is the sole holder.
- **The preview follows the display, not a switch.**  The GUIX display step
  copies a frame only while the display is on as `lcd on` / `lcd off` last set it
  (`ltdc_display_on()`).  Under GUIX `lcd off` drives the backlight and DISP and
  leaves the LTDC scanning, so the display step is skipped but the scanout is
  not stopped.  The boxes are drawn only for `nn stream`; `nn run` draws none,
  although its result is in the record (`nn dets`) either way.

> **Before issue #142** the GUI preview, `nn stream` and `net mjpeg` were three
> subscribers, the base kept no record of who wanted it, and `camera stream
> start` took `--frames` / `--secs`.
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
> (issue #121, `svc/nn/nn_top.c`); `nn run` and `nn dets` print those, never the
> output tensor as it is at print time.
>
> **A frame that arrives during an inference is skipped, not staged** (issue
> #130 step 6b). The camera producer preprocesses a frame straight into the
> model's input tensor, and only while the worker wants one; the shared
> hand-over word (`svc/nn/nn_handoff.h`, stepped by `svc/nn/nn_core_frame.c`, the same
> one grove-vision-ai-v2 and wio-lite-ai use) says who owns the input. In `nn
> stream stats`, `frames in` counts every frame offered, `skipped` the ones that
> arrived while the worker was busy (or before it armed), and the rest were
> handed over and inferred -- so `in - skipped` equals the inferences, give or
> take the one in flight when the stream stopped, and less any inference whose
> `nn_run()` failed or whose decode was refused (those are counted in
> `errors`, not in `infers`). A frame
> whose preparation was abandoned is in neither `in` nor `skipped`: that
> happens when its session ended part way through (a base detach or a stop
> moved the epoch) -- such a frame belongs to no session, which is how the
> staging buffers treated it too -- or when the input tensor was missing,
> which is counted in `errors`. Before step 6b the producer
> filled two SDRAM staging buffers and the worker copied one into the input:
> frames kept arriving into a free buffer during an inference, so `in -
> skipped` ran ahead of the inferences (34 vs 30 in the baseline below) and
> the frame inferred had usually sat in its buffer for most of an inference.
>
> **A decoder fault is counted apart; "not BlazeFace" is not a fault** (issue
> #130 step 6c, #122 P8, decision D6; `port/nn/nn_decode_count.c`). The
> resident decoder runs on every model, and for one that is not
> BlazeFace-shaped it answers `BF_ERR_MODEL` and the worker publishes the top
> 5 classes with it -- that is a classifier stream's normal result, so
> `model_errors` stays 0 on this board. Any other negative (`BF_ERR_UNINIT`,
> `BF_ERR_ARG`, or an undocumented code) is a wiring fault: it counts in
> `decoder_errors` and in `errors`, and the shared command prints the `of
> those:` line. Such an inference is **not** counted in `infers` (issue #138,
> as on grove-vision-ai-v2 and wio-lite-ai): `infers` counts the results a
> reader can use, so with decoder faults `in - skipped` equals `infers` plus
> the faults. Until #138 it counted in both, because it ran; before step 6c
> neither was counted. A `nn run` counts no decode (it reports its own
> result).

### The `camera` command is shared with the other boards (issue #143)

The command is one file for all three boards, `shell/cmds/cmd_camera.c`, over
this board's adapter `port/camera/camera_svc_f746.c`, which implements the
contract in `svc/camera/camera_svc.h` (the layers are in
[svc/README.md](../../svc/README.md#the-camera-command-is-shared-issue-143)).
What only this board has stays in `cmds/cmd_camera_board.c` and is appended to
the shared table.  This board's capabilities (`svc/camera_svc_config.h`): power,
info, test pattern, save (`sd`, `fs`), send; `camera off` stops the operator's
stream first.

```
camera probe                    power up + read the OV5640 chip ID
camera on | off                 bring-up / cut the module's power
camera info                     driver / sensor state and the ISP settings
camera capture [test]           snapshot the current mode + statistics
camera save [sd|fs] <path>      write the LAST capture raw (default sd)
camera send [name]              send the LAST capture over YMODEM
camera stream start [test] | stop | stats
camera res | format | fps | set   (this board's own)
```

- **`save` and `send` read the last `camera capture`**, stream or no stream: the
  stream never writes the capture buffer.  With nothing captured since boot (or
  since `camera off`) they answer `camera: no captured frame`.  The medium may be
  left out (`camera save x.raw` writes to `sd:x.raw`); a lone medium name
  (`camera save sd`) is refused as a forgotten path.  A frame replaced while it
  is being written fails the save (`camera: frame changed by a concurrent
  capture (file left partial)`) or the send (`ymodem: source read error`, then
  the same sentence).
- **`sdram test` claims the frame buffer and the stream ring for its whole run**
  (issue #150, `camera_scratch_claim()` under the camera lock).  It is refused
  while a stream runs or is held; while it runs, `camera capture` and `camera
  stream start` from any console are refused as busy, `nn stream start` / `nn
  run` are refused, and afterwards `save` / `send` answer `no captured frame`
  until the next capture.
- **`sdram test` also claims `.sdram.ai` from nn and skips live Ethernet state**
  (issue #152).  The NN arena, model slots and decoder scratch live in
  `.sdram.ai` (bank 3), owned by `port/nn`, so the test takes
  `nn_scratch_claim()` right after the camera claim and keeps it for its whole
  run.  While it is held no nn session can be taken (`nn model load` /
  `unload`, `nn bench`, `nn run` and `nn stream start` are refused as busy,
  worded ``.sdram.ai is held by `sdram test`; try again when it finishes`` where
  the command prints a reason), and the first model open after boot -- which is
  where `tflm` adopts its built-in model into the arena -- builds nothing: a
  fresh-boot `nn info` answers busy instead of adopting it, and adopts it
  normally once the test is over.  The claim refuses while another nn command
  or a stream is active, or while the first open is building (`sdram: nn is
  busy; wait for the nn command / stream to finish`), and while a model is
  loaded (``sdram: a model is loaded in .sdram.ai; run `nn model unload`
  first``) -- a model left loaded over test patterns would run on garbage, and
  the test never unloads it for you.  Bank 2 (`.sdram.eth`) holds the ETH DMA
  descriptors and the NetX pool; the IP instance never goes down once boot
  created it, so while the network is up the test skips the whole bank
  (`sdram: bank2 (0xC0400000..0xC05FFFFF) skipped: the network is up (ETH DMA
  descriptors live there)`) and the summary counts only what was tested (6144
  KB for a full run).  The MJPEG server needs no guard of its own: it reads the
  camera stream, which the camera claim already refuses.
- **`sdram test` holds the display** (issue #152, `ltdc_scanout_hold()` under
  the LTDC lock).  `ltdc_fb` sits in bank 0, so the test is refused while GUIX
  owns the display (`run 'gui stop' first`); otherwise scan-out is stopped
  before the first write.  Until the test ends, `lcd on`, `lcd fill` / `bar` /
  `grad` / `clear` / `anim` / `blit` and `gui start` are refused with ``display
  is held by `sdram test` ``, the driver's draw helpers draw nothing and flips are
  refused, so nothing but the test writes the framebuffers.  `lcd off` is
  accepted and only means the display stays off afterwards; `lcd info` only
  reads.  At the end both buffers are repainted black, and scan-out and the
  backlight each return to what they were (recorded apart: under GUIX `lcd off`
  drops only the backlight).  `nn run` / `nn stream start` during the test answer busy with
  ``.sdram.ai is held by `sdram test`; try again when it finishes``.
- **`camera off` with the operator's stream running** prints `camera: stopping
  stream before power off`, waits for THAT stream to end and then cuts the rail
  (`camera: power off`).  A holder other than the operator refuses it (`camera:
  still in use: nn`).  If the stream has not ended within 200 ms the power is
  left on (`camera: the stream has not stopped yet -- timed out (it may still
  finish); power left on, retry 'camera off'`); a Ctrl+C during the wait also
  leaves the power on.  The wait is on the stream's GENERATION: camera.c counts
  streams (`cam_stream_gen`, advanced under the camera lock when a start's arm
  has succeeded; `cam_stream_done_gen`, written by the teardown under the same
  lock), and `camera_stream_stop_gen()` hands back the generation the stop asked
  to end, so a later start, or a start that withdrew the stop and kept the
  stream, is never taken for its end.
- **`camera capture` prints `warm-up  : <n> ms`**, the time the capture spent
  before its snapshot (bring-up, the configuration's settle sleeps, PCLK
  programming).  Measured: 632 ms on a cold sensor, 0 ms on the next capture,
  101 ms after a colour-bar switch.  0 is printed too.
- **Lines read `label    : value`** (the label padded to 8 and ` : `; a longer
  label keeps its length), the same form on all three boards.

Measured / derived for this board:

| Item | Value |
|---|---|
| Console stack high water after save / capture (`thread`) | 2,728 of 4,096 B (at the shared command's first version) |
| `camera save` deepest path (from the disassembly, dispatch included) | about 3.18 KB to the microSD, about 3.43 KB on the QSPI write-error path |
| `CAMERA_SVC_DETAIL_MAX` (the contract's failure text) | 319 B |

### [!] Two subscribers share one capture, and each has to drain

The base capture has two *subscribers* since issue #142:

- **the camera sink** (`port/camera/cam_sink.c`): one RGB565 subscriber whose
  `consume()` runs the GUIX display step, then the nn step, then
  `camera_frame_put()`.  It is subscribed once and stays subscribed; the base
  attaches it when it streams RGB565 and detaches it when it stops;
- **`net mjpeg`**, a subscriber of its own, because it needs a JPEG base this
  sink never sees.  It is not a holder either: it streams only while somebody
  holds the base in JPEG mode.

`gui stop` and `nn stream stop` do not unsubscribe anything.  Each switches its
own step off and waits for the producer to leave it (`svc/camera/cam_step.h`): the
step's armed mark is dropped, and its busy count reaching 0 proves the producer
is out of the step and cannot enter it again.  The other step and the base keep
running.  The owner keeps its transition claimed (`port/camera/cam_own.h`)
across disable, drain, its own clean-up and commit, exactly as it used to across
unsubscribe and pin drain.

Stopping a subscriber detaches it while the base keeps running, so a delivery
can already be in flight across the unlink: `frame_pipeline_publish()` copies
the sinks it will deliver to into a local array, drops the pipeline lock, and
only then calls `consume()` on each.

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

When a drain -- of a step's busy count, or of a subscriber's pins -- does spend
its budget, the owner refuses to release and says so:

| command | what you see | what it means |
|---|---|---|
| `gui stop` | `preview did not release the camera frame; display kept` | GUIX keeps the LCD and its display step stays claimed.  Run `gui stop` again. |
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

Since issue #142 a stream is torn down for two reasons only: a stop (the last
holder let go) and an error.  There is no frame or time limit to reach any
more, so the in-place resync above is the one way back from an overrun on the
raster stream.

These errors are still terminal -- the stream tears down (`state: stopped
(overrun)`) and the escalating auto-recovery of owhinata/stm32f746g-disco#100
takes over, holding the holders across it:

- an overrun on the JPEG stream (snapshot-per-frame, not this path);
- a DMA transfer error, or a DCMI synchronisation error or stop timeout, seen
  either in the HAL error codes or in the raw DMA / DCMI flags;
- an error indication without an overrun in it;
- CAPTURE not clearing within 500 ms, EN not clearing within 10 ms, the FIFO not
  draining, or a DMA re-init / restart failure;
- the 30th resync in a row with no frame completed between them (so at most 29
  in-place restarts, about one second at 30 fps) -- a persistent overrun falls
  back to the escalating path rather than resyncing forever.  A completed frame
  ends the run even when every slot is pinned and it is dropped rather than
  published (issue #138): the capture is running, and a slow subscriber's
  overruns must not add up to the limit.

The terminals decided inside the resync (the last two items, and an error code
or raw flag found there) log `DCMI overrun: in-place resync failed (<reason>);
stopping` first.  Those decided in the error callbacks -- a JPEG-stream overrun,
a DMA transfer error, a DCMI sync error -- do not.
A stop that is due by the time the teardown decides wins: the stream is not
auto-recovered, even when a resync (or any other terminal error, JPEG included)
failed after the stop was asked for.  The teardown looks at the holders again
under the camera lock first (issue #142): a stop asked for by the last holder's
leaving is withdrawn if a holder came back meanwhile; an error never is.  A terminal
error that was latched still shows as `stopped (overrun)` in `stats`.

**Known: how many overruns you get depends on code placement (issue #144).**
With nothing else changed, moving unrelated code by a few dozen bytes takes the
count from 0 to 16-18 in 3 minutes of GUIX preview, arriving in bursts of 3-4
every 39.77 s at the same times after every boot.  The logic of the code that
moved is not the cause: replacing an instruction with a NOP of the same length
(same placement) keeps the overruns, and adding 32 B of padding in a function
that runs once at boot removes them.  The root cause is suspected in the DMA2
FIFO / SDRAM arbitration between the DCMI writes and the LTDC scan-out.  Every
burst is resynced in place (`ovr ring` 0, nothing dropped by the ring), at a
cost of 3-4 frames every 40 s on an unlucky layout.

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
