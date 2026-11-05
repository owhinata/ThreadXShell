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
picocom -b 115200 --send-cmd "sb -k" --receive-cmd "rb" /dev/ttyACM0   # the VCP
```

The first configure downloads the pinned ARM GNU toolchain into `tools/` and
the submodules this board needs (`boards/f746g-disco/submodules.cmake`), and
fetches the ST Cube set this board pins in `board.cmake` -- the F7 HAL and CMSIS
device F7 -- and its CMSIS core into `boards/f746g-disco/lib/<repo>-<commit>/`,
git-ignored by this board's `.gitignore` (issues #136 / #177; see the root
README's Building section).  Those trees are shared with every other build
directory of the clone, so later configures are offline.  The
default NN backend (`tflm`) also fetches the `lib/tflite-micro` submodule on
first use and generates the tflite-micro tree from it at configure time (which
downloads its third-party dependencies); its build needs no network.
`-DCONFIG_NN_BACKEND=null` touches none of these and builds offline (see
[Build options](#build-options)).

The firmware carries no model (issue #159 Step 4c): after boot `nn info` says
`model : (none)`. To run the face detector, build its container, send it over
the VCP into the [QSPI asset store](#the-qspi-asset-store-blob-issue-194) and
load it by name (since issue #194; until then it was a file on the microSD
card):

```bash
cmake --build build/f746g-disco --target asset-blazeface   # fetches the pinned model
```

```
blob erase 12                    # the slot must be empty first
blob write blazeface 12          # then, in picocom: C-a C-s and
                                 # build/f746g-disco/asset/blazeface.nnc
blob list                        # slot 12's crc32 must be the receipt's
nn model load --name blazeface
```

The CRC32 `blob list` shows is the one the store computed over the bytes it
received; compare it with the one in the receipt the asset build printed (see
[Building this board's containers](#building-this-boards-containers)).

CMSIS core stays at `v5.6.0_cm7`, newer than the 5.4.0 STM32CubeF7 pairs with its
HAL. Going down to 5.4.0 would give up four properties for nothing but matching
the Cube: `SCB_EnableICache()` / `SCB_EnableDCache()` doing nothing when the cache
is already on (5.4.0 invalidates it again, dropping dirty D-cache lines if the
start-up path is entered without a reset); the `*_by_Addr` cache maintenance
handling a start address that is not 32-byte aligned (5.4.0 assumes it is); the
compiler barriers around `NVIC_EnableIRQ()` (the write order of the #137 DCMI
recovery would no longer be guaranteed by CMSIS); and a defined `__CLZ(0)` (5.4.0
leaves it undefined). Revisit this when the HAL is moved to a newer Cube release.

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
| SRAM | `0x20010000` | 224 KB | SRAM1: data, bss, heap, main stack |
| Plugin | `0x20048000` | 32 KB | top 16 KB of SRAM1 + SRAM2: the `.plugin` reservation (issue #159) |
| SDRAM | `0xC0000000` | 8 MB | FMC bank1, 16-bit bus |

ITCM placement was tried and dropped: with I-cache and D-cache enabled it was
worth about 0.6%, because the cache already hides the flash wait states.  The
region is left in the map as a fact about the part, not as a placement target.

### [!] The top 32 KB of internal SRAM is the plugin reservation (issue #159)

`.plugin` is a fixed NOLOAD reservation at `0x20048000..0x20050000` for the
load image of a model post-processing plugin (#78 Step 4; see [Plugin
containers](#plugin-containers-issue-159--78-step-4)).  A plugin is
**prelinked** for that address -- the loader services no relocations -- so it
cannot drift the way a sequential section would the day `.bss` grew, and moving
it later invalidates every container built for this board.  Internal SRAM is
executable through the default memory map (`PRIVDEFENA`; no MPU region covers
it, `src/bsp.c`), so the reservation needs no MPU change.

**The main stack and the heap moved below it.**  `RAM` shrank from 256 KB to
224 KB, so `_estack` is `0x20048000` (was `0x20050000`), the main-stack
reserve (`_Min_Stack_Size`, 1 KB) is unchanged in size, and the heap ceiling
`src/retarget.c` derives from `_estack` moved down with it: the heap runs from
`end` to `0x20047C00` (114,304 B on `tflm` at #159 Step 4a, was 147,072 B).
`free` reports SRAM as two rows, `SRAM` (up to the reservation) and `Plugin`
(the reservation, never counted as spendable).  `membench` still takes its
32 KB buffer from the heap.

The address is declared four times, independently, on purpose: the linker
script, the plugin `MEMORY` fragment (`ldscript/plugin_memory.ld`, which a
plugin links against from Step 4b), `board.cmake`'s statement to the shared
image gate (`F746_PLUGIN_GATE_BASE/END`), and `cmake/check_f746_layout.py`,
which checks the linked image in every backend: the section at exactly that
address and size, NOLOAD, nothing else allocated or named inside it, `_estack`
at or below its base, the heap floor below that -- and that the linked `_sbrk`
names `_estack` and no address inside the reservation, read from its own
constants.  Naming `_estack` is not using it as the bound (`_estack +
_Min_Stack_Size` names the same constants and passes that check -- the #159
review linked such an image), so the behaviour is checked separately:
`test/test_sbrk.c` compiles the real `src/retarget.c` on the host and requires
the break to stop at `_estack - _Min_Stack_Size`.  **Do not generate any of
them from another.**
`cmake/fixtures/run_layout_tests.py` watches each refusal (moved, shrunk,
loaded, missing, overlapped, a symbol inside, `_estack` above, `_sbrk` naming
the top or not naming the ceiling, `_sbrk` absent, the heap floor above).

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
`bsp.c` makes instruction-fetchable.  Since #173 bank 3 (MPU region 1) is Normal
write-back with no write-allocate (TEX=0 C=1 B=1, non-shareable): a store that
misses the D-cache does not allocate a line there, which made TFLM inference
about 11% faster; write-through, which measured about the same, is not used
because a write-through region is the condition of erratum ES0290 2.1.1.  The
reloc backend's execution window (region 2) stays write-back write-allocate.

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

### The NN backend: `tflm` by default (issue #130)

`tflm` runs tflite-micro with CMSIS-NN kernels.  [!] **It carries no model**
(issue #159 Step 4c): a model -- a bare `.tflite`, or a plugin container that
brings the decoder for it -- is sent into the QSPI asset store with `blob
write` and loaded with `nn model load --name` (issue #194; a file on the
microSD card and `--path` before).  The pinned BlazeFace front 128 int8 that wio-lite-ai and
grove-vision-ai-v2 ship (ST model zoo, `STMicroelectronics/stm32ai-modelzoo`,
commit `1423c78953a830903485135febe1dd98ff31aed8`,
`face_detection/facedetect_front/.../blazeface_front_128_int8.tflite`, SHA256
`e803bb4e93b10f7a19d4243bcc39698599a723f3128e19d3f90e0b1c0bc88dd8`, 189,816 B)
reaches this board as the `asset-blazeface` container
([Building this board's containers](#building-this-boards-containers)).  From
#130 to Step 4c it was compiled into Flash as the built-in model; that array,
its build-time fetch, its generator and `NN_TFLM_MODEL_OVERRIDE` are gone, and
a tree that still holds `NN_TFLM_MODEL_OVERRIDE` or the older `NN_TFLM_MODEL`
gets a configure warning saying so (`-U<name>` silences it).

[!] **A `tflm` configure needs the network, its build does not**: the
tflite-micro tree is generated at configure time (since issue #134 tflite-micro
itself is the `lib/tflite-micro` submodule, which the tflm configure initialises
on first use, and generating the tree downloads its third-party dependencies).
Only `asset-*` targets fetch a model, at build time and outside ALL (issue
#94). A `null` build needs neither and configures and builds offline.

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
file (each `.c` with its `.h` unless only a header is listed; the container
check, the decode count and the session-release rule moved to `svc/nn/` in
issue #163, see svc/README.md):

| File | Thread or context | Hardware reason |
|---|---|---|
| `port/nn/nn_svc_f746.c` | console | The thin adapter over the `nn.h` API (`svc/nn/nn.h`, implemented by `svc/nn/nn.c`, shared with wio-lite-ai); it adds no state. `nn thresh` is `svc/nn/nn_svc_common`'s and the refusals' words are `svc/nn/nn_refusal`'s (issue #174); this file passes its lease and its teardown clause. |
| `port/nn/nn_svc_f746_model.c` | console (`nn info`, `nn model load` / `unload`) | `nn info`'s model section is `svc/nn/nn_svc_common`'s (issue #174), given this file's counter read. The steps of the shared load order on this board: the blob named by `--name`, found and read under the store's busy flag (issue #194), with its length checked before the read and its CRC after; the model staging buffer in SDRAM (on `tflm` asked for only after the open model went down, issue #191); on `tflm`, the plugin policy, the container split and the open container's claims (issue #159; the claims' store and their `nn info` lines are `svc/plugin/plugin_claims`, read under this file's critical section). |
| `port/nn/nn_camera.c`, `nn_camera_priv.h` | console (start / stop / stats / record reads) and the camera sink's open / close of the nn step | Binds the shared frame path to this board's camera as the nn step of the one camera sink, and holds nn's place among the camera's holders (issue #142); owns the glue's state (the private header lists each counter's writers and resets) and the frame path's ops table. |
| `port/nn/nn_producer.c` | camera producer (the nn step of the camera sink's `consume()`) | Prepares a frame straight into the model's input tensor (nearest-neighbour resize + RGB565 expansion on the CPU, each value through the shared conversion `svc/nn/nn_input_contract.h`) and hands it over; it does not put the frame -- the sink does, last. |
| `port/camera/cam_sink.c` | camera producer (`consume()`), console (step enable / disable / drain) | Not an nn file, but nn runs inside it: the one RGB565 subscriber of the base capture, running the GUIX display step and then the nn step, with `camera_frame_put()` last. Each step's armed mark and busy count are `svc/camera/cam_step.h`'s. |
| `port/nn/nn_worker.c` | inference worker (priority 18) | Inference runs on the CPU, so the worker sits below every other thread; it runs the model, publishes the raw tensors ("nothing decoded them") or, under the plugin lease, the plugin's decode, and does the counting. |
| `port/nn/nn_active.c` | worker, GUIX, console, under the plugin lease (tflm) | This board's facts for the shared entries (`svc/nn/nn_active_lease.c` above `svc/nn/nn_active_core.c`): reading an `nn_tensor` as a descriptor, where a depth sample goes, the frame a box maps into, and the base vtable's transform and log sink. |
| `port/nn/nn_panel.c` | GUIX (CAMERA_FRAME handler) (tflm) | The panel's half of the shared frame path: the lease tried, then the frame lock, then the plugin's draw into the view buffer, with its spent / refused / hold figures.  The display is handed in as hooks (`nn_panel.h`). |
| `port/plugin/plugin_run.c` | console (load / unload), fault handler | The MPU read-back and the M7 cache maintenance around the shared loader, and its log; the reservation's address from the linker script. |
| `port/plugin/plugin_lease.c` | every path into the plugin | The lease as a ThreadX mutex with inheritance: the GUIX thread outranks the worker and would otherwise preempt a decode. |
| `port/plugin/plugin_paint.c` | GUIX | CPU pixel loops into the camera view buffer (row-major RGB565 in non-cacheable SDRAM; no DMA2D). |
| `src/fault_plugin.c` | fault handler | Whether a fault's pc is in the loaded plugin, and that its end is a reset (pure, host-tested). |
| `port/nn/nn_model_board.c` | worker (run), console (open / bench / info) | The state and hooks of the shared model API (`svc/nn/nn.c`): its state as plain CPU-only `.bss`, the PRIMASK critical section, the one-tick ThreadX wait, and the DWT cycle counter, enabled here with the CoreSight lock key (TRCENA, LAR unlock, CYCCNTENA). |
| `port/nn/nn_null.c` | as `svc/nn/nn.c` | The `null` backend: a BlazeFace-shaped stub with no runtime, present without a load. Its input is int8, scale 1/128, zero point 0, so under the default `nn norm 0` the camera writes 0..127 into it (issue #162; the old `v - 128`, -128..127, comes back with `nn norm 1`); its run does nothing, so the tensor's contents are the only visible effect. |
| `port/nn/nn_stedgeai.c`, `nn_stedgeai_reloc.c` | as `svc/nn/nn.c` | The non-default ST Edge AI Core backends (a model baked into Flash, or a relocatable one); they need a local ST install. |
| `port/nn/tflm/nn_tflm_board.cc` | as `svc/nn/tflm/nn_tflm.cc` (worker: invoke; console: load / info) | The state object of the default backend, the shared tflite-micro one (`svc/nn/tflm/`, CMSIS-NN kernels; one implementation with wio-lite-ai since issue #161), as plain `.bss` in internal SRAM, and this board's fact that a model is named by its blob name with no prefix (`sd:<path>` until issue #194, when it came off the SD card). C++ only because the state holds TFLM objects' storage. |
| `port/nn/tflm/nn_tflm_bufs.c` | as above | The arena and the one model staging buffer (1 MB since issue #191; two 512 KB slots before, the same bytes) in SDRAM bank3 (`.sdram.ai`, CPU-only), defined in C so `check_f746_layout.py` requires them by their plain names, and `nn_tflm_stage_cap`, the capacity `check_blob_staging.py` reads back against the slot table (issue #194). |

[!] **`nn run`'s driver (`nn_svc_run_once()`) stays in this board's adapter** (issue #174 D7).  Its policy is shared already: the refusals and the base words of how a run ends (`svc/nn/nn_refusal`), when the wait ends (`svc/nn/nn_run_wait`) and how one run ended (`svc/nn/nn_shot`).  What is left is the camera API (the start's arguments and refusals, how a lost stream is read, the decode read-out), the teardown's clause, and ThreadX's sleep and clock -- `svc/` cannot include `tx_api.h`, so a shared driver would need a hook table as large as the loop itself.

[!] **Only input 0 is filled** (issue #162 did not change this).  Since #161
the backend accepts a model with 1..8 input tensors (it refused more than one
before), but the camera producer writes input 0 only: a model with more inputs
runs with the others undefined (whatever the arena held).

This board has no panel thread.  The GUIX thread draws the overlay itself, in
its CAMERA_FRAME handler (`ui/guix_camera_ui.c`): a loaded plugin's draw
through `nn_panel_draw()`, and with none nothing (a stream with nothing to draw
is refused at its start, below).

### The camera input: one contract with wio-lite-ai (issue #162)

What the camera can fill and what each pixel becomes are
`svc/nn/nn_input_contract`, shared with wio-lite-ai.  This board keeps how a
pixel is sampled (nearest-neighbour over the whole frame, the RGB565 expansion).

**Admission.**  `nncam_open_model()` (under the NN session, which
`nn_camera_start()` takes before it looks at the model) asks
`nn_input_admit()`; each reason has its own code and sentence, the same as
wio-lite-ai's:

| Code | Reason | Sentence (`<name>`) |
|---|---|---|
| -3 | no input tensor, or it has no buffer | this board's "no model" sentence |
| -18 | dtype neither float32 nor int8 | `input type is not float32 or int8` |
| -19 | not 4-dimensional | `input is not 4-dimensional` |
| -20 | batch not 1 | `input batch is not 1` |
| -21 | height or width 0 | `input height or width is 0` |
| -22 | channels neither 1 nor 3 | `input channels are not 1 or 3` |
| -11 | int8 scale not a positive normal float | `int8 input scale is not a positive normal number -- a per-axis quantized input reads as scale 0` |
| -23 | int8 zero point outside -128..127 | `int8 input zero point is outside -128..127` |
| -24 | float32 buffer not 4-byte aligned | `float32 input buffer is not 4-byte aligned` |
| -25 | buffer shorter than H x W x C elements (64-bit product) | `input buffer is shorter than its shape` |

Every worded refusal reads `the model input cannot take camera pixels: <name>
(`nn info`)`, with status `hardware` and nothing held, from `nn run` and `nn
stream start` alike.  The -3 row (no model, or no usable input tensor) is
answered with status `state` instead, as on every board and as this board's
`nn bench` (issue #164; it said `hardware` until then).  The numbers and the table are `svc/nn/nn_input_codes`;
this board's `port/nn/nn_camera.h` takes its `NNCAM_ERR_*` from those names.

-4 is no longer returned.  **This board admits less than it did**: a
3-dimensional input (it used to be read as one channel), a batch other than 1,
2 or 4+ channels, uint8 / int16 / int32 inputs (written a byte at a time
before), an int8 scale of 0 or otherwise not a positive normal float -- so the
`stedgeai_reloc` backend's int8 input, which always reads as scale 0, is
refused -- a zero point outside int8, and an unaligned float32 buffer.

**The value.**  Each sampled 8-bit channel goes through `nn_input_f32()` or
`nn_input_i8()`: one explicit fused multiply-add (`VFMA.F32`, bias as the
accumulator) for the `nn norm` range, then for int8 a multiply by 1/scale
(computed once per frame, int8 only), a limit to +-255, +-0.5 and a conversion toward zero,
the zero point, and saturation to int8.  The rounding is close to, but not
exactly, TFLM's QUANTIZE (`std::round`): just below +-0.5 the float addition
rounds to 1.0, and TFLM divides by the scale where this multiplies by 1/scale.
The host test `shell/test/test_nn_input_contract.c` compares every 8-bit value,
both ranges and a set of scales / zero points against a reference that spells
out the target's instruction sequence.

**What changed in the values** (issue #162 step 5):

- int8 follows the tensor's scale, zero point and `nn norm` instead of a fixed
  `v - 128`.  With the `null` backend (scale 1/128, zero point 0) and this
  board's default `nn norm 0` the input is now `round(v / 255 * 128)`, 0..127
  (saturating at 127), where it was -128..127; `nn norm 1` gives the old range
  back to within the gain's rounding.
- float32 is a fused multiply-add instead of a divide, so a value can differ
  from before in the last bit; detections are expected to match and scores to
  move by at most the last printed digit.

**Cost -- estimates, not measured.**  From the instruction count of the final
image, 49,152 elements (128 x 128 x 3) at 216 MHz, 1 to 1.5 cycles per added
instruction:

- int8: about 26 more instructions per element (fma, multiply, three compares,
  add, convert, saturate in place of one subtract), roughly +5.9 to +9.1 ms per
  frame.  There is no int8-input model for this board to measure it with.
- float32: the per-element `VDIV.F32` (14-cycle latency) is gone, roughly 2.5
  to 3.2 ms per frame faster.

**Producer stack.**  The camera producer thread (`cam_prod`, 1024 B): deepest
reachable path 600 B (its stream-start logging path; the nn path through
`nncam_prep`, 96 B of it, is 400 B), plus 104 B for an exception frame with FP
state and 100 B for the ThreadX context save -- 804 B of 1024.  From the
`f746-tflm` `.su` files, with the calls through the pipeline's and the sink's
function pointers added by hand.

### No resident decoder (issue #159 Step 4c)

A decoder reaches this board only in a plugin container, as on wio-lite-ai
since its #116; nothing in the firmware links `svc/nn/blazeface.c` (the
blazeface plugin compiles and audits it). The answers are wio-lite-ai's, line
for line:

- The shape question of the admission `nn run` and `nn stream start` share
  passes with no plugin; a bare model's `nn run` runs and reports its tensors.
- `nn stream start` with nothing to draw -- no plugin, or one with no `draw()`
  -- is refused, and `nn_active_can_draw()` is the one thing that says so:
  `nothing would annotate a live preview: no decoder is loaded, or the one that
  is draws nothing (`nn info`); `nn run` still works`.  (Status `wrong state`;
  wio-lite-ai reports the same words as a hardware refusal.)
- `nn thresh` reads `none -- the active decoder has no threshold`, and setting
  one is a state refusal, not a bad value.
- With no plugin the worker still publishes "nobody interpreted this" under the
  generation rule, so `nn run` does not time out; it counts as an inference.
- A non-`tflm` build (`null`, `stedgeai`, `stedgeai_reloc`) gives the same
  answers without the plugin machinery: `nn stream start` refused, `nn run`
  reports tensors, `nn thresh` none.

### `nn run` is one frame and ends one of five ways (issue #171)

`nn run` drives the stream's producer and worker for exactly one frame.  The
start gives the run a new number (`svc/nn/nn_shot`, shared with wio-lite-ai);
the producer asks whether the run is still open before it begins filling the
input tensor, and the worker parks after every job and arms again only while
the run is open -- that is, while nothing has ended it yet.  Every ending (the
producer's preparation refused, the inference failed, the plugin lease not had
in time, the publish) is recorded under `nncam_lock`, the lock the attach, the
detach and the stop move the record's generation under, and the first one wins.

The console's wait ends one of five ways, in this order of precedence
(`svc/nn/nn_run_wait.c`, the same table on all three boards): **inferred** (the
record accepted a publish), **no result**, **lost** (the camera stream ended,
issue #163), **cancelled** (Ctrl+C), **timed out** (2 s).  No result means the
run ended without a publish; the status and the sentence come from the shared
table in `svc/nn/nn_shot.c`, e.g. `inference failed` (status `hardware`) or
`the plugin lease was not released in time; nothing was decoded` (status
`busy`), and an ending the table does not know reads `the inference worker
ended the run without a result` rather than waiting on.  Before #171 those runs
sat out the 2 s and said timed out, and the worker armed again after every job
while the run lasted.  There is no bench procedure that makes an inference or
a preparation fail, so the no-result paths are covered by
`shell/test/test_nn_shot.c` and `shell/test/test_nn_shot_seq.c` only.
`camera stream stop` during a `nn run` is still refused (`camera: still in use:
nn`), so it does not make the run lost.

Measured 2026-11-01 (container and bare `.tflite` `nn run`, Ctrl+C, a stream,
ten `nn run` in a row):

| | before | after |
|---|---:|---:|
| Flash, `tflm` | 614,404 B | 615,684 B |
| Flash, `null` | 415,172 B | 416,388 B |
| `.bss` | | +32 B |
| `nn run` | | 0.66-0.71 s, one inference each |
| stack peaks | | `cam_prod` 548 / 1,024 B, `nn-worker` 1,512 / 4,096 B, `cli` 2,524 / 4,096 B (unchanged) |

### The model: loaded by name from the QSPI store, or nothing (issue #131, #159 Step 4c, #194)

The backend can hold no model at all: `nn info` then prints `model : (none)`
and, in place of the tensor lines, `note : nothing loaded -- `nn model load
...``; `nn run`, `nn stream start` and `nn bench` are refused (`nn bench` with
`no model is loaded`). On `tflm` that is the state after boot (Step 4c: there
is no built-in model) and after `nn model unload`.

- `nn model load builtin` is refused before anything is acquired: `this board
  loads a model from a QSPI asset by name (--name); it has no built-in model`
  (SPEC). The usage lists `--name` only.
- [!] **There is no SD load any more** (issue #194).  `nn model load --path
  <p>` is refused the same way (`... it has no filesystem load (send the file
  with `blob write`)`), `--slot <n>` too (`... no load by slot number (`blob
  list` names them)`), and a bare word by the shared grammar (`nn: '<word>' is
  not a model source.  Say which kind it is ...`).  A model that used to sit on
  the card is sent again into a slot -- see [Moving models off the SD
  card](#moving-models-off-the-sd-card).
- **`nn model load --name <name>`** (`port/nn/nn_svc_f746_model.c`) refuses a
  name the store could never have stored before it takes anything (`'<name>' is
  not a blob name (...)`), then holds the store's busy flag from the name
  lookup to the end of the payload read, so no `blob write` / `blob erase` /
  `qspi erase` / `qspi test` can change the slot between the two (a held flag:
  BUSY, `blob busy (...)`, before the swap, so the open model stays).  Under
  it: no VALID slot or two VALID slots under the name are ARG (`'<name>': ...
  (`blob list` shows what is where)`), headers that cannot be read are HW; a
  length of 0 or past the staging buffer is ARG (`slot <n> is <len> B,
  staging holds <cap> B`) before a payload byte is read; then the whole
  payload is read (in 64 KB pieces, `BLOB_READ_CHUNK`) and the flag given
  back.  A failed read is HW (`QSPI read failed (<rc>)`); a copy whose CRC-32
  disagrees with the header is HW too (`CRC32 mismatch -- stored <X>, in
  memory <Y>; the blob is intact on the QSPI only if `blob verify <slot>`
  passes, the copy is not`).  Only then is a container split.  All of it is
  `test/test_nn_model_life.c` and `test/test_nn_model_container.c` over a
  stand-in store (`test/nn_blob_stub.c`), including that the flag is taken
  and given back once on every path.
- [!] **Only the first open after boot can build anything**, and on `tflm` it
  builds nothing: the #131 latch is kept for the backends with a model in the
  image (`null`'s stub, `stedgeai`'s generated model), which adopt it there.
  The first open is serialised by the latch and does not return until it is
  done, so an unload typed right after boot finds a finished state.
- [!] **Every build of an interpreter zeroes the whole arena first** (issue
  #167, `nn_tflm_construct_interp()` in `svc/nn/tflm/nn_tflm_glue.h`): a model
  refused at an unregistered op otherwise leaves the rest of TFLM's per-op table
  holding the previous model's bytes, which the destructor calls as a function
  pointer. The cost is one memset of `nn_tflm_arena` (524,288 B, SDRAM) per
  load (until issue #191 the restore of a previous model paid it too).
- [!] **On `tflm` a refused `nn model load` that got as far as the read
  leaves no model** (issue #191).  The backend stages into one 1 MB buffer,
  the bytes the open model lives in, so the shared order takes the plugin
  lease and takes the open model and its plugin down before the payload is
  read, and the backend hands the buffer out only then.  Every refusal from
  the read on (a failed read, a CRC mismatch, a container refused, a model the
  backend refuses) ends `nothing is loaded now`, and the operator loads the
  previous blob again by its name.  The refusals before it (a running stream,
  SDRAM down, the NN session, the plugin lease, and since issue #194 the
  name's rules, the busy flag, the lookup and the length) leave the previous
  model.  From empty a refusal stays empty.  The answer
  comes from the reload itself (whether a model is left), not from whether the
  handle is open.  `stedgeai_reloc` keeps its two slots and still restores the
  previous model.  A model up to 1,048,576 B fits the buffer
  (`nn_tflm_stage_cap`); measured on hardware with the pinned blazeface padded
  to 600 KB, from the SD card at the time.  Since issue #194 the largest slot's
  payload, 1,044,480 B, is the limit in practice, and
  `cmake/check_blob_staging.py` keeps every slot within the buffer.  Cost on `build/f746-tflm`: -64 B of Flash
  (text + data 621,508 -> 621,444 B, `shell.bin` 621,548 -> 621,484 B), 8 B
  less `.bss`.
- [!] **A model whose input or output has a dimension outside 0..65535 is
  refused, `the model was refused (-13)`** (`NN_MODEL_ERR_DIMS`, issue #175):
  `nn_tensor`'s dims are 16-bit, and the descriptor used to be published with
  the dimension truncated.  The tflm backend checks the model's I/O shapes
  (`svc/nn/tflm/nn_tflm_model_check.h`) after the on-board verifier and the
  schema version, before anything is planned, so the refusal ends like any
  other (on `tflm` with no model, issue #191); `verify_tflite` runs the same function.
- [!] **The same walk covers the whole graph, lengths before elements** (issue
  #178): a subgraph without tensors, an operator without an inputs or outputs
  list, an operator list (inputs / outputs / intermediates) longer than 64 --
  judged before any element is read -- or naming a tensor that does not exist
  is `NN_MODEL_ERR_FORMAT` (-7); any tensor of rank above 6 (TFLM's
  `RuntimeShape::kMaxSmallSize`) is `NN_MODEL_ERR_TENSOR` (-4).  The on-board
  verifier counts tables, not the scalar vectors inside them, so before this a
  file within its 4,096 visits could hand `AllocateTensors()` ~1e8 reads on the
  console thread (one shared 65,000-entry shape or operator list named from
  thousands of entries); now every entry is one visit and the walk reads at
  most 192 list elements per operator entry, which bounds those reads in
  `AllocateTensors()` too (its planner's own passes stay quadratic in tensors
  and operators, so the whole load is bounded by visits squared).  The one
  exception to lengths-first: subgraph 0's two I/O lists are walked once, one
  compare per entry, before their count is judged (at most ~128K entries in a
  512 KB file), so that a model broken in two ways stays `NN_MODEL_ERR_FORMAT`.
  The pinned model (blazeface, rank 4, operator lists <= 3) passes unchanged.
  Cost: +352 B of Flash on `build/f746-tflm` (text 612,044 -> 612,396 B); the
  `null` tree is byte-identical.
- [!] **The walk bounds the per-channel quantization too** (issue #180): a
  tensor's scale vector longer than 4,096 (`NN_TFLM_MAX_QUANT_CHANNELS`), a
  zero-point vector neither absent, 1 long nor the scale's length, a
  per-channel tensor whose `quantized_dimension` is outside its declared rank
  (rank >= 2) or outside 0..5 (rank 0 / 1: the converter's rank-1 biases carry
  the filter's qd, TFLM's person_detect qd 3),
  or scale lengths named by all operator entries adding up past 65,536
  (`NN_TFLM_MAX_QUANT_REFS`) is `NN_MODEL_ERR_FORMAT` (-7), judged by lengths
  only.  TFLM copies a tensor's zero points each time a kernel's `Prepare()`
  takes it (freed per node, so the arena never stops it); now those copies
  total at most 65,536 per load (the kernels' own per-channel loops are sized by
  dimension values and bounded by the arena).  Measured maxima over the 43
  models found that parse: 1,280 per tensor, 34,292 summed (TFLM's
  mobilenet_v2); the pinned blazeface int8 (96 / 4,539) passes unchanged.
  Cost: +592 B of Flash on `build/f746-tflm` (`.text` 541,456 -> 542,048 B;
  `shell.bin` 616,836 -> 617,412 B); the `null` tree is byte-identical.
- [!] **The walk checks the buffer indices and the offline plan too** (issue
  #181): a tensor of any subgraph or any metadata entry naming a buffer past
  the buffer vector (or no buffer vector at all), an `OfflineMemoryAllocation`
  entry (the name compared exactly as TFLM does) whose buffer has no data, less
  than the 12-byte header, a tensor count other than all subgraphs' tensors
  together, or fewer offsets than that count, or a model without an
  operator-code vector is `NN_MODEL_ERR_FORMAT` (-7).  TFLM indexes the buffer
  vector with both unchecked (this board builds TFLM with `NDEBUG`, so
  flatbuffers' index assert is gone) and reads the plan's data without a null
  or length check -- the on-board verifier proves offsets, not index values.
  A plan count that disagreed with the graph used to fail inside
  `AllocateTensors()` (`NN_MODEL_ERR_ARENA`) and pass `verify_tflite`, which
  does not plan.  Every plan offset must be -1 (plan online) or >= 0: TFLM's
  planner keeps any other negative value, and its arena check (offset + size)
  let -size < offset < -1 place a tensor before the arena; and below 2^30
  (issue #188, so that `int` sum cannot wrap).  Of the header only
  word [2] is read; buffer 0 need not be empty.  TFLM's person_detect and the 43 models found that parse keep their
  verdict.  Cost: +256 B of Flash on `build/f746-tflm` (`.text` 542,048 ->
  542,240 B, `.rodata` +64 B; `shell.bin` 617,412 -> 617,668 B); the `null`
  tree is byte-identical.
- [!] **The walk checks the constant buffers too** (issue #183): every tensor's
  type must be one TFLM gives a size (a STRING / VARIANT / unknown type used to
  fail inside `AllocateTensors()` as `NN_MODEL_ERR_ARENA` and pass
  `verify_tflite`); a tensor with data must have exactly element count x
  element size bytes of it (1 element for no shape, INT4 two to a byte; longer,
  0-element, negative-dimension and rank-7 constants are refused too), and no
  operator output and no input / output of subgraph 0 may have data -- all
  `NN_MODEL_ERR_FORMAT` (-7).  TFLM compares a constant's data length with
  nothing and a kernel that reads it by its own shape read a short buffer past
  the model in `.sdram.ai` (a constant read by another tensor's dims is the
  operator rules', issue #185, next item); and a tensor with data is not planned into the
  arena, so a graph input or an operator output with data would have been
  written into the model there.  The sparsity table is not read (TFLM micro
  reads a constant dense, so a sparse one is short and refused).  TFLM's
  person_detect and the 44 models found that pass the verifier keep their
  verdict (all 1,320 of their constants are exact).  Cost: +400 B of Flash on `build/f746-tflm` (`.text`
  542,240 -> 542,544 B, `.rodata` +80 B; `shell.bin` 617,668 -> 618,068 B); the
  `null` tree is byte-identical.
- [!] **The walk checks the operators that read a constant by another tensor's
  dims** (issue #185): every operator's `opcode_index` must name an entry of the
  operator-code vector (TFLM compares it only inside `AllocateTensors()`, so it
  was `NN_MODEL_ERR_ARENA` on the board and a PASS in `verify_tflite`), and a
  CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED must have tensors that fit
  together -- ranks, `filter[0]` (CONV_2D) / `filter[3]` (DEPTHWISE_CONV_2D) /
  `weights[0]` (FULLY_CONNECTED) equal to the output's channels, a depth
  multiplier of at least 1 with input channels x multiplier == filter channels,
  FULLY_CONNECTED's output rank 2..4 and input element count, and a bias of
  exactly the channel count and of the type the kernel reads (INT32 under int8,
  FLOAT32 under float32, INT64, or INT32 for CONV_2D, under int16), no grouped
  CONV_2D under an int16 input or an INT4 filter (those kernels read a filter
  row by the input's channels) and a DEPTHWISE_CONV_2D filter[0] of 1 -- all
  `NN_MODEL_ERR_FORMAT` (-7).  The CMSIS-NN int8 kernels read the filter /
  weights and the bias by the OUTPUT's channels and their `Prepare()` compares
  none of it (CMSIS-NN CONV_2D's bias check is dead code: it fetches the bias
  as output 2, always null; the reference kernels' `TFLITE_DCHECK`s are
  compiled out by `NDEBUG`), so a constant exactly its own shape's bytes was
  still read past the model in `.sdram.ai`.  The same holds for the operators
  whose index tensors or shapes size a kernel's reads: PAD's paddings INT32 and
  its input rank equal to the output's; STRIDED_SLICE's begin / end / strides
  INT32 with the input's rank of elements each; TRANSPOSE's permutation INT32;
  RESIZE_BILINEAR / RESIZE_NEAREST_NEIGHBOR's size INT32, rank 1, 2 elements,
  an output of rank 4 and, when the size has data, its two values equal to the
  output's height and width; an ADD / SUB / MUL (inputs 0 and 1, however many there are) output that is the
  broadcast of its inputs; a CONCATENATION whose inputs have the output's rank
  and dims but the axis, and whose axis dims add up to the output's -- all
  FORMAT.  [!] A RESIZE size WITHOUT data is not compared: `Prepare()` refuses
  it on the board (`NN_MODEL_ERR_ARENA`, it must be a constant) while
  `verify_tflite`, which does not plan, passes it -- a difference that predates
  the rule.  TFLM's person_detect and the 44 models found that pass the
  verifier keep their verdict.  Cost: +1,344 B of Flash on `build/f746-tflm`
  (`.text` 542,544 -> 543,880 B, `.rodata` unchanged; `shell.bin` 618,004 ->
  619,348 B; the rules' helpers are out of line, the header's one exception to
  forced inline); the `null` tree is byte-identical.
- [!] **The walk checks what a kernel WRITES against the output's frame** (issue
  #188, step 1): the planner sizes each tensor's frame from its declared shape,
  and some kernels size their writes from something else, compared with the
  output in a `TFLITE_DCHECK` only.  So, all `NN_MODEL_ERR_FORMAT` (-7): every
  tensor of every subgraph has no negative dimension (-1 included, so a
  RESHAPE's -1 is no longer resolved: TFLM's `int` size of such a shape becomes
  a 0-byte frame), a size (elements x TFLM's element size, INT4 1 byte) of at
  most `INT32_MAX - 15` with no partial product past `INT32_MAX` (TFLM's planner
  multiplies in `int`: `[1, 65536, 65537, 1]` int8 is a 64 KB frame), and all
  sizes rounded up to 16 adding up to at most 2^30, with every offline-plan
  offset below 2^30 (the greedy planner adds them in `int`; a rank-7 tensor is
  left to the rank rule, TENSOR).  And: QUANTIZE / DEQUANTIZE / RELU / RELU6 /
  LOGISTIC / LEAKY_RELU / SOFTMAX with an output of the input's element count;
  MAX_POOL_2D / AVERAGE_POOL_2D with input and output rank 4 (below that the
  kernel's loop counts come from past the shape); a PAD's paddings with data
  counted (2 x the input's rank) before they are read, none negative (CMSIS-NN
  moves the write before the output by a negative one), input + before + after
  == output in 64 bits (`Prepare()` adds them in `int`); a TRANSPOSE's
  permutation with data of the input's rank, each in range, and the output
  `output[i] == input[perm[i]]` (the int8 kernel writes by the output's
  strides over the input's dims); a MEAN's axis INT32 WITH data, at most 4 of
  them (the float kernel's stack `axis[4]`), each in range, and the output the
  input reduced over them (`keep_dims` or not).  Pool and RESIZE batch /
  channel mismatches do not write past the output (the kernels loop over the
  minimum) and were not refused in that step.  NOT bounded: the scratch a
  kernel requests in its `Prepare()`, sized by the kernel (the arena check
  stops it).  TFLM's person_detect and the 44 models found that pass the
  verifier keep their verdict (no negative dimension; all of a model's
  tensors together at most 15,114,544 B, the mobilenet_v2 224 of TFLM's xtensa
  example).  A model
  whose input or output declares -1 is now FORMAT rather than DIMS.  Cost:
  +768 B of Flash on `build/f746-tflm` (`.text` 543,904 -> 544,632 B,
  `.rodata` unchanged; `shell.bin` 619,348 -> 620,116 B); the `null` tree is
  byte-identical.
- [!] **Step 2 of #188, the window**: a CONV_2D, DEPTHWISE_CONV_2D or pool must
  have its options table and an output H and W equal to TFLM's
  `ComputeOutSize()` for its padding (SAME or VALID), strides, dilation and
  filter (each at least 1; every intermediate value inside int32, computed in
  64 bits) -- the CMSIS-NN CONV_2D 1x1 fast path sizes its rows by the INPUT's
  n x h x w, and the depthwise opt path memsets the scratch by an output H past
  the formula.  CONV_2D also keeps H, W and C of its tensors and its strides at
  most 65535 (uint16 in the general path); DEPTHWISE_CONV_2D keeps `(output -
  1) x stride` at most 32767 in H and W (its int16 bases) and its batch the
  input's; a pool and a RESIZE keep batch and channels the input's, and a
  RESIZE's input is rank 4 (a policy: those kernels loop over the smaller).
  All `NN_MODEL_ERR_FORMAT` (-7).  TFLM's person_detect and the 44 models found
  that pass the verifier keep their verdict (every conv / depthwise / pool
  output is the formula's, dilation 1).  Cost: +896 B of Flash on
  `build/f746-tflm` (`.text` 544,632 -> 545,584 B, `.rodata` unchanged;
  `shell.bin` 620,116 -> 621,012 B); the `null` tree is byte-identical.
- [!] **Step 3 of #188** (two gaps the review found): DEPTHWISE_CONV_2D also
  keeps the padding `Prepare()` derives (half the window's total) at most 32768
  in H and W, since its int16 bases are `output x stride - padding` (SAME with
  a filter of 65539 over an input of 1 pads 32769 and wrapped them).  A
  TRANSPOSE permutation with a repeated value is refused: CMSIS-NN's int8
  rank-2 path ignores the permutation and writes the input's n x h, so `{0, 0}`
  over [4, 64] into [4, 4] wrote 256 B into a 16 B frame.  Both
  `NN_MODEL_ERR_FORMAT` (-7); person_detect and the 44 models keep their
  verdict.  Cost: +128 B of Flash on `build/f746-tflm` (`.text` 545,584 ->
  545,688 B, `.rodata` unchanged; `shell.bin` 621,012 -> 621,140 B); the `null`
  tree is byte-identical.
- `stedgeai_reloc` is the same: it starts empty, and `builtin` used to mean
  "unload" there before #131 (use `nn model unload`).  Since issue #194 it
  loads a `.bin` by blob name like `tflm`, into its inactive 448 KB model slot
  (`load_region()`): a blob longer than that is refused at the length check,
  before it is read (ARG, `slot <n> is <len> B, staging holds 458752 B`), so
  a `.bin` for this backend goes into a 512 KB slot (3-8).  The staging check
  says NOT APPLICABLE for it (its slot is not the tflm buffer).  Not run on
  hardware: the backend needs a local ST Edge AI Core install.
- `stedgeai_reloc` refuses the same dimension with the same `-13` (issue
  #176). Its `reloc_map()` (`port/nn/nn_stedgeai_reloc.c`) maps each I/O
  `ai_buffer` through `nn_reloc_shape_map()` (`port/nn/nn_reloc_shape.h`,
  walked by `test/test_reloc_shape.c`) and returns its own refusal unchanged:
  a B/H/W/C above 65535 (after 0 becomes 1) is `NN_MODEL_ERR_DIMS` (-13), no
  data pointer or no element size is `NN_MODEL_ERR_TENSOR` (-4), and a tensor
  larger than the activation arena (`RELOC_ACTS_CAP`, 384 KB -- where the
  generated model's I/O buffers live) is `NN_MODEL_ERR_ARENA` (-3), the product
  taken in 64 bits so it cannot wrap. Its own refusals are numbered below
  every `NN_MODEL_ERR_*` so no value means two things (`RELOC_ERR_*` in that
  file): -21 not a usable `.bin` in the inactive slot (slot, length or the
  `.bin` verifier), -22 activations above `RELOC_ACTS_CAP`, -23 RW image above
  `RELOC_RT_RAM_CAP`, -24 / -25 / -26 the ST loader's create / init / report
  failed, -27 I/O count outside 1 input and 1..8 outputs, or no list.
  (`stedgeai`'s shapes are fixed at build time and `null`'s are constants, so
  neither can meet one.)
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
  on `tflm` that includes the payload read, because the open model goes down
  before it (issue #191); on the other backends the read happens before it --
  and a stream, which holds the NN session
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
arena the backend reserves (`tflm`: `nn_tflm_arena`, 524,288 B; `stedgeai_reloc`:
`g_acts`, 393,216 B; `stedgeai`: the generated activations size; `null`: 0),
and `used : N B (activations)` is what the open model's activations actually
take of it (absent with no model, and on `null`). The same two lines as
wio-lite-ai and grove-vision-ai-v2.

Measured on the default (`tflm`, CMSIS-NN) build at #130, fresh configure:

| | null | tflm |
|---|---|---|
| `shell.bin` | 402,932 B | 777,280 B (74.1% of 1 MB) |
| `size` text / data | 398,560 / 4,360 | 772,900 / 4,372 |

Of the 374 KB difference, 189,816 B was the model array (`.rodata`); the rest
is the interpreter, the kernels and the C++ runtime.

At issue #159 Step 4c (the model array and the resident decoder removed; the
plugin machinery of Steps 4a-4b in place), `shell.bin` is 613,388 B on `tflm`
(807,188 B before the step: 193,800 B less, of which 189,816 B is the model)
and 413,764 B on `null` (417,116 B before: 3,352 B less, the decoder).

At issue #161 (the dispatcher and the tflm backend shared with wio-lite-ai),
`shell.bin` is 612,980 B on `tflm`, 368 B less than at 3ead964 (613,348 B):
the op resolver is constructed in the backend's state instead of as a
function-local static, so its atexit registration is gone.  Internal SRAM grew
by 32 B; the backend's state, `nn_tflm_board_state`, is 1,600 B of `.bss`
there.  `nn info`'s backend line now reads `tflm (tflite-micro + CMSIS-NN)`,
taken from the build configuration (kernel set) as on wio-lite-ai.

SDRAM bank3 (`.sdram.ai`, `0xC0600000`, 2 MB) under `tflm`, after #130 step 6b
removed the camera staging buffers (`nncam_stage`, 393,216 B) and #159 Step 4c
the resident decoder's candidate scratch (`nn_dec_scratch`, 1,536 B):

| Object | Address | Size | Owner |
|---|---|---|---|
| `nn_tflm_model_buf` | `0xC0600000` | 1,048,576 B (one buffer since issue #191; 2 x 512 KB before, the same bytes) | `port/nn/tflm/nn_tflm_bufs.c`, model staging buffer |
| `nn_tflm_arena` | `0xC0700000` | 524,288 B | `port/nn/tflm/nn_tflm_bufs.c`, activation arena (holds the input tensor) |
| free | `0xC0780000` | 524,288 B | |

`check_f746_layout.py` requires `nn_tflm_arena` and `nn_tflm_model_buf` in bank3
in a `tflm` build only (C globals since #161; until then they were
anonymous-namespace objects `g_arena` / `g_sd_model_buf`, named by their mangled
local symbols, at the same addresses). A `null` build is not asked for them.
[!] The gate checks the bank, not the address: the order in the table above
(`nn_tflm_model_buf` first) comes from the compiler emitting the two
definitions of `port/nn/tflm/nn_tflm_bufs.c` in reverse order, so the addresses
are what the #161 map says (`_esdram_ai` still `0xC0780000`), not a promise. The
negative tests are `cmake/fixtures/run_layout_tests.py`, run by the host test
suite.

**What the camera producer writes is covered by the same requires.** Since #130
the producer preprocesses each frame straight into the model's input tensor,
and that tensor is not an object of its own: TFLM allocates it inside `nn_tflm_arena`
(and the `null` backend's is `null_in_buf`, required in a `null` build). So the
`nn_tflm_arena` require is also the placement guarantee for the producer's writes --
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
| `nn info` arena line | `470352 B reserved` -- [!] labelled "reserved" but it was the arena TFLM actually USES; since #131 that figure is the `used` line and `arena` is the reservation, `nn_tflm_arena`, 524,288 B |

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

At issue #161 (2026-10-31, against the 3ead964 baseline log): `nn stream start
--frames 100` gave 9 inferences in about 6.7-7.1 s, and `nn bench` averaged
633.3 ms at 216 MHz; apart from the backend line the console output matched
the baseline.

At issue #163 (2026-11-01, `/blazeface.nnc`): `nn bench 10` gave min 630,885 /
avg 635,996 / max 638,293 us at 216 MHz.  [!] Since #163 the bench fills its
input with `0x5A` (`svc/nn/nn_bench.c`, the byte every board uses) where this
board filled `0x00`, so bench figures from before and after #163 are not
strictly comparable.  The `cli` stack high-water was 1,216 / 4,096 B at boot
and 2,748 / 4,096 B after `nn model load --path /blazeface.nnc` (from the SD
card, before issue #194), unchanged by
`nn bench 10` and by a stream start / stop: the container load is the deepest
console path, not the bench (the 2,156 B in the table above was taken after
`nn bench 10` with the built-in model, before containers).

At issue #173 (2026-11-02, `/blazeface.nnc`): bank 3 (MPU region 1) changed
from write-back write-allocate (TEX=1 C=1 B=1, `RASR` `0x130B0029`) to
write-back no write-allocate (TEX=0 C=1 B=1, `RASR` `0x13030029`).  The
reproduction the issue asked for (three or more runs before the code changed)
was taken by rewriting region 1's `RASR` with `devmem` within one boot, A
(write-allocate) / B (no write-allocate) / A again, on an image whose layout
gave no DCMI overruns (`ovr dcmi` 0 throughout; the stream latency row is that
run's last reported latency):

| Measurement | A (write-allocate) | B (no write-allocate) |
|---|---|---|
| `nn bench 3` | 636.4-637.6 ms (9 samples) | 563.5-565.4 ms (6 samples) |
| `nn stream`, 60 s | 1.49 inf/s (both A phases) | 1.65 inf/s |
| stream latency | 637 ms | 566 ms |

Acceptance was then taken on two images that differ in exactly one byte (the
`RASR` value in `bsp_init`'s literal pool, checked with `cmp -l`): A = main
before the change, B = after, A again, each flashed and booted fresh.  Both
images have the overrun-prone layout of main at c74991f, so the DCMI overruns
(#172) are the same on both and are counted per operation:

| Measurement | A | B | A again |
|---|---|---|---|
| `nn bench 3` x3 (avg) | 633.6 / 635.5 / 637.3 ms | 565.6 / 564.3 / 565.1 ms | 631.5 / 635.1 / 632.7 ms |
| `nn stream start --frames 300` | 1.22 inf/s, latency 646 ms | 1.42 inf/s, latency 577 ms | 1.27 inf/s, latency 643 ms |
| `ovr dcmi` during bench x3 / during the 300-frame stream | (not split) | 4 / 12 | 4 / 12 |
| `/pinned.tflite` `nn bench 3` x3 (avg) | 634.3 / 631.1 / 634.4 ms | 564.1 / 565.6 / 564.2 ms | (not taken) |
| `nn run` x5, `lcd info`, `sdram test` | faces 0 x5, underrun=no, 6144 KB OK | same | (not taken) |

[!] Known limit: only `tflm` was measured, and only for `tflm` (and `null`) is
bank 3 known to be CPU-only.  The two `stedgeai` backends get the same
region 1 attribute, but that their generated code and the X-CUBE-AI runtime
never hand bank 3 to a DMA has not been checked.

## The QSPI asset store (`blob`, issue #194)

Models and containers live on the on-board 16 MB QSPI NOR (N25Q128A) in named
slots, the same store wio-lite-ai has on its external NOR: the same slot table,
the same header format and the same `blob` command, so `blob write` and `nn
model load --name` mean the same thing on both.  Until issue #194 the QSPI NOR
was a LevelX + FileX volume (the `fs` command) and models were files on the
microSD card; both are gone (see [Moving models off the SD
card](#moving-models-off-the-sd-card)).

What is shared and what is this board's:

| Part | Where | What |
|---|---|---|
| the command | `shell/cmds/cmd_blob.c` | every `blob` subcommand, its refusals and its words, one file for all three boards ([svc/README.md](../../svc/README.md#the-asset-stores-shared-parts-issue-192)) |
| the store's rules | `svc/blob/` | the header format, the name and slot rules, the order of one `blob write` (`blob_write.c`), the result codes (`blob_err.h`), the contract the command calls (`blob_svc.h`) |
| store side | `src/blob.c`, `src/blob.h`, `src/blob_slots.c` | the interval and the slot table, the header and payload reads, the erases, the name lookup and the busy flag |
| write side | `src/blob_writer.c` | the functions `blob_svc.h` names: the 1 KB staging buffer, the reservation (the busy flag), the program and the read-back, each held to the store's interval, and the log lines a transfer leaves |
| console side | `cmds/cmd_blob_board.c` | YMODEM over the VCP, the refusal on the telnet console, the busy and driver-down words, the VCP's RX ring high-water |
| the driver | `port/qspi/qspi_flash.c` | indirect-mode reads, page programs and erases, one mutex per call |

| QSPI range | What |
|---|---|
| `0x000000 .. 0x100000` | reserved, no slot (kept free to match wio-lite-ai's layout; `qspi erase` / `qspi test` reach it) |
| `0x100000 .. 0x800000` | the blob store: 13 slots, below |
| `0x800000 .. 0x1000000` | unallocated, no slot |

| Slots | Base | Size | Payload |
|---|---|---|---|
| 0-2 | `0x100000`, `0x200000`, `0x300000` | 1 MB | 1,044,480 B |
| 3-8 | `0x400000` .. `0x680000`, every 512 KB | 512 KB | 520,192 B |
| 9-12 | `0x700000`, `0x740000`, `0x780000`, `0x7C0000` | 256 KB | 258,048 B |

The first 4 KB subsector of a slot is its header (the shared format's two
256 B pages, magic page first and written last); the rest is the payload.
`test/test_blob_slots.c` pins every entry.  The largest class is 1 MB because
that is the tflm staging buffer (`NN_TFLM_MODEL_KB`): the shared
`cmake/check_blob_staging.py` at the repository root (wio-lite-ai's until
#194) reads the table and the buffer's capacity back out of the linked image
and stops the build if any slot's payload is larger.  With `null`,
`stedgeai` and `stedgeai_reloc` it says NOT APPLICABLE and checks the image
has no tflm staging buffer (`test/host_tests.sh` runs its shared tests,
`cmake/fixtures/run_blob_staging_tests.py`, with this board's directory, so the
4 KB header unit `board.cmake` passes is compared with `src/blob.h`);
`stedgeai_reloc`'s own 448 KB limit is enforced
at load time instead (see [the model](#the-model-loaded-by-name-from-the-qspi-store-or-nothing-issue-131-159-step-4c-194)).
The host checker `verify_tflite` is built with the same table.

### Commands

The `blob` subcommands and their rules are the shared command's, described
once in [svc/README.md](../../svc/README.md#the-blob-command-is-shared-issue-194):

```
blob list | info <slot> | read <slot> [off] [len] | free
blob write <name> [slot]     erase the slot, receive a file over YMODEM, verify
blob verify <slot> | erase <slot>
nn model load --name <name>  load the blob stored under <name>
```

What is this board's:

- **[!] The store's reads answer busy while its busy flag is held**, unlike
  wio-lite-ai's: `blob list`, `blob info`, `blob read`, `blob verify` and
  `blob free` answer `blob: another QSPI operation is running (...)` while a
  `blob write` / `blob erase`, a `qspi erase` / `qspi test` or an `nn model
  load --name` (lookup to end of read) runs on another console or in the
  background, rather than describe flash that is being changed.  They only
  look at the flag, so two readers do not refuse each other.  `qspi erase` and
  `qspi test` take the flag for their whole run (`qspi: busy -- ...` when they
  cannot); `qspi probe`, `qspi info` and `qspi read` do not look at it.
- **The erase is one 64 KB sector at a time** where a whole sector is left,
  4 KB subsectors elsewhere (`blob_erase_span()`; every slot is whole 64 KB
  sectors, so in practice all sectors), one erase per driver call, so the
  driver's mutex is given back between them and Ctrl+C stops the erase between
  sectors.  The announcement uses the datasheet's 64 KB figures
  (`svc/blob_svc_config.h`: 0.7 s typical, 3 s at most): about 3 / 6 / 12 s
  for a 256 KB / 512 KB / 1 MB slot, at most 12 / 24 / 48 s.  `blob erase
  <slot>` erases the header subsector only.
- **`blob write` is VCP only.**  From the telnet console it is refused before
  anything is touched (`ymodem: refused on the telnet console -- it would
  destroy this session. Run it from the VCP console.`; YMODEM over telnet is
  issue #197), and a background `blob write ... &` is refused by the shared
  command.
- **[!] The VCP's RX ring is 4,096 B** (`CLI_UART_RX_BUFFER_SIZE` in
  `board.cmake`; 256 B before #194).  At 115200 baud that is about 355 ms of
  line time, against a 1 KB YMODEM block while the same thread programs the
  QSPI.  `blob write` ends with one line about it:

  ```
  rx drops : 0, ring high-water <n> of 4096 B  (clean)
  ```

  `rx drops` is what the ring dropped during this transfer (`<-- NOT CLEAN`
  when it is not 0), and the high-water is the fullest the ring got during it
  (restarted at the transfer's start from what the ring held then).  **The
  pass line is 0 drops and a high-water at or below half the ring (2,047 B,
  as `board.cmake` states it)**; a clean transfer near the top says the
  margin is gone even though nothing was lost.  The drop count also goes to
  `dmesg`, with the transfer's result.
- **The write side** (`src/blob_writer.c`) follows the shared order (body
  page, payload, read-back of the whole payload against the CRC of the
  stream, magic page last).  Every program and read-back it is asked for is
  checked to lie inside `0x100000 .. 0x800000` first (wio-lite-ai's writer has
  no such check), so a wrong address cannot reach the reserved first megabyte
  or the top.  A program is not read back page by page: `qspi_flash_write()`
  checks the flag status register after every page, and the body page carries
  its own CRC, so a body that programmed wrongly reads `invalid`.
- **[!] A plugin may not define the store's writers**: `F746_PLUGIN_FORBIDDEN`
  in `board.cmake` adds `qspi_flash_write`, `qspi_flash_erase_sector`,
  `qspi_flash_erase_subsector`, `qspi_flash_erase_chip`, `blob_erase_header`,
  `blob_erase_span`, `blob_svc_erase_header`, `blob_svc_erase_span` and
  `blob_svc_program` to the internal-flash, clock and MPU entries, and the
  plugin image gate refuses a plugin that defines any of them.
- `empty` means "no header", not "blank flash": the payload underneath may
  still hold old data.

### Moving models off the SD card

A board updated in place still has whatever the LevelX volume left on the
QSPI NOR: the volume covered the whole part in 64 KB blocks, so a slot base is
where a LevelX block header sat, and such a slot is expected to read `invalid`
(`magic page is neither erased nor a header`) until `blob erase <slot>`.  A
part that never held a formatted volume reads `empty`.  Read `blob list`
first.

The models that used to be on the card are sent again, one slot each (the
assignment below is provisional):

| File on the card | Size | Slot | How |
|---|---|---|---|
| `/blazeface.nnc` | 194,232 B | 12 (256 KB) | rebuild it (`--target asset-blazeface`) and follow the receipt; its CRC moves with the build id, so a copy from the card is compared with nothing |
| `/pinned.tflite` (the bare pinned blazeface) | 189,816 B | 11 (256 KB) | `verify_tflite` it (`--target verify-model`), then `blob erase 11`, `blob write pinned 11` and send it; compare `blob list` with the CRC `verify_tflite` printed |
| a `stedgeai_reloc` `.bin` | up to 448 KB | one of 3-8 (512 KB) | `blob write <name> <slot>`; the load refuses a blob longer than 448 KB |

A file that is only on the card can be brought to the PC with `xfer send sd
<path>` first.  The name a blob is loaded by is the one typed at `blob write`,
not the file's.

### Measured

| | before #194 (`c4ebdd6`) | after (`dce051d`) |
|---|---|---|
| `shell.bin` (`tflm`) | 621,492 B | 629,436 B (+7,944 B: the store, the shared command, YMODEM receive; the LevelX + FileX volume -8,408 B) |
| `shell.bin` (`null`) | 417,164 B | 424,404 B |
| internal SRAM `.data` + `.bss` (`tflm`) | 110,768 B | 101,624 B (-9,144 B: the volume's buffers out, the 4 KB RX ring and the 1 KB staging buffer in) |
| `nn model load --name` deepest stack | | about 2.0 KB of the 4 KB console stack (from the `.su` files; the QSPI read side) |

A 1 MB payload is read for a load in sixteen 64 KB pieces (`BLOB_READ_CHUNK`),
about 250 ms at the 4 KB/ms `qspi test` measured, each piece far below the
driver's 5 s timeout, with interrupts on and the IWDG fed by its own thread.

Not yet measured on hardware (to be filled in after the first run):

| | Value |
|---|---|
| 64 KB sector erase, as `blob write` sees it (one 256 KB / 512 KB / 1 MB slot) | not measured |
| 1 MB `blob write` transfer at 115200 baud | not measured |
| RX ring high-water during `blob write` (idle; with `nn stream`, GUIX, telnet and MJPEG running) | not measured |
| `nn model load --name` of a 1 MB-class blob | not measured |
| `blob list` on the first boot of the #194 firmware (updated in place) | not seen |

## Plugin containers (issue #159 = #78 Step 4)

A **container** carries a model together with the code that interprets its
output (a *plugin*).  The format, the validator and the build rules are shared
with grove-vision-ai-v2 and wio-lite-ai (their READMEs have the design); this
section is what is different here.  Step 4a recognises, validates and reports a
container **without ever executing one**; Step 4b runs the plugin and builds
this board's own containers; Step 4c removed the resident decoder and the
built-in model, so a container is the only way a decoder reaches this board.

### The target word

This board's plugin target word is `0x1101` (Cortex-M7 / FPv5-SP-D16 / hard
float / little-endian, no CMSE), in every backend.  It is checked at both ends:
`port/nn/nn_svc_f746.c` static-asserts it against this firmware's predefined
macros (`svc/plugin/plugin_target.h`; also the only check of the CMSE bit), and
**because those macros are exactly a Cortex-M4 with FPv4's too** (GCC 15.2:
the two predefine the same set), `cmake/check_target_word.py` checks it against
the linked image after every build: its `Tag_FP_arch` says FPv5, which an
M4/FPv4 image's does not.  `cmake/fixtures/run_plugin_gate_tests.py` builds an
M4/FPv4 firmware image and watches it refused under `0x1101`.

### Loading one (Step 4a, `tflm` only)

Since issue #194 a container reaches the board the way it reaches
wio-lite-ai: sent into a slot of the [QSPI asset
store](#the-qspi-asset-store-blob-issue-194) with `blob write`, and loaded
with `nn model load --name <name>`.  (Until then it was a file the operator
copied to the microSD card and loaded with `nn model load --path`, and the
reader, `cmds/cmd_nn_board.c` with `svc/nn/nn_container.c`, printed the CRC of
the bytes it read; both are gone.)

- The adapter reads the whole blob into the backend's staging buffer, which
  the backend hands out only once the open model is down (issue #191), and
  compares the CRC-32 of the copy with the header's before it probes it.
  **The closing check** is now the store's: `blob list`'s crc32 for the slot
  must equal the asset receipt's CRC, because nothing on the PC checks that
  the file sent is the file the build published.  The load prints no CRC.
  Measured at Step 4a on hardware (from the SD card): reading and validating a
  container on the console thread raised the `cli` stack peak from 2,096 B to
  2,652 B of 4,096 (`thread`, the wio container refused by target word); the
  inference worker's peak (1,516 B) did not move.
- The container goes through `svc/plugin/plugin_load.c` -- the validator the
  host asset build runs -- with this board's policy (target word `0x1101`, link
  address `0x20048000`, capacity 32 KB, the stack table in `board.cmake`).  A
  refusal names its reason (`slot <n> is a container this firmware refuses:
  built for another target`, `... size disagrees with payload`, `... digest
  mismatch`, ...) and ends with no model and no claims: since issue #191 the
  open model went down before the read (before, it changed nothing).
- Its **model section is handed to the backend in place**, at its offset
  inside the staged file.  The tflm backend now accepts any range wholly inside
  its staging buffer that starts on `NN_MODEL_ALIGN` (16 B) -- identified by
  containment, not equality, as on wio-lite-ai since #108 --
  so the validated container is never overwritten with unvalidated bytes.
  The rule is `svc/nn/tflm/nn_tflm_fit.h`, a pure function the backend calls
  and `shell/test/test_nn_tflm_fit.c` walks at its boundaries (since #161 it
  also refuses the live slot, as wio-lite-ai's always did).
- **Since Step 4b the plugin is started** (below): after the backend has
  adopted the model section, the shared load order copies the plugin into
  `.plugin` and calls its entry, under the plugin lease.
- `nn info` then shows the container's claims:

  ```
  plugin  : <name> (build <id>, crc <plugin digest>), running
    from  : slot <n>, blob '<name>'
    image : ...
    stack : ...
    decl  : ...
  ```

  The `from` line names the slot and the blob the claims came with, as on
  wio-lite-ai (until issue #194 it was `<path>, <n> B, file crc32 <CRC>`; the
  whole container's CRC is now `blob list`'s).  The plugin line's `crc` is the
  plugin section's digest, a different number.
  `running` is the loader's own state, read in the same critical section as the
  claims; `validated, not loaded` is a container whose plugin the loader
  refused (the model stays open -- issue #122 D6).  A model-only container
  shows `plugin  : (none in this container)` and still its `from` line.  A bare
  load or `nn model unload` clears the claims (`plugin  : (none) --
  reservation 32768 B at 0x20048000`); a refused load leaves the previous ones.
  The store, the snapshot rule and these lines are `svc/plugin/plugin_claims`,
  shared with wio-lite-ai.

**Step 4a's provisional c (640 B, `F746_PLUGIN_VENEER_COST_4A`) is gone.**  The
policy's c is `PLUGIN_VENEER_BASE_COST`, which `veneer_cost_gate()` derives and
checks against the shipped image and compiles in; the build reads the policy
back out of `shell.elf` (`cmake/check_policy_probe.py`).  Since issue #165 the
policy is `plugin_board_policy` in `port/nn/nn_svc_f746_model.c`, read by that
name by both the loader (which takes no policy argument) and the check, and the
function behind each plugin veneer is a word of `plugin_board_bindings` in
`port/nn/nn_active.c` -- the base handed to `entry()` at its head, then the
painter's three and the printer's write -- which the veneer check reads out of
the image instead of a list in `board.cmake` (`cmake/README.md`).  The adapter refuses
to compile if the provisional macro reappears.

### Running one (Step 4b, `tflm` only)

What runs where, and why each piece is this board's or shared:

| Slot | Thread | Allowance | Who calls it |
|---|---|---|---|
| entry | console (`nn model load`) | 1,024 B | the shared loader, `svc/plugin/plugin_run_core.c` |
| shapes_ok | console (`nn run`, `nn stream start`) | 1,024 B | the start's admission, under the lease |
| decode | nn worker (priority 18) | 1,024 B | `port/nn/nn_worker.c`, lease held across decode + publish |
| draw | GUIX (priority 14) | 1,024 B | the CAMERA_FRAME handler (`ui/guix_camera_ui.c` -> `port/nn/nn_panel.c`) |
| report | console (`nn run`, `nn dets`) | 1,024 B | `nn_camera_decode_get()`, snapshot and report in one hold |
| param_set / param_get | console (`nn thresh`) | 1,024 B | `nn_svc_thresh_*()`, under the lease |

- **The lease** is this board's ThreadX mutex with inheritance
  (`port/plugin/plugin_lease.c`, the same implementation as the other boards').
  Every entry into the plugin is checked by the shared entries
  (`svc/nn/nn_active_lease.c`, `svc/plugin/plugin_run_core.c`); a caller that
  does not hold it gets `NOT_HELD`, counted in `nn stream stats`
  (`entry(s) refused unheld`).  Order: always the lease, then the frame lock
  (`ltdc_lock_frame()`), then the record lock (`nncam_lock`).
- **The draw** runs where the resident boxes used to: in the GUIX thread's
  CAMERA_FRAME handler, between the producer's store into the view buffer and
  the icon redraw.  `nn_panel_draw()` TRIES the lease (a busy lease is a frame
  shown bare, counted as a miss), takes the frame lock, asks the shared
  questions (the overlay is derived -- a stream, not a `nn run` -- and the
  record is valid, of this session, and the plugin's), lets the plugin paint
  into the view buffer with the CPU (`port/plugin/plugin_paint.c`, row-major
  RGB565, no DMA2D), marks both LTDC buffers stale and releases the lock.  With
  no plugin in force it answers NONE and nothing is drawn (Step 4c removed the
  resident decoder's boxes).
- **The geometry**: the producer squashes the whole camera frame onto the model
  input, so `to_frame()` multiplies by the frame size the stream attached with
  (QVGA or QQVGA) -- the same frame the preview stores.
- **The loader binding** (`port/plugin/plugin_run.c`) is the MPU read-back
  under a critical section, judged by `svc/plugin/plugin_mpu_v7m.c`, and the
  D-clean + I-invalidate of the reservation.  `test/test_plugin_mpu_f746.c`
  holds the verdict on this board's real region set: SDRAM non-cacheable XN,
  bank 3 write-back no-write-allocate XN (#173), PRIVDEFENA -- the reservation
  is executable through the default map -- and refuses the background map off
  and an XN / Device / read-only region placed over it.
- **A plugin's fault** is attributed (`pc is in plugin '<name>' +0x<off>` in the
  RAM log) and **the board resets at once** (`src/fault_plugin.c`); under a
  debugger it halts instead.  A fault anywhere else keeps the full UART dump
  and the halt the IWDG resets.  `crash` is outside the reservation and behaves
  as before (seen at the Step 4b acceptance with `crash bus`).
  [!] **The attribution is by the stacked PC, and only a PC inside the
  reservation is the plugin's -- on purpose.**  A plugin that hands a firmware
  function a bad argument (a rectangle to `paint_rect`, a length to
  `nn_report_write` or `log_write_bytes`) faults with the PC in the firmware,
  so that fault takes the ordinary path: the UART dump, then the IWDG reset.
  The handler names where the PC is, not who caused it; the backtrace in the
  dump (and the plugin being the one loaded) is what points back at it.
- **The asynchronous reserve is 208 B** on this core and ThreadX port: the
  extended exception frame (26 words + the alignment pad; PM0253 2.3.7,
  STKALIGN reads 1) plus ThreadX's switch save (r4-r11, s16-s31, LR).  Nested
  exceptions stack on the MSP, and FPCCR ASPEN / LSPEN (reset 1, written
  nowhere on this board) can only shrink the frame.  Each allowance must fit
  "thread stack - depth at call - 208 - margin" on the shallowest thread of its
  slot; the depth is what `nn stream stats` prints (`at call`), sampled at the
  indirect call and, for entry, in the loader's exec_ok hook.  Measured at the
  Step 4b acceptance (blazeface container, a stream, `nn run`, `nn thresh`,
  `nn dets` from both consoles):

  | Thread (stack) | Slots | `at call` high-water | Allowance | Left after 208 B reserve |
  |---|---|---|---|---|
  | nn-worker (4,096 B) | decode | 428 B | 1,024 B | 2,436 B |
  | GUIX (4,096 B) | draw | 412 B | 1,024 B | 2,452 B |
  | console (4,096 B) | entry, shapes_ok, report, params | 2,240 B | 1,024 B | **624 B** (the shallowest) |

  `thread` peaks over the same run: `cli` 2,748 B, `nn-worker` 1,504 B, GUIX
  776 B.  At issue #161 the nn-worker's `at call` high-water is 432 B (2,432 B
  left); the console stays the shallowest.  Issue #165 made the report path
  8 B shallower by the ELF (`nn_active_report_to` tail-calls
  `nn_active_core_report_to` as `nn_active_report` did, and the latter's frame
  is 32 B where `nn_active_core_report`'s was 40) and left entry unchanged;
  the review of #165 made the shared code build the painter, which puts the
  draw 8 B deeper on the GUIX thread (`panel_paint` 32 B where it was 64, and
  `nn_active_core_draw_on` 40 B calling the plugin where `nn_active_core_draw`
  was 16 B and tail-called it) -- 412 + 8 is still far under the 1,024 B
  allowance.  Not re-measured, so the console figure above stays an upper
  bound and the GUIX one is 8 B short of it.

#### The draw budget

A plugin's draw is charged pixels and primitives (`svc/plugin/plugin_paint_budget.c`);
the budget bounds how long the GUIX thread holds the frame lock, which the
producer needs to store the next frame into the view buffer.  **Provisional:**
an eighth of the frame (9,600 px at QVGA) and 64 primitives
(`port/nn/nn_panel.c`) -- not wio-lite-ai's number.  The acceptance measures it:

1. `camera stream stats` -> `slots` and the frame rate: the producer can wait
   on the lock about `(slots - 2) x frame period` before the DCMI ring runs out
   of free slots (the DMA owns one slot, the producer the one it is storing).
2. Load the blazeface container, `nn stream start`, face the camera, and let it
   run; `nn stream stats` prints `plugin  : frame lock held <n> us max by a
   draw` -- the longest hold a plugin draw caused -- and `drew <px> max/frame
   of <budget>`.
3. The budget stands when the longest hold of a full-budget draw stays well
   inside step 1's slack and the overrun rate over the 10-minute stream is not
   above the baseline (19 overruns / 600 s, tflm, `/pinned.tflite`).

**Measured at the Step 4b acceptance, and the budget stays at an eighth of the
frame.**  The blazeface plugin drew at most 884 px of the 9,600 px budget per
frame, refused 0 primitives, and held the frame lock at most 64 us; the
producer's slack is 6 slots x 67 ms.  Over a 624 s stream with the preview
visible and both consoles issuing `nn` commands, `camera stream stats` counted
2 DCMI overruns (the baseline: 19 / 600 s).  After `nn stream stop`, with the
preview still running, `missed` stayed 0 (the panel asks for the overlay
before it tries the lease).  A pass through the GUIX settings screen and back
was fine.

#### Building this board's containers

`cmake --build build/f746-tflm --target asset-blazeface` builds
`build/f746-tflm/asset/blazeface.nnc` -- the pinned BlazeFace (the same commit
and SHA-256 as the other boards'; until Step 4c it was also this board's
built-in model) with this board's blazeface plugin -- through every gate, and prints the receipt:

```
  asset blazeface    .../build/f746-tflm/asset/blazeface.nnc
  slot 12            194232 B   crc32 <CRC>

  on the board:  blob erase 12   (the slot must be empty first)
                 blob write blazeface 12   (erases the slot, then waits for the file)
  in picocom:    C-a C-s, then the path above
  afterwards:    `blob list` must show crc32 <CRC> -- that is how you know
                 the bytes that were built are the bytes that were stored.
  then:          nn model load --name blazeface
```

The destination is a SLOT (`cmake/add_asset.cmake`, issue #194), the same
receipt as wio-lite-ai's: its fit is checked against that slot's payload in
the table the firmware links (`plugin/slot_table.json`, compiled on the host
from `src/blob_slots.c` with the shared lookups, `scripts/slot_table.c`), and `blazeface` goes to slot 12, the last 256 KB slot, as on
wio-lite-ai (provisional; the slot is one argument in `board.cmake`).  Until
#194 the destination was a FILE checked against `nn_tflm_stage_cap`, and the
receipt said to copy it to the card and compare with the CRC the load
printed.  The CRC moves with the build id (the HEAD the tree was configured
at), so compare with the receipt of the build that produced the file you
sent.  The `cifar10` plugin
is built and gated too, but no asset carries it: no CIFAR-10 model this
backend runs is pinned in the tree (Grove's is a Vela model for the Ethos-U).

Measured at Step 4b (`build/f746-tflm`): Flash 807,220 B; plugins blazeface
text 4,092 B / bss 4,816 B (decode 784 / draw 716 / report 728 B with c = 640),
cifar10 text 2,764 B / bss 8,852 B; the stack below a veneer derived from the
image is 192 B (the log path) against the declared 640 B; `blazeface.nnc` is
194,232 B (its CRC is the receipt's: it moves with the build id).

With the container loaded, `nn run` prints the plugin's own words, in frame
pixels (the base's `to_frame()`), e.g.:

```
faces 1 thresh 600/1000
face 0 x 230 y 78 w 63 h 47 px score 859
```

Before #160 the first SD read of a large file could fail while the GUIX
camera preview ran (`sd: read failed: I/O error`, an SDMMC RX overrun, about 1
load in 3; models still came off the card then).  The cause was the display flip, not the SD driver or the container
path: see "The flip writes only the layer address" below.  SD hardware flow
control stays off.

`scripts/verify_tflite.cc` (`--target verify-model`) is this board's model gate
for that asset build: the tree's own `VerifyModelBuffer()` with the limits the
firmware's on-board verifier (`NN_TFLM_VERIFY=1`) and grove-vision-ai-v2 use --
depth 16, 4,096 table visits, `nn_tflm_verify_model_buffer()` in
`svc/nn/tflm/nn_tflm_model_check.h` (issue #175; a model the board would refuse
as malformed is refused here first) -- the length (since issue #194: the largest
payload of the slot table it is compiled with, `src/blob_slots.c`, then the
staging buffer's `NN_TFLM_MODEL_KB`; it names the smallest slot class that
holds the file, and `cmake/fixtures/run_verify_tflite_size_tests.py` pins both
sides, including the old 1 MB limit, now refused), the I/O check the backend runs before planning (issue
#175, #178, #180, #181, #183, #185, #188: `svc/nn/tflm/nn_tflm_model_check.h` compiled in -- a missing,
too long (> 64) or out-of-range I/O or operator list, a subgraph without
tensors, quantization vectors too long or inconsistent, a buffer index past the
buffer vector, an offline plan that does not match the graph, no
operator-code vector, a tensor type without a size, a negative dimension or a
tensor size past an `int` (or all of them past 2^30, #188), a constant whose data is not
exactly its shape's bytes, data on a tensor the graph writes, an operator code
index past the code vector, or an operator whose tensors do not fit together
(conv / depthwise / FC channels and bias, PAD / STRIDED_SLICE / TRANSPOSE /
RESIZE index tensors, ADD / SUB / MUL broadcast, CONCATENATION shapes, #185;
elementwise element counts, pool ranks, PAD / TRANSPOSE / MEAN output shapes,
the conv / depthwise / pool window, #188), is
`NN_MODEL_ERR_FORMAT`, a tensor above rank 6 (an I/O tensor above
4) `NN_MODEL_ERR_TENSOR`, an I/O dimension outside 0..65535 `NN_MODEL_ERR_DIMS`), and the operators the firmware registers -- read from
`svc/nn/tflm/nn_tflm_ops.h` under the `extended` profile, the same list
`svc/nn/tflm/nn_tflm.cc` registers from.  `test/host_tests.sh` runs it on
generated files wrong in exactly one way (`cmake/fixtures/run_model_io_gate_tests.py`,
against `build/f746-tflm` or `F746_TFLM_BUILD_DIR`; a loud SKIP without one),
including 4,096 table visits (passes) and 4,097 (refused at the verifier).

The on-board verifier runs on `nn model load` on the console thread.  Its stack
(issue #175 step 2, `cmake/thumb_stack_analysis.py` over `build/f746-tflm/shell.elf`,
frames in bytes): `cmd_nn_model_load` 376 + `nn_svc_model_load` 432 +
`nn_core_model_load` 48 + `nn_life_swap` 8 + `nn_model_reload` 48 +
`tflm_bk_reload` 48 + `build_interp` 120 + the verifier chain 300 (`Model` 72,
`SubGraph` 48, `Tensor` 40, `SparsityParameters` 48, `DimensionMetadata` 40,
`VerifySparseIndexVector` 28, `Int32Vector` 24) = **1,380 B, 1,588 B with the
208 B exception reserve, of 4,096 B**.  Two of the links are indirect calls
(the board's `swap` and the backend's `reload`) and are added by hand.  The
limits do not change this: the chain is the schema's (depth 6 at most), the
same under any `max_depth`.  Flash at step 2: 616,452 -> 616,388 B, 64 B
smaller -- the bounded verifier 128 B smaller (the inliner folded the op
resolver into `build_interp`), the I/O walk's count and rank checks 64 B larger.

Measured at Step 4a (`build/f746-tflm`): Flash 796,868 B (was 792,100; the
validator, the CRC and the claims), `RAM` 115,584 B of 224 KB.

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
hint the other boards get.  So `check_f746_layout.py` names what has to be
there, per backend: `nn_tflm_arena` and `nn_tflm_model_buf` on `tflm`, `null_in_buf` on
`null`, `stai_acts` on `stedgeai`, and `g_acts` and `g_rt_ram` on
`stedgeai_reloc` (whose executable model slots, `g_model_slot`, are required in
the `0xC0700000` window).  From issue #97 to #159 Step 4c it also required the
resident BlazeFace decoder's candidate scratch, `nn_dec_scratch`,
unconditionally -- the only bank3 check the two `stedgeai` backends had until
Step 4c named theirs -- and forbade the decoder's state in SDRAM
(`--forbid-sdram`, removed with it: nothing else used it).  The blazeface
plugin's scratch is in its own `.plugin` reservation.

The shared decoder (`svc/nn/blazeface.c`) still owns NO storage, and
`cmake/check_no_mutable_storage.py` still refuses a static added to it: it is
audited as the blazeface plugin's `AUDIT_SHARED` source with the plugin's
compile command.  Since issue #117, firmware coverage is derived across the
actual `svc_obj` and `shell` producers (`cmd_nn.c`, `nn_cmd_core.c`); each
audit replays its producer's compile command. See the
[shared storage gate contract](../../cmake/README.md).

## Commands

```
blob      camera    console   coremark  crash     devmem
dmesg     echo      free      gui       help      jobs
kill      lcd       membench  net       nn        qspi
reboot    sd        sdram     sleep     thread    touch
uptime    usleep    version   watch     wdt       xfer
```

`help` lists them with one-line descriptions; `help <cmd>` and
`help <cmd> <sub>` walk the tree.

Subsystems behind them: LTDC + GUIX (`gui`, `lcd`), OV5640 over DCMI
(`camera`), the QSPI NOR as the asset store (`blob`, `qspi`; a LevelX + FileX
volume with an `fs` command until issue #194), microSD (`sd`), FT5336 touch
(`touch`), Ethernet with NetX Duo (`net`), the NN backends (`nn`), and YMODEM
transfer over the console (`xfer send` to the PC, `blob write` from it).

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
- **`nn run` reports a lost stream** (issue #163; the words are
  `svc/nn/nn_refusal`'s on every board since issue #174): when
  the camera stream ends by itself before the inference completes -- the
  auto-recovery gave up -- the run stops waiting and prints
  `nn: run: the camera stream was lost before an inference completed (-76)`,
  with ``; the camera has not released the frame either (`nn stream stop`
  finishes it)`` before the code when its teardown did not finish, instead of
  timing out after 2 s.  There is no bench procedure that makes the
  auto-recovery give up, so this outcome is covered by
  `shell/test/test_nn_run_wait.c` (the shared wait table) only.
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
> whose rollback left nothing, which on `tflm` since issue #191 is every refusal
> from the payload read on -- a refusal that kept the previous model keeps its
> result; the reload reports which, issue #122 P1) and `nn model unload` clear it. `nn run` waits on
> the record's accepted-publish count, not the worker's inference counter, which
> this board bumps before the decode is published. `nn stream stats` reads
> `infers` and `last` in separate critical sections, so on a running stream the
> two can be one frame apart. The GUI boxes are drawn only
> from a result of the session in force, so they still clear on `nn stream stop`.
> With no plugin the worker publishes the output descriptors of the model that
> ran, taken before its next inference (issue #121); `nn run` and `nn dets`
> print those, never the output tensor as it is at print time.  (Until #159
> Step 4c the resident decoder's "not BlazeFace" published the top 5 classes
> of output 0 here, through `svc/nn/nn_top.c`; this board no longer links it.)
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
> **A decoder fault is counted apart** (issue #130 step 6c, #122 P8;
> `svc/nn/nn_decode_count.c` since issue #163, one table since #159 Step 4c --
> the other boards'). A plugin's `BF_ERR_MODEL` is a mismatch between the container's
> halves and counts in `model_errors`.  (Until Step 4c the resident decoder
> ran on every model and its `BF_ERR_MODEL` was a classifier stream's top-5
> result, never counted; with no plugin the worker now publishes the raw
> tensors, an inference with nothing decoded.) Any other negative (`BF_ERR_UNINIT`,
> `BF_ERR_ARG`, or an undocumented code) is a wiring fault: it counts in
> `decoder_errors` and in `errors`, and the shared command prints the `of
> those:` line. Such an inference is **not** counted in `infers` (issue #138,
> as on grove-vision-ai-v2 and wio-lite-ai): `infers` counts the results a
> reader can use, so with decoder faults `in - skipped` equals `infers` plus
> the faults. Until #138 it counted in both, because it ran; before step 6c
> neither was counted. A `nn run` counts nothing in the worker's counters:
> not its publish, not an invoke failure, not a lease timeout (it reports each
> as its own ending). The producer's invariant errors (a band of the wrong
> height, no input tensor, a raced hand-over) are counted in `errors` whatever
> the run kind. Until issue #164 a `nn run`'s publish counted in `infers` --
> even a refused decode -- and its invoke failure or lease timeout in `errors`,
> so the `nn stream stats` after a `nn run` said `infers 1`; since #164 the
> tally (`nn_decode_tally_of()`) is one rule for all three boards.
>
> **Only what the record took is counted** (issue #159 Step 4b-iii, #122 P18,
> `nn_decode_tally_of()`), as on wio-lite-ai and (since issue #164)
> grove-vision-ai-v2: a publish the generation rule
> dropped -- a stop or a re-attach landing during the inference -- counts in
> nothing.  Until then the resident path counted the inference whether or not
> the record took it, so a stream whose one publish a stop refused said `infers
> 1` beside `last : nothing decoded yet`.  To see it: with
> `/blazeface.nnc` loaded, type `nn stream start` and then `nn stream stop` within
> about a second, then `nn stream stats`.  When a frame was handed over (`in`
> minus `skipped` is 1) and its inference was still running at the stop, the
> stats say `infers` 0 and `last : nothing decoded yet`; if `in` is 0 the stop
> came before any frame -- repeat.

### The `camera` command is shared with the other boards (issue #143)

The command is one file for all three boards, `shell/cmds/cmd_camera.c`, over
this board's adapter `port/camera/camera_svc_f746.c`, which implements the
contract in `svc/camera/camera_svc.h` (the layers are in
[svc/README.md](../../svc/README.md#the-camera-command-is-shared-issue-143)).
What only this board has stays in `cmds/cmd_camera_board.c` and is appended to
the shared table.  This board's capabilities (`svc/camera_svc_config.h`): power,
info, test pattern, save (`sd`; `fs` too until issue #194), send; `camera off` stops the operator's
stream first.

```
camera probe                    power up + read the OV5640 chip ID
camera on | off                 bring-up / cut the module's power
camera info                     driver / sensor state and the ISP settings
camera capture [test]           snapshot the current mode + statistics
camera save [sd] <path>         write the LAST capture raw to the microSD
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
  until the next capture.  The refusal names what clears it: `nn stream stop`
  if nn holds the camera, then `camera stream stop` -- nn's stop leaves the
  camera running for the operator (issue #142's `manual` holder, which the
  GUIX camera screen also opens at boot), so `nn stream stop` alone never
  clears it (issue #175).
- **`sdram test` also claims `.sdram.ai` from nn and skips live Ethernet state**
  (issue #152).  The NN arena and model buffer live in
  `.sdram.ai` (bank 3), owned by `port/nn`, so the test takes
  `nn_scratch_claim()` right after the camera claim and keeps it for its whole
  run.  While it is held no nn session can be taken (`nn model load` /
  `unload`, `nn bench`, `nn run` and `nn stream start` are refused as busy,
  worded ``.sdram.ai is held by `sdram test`; try again when it finishes`` where
  the command prints a reason), and the first model open after boot -- which is
  where a backend with a model in the image (`null`, `stedgeai`) adopts it into
  the arena -- builds nothing: there a fresh-boot `nn info` answers busy
  instead, and opens normally once the test is over.  On `tflm` and
  `stedgeai_reloc` the first open builds nothing in `.sdram.ai`, so it is not
  refused: `nn info` during the test says `model : (none)` (issue #159 Step 4c).  The claim refuses while another nn command
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
| `camera save` deepest path (from the disassembly, dispatch included) | about 3.18 KB to the microSD (the 3.43 KB QSPI write-error path went with the QSPI volume in issue #194) |
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
is enough, and moving code around shifts that alignment.  That is not the only
shape: the overruns measured for issue #172 almost never coincided with a
DMA2D repaint and fell while `nn-worker` was in the CMSIS-NN kernels, with a
rate that moves between builds; that investigation is #172's.  RM0385 17.3.10: the
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

### The unused DCMI LINE / VSYNC interrupts stay masked (issue #144)

Symptom, on some builds: during a GUIX preview the raster stream gets DCMI
overruns in bursts of 3-4 about every 40 s, at the same times after every boot
(15.7 / 55.7 / 95.7 s ...), whatever the stream starts and stops in between.
Only the first overrun of a burst is the real event; the others re-trigger
right after each in-place resync above.  Whether a build shows it comes and goes
with code placement: moving unrelated code by a few dozen bytes takes the count
from 0 to 16-18 in 3 minutes.  Every burst is resynced in place (`ovr ring` 0).

Same-boot A/B/A tests on a reproducing build established two necessary
conditions, in the configurations tested (QVGA RGB565, GUIX preview, 8-slot
SDRAM ring):

- the DCMI LINE interrupt is enabled: with IER 0x06 (ERR | OVR) there were 0
  overruns in 35 minutes; re-enabling LINE alone gave 6 in 71 s, VSYNC alone
  gave 0 in 208 s;
- the DMA destination is external SDRAM: two images differing only in the
  buffer base gave 6 overruns (SDRAM) against 0 (SRAM) in about 200 s.  The
  frame ring can only live in SDRAM, so this one stays.

`HAL_DCMI_Init()` enables LINE | VSYNC | ERR | OVR unconditionally, and nothing
on this board consumes LINE or VSYNC (the HAL callbacks are the weak empty ones,
about 3,600 interrupts/s at QVGA 15 fps).  `dcmi_hal_init()` in
`port/camera/camera.c` therefore masks both right after every Init (camera
init and the JPEG-mode toggle); the snapshot path already re-armed only ERR |
OVR.  IER (`devmem peek 0x5005000C`; bit0 FRAME, bit1 OVR, bit2 ERR, bit3 VSYNC,
bit4 LINE, RM0385 17.5.4) should read 0x06 on the raster stream and 0x07 while a
JPEG frame is armed.

Not known, and not pursued further: where the 40 s period comes from and why
code placement matters.  The reproducing binary and the raw investigation notes
are in the git-ignored `build/f746-144-repro/`.

### [!] The flip writes only the layer address (issue #160)

RM0385 18.3.3 (Table 117): every access to an LTDC register in the pixel-clock
domain -- the layer registers LxWHPCR, LxWVPCR, LxCKCR, LxPFCR, LxCACR, LxDCCR,
LxBFCR, LxCLUTWR and the global SSCR, BPCR, AWCR, TWCR, GCR, BCCR, LIPCR, CPSR,
CDSR -- stalls APB2 for 6 (write) / 7 (read) PCLK2 + 5 LCD_CLK periods, about 1.1 us at this
board's 4.8 MHz pixel clock.  LxCR, LxCFBAR, LxCFBLR and LxCFBLNR are in the
HCLK domain; SRCR, IER, ISR and ICR cost 6-7 PCLK2 only.

`HAL_LTDC_SetAddress_NoReload()` goes through `LTDC_SetConfig()`, which
rewrites the whole layer: 11 write / read-modify-write statements on
pixel-clock-domain layer registers (16 bus accesses) plus 4 BPCR reads, so
at least about 12 us and up to about 22 us of APB2 stall on every flip,
back to back.  SDMMC1 sits on
APB2 and its 32-word RX FIFO, drained by the DMA2 peripheral port, overflows
after about 8 us without service at 24 MHz 4-bit, so an SD read that overlapped
a GUIX buffer toggle failed with `HAL err 0x20` (RX overrun) -- about 1
`nn model load --path` in 3 while the camera preview ran.  The DMA2 peripheral
port is shared with the DCMI stream, so the same stall, landing on active
pixels, also starves the DCMI (one captured SD failure coincided with a DCMI
overrun).  It is the likely source of the rare DCMI overruns under GUIX (2 per
624 s under the `nn stream` load before the fix); that run has not been
repeated after the fix.

Verified after the fix (2026-10-30): 30 consecutive `nn model load --path`
with the camera preview running, 0 failures, no `HAL err 0x20` in `dmesg`;
811 s of preview with `ovr dcmi` 0; `gui stop` / `gui start` and `lcd off` /
`lcd on` round trips intact.

`ltdc_flip_locked()` (`port/ltdc/ltdc_display.c`) therefore writes LxCFBAR
alone (and keeps `hltdc.LayerCfg[0].FBStartAdress` in step) before
`HAL_LTDC_Reload(VERTICAL_BLANKING)`.  LTDC accesses that remain:

| Path | Registers | Domain |
|---|---|---|
| every flip (GUIX canvas toggle, `lcd` draw commands) | LxCFBAR write; IER read-modify-write and SRCR write (`HAL_LTDC_Reload`); SRCR read, polled at 1 ms until VBR clears | HCLK / PCLK2 |
| reload-ready IRQ, once per flip | ISR, IER reads; IER read-modify-write; ICR write | PCLK2 |
| `lcd on` / `lcd off`, `sdram test` hold / unhold | GCR read-modify-write | pixel |
| `lcd info` | ISR read | PCLK2 |
| bring-up / failed bring-up | `HAL_LTDC_Init`, `HAL_LTDC_ConfigLayer`, `HAL_LTDC_DeInit` | all |

Nothing on a per-frame or per-flip path touches a pixel-clock-domain register;
keep it that way (no `HAL_LTDC_SetAddress*`, `HAL_LTDC_SetPitch*`,
`HAL_LTDC_SetWindow*`, `HAL_LTDC_ConfigLayer*` or CPSR / CDSR reads there).

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
