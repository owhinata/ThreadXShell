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

# issue #97 -- the adapter onto the SHARED BlazeFace decoder (port/nn/nn_decoder.c).
# The decoder's arithmetic moved to svc/blazeface.c and is covered by
# shell/test/test_blazeface.c, which is board-independent; what stays here is the
# half that cannot be -- nn_tensor -> tensor_desc, against this board's real nn.h.
# struct nn_model is opaque (defined in nn.c), so the test supplies its own
# nn_output_count() / nn_output(), which is also what makes the adapter testable.
#
# The cases are the translation's own failure modes: an unsupported dtype mapped
# onto one the decoder reads, float32 put through the affine form (this board
# publishes scale 0 for an unquantised tensor, and its graphs are float32), a rank
# above four truncated into a match, and a hole in the output set reported as a
# model-shape problem.
gcc $CFLAGS -I "$board/port/nn" -I "$HOST_TEST_SVC" \
    "$here/test_nn_decoder.c" "$board/port/nn/nn_decoder.c" \
    "$HOST_TEST_SVC/blazeface.c" \
    $LDFLAGS -lm -o "$out/test_nn_decoder"
"$out/test_nn_decoder"

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

# issue #130 -- when `nn stream` may give the nn session back
# (port/nn/nn_sess_release.c).  The producer now writes the input tensor, inside
# the arena the session guards, so the release waits for a sink drain that
# confirmed the producer out.  A drain that runs out cannot be typed; the table
# and the four stop / worker interleavings are walked here instead.
gcc $CFLAGS -I "$board/port/nn" \
    "$here/test_nn_sess_release.c" "$board/port/nn/nn_sess_release.c" \
    $LDFLAGS -o "$out/test_nn_sess_release"
"$out/test_nn_sess_release"

# issue #130 step 6c (#122 P8) -- how the resident decoder's answer is counted in
# `nn stream stats` (port/nn/nn_decode_count.c).  BF_ERR_MODEL is the top-5
# result of a classifier stream here and is NOT counted (D6); every other
# negative is a decoder fault -- and none of those can be typed.
gcc $CFLAGS -I "$board/port/nn" -I "$HOST_TEST_SVC" \
    "$here/test_nn_decode_count.c" "$board/port/nn/nn_decode_count.c" \
    $LDFLAGS -o "$out/test_nn_decode_count"
"$out/test_nn_decode_count"

# issue #130 -- negative tests for the tflm residents of cmake/check_f746_layout.py
# (the activation arena and the SD model slots must sit in SDRAM bank3).  Driven
# by fake nm/objdump, so no toolchain is needed: what is checked is the gate's
# DECISION on the exact names board.cmake passes (read from board.cmake, not
# copied), including that a null build is not asked for them.  That the compiler
# spells them that way is the firmware build's job -- the same gate fails there
# on a name that matches nothing.
python3 "$board/cmake/fixtures/run_layout_tests.py"

# issue #130 -- cmake/gen_model_array.py re-verifies the pinned model's SHA256
# on the bytes it emits (the fetch verified it once, earlier), refuses an empty
# or malformed expected hash, and leaves no output behind on a refusal.
python3 "$board/cmake/fixtures/run_gen_model_array_tests.py"

# issue #131 P16 -- this board's model singleton (port/nn/nn.c) over a stub
# backend that keeps the contract nn_backend.h states.  The two properties a
# console cannot pin down on its own: only the first open after boot reaches the
# backend (an `nn info` after `nn model unload` must not put the built-in model
# back), and a reload's answer is whether a MODEL is left -- a refused load from
# empty is EMPTY through the shared table, not PREVIOUS.  The HAL and ThreadX are
# test/nn_shim.
gcc $CFLAGS -I "$here/nn_shim" -I "$board/port/nn" -I "$HOST_TEST_SVC" \
    "$here/test_nn_model.c" "$board/port/nn/nn.c" "$HOST_TEST_SVC/nn_swap.c" \
    $LDFLAGS -o "$out/test_nn_model"
"$out/test_nn_model"
# ...and again with the one adoption failing: the next open must reach the
# backend through a latch that was given back, build nothing and open empty.
gcc $CFLAGS -DTEST_FIRST_BUILD_FAILS -I "$here/nn_shim" -I "$board/port/nn" \
    -I "$HOST_TEST_SVC" \
    "$here/test_nn_model.c" "$board/port/nn/nn.c" "$HOST_TEST_SVC/nn_swap.c" \
    $LDFLAGS -o "$out/test_nn_model_fail"
"$out/test_nn_model_fail"

# issue #131 step 7d -- this board's `nn info` / `nn model load` / `nn model
# unload` (port/nn/nn_svc_f746_model.c) behind the shared order
# (svc/nn_core_model.c), against the real nn.c over a stub backend.  What a
# console cannot produce on demand: a refusal from empty, a backend that
# "restores" from nothing (EMPTY, released), a success that left no model (HW),
# and `nn info` from another console in the middle of a swap (BUSY on all three
# lines).  shell/test/test_nn_core_model.c walks the order itself.
gcc $CFLAGS -I "$here/nn_shim" -I "$board/port/nn" -I "$board/port/camera" \
    -I "$board/port/sdram" -I "$HOST_TEST_SVC" \
    "$here/test_nn_model_life.c" "$board/port/nn/nn_svc_f746_model.c" \
    "$board/port/nn/nn.c" "$HOST_TEST_SVC/nn_core_model.c" \
    "$HOST_TEST_SVC/nn_swap.c" "$HOST_TEST_SVC/fmt.c" \
    $LDFLAGS -o "$out/test_nn_model_life"
"$out/test_nn_model_life"
