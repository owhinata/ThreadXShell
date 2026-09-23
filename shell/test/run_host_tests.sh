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

# issue #123 -- the agent-instruction files stay invariants-only, and the only
# thing that had ever enforced that was intent.  Both had grown into per-issue
# work logs (1,164 and 1,072 lines) because "CLAUDE.md holds only what must not
# be broken" had no ceiling: every issue closed by adding its measured values and
# its story as though they were rules.  A line budget is a crude proxy for
# "invariants only", but it is one a person notices BEFORE the commit, and the
# choice it forces -- board README for measurements, persistent memory for the
# story -- is exactly the split those files already declare.
#
# The limit is written once, here.  Both counts are printed before anything
# fails, because reporting one file's overflow while hiding the other's would
# send someone back for a second round.
doc_limits="CLAUDE.md:350 AGENTS.md:300"
doc_over=0
for entry in $doc_limits; do
    f=${entry%:*}
    max=${entry#*:}
    n=$(wc -l < "$repo/$f")
    if [ "$n" -gt "$max" ]; then
        echo "doc size: $f $n lines (limit $max) -- OVER" >&2
        doc_over=1
    else
        echo "doc size: $f $n lines (limit $max)"
    fi
done
if [ "$doc_over" -ne 0 ]; then
    echo "run_host_tests: an agent-instruction file is over its line budget." \
         "Measured values belong in boards/<board>/README.md and the story in" \
         "persistent memory; only a new invariant earns a line here." >&2
    exit 1
fi

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

# issue #101 (#78 Step 1a) -- container and manifest validation (svc/plugin_load.c):
# everything that must hold BEFORE a plugin's first instruction is fetched.
#
# [!] EVERY ONE OF THE THIRTY-TWO REFUSAL CODES IS PROVOKED HERE.  plugin_load.h names
# them separately on purpose -- several are ordinary operator mistakes (a container
# built for another board, a stale ABI) that must not read like corruption -- and a
# reason no input can reach is not a reason.  Each case mutates ONE field of an
# otherwise valid container, because a hand-rolled broken buffer can be refused by
# an earlier check and then pass while proving nothing about the check it names.
# Two cases in this file were written wrong in exactly that way and caught by their
# own assertion: a section overlap that ran past the container end, and a
# misaligned model that collided with the next section.
#
# Pure svc layer, so the svc include dir is all it needs.
gcc $CFLAGS -I "$svc" \
    "$here/test_plugin_load.c" "$svc/plugin_load.c" "$svc/crc32.c" \
    $LDFLAGS -o "$out/test_plugin_load"
"$out/test_plugin_load"

# issue #111 -- the plugin's painter and printer veneers (asset/common/
# plugin_base.c).  ABI 2 gave both vtables a version and a size, and every call
# through them now waits on "is this ABI's, and does the size reach this member".
# Board-independent -- every board's plugins link it -- so it runs here: a
# version mismatch, a size one byte short of each member and a size ending
# exactly at it, per member.
gcc $CFLAGS -I "$svc" -I "$repo/asset/common" \
    "$here/test_plugin_veneer.c" "$repo/asset/common/plugin_base.c" \
    $LDFLAGS -o "$out/test_plugin_veneer"
"$out/test_plugin_veneer"

# issue #111 -- `nn info`'s plugin stack lines (svc/plugin_info.c), shared by
# grove-vision-ai-v2 and wio-lite-ai.  Pinned byte for byte: no gate compares
# what a command prints, and the widest line must keep its CRLF.
gcc $CFLAGS -I "$svc" \
    "$here/test_plugin_info.c" "$svc/plugin_info.c" "$svc/fmt.c" \
    $LDFLAGS -o "$out/test_plugin_info"
"$out/test_plugin_info"

# issue #110 (#78 Step 3b) -- the capture of an external decoder's own account
# of its result (svc/nn_report.c).  A contract about OUTCOMES: zero bytes is a
# legal report, "no report to give" is a different answer, truncation is a
# third and a decoder's own refusal a fourth, and a consumer that inferred any
# of them from the length would tell an operator the wrong one.  The last case
# demonstrates the ownership -- two callers, two frames -- because a
# board-owned capture slot, which is the obvious first design and the wrong
# one, passes every other case in the file.
gcc $CFLAGS -I "$svc" -I "$inc" \
    "$here/test_nn_report.c" "$svc/nn_report.c" \
    $LDFLAGS -o "$out/test_nn_report"
"$out/test_nn_report"

# issue #110 (#78 Step 3b) -- the loader that runs AFTER that validation
# (svc/plugin_exec.c), shared by grove-vision-ai-v2 and wio-lite-ai.
#
# What it must get right is an ORDER, and every step of that order is invisible
# from a console: a board can show that a plugin ran, not that the fault
# reporter would have named it had a fault arrived between two particular
# instructions.  The port hooks are the observation points -- each records when
# it was called and what the world looked like from there -- so the rules
# AGENTS.md states about this one mechanism are checked where they are made.
#
# [!] AND THE ENTRY POINT IS REALLY BRANCHED TO.  Testing it with an "entry
# absent" manifest would exercise a shape plugin_parse() refuses to emit and
# leave publish-before-branch checked by nothing, so the image copied in is
# twelve bytes of position-independent machine code that jumps back into the
# test.  That part is host-specific and SKIPs elsewhere; the ordering cases run
# everywhere.
gcc $CFLAGS -I "$svc" \
    "$here/test_plugin_exec.c" "$svc/plugin_exec.c" \
    $LDFLAGS -o "$out/test_plugin_exec"
"$out/test_plugin_exec"

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

# issue #108 -- the plugin image gate is shared, and the three facts that differ
# per board (the reservation it checks against, the forbidden table, the stack
# the base spends behind a veneer) reach it only as add_plugin() arguments.
# Leaving one out must fail at CONFIGURE rather than inherit another board's
# value, so the refusal itself is tested, through a real `cmake` over the real
# helper.  Needs cmake only -- no toolchain -- which is why it runs here.
# Since #112 the same configure also registers the firmware-side veneer cost
# gate (add_plugin() refuses without it) against a dummy executable, so it
# needs the host C compiler too -- still no cross toolchain.
python3 "$repo/cmake/fixtures/run_add_plugin_arg_tests.py"

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

# issue #122 P15 -- a board's `nn` wording is checked against NN_SVC_DETAIL_MAX at
# build time, because the copy into a result truncates and a truncated sentence
# does not look truncated (one of wio's refusals was 164 characters and ended
# "or see `d" on hardware).  The check fires in the COMPILER, so it is tested by
# compiling: the boundary must build and run, and each over-long or non-literal
# form must fail to build with the assertion's own message -- a compile that
# fails for some other reason proves nothing about the gate.
gcc $CFLAGS -I "$svc" "$here/test_nn_detail_check.c" \
    $LDFLAGS -o "$out/test_nn_detail_check"
"$out/test_nn_detail_check"
for neg in NN_DC_OVER_LIT NN_DC_OVER_FMT NN_DC_NOT_LITERAL; do
    if gcc $CFLAGS -D"$neg" -I "$svc" "$here/test_nn_detail_check.c" \
           $LDFLAGS -o "$out/test_nn_detail_check_neg" \
           > "$out/detail_neg.log" 2>&1; then
        echo "test_nn_detail_check: $neg COMPILED -- the length gate did not fire" >&2
        exit 1
    fi
    case "$neg" in
    NN_DC_NOT_LITERAL) want='expected' ;;   # `"" p` is a syntax error, by design
    *)                 want='detail literal is longer than NN_SVC_DETAIL_MAX' ;;
    esac
    if ! grep -q "$want" "$out/detail_neg.log"; then
        echo "test_nn_detail_check: $neg failed to compile, but not on the gate:" >&2
        cat "$out/detail_neg.log" >&2
        exit 1
    fi
    echo "  ok   $neg refused at compile time"
done

# issue #122 -- what `nn model load` prints on a wrong invocation, through the REAL
# dispatcher and the REAL shell/cmds/cmd_nn.c, once per source set a board
# declares.  The line is joined from two halves -- the dispatcher's command path
# and the command's argument spelling -- and it read "usage: nn model load load
# <...>" on every board because the parent and the leaf shared one spelling.
# Neither half is wrong on its own, so only the joined line can be tested.  The
# adapters are stubs (nothing is loaded); nn_cfg/ stands in for a board's
# nn_svc_config.h with the loader on, and -D picks the sources.
for shape in "-DNN_SVC_HAS_MODEL_SLOT=1" \
             "-DNN_SVC_HAS_MODEL_NAME=1 -DNN_SVC_HAS_MODEL_ADDR=1" \
             "-DNN_SVC_HAS_MODEL_PATH=1 -DNN_SVC_HAS_MODEL_BUILTIN=1"; do
    # -Wno-unused-function: the stub config has no camera or bench, so the
    # cancel shim they share is unused here and nowhere on a real board.
    # shellcheck disable=SC2086  # $shape is deliberately word-split
    gcc $CFLAGS -Wno-unused-function -DCLI_USE_COLOR=0 $shape \
        $glue_inc -I "$here/shim" -I "$here/nn_cfg" -I "$inc" -I "$core" \
        -I "$here/../cmds" -I "$svc" \
        "$here/test_nn_cmd_usage.c" "$here/../cmds/cmd_nn.c" \
        "$here/../cmds/nn_cmd_core.c" \
        "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
        "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
        $glue \
        $LDFLAGS -o "$out/test_nn_cmd_usage"
    "$out/test_nn_cmd_usage"
done

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
