#!/usr/bin/env sh
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Board-pinned host tests for Grove Vision AI V2.  These compile code this board
# OWNS against as much of its REAL source as a host compiler can take -- which
# is the point of them: a re-implemented copy of the logic would drift from the
# firmware's without anything noticing.
#
# Invoked by shell/test/run_host_tests.sh, which owns the toolchain flags and
# the scratch directory and passes them in the environment; there is no separate
# set of flags here, so a board test is built exactly like a core one.  Run the
# whole suite (or `run_host_tests.sh grove-vision-ai-v2` for this board alone)
# rather than executing this file directly.
set -eu

: "${HOST_TEST_OUT:?run via shell/test/run_host_tests.sh}"
: "${HOST_TEST_CFLAGS:?}" "${HOST_TEST_LDFLAGS:?}"
: "${HOST_TEST_INC:?}" "${HOST_TEST_SVC:?}" "${HOST_TEST_REPO:?}"

here=$(cd "$(dirname "$0")" && pwd)
board=$(cd "$here/.." && pwd)
out="$HOST_TEST_OUT"
CFLAGS="$HOST_TEST_CFLAGS"
LDFLAGS="$HOST_TEST_LDFLAGS"
sdk="$board/sdk/EPII_CM55M_APP_S"

# issue #30 -- the vendor timer API seam (port/sdk_seam/timer_seam.c).  Its
# refusal path is the one part of this board that CANNOT be reached on hardware
# in M-G3a: the camera archives are not linked yet, so every wrapper is
# garbage-collected out of the firmware.  cmake/check_timer_seam.py proves the
# link-level claim (no vendor timer code survives the --wrap); this proves the
# behavioural one -- that a call for any timer other than TIMER_ID_0 is refused
# without writing a single register, TIMER2 (the cpu% time source) above all.
#
# The REAL timer_seam.c is compiled, against the REAL SDK hx_drv_timer.h so the
# TIMER_CFG_T layout and the enum ABI are the firmware's.  Only the device
# layer is shimmed (test/shim), because the real WE2_device.h is a Cortex-M55
# register map a host compiler cannot use; the register block is redirected to
# a plain array so "wrote nothing" is a memcmp rather than an argument.
#
# The SDK is fetched at configure time, so skip rather than fail when no build
# tree has ever been configured -- a missing SDK is not a test result.
if [ -f "$sdk/drivers/inc/hx_drv_timer.h" ]; then
    # -Wno-pointer-to-int-cast: the seam stores an ISR address in the uint32_t
    # the vector table takes.  Exact on the 32-bit target, lossy-looking to a
    # 64-bit host compiler -- the truncation is harmless here because the mock
    # NVIC only ever compares the value with itself.
    gcc $CFLAGS -Wno-pointer-to-int-cast \
        -DGROVE_TIMER_SEAM_T0_BASE='((uint32_t)(uintptr_t)seam_host_env.regs)' \
        -I "$here" -I "$here/shim" \
        -I "$board/port/sdk_seam" -I "$board/port/threadx" -I "$board/svc" \
        -I "$sdk/drivers/inc" \
        "$here/test_timer_seam.c" "$here/seam_host_env.c" \
        "$board/port/sdk_seam/timer_seam.c" \
        $LDFLAGS -o "$out/test_timer_seam"
    "$out/test_timer_seam"

    # issue #35 -- the Timer0 interrupt-DELIVERY probe, which is what M-G3a
    # carried forward: hw_start proves the counter runs, and that is a
    # different claim from "the interrupt arrives".  The probe is itself a
    # check, so the case that matters is the one hardware cannot produce on
    # demand -- a Timer0 that counts perfectly and never fires.  The mock NVIC
    # calls the installed vector (or does not) from inside udelay(), which is
    # where the firmware waits.
    #
    # Twice, with different -D: the probe latches its answer on the first call
    # by design, so the two cases cannot share a process.  Same pattern as
    # test_complete / test_complete_smallbuf in the core suite.
    for _case in DELIVERED SILENT; do
        gcc $CFLAGS -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
            -DPROBE_CASE_$_case \
            -DGROVE_TIMER_SEAM_T0_BASE='((uint32_t)(uintptr_t)seam_host_env.regs)' \
            -I "$here" -I "$here/shim" \
            -I "$board/port/sdk_seam" -I "$board/port/threadx" -I "$board/svc" \
            -I "$sdk/drivers/inc" \
            "$here/test_timer_probe.c" "$here/seam_host_env.c" \
            "$board/port/sdk_seam/timer_seam.c" \
            $LDFLAGS -o "$out/test_timer_probe_$_case"
        "$out/test_timer_probe_$_case"
    done

    # issue #59 -- the WDMA3 landing-buffer transition machine
    # (port/camera/cam_wdma3.c).  Its worst failure is a flip that silently
    # became a no-op: every arm landing on buffer 0 is a working preview at
    # the OLD frame rate, which no runtime check can tell from a working one.
    # The REAL module is compiled against the REAL SDK hx_drv_xdma.h (the
    # shimmed WE2_device.h resolves first, exactly as for the timer seam), and
    # the mock keeps the accessors' disassembled quirks -- halfword-writing
    # getters, setters that return success unconditionally -- because a mock
    # politer than the hardware passes callers the hardware fails.
    gcc $CFLAGS \
        -I "$here" -I "$here/shim" \
        -I "$board/port/camera" \
        -I "$sdk/drivers/inc" \
        "$here/test_cam_wdma3.c" \
        "$board/port/camera/cam_wdma3.c" \
        $LDFLAGS -o "$out/test_cam_wdma3"
    "$out/test_cam_wdma3"
else
    echo "  (skipped test_timer_seam: no Himax SDK at $sdk --" \
         "configure a grove-vision-ai-v2 build tree first)"
fi

# issue #30 -- the measure-then-wrap IRQ accounting (port/sdk_seam/epk_irq_wrap.c).
# The wrap is a TRANSACTION over hardware, and its failure mode is invisible: a
# bring-up that wrapped two lines and then failed on the third used to leave them
# enabled and registered while the caller closed the device underneath, which
# made cpu% permanently untrustworthy with nothing to show for it.  Reproducing
# that on hardware means engineering a driver failure, so the NVIC (enable bits
# AND the writable vector table) is modelled in test/shim and the real
# epk_irq_wrap.c is compiled against it.  The sweep the tests run after every
# step is the invariant AGENTS.md states: every line is either DISABLED, or
# wrapped AND registered -- never a third thing.
gcc $CFLAGS -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
    -I "$here" -I "$here/shim" \
    -I "$board/port/sdk_seam" -I "$board/port/threadx" -I "$board/svc" \
    "$here/test_epk_irq_wrap.c" "$here/seam_host_env.c" \
    "$board/port/sdk_seam/epk_irq_wrap.c" \
    $LDFLAGS -o "$out/test_epk_irq_wrap"
"$out/test_epk_irq_wrap"

# issue #46 -- the Ethos-U command-stream payload check (port/npu/npu_payload.c).
# No shim and no SDK header: the walk is deliberately plain C over plain bytes,
# which is why it was split out of npu_tflm.cc.  It is a SAFETY check -- it is
# what stops npu_open() accepting a model whose driver actions continue past the
# launch, where the driver can return without ever handing the arena back -- so
# the reject cases matter as much as the accept ones, and neither can be
# exercised on the board without deliberately building a broken model.
gcc $CFLAGS \
    -I "$here" -I "$board/port/npu" \
    "$here/test_npu_payload.c" "$board/port/npu/npu_payload.c" \
    $LDFLAGS -o "$out/test_npu_payload"
"$out/test_npu_payload"

# issue #45 -- the BlazeFace decoder (port/npu/models/blazeface.c).  The REAL
# decoder is compiled, which is possible at all because it takes tensor
# DESCRIPTORS instead of reaching into the NPU singleton.  Its failure modes are
# a scale factor or an offset -- boxes that are still boxes, in the wrong place,
# on an image nobody can see -- and each hypothesis otherwise costs a flash
# cycle of a NOR with ~100k of them.  The case that earns the file is the
# candidate cap: the donor implementation stops decoding when its buffer fills,
# so a busy frame drops the strongest face if it lands in the 8x8 group, which
# is scanned last.
#
# No shim: npu.h is plain C with no hardware in it, and the test supplies
# npu_tensor_is_int8() itself (on the board it lives in npu_tflm.cc, where a
# static_assert pins the enumerator).
gcc $CFLAGS \
    -I "$here" -I "$board/port/npu" -I "$board/port/npu/models" \
    "$here/test_blazeface.c" "$board/port/npu/models/blazeface.c" \
    $LDFLAGS -o "$out/test_blazeface"
"$out/test_blazeface"

# issue #45 -- the flash partition check (cmake/check_flash_partitions.py).
# A gate over a destructive, unrecoverable-in-place operation: the model and the
# firmware are written to the same NOR by an xmodem tool that erases blocks and
# reads nothing back.  Its cases cannot be produced on hardware without
# destroying a partition to see whether the check would have stopped it.
python3 "$here/test_flash_partitions.py"

# issue #35 -- the planar B/G/R -> RGB565 packer (port/camera/cam_convert.c).
# No shim and no SDK header: this translation unit is deliberately pure
# arithmetic over memory, which is what lets it be tested here at all.  Every
# bug it can have -- swapped R and B, the wrong endianness in the slot, a plane
# stride off by one -- comes out of the board as a photograph that looks
# plausible and is wrong, and each hypothesis costs a flash cycle to test.
gcc $CFLAGS \
    -I "$here" -I "$board/port/camera" \
    "$here/test_cam_convert.c" "$board/port/camera/cam_convert.c" \
    $LDFLAGS -o "$out/test_cam_convert"
"$out/test_cam_convert"

# issue #48 -- the camera frame -> input tensor resize, and the detection box ->
# frame pixel map (port/npu/nn_preproc.c).  Two mappings that MUST be one
# transform: sampling carries the half-pixel term and a box edge must not, and
# getting that wrong shifts every box by half a source pixel -- invisible
# against a face, and a flash cycle per hypothesis to chase on hardware.  Also
# where the negative-coordinate upscale path is pinned, since C's toward-zero
# cast is off by one exactly there.
gcc $CFLAGS \
    -I "$here" -I "$board/port/npu" \
    "$here/test_nn_preproc.c" "$board/port/npu/nn_preproc.c" \
    $LDFLAGS -o "$out/test_nn_preproc"
"$out/test_nn_preproc"

# issue #35 -- the auto exposure / white balance control laws
# (port/camera/cam_auto.c).  A control loop's failure modes -- oscillation,
# runaway, hunting on noise -- all look fine in a single capture and are
# miserable in a live preview.  Running the loop against a simulated sensor for
# a few hundred iterations catches them; staring at a panel does not.
gcc $CFLAGS \
    -I "$here" -I "$board/port/camera" \
    "$here/test_cam_auto.c" "$board/port/camera/cam_auto.c" \
    $LDFLAGS -o "$out/test_cam_auto"
"$out/test_cam_auto"

# issue #42 -- the floating-point precondition's verdict
# (port/threadx/fp_enforce.c).  The check it belongs to halts the board before
# the scheduler starts, and the boots that would make it halt cannot be produced
# on hardware -- nothing can ask this part to refuse an FPCCR write.  The
# decision was split out from the halt for this file, because the alternative is
# a gate nobody has ever seen fail, which this repository already has one of
# (issue #66).
gcc $CFLAGS \
    -I "$here" -I "$board/port/threadx" \
    "$here/test_fp_enforce.c" "$board/port/threadx/fp_enforce.c" \
    $LDFLAGS -o "$out/test_fp_enforce"
"$out/test_fp_enforce"

# issue #65 -- the stop's decision table (port/camera/cam_state.c).  The stop
# has to separate three answers that all look like "not streaming": poisoned,
# nothing to stop, and the API mutex never came free.  Only the middle one may
# report success, because success is what permits the caller to detach its sink.
# The case that matters -- a stop that waited for the mutex and woke up owning it
# with the port already poisoned by another stop -- needs two stops overlapping
# on a board with one shell, so it cannot be produced on hardware at all.
gcc $CFLAGS \
    -I "$here" -I "$board/port/camera" \
    "$here/test_cam_stop.c" "$board/port/camera/cam_state.c" \
    $LDFLAGS -o "$out/test_cam_stop"
"$out/test_cam_stop"

# issue #68 -- the EDM observer's bookkeeping (port/camera/cam_edm.c).  Nothing
# a console can type makes EDM fire: the event has been seen twice, both times
# inside a hang, and it cannot be asked for again.  So the accumulation and the
# "first, then every power of two" policy would otherwise be code added to
# explain a failure that has never once run -- the shape of gate this repository
# has already been bitten by (issues #66, #42).  The case that carries the point
# is saturation: a counter that wrapped would restart the geometric trail and
# flood the one log ring that survives the recovery reset.
gcc $CFLAGS \
    -I "$here" -I "$board/port/camera" \
    "$here/test_cam_edm.c" "$board/port/camera/cam_edm.c" \
    $LDFLAGS -o "$out/test_cam_edm"
"$out/test_cam_edm"

# issue #35 -- the CSI FIFO fill computation (port/camera/cam_mipi_calc.c).  The
# firmware rewrote the SDK's double + ceil() formula in integer arithmetic
# because this link has no libm; this checks the rewrite against the ORIGINAL
# formula, transcribed in the test and swept over the whole parameter space,
# rather than against numbers somebody worked out by hand.  -lm is for the
# reference transcription's ceil() -- the host has a libm even though the board
# does not, which is the whole reason this comparison can be made at all.
gcc $CFLAGS \
    -I "$here" -I "$board/port/camera" \
    "$here/test_cam_mipi_calc.c" "$board/port/camera/cam_mipi_calc.c" \
    $LDFLAGS -lm -o "$out/test_cam_mipi_calc"
"$out/test_cam_mipi_calc"
