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
        # A tab-completed "grove-vision-ai-v2/" names the same board.  Without
        # this the name still passed the directory test below but matched no
        # per-board rule further down (issue #204).
        b=${b%/}
        case "$b" in
            ""|*/*) echo "run_host_tests: '$b' is not a board name" >&2; exit 1 ;;
        esac
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
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
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
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_output.c" "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_session.c" \
    $glue \
    $LDFLAGS -o "$out/test_output"
"$out/test_output"

# issue #229 -- the console hand-over of a binary transfer (cli_console_claim /
# cli_console_release, cli_printf.c) over the host output lock, which counts its
# takes and releases and can be made to fail: the mark follows the claim, a
# background job is refused, a lock that cannot be taken leaves nothing behind.
gcc $CFLAGS -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" \
    "$here/test_console_claim.c" "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_session.c" \
    $glue \
    $LDFLAGS -o "$out/test_console_claim"
"$out/test_console_claim"

# dummy backend end-to-end: input -> execute -> output driven THROUGH the transport
# (cli_dummy_inject -> read() -> state machine -> write() -> capture), flow control
# (backpressure completes / timeout drops / immediate fail), abnormal cases and
# multi-instance isolation.  Small CLI_* limits + colour OFF as for the core test.
gcc $CFLAGS -DCLI_CMD_BUFFER_SIZE=16 -DCLI_MAX_ARGC=4 -DCLI_MAX_SUBCMD_DEPTH=2 \
    -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
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
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
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
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
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
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_complete.c" "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_complete"
"$out/test_complete"

# Tab completion (buffer-full): same as above but a tiny CLI_CMD_BUFFER_SIZE so
# completion that would overflow the line rings BEL and leaves the line unchanged,
# and an LCP-extend that cannot fit still reaches the two-stage list on the next Tab.
gcc $CFLAGS -DCLI_USE_COLOR=0 -DCLI_CMD_BUFFER_SIZE=8 -DTEST_COMPLETE_SMALL_BUF \
    $glue_inc -I "$here/shim" -I "$inc" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
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
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
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
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_ymodem_recv.c" "$svc/ymodem.c" \
    $LDFLAGS -pthread -o "$out/test_ymodem_recv"
"$out/test_ymodem_recv"

# owhinata/wio-lite-ai#8 phase 3a -- camera frame pipeline core (svc/camera/frame_pipeline.c): ring
# slot acquire/publish, refcount pin/put, DROP/LATEST policy + pending transfer, detach
# in-flight count, read_latest generation, and an N=4 ring cycling under a counting
# sink.  Pure svc layer -- HAL/ThreadX/shell-free, and the mutual exclusion it needs
# is injected (struct frame_os), so the whole engine runs on the host with a no-op
# lock.  This is the only part of the camera work that can be verified without the
# board, which is exactly why it is ported byte-identical from the f746 firmware.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_frame_pipeline.c" "$svc/camera/frame_pipeline.c" \
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
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_crc32.c" "$svc/crc32.c" \
    $LDFLAGS -o "$out/test_crc32_svc"
"$out/test_crc32_svc"

# issue #92 (#49 Step 2), shared since issue #192 -- the blob header codec and
# the transfer's decisions (svc/blob/blob_state.c).  A body whose CRC is wrong, a
# header that names a slot it is not in, a sender that closes the batch short of
# what block 0 declared, a program that failed in the middle of a 1.7 MB
# payload: staging any of those on a board costs flash cycles (on
# grove-vision-ai-v2, of a part whose endurance is not documented, issue #89),
# and most cannot be staged at all without a PC that misbehaves on demand.  What
# they decide is destructive -- a header that decodes when it should not is a
# `blob write` aimed at the wrong slot.  The header's page is the format's own
# constant, so no board header is included; linked with svc/crc32.c, which is
# what the body checksum is.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_blob_state.c" "$svc/blob/blob_state.c" "$svc/crc32.c" \
    $LDFLAGS -o "$out/test_blob_state"
"$out/test_blob_state"

# issue #92 (#49 Step 2), shared since issue #192 -- the rules an asset slot
# table has to satisfy and the lookups over one (svc/blob/blob_map.c).  A
# firmware ships one table, so every table that breaks a rule -- a base half a
# unit off the erase grid, a slot running past the writable interval, two slots
# claiming the same flash -- exists here or nowhere; on a board the seam bounds
# the WRITER and not the table, so an overlapping slot would erase a neighbour's
# payload with nothing out of the ordinary happening.  The test defines its own
# small table under the fixed names the lookups read; a board's shipped table is
# pinned by that board's own test (grove-vision-ai-v2: test/test_blob_map.c).
# Built as test_blob_map_svc: that board's test owns test_blob_map in this
# scratch dir.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_blob_map.c" "$svc/blob/blob_map.c" \
    $LDFLAGS -o "$out/test_blob_map_svc"
"$out/test_blob_map_svc"

# issue #194 -- the results every board's blob store answers
# (svc/blob/blob_err.h), pinned by value and checked pairwise distinct.  A
# renumbering would compile and pass every test that compares by name, while
# the boards that print a result raw would start saying something else.
gcc $CFLAGS -I "$svc/blob" "$here/test_blob_err.c" $LDFLAGS -o "$out/test_blob_err"
"$out/test_blob_err"

# issue #92 (#49 Step 2), shared since issue #192 -- the `blob write` coordinator
# (svc/blob/blob_write.c).  A write holds the board writer's reservation and the
# console claim at once and there are nine ways to leave; each has to give back
# exactly what it took.  A board can stage perhaps two of them, each costs flash
# cycles (on grove-vision-ai-v2, of a part whose endurance is not documented,
# issue #89), and a leaked reservation does not announce itself -- it turns the
# next reader into "busy" for the rest of the session, by which time the cause
# is gone.  So the coordinator takes its operations as a vtable, and this fails
# each in turn and counts the unwinding.  It drives the coordinator's own sink
# as well, which is how a rejected block 0, a short file and an empty batch
# happen without a PC.  The staging buffer is the caller's argument, so the test
# hands over its own (one of 64 KB, one of exactly one page, and ones the
# coordinator must refuse before taking anything) and links no board file.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_blob_write.c" "$svc/blob/blob_write.c" \
    "$svc/blob/blob_state.c" "$svc/crc32.c" \
    $LDFLAGS -o "$out/test_blob_write"
"$out/test_blob_write"

# issue #214 -- the key-value store's record format and its key and value checks
# (svc/kv/kv_record.c, moved out of wio-lite-ai's src/kv.c when the store became
# shared): the encoder byte for byte (records already on a board's flash must keep
# decoding), every field the decoder refuses on its own, the description bounded
# before the value length is derived from it, both maxima in and one more out, and
# the key alphabet and length.
gcc $CFLAGS -I "$svc/kv" \
    "$here/test_kv_record.c" "$svc/kv/kv_record.c" \
    $LDFLAGS -o "$out/test_kv_record"
"$out/test_kv_record"

# issue #214 -- the store itself (svc/kv/kv.c) running the real FlashDB and FAL
# under the real svc/kv/fdb_cfg.h, on a flash model in RAM (kv_cfg/fal_cfg.h is
# its device and partition table, with a neighbour on each side): no program onto
# a byte that is not 0xFF, nothing outside the partition, every device access
# under a hold, every hold given back, the board's hold answers passed through
# with the device untouched, a failed FlashDB lock latching the store offline, a
# corrupt partition left alone, and the lines reported through the board's report
# function.  FlashDB is a submodule (lib/flashdb, fetched by configuring a board
# that has a store); without it this test cannot run, and says so by failing
# rather than passing without it.  Its own sources are third-party and printf
# their size_t with %u, hence the relaxed warnings on them alone.
fdb="$repo/lib/flashdb"
if [ ! -f "$fdb/src/fdb_kvdb.c" ]; then
    echo "run_host_tests: lib/flashdb is not fetched, so test_kv_store cannot" \
         "run -- configure wio-lite-ai once (it fetches the submodule)" >&2
    exit 1
fi
mkdir -p "$out/kv_fdb"
for kv_src in src/fdb.c src/fdb_kvdb.c src/fdb_utils.c \
         port/fal/src/fal.c port/fal/src/fal_flash.c port/fal/src/fal_partition.c; do
    gcc $CFLAGS -Wno-unused-parameter -Wno-format \
        -I "$svc/kv" -I "$here/kv_cfg" -I "$fdb/inc" -I "$fdb/port/fal/inc" \
        -c "$fdb/$kv_src" -o "$out/kv_fdb/$(basename "$kv_src" .c).o"
done
gcc $CFLAGS -I "$svc/kv" -I "$here/kv_cfg" -I "$fdb/inc" -I "$fdb/port/fal/inc" \
    "$here/test_kv_store.c" "$svc/kv/kv.c" "$svc/kv/kv_record.c" \
    "$out/kv_fdb"/*.o \
    $LDFLAGS -o "$out/test_kv_store"
"$out/test_kv_store"

# issue #214 -- the shared `kv` command (shell/cmds/cmd_kv.c) through the REAL
# dispatcher over a stand-in store defined by the test (kv_type_name() is the real
# svc/kv/kv_record.c), with made-up board words (kv_cfg/kv_svc_config.h).  No
# gate compares what a command prints, so its lines are pinned here: the one
# table of secret keys (`kv list` hides wifi.psk, `kv get` prints it), each
# type's text to stored bytes and every refusal before the store is called, the
# joined description, the state gate, every store result's sentence, `kv info`
# and `kv format` with the board's words.
gcc $CFLAGS -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$here/kv_cfg" \
    -I "$inc" -I "$core" -I "$here/../cmds" -I "$svc" -I "$svc/kv" \
    "$here/test_kv_cmd.c" "$here/../cmds/cmd_kv.c" "$svc/kv/kv_record.c" \
    "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_kv_cmd"
"$out/test_kv_cmd"

# issue #214 -- the shared boot-time configuration steps (shell/cmds/kv_cfg.c:
# open the store, the telnet port, net.mode) over a stand-in store, address
# operations and telnet console defined by the test (cli_parse_ipv4() is the real
# one), once with the board default NONE (wio-lite-ai's) and once with DHCP.
# The port handed over exactly once on every path, what each missing, mistyped
# or out-of-range key leads to, the 30 s DHCP wait, the static refusals and the
# fall-back to the default, and every reported line word for word.
for kv_dflt in none dhcp; do
    kv_def=""
    [ "$kv_dflt" = dhcp ] && kv_def="-DKV_CFG_TEST_DEFAULT_DHCP"
    gcc $CFLAGS $kv_def -I "$here/shim" -I "$here/nx_shim" -I "$here/../cmds" \
        -I "$svc/net" -I "$inc" -I "$backend" -I "$core" -I "$svc/kv" \
        "$here/test_kv_cfg.c" "$here/../cmds/kv_cfg.c" "$core/cli_parse.c" \
        $LDFLAGS -o "$out/test_kv_cfg_$kv_dflt"
    "$out/test_kv_cfg_$kv_dflt"
done

# issue #101 (#78 Step 1a) -- container and manifest validation (svc/plugin/plugin_load_body.h):
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
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_load.c" "$svc/plugin/plugin_load.c" "$svc/plugin/plugin_load_host.c" "$svc/crc32.c" \
    $LDFLAGS -o "$out/test_plugin_load"
"$out/test_plugin_load"

# issue #111 -- the plugin's painter and printer veneers (asset/common/
# plugin_base.c).  ABI 2 gave both vtables a version and a size, and every call
# through them now waits on "is this ABI's, and does the size reach this member".
# Board-independent -- every board's plugins link it -- so it runs here: a
# version mismatch, a size one byte short of each member and a size ending
# exactly at it, per member.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" -I "$repo/asset/common" \
    "$here/test_plugin_veneer.c" "$repo/asset/common/plugin_base.c" \
    $LDFLAGS -o "$out/test_plugin_veneer"
"$out/test_plugin_veneer"

# issue #111 -- `nn info`'s plugin stack lines (svc/plugin/plugin_info.c), shared by
# grove-vision-ai-v2 and wio-lite-ai.  Pinned byte for byte: no gate compares
# what a command prints, and the widest line must keep its CRLF.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_info.c" "$svc/plugin/plugin_info.c" "$svc/fmt.c" \
    $LDFLAGS -o "$out/test_plugin_info"
"$out/test_plugin_info"

# issue #159 -- the open model's container claims and the `nn info` lines around
# those stack lines (svc/plugin/plugin_claims), shared by wio-lite-ai and
# f746g-disco, and grove-vision-ai-v2 since issue #174: a counter that is odd,
# or not the value `nn info`'s copy belongs to, or a copy with no value, is BUSY
# whatever is stored (issue #199), and each state's lines, with the board's own "from" words passed
# through -- including for a model-only container, whose from-line is where
# f746g-disco repeats the CRC its load printed.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_claims.c" "$svc/plugin/plugin_claims.c" \
    "$svc/plugin/plugin_info.c" "$svc/fmt.c" \
    $LDFLAGS -o "$out/test_plugin_claims"
"$out/test_plugin_claims"

# issue #174 -- the same claims under the shared load order (svc/nn/nn_core_model.c
# + nn_swap.c): after every ending a load or an unload can have (loaded, the
# previous model restored, nothing left, the plugin refused, bare, the lookup or
# the lease refused), the claims are the ACTUALLY open model's, and a reader that
# copies the counter and the store in one critical section at any hook sees
# either BUSY or the open model's claims -- and BUSY whenever the counter has
# moved since the `nn info` copy it is checked against (issue #199).  A refused
# swap and a refused plugin cannot be typed.
gcc $CFLAGS -I "$inc" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_claims_order.c" "$svc/nn/nn_core_model.c" "$svc/nn/nn_swap.c" \
    $LDFLAGS -o "$out/test_plugin_claims_order"
"$out/test_plugin_claims_order"

# issue #174 -- `nn thresh` on all three boards and `nn info`'s model section on
# wio-lite-ai and f746g-disco (svc/nn/nn_svc_common.c).  The order on every exit
# (count in -> lease -> entry -> lease back -> count out on grove-vision-ai-v2,
# lease -> entry -> lease back on the other two), every answer of the decoder's
# entries, and when an `nn info` copy stands: a timed-out lease, a count refused
# by another console's load and a load between the two counter reads cannot be
# typed.  The inference API is stubbed.
gcc $CFLAGS -I "$inc" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_svc_common.c" "$svc/nn/nn_svc_common.c" "$svc/nn/nn_core_model.c" \
    "$svc/nn/nn_swap.c" $LDFLAGS -o "$out/test_nn_svc_common"
"$out/test_nn_svc_common"

# issue #103 -- is the plugin reservation executable?  (svc/plugin/plugin_mpu.c, the
# Armv8-M verdict grove-vision-ai-v2 links)
#
# The loader reads the MPU back before it branches into a loaded image, because
# the vendor's enable_XIP() reconfigures the MPU; but a board on which the reservation is
# Device memory, or covered by two regions, or execute-never, is not a board
# this project can arrange to have.  Every refusal in that check is therefore
# unreachable from hardware, and this file is the only thing that can see one.
#
# Two cases here exist because of mistakes made next door: the RLAR limit
# includes its last 32-byte block (port/nor/nor_flash.c's diagnostic capture
# compares against the masked value and so reads every region 31 bytes short),
# and "not Device" is not the same question as "Normal" -- outer set with inner
# zero is a RESERVED encoding that an inequality would wave through.
#
# And one case exists because of a mistake made here: until issue #114 the judge
# clamped MPU_TYPE.DREGION to the table it was handed, and this file expected
# that to come out OK.  test_truncated() now holds the same snapshot read short
# and read whole -- refused, and a fault.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_mpu.c" "$svc/plugin/plugin_mpu.c" \
    $LDFLAGS -o "$out/test_plugin_mpu"
"$out/test_plugin_mpu"

# issue #110 (#78 Step 3b) -- the Armv7-M MPU verdict wio-lite-ai links
# (svc/plugin/plugin_mpu_v7m.c), read back immediately before the loader
# branches into a plugin image.
#
# [!] EVERY FAILING CASE IS ONE THIS BOARD CANNOT BE MADE TO HAVE.  The four
# regions mpu.c programs are fixed, none of them covers the plugin reservation,
# and nothing reconfigures the MPU after boot -- so a check written inline in
# the loader would be a check nobody has ever seen say no, which is the shape
# issues #42 and #66 removed from this repository.  The verdict is a pure
# function of the register values, so every branch is reachable here.
#
# [!] AND IT IS NOT grove-vision-ai-v2's JUDGEMENT.  On Armv7-M the
# highest-numbered matching region wins; on Armv8-M two matches is a fault.
# This board RELIES on the v7-M rule -- mpu.c carves a cacheable window out of a
# larger non-cacheable one -- so the other board's judgement would refuse the
# configuration this one ships with.  Sub-regions and the background map have no
# counterpart there either.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_mpu_v7m.c" "$svc/plugin/plugin_mpu_v7m.c" \
    $LDFLAGS -o "$out/test_plugin_mpu_v7m"
"$out/test_plugin_mpu_v7m"

# issue #126 -- what a painter primitive decides before its first store
# (svc/plugin/plugin_paint_span.c): the clip, a blit's source offset, the charge, and
# how the painter is filled in.  Shared by grove-vision-ai-v2 and wio-lite-ai;
# their loops stay theirs, and each board's test_plugin_paint.c counts the
# stores its own loop makes against what this charged.  Every geometry case
# runs on 320x240 and on 240x320 with the rectangle transposed, so a w/h mix-up
# cannot pass on the one surface both boards happen to use.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_paint_span.c" "$svc/plugin/plugin_paint_span.c" \
    "$svc/plugin/plugin_paint_budget.c" "$svc/rect_geom.c" \
    $LDFLAGS -o "$out/test_plugin_paint_span"
"$out/test_plugin_paint_span"

# issue #165 -- the board's bindings table (svc/plugin/plugin_bindings.c): the
# painter and printer are filled from it, and the record of which word is
# which veneer is held against the plugin's own veneers (asset/common/
# plugin_base.c), the second source cmake/check_veneer_base_cost.py relies on.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" -I "$repo/asset/common" \
    "$here/test_plugin_bindings.c" "$svc/plugin/plugin_bindings.c" \
    "$repo/asset/common/plugin_base.c" \
    $LDFLAGS -o "$out/test_plugin_bindings"
"$out/test_plugin_bindings"

# issue #126 -- the one place that decides whether a plugin decodes
# (svc/nn/nn_active_core.c), shared by grove-vision-ai-v2 and wio-lite-ai: which
# slot is called, with what, how often, and what is answered when none is.
# The no-plugin answer to shapes_ok is the BOARD's and is tried both ways --
# grove refuses, wio accepts -- and with a plugin loaded neither may leak
# through.  Every call into a slot is preceded by exactly one depth sample for
# it, and a question that calls nothing samples nothing.  Each board's own
# test drives its port nn_active.c through this file with the real plugin.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_active_core.c" "$svc/nn/nn_active_core.c" "$svc/plugin/plugin_bindings.c" \
    $LDFLAGS -o "$out/test_nn_active_core"
"$out/test_nn_active_core"

# issues #97, #116, #159, #161 -- nn_tensor -> tensor_desc (svc/nn/nn_desc.c),
# on its own, against the real svc/nn/nn.h.  wio-lite-ai and f746g-disco both
# link this file; until #161 each board ran an identical copy of this test.
# Nothing here links svc/nn/blazeface.c: the translation has no model in it.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_desc.c" "$svc/nn/nn_desc.c" \
    $LDFLAGS -o "$out/test_nn_desc"
"$out/test_nn_desc"

# issues #131 P16, #152, #161 -- the model singleton of wio-lite-ai and
# f746g-disco (svc/nn/nn.c, one implementation since #161) over a stub backend
# and shell/test/nn_model_host.c (the board's state object and hooks,
# modelled).  Only the first open reaches the backend; a reload's answer is
# whether a MODEL is left; a refused load whose restore also failed stays open
# and empty and the next load goes through; every refusal code comes back
# unchanged; each critical section hands back the mask it found; the external
# memory claim (`sdram test`) refuses and is refused as nn.h says.  Built four
# times: a model in the image, its adoption failing, empty from boot, and a
# backend that cannot swap (NN_ERR_NOSUP for all three optional entries).
for nn_model_variant in "" -DTEST_FIRST_BUILD_FAILS -DTEST_EMPTY_FROM_BOOT -DTEST_NO_SWAP; do
    gcc $CFLAGS $nn_model_variant -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
        "$here/test_nn_model.c" "$here/nn_model_host.c" "$svc/nn/nn.c" "$svc/nn/nn_swap.c" \
        $LDFLAGS -o "$out/test_nn_model"
    "$out/test_nn_model"
done

# issues #159, #161 -- which staging slot the tflm backend (svc/nn/tflm/nn_tflm.cc,
# wio-lite-ai and f746g-disco) may adopt a model range from
# (svc/nn/tflm/nn_tflm_fit.h, the header the backend calls): a range ending
# exactly at the slot's end, one byte past it, misalignments 1..15, outside the
# slot, empty, wrapping, a slot at the top of the address space; then which of
# two slots, each refusal's code in its order, and never the live slot.
gcc $CFLAGS -I "$svc/nn" -I "$svc/nn/tflm" \
    "$here/test_nn_tflm_fit.c" $LDFLAGS -o "$out/test_nn_tflm_fit"
"$out/test_nn_tflm_fit"

# issue #162 -- the camera input tensor's admission test and the per-element
# conversion wio-lite-ai and f746g-disco share (svc/nn/nn_input_contract.{h,c}):
# every refusal by its own code and in its order (dtype, rank, batch, empty,
# channels, int8 scale incl. 0 / NaN / infinities / subnormals, int8 zero point,
# a byte count a 32-bit product would wrap past), and the conversion against a
# reference that spells out the target's instruction sequence (fmaf for VFMA)
# for every 8-bit value, both normalisations and a set of scales / zero points,
# plus the int8 limit on both sides and the +-0.5 boundaries.  -lm: the host's
# fmaf is the C library's (no FMA instruction is assumed on the build host).
gcc $CFLAGS -I "$svc/nn" \
    "$here/test_nn_input_contract.c" "$svc/nn/nn_input_contract.c" \
    $LDFLAGS -lm -o "$out/test_nn_input_contract"
"$out/test_nn_input_contract"

# issue #162 -- the input admission test's reasons as the start codes and
# sentences wio-lite-ai and f746g-disco both report (svc/nn/nn_input_codes.c):
# every reason against a table with the numbers written out, no two reasons on
# one code, an undocumented reason still a refusal, the reverse lookup exactly
# the worded codes, every sentence distinct and within NN_SVC_DETAIL_MAX.  Each
# board checks its own codes do not collide (boards/<board>/test/).
gcc $CFLAGS -I "$svc/nn" \
    "$here/test_nn_input_codes.c" "$svc/nn/nn_input_codes.c" \
    "$svc/nn/nn_input_contract.c" $LDFLAGS -o "$out/test_nn_input_codes"
"$out/test_nn_input_codes"

# issue #130 -- the loader's plugin-lease checks (svc/plugin/plugin_run_core.c,
# shared by wio-lite-ai and f746g-disco since issue #159): a load refused before
# the shared loader touches anything, the exec_ok hook refusing again at the
# branch into entry() -- by name, not as the MPU refusal the loader turns a
# hook's "no" into -- and an unload that does not reach the loader, each
# without the lease and each counted once.  The board's binding (MPU verdict,
# caches, log, depth record) is stubbed; the shared loader's unload is wrapped so
# "it did not unpublish" can be observed as "it did not call".  Each board's
# check_lease_entries run holds this file's table against the objects.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_run_lease.c" "$svc/plugin/plugin_run_core.c" \
    "$svc/plugin/plugin_exec.c" "$svc/plugin/plugin_load.c" "$svc/plugin/plugin_load_host.c" "$svc/crc32.c" \
    "$svc/fmt.c" \
    $LDFLAGS -Wl,--wrap=plugin_exec_unload -o "$out/test_plugin_run_lease"
"$out/test_plugin_run_lease"

# issue #159 -- the lease-checked entries above that branch
# (svc/nn/nn_active_lease.c), moved out of wio-lite-ai so f746g-disco takes the
# same ones: every entry refuses a caller without the plugin lease with
# NN_ACTIVE_NOT_HELD, enters and reads nothing, counts the refusal once, and
# goes in with it held; the descriptors it builds keep the count, the cap, and a
# hole read as zeroed rather than as the board's residue.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_active_lease.c" "$svc/nn/nn_active_lease.c" \
    "$svc/nn/nn_active_core.c" "$svc/plugin/plugin_bindings.c" \
    $LDFLAGS -o "$out/test_nn_active_lease"
"$out/test_nn_active_lease"

# issue #110 (#78 Step 3b) -- the capture of an external decoder's own account
# of its result (svc/nn/nn_report.c).  A contract about OUTCOMES: zero bytes is a
# legal report, "no report to give" is a different answer, truncation is a
# third and a decoder's own refusal a fourth, and a consumer that inferred any
# of them from the length would tell an operator the wrong one.  The last case
# demonstrates the ownership -- two callers, two frames -- because a
# board-owned capture slot, which is the obvious first design and the wrong
# one, passes every other case in the file.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" -I "$inc" \
    "$here/test_nn_report.c" "$svc/nn/nn_report.c" \
    $LDFLAGS -o "$out/test_nn_report"
"$out/test_nn_report"

# issue #110 (#78 Step 3b) -- the loader that runs AFTER that validation
# (svc/plugin/plugin_exec.c), shared by grove-vision-ai-v2 and wio-lite-ai.
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
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_exec.c" "$svc/plugin/plugin_exec.c" \
    $LDFLAGS -o "$out/test_plugin_exec"
"$out/test_plugin_exec"

# issue #97 -- the shared BlazeFace decoder (svc/nn/blazeface.c), folded from three
# diverged board copies.  The REAL decoder is compiled here against synthetic
# tensors, which it permits because it takes descriptors (svc/nn/tensor.h) rather
# than reaching into any inference singleton -- the arrangement that lets a
# decode bug be found without spending a flash cycle on a board.  Absorbs the
# assertions of all three tests it replaced, and runs the type-independent ones
# against BOTH int8 (Grove) and float32 (wio, f746): per-tensor dequantisation,
# the anchor grid, lookup by shape, the candidate cap not truncating the scan
# (issue #47), NMS, the distinct failure codes (issue #57), and init validation.
# Pure svc layer -- HAL/ThreadX/shell-free, so it needs only the svc include dir.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_blazeface.c" "$svc/nn/blazeface.c" \
    $LDFLAGS -o "$out/test_blazeface_svc"
"$out/test_blazeface_svc"

# ---- negative-test runners: which must have run, and how many cases ------- *
# issue #205 (after #204, which did this for one Grove runner) -- a negative-test
# runner whose call is deleted, turned into `:` or skipped when it should not
# have been prints nothing and fails nothing: the suite still ends in "host
# tests passed".  So every runner ends a passing run with exactly one stdout
# line, "negative-tests: <path from the repository root> OK (N cases)", N being
# the verdicts it reached, and THIS script holds every such line against the
# table below.  A row is: the runner / the log its line must be in ("shared"
# for this script's own calls, else the board whose host_tests.sh calls it) /
# when it must have run / N under that condition.
#
# The condition is decided HERE, from facts, and never from a board's SKIPPED
# line (a skip that should not have happened reads like a reason).  The
# predicates, joined with "+", each optionally negated with "!":
#   always                 -- no requirement
#   toolchain              -- the pinned arm-none-eabi-gcc under tools/
#   tree:<board>           -- that board's configured tree (a build.ninja), at
#                             the path its host_tests.sh uses (neg_tree below)
#   target:<board>:<name>  -- that tree has a ninja target <name>
# The board scripts keep their own conditions; the two are independent and are
# compared both ways: a row whose condition holds needs its line exactly once
# with N equal to the row's, a (runner, log) none of whose rows holds must have
# no line, and a line no row names fails.  A board that was not run is not
# looked at.  [!] N is compared for EQUALITY, not as a floor: with a floor, a
# case added without raising it would let a later one be dropped unnoticed, so
# a commit that adds or removes a case changes its row here.
#
# And the table is held against the files: every .py and .sh file anywhere
# under cmake/fixtures/, boards/*/cmake/fixtures/ and boards/*/test/ is taken
# for a runner and needs a row or a line in neg_excluded with its reason -- a
# runner nothing calls is found here.  Each row is checked before anything
# runs, so that a row which could never be looked at fails instead of reading
# as coverage: its file exists, its log is "shared" or a board with a
# test/host_tests.sh, the boards its condition names are such boards, a
# target:<board>:<name> names a target that board's CMake sources (or the
# shared ones) define, no predicate is "!always", and N is a number.  Last,
# the line's text may appear only in the runners the table names and in the
# two negative-test sections of this script: a board script that echoes it
# would otherwise pass for the runner.
neg_table="$out/neg_table"
cat > "$neg_table" <<'EOF'
cmake/fixtures/run_storage_gate_tests.py                         shared             always      8
cmake/fixtures/run_add_plugin_arg_tests.py                       shared             always      103
cmake/fixtures/run_asset_dest_tests.py                           shared             always      45
cmake/fixtures/run_slot_table_tests.py                           shared             always      12
cmake/fixtures/run_pinned_input_tests.py                         shared             always      16
cmake/fixtures/run_fetch_upstream_tests.py                       shared             always      52
cmake/fixtures/run_git_env_tests.py                              shared             always      58
boards/f746g-disco/cmake/fixtures/run_layout_tests.py            f746g-disco        always      41
cmake/fixtures/run_blob_staging_tests.py                         f746g-disco        toolchain   36
cmake/fixtures/run_blob_staging_tests.py                         f746g-disco        !toolchain  5
cmake/fixtures/run_model_io_gate_tests.py                        f746g-disco        target:f746g-disco:verify_tflite  278
boards/f746g-disco/cmake/fixtures/run_verify_tflite_size_tests.py f746g-disco       target:f746g-disco:verify_tflite  6
boards/grove-vision-ai-v2/test/test_flash_partitions.py          grove-vision-ai-v2 always      31
boards/grove-vision-ai-v2/test/test_flash_geometry.py            grove-vision-ai-v2 always      23
boards/grove-vision-ai-v2/test/test_planar_bgr_to_png.py         grove-vision-ai-v2 always      92
boards/grove-vision-ai-v2/test/test_plugin_stack.py              grove-vision-ai-v2 always      118
cmake/fixtures/run_plugin_gate_tests.py                          grove-vision-ai-v2 toolchain   141
cmake/fixtures/run_veneer_cost_tests.py                          grove-vision-ai-v2 toolchain   302
cmake/fixtures/run_veneer_gate_build_tests.py                    grove-vision-ai-v2 toolchain   385
boards/grove-vision-ai-v2/cmake/fixtures/run_fixture_tests.py    grove-vision-ai-v2 tree:grove-vision-ai-v2+toolchain  25
cmake/fixtures/run_shared_storage_build_tests.py                 grove-vision-ai-v2 tree:grove-vision-ai-v2+toolchain  145
cmake/fixtures/run_model_io_gate_tests.py                        grove-vision-ai-v2 target:grove-vision-ai-v2:verify_vela_model  486
boards/wio-lite-ai/test/test_lease_entries.py                    wio-lite-ai        always      2
boards/wio-lite-ai/cmake/fixtures/run_reservation_tests.py       wio-lite-ai        toolchain   10
cmake/fixtures/run_blob_staging_tests.py                         wio-lite-ai        toolchain   36
cmake/fixtures/run_blob_staging_tests.py                         wio-lite-ai        !toolchain  5
cmake/fixtures/run_veneer_cost_tests.py                          wio-lite-ai        toolchain   302
cmake/fixtures/run_veneer_gate_build_tests.py                    wio-lite-ai        toolchain   385
boards/wio-lite-ai/cmake/fixtures/run_fixture_tests.py           wio-lite-ai        tree:wio-lite-ai+toolchain  34
cmake/fixtures/run_model_io_gate_tests.py                        wio-lite-ai        target:wio-lite-ai:verify_tflite  278
boards/wio-lite-ai/cmake/fixtures/run_verify_tflite_size_tests.py wio-lite-ai       target:wio-lite-ai:verify_tflite  6
EOF
# Files under those directories that are deliberately not host-test runners.
# [!] Being listed here does not let a file print the line (see the end).
neg_excluded="$out/neg_excluded"
cat > "$neg_excluded" <<'EOF'
cmake/fixtures/model_io/schema_table_depth.py     a helper run_model_io_gate_tests.py runs and counts as part of its own case
boards/f746g-disco/test/host_tests.sh             the board's dispatcher: it calls runners, it is not one
boards/grove-vision-ai-v2/test/host_tests.sh      the board's dispatcher: it calls runners, it is not one
boards/wio-lite-ai/test/host_tests.sh             the board's dispatcher: it calls runners, it is not one
EOF
# The boards a row may name: every one with a dispatcher, selected or not.
neg_boards=""
for f in "$repo"/boards/*/test/host_tests.sh; do
    [ -f "$f" ] && neg_boards="$neg_boards $(basename "$(dirname "$(dirname "$f")")")"
done

neg_tree() {  # <board> -> that board's configured tree, as its host_tests.sh finds it
    case "$1" in
    f746g-disco)        echo "${F746_TFLM_BUILD_DIR:-$repo/build/f746-tflm}" ;;
    wio-lite-ai)        echo "${WIO_BUILD_DIR:-$repo/build/wio-lite-ai}" ;;
    grove-vision-ai-v2) echo "${GROVE_BUILD_DIR:-$repo/build/grove-vision-ai-v2}" ;;
    *) echo "run_host_tests: negative-test table: no tree known for board '$1'" >&2
       exit 1 ;;
    esac
}
neg_pred() {  # <predicate>
    case "$1" in
    always) return 0 ;;
    toolchain)
        _ng=$(ls -d "$repo"/tools/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | tail -1)
        [ -n "$_ng" ] && [ -x "$_ng" ] ;;
    tree:*)
        _nd=$(neg_tree "${1#tree:}") || exit 1
        [ -f "$_nd/build.ninja" ] ;;
    target:*)
        _nr=${1#target:}
        _nd=$(neg_tree "${_nr%%:*}") || exit 1
        [ -f "$_nd/build.ninja" ] &&
            ninja -C "$_nd" -t targets all 2>/dev/null </dev/null | grep -q "^${_nr#*:}:" ;;
    *) echo "run_host_tests: negative-test table: unknown condition '$1'" >&2
       exit 1 ;;
    esac
}
neg_cond() {  # <p1+p2+...>: every predicate holds (a "!" one must not)
    _nrest=$1
    while [ -n "$_nrest" ]; do
        _np=${_nrest%%+*}
        case "$_nrest" in *+*) _nrest=${_nrest#*+} ;; *) _nrest="" ;; esac
        case "$_np" in
        '!'*) if neg_pred "${_np#!}"; then return 1; fi ;;
        *)    if ! neg_pred "$_np"; then return 1; fi ;;
        esac
    done
    return 0
}

# The inventory and the rows, before anything runs (it costs nothing and needs
# no tree).
neg_bad=0
neg_board_ok() {  # <board>: one with a dispatcher, and a tree neg_tree knows
    case " $neg_boards " in *" $1 "*) ;; *) return 1 ;; esac
    ( neg_tree "$1" ) >/dev/null 2>&1
}
neg_target_defined() {  # <board> <name>: a target the CMake sources define
    _nre=$(printf '%s' "$2" | sed 's/[.+]/\\&/g')
    for _nf in "$repo/CMakeLists.txt" "$repo"/cmake/*.cmake \
               "$repo/boards/$1/board.cmake" "$repo/boards/$1"/cmake/*.cmake; do
        [ -f "$_nf" ] || continue
        grep -qE '(add_custom_target|add_executable|add_library)\([[:space:]]*'"$_nre"'([[:space:])]|$)|OUTPUT[[:space:]]+"\$\{CMAKE_BINARY_DIR\}/'"$_nre"'"' \
            "$_nf" && return 0
    done
    return 1
}
find "$repo/cmake/fixtures" "$repo"/boards/*/cmake/fixtures "$repo"/boards/*/test \
    -type f \( -name '*.py' -o -name '*.sh' \) 2>/dev/null | sort > "$out/neg_files"
while IFS= read -r f; do
    rel=${f#"$repo"/}
    if ! awk -v p="$rel" '$1 == p { found = 1 } END { exit !found }' \
            "$neg_table" "$neg_excluded"; then
        echo "run_host_tests: $rel is a negative-test runner with no row in the" \
             "negative-test table and no line in neg_excluded -- nothing checks" \
             "that it runs" >&2
        neg_bad=1
    fi
done < "$out/neg_files"
while read -r rel _nlog _ncond _nn; do
    _nrow="the negative-test table's row for $rel ($_nlog log, '$_ncond')"
    if [ ! -f "$repo/$rel" ]; then
        echo "run_host_tests: $_nrow names a file that does not exist" >&2
        neg_bad=1
    fi
    if [ "$_nlog" != shared ] && ! neg_board_ok "$_nlog"; then
        echo "run_host_tests: $_nrow: the log '$_nlog' is neither 'shared' nor" \
             "a board with a test/host_tests.sh (boards:$neg_boards), so its" \
             "line would never be looked for" >&2
        neg_bad=1
    fi
    case "$_nn" in
    ''|*[!0-9]*) echo "run_host_tests: $_nrow: N '$_nn' is not a number" >&2
                 neg_bad=1 ;;
    esac
    _nrest=$_ncond
    while [ -n "$_nrest" ]; do
        _np=${_nrest%%+*}
        case "$_nrest" in *+*) _nrest=${_nrest#*+} ;; *) _nrest="" ;; esac
        _npb=""
        case "${_np#!}" in
        always)
            if [ "$_np" != always ]; then _npb="'$_np' never holds"; fi ;;
        toolchain) ;;
        tree:*)
            _ntb=${_np#*tree:}
            neg_board_ok "$_ntb" ||
                _npb="'$_ntb' is not a board with a test/host_tests.sh" ;;
        target:*)
            _nr=${_np#*target:}
            _ntb=${_nr%%:*}
            _ntn=${_nr#*:}
            if [ "$_ntb" = "$_nr" ] || ! neg_board_ok "$_ntb"; then
                _npb="'$_ntb' is not a board with a test/host_tests.sh"
            elif ! printf '%s\n' "$_ntn" | grep -qE '^[A-Za-z0-9_.+-]+$'; then
                _npb="'$_ntn' is not a target name"
            elif ! neg_target_defined "$_ntb" "$_ntn"; then
                _npb="no target '$_ntn' is defined in CMakeLists.txt,"
                _npb="$_npb cmake/*.cmake, boards/$_ntb/board.cmake or"
                _npb="$_npb boards/$_ntb/cmake/*.cmake"
            fi ;;
        *) _npb="'$_np' is not a condition" ;;
        esac
        if [ -n "$_npb" ]; then
            echo "run_host_tests: $_nrow: $_npb, so the row could never be" \
                 "held against a run" >&2
            neg_bad=1
        fi
    done
done < "$neg_table"
# The line's text anywhere else that the host tests could run: only the runners
# the table names may print it, and this script mentions it only in its two
# negative-test sections (this one and the one at the end).  README text is not
# run, so *.md is left out; so are upstream, fetched and built trees.
find "$repo" \( -path "$repo/.git" -o -path "$repo/_ref" -o -path "$repo/lib" \
        -o -path "$repo/build" -o -path "$repo/build-*" -o -path "$repo/tools" \
        -o -path "$repo/.venv*" -o -path "$repo/boards/*/lib" \
        -o -path "$repo/boards/*/sdk" -o -path "$out" -o -name __pycache__ \) \
        -prune -o -type f ! -name '*.md' -exec grep -lIF 'negative-tests:' {} + \
        2>/dev/null | sort > "$out/neg_texts"
while IFS= read -r f; do
    rel=${f#"$repo"/}
    if [ "$rel" = shell/test/run_host_tests.sh ]; then
        awk -v p="$rel" '
            /^# ---- negative-test runners: which must have run/       { s = 1 }
            /^# This script.s own runner calls keep their stdout/       { s = 0 }
            /^# ---- negative-test runners: the logs against the table/ { s = 1 }
            !s && index($0, "negative-tests:") { print p ":" NR ": " $0 }' \
            "$f" > "$out/neg_spoof"
    elif awk -v p="$rel" '$1 == p { found = 1 } END { exit !found }' "$neg_table"; then
        continue
    else
        grep -nF 'negative-tests:' "$f" | sed "s|^|$rel:|" > "$out/neg_spoof"
    fi
    if [ -s "$out/neg_spoof" ]; then
        echo "run_host_tests: the text 'negative-tests:' belongs to the runners" \
             "the table names (each prints its own line); it is also here:" >&2
        cat "$out/neg_spoof" >&2
        neg_bad=1
    fi
done < "$out/neg_texts"
[ "$neg_bad" -eq 0 ] || exit 1

# This script's own runner calls keep their stdout in one log, as the boards'
# do; the exit code travels in a file because a pipeline's status is tee's.
shared_log="$out/shared_tests.log"
: > "$shared_log"
shared_py() {  # <python3 arguments...>
    { _src=0; python3 "$@" || _src=$?; echo "$_src" > "$shared_log.rc"; } \
        | tee -a "$shared_log"
    _src=$(cat "$shared_log.rc")
    [ "$_src" -eq 0 ] || exit "$_src"
}

# issue #97 -- negative tests for cmake/check_no_mutable_storage.py, the gate that
# keeps svc/nn/blazeface.c from growing state of its own (each board passes in its
# own scratch so that it keeps its own placement and its own residency gate).
# Four fixtures: clean, a plain static, thread-local storage and anonymous
# writable bytes from inline asm -- the last two are why the gate measures
# SECTIONS and not symbols, since neither is an STT_OBJECT.
# [!] The fifth, `target_only`, PASSES here on purpose: it puts its static behind
# `#if defined(__arm__)`, so a host-side run cannot see it.  That is the whole
# reason the gate itself is wired per board against the cross compiler rather
# than once here.  The boards run this same script with their own toolchain.
shared_py "$repo/cmake/fixtures/run_storage_gate_tests.py" \
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
shared_py "$repo/cmake/fixtures/run_add_plugin_arg_tests.py"

# issues #159, #195 -- an asset's destination is a slot, both halves required,
# and the receipt is one text for every board: build_asset.py refuses a
# missing --slot / --slot-table and the removed file options before packing,
# and asset_receipt.py prints that text byte for byte for a one- and a
# two-digit slot and refuses the removed per-board options.  It also runs build_asset.py to the end with stand-in tools: a
# container that fits its slot exactly is published, and a byte over, a slot
# the table lacks, an unreadable slot table and a non-numeric slot publish
# nothing.
shared_py "$repo/cmake/fixtures/run_asset_dest_tests.py"

# issue #195 -- the one slot-table tool every board's asset build runs
# (asset/tools/slot_table.c): each board's shipped table passes and comes out
# as JSON; a slot outside the interval, overlapping or not whole erase units
# is refused and leaves no JSON (an earlier one is removed); and the interval
# checked is the board's own.  Host cc only.
shared_py "$repo/cmake/fixtures/run_slot_table_tests.py"

# issue #132 -- the pin is checked again by the FIRST CONSUMER of the fetched
# model, inside its own command (ninja reruns only the consumer when the file
# changes after its fetch): the packer's --model-sha256 on the bytes it packs,
# and cmake/run_with_pinned_input.py in front of an ingest tool.  A one-byte
# flip must refuse and write nothing, an empty hash must refuse, and a match
# must go through.  Python only.
shared_py "$repo/cmake/fixtures/run_pinned_input_tests.py"

# issue #136 -- cmake/fetch_upstream.cmake, the per-board pinned fetch of the ST
# Cube trees (HAL / CMSIS device / CMSIS core).  An existing checkout is only ever
# checked, never repaired: moved to another commit, edited, a probe gone or an
# empty directory in its place must each stop the configure, a deleted one must
# be fetched again, the second configure must be offline, and a parallel
# configure must wait for the lock.  Against a local file:// upstream, so no
# network; needs git, cmake and ninja only.
shared_py "$repo/cmake/fixtures/run_fetch_upstream_tests.py"

# issue #158 -- the other two pinned fetches, cmake/fetch_model.cmake and the
# Grove SDK fetch (boards/grove-vision-ai-v2/cmake/himax_sdk_fetch.cmake), start
# git through the same FETCH_UPSTREAM_GIT.  With GIT_DIR / GIT_WORK_TREE /
# GIT_INDEX_FILE / GIT_OBJECT_DIRECTORY / GIT_COMMON_DIR pointed at a throwaway
# superproject, each fetch must succeed and leave the superproject byte for byte
# as it was; an alternate object store must not end up owning the SDK tree's
# objects; and a core.hooksPath or url.<x>.insteadOf carried in
# GIT_CONFIG_PARAMETERS or GIT_CONFIG_COUNT must neither run a hook nor send the
# fetch to another remote.  Local file:// upstreams; needs git and cmake only.
shared_py "$repo/cmake/fixtures/run_git_env_tests.py"

# issue #97 -- the published decode record (svc/nn/nn_det_record.c).  A camera stream
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
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_det_record.c" "$svc/nn/nn_det_record.c" \
    $LDFLAGS -o "$out/test_nn_det_record"
"$out/test_nn_det_record"

# issue #121 -- the top classes a worker publishes with a result it could not
# decode (svc/nn/nn_top.c).  The walk moved out of the shared command, which read
# the output tensor at print time -- after a stream stop, the dropped frame's.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_top.c" "$svc/nn/nn_top.c" \
    $LDFLAGS -o "$out/test_nn_top"
"$out/test_nn_top"

# issue #50 -- the pure parts of the ONE shared `nn` command (shell/cmds).  Two
# things live here that a console cannot check:
#
#   the scaled-integer number formatting, because this firmware has no %f and the
#   form it replaces printed 1.5 as "0.1500000" -- silently wrong for every
#   quantisation scale >= 1, which output tensors routinely have, and plausible
#   enough either way that nobody noticed by reading it;
#
#   the model-name grammar (issue #196): one word, the blob name, on every board;
#   a first word that starts with '-' is an option and is refused, whatever
#   follows (every old tagged form lands there); a name is never cut.  Linked
#   with svc/blob/blob_state.c so the parser's '-' rule is compared with the
#   store's, which the parser does not call.
gcc $CFLAGS -I "$inc" -I "$here/../cmds" -I "$core" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_cmd_core.c" "$here/../cmds/nn_cmd_core.c" \
    "$svc/blob/blob_state.c" "$svc/crc32.c" \
    $LDFLAGS -o "$out/test_nn_cmd_core"
"$out/test_nn_cmd_core"

# issue #122 P15 -- a board's `nn` wording is checked against NN_SVC_DETAIL_MAX at
# build time, because the copy into a result truncates and a truncated sentence
# does not look truncated (one of wio's refusals was 164 characters and ended
# "or see `d" on hardware).  The check fires in the COMPILER, so it is tested by
# compiling: the boundary must build and run, and each over-long or non-literal
# form must fail to build with the assertion's own message -- a compile that
# fails for some other reason proves nothing about the gate.
#
# [!] "ON THE GATE" IS CHECKED, not assumed.  Every error in the log must be in
# svc/nn/nn_detail.h -- the gate -- or on a test line marked "must not compile";
# an unrelated error elsewhere fails this rather than passing for the gate.  And
# each case names what it must show: the assertion's message for the over-long
# ones, and for a pointer the syntax error that `"" s` produces INSIDE the
# gate's own macro (the only thing that stops a pointer being "checked").
detail_gate_failed() {  # <log> <source> <want>
    marked=$(grep -n "must not compile" "$2" | cut -d: -f1 | tr '\n' ' ')
    src=$(basename "$2")
    LC_ALL=C grep "error:" "$1" | while IFS= read -r line; do
        case "$line" in
        *nn_detail.h:*) ;;
        *"$src":*)
            ln=$(printf '%s\n' "$line" | sed -n "s/^.*$src:\([0-9]*\):.*/\1/p")
            case " $marked " in
            *" $ln "*) ;;
            *) echo "unmarked error: $line"; return 0 ;;
            esac ;;
        *) echo "error outside the gate: $line"; return 0 ;;
        esac
    done | grep -q . && return 1
    LC_ALL=C grep -q "$3" "$1"
}
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" "$here/test_nn_detail_check.c" \
    $LDFLAGS -o "$out/test_nn_detail_check"
"$out/test_nn_detail_check"
g++ -std=c++17 -Wall -Wextra -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" "$here/test_nn_detail_check_cxx.cc" \
    -o "$out/test_nn_detail_check_cxx"
"$out/test_nn_detail_check_cxx"
for lang in c cxx; do
    for neg in NN_DC_OVER_LIT NN_DC_OVER_FMT NN_DC_NOT_LITERAL; do
        if [ "$lang" = c ]; then
            src="$here/test_nn_detail_check.c"
            cc="gcc $CFLAGS"
            notlit="in definition of macro 'NN_SVC_DETAIL_CHECK'"
        else
            [ "$neg" = NN_DC_OVER_FMT ] && continue   # no C++ formatter check
            src="$here/test_nn_detail_check_cxx.cc"
            cc="g++ -std=c++17 -Wall -Wextra"
            notlit="in definition of macro 'NN_SVC_DETAIL_LIT'"
        fi
        # shellcheck disable=SC2086  # $cc is deliberately word-split
        if LC_ALL=C $cc -D"$neg" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" "$src" -o "$out/detail_neg" \
               > "$out/detail_neg.log" 2>&1; then
            echo "test_nn_detail_check ($lang): $neg COMPILED -- the length gate did not fire" >&2
            exit 1
        fi
        case "$neg" in
        NN_DC_NOT_LITERAL) want="$notlit" ;;
        *)                 want='detail literal is longer than NN_SVC_DETAIL_MAX' ;;
        esac
        if ! detail_gate_failed "$out/detail_neg.log" "$src" "$want"; then
            echo "test_nn_detail_check ($lang): $neg failed to compile, but not on the gate:" >&2
            cat "$out/detail_neg.log" >&2
            exit 1
        fi
        echo "  ok   $neg ($lang) refused at compile time, by the gate"
    done
done

# issue #122 review -- the svc/ SHARED sentence tables a board copies whole into
# its explanation (plugin_result_name, plugin_run_strerror, and since issue #192
# blob_name_verdict_name and blob_lookup_name).  They must not depend
# on the `nn` contract, so they are enumerated here instead of wrapped.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_detail_tables.c" "$svc/plugin/plugin_load.c" "$svc/plugin/plugin_load_host.c" "$svc/plugin/plugin_exec.c" \
    "$svc/blob/blob_state.c" "$svc/crc32.c" $LDFLAGS -o "$out/test_nn_detail_tables"
"$out/test_nn_detail_tables"

# issue #122 -- what `nn model load` prints on a wrong invocation, through the REAL
# dispatcher and the REAL shell/cmds/cmd_nn.c.  The line is joined from two
# halves -- the dispatcher's command path and the command's argument spelling --
# and it read "usage: nn model load load <...>" on every board because the parent
# and the leaf shared one spelling.  Neither half is wrong on its own, so only the
# joined line can be tested.  The adapters are stubs (nothing is loaded); nn_cfg/
# stands in for a board's nn_svc_config.h with the loader on.  One shape since
# issue #196: every board names the model the same way.
# -Wno-unused-function: the stub config has no camera or bench, so the cancel
# shim they share is unused here and nowhere on a real board.
gcc $CFLAGS -Wno-unused-function -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$here/nn_cfg" -I "$inc" -I "$core" \
    -I "$here/../cmds" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_cmd_usage.c" "$here/../cmds/cmd_nn.c" \
    "$here/../cmds/nn_cmd_core.c" \
    "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_nn_cmd_usage"
"$out/test_nn_cmd_usage"

# issue #199 -- one `nn info` never prints two models, through the REAL
# dispatcher, shell/cmds/cmd_nn.c and the container lines
# (svc/plugin/plugin_claims.c), over a stand-in adapter with two states that
# differ in every line.  Another console's whole load is injected at each read
# the command makes (after the copy, after in[0], after the output count, after
# each output, after the threshold, before the claims): no line of the second
# state is printed, what came before is unchanged and the rest is busy.  With no
# load the output is the bytes it was before; an unreadable tensor still says
# `unavailable just now`.  Two consoles interleaving between single calls cannot
# be typed.
gcc $CFLAGS -Wno-unused-function -DCLI_USE_COLOR=0 \
    $glue_inc -I "$here/shim" -I "$here/nn_cfg" -I "$inc" -I "$core" \
    -I "$here/../cmds" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_info_one_state.c" "$here/../cmds/cmd_nn.c" \
    "$here/../cmds/nn_cmd_core.c" \
    "$svc/plugin/plugin_claims.c" "$svc/plugin/plugin_info.c" \
    "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
    "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
    $glue \
    $LDFLAGS -o "$out/test_nn_info_one_state"
"$out/test_nn_info_one_state"

# issue #142 review / #143 -- which stream a stop waits for (svc/camera/cam_gen.h).  A
# `camera stream stop &` (wio) or a `camera off` (f746, wio) below a console that
# starts the next stream can wake to find another stream running; the wait must
# ask "is MY generation over", never "is a stream active".  Two consoles at two
# priorities: not typeable.  Moved here from wio-lite-ai when f746 took it up.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_cam_gen.c" \
    $LDFLAGS -o "$out/test_cam_gen"
"$out/test_cam_gen"

# issue #143 -- the shared `camera` command (shell/cmds/cmd_camera.c) through the
# REAL dispatcher over a stub adapter (camera_svc_stub.c), once per capability
# shape a board may declare (camera_cfg/camera_svc_config.h): the table's order
# and its capabilities, the refusal sentences (a board without a test pattern
# still recognises `test` and says why), the lines the shared command prints
# around a board's report, and that every frame it opens it closes (MIN: no
# info, save or send).  The DUP
# shape registers a board `info` on top of the shared one: the check that the
# real boards' tables pass (boards/<board>/test/host_tests.sh) must see that.
# fx_shim/ stands in for FileX, which `camera save` writes through.
for shape in FULL BARE MIN DUP; do
    gcc $CFLAGS -DCLI_USE_COLOR=0 -DCAM_TEST_SHAPE_$shape \
        $glue_inc -I "$here/shim" -I "$here/fx_shim" -I "$here/camera_cfg" \
        -I "$inc" -I "$core" -I "$here/../cmds" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
        "$here/test_camera_cmd.c" "$here/camera_svc_stub.c" \
        "$here/../cmds/cmd_camera.c" \
        "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
        "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
        $glue \
        $LDFLAGS -o "$out/test_camera_cmd"
    "$out/test_camera_cmd"
done

# issue #194 -- the shared `blob` command (shell/cmds/cmd_blob.c) through the
# REAL dispatcher and the REAL write coordinator, over a stub store
# (blob_svc_stub.c), once per shape a board may declare
# (blob_cfg/blob_svc_config.h).  Nothing gates what a command prints, so the
# sentences are pinned here: each store result's refusal, each write outcome
# (the incomplete slot after a failed verify, the unknown state after a failed
# magic page), no slot number in the log for a write refused before a slot was
# chosen, one colon column for 2-digit slots, `blob erase` only under the
# reservation, the drops line with and without the RX ring's high-water (the
# RXLEVEL shape: restarted before the erase, clean / NOT CLEAN, a console with
# no ring) -- and, without a store or a receiver, nothing registered.
for shape in FULL RXLEVEL NOSTORE NORECV; do
    gcc $CFLAGS -DCLI_USE_COLOR=0 -DBLOB_TEST_SHAPE_$shape \
        $glue_inc -I "$here/shim" -I "$here/blob_cfg" \
        -I "$inc" -I "$core" -I "$here/../cmds" -I "$svc" -I "$svc/blob" \
        "$here/test_blob_cmd.c" "$here/blob_svc_stub.c" \
        "$here/../cmds/cmd_blob.c" "$svc/blob/blob_write.c" \
        "$svc/blob/blob_state.c" "$svc/blob/blob_map.c" "$svc/crc32.c" \
        "$svc/ymodem.c" \
        "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
        "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
        $glue \
        $LDFLAGS -o "$out/test_blob_cmd_$shape"
    "$out/test_blob_cmd_$shape"
done

# issue #219 -- the shared `net` command (shell/cmds/cmd_net.c) through the
# REAL dispatcher over a stub adapter that is also the clock (net_svc_stub.c),
# once per shape a board may declare (net_cfg/net_svc_config.h).  Nothing gates
# what a command prints, and the exit value does not reach the console, so both
# are pinned here: the bad counts and the default of 4 (N1), exit 1 with no
# reply (N2), the 30 s DHCP wait (N3), the two address lines and the STALE mark
# (N4), BUSY called again within one deadline a slow adapter
# does not stretch, Ctrl+C in each wait, and the board's lines through the
# writer.  The DUP shape registers a board `info` on top of the shared one: the
# check the real boards' tables pass (boards/<board>/test/host_tests.sh) must
# see that.  TX_TIMER_TICKS_PER_SECOND is passed because the shim does not
# define it (the command asserts 1 ms ticks).
for shape in EXTRA BARE DUP; do
    gcc $CFLAGS -DCLI_USE_COLOR=0 -DNET_TEST_SHAPE_$shape -DTX_TIMER_TICKS_PER_SECOND=1000 \
        $glue_inc -I "$here/shim" -I "$here/net_cfg" \
        -I "$inc" -I "$core" -I "$here/../cmds" -I "$svc" -I "$svc/net" \
        "$here/test_net_cmd.c" "$here/net_svc_stub.c" \
        "$here/../cmds/cmd_net.c" \
        "$core/cli_session.c" "$core/cli_edit.c" "$core/cli_history.c" \
        "$core/cli_printf.c" "$svc/fmt.c" "$core/cli_parse.c" "$core/cli_complete.c" \
        $glue \
        $LDFLAGS -o "$out/test_net_cmd_$shape"
    "$out/test_net_cmd_$shape"
done

# issue #220 -- the one implementation of the address, DHCP and ping operations
# (shell/cmds/net_core.c) as it is compiled for the boards, over fake NetX / DHCP
# / ThreadX calls that record each call, whether the address lock was held for
# it and the interrupt-masked sections (nx_shim/, shim/tx_api.h).  The fake DHCP
# record refuses a start after a stop until it is reinitialised, as NetX documents.
# Nothing a board prints shows a rollback's order or a lock that was not held,
# so they are pinned here: `net ip` puts everything back (the reinitialise
# before the address), `net dhcp`'s three cases and LINK with nothing called,
# each failed step moving the state only by what succeeded, ping's three
# answers, the stale mark, and the link: the notification (one masked section,
# no NetX call, no lock), one worker pass's reactions and folding, a notice
# after the worker's copy, a bounce before `net dhcp`, the last-failure record,
# the KV source's wake and init failing on any of its objects.
# Since issue #221 the notification also counts the falls, and a pass that
# handled one wakes the telnet console (faked here) with the lock released.
gcc $CFLAGS -I "$here/shim" -I "$here/nx_shim" -I "$here/../cmds" -I "$svc/net" \
    -I "$here/../include" -I "$backend" \
    "$here/test_net_core.c" "$here/../cmds/net_core.c" \
    $LDFLAGS -o "$out/test_net_core"
"$out/test_net_core"

# issue #221 -- the one telnet console (shell/cmds/net_tty.c) as it is compiled
# for the boards, over a fake NetX TCP server socket that keeps NetX's states
# and its trap (a cut handshake leaves the socket listening but still bound,
# and accepting it again leaks a packet), fake event flags and masked sections,
# and a CLI that records its notifications.  None of it can be typed at a
# console: one accept per connection (unaccept + relisten first, a failure
# stops the listener), the publish against the order of the establishment, a
# peer's close and a link fall, the fall count compared before every wait, the
# three ends (gate, ring, the 0x03 ahead of a full receive ring, back to
# listening), nothing crossing into the next session, IAC both ways, the
# refusals by reason, the telnet question and the `net info` lines.
gcc $CFLAGS -DCLI_USE_COLOR=0 -I "$here/shim" -I "$here/nx_shim" -I "$here/../cmds" \
    -I "$inc" -I "$core" -I "$backend" -I "$svc" -I "$svc/net" \
    "$here/test_net_tty.c" "$here/../cmds/net_tty.c" "$svc/fmt.c" \
    $LDFLAGS -o "$out/test_net_tty"
"$out/test_net_tty"

# issue #99 -- the shared stream lifecycle (svc/nn/nn_stream_life.c).
#
# A `--frames` waiter and a second console racing over one stream: the waiter
# must never tear down a stream it did not start.  The generation alone does not
# close that -- two callers can both be admitted unless the stop transition is
# CLAIMED in the same breath, which is the defect the adversarial review of this
# issue found on two of the three boards.  None of it can be typed: the window is
# between two statements of another thread, and the board where it matters most
# has one console whose background jobs run below the foreground shell under
# TX_NO_TIME_SLICE.  One machine, one test.
gcc $CFLAGS -I "$inc" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_stream_life.c" "$svc/nn/nn_stream_life.c" \
    $LDFLAGS -o "$out/test_nn_stream_life"
"$out/test_nn_stream_life"

# issue #130 -- the policy around that machine, written once for every board
# (svc/nn/nn_core.c): the baselines a commit latches, the two-phase poll and the
# settle's disposition table, each walked under the three property sets the
# boards differ by (plain / re-arming / gated).  The stand-in hooks also record
# a counter or a record read INSIDE the critical section (a deadlock on hardware)
# and a gate or a latch touched OUTSIDE it (a race) -- neither shows in a return.
gcc $CFLAGS -I "$inc" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_core.c" "$svc/nn/nn_core.c" "$svc/nn/nn_stream_life.c" \
    "$svc/nn/nn_det_record.c" \
    $LDFLAGS -o "$out/test_nn_core"
"$out/test_nn_core"

# issue #129 / #130 -- who may touch the inference worker's input
# (svc/nn/nn_handoff.c, moved out of grove-vision-ai-v2 and given FILLING by issue
# #130).  The producer writes the input tensor in place, so this word is the only
# thing keeping it and the worker apart, and the sequences that would break it --
# a stop while a frame is handed over, a stale wake-up, a band after its frame was
# abandoned -- are windows between three threads that cannot be typed.  Every
# (state, operation) pair is spelled out.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_handoff.c" "$svc/nn/nn_handoff.c" \
    $LDFLAGS -o "$out/test_nn_handoff"
"$out/test_nn_handoff"

# issue #122 / #131 -- where a `nn model load` ends and what that obliges
# (svc/nn/nn_swap.c, moved out of grove-vision-ai-v2 and given wio-lite-ai's and
# f746g-disco's way in by issue #131).  A load over an open model is a
# replacement with a rollback, and the endings that matter most -- the backend
# refuses the new model AND the previous one, a plugin the device refuses after
# the host packer validated it -- cannot be typed.  Every board's table is
# spelled out against the one function, and the real contract header supplies
# the operator's vocabulary (enum nn_model_state), so the two cannot drift.
gcc $CFLAGS -I "$inc" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_swap.c" "$svc/nn/nn_swap.c" \
    $LDFLAGS -o "$out/test_nn_swap"
"$out/test_nn_swap"

# issue #131 -- the ORDER around that table (svc/nn/nn_core_model.c): claim ->
# preparation -> lookup -> plugin lease -> backend -> plugin -> obligations, and
# the unload's.  A stand-in board traces every hook, and the hooks check what no
# return shows: a hook inside the critical section, a plugin touched without the
# lease, a change while the transition counter is even, BUSY answered after the
# backend was touched (issue #131 stage 8 relies on that never happening).
gcc $CFLAGS -I "$inc" -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_core_model.c" "$svc/nn/nn_core_model.c" "$svc/nn/nn_swap.c" \
    $LDFLAGS -o "$out/test_nn_core_model"
"$out/test_nn_core_model"

# issue #129 -- how `nn run`'s wait for its one inference ends
# (svc/nn/nn_run_wait.c, moved out of grove-vision-ai-v2 by issue #163).  A
# publish and the deadline, a publish and a Ctrl+C, a finished worker and a lost
# stream can all hold in one poll, and only the order decides the status; none
# of those coincidences can be typed.
gcc $CFLAGS -I "$svc" -I "$svc/nn" \
    "$here/test_nn_run_wait.c" "$svc/nn/nn_run_wait.c" \
    $LDFLAGS -o "$out/test_nn_run_wait"
"$out/test_nn_run_wait"

# issue #171 -- how one `nn run` ended, and whether it is still open
# (svc/nn/nn_shot.c, wio-lite-ai and f746g-disco).  An invoke that failed, a lease
# not had in time, a prep refused and a boundary that moved the generation in
# between cannot be made on hardware; the first walks the table and every
# transition, the second the boards' call orders through the real frame path,
# hand-over word and wait table.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_shot.c" "$svc/nn/nn_shot.c" \
    $LDFLAGS -o "$out/test_nn_shot"
"$out/test_nn_shot"
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_shot_seq.c" "$svc/nn/nn_shot.c" "$svc/nn/nn_core_frame.c" \
    "$svc/nn/nn_handoff.c" "$svc/nn/nn_det_record.c" "$svc/nn/nn_run_wait.c" \
    $LDFLAGS -o "$out/test_nn_shot_seq"
"$out/test_nn_shot_seq"

# issue #174 -- the one table of `nn` admission refusals and `nn run` endings
# (svc/nn/nn_refusal.c).  A dead lifecycle, a start and a stop in flight and a
# value outside the enum cannot be typed, and every sentence is picked from a
# table or composed with a number, which the build-time check cannot see: each
# input is run and its whole sentence compared with the expected text and the
# bound.  A board's clause after a run's sentence is checked at build time
# against NN_REFUSAL_RUN_BASE_MAX (the longer of the base sentences and
# nn_shot_says()'s), so one character too many must fail to compile, on that
# assertion.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_refusal.c" "$svc/nn/nn_refusal.c" "$svc/nn/nn_shot.c" "$svc/fmt.c" \
    $LDFLAGS -o "$out/test_nn_refusal"
"$out/test_nn_refusal"
if LC_ALL=C gcc $CFLAGS -DNN_REFUSAL_NEG_CLAUSE -I "$svc" -I "$svc/nn" -I "$svc/plugin" \
       -I "$svc/camera" -I "$svc/blob" -c "$here/test_nn_refusal.c" -o "$out/refusal_neg.o" \
       > "$out/refusal_neg.log" 2>&1; then
    echo "test_nn_refusal: NN_REFUSAL_NEG_CLAUSE COMPILED -- the clause check did not fire" >&2
    exit 1
fi
if ! LC_ALL=C grep -q "nn run clause does not fit after the longest sentence" \
       "$out/refusal_neg.log"; then
    echo "test_nn_refusal: NN_REFUSAL_NEG_CLAUSE failed to compile, but not on the check:" >&2
    cat "$out/refusal_neg.log" >&2
    exit 1
fi
echo "  ok   NN_REFUSAL_NEG_CLAUSE refused at compile time, by the clause check"

# issue #163 -- the one `nn bench` procedure (svc/nn/nn_bench.c): the board's
# hold taken once, every input filled and every run timed under it, given back
# exactly once on every exit and never after a failed acquire.  A console
# reaches only the common exits; a fake board records the order of all of them.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/blob" \
    "$here/test_nn_bench.c" "$svc/nn/nn_bench.c" "$svc/fmt.c" \
    $LDFLAGS -o "$out/test_nn_bench"
"$out/test_nn_bench"

# issue #122 -- the gate's claim and the count of console calls inside the
# plugin (svc/nn/nn_param_calls.c, moved out of grove-vision-ai-v2 by issue
# #163).  A threshold call in a background job preempted by a load on the other
# console is a window of microseconds; it cannot be typed.
gcc $CFLAGS -I "$svc" -I "$svc/nn" \
    "$here/test_nn_param_calls.c" "$svc/nn/nn_param_calls.c" \
    $LDFLAGS -o "$out/test_nn_param_calls"
"$out/test_nn_param_calls"

# issue #130 -- when `nn stream` may give the nn session back
# (svc/nn/nn_sess_release.c, moved out of f746g-disco by issue #163).  The
# producer writes the input tensor, inside the arena the session guards, so the
# release waits for a sink drain that confirmed the producer out.  A drain that
# runs out cannot be typed; the table and the four stop / worker interleavings
# are walked here instead.
gcc $CFLAGS -I "$svc" -I "$svc/nn" \
    "$here/test_nn_sess_release.c" "$svc/nn/nn_sess_release.c" \
    $LDFLAGS -o "$out/test_nn_sess_release"
"$out/test_nn_sess_release"

# issue #130 step 6c (#122 P8) -- how a decode's answer is counted in `nn stream
# stats` (svc/nn/nn_decode_count.c; one copy for wio-lite-ai and f746g-disco
# since issue #163, which merged the two boards' tests, and grove-vision-ai-v2's
# too since issue #164).  A plugin that answers BF_ERR_UNINIT, BF_ERR_ARG or an
# undocumented code cannot be typed, and each must be counted as a decoder
# fault, never as "the model" or as nothing; only what the record took is
# counted (P18); and a one-shot counts nothing (#164).
gcc $CFLAGS -I "$svc" -I "$svc/nn" \
    "$here/test_nn_decode_count.c" "$svc/nn/nn_decode_count.c" \
    $LDFLAGS -o "$out/test_nn_decode_count"
"$out/test_nn_decode_count"

# issue #130 -- the shared frame path that runs that word (svc/nn/nn_core_frame.c):
# a frame in four parts (wio-lite-ai's bands) and in one (grove-vision-ai-v2's),
# the two ways out of FILLING (the producer's ABANDON, the stop's JOIN), the
# worker's end of a job under and without the lease, and the panel's try.  The
# stand-in hooks watch the ORDER -- the input written outside FILLING, a hook
# inside the critical section, the lease given back after the board counted, a
# paint without the lease or before the frame lock -- which no return shows.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_nn_core_frame.c" "$svc/nn/nn_core_frame.c" "$svc/nn/nn_handoff.c" \
    "$svc/nn/nn_det_record.c" \
    $LDFLAGS -o "$out/test_nn_core_frame"
"$out/test_nn_core_frame"

# issue #127 -- what a refused no-wait acquire of the plugin lease counts
# (svc/plugin/plugin_lease_miss.c, moved out of grove-vision-ai-v2 by issue #130).  A
# frame can be lost at the producer's decode or at the panel's draw; a run reset
# by the producer's success, or a frame counted at both, would each print a
# plausible stats line.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_plugin_lease_miss.c" "$svc/plugin/plugin_lease_miss.c" \
    $LDFLAGS -o "$out/test_plugin_lease_miss"
"$out/test_plugin_lease_miss"

# issue #142 -- who holds the camera stream open (svc/camera/cam_holders.c).  The
# table decides when three boards start and stop their cameras, and the cases
# that matter most -- a start that fails while another holder is trying to
# register, a rollback that must not take a holder that was already there, the
# operator's stop while nn holds the stream -- are windows that cannot be typed.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_cam_holders.c" "$svc/camera/cam_holders.c" \
    $LDFLAGS -o "$out/test_cam_holders"
"$out/test_cam_holders"

# issue #142 -- a step of a camera sink (svc/camera/cam_step.c; f746g-disco's camera
# sink first, wio-lite-ai's band sink since).  The display and the nn ingest are
# two steps of one sink, each switched off while the base keeps running: disarm,
# then wait for the producer's count to reach zero, which is what proves it out
# of the step for good.  A disarm while the producer is inside, a re-arm before
# it has left and a leave with no enter cannot be typed; they are walked here.
gcc $CFLAGS -I "$svc" -I "$svc/nn" -I "$svc/plugin" -I "$svc/camera" -I "$svc/blob" \
    "$here/test_cam_step.c" "$svc/camera/cam_step.c" \
    $LDFLAGS -o "$out/test_cam_step"
"$out/test_cam_step"

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

neg_logs="shared"
for b in $boards; do
    script="$repo/boards/$b/test/host_tests.sh"
    if [ -f "$script" ]; then
        echo "--- board tests: $b"
        # stdout is kept to be read back; the exit code travels in a file
        # because a pipeline's status is tee's, not the board script's.
        log="$out/board_tests_$b.log"
        { brc=0; sh "$script" || brc=$?; echo "$brc" > "$log.rc"; } \
            | tee "$log"
        brc=$(cat "$log.rc")
        [ "$brc" -eq 0 ] || exit "$brc"
        neg_logs="$neg_logs $b"
    else
        echo "--- board tests: $b (none)"
    fi
done

# ---- negative-test runners: the logs against the table (see above) ------- *
neg_log_file() {  # <log name>
    if [ "$1" = shared ]; then echo "$shared_log"; else echo "$out/board_tests_$1.log"; fi
}
neg_lines() {  # <log file> <runner>: that runner's lines, whatever their shape
    awk -v p="$2" '$1 == "negative-tests:" && $2 == p' "$1"
}
echo "--- negative tests"
neg_bad=0
neg_held="$out/neg_held"
: > "$neg_held"
while read -r rel nlog ncond nn; do
    case " $neg_logs " in *" $nlog "*) ;; *) continue ;; esac
    neg_cond "$ncond" || continue
    if grep -qxF "$nlog $rel" "$neg_held"; then
        echo "run_host_tests: the negative-test table has two rows for $rel in" \
             "the $nlog log whose conditions both hold" >&2
        neg_bad=1
        continue
    fi
    echo "$nlog $rel" >> "$neg_held"
    lf=$(neg_log_file "$nlog")
    hits=$(neg_lines "$lf" "$rel" | wc -l)
    got=$(neg_lines "$lf" "$rel" | sed -n 's|^negative-tests: [^ ]* OK (\([0-9][0-9]*\) cases)$|\1|p' \
          | tr '\n' ' ' | sed 's/ $//')
    if [ "$hits" -ne 1 ] || [ "$got" != "$nn" ]; then
        echo "run_host_tests: negative tests: $rel in the $nlog log: '$ncond'" \
             "holds, so one line 'negative-tests: $rel OK ($nn cases)' is" \
             "required; found $hits line(s), N='${got:-none}', declared $nn" \
             "(change the row in the table only with the cases)" >&2
        neg_bad=1
    else
        echo "  ok   $rel ($nlog): $got cases, declared $nn, '$ncond' holds"
    fi
done < "$neg_table"
# A (runner, log) none of whose rows holds must have printed nothing.
while read -r rel nlog ncond nn; do
    case " $neg_logs " in *" $nlog "*) ;; *) continue ;; esac
    grep -qxF "$nlog $rel" "$neg_held" && continue
    echo "$nlog $rel" >> "$neg_held"   # once per pair
    hits=$(neg_lines "$(neg_log_file "$nlog")" "$rel" | wc -l)
    if [ "$hits" -ne 0 ]; then
        echo "run_host_tests: negative tests: $rel in the $nlog log: no row's" \
             "condition holds (this row: '$ncond'), so it must not have run;" \
             "found $hits line(s) -- the board script and this table disagree" \
             "about when it runs" >&2
        neg_bad=1
    else
        echo "  --   $rel ($nlog): not required, '$ncond' does not hold, no line"
    fi
done < "$neg_table"
# Every line in a log that was looked at has the one shape and a row.
for nlog in $neg_logs; do
    grep '^negative-tests:' "$(neg_log_file "$nlog")" > "$out/neg_found" || true
    while IFS= read -r line; do
        rel=$(printf '%s\n' "$line" | awk '{ print $2 }')
        if ! printf '%s\n' "$line" | grep -qE '^negative-tests: [^ ]+ OK \([0-9]+ cases\)$'; then
            echo "run_host_tests: negative tests: a line in the $nlog log is not" \
                 "'negative-tests: <runner> OK (N cases)': $line" >&2
            neg_bad=1
        elif ! awk -v p="$rel" -v l="$nlog" '$1 == p && $2 == l { found = 1 } END { exit !found }' \
                "$neg_table"; then
            echo "run_host_tests: negative tests: the $nlog log has a line for" \
                 "$rel, which has no row for that log in the table: $line" >&2
            neg_bad=1
        fi
    done < "$out/neg_found"
done
[ "$neg_bad" -eq 0 ] || exit 1

echo "host tests passed"
