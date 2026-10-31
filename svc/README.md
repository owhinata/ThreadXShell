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
| `svc/nn/` | The `nn` command's contract and shared core (`nn_*`, described below), the neutral tensor descriptor (`tensor.h`), the inference API `nn.h`, its one implementation `nn.c` (the dispatcher over the compiled-in backend, `nn_backend.h`) and its translation into that descriptor `nn_desc` (wio-lite-ai and f746g-disco, issue #161), and the BlazeFace decoder (`blazeface`), which reaches every board only inside a plugin container (no board links it into its firmware since issue #159 Step 4c). |
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
         boards/grove-vision-ai-v2/port/nn/npu/ (Ethos-U55 + TFLM)
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
| `nn/nn` | The inference API (`nn.h`) and its one dispatcher for wio-lite-ai and f746g-disco (issue #161): the model singleton and its first-open latch, the session guard, the external claim on the backend's memory (f746g-disco's `sdram test`), cycle timing around `nn_run()`, and named refusal codes passed through unfolded. |
| `nn/nn_backend.h` | The vtable the one compiled-in backend (`null`, `tflm`, f746g-disco's `stedgeai*`) exports as `nn_backend_vt_selected`. |
| `nn/nn_dispatch_state.h` | What a board gives `nn.c`: its state as one object the board defines (`nn_model_board_state`, bound at link time) and the hooks -- a critical section that restores the mask it found, a one-tick wait, and the cycle counter's start and fenced read (`boards/<board>/port/nn/nn_model_board.c`). |
| `nn/nn_desc` | `nn_tensor` -> `svc/nn/tensor.h` descriptor, for the boards that use `nn.h`. |
| `nn/tflm/` | The one tflite-micro backend of wio-lite-ai and f746g-disco (issue #161; built only with `CONFIG_NN_BACKEND=tflm`, into the board's `tflm` library): `nn_tflm.cc` (the vtable), `nn_tflm_state.h` (its state type, defined by the board in `port/nn/tflm/nn_tflm_board.cc` with the board's facts), `nn_tflm_mem.h` (the arena, staging slots and capacity object the board places in `port/nn/tflm/nn_tflm_bufs.c`; sizes are compile definitions), `nn_tflm_ops.h` (the op profiles, shared with each board's host `verify_tflite`), `nn_tflm_fit.h` (which slot a model range may be adopted from, never the live one; host-tested) and `cxx_runtime.cc` (the C++ runtime shim). The only directory under `svc/` that includes TFLM headers. |
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
| `plugin/plugin_abi.h` | The wire ABI between the firmware and a plugin shipped with its model. |
| `plugin/plugin_target.h` | The plugin target word this translation unit is compiled for. |
| `plugin/plugin_load` | Container and manifest validation; returns offsets, never a callable pointer. |
| `plugin/plugin_exec` | Copies a validated image into the board's reservation and calls its entry. |
| `plugin/plugin_run_core` | Above that loader, for an Armv7-M board staging in RAM (wio-lite-ai, f746g-disco): the lease checked before a load or unload and again at the branch into entry, the load's log lines and the staging-region check; the board keeps its MPU read-back and cache maintenance. |
| `plugin/plugin_mpu` | Armv8-M MPU verdict: is the plugin reservation executable (pure function). |
| `plugin/plugin_mpu_v7m` | The same question for an Armv7-M PMSA, answered by its own rules. |
| `plugin/plugin_info` | The plugin stack lines of `nn info`. |
| `plugin/plugin_claims` | The open model's container claims (a board-owned store, read in the board's critical section) and the `nn info` plugin lines around the stack lines (wio-lite-ai, f746g-disco). |
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
`nn_tflm_board_state`). This is enforced per
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
