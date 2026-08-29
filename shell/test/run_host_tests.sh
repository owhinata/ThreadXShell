#!/usr/bin/env sh
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Host smoke/unit tests for the Shell core, built and run with the host gcc.
# The Shell core (shell/core, shell/include, shell/backend) and svc/fmt are a
# verbatim port from stm32f746g-disco, so these HW-independent tests run
# unchanged.  Each test links shell/test/host_sections.ld, which supplies the
# .shell_root_cmds section + boundary symbols that the target ldscript
# (ldscript/STM32H725AEIx_IROM.ld) provides on hardware.  No firmware build is
# involved -- this runs on the build host, not the board.
#
# svc/ymodem.c came over with owhinata/wio-lite-ai#19 M4 (the RTL8720DN flash backup streams
# over the console with YMODEM), so its test is ported too.  (The donor's
# frame_pipeline test covers a camera module that has no counterpart here.)
#
# Usage: run_host_tests.sh [board ...]
#
# Everything in THIS file is board-independent (shell/ + svc/ only) and always
# runs.  A test that compiles board-owned code against the REAL board headers --
# which is the point of such a test, since a shimmed copy could drift -- belongs
# to that board and lives in boards/<board>/test/host_tests.sh, sourced below.
# With no argument every board that has one runs; name boards to narrow it down.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
inc="$here/../include"
core="$here/../core"
svc="$here/../../svc"       # freestanding service layer (fmt.c / fmt.h)
backend="$here/../backend"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Board selection: named boards, or every board that owns host tests.
#
# What decides "owns host tests" is the boards/<board>/test DIRECTORY, not the
# host_tests.sh inside it.  Keying off the script would make this suite fail open:
# rename or delete one board's dispatcher and its tests stop running while the run
# still ends in "host tests passed" -- less coverage, same green.  A board with
# test sources but no dispatcher is therefore an error, and a board with no test
# directory at all genuinely pins nothing (all three have one as of issue #47).
boards=""
if [ $# -gt 0 ]; then
    for b in "$@"; do
        if [ ! -d "$repo/boards/$b" ]; then
            echo "run_host_tests: no such board '$b'; available:" >&2
            ls "$repo/boards" >&2
            exit 1
        fi
        boards="$boards $b"
    done
else
    for d in "$repo"/boards/*/test; do
        [ -d "$d" ] || continue
        boards="$boards $(basename "$(dirname "$d")")"
    done
fi

for b in $boards; do
    if [ -d "$repo/boards/$b/test" ] && [ ! -f "$repo/boards/$b/test/host_tests.sh" ]; then
        echo "run_host_tests: boards/$b/test exists but has no host_tests.sh --" \
             "its tests would silently not run" >&2
        exit 1
    fi
done

# Flags mirror the target link so the tests exercise the real retention path:
#   -ffunction-sections -fdata-sections + -Wl,--gc-sections : same GC as the
#       firmware; proves `used` + linker KEEP keep the (otherwise unreferenced)
#       command entries from being garbage-collected.
#   -no-pie : resolve the const command/pointer table absolutely at link time so
#       it stays read-only without runtime text relocations (target firmware is
#       linked absolute/static, so this only matters on the host).
CFLAGS="-std=c11 -Wall -Wextra -ffunction-sections -fdata-sections -no-pie"
LDFLAGS="-Wl,--gc-sections -Wl,-T,$here/host_sections.ld"

# command registration foundation.
gcc $CFLAGS -I "$inc" \
    "$here/test_registration.c" \
    $LDFLAGS -o "$out/test_registration"
"$out/test_registration"

# command-line parser.  cli_parse.c and the test share one compile so the small
# CLI_MAX_ARGC / CLI_MAX_SUBCMD_DEPTH overrides (used to exercise the token-limit
# and nesting-limit paths with a compact tree) apply consistently.
gcc $CFLAGS -DCLI_MAX_ARGC=8 -DCLI_MAX_SUBCMD_DEPTH=2 \
    -I "$inc" -I "$core" \
    "$here/test_parse.c" "$core/cli_parse.c" \
    $LDFLAGS -o "$out/test_parse"
"$out/test_parse"

# Shared host pieces for the core / output / integration tests: the dummy
# (loopback) backend and the ThreadX-free glue (no-op lock/notify, a faithful
# cli_tx_send_blocking over tr->api->write, and the RX pump).  Found via the test
# dir (host_glue.h) and the backend dir (cli_backend_dummy.h).
glue="$backend/cli_backend_dummy.c $here/host_glue.c"
glue_inc="-I $here -I $backend"

# shell core: ASCII filter, RX state machine, dispatch, fail-safe.  cli_session.c /
# cli_edit.c are ThreadX-free (the tx_* glue lives in cli_core.c, firmware only),
# so they build on the host against the tx_api.h shim in test/shim, placed first on
# the include path.  Output + tx_* glue route through the shared dummy backend.
gcc $CFLAGS -DCLI_CMD_BUFFER_SIZE=16 -DCLI_MAX_ARGC=4 -DCLI_MAX_SUBCMD_DEPTH=2 \
    -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_core.c" "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_core"
"$out/test_core"

# output API: minimal formatter, 32 B staging + autoflush, VT100 colour, hexdump,
# TX-failure drop/return.  cli_printf.c is ThreadX-free; colour ON (default) and
# the real 32 B CLI_PRINTF_BUFFER_SIZE so the SGR escapes and autoflush are
# exercised.  cli_session.c is linked for the cancel helpers.
gcc $CFLAGS \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_output.c" "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_session.c" \
    $glue \
    $LDFLAGS -o "$out/test_output"
"$out/test_output"

# dummy backend end-to-end: input -> execute -> output driven THROUGH the transport
# (cli_dummy_inject -> read() -> state machine -> write() -> capture), flow control
# (backpressure completes / timeout drops / immediate fail), abnormal cases and
# multi-instance isolation.  Small CLI_* limits + colour OFF as for the core test.
gcc $CFLAGS -DCLI_CMD_BUFFER_SIZE=16 -DCLI_MAX_ARGC=4 -DCLI_MAX_SUBCMD_DEPTH=2 \
    -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_integration.c" "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_integration"
"$out/test_integration"

# line editor: cursor model (cur split from len), in-line insert/overwrite/delete,
# meta keys (Ctrl+a/b/d/e/f/k/u/w, Alt+b/f, Ctrl+l), VT100 escapes (arrows / Home /
# End / Del / Insert / SS3), invalid-escape ignore, the CPR width probe + guarded
# reply, and wrap redraw at a forced small term_width.  Drives cli_input_byte
# directly (model assertions) so it needs no backend.  Colour OFF.
gcc $CFLAGS -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_edit.c" "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_edit"
"$out/test_edit"

# command history fixed ring: add + recall (arrows / Ctrl+p,n), consecutive-
# duplicate suppression, non-consecutive duplicates kept, FIFO eviction at the byte
# cap, empty lines skipped, navigation-state reset on submit / Ctrl+C / blank
# re-submit, no-draft-restore, and per-instance isolation.  A small 32 B
# CLI_HISTORY_BUFFER_SIZE forces eviction with a few short entries; colour OFF.
gcc $CFLAGS -DCLI_USE_COLOR=0 -DCLI_HISTORY_BUFFER_SIZE=32 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_history.c" "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_history"
"$out/test_history"

# UART backend byte ring: the pure, lock-free FIFO helpers (cli_uart_ring.h) that
# the ring-buffered backends layer RX/TX on.  HAL/ThreadX-free, so it builds with
# the host gcc and needs no shim -- only the backend include dir for the header.
gcc $CFLAGS -I "$backend" \
    "$here/test_uart_ring.c" \
    $LDFLAGS -o "$out/test_uart_ring"
"$out/test_uart_ring"

# Registry slot rules (issue #81): which registrations are refused and why, with
# the refusal reason decided from the whole table in a fixed order, and the
# removal sweep whose postcondition is "no entry has this thread".  Both are
# unreachable from healthy firmware -- a test is the only thing that can build
# the damaged table -- which is exactly why they live in cli_registry.c rather
# than cli_core.c (that TU needs ThreadX and an MRS on IPSR, so it cannot build
# on the host at all).
gcc $CFLAGS \
    -I "$here/shim" -I "$inc" -I "$core" \
    "$here/test_registry.c" "$core/cli_registry.c" \
    $LDFLAGS -o "$out/test_registry"
"$out/test_registry"

# Console-counter snapshot (issue #28): which registry entries count as a running
# interactive console, and how a too-small caller array is reported.  Same TU as
# above; the TX_DISABLE wrappers (cli_core.c) and the `console` command itself
# are firmware-only, as cmd_thread.c is.
gcc $CFLAGS \
    -I "$here/shim" -I "$inc" -I "$core" \
    "$here/test_console.c" "$core/cli_registry.c" \
    $LDFLAGS -o "$out/test_console"
"$out/test_console"

# Tab completion: word boundary + read-only token walk (command-set resolution),
# prefix scan with longest-common-prefix tracking, single-candidate complete +
# trailing space, bash-style two-stage candidate list, BEL on no match / argument
# territory, and the buffer-full guard.  Drives cli_input_byte (Tab=0x09) +
# cli_tab_complete directly and asserts the model + captured output.  Colour OFF.
gcc $CFLAGS -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_complete.c" "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_complete"
"$out/test_complete"

# Tab completion (buffer-full): same as above but a tiny CLI_CMD_BUFFER_SIZE so
# completion that would overflow the line rings BEL and leaves the line unchanged,
# and an LCP-extend that cannot fit still reaches the two-stage list on the next Tab.
gcc $CFLAGS -DCLI_USE_COLOR=0 -DCLI_CMD_BUFFER_SIZE=8 -DTEST_COMPLETE_SMALL_BUF \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_complete.c" "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_complete_smallbuf"
"$out/test_complete_smallbuf"

# owhinata/wio-lite-ai#19 M4 -- clean-room YMODEM-CRC sender (svc/ymodem.c): CRC-16/CCITT
# vectors, block framing (block 0 name+size, STX/SOH, 0x1A short-block padding, seq/~seq, CRC),
# NAK resend, CAN abort + teardown, seq wrap mod 256, and a 1-byte-at-a-time
# source filling full blocks.  Pure svc layer -- HAL/ThreadX/shell-free, so it
# builds with the host gcc and needs only the svc include dir for the header.
gcc $CFLAGS -I "$svc" \
    "$here/test_ymodem.c" "$svc/ymodem.c" \
    $LDFLAGS -o "$out/test_ymodem"
"$out/test_ymodem"

# owhinata/wio-lite-ai#19 M5 -- the YMODEM RECEIVER (ymodem_recv), which is what lets the board
# take a firmware image FROM the PC (and therefore what makes the stock backup restorable).
# Two harnesses: a pthread duplex loopback that runs ymodem_send() and ymodem_recv()
# against each other through blocking FIFOs -- the only way to exercise the real
# handshake, since the sender only advances on the receiver's 'C'/ACKs -- and scripted
# transcripts for the error paths (CRC damage, duplicate/out-of-order blocks, a sink
# that refuses the file, a batch that never closes, short-block trimming).
gcc $CFLAGS -I "$svc" \
    "$here/test_ymodem_recv.c" "$svc/ymodem.c" \
    $LDFLAGS -pthread -o "$out/test_ymodem_recv"
"$out/test_ymodem_recv"

# owhinata/wio-lite-ai#8 phase 3a -- camera frame pipeline core (svc/frame_pipeline.c): ring
# slot acquire/publish, refcount pin/put, DROP/LATEST policy + pending transfer, detach
# in-flight count, read_latest generation, and an N=4 ring cycling under a counting
# sink.  Pure svc layer -- HAL/ThreadX/shell-free, and the mutual exclusion it needs
# is injected (struct frame_os), so the whole engine runs on the host with a no-op
# lock.  This is the only part of the camera work that can be verified without the
# board, which is exactly why it is ported byte-identical from the f746 firmware.
gcc $CFLAGS -I "$svc" \
    "$here/test_frame_pipeline.c" "$svc/frame_pipeline.c" \
    $LDFLAGS -o "$out/test_frame_pipeline"
"$out/test_frame_pipeline"

# issue #92 (#49 Step 2) -- the CRC-32 the Grove blob store stamps assets with
# (svc/crc32.c): canonical CRC-32/ISO-HDLC vectors, chaining at every split point,
# a cls-model-sized stream accumulated in 1024-byte YMODEM blocks (bit-sensitive),
# the double-inversion wrapper that must NOT be re-added, and agreement with a
# table-free reference computed straight from the polynomial -- the last is what
# keeps the 64-byte table honest, since the test shares no table with the code.
# Pure svc layer -- HAL/ThreadX/shell-free, so it needs only the svc include dir.
# (Built as test_crc32_svc: boards/wio-lite-ai/test pins the same properties for
# FlashDB's fdb_calc_crc32(), which is what the donor's blob uses, and that one
# already owns the name test_crc32 in this scratch dir.)
gcc $CFLAGS -I "$svc" \
    "$here/test_crc32.c" "$svc/crc32.c" \
    $LDFLAGS -o "$out/test_crc32_svc"
"$out/test_crc32_svc"

# issue #97 -- the shared BlazeFace decoder (svc/blazeface.c), folded from three
# diverged board copies.  The REAL decoder is compiled here against synthetic
# tensors, which it permits because it takes descriptors (svc/tensor.h) rather
# than reaching into any inference singleton -- the arrangement that lets a
# decode bug be found without spending a flash cycle on a board.  Absorbs the
# assertions of all three tests it replaced, and runs the type-independent ones
# against BOTH int8 (Grove) and float32 (wio, f746): per-tensor dequantisation,
# the anchor grid, lookup by shape, the candidate cap not truncating the scan
# (issue #47), NMS, the distinct failure codes (issue #57), and init validation.
# Pure svc layer -- HAL/ThreadX/shell-free, so it needs only the svc include dir.
gcc $CFLAGS -I "$svc" \
    "$here/test_blazeface.c" "$svc/blazeface.c" \
    $LDFLAGS -o "$out/test_blazeface_svc"
"$out/test_blazeface_svc"

# issue #97 -- negative tests for cmake/check_no_mutable_storage.py, the gate that
# keeps svc/blazeface.c from growing state of its own (each board passes in its
# own scratch so that it keeps its own placement and its own residency gate).
# Four fixtures: clean, a plain static, thread-local storage and anonymous
# writable bytes from inline asm -- the last two are why the gate measures
# SECTIONS and not symbols, since neither is an STT_OBJECT.
# [!] The fifth, `target_only`, PASSES here on purpose: it puts its static behind
# `#if defined(__arm__)`, so a host-side run cannot see it.  That is the whole
# reason the gate itself is wired per board against the cross compiler rather
# than once here.  The boards run this same script with their own toolchain.
python3 "$repo/cmake/fixtures/run_storage_gate_tests.py" \
    --cc gcc --objdump objdump --nm nm

# issue #97 -- the published decode record (svc/nn_det_record.c).  A camera stream
# decodes on a worker and a console prints on another thread, so what the two
# exchange has a rule: the boxes and the diagnostics that describe them travel
# together, and a decode that outlived its session lands nowhere.
#
# [!] THE SECOND HALF IS WHY THIS FILE EXISTS.  Stopping a stream cannot cancel an
# inference already running -- it can only wait, and the wait is bounded -- so
# "stop clears the record, the old inference finishes and publishes" is reachable
# on hardware and would resurrect a stopped session's boxes.  It cannot be
# INJECTED there, though, so the interleavings are written out by hand here; a
# test that ran a stream and looked for stale boxes would pass almost always and
# prove nothing.  Pure svc layer, so it needs only the svc include dir.
gcc $CFLAGS -I "$svc" \
    "$here/test_nn_det_record.c" "$svc/nn_det_record.c" \
    $LDFLAGS -o "$out/test_nn_det_record"
"$out/test_nn_det_record"

# issue #50 -- the pure parts of the ONE shared `nn` command (shell/cmds).  Two
# things live here that a console cannot check:
#
#   the scaled-integer number formatting, because this firmware has no %f and the
#   form it replaces printed 1.5 as "0.1500000" -- silently wrong for every
#   quantisation scale >= 1, which output tensors routinely have, and plausible
#   enough either way that nobody noticed by reading it;
#
#   the model-source grammar, whose whole point is that a BARE string is refused.
#   The same word means a blob name on one board, a path on another and nothing
#   on a third, so a parser that accepted one would have to guess per board --
#   board knowledge back inside shell/ by a quieter route.  The refusal is the
#   assertion someone deletes to make the command friendlier.
#
# Links cli_parse.c for cli_parse_u32(): the grammar uses this firmware's own
# number parser rather than growing a second one.
gcc $CFLAGS -I "$inc" -I "$here/../cmds" -I "$core" -I "$svc" \
    "$here/test_nn_cmd_core.c" "$here/../cmds/nn_cmd_core.c" "$core/cli_parse.c" \
    $LDFLAGS -o "$out/test_nn_cmd_core"
"$out/test_nn_cmd_core"

# issue #99 -- the shared stream lifecycle (svc/nn_stream_life.c).
#
# A `--frames` waiter and a second console racing over one stream: the waiter
# must never tear down a stream it did not start.  The generation alone does not
# close that -- two callers can both be admitted unless the stop transition is
# CLAIMED in the same breath, which is the defect the adversarial review of this
# issue found on two of the three boards.  None of it can be typed: the window is
# between two statements of another thread, and the board where it matters most
# has one console whose background jobs run below the foreground shell under
# TX_NO_TIME_SLICE.  One machine, one test.
gcc $CFLAGS -I "$inc" -I "$svc" \
    "$here/test_nn_stream_life.c" "$svc/nn_stream_life.c" \
    $LDFLAGS -o "$out/test_nn_stream_life"
"$out/test_nn_stream_life"

# ---- board-pinned tests --------------------------------------------------- *
# Same toolchain flags and the same scratch dir, exported so a board test is built
# exactly like a core one and cannot quietly diverge.  A board with no
# test/host_tests.sh simply has none -- that is reported, not an error, so the
# suite stays green on a board that pins nothing.
export HOST_TEST_REPO="$repo"
export HOST_TEST_OUT="$out"
export HOST_TEST_CFLAGS="$CFLAGS"
export HOST_TEST_LDFLAGS="$LDFLAGS"
export HOST_TEST_INC="$inc"
export HOST_TEST_CORE="$core"
export HOST_TEST_SVC="$svc"
export HOST_TEST_BACKEND="$backend"
export HOST_TEST_SHELL_TEST="$here"

for b in $boards; do
    script="$repo/boards/$b/test/host_tests.sh"
    if [ -f "$script" ]; then
        echo "--- board tests: $b"
        sh "$script"
    else
        echo "--- board tests: $b (none)"
    fi
done

echo "host tests passed"
