#!/usr/bin/env sh
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Board-pinned host tests for STM32F746G-DISCO (issue #47).  These compile code
# this board OWNS against the board's REAL headers -- which is the point of them:
# a shimmed copy could drift from the firmware's without anything noticing.  They
# cannot live in shell/test, which is board-independent by construction.
#
# Invoked by shell/test/run_host_tests.sh, which owns the toolchain flags and the
# scratch directory and passes them in the environment; there is no separate set
# of flags here, so a board test is built exactly like a core one.  Run the whole
# suite (or `run_host_tests.sh f746g-disco` for this board alone) rather than
# executing this file directly.
set -eu

: "${HOST_TEST_OUT:?run via shell/test/run_host_tests.sh}"
: "${HOST_TEST_CFLAGS:?}" "${HOST_TEST_LDFLAGS:?}"

here=$(cd "$(dirname "$0")" && pwd)
board=$(cd "$here/.." && pwd)
out="$HOST_TEST_OUT"
CFLAGS="$HOST_TEST_CFLAGS"
LDFLAGS="$HOST_TEST_LDFLAGS"

# issue #72 -- the sink-drain decision (port/camera/cam_drain.c).  Three
# subscribers here detach while the base capture keeps running, so each has to
# wait for its sink to go idle before releasing what that sink reads.  The
# branch that matters is the wait that does NOT finish, and nothing a console
# can type produces it: it needs a consume() that never returns, or one that
# returns without putting.  The vector that carries the point is "the count is
# zero on the poll where the deadline also expired" -- a drain that completed at
# the instant its budget ran out has completed, and checking the clock first
# would strand a teardown entitled to proceed (the shape of issue #65's bug).
gcc $CFLAGS -I "$board/port/camera" \
    "$here/test_cam_drain.c" "$board/port/camera/cam_drain.c" \
    $LDFLAGS -o "$out/test_cam_drain"
"$out/test_cam_drain"

# issue #72 -- the owner lifecycle (port/camera/cam_own.c).  The states that
# decide whether a start / re-open / reuse may touch a sink whose teardown has
# not finished.  Same reason it is a host test: the interesting transitions need
# a drain that spends its budget or two owner commands in flight at once, and
# neither can be typed.  Compiles the PURE half only -- the serialised half needs
# a PRIMASK, and what it guards (the critical section, and the ordering that puts
# DRAINING before the unsubscribe) is not something this can check.
gcc $CFLAGS -I "$board/port/camera" \
    "$here/test_cam_own.c" "$board/port/camera/cam_own.c" \
    "$board/port/camera/cam_drain.c" \
    $LDFLAGS -o "$out/test_cam_own"
"$out/test_cam_own"

# issue #142 -- this board's two decisions around the camera's holder table
# (port/camera/cam_hold_rules.c): whether the producer's teardown still goes
# ahead once a holder came back after the stop was asked for (an error teardown
# always does), and whether nn may join the stream in the current mode (D3).
gcc $CFLAGS -I "$board/port/camera" \
    "$here/test_cam_hold_rules.c" "$board/port/camera/cam_hold_rules.c" \
    $LDFLAGS -o "$out/test_cam_hold_rules"
"$out/test_cam_hold_rules"

# issue #130 -- when `nn stream` may give the nn session back: in svc/ since
# issue #163 (svc/nn/nn_sess_release.c) and walked by
# shell/test/test_nn_sess_release.c.

# issue #130 step 6c (#122 P8) -- how a plugin's decode is counted in `nn stream
# stats`: in svc/ since issue #163 (svc/nn/nn_decode_count.c, one copy with
# wio-lite-ai, and grove-vision-ai-v2 since issue #164) and walked by shell/test/test_nn_decode_count.c.

# issue #162 -- the camera input refusals (svc/nn/nn_input_codes.h, tested in
# shell/test/test_nn_input_codes.c) against this board's other nn_camera_start()
# codes: none may land on a value that already means something else here.
gcc $CFLAGS -I "$board/port/nn" -I "$board/port/camera" -I "$board/svc" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" -I "$HOST_TEST_SVC/plugin" -I "$HOST_TEST_SVC/camera" -I "$HOST_TEST_SVC/blob" \
    "$here/test_nncam_input.c" $LDFLAGS -o "$out/test_nncam_input"
"$out/test_nncam_input"

# issue #176 -- the stedgeai_reloc backend's shape mapping (port/nn/nn_reloc_shape.h,
# the ST-header-free half of reloc_map()): 65535 passes and 65536 / > INT32_MAX is
# NN_MODEL_ERR_DIMS on each of B / H / W / C, 0 becomes 1, W <= 1 collapses to 3-D,
# no element size is NN_MODEL_ERR_TENSOR, and bytes above the arena -- including a
# product that would wrap 32 bits -- is NN_MODEL_ERR_ARENA.  No model on hand reaches
# any of these, and the reloc tree needs the ST SDK, so this is where they are seen.
gcc $CFLAGS -I "$board/port/nn" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" \
    "$here/test_reloc_shape.c" $LDFLAGS -o "$out/test_reloc_shape"
"$out/test_reloc_shape"

# issue #130 -- negative tests for the tflm residents of cmake/check_f746_layout.py
# (the activation arena and the SD model slots must sit in SDRAM bank3).  Driven
# by fake nm/objdump, so no toolchain is needed: what is checked is the gate's
# DECISION on the exact names board.cmake passes (read from board.cmake, not
# copied), including that a null build is not asked for them.  That the compiler
# spells them that way is the firmware build's job -- the same gate fails there
# on a name that matches nothing.
python3 "$board/cmake/fixtures/run_layout_tests.py"

# The model singleton itself (svc/nn/nn.c, shared with wio-lite-ai since issue
# #161) is walked by shell/test/test_nn_model.c; the two tests below link the
# same file over shell/test/nn_model_host.c, the board's state and hooks
# modelled.
#
# issue #131 step 7d -- this board's `nn info` / `nn model load` / `nn model
# unload` (port/nn/nn_svc_f746_model.c; `nn info`'s body is
# svc/nn/nn_svc_common.c's since issue #174) behind the shared order
# (svc/nn/nn_core_model.c), against the real svc/nn/nn.c over a stub backend.  What a
# console cannot produce on demand: a refusal from empty, a backend that
# "restores" from nothing (EMPTY, released), a success that left no model (HW),
# and `nn info` from another console in the middle of a swap (BUSY on all three
# lines).  shell/test/test_nn_core_model.c walks the order itself.  Since issue
# #194 the model is a blob, read by name out of a stand-in store
# (nn_blob_stub.c): every answer of the store, refused with its own status and
# words, and the blob flag taken and given back once on every path.
gcc $CFLAGS -I "$here/nn_shim" -I "$here" -I "$board/src" -I "$board/port/qspi" \
    -I "$board/port/nn" -I "$board/port/camera" \
    -I "$board/port/sdram" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" -I "$HOST_TEST_SVC/plugin" -I "$HOST_TEST_SVC/camera" -I "$HOST_TEST_SVC/blob" \
    "$here/test_nn_model_life.c" "$board/port/nn/nn_svc_f746_model.c" \
    "$here/nn_blob_stub.c" "$HOST_TEST_SVC/blob/blob_state.c" \
    "$HOST_TEST_SVC/crc32.c" \
    "$HOST_TEST_SVC/nn/nn.c" "$HOST_TEST_REPO/shell/test/nn_model_host.c" \
    "$HOST_TEST_SVC/nn/nn_core_model.c" "$HOST_TEST_SVC/nn/nn_svc_common.c" \
    "$HOST_TEST_SVC/nn/nn_swap.c" "$HOST_TEST_SVC/fmt.c" \
    $LDFLAGS -o "$out/test_nn_model_life"
"$out/test_nn_model_life"

# issue #159 (#78 Step 4a) -- `nn model load --name <container>` (a file on the
# SD card until issue #194) on the tflm
# build: the same adapter compiled WITH CONFIG_NN_BACKEND_TFLM and this board's
# policy macros (the values board.cmake gives a tflm build), over the real
# plugin validator and CRC -- through the firmware's entry
# (plugin_load_board.c), which reads this adapter's plugin_board_policy by
# name, as the device does (issue #165).  A container built byte by byte with this board's
# facts is accepted and its MODEL SECTION handed over in place; wio's target
# word, a truncated file and a damaged plugin byte are refused before the
# backend is touched, and so is a copy whose CRC disagrees with its header.
gcc $CFLAGS -DCONFIG_NN_BACKEND_TFLM=1 -DF746_PLUGIN_TARGET_ID=0x1101 \
    -DF746_PLUGIN_BASE=0x20048000u -DF746_PLUGIN_MAX=32768u \
    -DPLUGIN_VENEER_BASE_COST=640u -DF746_PLUGIN_STACK_NN_WORK=1024u \
    -DF746_PLUGIN_STACK_GUIX=1024u -DF746_PLUGIN_STACK_SHELL=1024u \
    -I "$here/nn_shim" -I "$here" -I "$board/src" -I "$board/port/qspi" \
    -I "$board/port/nn" -I "$board/port/camera" \
    -I "$board/port/sdram" -I "$board/port/plugin" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" -I "$HOST_TEST_SVC/plugin" -I "$HOST_TEST_SVC/camera" -I "$HOST_TEST_SVC/blob" \
    "$here/test_nn_model_container.c" "$board/port/nn/nn_svc_f746_model.c" \
    "$here/nn_blob_stub.c" "$HOST_TEST_SVC/blob/blob_state.c" \
    "$HOST_TEST_SVC/nn/nn.c" "$HOST_TEST_REPO/shell/test/nn_model_host.c" \
    "$HOST_TEST_SVC/nn/nn_core_model.c" "$HOST_TEST_SVC/nn/nn_svc_common.c" \
    "$HOST_TEST_SVC/nn/nn_swap.c" "$HOST_TEST_SVC/fmt.c" \
    "$HOST_TEST_SVC/plugin/plugin_load.c" "$HOST_TEST_SVC/plugin/plugin_load_board.c" "$HOST_TEST_SVC/plugin/plugin_info.c" \
    "$HOST_TEST_SVC/plugin/plugin_claims.c" \
    "$HOST_TEST_SVC/crc32.c" \
    $LDFLAGS -o "$out/test_nn_model_container"
"$out/test_nn_model_container"

# issue #159 -- the heap's bound by BEHAVIOUR: the real src/retarget.c must
# grow the break to exactly `_estack - _Min_Stack_Size` (below the .plugin
# reservation) and no further.  cmake/check_f746_layout.py checks that the
# linked _sbrk NAMES _estack; it cannot check that _estack GOVERNS the
# comparison (`_estack + _Min_Stack_Size` names the same constants), and this
# can.  The linker symbols are renamed for retarget.c only, to labels
# test_sbrk.c lays out with memory above _estack, so a bound past it links and
# fails rather than failing to link.
gcc $CFLAGS -Dend=f746_test_heap_floor -D_estack=f746_test_estack \
    -D_Min_Stack_Size=f746_test_min_stack \
    -c "$board/src/retarget.c" -o "$out/retarget_host.o"
gcc $CFLAGS "$here/test_sbrk.c" "$out/retarget_host.o" \
    $LDFLAGS -o "$out/test_sbrk"
"$out/test_sbrk"

# issue #159 -- the file destination's fit check reads the staging capacity
# back out of the shipped firmware (cmake/build_asset.py stage_capacity()).  No
# board sends an asset as a file since issue #194 (this one sends to a slot);
# the shared route and its tests stay until it is removed (issue #195).  Its
# refusals need a real ARM ELF, so this runs with the repository-pinned cross
# toolchain and SKIPS -- loudly -- without it.  (The toolchain-free cases of the
# same script run from shell/test/run_host_tests.sh.)
gate_cc=$(ls -d "$HOST_TEST_REPO"/tools/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | tail -1)
if [ -n "$gate_cc" ] && [ -x "$gate_cc" ]; then
    python3 "$HOST_TEST_REPO/cmake/fixtures/run_asset_dest_tests.py" --cc "$gate_cc"
else
    echo "run_asset_dest_tests --cc: SKIPPED -- no arm-none-eabi toolchain under" \
         "$HOST_TEST_REPO/tools/ (configure an f746 build once to fetch it)" >&2
fi

# issue #194 -- the QSPI driver's two decisions after a receive and after a
# failed HAL call (port/qspi/qspi_idle.h): whether the CCR may be rewritten to
# put the functional mode back to indirect write (only for a form with a data
# phase -- rewriting one without sends its instruction again), and whether the
# controller reads idle (no abort pending, not busy, not in indirect read);
# and before every erase / program / register write, whether the flash part
# allows it to start (write-enable latch set, no earlier operation running).
# The controller decides when it is busy, so no console command reaches these
# combinations; the driver around them is seen on the board only.
gcc $CFLAGS -I "$board/port/qspi" \
    "$here/test_qspi_idle.c" $LDFLAGS -o "$out/test_qspi_idle"
"$out/test_qspi_idle"

# issue #194 -- the blob store on the QSPI NOR.  First the slot table
# (src/blob_slots.c), pinned entry by entry and checked against the interval
# src/blob.h compiles into the firmware; then the store code itself
# (src/blob.c), compiled unchanged against a 16 MB array for the NOR with the
# qspi_flash_* calls replaced and counted and the PRIMASK intrinsics it takes
# from the HAL traced (blob_shim/).  Until the `blob` command and the nn reader
# arrive most of src/blob.c is dropped from the firmware by --gc-sections, so
# this is where it runs.
gcc $CFLAGS \
    -I "$board/src" -I "$board/port/qspi" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/blob" \
    "$here/test_blob_slots.c" "$board/src/blob_slots.c" \
    "$HOST_TEST_SVC/blob/blob_map.c" "$HOST_TEST_SVC/blob/blob_state.c" \
    "$HOST_TEST_SVC/crc32.c" \
    $LDFLAGS -o "$out/test_blob_slots_f746"
"$out/test_blob_slots_f746"
gcc $CFLAGS -I "$here/blob_shim" \
    -I "$board/src" -I "$board/port/qspi" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/blob" \
    "$here/test_blob_store.c" "$board/src/blob.c" "$board/src/blob_slots.c" \
    "$HOST_TEST_SVC/blob/blob_map.c" "$HOST_TEST_SVC/blob/blob_state.c" \
    "$HOST_TEST_SVC/crc32.c" \
    $LDFLAGS -o "$out/test_blob_store_f746"
"$out/test_blob_store_f746"
# issue #194 -- the whole store (src/blob.c + src/blob_writer.c) under the shared
# write coordinator, end to end on the array: a completed write reads VALID and
# verifies, a disagreeing read-back leaves INCOMPLETE, a cancelled erase leaves
# no header, the busy flag is taken and given back once and refuses the public
# reads while held (`qspi test` holds it too), and the writer stays inside the
# store's interval.  blob_shim/log.h stands in for svc/log.h.
gcc $CFLAGS -I "$here/blob_shim" \
    -I "$board/src" -I "$board/port/qspi" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/blob" \
    "$here/test_blob_writer.c" "$board/src/blob.c" "$board/src/blob_writer.c" \
    "$board/src/blob_slots.c" "$HOST_TEST_SVC/blob/blob_map.c" \
    "$HOST_TEST_SVC/blob/blob_state.c" "$HOST_TEST_SVC/blob/blob_write.c" \
    "$HOST_TEST_SVC/crc32.c" \
    $LDFLAGS -o "$out/test_blob_writer_f746"
"$out/test_blob_writer_f746"

# issue #194 -- tests for cmake/check_blob_staging.py (shared with
# wio-lite-ai), the post-link gate that reads the slot table and the model
# staging capacity back out of the image.  The header unit this board's
# board.cmake passes is compared with the real src/blob.h, and the backend
# lists with the backends this board offers (stedgeai and stedgeai_reloc are
# not applicable, as null is); the synthetic images need the pinned toolchain
# and SKIP loudly without it.
if [ -n "$gate_cc" ] && [ -x "$gate_cc" ]; then
    python3 "$HOST_TEST_REPO/cmake/fixtures/run_blob_staging_tests.py" \
        --board-dir "$board" --include port/qspi --cc "$gate_cc"
else
    python3 "$HOST_TEST_REPO/cmake/fixtures/run_blob_staging_tests.py" \
        --board-dir "$board" --include port/qspi
    echo "run_blob_staging_tests image cases: SKIPPED -- no arm-none-eabi" \
         "toolchain under $HOST_TEST_REPO/tools/" >&2
fi

# issue #143 -- the `camera` table this board builds: the shared command
# (shell/cmds/cmd_camera.c) against this board's REAL svc/camera_svc_config.h,
# over the shared stub adapter.  A board subcommand that reuses a shared name
# is silently unreachable (the parser takes the first match), and only the
# built table shows it; so does a shared subcommand whose capability is 0 but
# that is registered anyway.  shell/test/test_camera_cmd.c proves the check
# sees a duplicate.
st="$HOST_TEST_SHELL_TEST"
gcc $CFLAGS -DCLI_USE_COLOR=0 \
    -I "$st" -I "$HOST_TEST_BACKEND" -I "$st/shim" -I "$st/fx_shim" \
    -I "$board/svc" -I "$HOST_TEST_INC" -I "$HOST_TEST_CORE" \
    -I "$st/../cmds" -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" -I "$HOST_TEST_SVC/plugin" -I "$HOST_TEST_SVC/camera" -I "$HOST_TEST_SVC/blob" \
    "$here/test_camera_table.c" "$st/camera_svc_stub.c" \
    "$st/../cmds/cmd_camera.c" \
    "$HOST_TEST_CORE/cli_session.c" "$HOST_TEST_CORE/cli_edit.c" \
    "$HOST_TEST_CORE/cli_history.c" "$HOST_TEST_CORE/cli_printf.c" \
    "$HOST_TEST_SVC/fmt.c" "$HOST_TEST_CORE/cli_parse.c" \
    "$HOST_TEST_CORE/cli_complete.c" \
    "$HOST_TEST_BACKEND/cli_backend_dummy.c" "$st/host_glue.c" \
    $LDFLAGS -o "$out/test_camera_table"
"$out/test_camera_table"

# issue #159 (#78 Step 4b) -- the painter's loops (port/plugin/plugin_paint.c):
# row-major CPU stores into the camera view buffer.  Built with
# PLUGIN_PAINT_COUNT_STORES so every store is counted where it happens; the
# outline goldens are wio-lite-ai's (rect_geom.c is shared), so the two boards'
# loops are held to the same rule from opposite layouts.
gcc $CFLAGS -DPLUGIN_PAINT_COUNT_STORES -I "$board/port/plugin" \
    -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" -I "$HOST_TEST_SVC/plugin" -I "$HOST_TEST_SVC/camera" -I "$HOST_TEST_SVC/blob" \
    "$here/test_plugin_paint.c" "$board/port/plugin/plugin_paint.c" \
    "$HOST_TEST_SVC/plugin/plugin_paint_budget.c" \
    "$HOST_TEST_SVC/plugin/plugin_bindings.c" "$HOST_TEST_SVC/plugin/plugin_paint_span.c" "$HOST_TEST_SVC/rect_geom.c" \
    $LDFLAGS -o "$out/test_plugin_paint_f746"
"$out/test_plugin_paint_f746"

# issue #159 -- the shared Armv7-M MPU verdict on THIS board's region set (the
# ones src/bsp.c programs), and the configurations that must be refused: the
# background map off, an XN / Device / read-only region over the reservation,
# a DREGION the snapshot cannot hold.  Since issue #194 the set includes the
# two no-access fences over 0x60000000-0x9FFFFFFF: the reservation still
# passes, and an address inside them is refused.
gcc $CFLAGS -I "$HOST_TEST_SVC/plugin" \
    "$here/test_plugin_mpu_f746.c" "$HOST_TEST_SVC/plugin/plugin_mpu_v7m.c" \
    $LDFLAGS -o "$out/test_plugin_mpu_f746"
"$out/test_plugin_mpu_f746"

# issue #159 -- a fault inside the loaded plugin is attributed and resets at
# once; anywhere else keeps the dump-and-halt (src/fault_plugin.c over the real
# shared loader).  A plugin cannot be made to fault on demand on the board.
gcc $CFLAGS -I "$board/src" \
    -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" -I "$HOST_TEST_SVC/plugin" -I "$HOST_TEST_SVC/camera" -I "$HOST_TEST_SVC/blob" \
    "$here/test_fault_plugin.c" "$board/src/fault_plugin.c" \
    "$HOST_TEST_SVC/plugin/plugin_run_core.c" "$HOST_TEST_SVC/plugin/plugin_exec.c" \
    "$HOST_TEST_SVC/plugin/plugin_load.c" "$HOST_TEST_SVC/plugin/plugin_load_host.c" "$HOST_TEST_SVC/crc32.c" "$HOST_TEST_SVC/fmt.c" \
    $LDFLAGS -o "$out/test_fault_plugin"
"$out/test_fault_plugin"

# issue #159 -- every way into the plugin on this board is walked without the
# lease by a host test, or named in cmake/check_lease_entries.py with why (the
# script wio-lite-ai has used since #130, shared now).  The entries are the
# external text symbols of this board's nn_active.c and plugin_run.c and of the
# two shared TUs, each compiled ON ITS OWN; the walks are the shared tests'
# (shell/test/test_nn_active_lease.c, test_plugin_run_lease.c).  The HAL for
# the binding is test/plugin_run_shim (compile only).
lease_check="$HOST_TEST_REPO/cmake/check_lease_entries.py"
lease_inc="-I $board/port/nn -I $board/port/plugin -I $board/port/camera -I $board/svc -I $here/plugin_run_shim -I $here/nn_shim -I $HOST_TEST_SVC -I $HOST_TEST_SVC/nn -I $HOST_TEST_SVC/plugin -I $HOST_TEST_SVC/camera -I $HOST_TEST_SVC/blob"
gcc $CFLAGS -DCONFIG_NN_BACKEND_TFLM=1 $lease_inc -c "$board/port/nn/nn_active.c" \
    -o "$out/f746_lease_nn_active.o"
gcc $CFLAGS $lease_inc -c "$HOST_TEST_SVC/nn/nn_active_lease.c" \
    -o "$out/f746_lease_nn_active_lease.o"
gcc $CFLAGS $lease_inc -c "$HOST_TEST_SVC/plugin/plugin_run_core.c" \
    -o "$out/f746_lease_plugin_run_core.o"
gcc $CFLAGS $lease_inc -c "$board/port/plugin/plugin_run.c" \
    -o "$out/f746_lease_plugin_run.o"
python3 "$lease_check" \
    --obj "$out/f746_lease_nn_active.o" "$out/f746_lease_nn_active_lease.o" \
          "$out/f746_lease_plugin_run_core.o" "$out/f746_lease_plugin_run.o" \
    --test "$HOST_TEST_REPO/shell/test/test_nn_active_lease.c" \
           "$HOST_TEST_REPO/shell/test/test_plugin_run_lease.c"

# issue #159 (Step 4b review) -- this board's MPU read-back
# (port/plugin/plugin_run.c, plugin_run_mpu_ok()): the REAL binding over
# test/plugin_run_shim, whose MPU answers RBAR / RASR for the region RNR
# selects.  Every region up to DREGION is read, RNR and PRIMASK are put back,
# DREGION above the snapshot refuses rather than clamps.  The shared lease test
# stubs this binding, so this is where the loop itself is seen.
gcc $CFLAGS -I "$here/plugin_run_shim" -I "$board/svc" \
    -I "$HOST_TEST_SVC" -I "$HOST_TEST_SVC/nn" -I "$HOST_TEST_SVC/plugin" -I "$HOST_TEST_SVC/camera" -I "$HOST_TEST_SVC/blob" \
    "$here/test_plugin_run_mpu.c" "$board/port/plugin/plugin_run.c" \
    "$HOST_TEST_SVC/plugin/plugin_mpu_v7m.c" \
    $LDFLAGS -o "$out/test_plugin_run_mpu"
"$out/test_plugin_run_mpu"

# issue #175 -- the model I/O check this board's host gate (verify_tflite) compiles from
# svc/nn/tflm/nn_tflm_model_check.h, the function the backend runs before it plans a
# model: generated files wrong in exactly one way must be refused as the board would
# refuse them (an I/O dimension outside 0..65535 -> NN_MODEL_ERR_DIMS, a missing or
# out-of-range I/O list -> NN_MODEL_ERR_FORMAT), and the 65535 control must pass the
# walk.  The gate is the one a CONFIGURED tree builds (`verify-model`, host c++ against the
# tree's TFLM), so it needs that tree: build/f746-tflm, or
# F746_TFLM_BUILD_DIR to use another.  No tree, or no gate rule in it: a loud SKIP.
io_build="${F746_TFLM_BUILD_DIR:-$HOST_TEST_REPO/build/f746-tflm}"
if [ ! -f "$io_build/build.ninja" ]; then
    echo "run_model_io_gate_tests: SKIPPED -- no configured build tree at" \
         "$io_build (set F746_TFLM_BUILD_DIR to use another)" >&2
elif ! ninja -C "$io_build" -t targets all 2>/dev/null | grep -q "^verify_tflite:"; then
    echo "run_model_io_gate_tests: SKIPPED -- $io_build has no verify_tflite rule" \
         "(a tflm backend and a host C++ compiler are needed)" >&2
else
    python3 "$HOST_TEST_REPO/cmake/fixtures/run_model_io_gate_tests.py" \
        --build-dir "$io_build" --gate verify_tflite --style tflite
    # issue #194 -- the same tool's length checks: the slot limit is the largest
    # payload of the shipped table (compiled in), so 1,044,480 B gets past it
    # and one more -- and the old 1 MB limit -- must not.  Same tree, same SKIP.
    python3 "$board/cmake/fixtures/run_verify_tflite_size_tests.py" \
        --build-dir "$io_build"
fi
