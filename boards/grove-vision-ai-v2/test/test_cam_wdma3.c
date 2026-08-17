/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the WDMA3 landing-buffer transition machine (issue #59,
 * port/camera/cam_wdma3.c).
 *
 * WHY THIS EXISTS AS A HOST TEST.  The machine's worst failure is SILENT: a
 * flip that degenerates into a no-op -- every arm landing on buffer 0 --
 * produces a working preview at the OLD frame rate, which no runtime check on
 * the board can tell from a working one, and each hypothesis about it costs a
 * flash cycle of a NOR with ~100k of them.  So the REAL cam_wdma3.c is
 * compiled against the REAL SDK hx_drv_xdma.h (the ABI that matters: the
 * enums, the halfword-writing getters) and driven through
 * configure -> complete -> arm -> complete against mock registers that record
 * every operation in order.
 *
 * The mock keeps the accessors' disassembled quirks: the 16-bit getters store
 * a HALFWORD into whatever the pointer aims at (so a caller that forgets to
 * zero-initialise an int-sized enum keeps stack garbage in the upper half),
 * and the setters return success unconditionally.  A mock politer than the
 * hardware would pass callers the hardware fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hx_drv_xdma.h"
#include "cam_wdma3.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* ---- the register mock ---------------------------------------------------- */

#define J_MAX 64

static struct {
	uint32_t ch_addr[3];
	uint32_t ch_size[3];
	uint16_t mask;
	uint16_t status;
	int      wdma3_en;

	/* fault injection */
	int      sticky_regs;      /* address setters silently ignored     */
	int      sticky_status;    /* the acknowledge does not clear       */
	int      premature_on_disable;
	int      fail_get_mask;

	unsigned clears;
	unsigned retriggers;

	char     journal[J_MAX][32];
	unsigned nops;
} mock;

static void op(const char *fmt, uint32_t a, uint32_t b)
{
	if (mock.nops < J_MAX) {
		snprintf(mock.journal[mock.nops], sizeof mock.journal[0],
		         fmt, (unsigned long)a, (unsigned long)b);
	}
	mock.nops++;
}

/* First index of an op, or -1: the ORDER assertions read from this. */
static int op_index(const char *what)
{
	unsigned i, n = (mock.nops < J_MAX) ? mock.nops : J_MAX;

	for (i = 0u; i < n; i++)
		if (strcmp(mock.journal[i], what) == 0)
			return (int)i;
	return -1;
}

static void mock_reset(void)
{
	memset(&mock, 0, sizeof mock);
}

/* Program the registers the way set_WDMA3allCfg() does for @base: three
 * contiguous channels.  The default sizes are the demosaic leg's equal
 * 76,800s, but nothing in the machine may depend on them being equal. */
static void mock_prime_vendor(uint32_t base, uint32_t s1, uint32_t s2,
                              uint32_t s3)
{
	mock.ch_addr[0] = base;
	mock.ch_addr[1] = base + s1;
	mock.ch_addr[2] = base + s1 + s2;
	mock.ch_size[0] = s1;
	mock.ch_size[1] = s2;
	mock.ch_size[2] = s3;
	mock.wdma3_en   = 1;
}

/* ---- the mocked driver ---------------------------------------------------- */

XDMA_ERROR_E hx_drv_xdma_set_enable(uint8_t w1, uint8_t w2, uint8_t w3,
                                    uint8_t rdma)
{
	op("enable(%lu,%lu)", ((uint32_t)w1 << 1) | w2,
	   ((uint32_t)w3 << 1) | rdma);
	if (w3 == 0u && mock.wdma3_en && mock.premature_on_disable)
		mock.status |= (uint16_t)XDMA_WDMA3STATUS_ERR_DIS_BEFORE_FINISH;
	mock.wdma3_en = w3;
	return XDMA_NO_ERROR;
}

static XDMA_ERROR_E mock_set_ch(unsigned ch, uint32_t addr, uint32_t size)
{
	op("set_ch%lu(%lu)", ch, addr);
	if (!mock.sticky_regs) {
		mock.ch_addr[ch - 1u] = addr;
		mock.ch_size[ch - 1u] = size;
	}
	return XDMA_NO_ERROR;   /* unconditionally, like the real one */
}

static XDMA_ERROR_E mock_get_ch(unsigned ch, uint32_t *addr, uint32_t *size)
{
	*addr = mock.ch_addr[ch - 1u];
	*size = mock.ch_size[ch - 1u];
	return XDMA_NO_ERROR;
}

XDMA_ERROR_E hx_drv_xdma_set_WDMA3Ch1AddrSize(uint32_t a, uint32_t s)
{ return mock_set_ch(1u, a, s); }
XDMA_ERROR_E hx_drv_xdma_set_WDMA3Ch2AddrSize(uint32_t a, uint32_t s)
{ return mock_set_ch(2u, a, s); }
XDMA_ERROR_E hx_drv_xdma_set_WDMA3Ch3AddrSize(uint32_t a, uint32_t s)
{ return mock_set_ch(3u, a, s); }
XDMA_ERROR_E hx_drv_xdma_get_WDMA3Ch1AddrSize(uint32_t *a, uint32_t *s)
{ return mock_get_ch(1u, a, s); }
XDMA_ERROR_E hx_drv_xdma_get_WDMA3Ch2AddrSize(uint32_t *a, uint32_t *s)
{ return mock_get_ch(2u, a, s); }
XDMA_ERROR_E hx_drv_xdma_get_WDMA3Ch3AddrSize(uint32_t *a, uint32_t *s)
{ return mock_get_ch(3u, a, s); }

XDMA_ERROR_E hx_drv_xdma_get_WDMA3INTMask(uint16_t *mask)
{
	if (mock.fail_get_mask)
		return XDMA_UNKNOWN_ERROR;
	*mask = mock.mask;
	return XDMA_NO_ERROR;
}

XDMA_ERROR_E hx_drv_xdma_set_WDMA3INTMask(uint16_t mask)
{
	op("set_mask(%lu)", (uint32_t)mask, 0u);
	mock.mask = mask;
	return XDMA_NO_ERROR;
}

XDMA_ERROR_E hx_drv_xdma_get_WDMA3INTStatus(XDMA_WDMA3STATUS_E *status)
{
	/* HALFWORD store, like the real one's strh: the upper bytes of an
	 * int-sized enum are the caller's problem.  memcpy rather than a cast
	 * so the fidelity survives strict aliasing. */
	memcpy(status, &mock.status, sizeof(uint16_t));
	return XDMA_NO_ERROR;
}

XDMA_ERROR_E hx_drv_xdma_WDMA3_clear_AbNormalint(void)
{
	op("clear_abn", 0u, 0u);
	mock.clears++;
	if (!mock.sticky_status)
		mock.status &= (uint16_t)XDMA_WDMA3STATUS_NORMAL_FINISH;
	return XDMA_NO_ERROR;
}

/* The board's seam: sensordplib_retrigger_capture(), which re-enables. */
void cam_wdma3_hw_retrigger(void)
{
	op("retrigger", 0u, 0u);
	mock.retriggers++;
	mock.wdma3_en = 1;
}

/* ---- fixtures ------------------------------------------------------------- */

#define B0     0x34068A20u
#define BYTES  230400u
#define B1     (B0 + BYTES)
#define PLANE  76800u

static const uint32_t bases[CAM_WDMA3_BUFFERS] = { B0, B1 };

static int capture_ok(void)
{
	mock_prime_vendor(B0, PLANE, PLANE, PLANE);
	return cam_wdma3_capture_layout(bases, BYTES);
}

/* ---- cases ---------------------------------------------------------------- */

static void test_capture_layout(void)
{
	uint32_t odd[CAM_WDMA3_BUFFERS] = { B0, B0 + BYTES - 32u };

	mock_reset();
	CHECK(capture_ok() == 0, "vendor layout refused: %s",
	      cam_wdma3_fault());
	CHECK(cam_wdma3_read_index() == 0u, "fresh layout must read buffer 0");
	CHECK(cam_wdma3_completions(0u) == 0u &&
	      cam_wdma3_completions(1u) == 0u, "counts must restart");

	/* UNEQUAL channel sizes are a layout, not an error: the machine
	 * adopts whatever the vendor programmed, so long as it tiles. */
	mock_reset();
	mock_prime_vendor(B0, PLANE + 32u, PLANE - 64u, PLANE + 32u);
	CHECK(cam_wdma3_capture_layout(bases, BYTES) == 0,
	      "unequal channel sizes must be adopted, not refused");

	/* refusals, each fail-closed before any stream can run on it */
	mock_reset();
	mock_prime_vendor(B0 + 32u, PLANE, PLANE, PLANE);
	CHECK(cam_wdma3_capture_layout(bases, BYTES) != 0,
	      "channel 1 off buffer 0 must refuse");

	mock_reset();
	mock_prime_vendor(B0, PLANE, PLANE, PLANE);
	mock.ch_addr[2] += 64u;   /* not contiguous */
	CHECK(cam_wdma3_capture_layout(bases, BYTES) != 0,
	      "a non-contiguous channel must refuse");

	mock_reset();
	mock_prime_vendor(B0, PLANE, PLANE, PLANE - 32u);
	CHECK(cam_wdma3_capture_layout(bases, BYTES) != 0,
	      "sizes that do not sum to a frame must refuse");

	mock_reset();
	mock_prime_vendor(B0, 0u, PLANE, PLANE);
	CHECK(cam_wdma3_capture_layout(bases, BYTES) != 0,
	      "a zero-size channel must refuse");

	mock_reset();
	mock_prime_vendor(B0, PLANE, PLANE, PLANE);
	CHECK(cam_wdma3_capture_layout(odd, BYTES) != 0,
	      "overlapping landing buffers must refuse");
}

static void test_no_layout_guards(void)
{
	mock_reset();
	cam_wdma3_reset();
	CHECK(cam_wdma3_read_index() == 0u, "reset must read buffer 0");
	CHECK(cam_wdma3_frame_complete() != 0,
	      "frame_complete without a layout must refuse");
	CHECK(cam_wdma3_arm_next() != 0,
	      "arm_next without a layout must refuse");
	CHECK(mock.retriggers == 0u, "nothing may have been armed");
}

/*
 * The heart of it: 0 -> 1 -> 0, each commit verified, each arm programming
 * the OTHER buffer.  This is the case a silent no-op flip fails.
 */
static void test_alternation(void)
{
	char want[32];

	mock_reset();
	mock.mask = 0x0281u;   /* an arbitrary configured mask to preserve */
	CHECK(capture_ok() == 0, "layout");

	/* frame 0 lands in buffer 0 */
	CHECK(cam_wdma3_frame_complete() == 0, "complete 0: %s",
	      cam_wdma3_fault());
	CHECK(cam_wdma3_read_index() == 0u, "frame 0 reads buffer 0");
	CHECK(cam_wdma3_completions(0u) == 1u, "b0 count");

	/* arm 1: mask -> disable -> program b1 -> retrigger -> restore */
	mock.nops = 0u;
	CHECK(cam_wdma3_arm_next() == 0, "arm 1: %s", cam_wdma3_fault());
	CHECK(mock.ch_addr[0] == B1 && mock.ch_addr[1] == B1 + PLANE &&
	      mock.ch_addr[2] == B1 + 2u * PLANE,
	      "arm 1 must program buffer 1's channels");
	CHECK(mock.mask == 0x0281u, "the configured mask must be restored");
	CHECK(mock.retriggers == 1u, "one retrigger");
	CHECK(mock.wdma3_en == 1, "the retrigger re-enables");

	/* the ORDER, from the journal */
	CHECK(op_index("set_mask(65535)") >= 0 &&
	      op_index("set_mask(65535)") < op_index("enable(0,0)"),
	      "mask everything BEFORE the disable");
	snprintf(want, sizeof want, "set_ch1(%lu)", (unsigned long)B1);
	CHECK(op_index("enable(0,0)") < op_index(want),
	      "disable BEFORE the address writes");
	CHECK(op_index(want) < op_index("retrigger"),
	      "programme BEFORE the retrigger");
	CHECK(op_index("retrigger") < op_index("set_mask(641)"),
	      "restore the mask AFTER the retrigger");

	/* frame 1 lands in buffer 1; arm 2 goes back to buffer 0 */
	CHECK(cam_wdma3_frame_complete() == 0, "complete 1: %s",
	      cam_wdma3_fault());
	CHECK(cam_wdma3_read_index() == 1u, "frame 1 reads buffer 1");
	CHECK(cam_wdma3_completions(1u) == 1u, "b1 count");
	CHECK(cam_wdma3_arm_next() == 0, "arm 2");
	CHECK(mock.ch_addr[0] == B0, "arm 2 must program buffer 0 again");
	CHECK(cam_wdma3_frame_complete() == 0, "complete 2");
	CHECK(cam_wdma3_read_index() == 0u, "frame 2 reads buffer 0");
	CHECK(cam_wdma3_completions(0u) == 2u &&
	      cam_wdma3_completions(1u) == 1u, "counts 2/1 after 0,1,0");
}

static void test_complete_mismatch(void)
{
	mock_reset();
	CHECK(capture_ok() == 0, "layout");

	mock.ch_addr[1] += 32u;   /* somebody moved a channel */
	CHECK(cam_wdma3_frame_complete() != 0,
	      "a moved channel must refuse the frame");
	CHECK(cam_wdma3_fault() != NULL, "the reason must be latched");
	CHECK(cam_wdma3_completions(0u) == 0u,
	      "a refused frame must not count");
	CHECK(cam_wdma3_read_index() == 0u, "the read index must not move");
}

static void test_arm_readback_mismatch(void)
{
	mock_reset();
	mock.mask = 0x0281u;
	CHECK(capture_ok() == 0, "layout");
	CHECK(cam_wdma3_frame_complete() == 0, "complete");

	mock.sticky_regs = 1;   /* the setters' writes do not stick */
	CHECK(cam_wdma3_arm_next() != 0,
	      "a readback that does not match must refuse");
	CHECK(mock.retriggers == 0u, "NOTHING may be armed on that path");
	CHECK(mock.wdma3_en == 0, "xDMA stays disabled on that path");
	CHECK(mock.mask == 0x0281u,
	      "the mask must be restored on the failure path too");

	/* and the machine did not adopt the arm: the next complete still
	 * verifies buffer 0 */
	mock.sticky_regs = 0;
	CHECK(cam_wdma3_frame_complete() == 0,
	      "a failed arm must leave the armed index alone");
	CHECK(cam_wdma3_read_index() == 0u, "still buffer 0");
}

static void test_premature_disable(void)
{
	uint32_t before = cam_wdma3_premature_disables();

	mock_reset();
	mock.mask = 0x0281u;
	CHECK(capture_ok() == 0, "layout");
	CHECK(cam_wdma3_frame_complete() == 0, "complete");

	/* the disable raises DIS_BEFORE_FINISH: counted, acknowledged,
	 * re-read clean, and the arm proceeds */
	mock.premature_on_disable = 1;
	CHECK(cam_wdma3_arm_next() == 0,
	      "an acknowledged premature disable must not kill the arm: %s",
	      cam_wdma3_fault());
	CHECK(cam_wdma3_premature_disables() == before + 1u,
	      "the premature disable must be COUNTED (pre-acknowledge)");
	CHECK(mock.clears == 1u, "exactly one acknowledge");
	CHECK(mock.retriggers == 1u, "and the arm went through");
	CHECK(mock.mask == 0x0281u, "mask restored");

	/* cumulative across configurations, deliberately */
	mock.premature_on_disable = 0;
	CHECK(capture_ok() == 0, "reconfigure");
	CHECK(cam_wdma3_premature_disables() == before + 1u,
	      "the count must survive a reconfiguration");
}

static void test_premature_sticky(void)
{
	mock_reset();
	mock.mask = 0x0281u;
	CHECK(capture_ok() == 0, "layout");
	CHECK(cam_wdma3_frame_complete() == 0, "complete");

	mock.premature_on_disable = 1;
	mock.sticky_status = 1;   /* the acknowledge does not clear it */
	CHECK(cam_wdma3_arm_next() != 0,
	      "a status that survives its acknowledge must refuse");
	CHECK(mock.retriggers == 0u, "no arm on that path");
	CHECK(mock.mask == 0x0281u, "mask restored on that path too");
}

static void test_other_abnormal(void)
{
	mock_reset();
	mock.mask = 0x0281u;
	CHECK(capture_ok() == 0, "layout");
	CHECK(cam_wdma3_frame_complete() == 0, "complete");

	/* a bus error is NOT this port's doing: refuse, and do NOT
	 * acknowledge -- hiding it is the fail-open this machine refuses */
	mock.status = (uint16_t)XDMA_WDMA3STATUS_ERR_BUS;
	CHECK(cam_wdma3_arm_next() != 0, "a bus error must refuse the arm");
	CHECK(mock.clears == 0u, "and must NOT be acknowledged");
	CHECK(mock.retriggers == 0u, "and must not arm");
	CHECK(cam_wdma3_fault_status() ==
	      (uint32_t)XDMA_WDMA3STATUS_ERR_BUS,
	      "the raw status must be latched for the log");
	CHECK(mock.mask == 0x0281u, "mask restored");
}

static void test_normal_finish_discarded(void)
{
	mock_reset();
	CHECK(capture_ok() == 0, "layout");
	CHECK(cam_wdma3_frame_complete() == 0, "complete");

	/* whether the vendor ISR's acknowledge makes this read back zero is
	 * undocumented -- the machine must not fault on the bit either way */
	mock.status = (uint16_t)XDMA_WDMA3STATUS_NORMAL_FINISH;
	CHECK(cam_wdma3_arm_next() == 0,
	      "a lingering normal-finish bit must not refuse the arm: %s",
	      cam_wdma3_fault());
	CHECK(mock.clears == 0u, "and needs no acknowledge");
}

static void test_get_mask_failure(void)
{
	mock_reset();
	CHECK(capture_ok() == 0, "layout");
	CHECK(cam_wdma3_frame_complete() == 0, "complete");

	/* a mask that cannot be read cannot be restored: refuse BEFORE
	 * touching anything */
	mock.fail_get_mask = 1;
	mock.nops = 0u;
	CHECK(cam_wdma3_arm_next() != 0,
	      "an unreadable mask must refuse the arm");
	CHECK(mock.nops == 0u, "and must have touched NOTHING");
}

int main(void)
{
	test_capture_layout();
	test_no_layout_guards();
	test_alternation();
	test_complete_mismatch();
	test_arm_readback_mismatch();
	test_premature_disable();
	test_premature_sticky();
	test_other_abnormal();
	test_normal_finish_discarded();
	test_get_mask_failure();

	if (failures == 0)
		printf("test_cam_wdma3: all passed\n");
	return failures;
}
