# svc/ -- board-independent services

`svc/` is the freestanding service layer. Its translation units include no HAL,
no CMSIS device header, no board header and no `tx_api.h`, so the same source is
compiled into every board's firmware (from `shell/` and from `boards/<board>/`)
and into the host tests under `shell/test/` and `boards/<board>/test/`. Anything
that needs a peripheral, a memory map, a lock or a thread stays in the board and
is reached from here through a hook or a declared API.

The files are grouped by the feature they serve (issue #153):

| Directory | What it holds |
|---|---|
| `svc/` | Generic services any layer uses: the formatter (`fmt`), CRC-32 (`crc32`), YMODEM (`ymodem`) and small graphics helpers (`gfx_rot`, `rect_geom`). |
| `svc/nn/` | The `nn` command's contract and shared core (`nn_*`, described below), the neutral tensor descriptor (`tensor.h`), the inference API `nn.h`, its one implementation `nn.c` (the dispatcher over the compiled-in backend, `nn_backend.h`) and its translation into that descriptor `nn_desc` (every board: issue #161, grove-vision-ai-v2 since issue #169), the tflm and Ethos-U backends (`tflm/`, `ethosu/`), and the BlazeFace decoder (`blazeface`), which reaches every board only inside a plugin container (no board links it into its firmware since issue #159 Step 4c). |
| `svc/plugin/` | The plugin container's ABI, loader, executor and the checks around them (`plugin_*`, below). |
| `svc/camera/` | The camera frame pipeline (`frame`, `frame_pipeline`), the camera stream's holder table, sink steps and stream generations (`cam_holders`, `cam_step`, `cam_gen`, below) and the `camera` command's contract (`camera_svc`, below). |

Every one of the four directories is on the include path wherever `svc/` is, so
a source names a header by its file name alone (`#include "nn_core.h"`), and the
storage audit covers the whole tree under `svc/`.

## The `nn` command's layering

There is one `nn` command for all three boards. What differs between boards is
below the adapter, never in the command.

```
shell/cmds/cmd_nn.c (+ nn_cmd_core.c)    one command; a subcommand whose
        |                                NN_SVC_HAS_* capability is 0 is not
        |                                registered (boards/<board>/svc/nn_svc_config.h)
        v
svc/nn/nn_svc.h                          the contract: the nn_svc_*() calls
        |
        v
boards/<board>/port/nn/nn_svc_<board>*.c the adapter: translates the contract
        |                                to the board's model API (console side)
        v
boards/<board>/port/nn/                  the board's glue, one file per thread:
   nn_producer.c | nn_worker.c | nn_panel.c | nn_camera.c (console side, owns
   producer      | worker      | panel      | the state; nn_camera_priv.h shares it)
        |            |                     each thread calls into the shared
        |            |                     core and passes it hooks and state
        |            v
        |   svc/nn/nn_core.c       lifecycle policy and the stream's numbers
        |   svc/nn/nn_core_frame.c producer / worker / panel frame path
        |   svc/nn/nn_stream_life.c, nn_handoff.c, nn_det_record.c,
        |   nn_core_model.c, nn_swap.c, nn_active_core.c, nn_active_lease.c,
        |   svc/plugin/plugin_lease_api.h (implemented by the board)
        v
backend: svc/nn/tflm/ (wio-lite-ai, f746g-disco; the board places its buffers
         and defines its state in boards/<board>/port/nn/tflm/)
         svc/nn/ethosu/ (grove-vision-ai-v2, Ethos-U55; the board defines its
         state and places its arena in boards/grove-vision-ai-v2/port/nn/npu/)
```

The producer above is not a subscriber of its own.  Since issue #142 each board
has ONE camera sink, and nn's producer is that sink's second step:

```
camera producer thread
   -> the board's camera sink, consume() per frame (or per band on wio-lite-ai)
        1. display step  -- only while the display is on
        2. nn step       -- only while nn holds a stream and the worker wants a frame
                            (nn_producer.c above)
        camera_frame_put() last (f746g-disco, grove-vision-ai-v2)

f746g-disco         port/camera/cam_sink.c    GUIX preview, then nn
wio-lite-ai         port/camera/cam_band.c    LCD preview band, then nn ingest
grove-vision-ai-v2  port/camera/cam_lcd_sink.c  panel; nn is the overlay on it
```

The command and the shared core never see a backend's tensor type: the board
turns its own tensors into `svc/nn/tensor.h` descriptors before anything shared
reads them.

### The `nn` command's meaning on the three boards (issue #164)

One row per subcommand.  The meaning is the shared command's and the shared
core's, the same on every board; the last column names what differs by board,
nearly all of it the hardware's (a capability is in the table below, and the
statuses that differ are listed after it).  How each board does it, with its waits, frame
counts and stack depths, is the board README's:
[Grove Vision AI V2](../boards/grove-vision-ai-v2/README.md#the-nn-files),
[Wio Lite AI](../boards/wio-lite-ai/README.md#the-nn-files),
[STM32F746G-DISCO](../boards/f746g-disco/README.md#the-nn-files).

| Subcommand | Meaning | Capability | What differs by board |
|---|---|---|---|
| `nn info` | The backend (`nn_backend()`'s name and version), the model and where it came from, the arena reserved and used, the input and output tensors, then the board's plugin lines; answered in full while a stream runs, busy while a load is in flight. | always | f746g-disco: `sdram test` holding `.sdram.ai` answers busy.  grove-vision-ai-v2: also busy while a `nn run` or `nn bench` holds its gate; the `source` line says the NPU runs secure and privileged. |
| `nn model load --name / --slot / --path / --addr` | Point the model at bytes from the board's one store and swap it in as one transaction (the plugin after the backend succeeds; the order is `nn/nn_core_model`). | `MODEL_LOAD` and the tag's | grove-vision-ai-v2: a NOR blob by name, or a raw window with its length, read in place through the XIP window.  wio-lite-ai: a NOR slot copied to PSRAM, the CRC checked on the copy.  f746g-disco: a microSD file into SDRAM; prints the CRC32 of every byte it adopted. |
| `nn model unload` | Unload the plugin and the model; nothing is loaded afterwards. | `MODEL_LOAD` | grove-vision-ai-v2: also powers the NPU down and gives back the NOR lease. |
| `nn bench [n]` | One hold: fill every input with `0x5A`, n timed runs, min / avg / max (`nn/nn_bench`). | `BENCH` | grove-vision-ai-v2: the 1 ms ThreadX tick and no `@MHz` (the DWT stops in WFI); the other two count DWT cycles. |
| `nn run` | One frame through the same producer and worker as a stream; ends one of five ways (`nn/nn_run_wait`) and prints its own result. | `CAMERA` | grove-vision-ai-v2: int8 input only (the Ethos-U55 fills a byte per element), and the first frames after a camera bring-up are skipped.  wio-lite-ai: the input must tile onto the 60-row DCMI bands; PSRAM must be up and OCTOSPI1 not held by a retuner.  f746g-disco: the camera must stream RGB565; `sdram test` holding `.sdram.ai` answers busy. |
| `nn dets` | The last published record: the plugin's report captured with it, or the raw output shapes. | always | -- |
| `nn out [tensor] [count]` | Dequantised values of one output tensor, the model held; busy while a stream runs. | always | wio-lite-ai: also takes the PSRAM guard (the arena is behind OCTOSPI1). |
| `nn thresh [1..999]` | Get or set the loaded decoder's threshold under the plugin lease; `none` with no decoder. | always | -- |
| `nn norm <0\|1>` | The float range the camera input is converted into. | `NORM` | grove-vision-ai-v2 has no float input to set (int8 only, fixed `pixel - 128`). |
| `nn stream start [--frames <n>]` | Claim the stream, start the camera if it is not running, start live inference; returns at once, or waits n frames and stops its own generation. | `STREAM` | As `nn run`, and: grove-vision-ai-v2 refuses a model with no decoder before any shape or draw question (CLAUDE.md: not aligned); wio-lite-ai re-arms a band stream it lost (a new generation, the counters continue). |
| `nn stream start --test` | Stream the sensor's colour bar instead of the scene. | `STREAM_TEST` | -- |
| `nn stream stop` | Stop the current stream; `not running` is not a failure; a retryable or terminal ending is reported. | `STREAM` | -- |
| `nn stream stats` | Frames in, skipped, errors, infers and rate, decoder refusals, the last latency and result, then the board's own lines; STATE until a stream has run once (`nn_core_poll()`), and a `nn run` is not one. | `STREAM` | grove-vision-ai-v2: the latency in 1 ms steps (the same clock as `nn bench`). The board lines are each board's. |

`infers` counts only a stream's publish the record took -- a raw one (no
decoder) as an inference, a refused decode as an error and never an inference;
a publish the record dropped and a `nn run` count nothing in the worker's
counters (`nn/nn_decode_count`, the same rule on every board since issue #164).

| Capability | grove-vision-ai-v2 | wio-lite-ai | f746g-disco |
|---|---|---|---|
| `NN_SVC_HAS_MODEL_LOAD` | yes | `tflm` with the KV store | `tflm` or `stedgeai_reloc` |
| `NN_SVC_HAS_MODEL_NAME` / `_ADDR` | yes | -- | -- |
| `NN_SVC_HAS_MODEL_SLOT` | -- | as `MODEL_LOAD` | -- |
| `NN_SVC_HAS_MODEL_PATH` | -- | -- | as `MODEL_LOAD` |
| `NN_SVC_HAS_CAMERA`, `_BENCH`, `_STREAM` | yes | yes | yes |
| `NN_SVC_HAS_NORM` | -- | yes | yes |
| `NN_SVC_HAS_STREAM_TEST` | -- | yes | -- |

The declarations are `boards/<board>/svc/nn_svc_config.h`; a capability that is
0 leaves its subcommand or tag unregistered.  `NN_SVC_HAS_MODEL_BUILTIN` (`nn
model load builtin`) is declared by no board.

The lifecycle's refusals of `nn run`, `nn stream start` and `nn stream stop`
(a stream already running, a `nn run` holding the camera, a start or stop in
flight, a dead lifecycle, a replaced generation, not running) are one table on
every board since issue #174, `nn/nn_refusal`: the status, the claim
disposition and the sentence, with the BUSY sentence chosen by a property of
`struct nn_core_board` (whether the board has a gate), never its name.  So are
the base sentences of a `nn run` that ended LOST, CANCELLED or TIMEOUT; what a
board adds is only the clause on what its teardown did not release (wio-lite-ai
the band stream and the PSRAM guard, f746g-disco the DCMI frame), checked at
build time against the longest sentence it follows -- grove-vision-ai-v2 replaces
the sentence with its stop's verdict instead.  The plugin lines of `nn info` are
`plugin/plugin_claims`'s on all three boards since the same issue.  The board
lines of `nn stream stats` stay each board's (grove-vision-ai-v2's `painter :`
and the other two's `plugin  : drew ...` are not aligned, by decision): they are
the board's own subject in the contract.  The status a refusal reports
(`NN_SVC_ERR_*`) is the same on the three boards (issue #164): no model, a
decoder that cannot read the model's outputs and one that draws nothing are
STATE; an input the board cannot fill is HW, from `nn run` and `nn stream start`
alike.

### The inference layer is one implementation (issue #161)

On every board the model API below the adapter is shared too (grove-vision-ai-v2
since issue #169, over its own backend -- see [The Ethos-U backend](#the-ethos-u-backend-issue-169)):
`svc/nn/nn.c` is the one dispatcher (the model singleton, the session, cycle
timing, the model swap) over the one compiled-in backend (`nn_backend.h`), and
`svc/nn/tflm/` is the one tflite-micro backend.  `svc/nn/tflm/` is compiled only
when `CONFIG_NN_BACKEND=tflm`, and the TFLM headers are the only include it adds
to `svc/`'s (`svc/nn/ethosu/` below is the other directory that includes them).
The parts of the backend that do not depend on where a model is staged -- tensor
descriptors, the display name, the empty state, the op resolver, the interpreter's
construction and destruction -- are in `nn_tflm_glue.h`, which the Ethos-U backend includes into
its own translation unit (issue #169).  The construction zeroes the whole arena
first (issue #167): TFLM does not clear its per-op table, and a model refused
part-way through it would otherwise leave entries the destructor calls as function
pointers.  What a model's I/O must satisfy before it is planned is in
`nn_tflm_model_check.h`, which both backends and the three host gates
(`verify_tflite` on wio-lite-ai and f746g-disco, `verify_vela_model` on
grove-vision-ai-v2) call (issues #175, #178).  The walk runs in a fixed order:

1. Structure (`NN_MODEL_ERR_FORMAT`): subgraph 0's input / output lists present
   and naming tensors that exist; every subgraph has a tensor vector; every
   operator present has an inputs and an outputs list, each of its inputs /
   outputs / intermediates is at most `NN_TFLM_MAX_OP_TENSORS` (64) long --
   lengths judged before any element is read -- and every index names a tensor
   of its subgraph (or -1, the optional tensor, in inputs).  TFLM itself range-checks
   none of these indices: its planner writes per-tensor records through them.
2. The count of each I/O list (1..`NN_MAX_IO`, `NN_MODEL_ERR_SHAPE`).
3. Rank (`NN_MODEL_ERR_TENSOR`): every tensor of every subgraph at most
   `NN_TFLM_MAX_TENSOR_RANK` (6, TFLM's `RuntimeShape::kMaxSmallSize`, which
   TFLM only DCHECKs), each I/O tensor at most `NN_MAX_DIMS`.  Only the length of
   an internal tensor's shape is read, never its values (Vela's internal tensors
   reach 2.2 M).
4. The I/O values: every dimension inside 0..65535, the range of `struct
   nn_tensor`'s 16-bit dims (`NN_MODEL_ERR_DIMS`, the predicate
   `nn_tensor_dim_fits()` in `nn.h`, tied to the field's width at compile time).

All of it runs on the model, before `AllocateTensors()`; the SHAPE / TENSOR /
DIMS checks on the planned tensors stay as the backstop and give the same codes.
The order is what bounds the shape, operator-list and index reads of a load:
the verifier checks a scalar vector in O(1), does not count it as a table and
accepts many entries naming one table, so a 512 KB file could hand
`AllocateTensors()` ~1e8 shape and list reads behind the 4,096 visits.  Lengths
first, the walk does one compare per tensor entry and at most 3 + 3 x 64 reads
per operator entry, each entry a verifier visit -- at most 4,096 x 192 list
reads -- and what it passes bounds TFLM's own passes over the same vectors the
same way.  It does not bound all of `AllocateTensors()`: the per-operator
scratch-request walk and the greedy memory planner are quadratic in tensors and
operators, so the whole load is bounded by visits squared x small constants.
The one exception to lengths-first: subgraph 0's two I/O lists are walked once,
one compare per entry, before their count is judged (at most ~128K entries in a
512 KB file), so that a model broken in two ways stays `NN_MODEL_ERR_FORMAT`.  The same header holds the bounded
flatbuffer verifier, `nn_tflm_verify_model_buffer()` (depth 16, 4,096 table
visits, nested-flatbuffer check off): the tflm backend runs it under
`NN_TFLM_VERIFY` (f746g-disco), `nn/ethosu/npu_verify.h` delegates to it behind
grove-vision-ai-v2's length floor, and all three host gates call it, so no gate
passes what a board's verifier refuses.  Its header states what each limit
bounds: depth is bounded by the schema (acyclic, longest table chain 6 in both
pinned schemas; `cmake/fixtures/model_io/schema_table_depth.py` reruns it), so
16 is the guard for a future cycle; the table count bounds the verifier's own
work (visits, which shared sub-tables multiply) and, through the walk above, the
load's shape and list reads.  wio-lite-ai runs no verifier, so there only the file's length bounds the
entries.  grove-vision-ai-v2's payload walk per operator entry is bounded by its own
scan (`ethosu/npu_model_scan`, #179); out of scope by decision: the per-channel quantization copy (#180).  Neither keeps storage of its own; the board supplies everything
that is a fact about its memory or its core, bound at link time (the callers of
`nn.h` pass no context to carry it):

| What the board gives | Where | Why it is the board's |
|---|---|---|
| `nn_model_board_state` and four hooks: critical section (restores the mask it found), one-tick wait, cycle-counter start and read (`nn_dispatch_state.h`) | `port/nn/nn_model_board.c` | PRIMASK, the ThreadX tick and the DWT are the core's and the RTOS's; f746g-disco unlocks the DWT, wio-lite-ai's timebase already owns it |
| `nn_tflm_board_state` and the facts (the model-name prefix) (`nn_tflm_state.h`) | `port/nn/tflm/nn_tflm_board.cc` | which RAM the interpreter's state lives in, and how this board names a model (`sd:<path>` or a blob name) |
| `nn_tflm_arena`, `nn_tflm_model_buf`, `nn_tflm_stage_cap` (`nn_tflm_mem.h`) | `port/nn/tflm/nn_tflm_bufs.c` | placement: SDRAM bank3 on f746g-disco, the PSRAM carve-out on wio-lite-ai, each under that board's residency gate |
| `NN_TFLM_ARENA_KB`, `NN_TFLM_MODEL_KB`, `NN_TFLM_VERIFY`, the op profile | `cmake/tflite-micro.cmake` (compile definitions) | sizes the board's memory can hold, whether the board can afford the on-board flatbuffer verifier, and which kernels fit its flash |

The op profile is one definition selecting a complete list in
`nn_tflm_ops.h`; the resolver's size is counted from it.  Since issue #169 it
also holds the Ethos-U profile, `NN_TFLM_OPS_ETHOSU_PROFILE=1`: the one
`ethos-u` custom operator and no CPU kernel, which grove-vision-ai-v2's board.cmake
selects for the Ethos-U backend and for its host gate alike.

Where `nn_model_reload()` may adopt a model from is the backend's to declare
(`nn.h`, issue #169).  A staging backend (tflm) adopts only from a slot
`load_region()` handed out, aligned and not the live one.  An in-place backend
(Ethos-U) adopts from inside the read window the board declares, aligned; it
has no `load_region()` (so `nn_model_load_region()` is `NN_ERR_NOSUP`), and the
model bytes stay valid only because the caller holds its lease on the window
for as long as the model is loaded.  Its refusals that have no staging
counterpart have their own codes -- `NN_MODEL_ERR_WINDOW`, `NN_MODEL_ERR_IDENT`
(kept apart from `NN_MODEL_ERR_FORMAT`) and `NN_MODEL_ERR_PAYLOAD` -- worded by
that backend rather than by `nn_model_strerror()`.  `NN_MODEL_ERR_DIMS` (issue
#175) is both backends' and is worded by `nn_model_strerror()`; the reload's
status and ending do not depend on which code refused, so it needed no table
change.  [!] It judges the DECLARED shapes, so a model whose
input or output declares `-1` (a dimension a RESHAPE used to resolve at
`Prepare()`, loading as before) is now refused as `NN_MODEL_ERR_DIMS`.

### The Ethos-U backend (issue #169)

`svc/nn/ethosu/` is grove-vision-ai-v2's backend behind the same `svc/nn/nn.c`
dispatcher the other two boards use, and the checks and the cache protocol that
come with it.  None of it keeps storage of its own: the stateless-service audit
compiles every TU here in Grove's firmware context, like every other TU under
`svc/`, and the C++ ones against the Himax SDK's TFLM snapshot, the only TFLM tree
that board builds.

| File | What it is |
|---|---|
| `nn_ethosu.cc` | The backend (`nn_backend_vt_selected`). A model is interpreted IN PLACE, inside the read window the board declares: no `load_region()`, no staging slot, no second buffer; the caller's lease keeps the bytes in place (`nn.h`). The checks run in one order -- window, length, alignment, identifier, bounded verifier, schema version, payload walk, the model's graph and I/O (`nn_tflm_model_check.h`), resolver, AllocateTensors, the I/O rule -- each refusal its own code (`NN_MODEL_ERR_WINDOW` / `_IDENT` / `_PAYLOAD` for the ones only an in-place backend gives; `_DIMS` as tflm). It tells the cache protocol which arena is live and closes out each `Invoke()` with its backstop. Transactional like tflm: a refused model leaves the previous one (rebuilt from the same bytes) or the empty state. Uses `svc/nn/tflm/nn_tflm_glue.h` with its state named. |
| `nn_ethosu.h` | C, for the adapter: `nn_ethosu_strerror()`, the sentences for the in-place refusals (every other code goes to `nn_model_strerror()`). |
| `nn_ethosu_state.h` | The state type and the board facts (name prefix, the read window). Needs `NN_TFLM_OPS_ETHOSU_PROFILE`: the resolver's size is part of the type. |
| `nn_ethosu_mem.h` | The arena declaration, sized by the board's `NN_ETHOSU_ARENA_KB`. |
| `npu_verify.h` | Grove's entry to the bounded FlatBuffer verifier: its length floor, then `nn_tflm_verify_model_buffer()` from `nn/tflm/nn_tflm_model_check.h`, where the limits (depth, table count) are declared for every board (issue #175); the host gate `verify_vela_model` compiles the same header. |
| `npu_model_scan` (`.cc` / `.h`) | Which bytes the driver will parse (the `ethos-u` operator's input tensor 0) and whether the model may be opened at all: one subgraph, every operator `ethos-u`, no variable payload tensor, no `-1` among an `ethos-u` operator's inputs after 0 (its `Eval()` reads them unchecked), and the payloads adding up to at most the model's declared length -- each charged before it is walked, so the scan reads at most length / 4 words however many operator entries share one stream (#179). Shared with the host gate. |
| `npu_payload` (`.c` / `.h`) | Whether that payload is exactly one COMMAND_STREAM action, as the last action. Plain C over bytes, so the host test compiles it directly. |
| `npu_cache` (`.c` / `.h`) | The arena ownership protocol and the driver's four cache hooks (`ethosu_flush_dcache`, `ethosu_invalidate_dcache`, `ethosu_inference_begin`, `ethosu_inference_end`), strong definitions that override the driver's weak ones at link time: the whole arena cleaned before launch and invalidated once DONE and OK are both seen, a checked reset first on any other ending, fail-stop otherwise. Its state (`npu_cache_board_state`), the cache-line primitive (`npu_cache_clean()` / `npu_cache_invalidate()`) and the fail-stop are the board's (`port/nn/npu/npu_cache_board.c`). |

What the board gives, in `boards/grove-vision-ai-v2/`:

| What | Where | Why it is the board's |
|---|---|---|
| `nn_model_board_state` and the four hooks; the cycle counter reported unavailable, so `nn_last_cycles()` is 0 | `port/nn/nn_model_board.c` | the DWT stops in WFI while the inference thread sleeps on the driver's semaphore; `nn bench` and the stream keep their own clocks |
| `nn_ethosu_board_state` and the facts (`""`, `NOR_XIP_BASE`, `NOR_SIZE`) | `port/nn/npu/nn_ethosu_board.cc` | which RAM the state lives in, and the one statement of the read window |
| `nn_ethosu_arena` | `port/nn/npu/npu_arena.c` | placement: `.nn_arena` in SRAM, NOLOAD, 32-byte aligned, pinned by the placement gate |
| `NN_ETHOSU_ARENA_KB`, `NN_TFLM_OPS_ETHOSU_PROFILE` | `board.cmake` (compile definitions, to the firmware and the host gate) | the size the board reserves, and the one-operator list |
| the ThreadX mutex / semaphore hooks the driver binds by name | `port/nn/npu/npu_rtos.c` | they are ThreadX, which `svc/` does not include |
| the NPU bring-up, its interrupt, the NOR window's lease and its lifetime | `port/nn/npu/npu_hw.c`, `port/nn/nn_svc_grove.c` | the adapter takes the lease before a load and returns it only when no model is left; `nn.c`'s session is taken and given with the adapter's own gate |

### The board's side of the contract (issue #164)

`svc/nn/nn_svc.h` is the contract between the command and the board's
adapter -- what `nn_svc_*()` answers -- not the board's hardware layer.  Below
the adapter, what a board hands the shared code is declared in these headers;
every object is the board's (state as zero-initialised static storage, hooks
and facts as `const` objects in its `.rodata`), and the shared files keep no
storage of their own.

| Header | What the board gives | What it decides |
|---|---|---|
| `nn/nn_dispatch_state.h`, `nn/tflm/nn_tflm_state.h`, `nn/tflm/nn_tflm_mem.h`, `nn/ethosu/nn_ethosu_state.h`, `nn/ethosu/nn_ethosu_mem.h` | the model API's and the backend's state, its hooks and its buffers | see [The inference layer is one implementation](#the-inference-layer-is-one-implementation-issue-161) and [The Ethos-U backend](#the-ethos-u-backend-issue-169) |
| `nn/nn_core.h` | `struct nn_core` (the live-inference state) and a `struct nn_core_board`: the critical section, the clock and its rate, the counter and record samplers, the optional transient-claim hooks decided with the lifecycle, `latch_extra`, and the properties `based`, `rearm`, `clock_needs_producer` | the stream's lifecycle and numbers, from this board's counters and clock |
| `nn/nn_core_frame.h` | `struct nn_core_frame` (the frame word's state), a `struct nn_core_frame_ops` (prep, wake the worker, the generation question, the raw and decoded publish, outputs, decode, the board's counting, the plugin lease's hooks) and a `struct nn_core_panel` | how a frame is filled, decoded, published and counted on this board's producer, worker and panel threads |
| `nn/nn_core_model.h` | a `struct nn_core_model_board` (which spec tags it loads from, its preconditions and claim, bring-up, fetch) and its `struct nn_core_model_backend` | where the model's bytes come from and what a load needs first; the order around the swap is the shared file's |
| `nn/nn_bench.h` | a `struct nn_bench_ops`: the hold and the clock | how a bench holds the model and times a run |
| `nn/nn_active_core.h` | a `struct nn_active_board`: is a plugin live, a slot's address, where a depth sample goes, the no-plugin shape answer | how the plugin's decoder slots are reached on this board |
| `nn/nn_active_lease.h` | `nn_active_facts` (`struct nn_active_lease_board`): output count and descriptors -- wio-lite-ai and f746g-disco, whose model is a `struct nn_model` | the lease-checked entries' view of the model |
| `nn/nn_det_record.h` | the `struct nn_det_record` and the lock it is read and published under | which lock orders the record against the board's other locks |
| `nn/nn_shot.h` | a `struct nn_shot`, moved only under the lock that moves the record's generation -- wio-lite-ai and f746g-disco (grove-vision-ai-v2 keeps its own `nn_ov_shot` state and links `nn_shot.c` for its words only: `nn_shot_says()` from its `port/nn/nn_run_says.c`, which keeps its own sentences for the endings only it has) | how one `nn run` ended |
| `nn/nn_refusal.h` | nothing new: the board's existing `struct nn_core_board` (its `gate_held` picks the BUSY sentence), and for a `nn run` that ended without a result its teardown clause as a `NN_REFUSAL_RUN_UNFINISHED()` literal | what its teardown did not release; the words and status of every refusal are the table's |
| `nn/nn_svc_common.h` | a const `struct nn_svc_thresh_ops` (the plugin lease, the decoder's threshold entries and its `NN_ACTIVE_NOT_HELD`, and on grove-vision-ai-v2 the count's way in and out), and for `nn info` on wio-lite-ai and f746g-disco its read of the load order's transition counter | which protections a threshold call takes on this board; the order and the answers are the shared file's |
| `plugin/plugin_lease_api.h` | the plugin lease itself (`plugin_lease_try` / `_take` / `_give` / `_held` and the miss counters), in the board's `port/plugin/plugin_lease.c` -- all three boards | who may be inside the loaded plugin; the wait and the lock order are the board's `plugin_lease.h` |

Why the locks and the threads behind these hooks are the board's is
[below](#why-the-lock-and-the-threads-are-the-boards).

## The camera stream's ownership (issue #142)

The camera streams while, and only while, somebody holds it.  Who that is lives
in one table, `cam_holders`, which every board uses; what the table answers is
what the board does, in the same locked region.

**The holders.**  Two, on every board: the operator (`camera stream start`,
named `manual`) and nn (`nn stream start`, `nn run`, named `nn`).  A holder is a
pointer to a static `struct cam_holder` its owner defines; the pointer is its
identity and the name is only what `holders:` prints.  The camera's own code
defines the operator's holder and **knows nn only as a name**: no file under
`port/camera/` includes an nn header, and nn's holder is defined in its own
`port/nn/nn_camera.c`.  The display is not a holder -- `lcd on` / `lcd off`
never start or stop the camera.

**The table's rules** (`cam_holders.h`, host test `shell/test/test_cam_holders.c`):

| Call | Answer |
|---|---|
| add a holder already there | nothing changes (idempotent); its entry takes a fresh generation |
| remove a holder not there | nothing changes |
| add to an empty table | `T_START`: start the camera, then commit or abort; the table is CLAIMED meanwhile |
| any add or remove while claimed | refused, retryable (`E_CLAIMED`) |
| abort | removes only what the claiming add registered |
| remove the last holder | `T_STOP`: request a stop |
| remove a `KEEPS_STREAM` holder (the operator) while one without it (nn) is there | refused (`E_IN_USE`), nothing changes |
| add past `CAM_HOLDERS_MAX` (4) | refused (`E_FULL`) |

`cam_holders_add_or_start()` is nn's call: it joins a running stream, or, when
nothing streams, registers the operator on the operator's behalf and then nn,
in one call.  It returns a ticket saying what it added and under which
generations, and `cam_holders_undo()` removes only entries that still carry
those generations -- so a late undo never takes away an operator who registered
again in between.  `cam_holders_clear()` empties the table at a confirmed end
and keeps the generation counter.  Refusals are distinct codes;
`cam_holders_disp_of()` says which are retryable, and an unknown one is terminal.

**What the commands do with it** (the same on all three boards):

- `camera stream start [test]` registers the operator.  It has no `--frames` /
  `--secs`; the only bound is `nn stream start --frames <n>`.  `test` (a colour
  bar) applies when this call starts the camera; on a running stream the same
  setting is accepted and a different one refused.  grove-vision-ai-v2 has no
  test pattern and refuses `test`.
- `camera stream stop` removes the operator.  While nn holds the stream it
  answers `camera: still in use: nn` and changes nothing: the operator stays too.
- `nn stream start` and `nn run` join a running stream, or start it holding the
  operator's place as well.  nn's stop removes only nn, so the camera keeps
  streaming after it; stopping the camera is always a later `camera stream stop`.
- `camera stream stats` prints the holders on a `holders  :` line, and how many
  the last confirmed end lost.
- The preview is derived, not switched: it is drawn while the camera streams and
  the display is on.  The nn boxes are drawn while it is drawn and a `nn stream`
  runs; a `nn run` draws none.  There is no `camera preview` or `nn overlay`
  command any more.

**[!] An empty table is not a permission to tear down.**  `T_STOP` says "request
a stop".  The board gives resources back only after its own confirmed stop
(grove-vision-ai-v2: `camera_stream_stop()` succeeded; wio-lite-ai: the band
callbacks drained; f746g-disco: the producer's teardown).  A stop that cannot be
confirmed keeps them.

**Two ways a stream ends without its holders leaving.**

- A *confirmed end* -- the producer stopped on an error and that stop is
  confirmed, or f746g-disco's auto-recovery gave up: the board clears the table
  and marks each holder lost.  The mark does not go when somebody else restarts
  the stream; it goes when that holder takes hold again (on wio-lite-ai also
  when it lets go).  nn's step is not
  re-armed by another holder's restart, so nn takes no frames until nn itself
  starts again.
- An *unconfirmed loss* (grove-vision-ai-v2's `CAM_ST_LOST`, or a panel thread
  that did not come back): terminal.  The table, the sink and every resource
  stay where they are, later starts are refused, and only a reboot clears it.

**The steps of the sink** (`cam_step.h`, host test `shell/test/test_cam_step.c`).
Each step of the one camera sink has an *armed* mark and a *busy* count.  The
producer reads the mark and increments the count in one critical section, and
skips the step when it is not armed.  Switching a step off is: disarm, then wait
(wall clock) for the count to reach 0.  Zero after disarm proves the producer is
out of the step and cannot enter it again; nn's stop uses it as the producer
half of its JOIN.  A step is armed only at count 0, so what its owner
initialises just before arming is never read half-way.  The owner keeps its own
transition claimed across disarm -> drain -> clean-up -> commit.  A drain that
runs out of time is retryable: the resources and the claim stay, and the next
stop finishes it.

## The `camera` command is shared (issue #143)

There is one `camera` command for all three boards, in the same layering as
`nn`: one way down, the command never reaching below the contract.

```
shell/cmds/cmd_camera.c                  one command: grammar, usage, refusals,
        |                                the holders / warm-up lines, the save /
        |                                send loops; a subcommand whose
        |                                CAMERA_SVC_HAS_* is 0 is not registered;
        |                                the board's own subcommands are
        |                                appended (CAMERA_SVC_BOARD_SUBCMDS)
        |   shell/cmds/camera_cmd_board.h  the two shell-layer services a board
        |                                gives it: YMODEM send, the save medium
        v
svc/camera/camera_svc.h                  the contract: the camera_svc_*() calls
svc/camera/camera_svc_adapter.h          static-inline result / line helpers
        |
        v
boards/<board>/port/camera/camera_svc_<board>.c   the adapter (no shell instance;
        |                                writes through the caller's writer,
        |                                answers a cancel callback)
        v
the board's camera driver (port/camera/camera.c ...), which keeps the
holder table (svc/camera/cam_holders), the sink steps (svc/camera/cam_step)
and, on f746g-disco and wio-lite-ai, the stream generations
(svc/camera/cam_gen.h)
```

| Capability (`boards/<board>/svc/camera_svc_config.h`) | f746g-disco | wio-lite-ai | grove-vision-ai-v2 |
|---|---|---|---|
| `CAMERA_SVC_HAS_POWER` (`on` / `off`) | 1 | 1 | 0 |
| `CAMERA_SVC_OFF_STOPS_STREAM` (`off` stops the operator's stream first) | 1 | 1 | -- (no `off`) |
| `CAMERA_SVC_HAS_INFO` | 1 | 1 | 1 |
| `CAMERA_SVC_HAS_TEST` (colour bar) | 1 | 1 | 0 |
| `CAMERA_SVC_HAS_SAVE` (`save [<medium>] <path>`) | 1 (sd, fs) | `BSP_ENABLE_SD` (sd) | 0 |
| `CAMERA_SVC_HAS_SEND` | 1 | 1 | 1 |
| `CAMERA_SVC_BOARD_SUBCMDS` | res format fps set | scan reg | raw profile vts bench exposure gain auto black sat gamma bayer wb |

What the contract fixes, on every board:

- **A result is `{status, detail, users}`**; the statuses (ARG / NOSUP / STATE /
  BUSY / IN_USE / NO_FRAME / HW / TIMEOUT / LOST / CANCELLED) are not folded into
  each other.  The detail is a copy of the board's words, up to
  `CAMERA_SVC_DETAIL_MAX` (319) and possibly several lines.  Cleanup is the
  adapter's; the command never releases anything on the strength of a result.
- **A report is written from one sample**, one whole line per write, never with
  a camera lock held; `camera stream stats` takes ONE sample for the state, the
  holders head and the board's lines.
- **A frame read checks and copies in one step** under the same exclusion as
  every path that starts overwriting the buffer.  `save` / `send` read the last
  `camera capture`.
- **`camera off` waits for the stream it stopped by its generation**, and leaves
  the power on after a TIMEOUT, any other failed stop, or a Ctrl+C.
- **The capture reports its warm-up** (frames let go, or ms) on the caller's
  stack; the command prints `warm-up  : ...`, 0 included.
- **[!] The shared command owns no mutable storage**: line buffers, results and
  frame cursors are on the caller's stack.  The storage audit's
  `SHARED_COMMANDS` (`cmake/check_shared_storage_build.py`) lists
  `shell/cmds/cmd_camera.c` next to `cmd_nn.c` and `nn_cmd_core.c`, so every
  board's build audits it and fails without it.

## One line per shared nn / plugin / camera file

Each pair is a `.c` and its `.h` unless only a header is listed; the path is
relative to `svc/`.

| File | Role |
|---|---|
| `nn/nn_svc.h` | The contract the shared `nn` command speaks; implemented by each board's adapter. |
| `nn/nn` | The inference API (`nn.h`) and its one dispatcher for every board (issue #161; grove-vision-ai-v2 since issue #169): the model singleton and its first-open latch, the session guard, the external claim on the backend's memory (f746g-disco's `sdram test`), cycle timing around `nn_run()`, and named refusal codes passed through unfolded. |
| `nn/nn_backend.h` | The vtable the one compiled-in backend (`null`, `tflm`, f746g-disco's `stedgeai*`, grove-vision-ai-v2's `ethosu`) exports as `nn_backend_vt_selected`. |
| `nn/nn_dispatch_state.h` | What a board gives `nn.c`: its state object and the hooks (see [The inference layer](#the-inference-layer-is-one-implementation-issue-161)). |
| `nn/nn_desc` | `nn_tensor` -> `svc/nn/tensor.h` descriptor, for the boards that use `nn.h`. |
| `nn/nn_input_contract` | wio-lite-ai's and f746g-disco's camera input (issue #162): `nn_input_admit()`, whether a camera can fill the input tensor (float32 or int8, 1xHxWx1/3, a positive normal int8 scale and an int8 zero point, a 4-byte aligned float32 buffer, the bytes for the shape in 64 bits; each refusal its own reason), and the inline per-element conversion (`nn_input_f32()`, `nn_input_i8()`: one explicit fused multiply-add for the `nn norm` range, then 1/scale, a +-255 limit, +-0.5 toward zero, zero point, saturation). The boards keep only how a pixel is sampled; grove-vision-ai-v2 does not use it. Host-tested against the target's instruction sequence. |
| `nn/nn_input_codes` | Those reasons as the start codes and sentences both boards report (`NN_INPUT_START_ERR_*`: -3, -11, -18..-25), which each board's `nn_camera.h` takes by name. |
| `nn/tflm/` | The one tflite-micro backend of wio-lite-ai and f746g-disco (issue #161; built only with `CONFIG_NN_BACKEND=tflm`, into the board's `tflm` library): `nn_tflm.cc` (the vtable), `nn_tflm_state.h` (its state type and facts, defined by the board), `nn_tflm_mem.h` (the buffers the board places; see [The inference layer](#the-inference-layer-is-one-implementation-issue-161)), `nn_tflm_ops.h` (the op profiles, shared with each board's host `verify_tflite`; includes the one-operator `ethosu` profile), `nn_tflm_fit.h` (which slot a model range may be adopted from, never the live one; host-tested), `nn_tflm_model_check.h` (the bounded flatbuffer verifier and its limits, and the model's graph before planning -- every list present, at most 64 long and in range, every tensor's rank, every I/O dimension within 0..65535 -- shared with the Ethos-U backend and the three host gates; issues #175, #178), `nn_tflm_glue.h` (the glue that does not depend on staging -- descriptors, name, empty state, op resolver, interpreter destruction -- included into each TFLM-based backend's own translation unit with its state object named by `NN_TFLM_GLUE_STATE` / `NN_TFLM_GLUE_FACTS`; issue #169) and `cxx_runtime.cc` (the C++ runtime shim). One of the two directories under `svc/` that include TFLM headers (the other is `nn/ethosu/`). |
| `nn/ethosu/` | grove-vision-ai-v2's backend behind `nn/nn.c` (issue #169): `nn_ethosu.cc` (the vtable; models interpreted in place inside the board's read window), its state, facts and arena headers, the bounded verifier's entry and length floor (`npu_verify.h`; the limits are `nn/tflm/nn_tflm_model_check.h`'s), the payload locator and model refusal (`npu_model_scan`) and the command-stream walk (`npu_payload`), which the host `verify_vela_model` compiles from the same files, and the arena ownership protocol with the driver's cache hooks (`npu_cache`) (see [The Ethos-U backend](#the-ethos-u-backend-issue-169)). |
| `nn/nn_svc_adapter.h` | Static-inline helpers every adapter uses to build a result and an `nn info` line. |
| `nn/nn_detail.h` | The bound on a board's `nn` explanation text and its build-time check. |
| `nn/nn_stream_life` | The one live-inference state machine (start admission, generations, stop claims). |
| `nn/nn_core` | The policy around that machine and the stream's numbers, called from a console. |
| `nn/nn_core_frame` | The frame path: what the producer, worker and panel do with each frame. |
| `nn/nn_handoff` | The hand-over word that says who may touch the model's input and when. |
| `nn/nn_det_record` | The published decode record and the generation rule that keeps a stale decode out. |
| `nn/nn_core_model` | The order of `nn model load` and `nn model unload`. |
| `nn/nn_swap` | Where a model load ended up and what that obliges (the ending table). |
| `nn/nn_active_core` | Whether a plugin decodes, and every call into its decoder slots. |
| `nn/nn_active_lease` | The lease-checked entries above that branch, for a board whose model is a `struct nn_model` (wio-lite-ai, f746g-disco); the board supplies its facts (`nn_active_facts`). |
| `nn/nn_report` | The bounded sink that captures a plugin's account of its own result. |
| `nn/nn_top` | The top classes of an output tensor, taken on the thread that ran the inference.  No board links it since f746g-disco's resident decoder went (issue #159 Step 4c); host-tested only.  The carriers are still there with no producer: `nn_det_record_publish()`'s top-5 (`NN_EXTRA_TOP`, issue #121) and the `nn run` / `nn dets` printing of it in `shell/cmds/cmd_nn.c`, and no firmware calls `nn_det_record_publish()` (the caller-boxes publish) either. |
| `nn/nn_run_wait` | How `nn run`'s wait for its one inference ends (INFERRED > NO_RESULT > LOST > CANCELLED > TIMEOUT, and the post-deadline promotion of a late publish); every board since issue #163 (grove-vision-ai-v2's until then), and all three pass "finished" since issue #171, so the wait ends one of these five ways everywhere. Host-tested (`shell/test/test_nn_run_wait.c`). |
| `nn/nn_shot` | How one `nn run` ended and whether it is still open (issue #171), linked by wio-lite-ai and f746g-disco. grove-vision-ai-v2 keeps its own one-frame `nn_ov_shot` state and links this file for its words only (issue #174): its `port/nn/nn_run_says.c` answers the endings this table names (prep, invoke, lease not had in time, decode reached unleased) with `nn_shot_says()`, and keeps its own sentences for the ones only it has (which output is unreadable, a publish the record did not take, a stop first). It keeps the run's number and the first thing that ended it, over state the board passes in: the producer asks whether the run is open before it starts filling a frame, and the worker arms again only while it is, so a `nn run` is one frame. Every ending is recorded in one hold of the lock that moves the board's record generation (f746g-disco `nncam_lock`, wio-lite-ai `nncam_det_lock`; not the interrupt mask), and the first ending wins. A publish the record took ends the run but is not a failure, so it is never passed to the wait as "finished" (the wait sees it in the accepted count, as INFERRED). A worker answer the table does not know ends the run as NO_RESULT ("the inference worker ended the run without a result"), not as keep-waiting. Each failure's status and sentence come from the one table in `nn_shot.c`. Pure, no mutable storage. Host-tested (`shell/test/test_nn_shot.c`, and `shell/test/test_nn_shot_seq.c`, which drives both boards' call orders through the real `nn_core_frame.c` and `nn_handoff.c`). |
| `nn/nn_refusal` | The one table of `nn` admission refusals and of how a `nn run` ended without a result (issue #174): for `nn run`, `nn stream start` and `nn stream stop`, the lifecycle's answer -> status, claim disposition and sentence, written into the caller's result; the BUSY sentence is picked by whether the board has a gate (`nn_core_board.gate_held`), never by board name; a value outside the enum is BUSY, NONE for all three. For `nn run`'s LOST / CANCELLED / TIMEOUT (the wait in ms) the status and the base sentence, followed by the board's clause on what its teardown did not release, appended only when the disposition is not NONE and checked at build time against the longer of `NN_REFUSAL_RUN_END_MAX` and `nn_shot.h`'s `NN_SHOT_SAYS_MAX` (`NN_REFUSAL_RUN_BASE_MAX`, `NN_REFUSAL_RUN_UNFINISHED()`; wio-lite-ai and f746g-disco follow a NO_RESULT sentence with the same clause). Pure, no mutable storage. Linked by all three boards; host-tested (`shell/test/test_nn_refusal.c`: every input's whole sentence against the expected text and `NN_SVC_DETAIL_MAX`, the status and claim of every answer, and the constant equal to the longest base sentence). |
| `nn/nn_svc_common` | The parts of the adapter that were the same word for word (issue #174): `nn info`'s backend / model / arena section for wio-lite-ai and f746g-disco (the name copied under the NN session when it is free, else kept only if the shared load order's counter did not move; BUSY while another command holds the backend's memory), with the board passing only its read of that counter; and `nn thresh` on all three boards, in one order on every exit (count in, lease, the decoder's entry, lease back, count out) with the board's hooks in a const `struct nn_svc_thresh_ops` (the count is grove-vision-ai-v2's only; a build with no decoder passes no entries and answers NONE / STATE). Each board keeps one-line wrappers. Pure, no mutable storage. Host-tested (`shell/test/test_nn_svc_common.c`). |
| `nn/nn_param_calls` | The `nn` gate's holder and the count of console calls inside the plugin from outside the gate (`nn thresh`, `nn dets`), each transition run in the board's critical section; grove-vision-ai-v2 only (wio-lite-ai and f746g-disco protect those plugin entries with the lease alone). Moved from that board in issue #163; `nn thresh` reaches it through `nn_svc_common`'s count hooks since issue #174. |
| `nn/nn_decode_count` | How a decoder's answer is counted in `nn stream stats` (`model_errors`, `decoder_errors`) and the tally of one publish: only a stream's publish the record took counts, a raw one as an inference, a refused decode as an error and not an inference, a one-shot (`nn run`) as nothing; all three boards since issue #164 (wio-lite-ai and f746g-disco since #163; enum order MODEL = 1, DECODER = 2). |
| `nn/nn_container` | Whether a file read for `nn model load --path` is a plugin container, and the CRC-32 of exactly the bytes read, the one function both of f746g-disco's printers call (its only consumer; moved in issue #163). |
| `nn/nn_sess_release` | When `nn stream`'s hold on the nn session may be given back: a pure function of the sink drain and the worker; f746g-disco's (moved in issue #163). |
| `nn/nn_bench` | The one `nn bench` procedure on every board (issue #163): acquire -> fill every input with `0x5A` -> N timed runs, each after a cancel question -> release, in one hold. The board passes its hold (acquire / release) and its clock as hooks in a const `struct nn_bench_ops`; the shared side never waits, takes no lock and has no mutable storage. Host-tested (`shell/test/test_nn_bench.c`). |
| `plugin/plugin_abi.h` | The wire ABI between the firmware and a plugin shipped with its model. |
| `plugin/plugin_target.h` | The plugin target word this translation unit is compiled for. |
| `plugin/plugin_load` | Container and manifest validation; returns offsets, never a callable pointer. |
| `plugin/plugin_exec` | Copies a validated image into the board's reservation and calls its entry. |
| `plugin/plugin_run_core` | Above that loader, for an Armv7-M board staging in RAM (wio-lite-ai, f746g-disco): the lease checked before a load or unload and again at the branch into entry, the load's log lines and the staging-region check; the board keeps its MPU read-back and cache maintenance. |
| `plugin/plugin_mpu` | Armv8-M MPU verdict: is the plugin reservation executable (pure function). |
| `plugin/plugin_mpu_v7m` | The same question for an Armv7-M PMSA, answered by its own rules. |
| `plugin/plugin_info` | The plugin stack lines of `nn info`. |
| `plugin/plugin_claims` | The open model's container claims (a board-owned store, read in the board's critical section) and the `nn info` plugin lines around the stack lines (all three boards; grove-vision-ai-v2 since issue #174). Host-tested (`shell/test/test_plugin_claims.c`, and `test_plugin_claims_order.c` under the shared load order). |
| `plugin/plugin_lease_api.h` | The plugin lease's API (one lock into the plugin); the board implements it. |
| `plugin/plugin_lease_miss` | What a refused no-wait lease acquire counts, over state the caller owns. |
| `plugin/plugin_paint_budget` | What one plugin `draw()` may spend, and the one place it is charged. |
| `plugin/plugin_paint_span` | What a painter primitive decides before its first store (clip, offset, charge). |
| `camera/cam_holders` | Who holds the camera stream open, and the start / stop / refusal each change implies (issue #142). |
| `camera/cam_step` | One step of the camera sink: may the producer enter it, and has it left (issue #142). |
| `camera/camera_svc.h` | The contract the shared `camera` command speaks; implemented by each board's adapter (issue #143). |
| `camera/camera_svc_adapter.h` | Static-inline helpers every camera adapter uses to fill a result and write a line. |
| `camera/cam_gen.h` | Stream generations: has THE stream a stop named ended (pure, shared by f746g-disco and wio-lite-ai). |

## Why the lock and the threads are the board's

`svc/` cannot include `tx_api.h` or any board header: the same object has to
build for three boards and for the host. So every mutex, every bounded wait and
every thread is the board's. The board implements the APIs this layer declares
(`plugin/plugin_lease_api.h`), and passes critical-section entry/exit, tick reads, the
lease and the decode as hooks in a const ops struct (`struct nn_core_frame_ops`,
`struct nn_core_board`).

Every entry into a plugin checks the lease; which entries exist is derived
from the objects, not listed (`cmake/check_lease_entries.py`, see
`cmake/README.md`).

The shared translation units also hold no mutable storage of their own: state
lives in a board-owned struct that the board passes in, or -- for `nn/nn.c`,
whose callers pass no context -- one the board defines under a name the shared
file declares `extern` (`nn_model_board_state`; the tflm backend's
`nn_tflm_board_state`; the Ethos-U cache protocol's `npu_cache_board_state`). This is enforced per
board by `cmake/check_no_mutable_storage.py` through the build-derived audit in
`cmake/shared_storage_gate.cmake` (see `cmake/README.md`; `svc/ymodem.c` is the
one exception).

## Where the per-board files are described

Each board README has an "nn files" table: one line per file under that
board's `port/nn/` (and `port/plugin/` where the board loads plugins), with the
thread it runs on and the hardware reason it is the board's.

- [Grove Vision AI V2](../boards/grove-vision-ai-v2/README.md#the-nn-files)
- [Wio Lite AI](../boards/wio-lite-ai/README.md#the-nn-files)
- [STM32F746G-DISCO](../boards/f746g-disco/README.md#the-nn-files)
