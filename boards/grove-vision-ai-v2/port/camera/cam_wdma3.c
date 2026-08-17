/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * The WDMA3 landing-buffer transition machine (issue #59).  See cam_wdma3.h
 * for the hardware rules and why this file is its own module; what lives here
 * is only the machine itself, written so a host test can compile it unchanged.
 *
 * Everything speaks through the vendor's per-register accessors rather than
 * raw MMIO, for two reasons: the register base is the driver's private state
 * (there is nothing to point at without it), and the accessors' encodings --
 * THR is size >> 2, the getters shift it back -- are the same ones
 * set_WDMA3allCfg() uses, so a readback compared against a programmed value is
 * comparing like with like.  All of that is disassembled fact, not header
 * documentation; the header documents none of it.
 *
 * [!] EVERY GETTER OUTPUT IS ZERO-INITIALISED before the call.  The 16-bit
 * getters store with a halfword write into whatever the pointer aims at
 * (disassembled: strh), so a wider variable keeps its upper half.  An enum is
 * int-sized, and a stack enum's upper half is stack garbage -- which would
 * turn "status is clean" into a coin toss.
 */
#include <stddef.h>
#include <stdint.h>

#include "hx_drv_xdma.h"

#include "cam_wdma3.h"

/* ---- state --------------------------------------------------------------- */

static uint32_t w3_base[CAM_WDMA3_BUFFERS];
static uint32_t w3_off[3];
static uint32_t w3_size[3];

static uint8_t  w3_have_layout;
static uint8_t  w3_armed;   /* index the write-DMA is (or was last) armed at */
static uint8_t  w3_read;    /* index the CPU may read                        */

static uint32_t w3_completions[CAM_WDMA3_BUFFERS];
static uint32_t w3_premature;      /* cumulative, deliberately */

static const char *w3_fault_reason;
static uint32_t    w3_fault_st;

/* First reason wins, as everywhere else in this port: the failure that
 * started the trouble is the one worth reading. */
static int w3_fail(const char *why, uint32_t st)
{
	if (w3_fault_reason == NULL) {
		w3_fault_reason = why;
		w3_fault_st     = st;
	}
	return -1;
}

/* ---- the readback -------------------------------------------------------- */

/*
 * Read the six channel registers back and compare them with @base's layout.
 * The getters echo programmed values (see the header note), so agreement means
 * "nobody moved the configuration" -- which is the exact claim both callers
 * need: frame_complete asks it of the buffer a frame just landed in, arm_next
 * asks it of the buffer it just programmed.
 */
static int w3_verify(uint32_t base)
{
	uint32_t i;

	for (i = 0u; i < 3u; i++) {
		uint32_t a = 0u, s = 0u;
		XDMA_ERROR_E rc;

		switch (i) {
		case 0u:  rc = hx_drv_xdma_get_WDMA3Ch1AddrSize(&a, &s); break;
		case 1u:  rc = hx_drv_xdma_get_WDMA3Ch2AddrSize(&a, &s); break;
		default:  rc = hx_drv_xdma_get_WDMA3Ch3AddrSize(&a, &s); break;
		}
		if (rc != XDMA_NO_ERROR)
			return w3_fail("a WDMA3 channel register would not read "
			               "back", 0u);
		if (a != base + w3_off[i] || s != w3_size[i])
			return w3_fail("the WDMA3 channel configuration is not "
			               "the buffer this port armed", 0u);
	}
	return 0;
}

/* ---- the transitions ----------------------------------------------------- */

int cam_wdma3_capture_layout(const uint32_t base[CAM_WDMA3_BUFFERS],
                             uint32_t bytes)
{
	uint32_t a[3] = { 0u, 0u, 0u };
	uint32_t s[3] = { 0u, 0u, 0u };
	uint32_t gap;

	w3_have_layout  = 0u;
	w3_fault_reason = NULL;
	w3_fault_st     = 0u;

	if (base == NULL || bytes == 0u)
		return w3_fail("no landing buffers were offered", 0u);
	gap = (base[1] > base[0]) ? (base[1] - base[0]) : (base[0] - base[1]);
	if (gap < bytes)
		return w3_fail("the two landing buffers overlap", 0u);

	/*
	 * The vendor just derived the three channels from its own global and
	 * programmed them for buffer 0.  Read back what it ACTUALLY wrote and
	 * adopt that as the layout, instead of re-deriving it from
	 * hx_drv_hw5x5_get_OutMEMSize() and hoping the two derivations agree.
	 * The chain check below is what keeps "adopt" from meaning "trust":
	 * a layout that does not start at buffer 0, is not contiguous, or does
	 * not sum to one frame is refused here, before any stream runs on it.
	 */
	if (hx_drv_xdma_get_WDMA3Ch1AddrSize(&a[0], &s[0]) != XDMA_NO_ERROR ||
	    hx_drv_xdma_get_WDMA3Ch2AddrSize(&a[1], &s[1]) != XDMA_NO_ERROR ||
	    hx_drv_xdma_get_WDMA3Ch3AddrSize(&a[2], &s[2]) != XDMA_NO_ERROR)
		return w3_fail("the WDMA3 channel registers would not read "
		               "back at configuration", 0u);

	if (s[0] == 0u || s[1] == 0u || s[2] == 0u)
		return w3_fail("a WDMA3 channel was configured with no size",
		               0u);
	if (a[0] != base[0] ||
	    a[1] != a[0] + s[0] ||
	    a[2] != a[1] + s[1])
		return w3_fail("the configured WDMA3 channels do not tile "
		               "landing buffer 0", 0u);
	if (s[0] + s[1] + s[2] != bytes)
		return w3_fail("the configured WDMA3 channels do not sum to "
		               "one frame", 0u);

	w3_base[0] = base[0];
	w3_base[1] = base[1];
	w3_off[0]  = 0u;
	w3_off[1]  = s[0];
	w3_off[2]  = s[0] + s[1];
	w3_size[0] = s[0];
	w3_size[1] = s[1];
	w3_size[2] = s[2];

	w3_armed = 0u;
	w3_read  = 0u;
	w3_completions[0] = 0u;
	w3_completions[1] = 0u;
	/* w3_premature is cumulative: a rare event must survive the next
	 * configuration, or it can only ever be seen by whoever was watching. */

	w3_have_layout = 1u;
	return 0;
}

void cam_wdma3_reset(void)
{
	w3_have_layout  = 0u;
	w3_armed        = 0u;
	w3_read         = 0u;
	w3_fault_reason = NULL;
	w3_fault_st     = 0u;
}

int cam_wdma3_frame_complete(void)
{
	if (!w3_have_layout)
		return w3_fail("a frame completed with no WDMA3 layout "
		               "captured", 0u);
	if (w3_verify(w3_base[w3_armed]) != 0)
		return -1;   /* w3_verify latched the reason */

	w3_read = w3_armed;
	w3_completions[w3_read]++;
	return 0;
}

int cam_wdma3_arm_next(void)
{
	uint16_t saved = 0u;
	uint32_t next, base, abn;
	/* Int-sized enum, halfword-written getter: the zero-init is
	 * load-bearing.  See the file comment. */
	XDMA_WDMA3STATUS_E st = XDMA_WDMA3STATUS_NO;
	int rc = -1;

	if (!w3_have_layout)
		return w3_fail("an arm was requested with no WDMA3 layout "
		               "captured", 0u);

	next = (uint32_t)w3_read ^ 1u;
	base = w3_base[next];

	/*
	 * Mask BEFORE the disable and restore only at the single exit below.
	 * The order of the two mask writes against the disable is the whole
	 * point; everything between them is the window in which a
	 * premature-disable status may latch without becoming an interrupt.
	 */
	if (hx_drv_xdma_get_WDMA3INTMask(&saved) != XDMA_NO_ERROR)
		return w3_fail("the WDMA3 interrupt mask would not read back",
		               0u);
	(void)hx_drv_xdma_set_WDMA3INTMask(0xFFFFu);
	(void)hx_drv_xdma_set_enable(0u, 0u, 0u, 0u);

	/* The setters return zero unconditionally (disassembled), so their
	 * return values prove nothing; the readback below is the evidence. */
	(void)hx_drv_xdma_set_WDMA3Ch1AddrSize(base + w3_off[0], w3_size[0]);
	(void)hx_drv_xdma_set_WDMA3Ch2AddrSize(base + w3_off[1], w3_size[1]);
	(void)hx_drv_xdma_set_WDMA3Ch3AddrSize(base + w3_off[2], w3_size[2]);
	if (w3_verify(base) != 0)
		goto out;   /* reason latched; xDMA stays disabled, no re-arm */

	/*
	 * The status audit.  NORMAL_FINISH is discarded, not asserted clear:
	 * the vendor's ISR acknowledges it before delivering frame-ready, but
	 * whether this readback then returns zero is undocumented, and a check
	 * that faults on every frame is as useless as one that cannot fault.
	 */
	if (hx_drv_xdma_get_WDMA3INTStatus(&st) != XDMA_NO_ERROR) {
		(void)w3_fail("the WDMA3 status would not read back", 0u);
		goto out;
	}
	abn = (uint32_t)st & ~(uint32_t)XDMA_WDMA3STATUS_NORMAL_FINISH;

	if (abn == (uint32_t)XDMA_WDMA3STATUS_ERR_DIS_BEFORE_FINISH) {
		/*
		 * Provably this port's own doing -- frame-ready already said
		 * the transfer finished, and the disable above is the only
		 * disable in flight -- so it may be acknowledged.  Counted
		 * from THIS sample, before the acknowledge can remove the
		 * evidence, which is what lets acceptance require zero.
		 */
		w3_premature++;
		(void)hx_drv_xdma_WDMA3_clear_AbNormalint();
		/* The acknowledge is aggregate (one bit for all of WDMA3's
		 * abnormal causes) and the SVD gives it no semantics, so the
		 * write alone proves nothing: read back, require clear. */
		st = XDMA_WDMA3STATUS_NO;
		if (hx_drv_xdma_get_WDMA3INTStatus(&st) != XDMA_NO_ERROR) {
			(void)w3_fail("the WDMA3 status would not read back "
			              "after an acknowledge", 0u);
			goto out;
		}
		abn = (uint32_t)st &
		      ~(uint32_t)XDMA_WDMA3STATUS_NORMAL_FINISH;
		if (abn != 0u) {
			(void)w3_fail("a WDMA3 abnormal status survived its "
			              "acknowledge", abn);
			goto out;
		}
	} else if (abn != 0u) {
		/* Not ours to explain away.  A FIFO mismatch or bus error here
		 * belongs to the frame just captured; hiding it under an
		 * acknowledge is the fail-open this refuses to be. */
		(void)w3_fail("an abnormal WDMA3 status was latched at the "
		              "arm", abn);
		goto out;
	}

	cam_wdma3_hw_retrigger();
	w3_armed = (uint8_t)next;
	rc = 0;

out:
	/*
	 * On EVERY exit, the fault paths included: a WDMA3 left masked would
	 * make the next stream's frame-ready never arrive, and that presents
	 * as a timeout in code far from here.  Restoring after the retrigger
	 * is safe -- the next completion is a frame away, and an error latched
	 * while masked is delivered now, into the same sticky latch the
	 * pre-publish check reads.
	 */
	(void)hx_drv_xdma_set_WDMA3INTMask(saved);
	return rc;
}

/* ---- read-side accessors ------------------------------------------------- */

uint32_t cam_wdma3_read_index(void)
{
	return w3_read;
}

uint32_t cam_wdma3_completions(uint32_t idx)
{
	return (idx < CAM_WDMA3_BUFFERS) ? w3_completions[idx] : 0u;
}

uint32_t cam_wdma3_premature_disables(void)
{
	return w3_premature;
}

const char *cam_wdma3_fault(void)
{
	return w3_fault_reason;
}

uint32_t cam_wdma3_fault_status(void)
{
	return w3_fault_st;
}
