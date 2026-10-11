# svc/ -- board-independent services

`svc/` is the freestanding service layer. Its translation units include no HAL,
no CMSIS device header, no board header and no `tx_api.h`, so the same source is
compiled into every board's firmware (from `shell/` and from `boards/<board>/`)
and into the host tests under `shell/test/` and `boards/<board>/test/`. Anything
that needs a peripheral, a memory map, a lock or a thread stays in the board and
is reached from here through a hook or a declared API.

Besides the `nn` and plugin files described below, it holds the formatter
(`fmt`), CRC-32 (`crc32`), YMODEM (`ymodem`), the camera frame pipeline
(`frame`, `frame_pipeline`), the camera stream's holder table and sink steps
(`cam_holders`, `cam_step`, below), small graphics helpers (`gfx_rot`, `rect_geom`), the
neutral tensor descriptor (`tensor.h`) and the BlazeFace decoder (`blazeface`).

## The `nn` command's layering

There is one `nn` command for all three boards. What differs between boards is
below the adapter, never in the command.

```
shell/cmds/cmd_nn.c (+ nn_cmd_core.c)    one command; a subcommand whose
        |                                NN_SVC_HAS_* capability is 0 is not
        |                                registered (boards/<board>/svc/nn_svc_config.h)
        v
svc/nn_svc.h                             the contract: the nn_svc_*() calls
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
        |   svc/nn_core.c       lifecycle policy and the stream's numbers
        |   svc/nn_core_frame.c producer / worker / panel frame path
        |   svc/nn_stream_life.c, nn_handoff.c, nn_det_record.c,
        |   nn_core_model.c, nn_swap.c, nn_active_core.c,
        |   plugin_lease_api.h (implemented by the board)
        v
backend: boards/<board>/port/nn/tflm/ (wio-lite-ai, f746g-disco)
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
turns its own tensors into `svc/tensor.h` descriptors before anything shared
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
- `camera stream stats` (`camera stats` on grove-vision-ai-v2) prints the
  holders on a `holders:` line, and how many the last confirmed end lost.
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

## One line per shared nn / plugin / camera file

Each pair is a `.c` and its `.h` unless only a header is listed.

| File | Role |
|---|---|
| `nn_svc.h` | The contract the shared `nn` command speaks; implemented by each board's adapter. |
| `nn_svc_adapter.h` | Static-inline helpers every adapter uses to build a result and an `nn info` line. |
| `nn_detail.h` | The bound on a board's `nn` explanation text and its build-time check. |
| `nn_stream_life` | The one live-inference state machine (start admission, generations, stop claims). |
| `nn_core` | The policy around that machine and the stream's numbers, called from a console. |
| `nn_core_frame` | The frame path: what the producer, worker and panel do with each frame. |
| `nn_handoff` | The hand-over word that says who may touch the model's input and when. |
| `nn_det_record` | The published decode record and the generation rule that keeps a stale decode out. |
| `nn_core_model` | The order of `nn model load` and `nn model unload`. |
| `nn_swap` | Where a model load ended up and what that obliges (the ending table). |
| `nn_active_core` | Whether a plugin decodes, and every call into its decoder slots. |
| `nn_report` | The bounded sink that captures a plugin's account of its own result. |
| `nn_top` | The top classes of an output tensor, taken on the thread that ran the inference. |
| `plugin_abi.h` | The wire ABI between the firmware and a plugin shipped with its model. |
| `plugin_target.h` | The plugin target word this translation unit is compiled for. |
| `plugin_load` | Container and manifest validation; returns offsets, never a callable pointer. |
| `plugin_exec` | Copies a validated image into the board's reservation and calls its entry. |
| `plugin_mpu` | Armv8-M MPU verdict: is the plugin reservation executable (pure function). |
| `plugin_mpu_v7m` | The same question for an Armv7-M PMSA, answered by its own rules. |
| `plugin_info` | The plugin stack lines of `nn info`. |
| `plugin_lease_api.h` | The plugin lease's API (one lock into the plugin); the board implements it. |
| `plugin_lease_miss` | What a refused no-wait lease acquire counts, over state the caller owns. |
| `plugin_paint_budget` | What one plugin `draw()` may spend, and the one place it is charged. |
| `plugin_paint_span` | What a painter primitive decides before its first store (clip, offset, charge). |
| `cam_holders` | Who holds the camera stream open, and the start / stop / refusal each change implies (issue #142). |
| `cam_step` | One step of the camera sink: may the producer enter it, and has it left (issue #142). |

## Why the lock and the threads are the board's

`svc/` cannot include `tx_api.h` or any board header: the same object has to
build for three boards and for the host. So every mutex, every bounded wait and
every thread is the board's. The board implements the APIs this layer declares
(`plugin_lease_api.h`), and passes critical-section entry/exit, tick reads, the
lease and the decode as hooks in a const ops struct (`struct nn_core_frame_ops`,
`struct nn_core_board`).

The shared translation units also hold no mutable storage of their own: state
lives in a board-owned struct that the board passes in. This is enforced per
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
