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
| `svc/plugin/` | The plugin container's ABI, loader, executor and the checks around them (`plugin_*`, below). How a container is built, stored, checked and run, end to end, is [`asset/README.md`](../asset/README.md). |
| `svc/camera/` | The camera frame pipeline (`frame`, `frame_pipeline`), the camera stream's holder table, sink steps and stream generations (`cam_holders`, `cam_step`, `cam_gen`, below) and the `camera` command's contract (`camera_svc`, below). |
| `svc/blob/` | The asset store's board-independent parts (issue #192): the header format and a transfer's decisions (`blob_state`), the slot table's rules and lookups (`blob_map`), the `blob write` coordinator (`blob_write`), and since issue #194 the one set of store results (`blob_err.h`) and the store contract of the shared `blob` command (`blob_svc.h`); see [The asset store's shared parts](#the-asset-stores-shared-parts-issue-192). No file here is named `blob.h` (every board has a `src/blob.h` of its own). |
| `svc/net/` | The contract of the shared `net` command, `net_svc.h` (issue #219; f746g-disco and wio-lite-ai, grove-vision-ai-v2 has no network). A header only: the command is `shell/cmds/cmd_net.c`, the one implementation under it is `shell/cmds/net_core.c` (issue #220) and the one telnet console is `shell/cmds/net_tty.c` (issue #221) -- both need NetX and ThreadX, so they cannot live here -- and each board's adapter is the board's; see [The `net` command and the telnet console are one implementation](#the-net-command-and-the-telnet-console-are-one-implementation-issue-212). |

Every one of the six directories is on the
include path wherever `svc/` is, so a source names a header by its file name
alone (`#include "nn_core.h"`), and the storage audit covers the whole tree
under `svc/`.

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
| `nn info` | The backend (`nn_backend()`'s name and version), the model and where it came from, the arena reserved and used, the input and output tensors, then the board's plugin lines; answered in full while a stream runs, busy while a load is in flight. [!] One report never mixes two models (issue #199): the model section is a copy that carries which state of the shared load order it belongs to (`nn_svc_info::seq`), and every later line -- in[0], the output count, each output, the threshold, the plugin lines -- is read first and then checked against it (`nn_svc_info_stands()`). From the first line whose check fails nothing more is read: the tensors end with one `tensors : -- another nn command is using the model; ask again when it returns` line, and the threshold and plugin lines say the same. A line that could not be read while the check still passes keeps `unavailable just now`. Host-tested through the real command, with a whole load injected at each read (`shell/test/test_nn_info_one_state.c`). | always | f746g-disco: `sdram test` holding `.sdram.ai` answers busy, and so do the threshold and plugin lines (that copy belongs to no one state).  grove-vision-ai-v2: also busy while a `nn run` or `nn bench` holds its gate; the `source` line says the NPU runs secure and privileged. |
| `nn model load <name>` | Point the model at the blob stored under `<name>` and swap it in as one transaction (the plugin after the backend succeeds; the order is `nn/nn_core_model`). | `MODEL_LOAD` | grove-vision-ai-v2: a NOR blob by name, read in place through the XIP window.  wio-lite-ai: a NOR blob by name (since issue #193) copied to PSRAM, the CRC checked on the copy.  f746g-disco: a QSPI blob by name (since issue #194; a microSD file before) copied to SDRAM, the CRC checked on the copy; `stedgeai_reloc` refuses a blob past its 448 KB slot.  On every board the closing check of a delivered asset is `blob list`'s crc32 against the receipt. |
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
| `NN_SVC_HAS_CAMERA`, `_BENCH`, `_STREAM` | yes | yes | yes |
| `NN_SVC_HAS_NORM` | -- | yes | yes |
| `NN_SVC_HAS_STREAM_TEST` | -- | yes | -- |

The declarations are `boards/<board>/svc/nn_svc_config.h`; a capability that is
0 leaves its subcommand unregistered.

**The model is named by one word, the blob name (issue #196).**  All three
boards look it up with the same `blob/blob_state` name rules, so the word means
the same thing everywhere -- which is what the tagged forms (`--name`,
`--slot`, `--path`, `builtin`, Grove's `--addr <addr> <len>`) existed to
guarantee while the boards' stores differed, and why none of them comes back.
`builtin` or `9` is an ordinary name now.  A word that starts with `-` is not
a name, and the rule lives in two places on purpose:

- the parser (`shell/cmds/nn_cmd_core.c`) refuses a first word that starts
  with `-` as "not a model name -- write the name alone", whatever follows, so
  an old `--name x` or `--addr 0x... 16` is told what to type instead of being
  read as two words.  It does not call `blob_name_check()` -- a build with no
  store still links the parser -- and `shell/test/test_nn_cmd_core.c` checks
  that the two agree over one list;
- `blob_name_check()` refuses it as `name starts with '-'`, so `blob write`
  never stores a name that could not be typed back.

The reader is wider than the writer: the header decoder keeps its own byte
check, so a `-x` blob stored before the rule still lists as `valid`, guards its
slot, and goes with `blob erase <slot>`.  It cannot be loaded by name (the
board's pre-check uses `blob_name_check()`).

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
grove-vision-ai-v2) call (issues #175, #178, #180, #181, #183, #185, #188).  The walk runs in a fixed order:

1. Structure (`NN_MODEL_ERR_FORMAT`): subgraph 0's input / output lists present
   and naming tensors that exist; every subgraph has a tensor vector; every
   operator present has an inputs and an outputs list, each of its inputs /
   outputs / intermediates is at most `NN_TFLM_MAX_OP_TENSORS` (64) long --
   lengths judged before any element is read -- and every index names a tensor
   of its subgraph (or -1, the optional tensor, in inputs).  TFLM itself range-checks
   none of these indices: its planner writes per-tensor records through them.
   And the quantization, by lengths only (issue #180): every tensor's scale vector
   at most `NN_TFLM_MAX_QUANT_CHANNELS` (4096; max 1,280 over the 43 models
   found that parse) long, its
   zero-point vector absent, 1 long or the scale's length (TFLM otherwise reads
   `zero_point[0]` for every channel, silently), a per-channel tensor's
   `quantized_dimension` inside its declared rank when the rank is 2 or more
   (TFLM aliases dims to the serialized shape and the filter's kernels read
   `dims[qd]` without checking the rank), or in 0..`NN_TFLM_MAX_TENSOR_RANK` - 1
   when the rank is 0 or 1 (the converter's rank-1 per-channel biases carry the
   filter's qd -- TFLM's person_detect, qd 3 -- and no kernel indexes them by
   it), and the scale lengths named by every
   operator entry of every subgraph adding up to at most `NN_TFLM_MAX_QUANT_REFS`
   (65,536; max 34,292 over the same 43 models).  Whether a scale's length matches `dims[qd]`
   is left to the kernels, which check it.
   And the buffer indices and the offline plan (issue #181): every tensor of every
   subgraph and every metadata entry names a buffer below the buffer vector's
   size (0 when the model has none) -- TFLM reads `(*buffers)[buffer()]` for
   both unchecked, and a release build compiles out flatbuffers' assert; every
   metadata entry TFLM takes for the offline plan (the name exactly
   `OfflineMemoryAllocation`, 23 bytes, compared as TFLM compares it) names a
   buffer with data, at least the 12-byte header, a tensor count (word [2], a
   signed int32) equal to every subgraph's tensors together -- what TFLM
   compares it with, refusing a mismatch only inside `AllocateTensors()`, where
   a board says `NN_MODEL_ERR_ARENA` and the gates that do not plan would have
   passed it -- and room for that many offsets, each -1 (plan online) or >= 0
   (TFLM's greedy planner keeps any other negative value as a fixed offset and
   its arena check adds offset + size, so -size < offset < -1 placed a tensor
   before the arena) and below 2^30 (issue #188: that int sum must not wrap,
   see the output frames below); every such entry is checked (TFLM reads each and keeps the
   last).  Of the 12-byte header only word [2] is read (words [0] / [1] are read
   by no one); an offset's alignment and the arena fit stay the planner's.
   Buffer 0 need not be empty (TFLM does not rely on it;
   Vela points its non-constant tensors there).  And the model has an
   operator-code vector (TFLM reads its size without a null check).  One compare
   per tensor entry, a constant per metadata entry (a table, so one verifier
   visit each).
   And the constant buffers (issue #183): TFLM points a tensor whose buffer has
   data at those bytes, in the model, and never compares their length with the
   shape (its byte counts multiply the shape in `int`; the verifier proves only
   that the vector lies in the file), while a kernel that reads a constant by its
   own dims, which are the serialized shape, reads a short buffer under a large
   shape past the model (a constant a kernel reads by ANOTHER tensor's dims is
   the operator rules', below).  And the planner does not place a tensor with data,
   so a graph input the board fills or an operator output a kernel writes would
   be written into the model (PSRAM staging, SDRAM, the XIP window).  So every
   tensor of every subgraph, data or not, has a type TFLM gives a size (its
   planner fails `AllocateTensors()` on STRING / VARIANT / an unknown value --
   `NN_MODEL_ERR_ARENA` on a board, a pass on the gates that do not plan); every
   tensor with data has at most `NN_TFLM_MAX_TENSOR_RANK` dimensions (judged on
   the length first, so a rank-7 constant is FORMAT, not TENSOR), none negative,
   and data EXACTLY element count x element size long (1 element for no shape,
   INT4 two to a byte; the product bounded by what the data can hold before each
   multiplication, so it cannot wrap; a longer buffer and a 0-element constant
   with data are refused as well -- a policy, every constant of the 44 models
   measured, 1,320 of them, is exact); and no operator output of any subgraph and
   no input / output of subgraph 0 has data (a variable neither).  The sparsity
   table is not read: TFLM micro reads a constant dense, so a sparse one is short
   and refused.  Per tensor entry one type, one buffer and at most 6 dimension
   values; per operator entry one buffer per output.
   And the operator rules (issue #185): some registered kernels read a constant
   by ANOTHER tensor's dims or as a type of their own -- the int8 CONV_2D reads
   the filter for, and the bias as, the output's channel count; DEPTHWISE_CONV_2D
   reads them by input depth x depth multiplier (and a multiplier of 0 never ends
   one CMSIS-NN loop); the int8 FULLY_CONNECTED reads the weights and the bias
   by the output's last dimension; the bias is read as int32 under an int8 input
   whatever its declared type -- and their `Prepare()` compares the two with a
   `TFLITE_DCHECK` (compiled out by `NDEBUG`) or not at all.  (CMSIS-NN's
   CONV_2D `Prepare()` does have a bias check, but it fetches the bias as output
   2, which is always null, so the check never runs.)  So, per operator entry,
   after its lists: R0, for every profile, its `opcode_index` is below the
   operator-code vector's length (TFLM compares that only inside
   `AllocateTensors()`, `NN_MODEL_ERR_ARENA` on a board, and the wio-lite-ai /
   f746g-disco gates, which read only the code vector, passed it); then its kind
   (the larger of `builtin_code` and `deprecated_builtin_code`, as
   `GetBuiltinCode()` reads it) picks one rule, through a switch built from
   `nn_tflm_ops.h`'s selected list, so a profile compiles only its own
   operators' rules and an operator added to the list without one does not
   build:
   - R1 CONV_2D: input, filter, output rank 4; `filter[0] == output[3]`; a bias
     (input 2, not -1, data or not) of `output[3]` elements, its type the one the
     kernel reads (FLOAT32 / FLOAT32, INT8 / INT32, INT16 / INT32 or INT64);
     under an int16 input or an INT4 filter no grouping, `filter[3] ==
     input[3]` (those CMSIS-NN kernels size a filter row by the input's
     channels and read groups times past the filter; int8 with an INT8 filter
     handles groups and stays allowed).
   - R2 DEPTHWISE_CONV_2D: the three rank 4; `filter[0] == 1` (the reference
     kernel's DCHECK); `filter[3] == output[3]`;
     `depth_multiplier >= 1` (an absent or other options table reads as 0) and
     `input[3] x depth_multiplier == filter[3]`; a bias of `filter[3]` elements
     (FLOAT32 / FLOAT32, INT8 / INT32, INT16 / INT64).
   - R3 FULLY_CONNECTED: weights rank 2, output rank 2..4; `weights[0]` equal to
     the output's last dimension; the input's elements equal to the output's
     other dimensions multiplied, times `weights[1]` (no division; a product
     past 32 bits is refused before it wraps); a bias of `weights[0]` elements
     (FLOAT32 / FLOAT32, INT8 / INT32, INT16 / INT64).
   - R4 PAD: the paddings INT32 (read as int32 whatever their type) and the
     input's rank equal to the output's (TFLM counts the paddings against the
     output's rank but reads 2 x the input's, inside `AllocateTensors()`); with
     data (issue #188), 2 x the input's rank of them before any value is read,
     each >= 0, and `input + before + after == output` in every dimension, in
     64 bits.
   - R5 STRIDED_SLICE: begin, end and strides INT32, each with the input's rank
     of elements (read so, no count check).
   - R6 TRANSPOSE: the permutation INT32; with data (issue #188), the input's
     rank (at most 6) of elements before any value, each in [0, rank), the
     output of that rank, and `output[i] == input[perm[i]]`; a repeated value is
     refused (step 3: CMSIS-NN's int8 rank-2 path ignores the permutation and
     writes the input's n x h, so `{0, 0}` over [4, 64] into [4, 4] wrote 256 B
     into a 16 B frame).
   - R7 RESIZE_BILINEAR / RESIZE_NEAREST_NEIGHBOR: the size INT32, rank 1, 2
     elements, the output rank 4; when the size has data, its two values equal
     `output[1]` and `output[2]` (the kernels write the output by those values).
     [!] A size without data is not compared: `Prepare()`'s live "constant"
     check refuses it on a board (`NN_MODEL_ERR_ARENA`) and the gates, which do
     not plan, pass it -- an existing difference this does not change.
   - R8 ADD / SUB / MUL (two inputs or more, inputs 0 and 1 judged -- the only
     two the kernels read; ADD's and SUB's `Prepare()` do not count the inputs,
     MUL's requires 2): the three ranks at most 6, each input's at
     most the output's; aligned on the right, each input dimension 1 or the
     output's and at least one of the two the output's -- the output is the
     broadcast (the loop runs over the output's dims and steps an input wherever
     its dimension is not 1, so anything else is read past).
   - R9 CONCATENATION: every input of the output's rank, the axis (absent: 0,
     negative: from the end) in range, every other dimension the output's, the
     axis dimensions (none negative) adding up to the output's.
   - R11 .. R13 (issue #188), below.  RESHAPE's and CUSTOM's rules are empty,
     the reason beside them; the ethos-u profile has only CUSTOM's, so on
     grove-vision-ai-v2 R0 (and R10 / R10b, which are not operator rules) is
     all that runs.
   A list shorter than the kernel needs, or a required tensor that is -1, is
   left to `Prepare()`'s live check; every rule judges its tensors' ranks before
   any dimension (so a rank-7 convolution input is FORMAT, not TENSOR) and
   compares dimension values without sizing any work by them.  A board and its
   gate, built with the same profile define, agree; two profiles may not (a
   FULLY_CONNECTED that breaks R3 is FORMAT on f746g-disco and an unregistered
   operator on wio-lite-ai's `blazeface` gate).  Every rule passes all 44 models
   found that pass the verifier.
   And the output frames (issue #188): the planner gives each tensor a frame of
   its declared shape x TFLM's element size, and some kernels decide how much
   they WRITE by something else -- the input's shape, the options, a tensor's
   values -- with only a `TFLITE_DCHECK` or `MatchingFlatSize()` against the
   output (compiled out by `NDEBUG`).  Past the frame lies the next planned
   tensor and then the allocator's persistent records (the eval tensors' data
   pointers); before it, the model's slot.  Read in the pinned tree, against what
   the issue assumed: a pool or RESIZE with another batch or channel count does
   NOT write past (`MatchingDim()` returns the minimum and the kernels loop over
   it); a pool below rank 4 does (its loop counts come from past the shape); a
   kernel that loops over the output's H and W is not safe under CMSIS-NN
   (CONV_2D's 1x1 fast path sizes its rows by the input, the depthwise opt path
   memsets the scratch by an output H past the formula and keeps its bases in
   int16 -- the window rule below); a size past `int` is every
   tensor's problem, not RESHAPE's (the planner multiplies every shape in
   `int`: `[1, 65536, 65537, 1]` int8 is planned as 64 KB); and, found here, a
   negative PAD padding moves CMSIS-NN's write BEFORE the output, MEAN's float
   kernel writes one stack entry per axis into `axis[4]`, and the int8
   TRANSPOSE writes by the output's strides over the input's dims.  So:
   - R10, every tensor of every subgraph, every profile (grove-vision-ai-v2
     included), in the tensor loop after the type and before the constant's
     length: no dimension negative (-1 too: TFLM's `int` size of such a shape,
     rounded up to 16 in `size_t`, is a 0-byte buffer the planner does not
     refuse -- so RESHAPE's -1 is no longer available), the product read in
     TFLM's order and refused before a multiplication would pass `INT32_MAX`,
     and elements x `TfLiteTypeSizeOf()` (INT4 is 1 byte there) at most
     `INT32_MAX - 15`, so the size rounded up to 16 is an `int` too.  A rank
     above 6 is left to the rank rule (TENSOR).
   - R10b: those sizes rounded up to 16, summed in 64 bits over every tensor
     entry of every subgraph (a shared table counted per entry), at most 2^30,
     and every offline-plan offset below 2^30 (above): the greedy planner adds
     offsets and sizes in `int`, and with both bounds offset + size stays below
     2^31.  A bound for the arithmetic, not an arena size (every board's arena
     is 512 KB or less; the planner still judges the fit).
   - R11 QUANTIZE / DEQUANTIZE / RELU / RELU6 / LOGISTIC / LEAKY_RELU / SOFTMAX:
     input 0 and output 0 (neither -1) of rank at most 6 with equal element
     counts (the shapes may differ).
   - R12a MAX_POOL_2D / AVERAGE_POOL_2D: input and output rank 4.
   - R13 MEAN: the axis INT32 and with data (without, refused), at most 4
     elements before any value (the float kernel's `axis[4]`; every type), the
     input's rank at most 6, each axis in [-rank, rank) (negative from the end),
     a repeated one once; the output the input with each axis 1 under
     `keep_dims` (ReducerOptions; absent: false), else with the axes removed.
   - The window (step 2), for CONV_2D, DEPTHWISE_CONV_2D and the pools: the
     output's H and W equal TFLM's `ComputeOutSize()` -- `eff = (filter - 1) x
     dilation + 1`, SAME `(in + stride - 1) / stride`, VALID `(in + stride -
     eff) / stride` -- with the options table present (absent reads as stride
     0), stride, dilation and filter at least 1, padding SAME or VALID, eff, the
     numerator and `Prepare()`'s padding `(out - 1) x stride + eff - in` inside
     int32 (computed in 64 bits), and a 32-bit division as TFLM's.  CONV_2D also
     keeps H, W and C of its three tensors and its strides at most 65535 (the
     general path holds them in uint16); DEPTHWISE_CONV_2D keeps `(output - 1)
     x stride` at most 32767 in H and W (its int16 row / column bases) and its
     batch the input's; and (step 3) the padding `Prepare()` derives, the total
     above (0 when negative) halved, at most 32768 in H and W, since the base
     is `output x stride - padding` (SAME with filter 65539 over 1 pads 32769
     and wrapped it).  The pools (R12b; `filter_height` / `_width`, dilation
     1) and RESIZE keep batch and channels the input's -- a policy, since those
     kernels loop over the smaller.
   - RESHAPE needs no rule: under R10 its live element-count compare and the
     copy of the input's bytes are exact.
   NOT bounded -- what remains: the scratch a kernel requests in its `Prepare()`
   is sized by the kernel from the dims, so its size and sum are not in R10b;
   the planner's arena check is what stops it.  The dimension values are read
   by the size bound and the operator rules' compares only; no work is sized by
   them.  All 44 models pass (no negative dimension, the largest non-constant
   tensor 1,247,616 B, the largest sum of sizes 15,114,544 B, against 2^30).
2. The count of each I/O list (1..`NN_MAX_IO`, `NN_MODEL_ERR_SHAPE`).
3. Rank (`NN_MODEL_ERR_TENSOR`): every tensor of every subgraph at most
   `NN_TFLM_MAX_TENSOR_RANK` (6, TFLM's `RuntimeShape::kMaxSmallSize`, which
   TFLM only DCHECKs), each I/O tensor at most `NN_MAX_DIMS`.  Only the length of
   a shape is read here; the values of a tensor without data are read only by
   the size bound (R10) and the operator rules, which compare them (Vela's
   internal tensors reach 2.2 M elements, far inside R10, and the ethos-u
   profile has no operator rule), a constant's are read in step 1.
4. The I/O values: every dimension inside 0..65535, the range of `struct
   nn_tensor`'s 16-bit dims (`NN_MODEL_ERR_DIMS`, the predicate
   `nn_tensor_dim_fits()` in `nn.h`, tied to the field's width at compile time;
   a negative one has already been refused by R10 as `NN_MODEL_ERR_FORMAT`).

All of it runs on the model, before `AllocateTensors()`; the SHAPE / TENSOR /
DIMS checks on the planned tensors stay as the backstop and give the same codes.
The order is what bounds the shape, operator-list and index reads of a load:
the verifier checks a scalar vector in O(1), does not count it as a table and
accepts many entries naming one table, so a 512 KB file could hand
`AllocateTensors()` ~1e8 shape and list reads behind the 4,096 visits.  Lengths
first, the walk does one compare per tensor entry and at most 3 + 3 x 64 reads
per operator entry, each entry a verifier visit -- at most 4,096 x 192 list
reads -- and what it passes bounds TFLM's own passes over the same vectors the
same way.  The same holds for TFLM's zero-point copies (issue #180): it copies a
tensor's zero points (one int per scale entry) each time a kernel's `Prepare()`
takes the tensor, freed per node, so with the cap and the sum they total at
most 65,536 per load (plus subgraph 0's I/O, at most 2 x 8 x 4,096); without
them one 65,000-entry scale vector named by 2,000 tensor entries, or one tensor
named by 4,000 operator entries, passed the verifier.  The kernels' own
per-channel work is NOT bounded by this: conv / depthwise `Prepare()` size
their multiplier loops by the filter's dimension value, and `Invoke()` loops
over planned tensors -- both bounded by the arena, not by the file.  It does not bound all of `AllocateTensors()`: the per-operator
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
load's shape and list reads and its per-channel quantization copies (#180).
wio-lite-ai runs no verifier, so there only the file's length bounds the
entries.  grove-vision-ai-v2's payload walk per operator entry is bounded by its own
scan (`ethosu/npu_model_scan`, #179).  Neither keeps storage of its own; the board supplies everything
that is a fact about its memory or its core, bound at link time (the callers of
`nn.h` pass no context to carry it):

| What the board gives | Where | Why it is the board's |
|---|---|---|
| `nn_model_board_state` and four hooks: critical section (restores the mask it found), one-tick wait, cycle-counter start and read (`nn_dispatch_state.h`) | `port/nn/nn_model_board.c` | PRIMASK, the ThreadX tick and the DWT are the core's and the RTOS's; f746g-disco unlocks the DWT, wio-lite-ai's timebase already owns it |
| `nn_tflm_board_state` (`nn_tflm_state.h`) | `port/nn/tflm/nn_tflm_board.cc` | which RAM the interpreter's state lives in (until issue #201 also the facts, the model-name prefix: `""` on both boards since issue #194, f746g-disco said `sd:<path>` before) |
| `nn_tflm_arena`, `nn_tflm_model_buf`, `nn_tflm_stage_cap` (`nn_tflm_mem.h`) | `port/nn/tflm/nn_tflm_bufs.c` | placement: SDRAM bank3 on f746g-disco, the PSRAM carve-out on wio-lite-ai, each under that board's residency gate |
| `NN_TFLM_ARENA_KB`, `NN_TFLM_MODEL_KB`, `NN_TFLM_VERIFY`, the op profile | `cmake/tflite-micro.cmake` (compile definitions) | sizes the board's memory can hold, whether the board can afford the on-board flatbuffer verifier, and which kernels fit its flash |

The op profile is one definition selecting a complete list in
`nn_tflm_ops.h`; the resolver's size is counted from it.  Since issue #169 it
also holds the Ethos-U profile, `NN_TFLM_OPS_ETHOSU_PROFILE=1`: the one
`ethos-u` custom operator and no CPU kernel, which grove-vision-ai-v2's board.cmake
selects for the Ethos-U backend and for its host gate alike.

A loaded model's display name -- what `nn info` prints on its `model :` line --
is the blob name as it is, with no prefix (issue #201: the boards' prefix,
`sd:` on f746g-disco until #194 and `""` since, is gone, and with it the tflm
board facts, which held nothing else).  Every field it passes through is
`BLOB_NAME_MAX + 1` bytes, from `svc/blob/blob_state.h` and not a number of
its own: the backends' field (tflm, Ethos-U, and f746g-disco's
`stedgeai_reloc`; `nn_tflm_glue.h` asserts it against the state type) and
`nn info`'s copy (`NN_SVC_MODEL_MAX`).  Before #201 the backends' field was 64
bytes and a 64-character name lost its last character on that line (the
`plugin` section's `from :` line, the blob's own name, was whole);
`shell/test/test_nn_model_name.cc` pins it, built by wio-lite-ai's and
grove-vision-ai-v2's host tests against their TFLM trees.

Where `nn_model_reload()` may adopt a model from is the backend's to declare
(`nn.h`, issue #169).  A staging backend (tflm) adopts only from the buffer
`load_region()` handed out, aligned and not while the live model is in it.
Since issue #191 tflm has one 1 MB buffer, the open model's: `load_region()`
refuses while a model is open and writes nothing, the backend's table says so
(`staging_is_open`, answered by `nn_model_staging_is_open()`, issue #200) so the
shared load order takes the open model down first, and
a refused reload ends with no model (there is nothing to restore).  The capacity
alone is `nn_model_stage_capacity()`, answered whatever is open, for a board
that checks a length before the model goes down.  An in-place backend
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
`Prepare()`, loading as before) was refused as `NN_MODEL_ERR_DIMS`; since issue
#188 any negative dimension of any tensor is refused before that, as
`NN_MODEL_ERR_FORMAT`, by the size bound R10.

### The Ethos-U backend (issue #169)

`svc/nn/ethosu/` is grove-vision-ai-v2's backend behind the same `svc/nn/nn.c`
dispatcher the other two boards use, and the checks and the cache protocol that
come with it.  None of it keeps storage of its own: the stateless-service audit
compiles every TU here in Grove's firmware context, like every other TU under
`svc/`, and the C++ ones against the Himax SDK's TFLM snapshot, the only TFLM tree
that board builds.

| File | What it is |
|---|---|
| `nn_ethosu.cc` | The backend (`nn_backend_vt_selected`). A model is interpreted IN PLACE, inside the read window the board declares: no `load_region()`, no staging slot, no second buffer; the caller's lease keeps the bytes in place (`nn.h`). The checks run in one order -- window, length, alignment, identifier, bounded verifier, schema version, payload walk, the model's graph and I/O (`nn_tflm_model_check.h`), the command streams' reach (`npu_cmdstream`, issue #184), resolver, AllocateTensors, the I/O rule -- each refusal its own code (`NN_MODEL_ERR_WINDOW` / `_IDENT` / `_PAYLOAD` for the ones only an in-place backend gives; `_DIMS` as tflm). It tells the cache protocol which arena is live and closes out each `Invoke()` with its backstop. Transactional: a refused model leaves the previous one (rebuilt from the same bytes) or the empty state -- unlike tflm since issue #191, whose refusals always leave the empty state. Uses `svc/nn/tflm/nn_tflm_glue.h` with its state named. |
| `nn_ethosu.h` | C, for the adapter: `nn_ethosu_strerror()`, the sentences for the in-place refusals (every other code goes to `nn_model_strerror()`). |
| `nn_ethosu_state.h` | The state type and the board facts (the read window, the NPU configuration the models are compiled for: its SHRAM size and minimum output block -- issue #184). Needs `NN_TFLM_OPS_ETHOSU_PROFILE`: the resolver's size is part of the type. |
| `nn_ethosu_mem.h` | The arena declaration, sized by the board's `NN_ETHOSU_ARENA_KB`. |
| `npu_verify.h` | Grove's entry to the bounded FlatBuffer verifier: its length floor, then `nn_tflm_verify_model_buffer()` from `nn/tflm/nn_tflm_model_check.h`, where the limits (depth, table count) are declared for every board (issue #175); the host gate `verify_vela_model` compiles the same header. |
| `npu_model_scan` (`.cc` / `.h`) | Which bytes the driver will parse (the `ethos-u` operator's input tensor 0) and whether the model may be opened at all: one subgraph, every operator `ethos-u`, no variable payload tensor, no `-1` among an `ethos-u` operator's inputs after 0 (its `Eval()` reads them unchecked), and the payloads adding up to at most the model's declared length -- each charged before it is walked, so the scan reads at most length / 4 words however many operator entries share one stream (#179). And, run after the graph walk, the table of base pointers each `ethos-u` operator hands the driver (inputs 1.. then outputs, cut at 8; each its tensor's bytes, writable only without data) given to `npu_cmdstream` (#184). Shared with the host gate. |
| `npu_payload` (`.c` / `.h`) | Whether that payload is exactly one COMMAND_STREAM action, as the last action, and where the stream is in it. Plain C over bytes, so the host test compiles it directly. |
| `npu_cmdstream` (`.c` / `.h`) | Where a command stream makes the NPU read and write (issue #184): command by command, the range each DMA and each operation can reach, refused unless it lies inside the tensor its base pointer stands for and a write lands only where that tensor has no data; only commands the TRM and the driver both define, every register an operation uses set first, a final stop, one NPU-internal DMA shape (a lookup table into the last 2 KB of the SHRAM size the caller passes), and the values only an operation can judge (an output block that is a multiple of the caller's minimum block and covers a depth that is not a multiple of 16, a second input with the first input's sign and precision, no zero-length DMA or weight stream). A conservative bound from the TRM and Vela's address model, not measured against the hardware. Plain C over words, state on the stack; host-tested, and the host gate compiles it. |
| `npu_cache` (`.c` / `.h`) | The arena ownership protocol and the driver's four cache hooks (`ethosu_flush_dcache`, `ethosu_invalidate_dcache`, `ethosu_inference_begin`, `ethosu_inference_end`), strong definitions that override the driver's weak ones at link time: the whole arena cleaned before launch and invalidated once DONE and OK are both seen, a checked reset first on any other ending, fail-stop otherwise. Its state (`npu_cache_board_state`), the cache-line primitive (`npu_cache_clean()` / `npu_cache_invalidate()`) and the fail-stop are the board's (`port/nn/npu/npu_cache_board.c`). |

What the board gives, in `boards/grove-vision-ai-v2/`:

| What | Where | Why it is the board's |
|---|---|---|
| `nn_model_board_state` and the four hooks; the cycle counter reported unavailable, so `nn_last_cycles()` is 0 | `port/nn/nn_model_board.c` | the DWT stops in WFI while the inference thread sleeps on the driver's semaphore; `nn bench` and the stream keep their own clocks |
| `nn_ethosu_board_state` and the facts (`""`, `NOR_XIP_BASE`, `NOR_SIZE`, `NPU_CS_CONFIG_INIT`) | `port/nn/npu/nn_ethosu_board.cc` | which RAM the state lives in, the one statement of the read window, and the configuration the board compiles models for, its SHRAM and minimum output block (`port/nn/npu/npu_hw.h`; the SHRAM read back from the NPU by `npu_hw_init()`; issue #184) |
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
storage of their own.  Since issue #165 the plugin policy and the bindings
table are among them, bound by fixed name rather than passed, so the object the
build reads back is the one the loader uses.

| Header | What the board gives | What it decides |
|---|---|---|
| `nn/nn_dispatch_state.h`, `nn/tflm/nn_tflm_state.h`, `nn/tflm/nn_tflm_mem.h`, `nn/ethosu/nn_ethosu_state.h`, `nn/ethosu/nn_ethosu_mem.h` | the model API's and the backend's state, its hooks and its buffers | see [The inference layer is one implementation](#the-inference-layer-is-one-implementation-issue-161) and [The Ethos-U backend](#the-ethos-u-backend-issue-169) |
| `nn/nn_core.h` | `struct nn_core` (the live-inference state) and a `struct nn_core_board`: the critical section, the clock and its rate, the counter and record samplers, the optional transient-claim hooks decided with the lifecycle, `latch_extra`, and the properties `based`, `rearm`, `clock_needs_producer` | the stream's lifecycle and numbers, from this board's counters and clock |
| `nn/nn_core_frame.h` | `struct nn_core_frame` (the frame word's state), a `struct nn_core_frame_ops` (prep, wake the worker, the generation question, the raw and decoded publish, outputs, decode, the board's counting, the plugin lease's hooks) and a `struct nn_core_panel` | how a frame is filled, decoded, published and counted on this board's producer, worker and panel threads |
| `nn/nn_core_model.h` | a `struct nn_core_model_board` (its name check, its preconditions and claim, bring-up, fetch) and its `struct nn_core_model_backend` | where the model's bytes come from and what a load needs first; the order around the swap is the shared file's |
| `nn/nn_bench.h` | a `struct nn_bench_ops`: the hold and the clock | how a bench holds the model and times a run |
| `nn/nn_active_core.h` | a `struct nn_active_board`: is a plugin live, a slot's address, where a depth sample goes, the no-plugin shape answer | how the plugin's decoder slots are reached on this board |
| `nn/nn_active_lease.h` | `nn_active_facts` (`struct nn_active_lease_board`): output count and descriptors -- wio-lite-ai and f746g-disco, whose model is a `struct nn_model` | the lease-checked entries' view of the model |
| `nn/nn_det_record.h` | the `struct nn_det_record` and the lock it is read and published under | which lock orders the record against the board's other locks |
| `nn/nn_shot.h` | a `struct nn_shot`, moved only under the lock that moves the record's generation -- wio-lite-ai and f746g-disco (grove-vision-ai-v2 keeps its own `nn_ov_shot` state and links `nn_shot.c` for its words only: `nn_shot_says()` from its `port/nn/nn_run_says.c`, which keeps its own sentences for the endings only it has) | how one `nn run` ended |
| `nn/nn_refusal.h` | nothing new: the board's existing `struct nn_core_board` (its `gate_held` picks the BUSY sentence), and for a `nn run` that ended without a result its teardown clause as a `NN_REFUSAL_RUN_UNFINISHED()` literal | what its teardown did not release; the words and status of every refusal are the table's |
| `nn/nn_svc_common.h` | a const `struct nn_svc_thresh_ops` (the plugin lease, the decoder's threshold entries and its `NN_ACTIVE_NOT_HELD`, and on grove-vision-ai-v2 the count's way in and out), and for `nn info` on wio-lite-ai and f746g-disco its read of the load order's transition counter | which protections a threshold call takes on this board; the order and the answers are the shared file's |
| `plugin/plugin_lease_api.h` | the plugin lease itself (`plugin_lease_try` / `_take` / `_give` / `_held` and the miss counters), in the board's `port/plugin/plugin_lease.c` -- all three boards | who may be inside the loaded plugin; the wait and the lock order are the board's `plugin_lease.h` |
| `plugin/plugin_load.h` | `plugin_board_policy`, the one `const struct plugin_policy`, under that fixed name (issue #165) -- all three boards | what a container must be to be admitted; the loader takes no policy argument and reads this object, the one the build reads back |
| `plugin/plugin_bindings.h` | `plugin_board_bindings`, the one `const struct plugin_bindings`: the base handed to a plugin's entry at its head, then the painter's three primitives and the printer's write (issue #165) -- all three boards | which firmware function sits behind each plugin veneer; the loader, the painter and the printer are filled from it, and the build derives the veneer stack roots from its words |

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
| `CAMERA_SVC_HAS_SAVE` (`save [<medium>] <path>`) | 1 (sd; fs until issue #194) | `BSP_ENABLE_SD` (sd) | 0 |
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
  frame cursors are on the caller's stack.  The storage audit covers every
  source the build compiles under `shell/cmds/` (issue #206), and
  `cmd_camera.c` is marked `required` in its first line, so every board's build
  audits it and fails without it (see `cmake/README.md`).

## One line per shared nn / plugin / camera / net file

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
| `nn/tflm/` | The one tflite-micro backend of wio-lite-ai and f746g-disco (issue #161; built only with `CONFIG_NN_BACKEND=tflm`, into the board's `tflm` library): `nn_tflm.cc` (the vtable), `nn_tflm_state.h` (its state type, defined by the board), `nn_tflm_mem.h` (the buffers the board places: the arena and the one staging buffer, issue #191; see [The inference layer](#the-inference-layer-is-one-implementation-issue-161)), `nn_tflm_ops.h` (the op profiles, shared with each board's host `verify_tflite`; includes the one-operator `ethosu` profile), `nn_tflm_fit.h` (whether a model range may be adopted from the staging buffer, never while the live model is in it; called with one slot since issue #191; host-tested), `nn_tflm_model_check.h` (the bounded flatbuffer verifier and its limits, and the model's graph before planning -- every list present, at most 64 long and in range, every tensor's rank, every I/O dimension within 0..65535 -- shared with the Ethos-U backend and the three host gates; issues #175, #178), `nn_tflm_glue.h` (the glue that does not depend on staging -- descriptors, name, empty state, op resolver, interpreter destruction -- included into each TFLM-based backend's own translation unit with its state object named by `NN_TFLM_GLUE_STATE`; issue #169) and `cxx_runtime.cc` (the C++ runtime shim). One of the two directories under `svc/` that include TFLM headers (the other is `nn/ethosu/`). |
| `nn/ethosu/` | grove-vision-ai-v2's backend behind `nn/nn.c` (issue #169): `nn_ethosu.cc` (the vtable; models interpreted in place inside the board's read window), its state, facts and arena headers, the bounded verifier's entry and length floor (`npu_verify.h`; the limits are `nn/tflm/nn_tflm_model_check.h`'s), the payload locator and model refusal (`npu_model_scan`), the payload's action walk (`npu_payload`) and the walk of the command stream itself (`npu_cmdstream`, issue #184), which the host `verify_vela_model` compiles from the same files, and the arena ownership protocol with the driver's cache hooks (`npu_cache`) (see [The Ethos-U backend](#the-ethos-u-backend-issue-169)). |
| `nn/nn_svc_adapter.h` | Static-inline helpers every adapter uses to build a result and an `nn info` line. |
| `nn/nn_detail.h` | The bound on a board's `nn` explanation text and its build-time check. |
| `nn/nn_stream_life` | The one live-inference state machine (start admission, generations, stop claims). |
| `nn/nn_core` | The policy around that machine and the stream's numbers, called from a console. |
| `nn/nn_core_frame` | The frame path: what the producer, worker and panel do with each frame. |
| `nn/nn_handoff` | The hand-over word that says who may touch the model's input and when. |
| `nn/nn_det_record` | The published decode record and the generation rule that keeps a stale decode out. |
| `nn/nn_core_model` | The order of `nn model load` and `nn model unload`. A backend whose staging is the open model's own place says so (`staging_is_open`, issue #191): the load then takes the plugin lease and moves the transition counter to odd before fetch, takes the open model down in unload's order (no hardware down), and goes on as a load from empty -- so once that ran every failure ends EMPTY, a BUSY from fetch is reported as HW, and a lease refused before it still changes nothing. The fact is the backend's (issue #200): every board hands the table `nn_model_staging_is_open()`, which answers from `struct nn_backend_vt` -- 1 on tflm (its one staging buffer), 0 on ethosu, null, stedgeai and stedgeai_reloc -- and no board writes it itself. |
| `nn/nn_swap` | Where a model load ended up and what that obliges (the ending table). |
| `nn/nn_active_core` | Whether a plugin decodes, and every call into its decoder slots. |
| `nn/nn_active_lease` | The lease-checked entries above that branch, for a board whose model is a `struct nn_model` (wio-lite-ai, f746g-disco); the board supplies its facts (`nn_active_facts`). |
| `nn/nn_report` | The bounded sink that captures a plugin's account of its own result. |
| `nn/nn_top` | The top classes of an output tensor, taken on the thread that ran the inference.  No board links it since f746g-disco's resident decoder went (issue #159 Step 4c); host-tested only.  The carriers are still there with no producer: `nn_det_record_publish()`'s top-5 (`NN_EXTRA_TOP`, issue #121) and the `nn run` / `nn dets` printing of it in `shell/cmds/cmd_nn.c`, and no firmware calls `nn_det_record_publish()` (the caller-boxes publish) either. |
| `nn/nn_run_wait` | How `nn run`'s wait for its one inference ends (INFERRED > NO_RESULT > LOST > CANCELLED > TIMEOUT, and the post-deadline promotion of a late publish); every board since issue #163 (grove-vision-ai-v2's until then), and all three pass "finished" since issue #171, so the wait ends one of these five ways everywhere. Host-tested (`shell/test/test_nn_run_wait.c`). |
| `nn/nn_shot` | How one `nn run` ended and whether it is still open (issue #171), linked by wio-lite-ai and f746g-disco. grove-vision-ai-v2 keeps its own one-frame `nn_ov_shot` state and links this file for its words only (issue #174): its `port/nn/nn_run_says.c` answers the endings this table names (prep, invoke, lease not had in time, decode reached unleased) with `nn_shot_says()`, and keeps its own sentences for the ones only it has (which output is unreadable, a publish the record did not take, a stop first). It keeps the run's number and the first thing that ended it, over state the board passes in: the producer asks whether the run is open before it starts filling a frame, and the worker arms again only while it is, so a `nn run` is one frame. Every ending is recorded in one hold of the lock that moves the board's record generation (f746g-disco `nncam_lock`, wio-lite-ai `nncam_det_lock`; not the interrupt mask), and the first ending wins. A publish the record took ends the run but is not a failure, so it is never passed to the wait as "finished" (the wait sees it in the accepted count, as INFERRED). A worker answer the table does not know ends the run as NO_RESULT ("the inference worker ended the run without a result"), not as keep-waiting. Each failure's status and sentence come from the one table in `nn_shot.c`. Pure, no mutable storage. Host-tested (`shell/test/test_nn_shot.c`, and `shell/test/test_nn_shot_seq.c`, which drives both boards' call orders through the real `nn_core_frame.c` and `nn_handoff.c`). |
| `nn/nn_refusal` | The one table of `nn` admission refusals and of how a `nn run` ended without a result (issue #174): for `nn run`, `nn stream start` and `nn stream stop`, the lifecycle's answer -> status, claim disposition and sentence, written into the caller's result; the BUSY sentence is picked by whether the board has a gate (`nn_core_board.gate_held`), never by board name; a value outside the enum is BUSY, NONE for all three. For `nn run`'s LOST / CANCELLED / TIMEOUT (the wait in ms) the status and the base sentence, followed by the board's clause on what its teardown did not release, appended only when the disposition is not NONE and checked at build time against the longer of `NN_REFUSAL_RUN_END_MAX` and `nn_shot.h`'s `NN_SHOT_SAYS_MAX` (`NN_REFUSAL_RUN_BASE_MAX`, `NN_REFUSAL_RUN_UNFINISHED()`; wio-lite-ai and f746g-disco follow a NO_RESULT sentence with the same clause). Pure, no mutable storage. Linked by all three boards; host-tested (`shell/test/test_nn_refusal.c`: every input's whole sentence against the expected text and `NN_SVC_DETAIL_MAX`, the status and claim of every answer, and the constant equal to the longest base sentence). |
| `nn/nn_svc_common` | The parts of the adapter that were the same word for word (issue #174): `nn info`'s backend / model / arena section for wio-lite-ai and f746g-disco (the name copied under the NN session when it is free, else kept only if the shared load order's counter did not move; BUSY while another command holds the backend's memory), with the board passing only its read of that counter. Since issue #199 the copy carries the counter's value it belongs to: the second read is taken before the session is given back, and the first before the model open, so a copy -- including a "nothing loaded" one from an open that failed -- has a value only when nothing can have run in between; a BUSY copy has none. `nn_svc_common_info_stands()` is the boards' `nn_svc_info_stands()`; and `nn thresh` on all three boards, in one order on every exit (count in, lease, the decoder's entry, lease back, count out) with the board's hooks in a const `struct nn_svc_thresh_ops` (the count is grove-vision-ai-v2's only; a build with no decoder passes no entries and answers NONE / STATE). Each board keeps one-line wrappers. Pure, no mutable storage. Host-tested (`shell/test/test_nn_svc_common.c`). |
| `nn/nn_param_calls` | The `nn` gate's holder and the count of console calls inside the plugin from outside the gate (`nn thresh`, `nn dets`), each transition run in the board's critical section; grove-vision-ai-v2 only (wio-lite-ai and f746g-disco protect those plugin entries with the lease alone). Moved from that board in issue #163; `nn thresh` reaches it through `nn_svc_common`'s count hooks since issue #174. |
| `nn/nn_decode_count` | How a decoder's answer is counted in `nn stream stats` (`model_errors`, `decoder_errors`) and the tally of one publish: only a stream's publish the record took counts, a raw one as an inference, a refused decode as an error and not an inference, a one-shot (`nn run`) as nothing; all three boards since issue #164 (wio-lite-ai and f746g-disco since #163; enum order MODEL = 1, DECODER = 2). |
| `nn/nn_sess_release` | When `nn stream`'s hold on the nn session may be given back: a pure function of the sink drain and the worker; f746g-disco's (moved in issue #163). |
| `nn/nn_bench` | The one `nn bench` procedure on every board (issue #163): acquire -> fill every input with `0x5A` -> N timed runs, each after a cancel question -> release, in one hold. The board passes its hold (acquire / release) and its clock as hooks in a const `struct nn_bench_ops`; the shared side never waits, takes no lock and has no mutable storage. Host-tested (`shell/test/test_nn_bench.c`). |
| `plugin/plugin_abi.h` | The wire ABI between the firmware and a plugin shipped with its model. |
| `plugin/plugin_target.h` | The plugin target word this translation unit is compiled for. |
| `plugin/plugin_load` | Probing a payload and naming a result: what the firmware and the host share besides the parse. |
| `plugin/plugin_load_body.h` | Container and manifest validation, the one body (every function `static`); returns offsets, never a callable pointer. Included only by the two files below; its one external definition (`plugin_load_body_once`) makes a second includer in one link a multiple definition. |
| `plugin/plugin_load_board.c` | The firmware's entry, `plugin_parse()`: the body over the board's `plugin_board_policy`, by name; and the `plugin_policy_probe` layout record the build reads the policy with. |
| `plugin/plugin_load_host` (`.c` / `.h`) | The host's entry, `plugin_parse_with_policy()`: the same body with a policy argument, for the container verifier and the host tests. Never linked into a firmware (`cmake/check_policy_entry.py`). |
| `plugin/plugin_bindings` (`.c` / `.h`) | The board's bindings table's type, the record of which word is which veneer (`plugin_bind_probe`), and the painter and printer filled from the table (`plugin_painter_init()`, `plugin_printer_init()`). |
| `plugin/plugin_exec` | Copies a validated image into the board's reservation and calls its entry with the head of `plugin_board_bindings` (no base argument). |
| `plugin/plugin_run_core` | Above that loader, for an Armv7-M board staging in RAM (wio-lite-ai, f746g-disco): the lease checked before a load or unload and again at the branch into entry, the load's log lines and the staging-region check; the board keeps its MPU read-back and cache maintenance. |
| `plugin/plugin_mpu` | Armv8-M MPU verdict: is the plugin reservation executable (pure function). |
| `plugin/plugin_mpu_v7m` | The same question for an Armv7-M PMSA, answered by its own rules. |
| `plugin/plugin_info` | The plugin stack lines of `nn info`. |
| `plugin/plugin_claims` | The open model's container claims (a board-owned store, read in the board's critical section) and the `nn info` plugin lines around the stack lines (all three boards; grove-vision-ai-v2 since issue #174). The snapshot is checked against the `nn info` copy the board is handed (issue #199): a counter that is odd, that is not the copy's value, or a copy with no value are one answer, `plugin  : -- another nn command is using the model; ask again when it returns` -- the model line's words for busy, never "in progress", since a load that already finished is as much a reason. Host-tested (`shell/test/test_plugin_claims.c`, and `test_plugin_claims_order.c` under the shared load order). |
| `plugin/plugin_lease_api.h` | The plugin lease's API (one lock into the plugin); the board implements it. |
| `plugin/plugin_lease_miss` | What a refused no-wait lease acquire counts, over state the caller owns. |
| `plugin/plugin_paint_budget` | What one plugin `draw()` may spend, and the one place it is charged. |
| `plugin/plugin_paint_span` | What a painter primitive decides before its first store (clip, offset, charge). |
| `camera/cam_holders` | Who holds the camera stream open, and the start / stop / refusal each change implies (issue #142). |
| `camera/cam_step` | One step of the camera sink: may the producer enter it, and has it left (issue #142). |
| `camera/camera_svc.h` | The contract the shared `camera` command speaks; implemented by each board's adapter (issue #143). |
| `camera/camera_svc_adapter.h` | Static-inline helpers every camera adapter uses to fill a result and write a line. |
| `camera/cam_gen.h` | Stream generations: has THE stream a stop named ended (pure, shared by f746g-disco and wio-lite-ai). |
| `net/net_svc.h` | The contract the shared `net` command (`shell/cmds/cmd_net.c`) speaks; implemented by each board's adapter (issue #219; f746g-disco and wio-lite-ai). |
| `../shell/cmds/net_core` | Under `shell/cmds/`, not `svc/` (it calls NetX and ThreadX): the one implementation of the address read, the static address, DHCP and ping over NetX Duo, and of the reaction to the link -- the notice on the IP thread and the `net-link` worker (issue #220).  It owns no storage: the board defines `net_core_board_state` and `net_core_board_facts`, declared in `net_core.h`.  The adapters call it; first line `optional BOARD_HAS_NETX`. |
| `../shell/cmds/net_tty` | Under `shell/cmds/` as `net_core` is: the one telnet console over NetX Duo's TCP, resident from boot (issue #221) -- the transport's function table, the receive and transmit rings, the IAC handling, the `net-tty` listener thread, the two telnet lines of `net info` and the question "is this the telnet console".  It owns no storage: the board defines `net_tty_board_state` and `net_tty_board_facts`, declared in `net_tty.h`, and its transport with `NET_TTY_TRANSPORT_DEFINE()`; first line `optional BOARD_HAS_NETX`. |

## The asset store's shared parts (issue #192)

All three boards link `svc/blob/` (grove-vision-ai-v2 since issue #192,
wio-lite-ai since #193, f746g-disco since #194), and one `blob` command,
`shell/cmds/cmd_blob.c`, sits on it (next section).  This section and the next
are the reference for the files and the command; the slot layout, what a slot
table may and may not change, and how a stored blob is checked against the
build are told end to end in
[`asset/README.md`](../asset/README.md#the-blob-store-and-its-slot-tables).
The files:

| File | What |
|---|---|
| `blob/blob_state` | The header format (two 256 B pages, magic page written last), every decision a transfer makes, the name rules and the name lookup (`blob_resolve_name()`: exactly one VALID slot under a name is FOUND, two are DUPLICATE) |
| `blob/blob_map` | The rules a slot table has to satisfy over the board's interval, and the lookups over it |
| `blob/blob_write` | The coordinator of one `blob write`, over a const `struct blob_write_ops` (`blob_write.h` has the order and why) |
| `blob/blob_err.h` | The one set of results every board's store answers (OK 0, PARAM -1, IO -2, STATE -3, BUSY -4, EMPTY -5, CRC -6, MAP -7, CANCEL -8, DUPLICATE -9, FAULT -10); wio-lite-ai's numbers kept, grove-vision-ai-v2's busy / table / fault moved (issue #194).  BUSY (retry), FAULT (terminal, reset only) and STATE (never came up) stay apart.  Pinned by `shell/test/test_blob_err.c` |
| `blob/blob_svc.h` | The store contract the shared command calls, by fixed names (below) |

What a board provides, and how:

| The board provides | How the shared code takes it |
|---|---|
| Its slot table and the number of slots | Const data under fixed names, `blob_map_slots[]` and `blob_map_slots_count` (declared in `blob_map.h`), plus `blob_map_table()` / `blob_map_count()`; `blob_map_slot()` and `blob_map_index_of()` read the table by name, so no caller can hand them another; the board asserts the count against `BLOB_MAX_SLOTS` at build time. Every board: `src/blob_slots.c` |
| The writable interval and the erase unit | Arguments to `blob_map_check()` and the payload arithmetic, and `blob_svc_interval()`; nothing here states a boundary (grove-vision-ai-v2: the NOR seam's limits; wio-lite-ai and f746g-disco: `src/blob.h`) |
| The reads (`blob_stat`, `blob_read`, `blob_verify`, `blob_check_map`, `blob_slot_geometry`, `blob_svc_ready`) | The board's store side, `src/blob.c`; each read takes and gives back what its board needs for one call (grove-vision-ai-v2: a NOR lease; wio-lite-ai: nothing; f746g-disco: nothing, but it answers BUSY while the busy flag is held) |
| The write side: the reservation, the erases, the program, the read-back, the stat under the reservation, the staging buffer (`blob_svc_*`) | The board's `src/blob_writer.c`.  The shared command turns them into the coordinator's `struct blob_write_ops`.  The reservation is grove-vision-ai-v2's NOR writer reservation and the other two boards' busy flag; the erase is one `nor_write.c` transaction (grove-vision-ai-v2), one `nor_erase()` per 64 KB block (wio-lite-ai) or one QSPI sector erase per 64 KB (f746g-disco); the staging buffer is grove-vision-ai-v2's 64 KB `blob_stage_buf`, a 1 KB buffer on the other two |
| The console pieces (receive, may this console carry a raw transfer, the board's own sentence for a result, the RX ring's high-water) | `shell/cmds/blob_cmd_board.h`, defined in the board's `cmds/cmd_blob_board.c` |
| The words and numbers the command prints | `boards/<board>/svc/blob_svc_config.h` (the part's name, the address format, the erase estimate, the part's word for the 4 KB header unit -- `sector` on the two NORs, `4 KB subsector` on f746g-disco's QSPI part, whose sectors are 64 KB -- the sender, `blob read`'s limit, the listing's note; `BLOB_SVC_HAS_RX_LEVEL` where the console reports its ring) |

**Whether a program is read back is the board writer's** (`blob_write.h` step
6): grove-vision-ai-v2's `nor_write.c` re-reads every transaction and a
mismatch is a terminal fault; wio-lite-ai's `nor_write()` and f746g-disco's
`qspi_flash_write()` report a failed page (f746g-disco from the flag status
register) and do not re-read it.  On every board the payload is read back whole
against the CRC of the stream before the magic page goes down, and the body
page carries its own CRC, so a body that went down wrong reads `invalid`.

**The header page is a fact of the format, not of a part.**  `BLOB_HDR_PAGE`
(256 B, `blob_state.h`) is the size of each of the header's two pages wherever
the header is stored.  A board whose flash programs in pages checks at build
time that its program page is that size (grove-vision-ai-v2: `src/blob.c`,
against `NOR_PROGRAM_PAGE`; wio-lite-ai: `src/blob.c`, against `NOR_PAGE_SIZE`)
rather than the format following the board.

[!] **A slot table only grows by appending once it has shipped** -- the base
address is the blob's identity; why, and when each board's table was rebuilt,
is [`asset/README.md`](../asset/README.md#the-blob-store-and-its-slot-tables).

The translation units hold no mutable storage (the build-derived audit covers
them like the rest of `svc/`, and `shell/cmds/cmd_blob.c` with the shared
commands).  The sentences of `blob_name_verdict_name()` and `blob_lookup_name()`,
which a board's `nn` adapter copies into its explanation, are checked against
`NN_SVC_DETAIL_MAX` by enumeration in `shell/test/test_nn_detail_tables.c`, not
with `nn_detail.h`.  Host tests: `shell/test/test_blob_state.c`,
`test_blob_map.c` (with a table of its own under the fixed names),
`test_blob_write.c`, `test_blob_err.c` and `test_blob_cmd.c` (the shared
command over a stand-in store); a board's shipped table and its store and
writer are pinned by that board's own tests.

### The `blob` command is shared (issue #194)

One file for all three boards, registered when the board's
`svc/blob_svc_config.h` sets both `BLOB_SVC_HAS_STORE` and
`BLOB_SVC_HAS_RECEIVE` (a store that cannot be filled from the console would
offer `blob erase` with no way back; wio-lite-ai's KV-off build registers
nothing, and still compiles it for the audit).

```
blob list                    every slot: base, size, state, length, crc32, name
blob info <slot>             one slot in full, and why an invalid one is
blob read <slot> [off] [len] hexdump of the payload (the board's limit)
blob free                    the slots `blob write` accepts with no `blob erase` first
blob write <name> [slot]     erase the slot, receive a file over YMODEM, verify
blob verify <slot>           re-read the payload against its stored crc32
blob erase <slot>            retire a blob: erase its header sector only
nn model load <name>         load the blob stored under <name>
```

- **The name is the one typed, not the sender's filename** (which only goes to
  `dmesg`).  Printable ASCII with no space, not starting with `-` (issue
  #196), at most 64 characters; anything else is refused before anything is
  touched.
- **A name already stored goes back to its own slot** (`blob write <name>`
  needs no slot then).  Naming a different slot for it is refused (`that name
  is in another slot`), and a new name has to say which slot.
- **A slot holding a VALID blob under another name, or bytes this firmware
  cannot read (`invalid`), is refused** (`that slot holds something else`):
  `blob erase <slot>` first, on purpose.  An `empty` or `incomplete` slot is
  taken as it is.  The refusals happen before anything is erased; a file that
  turns out empty or larger than the slot is found only once the sender's first
  block arrives, after the erase.
- **A slot header the store cannot read while the slot is chosen ends the
  write before anything is erased**, and says which and why (issue #194):
  `blob: refused -- the header of slot N could not be read; nothing was
  erased`, then the store's answer as every other `blob` refusal words it
  (the board's sentence where it has one; `blob: <part> read/program/erase
  failed` for an I/O error); `dmesg` has the slot and the store's code.  Every other refusal before the choice keeps the plain
  `blob: refused (...)` line.
- **The erase stops between sectors on Ctrl+C**: nothing is sent, the amount
  erased is printed and logged (after a Ctrl+C the shell discards what the
  command prints, so `dmesg` has it), and `blob list` shows what is left.  A
  cancel before the first sector leaves the slot as it was; after it, the
  header is gone.  There is no local Ctrl+C during the receive (0x03 is data
  then); cancel from the sender.
- **The console is claimed after the erase**, so the erase keeps a live line
  editor for its cancel; a background `blob write ... &` is refused before
  anything is taken, and so is a console the board says cannot carry a raw
  transfer (the telnet console on wio-lite-ai and f746g-disco).
- **The magic page goes down only after the payload was read back and
  matched** (issue #194; it went first before).  A read-back that disagrees
  leaves the slot `incomplete` with its name and CRC readable (`blob: verify
  FAIL -- ... Slot N is left incomplete (not valid); `blob write <name> N`
  sends it again`); a magic program that reports failure says the slot may
  read valid, invalid or incomplete and points at `blob list`.
- A finished transfer prints the YMODEM counters, then `rx drops : N (clean)`
  -- with the ring's high-water on a board that reports it (f746g-disco) --
  then `stored :` and `verify : PASS (read back <crc>)`.
- `empty` means "no header", not "blank flash"; the slot states are described
  in [`asset/README.md`](../asset/README.md#the-blob-store-and-its-slot-tables).

What stays a board's: its words and limits (above), which operations hold its
busy flag or reservation, and how the part behaves (each board README's store
section).

## The `net` command and the telnet console are one implementation (issue #212)

**This section describes the shape issue #212 built** (its child issues
#215 to #222).  Issue #215 wrote it first, as the target, before any of the
code; each child issue updated the paragraphs it changed, and #222 rewrote it
into the shape that was built, read from the files.  A behaviour or a file
from before a child issue is named in brackets with that issue.  Only
f746g-disco and wio-lite-ai have a network (grove-vision-ai-v2 has none).
Since #219 the two share one `net` command (`shell/cmds/cmd_net.c` over
`svc/net/net_svc.h`), since #220 one implementation of the address, DHCP and
ping work and of the reaction to the link (`shell/cmds/net_core.c` / `.h`,
with one worker thread), and since #221 one telnet console, resident from
boot (`shell/cmds/net_tty.c` / `.h`).  Since #222 wio-lite-ai's bridge session
is in its link layer (`src/link_bridge.c`), out of the file that creates its
NetX objects.
Until #221 each board had its own console (f746g-disco
`port/netxduo/nx_shell.c`, wio-lite-ai `src/net_shell.c`), the same design
written twice and drifting apart.

The line is drawn at NetX Duo.  Above it is one implementation for both
boards; below it is the board.  Everything that makes Ethernet differ from
WiFi stays below NetX.

```
eth (f746g-disco)   net: shell/cmds/cmd_net.c, one definition   wifi (wio-lite-ai)
   |                   |                             |                  |
   |       shell/cmds/net_core.c (#220):  shell/cmds/net_tty.c (#221):  |
   |         address / DHCP / ping,        the telnet console,          |
   |       the worker (net-link) reacts -> resident from boot,          |
   |        to NetX's link notice          woken on a link fall         |
   |                   |                             |                  |
   |                   +---------- NetX Duo ---------+                  |
   |          IP / ARP / ICMP / DHCP client / TCP (lib/, both boards)   |
   |                        |                   |                       |
   v                        v                   v                       v
link: ether (f746g-disco) <-+                   +-> link: wifi (wio-lite-ai)
port/netxduo/nx_eth_driver.c                        src/nx_link_driver.c
+ port/eth/eth_link.c                               + src/link_bridge.c (the
  (the eth-link thread polls the PHY)                 bridge session's thread)
HAL_ETH + LAN8742A PHY                              rtl_link / erpc -> RTL8720DN
```

There are three kinds of edge and no others:

1. **A board's command to its own link layer, directly**: f746g-disco's `eth`
   (`cmds/cmd_eth.c`, done in #217: `net link` and the `mac:` / `phy:` /
   `link:` lines of `net info` moved there) and wio-lite-ai's `wifi`.
2. **`net` to NetX**: the command, the worker and the telnet console use
   NetX's application API (`nx_ip_*`, `nx_dhcp_*`, `nx_icmp_ping()`,
   `nx_tcp_*`) and nothing below it.
3. **NetX to each link layer and back, through the driver's entries**: NetX
   hands frames and `NX_LINK_*` requests to the driver function it was created
   with; the driver hands received frames and link up / down events back.

[!] **No edge goes up from a link layer.**  When #215 wrote this there were
three; #218, #220 and #221 removed them all:

- f746g-disco: the eth-link thread ended each PHY poll by calling a hook the
  address code had registered, which took the address lock and started the
  DHCP client.  #220 removed the hook: the eth-link thread now only reports a
  change to NetX (`_nx_ip_driver_link_status_event()`) and kicks its deferred
  processing, and the shared worker reacts.
- wio-lite-ai: the resident thread of the bridge session (then in
  `src/nx_net.c`, beside the board's NetX objects) used to call
  `nx_dhcp_stop()` on a stop and on a revoked link.  #220 removed both calls:
  each of those paths already closes the driver's gate with
  `nx_link_driver_set_link(0)`, which raises the link event, and the worker
  stops the client.  The thread no longer takes the address lock.  #222 then
  moved it into the link layer, `src/link_bridge.c` (a move only: what it
  does and what it prints did not change).
- wio-lite-ai: the same thread used to stop the telnet console
  synchronously before it took the bridge down, and went FAILED if the
  console could not be shut.  #218 made it close the driver's gate
  (`nx_link_driver_set_link(0)`) first and only ask the console to stop,
  without waiting.  The gate holds for every sender, not just the console:
  the DATA channel keeps a send generation that the gate's closing moves, and
  a frame decided before that move is refused when it is queued, however long
  its sender was preempted.  #221 removed the rest -- the request to stop and
  the bring-up's wait for the console's socket (`nxn_arm()`): the console is
  resident, and a session it has is ended by its own listener once the link
  worker passes the fall on.

### The four rules of the layering (issue #212)

- [!] **The link layer calls only NetX's driver-side entries.**  The link
  layer is the NetX driver, f746g-disco's PHY-polling eth-link thread and
  wio-lite-ai's resident thread of the bridge session (`src/link_bridge.c`
  since #222); the entries are the frame handoff and
  `_nx_ip_driver_link_status_event()`.  It calls neither the application API
  (`nx_dhcp_*`, `nx_ip_address_*`) nor `net` (true on both boards since
  #220), and since #222 no wio-lite-ai link-layer file includes a header of
  `net` or of the file that creates the NetX objects (`net_core.h`,
  `net_tty.h`, `net_svc.h`, `nx_net.h`).
- [!] **`net` learns that the link went up or down from NetX's notice, and its
  own worker reacts.**  NetX calls the notice (registered with
  `nx_ip_link_status_change_notify_set()` by `net_core_init()`) on its IP
  thread with `nx_ip_protection` held, so the notice only records; touching
  DHCP there is a deadlock (issue #13: the DHCP thread holds its mutex and
  waits for `nx_ip_protection`).  The worker reacts under the address lock: a
  link up starts DHCP when DHCP is the source, a link down stops it and marks
  a DHCP address stale.  See [The link notice and the
  worker](#the-link-notice-and-the-worker-issue-220).
- [!] **The telnet console is resident from boot on both boards, with no start
  or stop** (like sshd).  It listens with no address, the listen survives a
  link down, and only an established connection is closed then.  Its state is
  two lines of `net info`.  Built in #221; see [The telnet
  console](#the-telnet-console-issue-221).
- [!] **The shared translation units hold no mutable storage.**  The `NX_IP`,
  the `NX_DHCP`, the state, the locks, the worker's stack and the telnet
  console's rings, socket and stack are the board's, placed with the board's
  section attributes and injected.  Injection is all the board provides: no
  predicate that asks the board's state, and no callback from below to above.

What the driver-side entries are today, from the two drivers: the receive
handoff (`_nx_ip_packet_receive()` and the ARP / RARP deferred receives on
f746g-disco, which already runs on the IP thread; the deferred variants of all
three on wio-lite-ai, whose receive runs on the link's service thread), the
link event (`_nx_ip_driver_link_status_event()`, both), the deferred-processing
kick (`_nx_ip_driver_deferred_processing()`, f746g-disco, every PHY poll as a
packet-pool starvation watchdog), the packet calls a driver makes on its own
frames (`_nx_packet_allocate()`, `_nx_packet_release()`,
`_nx_packet_transmit_release()`, `_nx_ip_header_add()`), and the calls that
describe the interface to NetX (`nx_ip_interface_mtu_set()`,
`nx_ip_interface_physical_address_set()`,
`nx_ip_interface_address_mapping_configure()`) inside `NX_LINK_INITIALIZE`.
wio-lite-ai also sets the interface's MAC with
`nx_ip_interface_physical_address_set()` from `nx_link_driver_set_mac()` when
the bridge comes up, because the MAC is the module's.  These describe the
interface and count as the driver's; the address the interface carries does
not.

### The `net` command's meaning on the two boards (issue #212)

One row per meaning that differed between the two boards when #215 wrote
this.  Every row now reads the same on both boards; that follows from there
being one implementation, not from each board being told.  N1 to N4 are the
command's own (#219), the address, DHCP and link rows are the shared address
work's (#220), and N6 is the shared telnet console's (#221).  What a board did
before is in brackets, with the issue that changed it.

| Meaning | Both boards |
|---|---|
| N1 `net ping` with a bad count | refused (`bad count`, 1..100), nothing sent (#219; before: f746g-disco silently sent 4 probes) |
| N2 `net ping` with no reply at all | exit 1 (#219; before: exit 0 on f746g-disco) |
| N3 `net dhcp`'s wait | 30 s, then an error and exit 1 (#219; before: on f746g-disco 10 s, a warning, exit 0) |
| N4 how the address is printed | two lines, `ip:` and `gw:`, STALE after `gw:` (#219; before: one line on f746g-disco, which prints STALE since #220) |
| No address and nothing has made DHCP the source | `ip:    none (unset)` (#220; before: wio-lite-ai printed `none (dhcp, no lease)` even when DHCP was never started).  On f746g-disco DHCP is the boot source, so it reads `none (dhcp, no lease)` until a lease, and `none (unset)` only when the DHCP client could not be created at boot |
| `net info` with no IP stack | answers: `ip: none (<the board's reason>)`, then what the board's lines can still say, exit 0 (#219; before: f746g-disco refused it, #217) |
| N6 the telnet console | resident from boot, nothing to operate, its state two lines of `net info` (#221; before: each board's own code -- on f746g-disco one line, on wio-lite-ai `net shell start` / `stop` / `status`, armed after a successful `net ip` / `net dhcp` and at boot when the KV store said so) |
| N7 where link operations live | the board's link command, `eth` on f746g-disco and `wifi` on wio-lite-ai; `net` has no link operations (#217; before: f746g-disco's `net link` and the PHY line of `net info`) |
| `net dhcp` while a lease is held | a renew is sent, the lease stays; a stale lease is acquired again (#220; before: wio-lite-ai stopped the client, set the address to 0 and acquired again) |
| `net dhcp` with the link down | refused, `net: <the board's link sentence>`, nothing changes (#220; before: f746g-disco started the client anyway) |
| `net ping` with the link down | refused with the link sentence (#220; before: on f746g-disco whatever NetX answered) |
| The DHCP calls into NetX | the interface-scoped ones (`nx_dhcp_interface_*`) (#220; before: wio-lite-ai used the global ones and discarded their result) |
| `net ip` that fails part way | the previous address, gateway and DHCP state are put back (#220; before: wio-lite-ai did not put them back) |
| The address after the link went down (lease stale) | STALE (#220; before: f746g-disco marked nothing) |
| DHCP again when the link comes back | yes, from the shared worker (#220; before: f746g-disco started it from the eth-link thread, wio-lite-ai not at all because of the lock order) |
| DHCP across a link down | stopped by the worker, the address stays and is marked STALE (#220; before: f746g-disco kept the client running through a cable pull; wio-lite-ai stopped it only when the link was taken away by `wifi on` / `off` / `reset`, and a lost association only marked STALE) |

How the last rows work is
[The link notice and the worker](#the-link-notice-and-the-worker-issue-220)
below.

What may differ by board is declared as a property in the board's
`boards/<board>/svc/net_svc_config.h` (issue #219), and the shared command
prints or registers under the property only, as `camera` does with
`CAMERA_SVC_BOARD_SUBCMDS`.  Every macro must be defined; a missing one is an
`#error` in `cmd_net.c`.  The link layer's commands (`eth`, `wifi`) are
outside `net` and are not declared there.

| Property (`svc/net_svc_config.h`) | f746g-disco | wio-lite-ai |
|---|---|---|
| `NET_SVC_HELP` (the `net` line of `help`) | NetX Duo over the on-board Ethernet, pointing at `eth` | IPv4 over the RTL8720 |
| `NET_SVC_BOARD_DECLS` / `NET_SVC_BOARD_SUBCMDS` (`net`'s own subcommands above NetX, appended to the shared table) | `mjpeg` | none (`shell`, the telnet console's controls, was removed by #221; `net echo` by #216) |

Two more things differ and are not properties, because they are the board's
data rather than a switch:

| Not a property | f746g-disco | wio-lite-ai |
|---|---|---|
| The lines of `net info` after the address: the board writes them through the contract's writer (`net_svc_board_lines()`), sampling every value before the first line and stopping at the first failed write | the telnet console's two lines first (the shared `net_tty_write_lines()`, #221), then the worker's last failure while one is recorded (#220), the MJPEG state while it runs, the Ethernet driver's, TCP's (`nx_tcp_info_get()`) and the packet pool's counters | the telnet console's two lines first (the same shared call, #221), then the host-stack section: its state and why, then the module's DATA counters accumulated across bridge re-arms and the link driver's, then the worker's last failure while one is recorded (#220) |
| Where the NetX objects are placed | the packet pool in `.sdram.eth` (SDRAM, non-cacheable through the MPU), the control blocks and the telnet console's buffers in internal SRAM | the packet pool, the IP thread's stack, the telnet console's TX ring and its listener's stack in DTCM (only the CPU touches a frame) |

Until #219 wio-lite-ai's `net info` also asked the module, over eRPC, whether it
was associated and what its MAC was (the `link:` and `mac:` lines, inside one
`rtl_link_begin()` session).  That is a link layer question asked from `net`,
and `wifi status` prints both, so #219 dropped the two lines from `net info` --
the same move as f746g-disco's PHY line going to `eth` (#217).  `net info` now
answers with the module powered off or eRPC held by another command.  (The
`mac:` line inside the host-stack section stays: it is the MAC NetX was given,
read without eRPC.)

### What the one implementation does (issues #219, #220, #221)

The `net` layer is five parts since #221:

| Part | What it holds |
|---|---|
| `shell/cmds/cmd_net.c` (#219) | The command: the grammar, the usage, the refusals, the waits, the ping loop and its totals, and every line `net` prints |
| `svc/net/net_svc.h` (#219) | The contract the command speaks, freestanding (no HAL, no board header, no `tx_api.h`): the results, the address, the writer, the calls |
| each board's adapter (`net_svc_f746.c`, `net_svc_wio.c`) | Its own state checked (is the IP stack there, is it changing -- the only source of BUSY), then the shared operation called; and the board's lines of `net info` |
| `shell/cmds/net_core.c` / `.h` (#220) | The one implementation over NetX Duo: the address read, the static address, DHCP, ping, the link notice and the worker |
| `shell/cmds/net_tty.c` / `.h` (#221) | The one telnet console over NetX Duo's TCP: the transport, the rings, the listener thread, its two lines of `net info` and the question "is this the telnet console" ([below](#the-telnet-console-issue-221)) |

`net_core.c` and `net_tty.c` use NetX's and ThreadX's APIs, which `svc/`
cannot include, so they live under `shell/cmds/` on the precedent of FileX's
`shell/cmds/fs_cmd_core.c`; the NetX types stay in `net_core.h` and
`net_tty.h`, and `net_svc.h` stays freestanding.  The adapter takes no
`struct cli_instance`: printing, waiting and Ctrl+C are the command's.

What the command and the contract fix since #219, on both boards:

- **A result is not folded.**  There are eight statuses -- OK, RENEWED
  (`net dhcp` kept a held lease and sent a renew; a success), DOWN (the IP
  stack does not have the network), BUSY (it is coming up or going down),
  FAILED (only a reset of the link leaves this state), LINK (the link is
  down), REFUSED (NetX said no), TIMEOUT (a probe got no reply) -- and each
  calls for something different, so the command words them apart.  A glue
  value the adapter does not know is read as REFUSED (every failure exits 1).
  REFUSED may leave part of the change made: a refused `net ip` puts the
  address, the gateway and the source back, but a refused `net dhcp` puts
  nothing back (below), so no line says "restored".
- **The reason is the board's sentence**, carried in the result as a pointer to
  a string literal (no copy, no storage): `net: <reason>` for a failed
  operation, `ip:    none (<reason>)` in `net info`.
- **The address comes from one call** (address, mask, gateway, DHCP or static,
  and the stale mark), so the `ip:` and `gw:` lines are never two separate
  questions.  [!] **The contract promises one call, not one moment**: since
  #220 both boards read every field in one hold of the address lock
  (`net_core_addr_get()`), but the DHCP client's own mutex is not taken.  When
  a lease arrives, the DHCP thread writes the address and the gateway in two
  separate NetX calls under that mutex, so for a few microseconds a reader can
  pair the new address with the old gateway.  Closing that would mean taking
  NetX's internal member `nx_dhcp_mutex`, which no public call exposes; the
  contract keeps "one call" instead (decided in #220).
- **The board's lines of `net info` go through a writer**: one whole line per
  write, every value sampled first with every lock released, and a failed
  write ends the section (the lines already written stay).
- **Every wait is measured in ThreadX ticks as the difference from its start**
  (wrap-safe), never by counting sleeps, because the adapter's calls and the
  output take time between them; `cmd_net.c` asserts at build time that a
  tick is 1 ms.  A deadline only stops the next call: a glue call already
  running (a probe's 1 s, a DHCP start) is not cut short.
- **BUSY changed nothing.**  An adapter returns it only from its own check of
  the state, before any glue call, so the command may call the same operation
  again.  `net ip`, `net dhcp` and the first probe of `net ping` call again
  every 100 ms under one deadline for the whole wait (20 s; a retry does not
  restart it); when it expires the command prints the board's reason and exits
  1, and Ctrl+C ends the wait with nothing changed.  `net info` does not wait:
  BUSY is its `ip: none (...)` line.  f746g-disco never returns BUSY (its IP
  stack is created once at boot and never taken down).
- **Every argument is parsed before anything is called**, and `net info` and
  `net dhcp` refuse extra arguments.
- **`net ping <a.b.c.d> [count]`** takes a count of 1 to 100 (4 when it is
  left out; anything else is refused with nothing sent), sends one probe a
  second, ends on Ctrl+C between probes, and exits 1 when no probe was
  answered.  The `PING ...` heading is printed once the first probe got an
  answer, a timeout or a refusal; a first probe that finds the stack DOWN,
  FAILED or the link down prints only the reason.  A later one that does stops
  the run with the reason, the totals so far, and exit 1.
- **`net dhcp`'s wait is the command's**: up to 30 s for an address, then
  `net: no DHCP lease after 30 s` and exit 1; Ctrl+C ends the wait but not the
  DHCP client.  The address is then printed as `ip:` and `gw:`, and after a
  successful `net ip` too.
- **Nothing is called because an address arrived.**  Until #221 the command
  called `net_svc_addressed()` after the operator's `net ip` or `net dhcp`
  gave the interface an address, and wio-lite-ai armed its telnet console
  there.  The console is resident since #221, which removed the call from the
  contract, the command and both adapters.

What the one implementation in `shell/cmds/net_core.c` does, on both boards
since #220:

- **A static address is a transaction.**  `net ip <a.b.c.d/mask> [gw]`
  refuses a gateway off the new subnet before it touches anything; otherwise
  it stops DHCP, sets the address, then the gateway (0 clears it: NetX refuses
  0.0.0.0 as a gateway).  A failure puts back the previous address, gateway
  and source, and the DHCP client when it was running: reinitialised and
  started again from scratch (the lease itself is not restored), or left
  "DHCP, not running" when that fails, which a later `net dhcp` or link up
  recovers.  A failure while putting things back is not reported.  It is
  allowed with the link down.
- **`net dhcp` is refused with the link down** (LINK, in the board's words,
  nothing changes) and **without a DHCP client** (REFUSED: the board's
  `nx_dhcp_create()` failed at boot).  Otherwise one of three: not running --
  drop the address, then start (OK); a lease held and not stale -- force a
  renew and keep the lease, the address never disappears (RENEWED); running
  without a lease, or with a stale one -- stop, drop the address, start again
  (OK).  [!] **A stale lease is never renewed**: NetX's force renew talks only
  to the old server and returns success at once, so a "renewed" stale lease
  would keep an address nobody confirms until its rebind time.  The address is
  dropped before a start so that "the interface has an address" means "a lease
  arrived".  The source becomes DHCP and the stale mark clears once the
  address is dropped.  [!] **A refused `net dhcp` puts nothing back**: a step
  that failed part way can leave the address dropped and the client stopped.
  The wait after it is the command's (above).
- **Every start is preceded by a reinitialise** (NetX documents that a stopped
  client must be reinitialised before it starts again), and **every DHCP call
  names the interface** (`nx_dhcp_interface_start()`, `_stop()`,
  `_reinitialize()`, `_force_renew()`).  The global versions loop over the
  records, discard each interface's status and return success, so their
  result cannot be checked, and for a renew "nothing was bound" would read as
  "renew sent".
- **A step is recorded only when NetX performed it.**  A stop is recorded only
  when NetX stopped the client or answered that it was not started; a failed
  step leaves the state where the steps that succeeded put it.
- **`net ping`** is LINK with the link down; otherwise one `nx_icmp_ping()`.
  It takes no lock, so a probe's wait of up to a second never holds up the
  worker.
- **`net ip` and `net dhcp` clear the stale mark** and the worker's last
  failure when they succeed.
- [!] **The lock order is the address lock, then NetX's locks** (the DHCP
  client's mutex, taken by NetX inside `nx_dhcp_*`, then `nx_ip_protection`,
  then the driver's own).  `net_core.c` takes only the address lock, and never
  while it holds anything else.  Only threads that hold no other lock take it:
  the commands, the worker, and on wio-lite-ai the KV boot thread.  The telnet
  console never takes it (since #221; wio-lite-ai's own listener took it to
  print its listening address).  Nothing that runs with a NetX mutex or the board's link lock
  held takes it (f746g-disco's `eth_lock`, wio-lite-ai's rtl_link coarse
  mutex; the link notice runs with `nx_ip_protection` held and takes no lock
  at all), so a `net dhcp` and a link going down are ordered by the address
  lock.

### The telnet console (issue #221)

One implementation for both boards, `shell/cmds/net_tty.c` / `.h`, over NetX
Duo's TCP API and the shell's transport abstraction
(`struct cli_transport_api`).  It replaced f746g-disco's
`port/netxduo/nx_shell.c` and wio-lite-ai's `src/net_shell.c` (with its
`net shell start` / `stop` / `status`, the auto-arm and the KV key
`net.shell.autoarm`, which nothing reads any more).  The comment at the top of
`net_tty.h` is the exact description; this is the shape.

What the shared file holds:

- **The transport's function table** (`net_tty_api`, const).  Each board
  defines its transport object with `NET_TTY_TRANSPORT_DEFINE(<name>)` and
  binds its own `CLI_INSTANCE_DEFINE` to it, with its own prompt (`net>`,
  `wio-net>`).
- **The receive ring**, filled by the IP thread's receive callback, which
  reads every packet to its end in pieces of the extraction buffer and strips
  telnet commands (IAC; IAC IAC is one 0xFF).  A byte that finds the ring
  full is dropped and counted (`rx drop`).  The ring has one writer, the IP
  thread, and one reader, the console's thread.
- **The transmit ring**, filled by the console's writers (any thread, inside
  an interrupt-masked section that also reads the write gate; a 0xFF goes in
  as IAC IAC, both halves or neither) and drained only by the listener, which
  builds the packets and is the only sender.  A flush at the end of a unit of
  output wakes it.  A refused send leaves the bytes in the ring and is
  counted by cause -- the transmit queue full (`q`, the designed
  back-pressure), the peer's window (`w`), the pool (`buf`, which includes the
  8 packets the drain leaves to the receive path), anything else (`err`) --
  and is tried again after 50 ms, or at once when an ACK frees the queue (the
  queue-depth callback).
- **The listener thread**, `net-tty`, priority 14: below the IP threads and
  the DHCP thread (13), above the link worker (15) and the consoles (16), the
  last two asserted in `net_tty.c`.
- **The character-mode negotiation** (IAC WILL ECHO, IAC WILL
  SUPPRESS-GO-AHEAD), put into the transmit ring whole when a session begins.
- **The two lines of `net info`** (`net_tty_write_lines()`, below), which each
  board's `net_svc_board_lines()` calls first.
- **The question "is this console the telnet one"** (`net_tty_is_console()`;
  a background job counts as the console it was launched from) and the
  refusal built on it (`net_tty_guard()`, below).

What the board injects, by fixed names declared in `net_tty.h` (the same shape
as `net_core.h`: NetX's socket callbacks take no context argument):

- `struct net_tty_state net_tty_board_state`, without an initializer: the
  rings' control blocks, the write gate and the session marks, the socket, the
  thread and its event flags, the port, the listener's state and the
  counters.
- `const struct net_tty_facts net_tty_board_facts`: the `NX_IP` and the packet
  pool; where the receive ring, the extraction buffer, the transmit ring and
  the listener's stack are, and their sizes; the socket's transmit queue depth
  and retransmit settings; and the name of the other console a refusal points
  to.
- The transport object (`NET_TTY_TRANSPORT_DEFINE()`), outside the state:
  `cli_init()` writes its instance into it, and an initializer on the state
  would have made all of it initialised data.
- The port, once (below), and which of its commands ask the question.

The rules:

- [!] **Resident from boot, with no start and no stop.**  `net_tty_init()`
  runs from `tx_application_define()` after the board created its NetX
  objects and `net_core_init()` and the instance bound to the transport was
  initialised; it only creates the event flags and the thread (NetX refuses a
  socket create or a listen from the init context).  The thread waits for the
  port, which the board hands over once with `net_tty_port_set()` (the first
  call wins, a later one is ignored), then creates the socket, registers its
  callbacks and listens.  It listens with no address and stays listening
  across link falls; the socket is never deleted.  A create, a callback
  registration or a listen that fails leaves "NOT listening" with the step and
  NetX's answer in `net info`, and is not retried.
- [!] **Accept is armed once per connection.**  The listener calls
  `nx_tcp_server_socket_accept()` once and waits in it for ever.  A handshake
  that fails (NX_NOT_CONNECTED: NetX leaves the socket listening while still
  bound) goes through unaccept and relisten before the next arm; a relisten
  that answers "a connection is pending" is success.  Arming the same socket
  again instead would leak a packet per call and stop accepting (issue #202,
  owhinata/wio-lite-ai#23).  Any other accept failure, and an unaccept or a
  relisten that fails, stops the listener until a reset ("NOT listening" with
  the step) instead of arming again.
- **A connection is published only after a check**, in one interrupt-masked
  section right after accept returns: the socket is ESTABLISHED, the
  establish callback ran, the peer has not gone, and the link's fall count has
  not moved since the establish callback.  Otherwise the connection is ended
  like a session.  To publish, the listener empties the transmit ring, marks
  the session live and tells the instance, whose session begin (on the
  instance's thread, between commands) empties the receive ring, opens the
  write gate and stores the negotiation in one masked section.
- [!] **Every way a session ends is one routine** -- the peer's FIN / RST, the
  link fell, the socket left ESTABLISHED: close the gate, throw away the
  transmit ring, raise the cancel mark and the receive discard, tell the
  instance it has input, disconnect (bounded, 1 s), unaccept, relisten.  The
  cancel mark makes the transport's next read return one 0x03 ahead of the
  receive ring, so a running command is cancelled even with the ring full;
  the receive discard makes every read consume and drop what the ring holds
  until the instance begins the next session, so a gone connection's bytes
  never reach the idle editor.  A cancel mark still set when the next session
  begins is cleared there: session begin runs between commands, so nothing of
  the old session is left to cancel.  Before #221 f746g-disco did not cancel
  on a peer close (the rule of issue #218).
- **A link fall ends a session; the count decides and the event only wakes.**
  `net_core.c`'s notice counts every fall (`net_core_board_state.link_falls`)
  in the masked section that records the level, on the IP thread; the
  establish callback, on the same thread, copies the count when the connection
  becomes established, so whether a session began before or after a fall is a
  comparison of two numbers taken in the IP thread's order.  After a pass that
  handled a fall, with the address lock released, the link worker calls
  `net_tty_link_fell()`, which only sets the listener's event (no NetX call,
  no lock, no wait, nothing counted).  The listener compares the counts before
  every wait of a session, the first one included, and clears a wake only in
  the wait itself, so a fall between the publish check and the publish, or
  one whose wake came during a drain, is still seen.  A fall while nobody is
  connected changes nothing: the listener keeps listening, and a session
  established after it carries the new count.
- [!] **The telnet code takes no lock of its own and never the address
  lock** -- only ThreadX interrupt-masked sections (`TX_DISABLE` /
  `TX_RESTORE`) and what NetX takes inside its calls -- and calls nothing of
  the link layer.  The listener owns the socket and is the only sender; the
  IP thread runs the callbacks (receive, disconnect, establish, queue depth),
  which only move bytes into the receive ring and set marks and flags; the
  console threads only write into the transmit ring.
- **The no-progress deadline** is 5 s for both boards
  (`NET_TTY_TX_DEADLINE`, published as the transport's `tx_timeout`), and each
  board asserts at build time that it exceeds its socket's first retransmit:
  a lost segment is recovered only after that retransmit, and a shorter
  deadline drops the rest of a command's output (owhinata/wio-lite-ai#48).
  The receive window (2,048 B), the segment size (1,400 B), the segments per
  drain pass (8) and the priority are shared too.

The refusal.  A board command that would destroy the session it runs on asks
`net_tty_guard(sh, "<what>")` first.  On the telnet console it prints
`<what>: refused on the telnet console -- it would destroy this session. Run
it from the <other> console.` and the command exits 1; anywhere else it
returns 0.  Which commands ask is the board's:

| Board | Refused on the telnet console | `<other>` |
|---|---|---|
| f746g-disco | the YMODEM of `blob write` (the other YMODEM paths are issue #197) | `VCP` |
| wio-lite-ai | `wifi connect` / `disconnect` / `on` / `off` / `reset`, `mlperf`, and every YMODEM transfer (`xfer`, `blob write`, `wifi flash imgload` / `backup`) | `USB CDC` |

The two lines of `net info`, every value sampled in one masked section before
the first is written, and a failed write ends them:

```
telnet: :<port> listening, <client attached|no client>, <n> sessions, last ended: <how>
telnet: rx drop <n>, tx <n> seg / <n> B, ring peak <n> B, refused q<n> w<n> buf<n> err<n>, dropped <n> B
```

- The first line is instead `telnet: NOT listening on :<port> -- <step>
  failed (0x<nn>); <n> sessions, last ended: <how>` once the listener stopped
  (`<step>` is `socket create`, `callback registration`, `listen`, `accept`,
  `unaccept` or `relisten`), `telnet: not listening yet (no port)` before the
  port was handed over, and `telnet: not available (init failed)` -- with no
  second line -- when `net_tty_init()` failed.
- `<how>` is `none yet`, `the peer closed`, `the link fell` or `it left
  ESTABLISHED`.
- `rx drop` is bytes lost to a full receive ring.  `tx` is segments and
  bytes sent (telnet-encoded), `ring peak` the transmit ring's high-water,
  `refused` the refusals by cause (above), and `dropped` the bytes a session
  had queued and lost at its end; output written while no session is open is
  swallowed and not counted.  All count from boot and are never reset.  They
  are updated outside a masked section, so a count can be lost (statistics
  only, accepted in #221).
- [!] **Nothing of the console goes to `dmesg`** -- not a connect, a
  disconnect or a failure to listen: the shared file cannot write a board's
  log (as for the link worker, #220).  These lines are where to look.

The NetX configuration it needs: `NX_ENABLE_EXTENDED_NOTIFY_SUPPORT` (the
establish callback; without it `nx_api.h` defines
`NX_DISABLE_EXTENDED_NOTIFY_SUPPORT` and the registration answers
NX_NOT_SUPPORTED, 0x4b) and `NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY` (the
drain's wake after an ACK).  `net_tty.c` stops the build with `#error` when
either is missing; both boards set both in `port/netxduo/nx_user.h`, the first
since #221.

Not part of it: the receive back-pressure and the BINARY negotiation of #197
(and f746g-disco's other YMODEM paths), a second session at a time (listen
backlog 1), and what a `net ip` that moves the address under a session does
(the session lasts until TCP's retransmits give up, as before).  The host test
`shell/test/test_net_tty.c` builds the real file over a fake TCP socket that
follows NetX's state rules, including the packet a second accept on a failed
handshake leaks.

### The link notice and the worker (issue #220)

The reaction to the link is one pair in `shell/cmds/net_core.c`, the same on
both boards; neither board registers a link-status callback of its own.

- **The notice** (`nx_ip_link_status_change_notify_set()`, registered by
  `net_core_init()`) runs on the IP thread with `nx_ip_protection` held, for
  every change the driver reports, including an up that repeats (f746g-disco
  reports speed and duplex changes that way).  It writes NetX's own interface
  link flag (NetX's deferred processing does not), then records the level and,
  on a down, a "the link fell" mark -- the two together, in one ThreadX
  interrupt-masked section (`TX_DISABLE` / `TX_RESTORE`) -- and after the
  section sets the worker's event flag.  [!] **It calls no NetX function,
  takes no lock and never waits**, so the IP thread never heads for the DHCP
  client's mutex (the cycle of issue #13 has no second half).
- **The hand-over.**  The level and the fall mark are one pair, read and
  written only inside such a masked section.  The event flag only wakes; what
  happened is in the pair.  The worker copies the pair and clears the mark in
  one section; a notice after the copy leaves a new mark and a new wake, so the
  next pass picks it up, and an extra wake is a pass with nothing to do.  The
  commands read the level the same way and never touch the mark: handling a
  fall is the worker's alone.
- **The worker** (`net-link`, one ThreadX thread, its stack the board's)
  reacts under the address lock from the source and the client's state of
  that moment, so a `net ip` that got in first leaves nothing to start.  A fall
  stops a running client (the address stays) and marks a DHCP address stale;
  the link up with DHCP the source and the client not running reinitialises,
  drops the address and starts it.  Its priority is 15: above the consoles
  (16, asserted in `net_core.c`), below both IP threads (f746g-disco 12,
  wio-lite-ai 11, asserted in each board's glue) and the DHCP thread (13), so
  the notice never hands it the CPU while the IP thread holds its mutex.
- **Folding.**  Up, down, up within one pass stops and restarts the client;
  down, up, down only stops it; a repeated up does nothing.  NetX itself keeps
  one "changed" mark and asks the driver for the level when it processes it,
  so a fall shorter than the IP thread's latency is not seen at all, as for
  everything above NetX (accepted in #220).
- **A command after a bounce.**  If a fall and a rise both happen before the
  worker runs and a `net dhcp` starts a client in between, the worker's pass
  stops that client for the pending fall and, the link being up, starts it
  again: the end state is "up, DHCP running" and the operator's wait goes on,
  having lost one acquisition.  The commands do not take the fall over, which
  would leave a client running across the outage.
- **A failed step is not retried.**  The state stays where the steps that
  succeeded put it, and recovery waits for the next notice or the operator's
  `net dhcp` / `net ip`.  The failure is kept in the board's state as the last
  failure (which step, NetX's answer, the link level the pass saw), and since
  the shared file cannot write a board's log, the board prints it as one line
  of `net info` while one is recorded:
  `dhcp: the link worker's <stop|reinitialise|start> failed (0x<nn>) with the
  link <up|down>`.  A later pass whose steps all succeed, or a successful
  `net dhcp` / `net ip`, clears it.
- **Start-up.**  `net_core_init()` runs from `tx_application_define()` right
  after the board created its NetX objects, before the scheduler and so
  before the first notice.  It creates the address lock, the event flags and
  the worker, registers the notice, and sets the ready mark last, only when
  all of that succeeded; until then every operation is DOWN in the board's
  words.  It only creates objects: it never waits and does not depend on
  `HAL_GetTick`.  The boot source is the board's fact: DHCP on f746g-disco
  (fixed; it has no KV store until #214), none on wio-lite-ai until its KV
  boot says otherwise.  wio-lite-ai's KV `net.mode=dhcp` calls `net_core_dhcp_source()`,
  which only makes DHCP the source and wakes the worker; unlike `net dhcp` it
  is not refused with the link down, because right after the bridge comes up
  the recorded level may not have caught up yet and the order must not rest
  on thread priorities.
- **A fall ends the telnet session** (#221): the notice also counts the fall
  (`link_falls`) in the same masked section, and after a pass that handled a
  fall the worker calls `net_tty_link_fell()` with no lock held, which only
  wakes the console's listener.  The listener needs nothing when the worker
  brings the address back: it never stopped listening.  See [The telnet
  console](#the-telnet-console-issue-221).

### The board's side of the contract (issues #219 to #221)

What the board hands the shared code is storage and facts, never behaviour.
`net_core.c` reaches the board's two objects by fixed name, declared `extern`
in `net_core.h` and resolved at link time -- the same shape as
`svc/nn/nn_dispatch_state.h` -- because a pointer the shared file remembered
would be mutable storage in it, and NetX's link-status callback takes no
context argument to carry one.  The board defines the state without an
initializer and calls `net_core_init()` with its `nx_dhcp_create()` result.
`net_tty.c` reaches `net_tty_board_state` and `net_tty_board_facts` the same
way (#221):


| The board provides | Where |
|---|---|
| The `NX_IP`, the `NX_PACKET_POOL` and its memory, the IP thread's stack, the ARP cache | f746g-disco `port/netxduo/nx_glue.c`; wio-lite-ai `src/nx_net.c` |
| The `NX_DHCP` | the same files |
| `net_core.c`'s mutable state, `struct net_core_state net_core_board_state` (#220): the address lock, the worker's thread and event flags, the ready mark, whether the DHCP client was created, the recorded link level and fall mark, the source, whether the client runs, the stale mark and the worker's last failure | the same files, in plain `.bss` (CPU-only) |
| `net_core.c`'s constant facts, `const struct net_core_facts net_core_board_facts` (#220): the `NX_IP`, the `NX_DHCP`, the interface, the boot source, the board's sentences for LINK and for DOWN before init, and the worker's stack | the same files; the worker's stack is 1 KB on both, in DTCM on wio-lite-ai (`net_worker_stack`, a name `cmake/check_dtcm_residency.py` requires) |
| `net_tty.c`'s mutable state, `struct net_tty_state net_tty_board_state`, and its constant facts, `const struct net_tty_facts net_tty_board_facts` (#221): the `NX_IP`, the pool, the receive ring (512 B), the extraction buffer (1,500 B), the transmit ring (4,096 B), the listener's stack (2,560 B), the socket's transmit queue depth and retransmit settings, the other console's name | f746g-disco `port/netxduo/nx_glue.c`, all in internal SRAM; wio-lite-ai `src/nx_net.c`, the transmit ring and the stack in DTCM (`net_tty_tx_buf`, `net_tty_stack`, names `cmake/check_dtcm_residency.py` requires), the rest in AXI-SRAM `.bss`.  Until #221 these were `port/netxduo/nx_shell.c` and `src/net_shell.c` |
| The telnet transport object (`NET_TTY_TRANSPORT_DEFINE()`) and the `CLI_INSTANCE_DEFINE` bound to it | `src/main.c` on each board |
| The telnet console's port, handed over once (`net_tty_port_set()`) | f746g-disco `src/main.c`, 23; wio-lite-ai `src/kv_boot.c`, `net.shell.port` or 23 (or `src/main.c`, 23, when the KV thread does not run) |
| The commands that refuse on the telnet console (`net_tty_guard()`) | f746g-disco `cmds/cmd_blob_board.c`; wio-lite-ai `cmds/cmd_wifi.c`, `src/wifi_connect.c`, `cmds/cmd_mlperf.c`, `cmds/cmd_xfer.c`, `cmds/cmd_blob_board.c` |
| The adapter behind `svc/net/net_svc.h` (#219) | f746g-disco `port/netxduo/net_svc_f746.c`; wio-lite-ai `src/net_svc_wio.c` |
| The properties (#219) | `svc/net_svc_config.h` on each board |
| The lines of `net info` after the address | f746g-disco `port/netxduo/net_svc_f746.c` (`net_svc_board_lines()`); wio-lite-ai `src/nx_net.c` (`nx_net_write_status()`, reached through its adapter) |
| Its own subcommands | f746g-disco `cmds/cmd_net_board.c` (`mjpeg`), which registers no root; wio-lite-ai none since #221 (its `cmds/cmd_net_board.c` held `shell` and was removed) |

The NetX objects are created and placed by the board (it knows the memory map
and the driver), and the NetX driver and the link layer stay the board's.
f746g-disco's `mjpeg` is plugged in the way a board's own `camera`
subcommands are (wio-lite-ai's `net shell` was too, until #221 removed it);
wio-lite-ai's `net echo` was removed by #216 rather than plugged in.  The link layer's
commands stay outside `net`: f746g-disco has `eth` (`cmds/cmd_eth.c`, #217)
for the PHY renegotiation and the link state, and wio-lite-ai has `wifi`.

The shared files are audited for mutable storage like every other file the
build compiles under `shell/cmds/` and `svc/` (`cmake/shared_storage_gate.cmake`,
issue #206).  `cmd_net.c`, `net_core.c` and `net_tty.c` carry `optional BOARD_HAS_NETX` in
their first line, not `required` as `cmd_camera.c` does, because
grove-vision-ai-v2 has no network: f746g-disco and wio-lite-ai set
`BOARD_HAS_NETX ON` (their builds must ship the files), grove-vision-ai-v2
sets it `OFF` (its build must compile them nowhere); see `cmake/README.md`.  The `net` root is registered by the shared
file only.

### Where the network files are

| File | What it holds |
|---|---|
| `shell/cmds/cmd_net.c` | `net` itself (#219): `info ip dhcp ping`, the grammar, the refusals, the waits, the ping totals and every line, the board's subcommands appended |
| `shell/cmds/net_core.c`, `shell/cmds/net_core.h` | the one implementation over NetX (#220): the address read, the static address, DHCP, ping, the link notice and the worker (which also counts the falls and wakes the telnet listener, #221); the board's state and facts declared by name |
| `shell/cmds/net_tty.c`, `shell/cmds/net_tty.h` | the one telnet console (#221): the transport, the rings, the IAC handling, the `net-tty` listener, the two lines of `net info`, the question "is this the telnet console"; the board's state and facts declared by name |
| `svc/net/net_svc.h` | the contract (#219): the results, the address, the writer, the calls (#221 removed `net_svc_addressed()`) |
| f746g-disco `svc/net_svc_config.h`, wio-lite-ai `svc/net_svc_config.h` | the properties (#219; the STALE property went in #220, both boards print the mark) |
| f746g-disco `cmds/cmd_net_board.c` | the board's: `net mjpeg` only (until #219 `cmds/cmd_net.c`, the whole command; #217 moved `link` and the MAC / PHY / link lines of `net info` to `cmds/cmd_eth.c`) |
| f746g-disco `port/netxduo/net_svc_f746.c` | the adapter: "is the IP stack up", then the shared operation (#220); the board's lines of `net info`, the worker's last failure first |
| f746g-disco `cmds/cmd_eth.c` | the board's: `eth info` / `eth link` (#217), the link layer's command, over `port/eth/eth_link.h` only |
| f746g-disco `port/netxduo/nx_glue.c` | the NetX objects and their placement, the counters, `net_core.c`'s state and facts with the worker's stack (#220: the address work and the link notice went to the shared code, the poll hook was removed), and `net_tty.c`'s state, facts and buffers (#221) |
| f746g-disco `port/netxduo/nx_shell.c` | removed by #221 (the shared `net_tty.c`; its storage is in `nx_glue.c`) |
| f746g-disco `port/netxduo/nx_eth_driver.c`, `port/eth/eth_link.c` | the link layer: the NetX driver; the MAC / PHY and the eth-link thread, which only reports a link change to NetX (the poll hook went in #220) |
| wio-lite-ai `cmds/cmd_net_board.c` | removed by #221 with `net shell` (until #219 `cmds/cmd_net.c`, the whole command; `echo` was removed by #216) |
| wio-lite-ai `src/net_svc_wio.c` | the adapter: the host stack's state checked before the shared operation (the only source of BUSY); the board's lines of `net info`, the worker's last failure last |
| wio-lite-ai `src/nx_net.c` | the NetX objects and their placement, `net_core.c`'s state and facts with the worker's stack (#220: static / DHCP / ping and the DHCP stop went to the shared code), `net_tty.c`'s state, facts and buffers (#221), the build-time check of the transmit budget (moved from `src/nx_link_driver.c` by #222), and the host-stack section of `net info` (`nx_net_write_status()`, which takes the session's values from the link layer).  Until #222 it also held the bridge session's resident thread |
| wio-lite-ai `src/net_shell.c` | removed by #221 (the shared `net_tty.c`, resident; its storage is in `nx_net.c`) |
| wio-lite-ai `src/link_bridge.c`, `src/link_bridge.h` | the link layer: the bridge session's resident thread `nx-net` (#222, moved from `src/nx_net.c`): arming and refreshing the module's bridge, its states and failure rule, the automatic re-association, the recovery after `wifi on` / `off` / `reset`, and the module's DATA counters |
| wio-lite-ai `src/nx_link_driver.c`, `src/link_data.c`, `src/rtl_link.c`, `src/erpc.c` | the link layer: the NetX driver; the DATA channel, the RTL8720DN link and its eRPC service thread |

How each link layer works stays in the board README and is only pointed to
from here: f746g-disco's
[The Ethernet link layer](../boards/f746g-disco/README.md#the-ethernet-link-layer-the-eth-command-issue-217)
(written in #217),
[Consoles](../boards/f746g-disco/README.md#consoles) and
[Network counters](../boards/f746g-disco/README.md#network-counters-net-info-and-net-mjpeg-stats-issue-202);
wio-lite-ai's [Commands](../boards/wio-lite-ai/README.md#commands) and
[The WiFi link layer](../boards/wio-lite-ai/README.md#the-wifi-link-layer-issue-222)
(written in #222, a map of its files and threads; the description is the
comments at the top of those files).

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
`nn_tflm_board_state`; the Ethos-U cache protocol's `npu_cache_board_state`;
`shell/cmds/net_core.c`'s `net_core_board_state` and `shell/cmds/net_tty.c`'s
`net_tty_board_state`, whose NetX callbacks pass no context either). This is enforced per
board by `cmake/check_no_mutable_storage.py` through the build-derived audit in
`cmake/shared_storage_gate.cmake`, which also covers everything compiled under
`shell/cmds/` (see `cmake/README.md`; `svc/ymodem.c` is the one exception).

## Where the per-board files are described

Each board README has an "nn files" table: one line per file under that
board's `port/nn/` (and `port/plugin/` where the board loads plugins), with the
thread it runs on and the hardware reason it is the board's.

- [Grove Vision AI V2](../boards/grove-vision-ai-v2/README.md#the-nn-files)
- [Wio Lite AI](../boards/wio-lite-ai/README.md#the-nn-files)
- [STM32F746G-DISCO](../boards/f746g-disco/README.md#the-nn-files)

The network files of f746g-disco and wio-lite-ai, today and after each child
issue of #212, are the table at the end of
[The `net` command and the telnet console are one implementation](#the-net-command-and-the-telnet-console-are-one-implementation-issue-212);
how each board's link layer works is that board's README.
