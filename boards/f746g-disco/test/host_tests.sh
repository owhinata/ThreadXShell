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
