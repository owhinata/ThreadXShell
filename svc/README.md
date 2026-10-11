# svc/ -- board-independent services

`svc/` is the freestanding service layer. Its translation units include no HAL,
no CMSIS device header, no board header and no `tx_api.h`, so the same source is
compiled into every board's firmware (from `shell/` and from `boards/<board>/`)
and into the host tests under `shell/test/` and `boards/<board>/test/`. Anything
that needs a peripheral, a memory map, a lock or a thread stays in the board and
is reached from here through a hook or a declared API.

Besides the `nn` and plugin files described below, it holds the formatter
(`fmt`), CRC-32 (`crc32`), YMODEM (`ymodem`), the camera frame pipeline
(`frame`, `frame_pipeline`), small graphics helpers (`gfx_rot`, `rect_geom`), the
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
boards/<board>/port/nn/nn_camera.c ...   the board's glue and threads:
   producer | worker | panel | console     each thread calls into the shared
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

The command and the shared core never see a backend's tensor type: the board
turns its own tensors into `svc/tensor.h` descriptors before anything shared
reads them.

## One line per shared nn / plugin file

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
