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

# issue #47 -- the BlazeFace decoder (port/nn/models/blazeface.c): SSD anchor decode,
# the bounded top-N candidate list and NMS.  This is the one piece of arithmetic on
# this board whose failure is silent, and it is also the one that CANNOT be exercised
# by the default build: CONFIG_NN_BACKEND=null links the decoder but publishes tensors
# it does not recognise, so blazeface_decode() returns -1 and touches nothing.  Running
# it on hardware needs a non-default backend AND a generated model; the arithmetic is
# therefore pinned here, where the expected box is computed by hand.
#
# The decoder depends on nn.h alone (no HAL, no ThreadX, no libm) and struct nn_model
# is opaque there, so the test supplies its own nn_output_count() / nn_output() and
# runs the real decoder unmodified.  Built against the REAL port/nn headers.
gcc $CFLAGS -I "$board/port/nn" -I "$board/port/nn/models" \
    "$here/test_blazeface.c" "$board/port/nn/models/blazeface.c" \
    $LDFLAGS -lm -o "$out/test_blazeface"
"$out/test_blazeface"

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
