#!/usr/bin/env sh
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Board-pinned host tests for Wio Lite AI (issue #7).  These compile code this
# board OWNS (port/flashdb, port/nn, port/mlperf) against the board's REAL
# headers -- which is the point of them: a shimmed copy of mem_sections.h or
# fdb_cfg.h could drift from the firmware's without anything noticing.  They
# cannot live in shell/test, which is board-independent by construction.
#
# Invoked by shell/test/run_host_tests.sh, which owns the toolchain flags and the
# scratch directory and passes them in the environment; there is no separate set
# of flags here, so a board test is built exactly like a core one.  Run the whole
# suite (or `run_host_tests.sh wio-lite-ai` for this board alone) rather than
# executing this file directly.
set -eu

: "${HOST_TEST_OUT:?run via shell/test/run_host_tests.sh}"
: "${HOST_TEST_CFLAGS:?}" "${HOST_TEST_LDFLAGS:?}"
: "${HOST_TEST_INC:?}" "${HOST_TEST_SVC:?}" "${HOST_TEST_REPO:?}"

here=$(cd "$(dirname "$0")" && pwd)
board=$(cd "$here/.." && pwd)
out="$HOST_TEST_OUT"
inc="$HOST_TEST_INC"
svc="$HOST_TEST_SVC"
CFLAGS="$HOST_TEST_CFLAGS"
LDFLAGS="$HOST_TEST_LDFLAGS"
fdb="$HOST_TEST_REPO/lib/flashdb"   # third-party FlashDB (its CRC-32 is used by src/blob.c)

# issue #10 (#9 P2b) -- the CRC-32 the blob region stamps assets with.  app/blob.c
# does not implement one: it reuses FlashDB's fdb_calc_crc32(), which is already in
# the build and already accumulating.  Two properties it relies on are invisible in
# that function's signature -- that starting from 0 gives standard CRC-32/ISO-HDLC
# (so the board and the PC agree) and that feeding the result back in continues the
# same CRC (so a file arriving as ~185 YMODEM blocks lands on the one-call value).
# Pinned here because it is the only part of the blob work verifiable off the board.
# Built against port/flashdb (fdb_cfg.h) exactly as the firmware is; the FlashDB
# sources are third-party, hence the one relaxed warning.
gcc $CFLAGS -Wno-unused-parameter \
    -I "$board/port/flashdb" -I "$fdb/inc" -I "$fdb/port/fal/inc" \
    "$here/test_crc32.c" "$fdb/src/fdb_utils.c" \
    $LDFLAGS -o "$out/test_crc32"
"$out/test_crc32"

# issue #97 -- the adapter onto the SHARED BlazeFace decoder (port/nn/nn_decoder.c).
# The decoder's arithmetic moved to svc/blazeface.c and is covered by
# shell/test/test_blazeface.c, which is board-independent; what stays here is the
# half that cannot be -- nn_tensor -> tensor_desc, against this board's real nn.h.
# struct nn_model is opaque (defined in nn.c), so the test supplies its own
# nn_output_count() / nn_output(), which is also what makes the adapter testable.
#
# The cases are the translation's own failure modes: an unsupported dtype mapped
# onto one the decoder reads, float32 put through the affine form (this board
# publishes scale 0 for an unquantised tensor, so that would zero every value), a
# rank above four truncated into a match, and a hole in the output set reported as
# a model-shape problem.
#
# Built against the REAL boards/<board>/include/mem_sections.h so the PSRAM_AI
# attribute on the host is the same one the firmware uses -- a shimmed copy could
# drift from it without anything noticing, and check_psram_ai_residency.py names
# `nn_dec_scratch` on the linked image as the other end of that.
gcc $CFLAGS -I "$board/include" -I "$board/port/nn" -I "$HOST_TEST_SVC" \
    "$here/test_nn_decoder.c" "$board/port/nn/nn_decoder.c" \
    "$HOST_TEST_SVC/blazeface.c" \
    $LDFLAGS -lm -o "$out/test_nn_decoder"
"$out/test_nn_decoder"

# issue #55 -- the MLPerf Tiny harness (port/mlperf/mlperf_th.cc), driven through
# UPSTREAM'S OWN PARSER (lib/mlperf-tiny/benchmark/api/internally_implemented.cpp,
# unmodified).  So what is under test is the protocol itself, fed the way the host's
# runner feeds it -- `db load N`, 31-byte hex chunks, `infer N W` -- and what is
# asserted is the bytes the board would put on the wire.
#
# It earns its place because all three things this layer can get wrong are SILENT on
# hardware: the per-benchmark input transform (shift by 128 / pass through / quantize
# from float) still produces confident scores when it is wrong, the three-decimal
# formatting is assembled from integers because svc/fmt.c has no %f, and the benchmark
# identification is what decides which test the host runs at all.  None of them
# announce themselves; they come back as accuracy that is quietly a few points low.
#
# Skipped rather than failed when the submodule is absent: it is ~340 MB and only
# fetched for CONFIG_MLPERF_TINY builds, so a plain checkout must still run the suite.
# g++ links it -- upstream's half is C++ and declares no linkage, which is the whole
# reason mlperf_th is C++ too (see its header).
mlperf="$HOST_TEST_REPO/lib/mlperf-tiny/benchmark"
if [ -f "$mlperf/api/internally_implemented.cpp" ]; then
    gcc $CFLAGS -c -I "$svc" "$svc/fmt.c" -o "$out/fmt.o"
    gcc $CFLAGS -I "$board/port/nn" -I "$board/port/mlperf" \
        -c "$here/test_mlperf.c" -o "$out/test_mlperf.o"
    # -std=gnu++17 to match the firmware, which passes no -std and so gets the GNU
    # dialect by default: the test should compile the shared headers the same way the
    # board does.  (This is also where cli_config.h's C11 _Static_assert was caught --
    # GCC only accepts that spelling in C++ from version 14, the ARM toolchain is 15
    # and this host's g++ is 13.  The header now uses CLI_STATIC_ASSERT and works in
    # both, but the dialect match is what made the difference visible.)
    g++ -std=gnu++17 -Wall -Wextra -ffunction-sections -fdata-sections -no-pie \
        -I "$mlperf" -I "$board/port/mlperf" -I "$board/port/nn" \
        -I "$inc" -I "$svc" -I "$board/port/threadx" \
        -fno-exceptions -fno-rtti \
        -include "$board/port/mlperf/mlperf_th.h" \
        "$mlperf/api/internally_implemented.cpp" \
        "$board/port/mlperf/mlperf_th.cc" \
        "$out/test_mlperf.o" "$out/fmt.o" \
        -Wl,--gc-sections -lm -o "$out/test_mlperf"
    "$out/test_mlperf"
else
    echo "test_mlperf: SKIP (lib/mlperf-tiny not checked out)"
fi
