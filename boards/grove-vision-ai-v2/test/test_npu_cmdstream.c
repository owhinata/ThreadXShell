/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Host test for the Ethos-U55 command-stream range check (issue #184).
 *
 * Compiles the REAL svc/nn/ethosu/npu_cmdstream.c.  Every stream here is built
 * by hand from the TRM tables (Arm Ethos-U55 TRM 102420_0200_02, Table 4-109 /
 * 4-112 / 4-113), so the opcodes below are written out again rather than
 * shared with the implementation: a test that borrowed them would agree with a
 * wrong one.
 *
 * WHAT A BOUNDARY CASE IS.  For each accepted shape the expected end of the
 * range is worked out by hand in the comment next to it, from the formula the
 * TRM gives (TRM 4.9 Table 4-140 for the IFM, the tile / stride registers for
 * the address).  The case passes with the region exactly that big and fails one
 * byte smaller, so an implementation that reads less than the formula, or more,
 * is caught either way.
 *
 * Layout used throughout: region 0 = weights (constant, read-only), 1 = arena
 * (writable), 2 = IFM, 3 = IFM2, 4 = scale (all read-only).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "npu_cmdstream.h"

/* ---- encoding, TRM Table 4-109 / 4-112 / 4-113 ---------------------------- */
#define CMD1 0x4000u

#define STOP 0x000u
#define CONV 0x002u
#define DEPTHWISE 0x003u
#define POOL 0x005u
#define ELEMENTWISE 0x006u
#define DMA_START 0x010u
#define DMA_WAIT 0x011u
#define KERNEL_WAIT 0x012u

#define PAD_TOP 0x100u
#define PAD_LEFT 0x101u
#define PAD_RIGHT 0x102u
#define PAD_BOTTOM 0x103u
#define IFM_DEPTH_M1 0x104u
#define UPSCALE 0x107u
#define OFM_WIDTH_M1 0x111u
#define OFM_HEIGHT_M1 0x112u
#define OFM_DEPTH_M1 0x113u
#define KERNEL_WIDTH_M1 0x120u
#define KERNEL_HEIGHT_M1 0x121u
#define KERNEL_STRIDE 0x122u
#define WEIGHT_REGION 0x128u
#define SCALE_REGION 0x129u
#define DMA0_SRC_REGION 0x130u
#define DMA0_DST_REGION 0x131u
#define IFM2_BROADCAST 0x180u

#define WEIGHT_BASE 0x020u
#define WEIGHT_LENGTH 0x021u
#define SCALE_BASE 0x022u
#define SCALE_LENGTH 0x023u
#define OFM_SCALE 0x024u
#define DMA0_SRC 0x030u
#define DMA0_DST 0x031u
#define DMA0_LEN 0x032u

/* One feature map's commands. */
struct fmc {
	uint32_t region, base0, stride_x, prec, width0, height0, height1;
};
static const struct fmc IFM = { 0x10Fu, 0x000u, 0x004u, 0x105u, 0x10Au, 0x10Bu, 0x10Cu };
static const struct fmc IFM2 = { 0x18Fu, 0x080u, 0x084u, 0x185u, 0x18Au, 0x18Bu, 0x18Cu };
static const struct fmc OFM = { 0x11Fu, 0x010u, 0x014u, 0x114u, 0x11Au, 0x11Bu, 0x11Cu };

/* Precision words.  IFM / IFM2: b0 signed, b[3:2] 0/1/2 = 8/16/32 bit, b[7:6]
 * format.  OFM: b0 signed, b[2:1] precision, b[7:6] format, b8 global scale. */
#define IP(bits, fmt) (1u | ((bits) << 2) | ((fmt) << 6))
#define OP(bits, fmt, global) (1u | ((bits) << 1) | ((fmt) << 6) | ((global) << 8))
#define I8 IP(0u, 0u)
#define O8 OP(0u, 0u, 0u)    /* per-channel scale */
#define O8G OP(0u, 0u, 1u)   /* global scale */

#define SHRAM 16384u
#define BIG (1u << 20)
#define NREG 5u

static uint32_t s[512];
static unsigned n;
static struct npu_cs_region reg[8];
static unsigned nreg;
static unsigned checks;

static void reset(void)
{
	unsigned i;

	n = 0;
	nreg = NREG;
	for (i = 0; i < 8; i++) {
		reg[i].bytes = BIG;
		reg[i].writable = (i == 1);
	}
}

static void c0(uint32_t op, uint32_t p)
{
	assert(n < 512);
	s[n++] = (op & 0x3FFu) | (p << 16);
}

static void c1x(uint32_t op, uint32_t ext, uint32_t v)
{
	assert(n + 1 < 512);
	s[n++] = CMD1 | (op & 0x3FFu) | (ext << 16);
	s[n++] = v;
}

static void c1(uint32_t op, uint32_t v)
{
	c1x(op, 0u, v);
}

static void stop(void)
{
	c0(STOP, 0xFFFFu);
}

/* A feature map with explicit tiles. */
static void fm_tiles(const struct fmc *f, uint32_t region, const uint32_t b[4],
                     uint32_t sx, uint32_t sy, uint32_t sc, uint32_t prec,
                     uint32_t w0m1, uint32_t h0m1, uint32_t h1m1)
{
	unsigned t;

	c0(f->region, region);
	for (t = 0; t < 4; t++)
		c1(f->base0 + t, b[t]);
	c1(f->stride_x, sx);
	c1(f->stride_x + 1u, sy);
	c1(f->stride_x + 2u, sc);
	c0(f->prec, prec);
	c0(f->width0, w0m1);
	c0(f->height0, h0m1);
	c0(f->height1, h1m1);
}

/* A feature map in one tile (tile 0 covers everything up to 65536). */
static void fm(const struct fmc *f, uint32_t region, uint32_t base,
               uint32_t sx, uint32_t sy, uint32_t sc, uint32_t prec)
{
	const uint32_t b[4] = { base, base, base, base };

	fm_tiles(f, region, b, sx, sy, sc, prec, 0xFFFFu, 0xFFFFu, 0xFFFFu);
}

static void ofm_shape(uint32_t h, uint32_t w, uint32_t d)
{
	c0(OFM_HEIGHT_M1, h - 1u);
	c0(OFM_WIDTH_M1, w - 1u);
	c0(OFM_DEPTH_M1, d - 1u);
}

static void kernel(uint32_t kh_m1, uint32_t kw_m1, uint32_t ks,
                   uint32_t pad_top, uint32_t pad_left, uint32_t up)
{
	c0(KERNEL_HEIGHT_M1, kh_m1);
	c0(KERNEL_WIDTH_M1, kw_m1);
	c0(KERNEL_STRIDE, ks);
	c0(PAD_TOP, pad_top);
	c0(PAD_LEFT, pad_left);
	c0(UPSCALE, up);
}

static void weights(uint32_t region, uint32_t base, uint32_t len)
{
	c0(WEIGHT_REGION, region);
	c1(WEIGHT_BASE, base);
	c1(WEIGHT_LENGTH, len);
}

static void scale(uint32_t region, uint32_t base, uint32_t len)
{
	c0(SCALE_REGION, region);
	c1(SCALE_BASE, base);
	c1(SCALE_LENGTH, len);
}

static void dma(uint32_t sr, uint32_t src, uint32_t dr, uint32_t dst,
                uint32_t len)
{
	c0(DMA0_SRC_REGION, sr);
	c1(DMA0_SRC, src);
	c0(DMA0_DST_REGION, dr);
	c1(DMA0_DST, dst);
	c1(DMA0_LEN, len);
	c0(DMA_START, 0u);
	c0(DMA_WAIT, 0u);
}

static enum npu_cs_refusal run_shram(uint32_t shram)
{
	return npu_cmdstream_check(s, n, reg, nreg, shram, NULL);
}

static enum npu_cs_refusal run(void)
{
	return run_shram(SHRAM);
}

static const char *name_of(enum npu_cs_refusal r)
{
	static const char *const names[] = {
		"OK", "BAD_ARGUMENTS", "UNKNOWN_COMMAND", "TRUNCATED_COMMAND",
		"ADDRESS_EXTENSION", "REGION_NOT_PASSED", "DMA_SOURCE_INTERNAL",
		"DMA_STRIDE_MODE", "DMA_DEST_INTERNAL", "STATE_UNSET",
		"BELOW_REGION", "PAST_REGION", "WRITE_TO_DATA", "PARAMETER_RANGE",
		"NO_FINAL_STOP",
	};

	return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

static void expect_r(const char *what, enum npu_cs_refusal got,
                     enum npu_cs_refusal want)
{
	checks++;
	if (got != want) {
		printf("  %-60s FAIL (got %s, want %s)\n", what, name_of(got),
		       name_of(want));
		exit(1);
	}
	printf("  %-60s %s\n", what, name_of(want));
}

static void expect(const char *what, enum npu_cs_refusal want)
{
	expect_r(what, run(), want);
}

/* The stream in s[] needs exactly @p exact bytes of region @p r. */
static void boundary(const char *what, unsigned r, uint32_t exact)
{
	char buf[128];

	reg[r].bytes = exact;
	snprintf(buf, sizeof(buf), "%s: region %u = %u", what, r, exact);
	expect(buf, NPU_CS_OK);
	reg[r].bytes = exact - 1u;
	snprintf(buf, sizeof(buf), "%s: region %u = %u", what, r, exact - 1u);
	expect(buf, NPU_CS_PAST_REGION);
	reg[r].bytes = BIG;
}

/*
 * A 3x3 convolution, stride 1, no padding:
 *   OFM 4x4x16 int8 NHWC in region 1 at 0, strides x 16, y 64, c 1
 *     end = 3*64 + 3*16 + 0 (one brick) + 16*1 = 256
 *   IFM 8 channels int8 NHWC in region 2 at 0, strides x 8, y 48, c 1
 *     rows = (4-1)*1 + 3 - 0 = 6, cols 6, depth 8 -> one brick of 16
 *     end = 5*48 + 5*8 + 0 + 16 = 296
 *   weights region 0 [16, 176), scale region 4 [8, 48)
 */
static void conv_setup(void)
{
	fm(&OFM, 1u, 0u, 16u, 64u, 1u, O8);
	ofm_shape(4u, 4u, 16u);
	fm(&IFM, 2u, 0u, 8u, 48u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(2u, 2u, 0u, 0u, 0u, 0u);
	weights(0u, 16u, 160u);
	scale(4u, 8u, 40u);
}

static void conv_stream(void)
{
	reset();
	conv_setup();
	c0(CONV, 0u);
	stop();
}

/* 2x2x32 pooling / depthwise output, used by several cases below. */
static void out_2x2x32(uint32_t prec)
{
	/* end = 1*64 + 1*32 + (2-1)*1 + 32*1 = 129 */
	fm(&OFM, 1u, 0u, 32u, 64u, 1u, prec);
	ofm_shape(2u, 2u, 32u);
}

static void test_control(void)
{
	struct npu_cs_extent e[NREG];
	enum npu_cs_refusal r;

	printf(" control\n");
	reset();
	dma(0u, 0u, 1u, 2048u, 176u);    /* weights into the arena */
	conv_setup();
	c0(KERNEL_WAIT, 0u);
	c0(CONV, 0u);
	stop();
	expect("DMA region 0 -> 1, conv, stop", NPU_CS_OK);

	r = npu_cmdstream_check(s, n, reg, nreg, SHRAM, e);
	expect_r("same, with the extent report", r, NPU_CS_OK);
	checks++;
	if (!e[0].touched || e[0].lo != 0 || e[0].hi != 176 ||
	    !e[1].touched || e[1].lo != 0 || e[1].hi != 2048 + 176 ||
	    !e[2].touched || e[2].lo != 0 || e[2].hi != 296 ||
	    e[3].touched || !e[4].touched || e[4].lo != 8 || e[4].hi != 48) {
		printf("  extent report FAIL\n");
		exit(1);
	}
	printf("  %-60s ok\n", "extent report: r0 [0,176) r1 [0,2224) r2 [0,296)");

	reset();
	stop();
	expect("a stop alone", NPU_CS_OK);
}

static void test_dma(void)
{
	printf(" DMA\n");
	reset();
	dma(0u, 32u, 1u, 64u, 100u);
	stop();
	boundary("DMA read [32, 132)", 0u, 132u);
	boundary("DMA write [64, 164)", 1u, 164u);

	/* 0xFFFFFFF0 + 0x20 wraps to 0x10 in 32 bits. */
	reset();
	dma(0u, 0xFFFFFFF0u, 1u, 0u, 0x20u);
	stop();
	expect("DMA source end 2^32 + 16 (64-bit sum)", NPU_CS_PAST_REGION);

	reset();
	dma(0u, 0u, 0u, 0u, 16u);
	stop();
	expect("DMA into region 0 (constant)", NPU_CS_WRITE_TO_DATA);

	reset();
	dma(NREG, 0u, 1u, 0u, 16u);
	stop();
	expect("DMA source region = regions passed", NPU_CS_REGION_NOT_PASSED);

	reset();
	dma(0u, 0u, NREG, 0u, 16u);
	stop();
	expect("DMA destination region = regions passed", NPU_CS_REGION_NOT_PASSED);

	reset();
	dma(0x100u, 0u, 1u, 0u, 16u);
	stop();
	expect("DMA source internal", NPU_CS_DMA_SOURCE_INTERNAL);

	reset();
	dma(0x200u, 0u, 1u, 0u, 16u);
	stop();
	expect("DMA source stride mode 1 (2D)", NPU_CS_DMA_STRIDE_MODE);

	reset();
	dma(0u, 0u, 0x401u, 0u, 16u);
	stop();
	expect("DMA destination stride mode 2 (3D)", NPU_CS_DMA_STRIDE_MODE);

	reset();
	dma(0x800u, 0u, 1u, 0u, 16u);
	stop();
	expect("DMA source region bit 11 (undefined)", NPU_CS_PARAMETER_RANGE);

	reset();
	dma(0u, 0u, 8u, 0u, 16u);
	stop();
	expect("DMA destination region 8", NPU_CS_PARAMETER_RANGE);

	reset();
	c0(DMA0_SRC_REGION, 0u);
	c1(DMA0_SRC, 0u);
	c0(DMA0_DST_REGION, 1u);
	c1(DMA0_DST, 0u);
	c1(DMA0_LEN, 16u);
	c0(DMA_START, 16u);      /* channel 1 */
	stop();
	expect("DMA start on channel 1", NPU_CS_PARAMETER_RANGE);

	reset();
	c0(DMA0_SRC_REGION, 0u);
	c1(DMA0_SRC, 0u);
	c0(DMA0_DST_REGION, 1u);
	c1(DMA0_DST, 0u);
	c0(DMA_START, 0u);
	stop();
	expect("DMA start with no length set", NPU_CS_STATE_UNSET);
}

/* U5: lookup-table loads into the last 2 KB of the SHRAM. */
static void test_lut_dma(void)
{
	printf(" DMA into NPU-internal memory (lookup table)\n");
	reset();
	dma(0u, 0u, 0x103u, 14336u, 2048u);
	stop();
	expect("LUT [14336, 16384) of 16384", NPU_CS_OK);

	reset();
	dma(0u, 0u, 0x103u, 15360u, 1024u);
	stop();
	expect("LUT [15360, 16384)", NPU_CS_OK);

	reset();
	dma(0u, 100u, 0x103u, 14336u, 256u);
	stop();
	boundary("LUT source read [100, 356)", 0u, 356u);

	reset();
	dma(0u, 0u, 0x103u, 14335u, 1u);
	stop();
	expect("LUT starting 1 byte before the last 2 KB", NPU_CS_DMA_DEST_INTERNAL);

	reset();
	dma(0u, 0u, 0x103u, 15360u, 1025u);
	stop();
	expect("LUT ending 1 byte past the SHRAM", NPU_CS_DMA_DEST_INTERNAL);

	reset();
	dma(0u, 0u, 0x103u, 14336u, 0u);
	stop();
	expect("LUT of length 0", NPU_CS_DMA_DEST_INTERNAL);

	reset();
	dma(0u, 0u, 0x103u, 0xFFFFFFFFu, 2u);
	stop();
	expect("LUT at 2^32 - 1 (64-bit sum)", NPU_CS_DMA_DEST_INTERNAL);

	reset();
	dma(0u, 0u, 0x101u, 14336u, 256u);
	stop();
	expect("internal destination, core mask 1", NPU_CS_DMA_DEST_INTERNAL);

	reset();
	dma(0u, 0u, 0x100u, 14336u, 256u);
	stop();
	expect("internal destination, core mask 0", NPU_CS_DMA_DEST_INTERNAL);

	reset();
	dma(0u, 0u, 0x303u, 14336u, 256u);
	stop();
	expect("internal destination, stride mode 1", NPU_CS_DMA_STRIDE_MODE);

	reset();
	dma(0u, 0u, 0x103u, 47104u, 2048u);
	stop();
	expect_r("LUT [47104, 49152) with a 48 KB SHRAM", run_shram(49152u), NPU_CS_OK);
	expect("same with a 16 KB SHRAM", NPU_CS_DMA_DEST_INTERNAL);

	reset();
	dma(0u, 0u, 0x103u, 14336u, 2048u);
	stop();
	expect_r("LUT [14336, 16384) with a 48 KB SHRAM", run_shram(49152u),
	         NPU_CS_DMA_DEST_INTERNAL);
}

static void test_conv(void)
{
	printf(" convolution\n");
	conv_stream();
	boundary("conv 3x3 s1: OFM", 1u, 256u);
	boundary("conv 3x3 s1: IFM 6x6x8", 2u, 296u);
	boundary("conv: weights [16, 176)", 0u, 176u);
	boundary("conv: scale [8, 48)", 4u, 48u);

	/* conv reads IFM_DEPTH channels even when the OFM is deeper: OFM 4x4x48
	 * (x 48, y 192: 3*192 + 3*48 + (3-1)*1 + 48 = 770), IFM still 296 */
	reset();
	conv_setup();
	c1(OFM.stride_x, 48u);
	c1(OFM.stride_x + 1u, 192u);
	c0(OFM_DEPTH_M1, 47u);
	c0(CONV, 0u);
	stop();
	boundary("conv OFM 4x4x48", 1u, 770u);
	boundary("conv IFM depth 8 under a 48-deep OFM", 2u, 296u);

	/* stride 2: rows = 3*2 + 3 = 9, IFM y stride 72: 8*72 + 8*8 + 16 = 656 */
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 72u);
	c0(KERNEL_STRIDE, 0x3u);           /* b0, b1: stride - 1 low bits */
	c0(CONV, 0u);
	stop();
	boundary("conv stride 2: IFM 9x9", 2u, 656u);

	/* stride 3: rows 3*3 + 3 = 12, y stride 96: 11*96 + 11*8 + 16 = 1160 */
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 96u);
	c0(KERNEL_STRIDE, 0x240u);         /* b[8:6] = 1, b[11:9] = 1 */
	c0(CONV, 0u);
	stop();
	boundary("conv stride 3: IFM 12x12", 2u, 1160u);

	/* stride x 3, y 2: rows 9, cols 12: 8*96 + 11*8 + 16 = 872 */
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 96u);
	c0(KERNEL_STRIDE, 0x42u);          /* b[8:6] = 1 (x), b1 = 1 (y) */
	c0(CONV, 0u);
	stop();
	boundary("conv stride x3 y2: IFM 9 rows x 12 cols", 2u, 872u);

	/* 3x3 kernel, dilation 2: KERNEL_*_M1 = (3-1)*2 = 4 is already the
	 * dilated size - 1 (TRM 0x120 / 0x121).  rows = 3 + 5 = 8, y stride 64:
	 * 7*64 + 7*8 + 16 = 520.  Applying dilation again would need 12 rows. */
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 64u);
	c0(KERNEL_HEIGHT_M1, 4u);
	c0(KERNEL_WIDTH_M1, 4u);
	c0(KERNEL_STRIDE, 0x18u);          /* b3, b4: dilation 2 */
	c0(CONV, 0u);
	stop();
	boundary("conv dilation 2 (M1 already dilated): IFM 8x8", 2u, 520u);

	/* pad top / left 1 subtract; bottom / right do not (conservative):
	 * rows = 3 + 3 - 1 = 5, y stride 40: 4*40 + 4*8 + 16 = 208 */
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 40u);
	c0(PAD_TOP, 1u);
	c0(PAD_LEFT, 1u);
	c0(PAD_BOTTOM, 1u);
	c0(PAD_RIGHT, 1u);
	c0(CONV, 0u);
	stop();
	boundary("conv pad 1 all sides: IFM 5x5", 2u, 208u);

	/* 2x2 kernel: rows = 3 + 2 = 5; upscale halves (ceil) to 3:
	 * y stride 24: 2*24 + 2*8 + 16 = 80; without upscale 4*24 + 4*8 + 16 = 144 */
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 24u);
	c0(KERNEL_HEIGHT_M1, 1u);
	c0(KERNEL_WIDTH_M1, 1u);
	c0(CONV, 0u);
	stop();
	boundary("conv 2x2, upscale 0: IFM 5x5", 2u, 144u);
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 24u);
	c0(KERNEL_HEIGHT_M1, 1u);
	c0(KERNEL_WIDTH_M1, 1u);
	c0(UPSCALE, 1u);
	c0(CONV, 0u);
	stop();
	boundary("conv 2x2, upscale 1: IFM 3x3", 2u, 80u);
	reset();
	conv_setup();
	c1(IFM.stride_x + 1u, 24u);
	c0(KERNEL_HEIGHT_M1, 1u);
	c0(KERNEL_WIDTH_M1, 1u);
	c0(UPSCALE, 2u);
	c0(CONV, 0u);
	stop();
	boundary("conv 2x2, upscale 2: IFM 3x3", 2u, 80u);

	/* a padding larger than the reach still reads one row / column:
	 * rows = 3 + 3 - 100 < 1 -> 1: end = 0 + 0 + 16 = 16 */
	reset();
	conv_setup();
	c0(PAD_TOP, 100u);
	c0(PAD_LEFT, 100u);
	c0(CONV, 0u);
	stop();
	boundary("conv padding past the kernel: IFM 1x1", 2u, 16u);
}

static void test_depthwise_pool(void)
{
	printf(" depthwise / pooling\n");
	/* IFM depth = max(IFM 8, OFM 32) = 32 = two bricks, 1x1 kernel, 2x2:
	 * IFM strides x 8, y 16, c 1: 1*16 + 1*8 + (2-1)*1 + 32 = 57 */
	reset();
	out_2x2x32(O8);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	weights(0u, 0u, 64u);
	scale(4u, 0u, 32u);
	c0(DEPTHWISE, 0u);
	stop();
	boundary("depthwise: IFM depth max(8, 32)", 2u, 57u);
	boundary("depthwise: OFM 2x2x32", 1u, 129u);
	boundary("depthwise: weights", 0u, 64u);

	/* pooling: no weights, global scale, scale never set */
	static const uint32_t modes[] = { 0u, 1u };
	unsigned i;

	for (i = 0; i < 2; i++) {
		reset();
		out_2x2x32(O8G);
		fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
		c0(IFM_DEPTH_M1, 7u);
		kernel(0u, 0u, 0u, 0u, 0u, 0u);
		c0(POOL, modes[i]);
		stop();
		boundary(i ? "average pool: IFM depth max(8, 32)"
		           : "max pool: IFM depth max(8, 32)", 2u, 57u);
	}

	/* REDUCE_SUM reads IFM_DEPTH: 8 -> one brick: 16 + 8 + 0 + 16 = 40 */
	reset();
	out_2x2x32(O8G);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	c0(POOL, 2u);
	stop();
	boundary("reduce_sum: IFM depth 8 (not the OFM's)", 2u, 40u);

	reset();
	out_2x2x32(O8G);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	c0(POOL, 3u);
	stop();
	expect("pool mode 3", NPU_CS_PARAMETER_RANGE);

	/* per-channel pool: the scale stream is used */
	reset();
	out_2x2x32(O8);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	c0(POOL, 1u);
	stop();
	expect("per-channel pool, scale never set", NPU_CS_STATE_UNSET);

	reset();
	out_2x2x32(O8);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	scale(4u, 8u, 40u);
	c0(POOL, 1u);
	stop();
	boundary("per-channel pool: scale [8, 48)", 4u, 48u);

	/* global pool, but a scale length is set: counted (conservative) */
	reset();
	out_2x2x32(O8G);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	scale(4u, 8u, 40u);
	c0(POOL, 0u);
	stop();
	boundary("global pool, scale length set: scale [8, 48)", 4u, 48u);

	reset();
	out_2x2x32(O8G);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	scale(4u, 8u, 0u);
	reg[4].bytes = 0u;
	c0(POOL, 0u);
	stop();
	expect("global pool, scale length 0: scale not read", NPU_CS_OK);

	reset();
	out_2x2x32(O8G);
	fm(&IFM, 2u, 0u, 8u, 16u, 1u, I8);
	c1(SCALE_LENGTH, 40u);             /* length only */
	c0(IFM_DEPTH_M1, 7u);
	kernel(0u, 0u, 0u, 0u, 0u, 0u);
	c0(POOL, 0u);
	stop();
	expect("global pool, scale length set but no base", NPU_CS_STATE_UNSET);
}

/* Elementwise OFM 2x3x32 int8 NHWC, strides x 32, y 96, c 1:
 * end = 1*96 + 2*32 + (2-1)*1 + 32 = 193.  IFM has the OFM's shape. */
static void ew_setup(uint32_t ifm_depth)
{
	fm(&OFM, 1u, 0u, 32u, 96u, 1u, O8G);
	ofm_shape(2u, 3u, 32u);
	fm(&IFM, 2u, 0u, 32u, 96u, 1u, I8);
	c0(IFM_DEPTH_M1, ifm_depth - 1u);
}

static void test_elementwise(void)
{
	printf(" elementwise\n");
	reset();
	ew_setup(32u);
	fm(&IFM2, 3u, 0u, 32u, 96u, 1u, I8);
	c0(IFM2_BROADCAST, 0u);
	c0(ELEMENTWISE, 1u);               /* ADD */
	stop();
	boundary("ADD: OFM 2x3x32", 1u, 193u);
	boundary("ADD: IFM 2x3x32", 2u, 193u);
	boundary("ADD: IFM2 2x3x32", 3u, 193u);

	/* IFM depth follows the OFM when IFM_DEPTH is smaller */
	reset();
	ew_setup(8u);
	fm(&IFM2, 3u, 0u, 32u, 96u, 1u, I8);
	c0(IFM2_BROADCAST, 0u);
	c0(ELEMENTWISE, 1u);
	stop();
	boundary("ADD, IFM_DEPTH 8: IFM depth max(8, 32)", 2u, 193u);

	/* broadcast H: 0 + 64 + 1 + 32 = 97; W: 96 + 0 + 1 + 32 = 129;
	 * C: 96 + 64 + 0 + 16 = 176 */
	static const struct { uint32_t bc, end; const char *what; } bcs[] = {
		{ 0x1u, 97u, "ADD broadcast H: IFM2 1x3x32" },
		{ 0x2u, 129u, "ADD broadcast W: IFM2 2x1x32" },
		{ 0x4u, 176u, "ADD broadcast C: IFM2 2x3x1" },
		{ 0x43u, 33u, "ADD broadcast H+W, operand order: IFM2 1x1x32" },
	};
	unsigned i;

	for (i = 0; i < sizeof(bcs) / sizeof(bcs[0]); i++) {
		reset();
		ew_setup(32u);
		fm(&IFM2, 3u, 0u, 32u, 96u, 1u, I8);
		c0(IFM2_BROADCAST, bcs[i].bc);
		c0(ELEMENTWISE, 1u);
		stop();
		boundary(bcs[i].what, 3u, bcs[i].end);
	}

	reset();
	ew_setup(32u);
	c0(IFM2_BROADCAST, 0x80u);         /* scalar */
	reg[3].bytes = 0u;
	c0(ELEMENTWISE, 0u);               /* MUL */
	stop();
	expect("MUL with a scalar: IFM2 not set, not read", NPU_CS_OK);

	reset();
	ew_setup(32u);
	c0(ELEMENTWISE, 1u);
	stop();
	expect("ADD with IFM2_BROADCAST never set", NPU_CS_STATE_UNSET);

	reset();
	ew_setup(32u);
	c0(IFM2_BROADCAST, 0u);
	c0(ELEMENTWISE, 1u);
	stop();
	expect("ADD with IFM2 never set", NPU_CS_STATE_UNSET);

	for (i = 5; i <= 7; i++) {
		char buf[64];

		reset();
		ew_setup(32u);
		reg[3].bytes = 0u;
		c0(ELEMENTWISE, i);        /* LReLU / ABS / CLZ */
		stop();
		snprintf(buf, sizeof(buf), "unary mode %u: IFM2 not used", i);
		expect(buf, NPU_CS_OK);
	}
	/* SHR / SHL (8, 9) are binary */
	reset();
	ew_setup(32u);
	c0(ELEMENTWISE, 8u);
	stop();
	expect("SHR (binary) with IFM2 never set", NPU_CS_STATE_UNSET);

	reset();
	ew_setup(32u);
	c0(ELEMENTWISE, 10u);
	stop();
	expect("elementwise mode 10", NPU_CS_PARAMETER_RANGE);

	reset();
	ew_setup(32u);
	c0(IFM2_BROADCAST, 0x08u);
	stop();
	expect("IFM2_BROADCAST bit 3 (undefined)", NPU_CS_PARAMETER_RANGE);

	/* per-channel elementwise uses the scale stream */
	reset();
	fm(&OFM, 1u, 0u, 32u, 96u, 1u, O8);
	ofm_shape(2u, 3u, 32u);
	fm(&IFM, 2u, 0u, 32u, 96u, 1u, I8);
	c0(IFM_DEPTH_M1, 31u);
	c0(ELEMENTWISE, 6u);
	stop();
	expect("per-channel ABS, scale never set", NPU_CS_STATE_UNSET);
}

/* Tiles: 4x4x16 int8 NHWC, strides x 16, y 32, c 1, WIDTH0 2, HEIGHT0 2,
 * HEIGHT1 3 (TRM 0x11A-0x11C).  Per tile (rows x cols -> end above base):
 *   0: 2 x 2 -> 1*32 + 1*16 + 16 = 64     1: 3 x 2 -> 2*32 + 16 + 16 = 96
 *   2: 2 x 2 -> 64                        3: 1 x 2 -> 0 + 16 + 16 = 32 */
static void test_tiles(void)
{
	static const uint32_t ends[4] = { 64u, 96u, 64u, 32u };
	unsigned t;
	unsigned side;

	printf(" tiles\n");
	for (side = 0; side < 2; side++) {
		for (t = 0; t < 4; t++) {
			uint32_t b[4] = { 0u, 0u, 0u, 0u };
			const uint32_t z[4] = { 0u, 0u, 0u, 0u };
			char buf[64];

			b[t] = 5000u;
			reset();
			fm_tiles(&OFM, 1u, side ? z : b, 16u, 32u, 1u, O8G, 1u, 1u, 2u);
			ofm_shape(4u, 4u, 16u);
			fm_tiles(&IFM, 2u, side ? b : z, 16u, 32u, 1u, I8, 1u, 1u, 2u);
			c0(IFM_DEPTH_M1, 15u);
			c0(ELEMENTWISE, 6u);   /* ABS */
			stop();
			snprintf(buf, sizeof(buf), "%s tile %u", side ? "IFM" : "OFM", t);
			boundary(buf, side ? 2u : 1u, 5000u + ends[t]);
		}
	}

	/* tile 3 starts at row HEIGHT1 even when that is above HEIGHT0:
	 * HEIGHT0 4, HEIGHT1 2, 4x4: tile 2 is empty, tile 3 is 2 x 2 -> 64 */
	{
		const uint32_t b[4] = { 0u, 0u, 0u, 5000u };

		reset();
		fm_tiles(&OFM, 1u, b, 16u, 32u, 1u, O8G, 1u, 3u, 1u);
		ofm_shape(4u, 4u, 16u);
		fm(&IFM, 2u, 0u, 16u, 32u, 1u, I8);
		c0(IFM_DEPTH_M1, 15u);
		c0(ELEMENTWISE, 6u);
		stop();
		boundary("OFM tile 3 below HEIGHT1 (< HEIGHT0)", 1u, 5064u);
	}

	/* tiles nothing lands in: BASE1..3 far away, still accepted */
	{
		const uint32_t b[4] = { 0u, 0xFFFFFF00u, 0xFFFFFF00u, 0xFFFFFF00u };

		reset();
		fm_tiles(&OFM, 1u, b, 16u, 32u, 1u, O8G, 3u, 3u, 3u);
		ofm_shape(2u, 2u, 16u);
		fm(&IFM, 2u, 0u, 16u, 32u, 1u, I8);
		c0(IFM_DEPTH_M1, 15u);
		c0(ELEMENTWISE, 6u);
		stop();
		boundary("unreached tiles 1..3 not counted", 1u, 64u);
	}

	/* ... but every BASE must be set (C4b) */
	reset();
	c0(OFM.region, 1u);
	c1(OFM.base0, 0u);
	c1(OFM.base0 + 1u, 0u);
	c1(OFM.base0 + 2u, 0u);
	c1(OFM.stride_x, 16u);
	c1(OFM.stride_x + 1u, 32u);
	c1(OFM.stride_x + 2u, 1u);
	c0(OFM.prec, O8G);
	c0(OFM.width0, 3u);
	c0(OFM.height0, 3u);
	c0(OFM.height1, 3u);
	ofm_shape(2u, 2u, 16u);
	fm(&IFM, 2u, 0u, 16u, 32u, 1u, I8);
	c0(IFM_DEPTH_M1, 15u);
	c0(ELEMENTWISE, 6u);
	stop();
	expect("OFM BASE3 never set (tile unreached)", NPU_CS_STATE_UNSET);
}

static void test_formats(void)
{
	printf(" formats and precisions\n");
	/* NHCWB16 int8 2x3x20: Vela strides x 16, c 16*3 = 48, y 3*32 = 96:
	 * 96 + 2*16 + (2-1)*48 + 16 = 192 (= 2*3*32, the real tensor) */
	reset();
	fm(&OFM, 1u, 0u, 16u, 96u, 48u, OP(0u, 1u, 1u));
	ofm_shape(2u, 3u, 20u);
	fm(&IFM, 2u, 0u, 16u, 96u, 48u, IP(0u, 1u));
	c0(IFM_DEPTH_M1, 19u);
	c0(ELEMENTWISE, 6u);
	stop();
	boundary("NHCWB16 int8 OFM 2x3x20", 1u, 192u);
	boundary("NHCWB16 int8 IFM 2x3x20", 2u, 192u);

	/* NHCWB16 with STRIDE_X 20: the register counts too: 96 + 40 + 48 + 16 */
	reset();
	fm(&OFM, 1u, 0u, 20u, 96u, 48u, OP(0u, 1u, 1u));
	ofm_shape(2u, 3u, 20u);
	fm(&IFM, 2u, 0u, 16u, 96u, 48u, IP(0u, 1u));
	c0(IFM_DEPTH_M1, 19u);
	c0(ELEMENTWISE, 6u);
	stop();
	boundary("NHCWB16 OFM with STRIDE_X 20", 1u, 200u);

	/* NHCWB16 with STRIDE_X 0: 16 bytes per column still count (Vela's
	 * address model ignores the register): 96 + 32 + 48 + 16 = 192 */
	reset();
	fm(&OFM, 1u, 0u, 0u, 96u, 48u, OP(0u, 1u, 1u));
	ofm_shape(2u, 3u, 20u);
	fm(&IFM, 2u, 0u, 16u, 96u, 48u, IP(0u, 1u));
	c0(IFM_DEPTH_M1, 19u);
	c0(ELEMENTWISE, 6u);
	stop();
	boundary("NHCWB16 OFM with STRIDE_X 0", 1u, 192u);

	/* NHCWB16 int16: x 32, c 96, y 192: 192 + 64 + 96 + 32 = 384 */
	reset();
	fm(&OFM, 1u, 0u, 32u, 192u, 96u, OP(1u, 1u, 1u));
	ofm_shape(2u, 3u, 20u);
	fm(&IFM, 2u, 0u, 32u, 192u, 96u, IP(1u, 1u));
	c0(IFM_DEPTH_M1, 19u);
	c0(ELEMENTWISE, 6u);
	stop();
	boundary("NHCWB16 int16 OFM 2x3x20", 1u, 384u);
	boundary("NHCWB16 int16 IFM 2x3x20", 2u, 384u);

	/* NHWC int16 2x2x16: x 32, y 64, c 2: 64 + 32 + 0 + 32 = 128 */
	reset();
	fm(&OFM, 1u, 0u, 32u, 64u, 2u, OP(1u, 0u, 1u));
	ofm_shape(2u, 2u, 16u);
	fm(&IFM, 2u, 0u, 32u, 64u, 2u, IP(1u, 0u));
	c0(IFM_DEPTH_M1, 15u);
	c0(ELEMENTWISE, 6u);
	stop();
	boundary("NHWC int16 OFM 2x2x16", 1u, 128u);
	boundary("NHWC int16 IFM 2x2x16", 2u, 128u);

	/* NHWC int32 2x2x4: x 16, y 32, c 4: 32 + 16 + 0 + 16*4 = 112
	 * (depth rounds up to a brick) */
	reset();
	fm(&OFM, 1u, 0u, 16u, 32u, 4u, OP(2u, 0u, 1u));
	ofm_shape(2u, 2u, 4u);
	fm(&IFM, 2u, 0u, 16u, 32u, 4u, IP(2u, 0u));
	c0(IFM_DEPTH_M1, 3u);
	c0(ELEMENTWISE, 7u);               /* CLZ, int32 */
	stop();
	boundary("NHWC int32 OFM 2x2x4", 1u, 112u);
	boundary("NHWC int32 IFM 2x2x4", 2u, 112u);

	/* NHWC with STRIDE_C 100 over 32 channels: 64 + 32 + 100 + 32 = 228 */
	reset();
	fm(&OFM, 1u, 0u, 32u, 64u, 100u, O8G);
	ofm_shape(2u, 2u, 32u);
	fm(&IFM, 2u, 0u, 32u, 64u, 1u, I8);
	c0(IFM_DEPTH_M1, 31u);
	c0(ELEMENTWISE, 6u);
	stop();
	boundary("NHWC OFM with STRIDE_C 100 counted per brick", 1u, 228u);

	reset();
	c0(IFM.prec, IP(3u, 0u));
	stop();
	expect("IFM precision 3 (reserved)", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(IFM.prec, IP(0u, 2u));
	stop();
	expect("IFM format 2 (reserved)", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(OFM.prec, OP(3u, 0u, 0u));
	stop();
	expect("OFM precision 3 (reserved)", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(OFM.prec, OP(0u, 3u, 0u));
	stop();
	expect("OFM format 3 (reserved)", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(IFM2.prec, IP(3u, 0u));
	stop();
	expect("IFM2 precision 3 (reserved)", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(IFM2.prec, IP(0u, 1u));
	stop();
	expect("IFM2 NHCWB16 (defined)", NPU_CS_OK);
}

static void test_arithmetic(void)
{
	printf(" signed strides and 64-bit products\n");
	/* y stride -64 from base 0: row 3 is at -192 */
	reset();
	fm(&OFM, 1u, 0u, 16u, 0xFFFFFFC0u, 1u, O8G);
	ofm_shape(4u, 4u, 16u);
	fm(&IFM, 2u, 0u, 16u, 64u, 1u, I8);
	c0(IFM_DEPTH_M1, 15u);
	c0(ELEMENTWISE, 6u);
	stop();
	expect("OFM y stride -64 from base 0", NPU_CS_BELOW_REGION);

	/* from base 192 it stays >= 0 signed, but as unsigned it is 3 * 2^32 */
	reset();
	fm(&OFM, 1u, 192u, 16u, 0xFFFFFFC0u, 1u, O8G);
	ofm_shape(4u, 4u, 16u);
	fm(&IFM, 2u, 0u, 16u, 64u, 1u, I8);
	c0(IFM_DEPTH_M1, 15u);
	c0(ELEMENTWISE, 6u);
	stop();
	expect("OFM y stride 0xFFFFFFC0 from 192 (read unsigned too)",
	       NPU_CS_PAST_REGION);

	/* 65535 * 0x10001 = 0x1_0000_FFFF: 0xFFFF in 32 bits */
	reset();
	fm(&OFM, 1u, 0u, 16u, 0x10001u, 1u, O8G);
	ofm_shape(65536u, 1u, 16u);
	fm(&IFM, 2u, 0u, 16u, 16u, 1u, I8);
	c0(IFM_DEPTH_M1, 15u);
	c0(ELEMENTWISE, 6u);
	reg[2].bytes = 0xFFFFFFFFu;
	stop();
	expect("OFM 65536 rows x stride 0x10001 (wraps in 32 bits)",
	       NPU_CS_PAST_REGION);

	/* the IFM of a 65536-row conv, stride 3, kernel 65536: 262141 rows */
	reset();
	fm(&OFM, 1u, 0u, 16u, 16u, 1u, O8);
	ofm_shape(65536u, 1u, 16u);
	reg[1].bytes = 0xFFFFFFFFu;
	fm(&IFM, 2u, 0u, 8u, 0x10000u, 1u, I8);
	c0(IFM_DEPTH_M1, 7u);
	kernel(0xFFFFu, 0u, 0x200u, 0u, 0u, 0u);
	weights(0u, 0u, 16u);
	scale(4u, 0u, 16u);
	reg[2].bytes = 0xFFFFFFFFu;
	c0(CONV, 0u);
	stop();
	expect("IFM 262141 rows x stride 65536 (2^34)", NPU_CS_PAST_REGION);

	reset();
	c1x(OFM_SCALE, 31u, 0x12345678u);  /* shift: not an address */
	stop();
	expect("OFM_SCALE with a shift parameter", NPU_CS_OK);
}

static void test_rules(void)
{
	printf(" regions, writes, unset state, parameters\n");

	conv_stream();
	s[0] = 0x11Fu | (NREG << 16);      /* OFM_REGION = regions passed */
	expect("OFM region = regions passed", NPU_CS_REGION_NOT_PASSED);
	nreg = 8;
	reg[5].writable = true;
	expect("same region with 8 passed", NPU_CS_OK);

	reset();
	c0(OFM.region, 8u);
	stop();
	expect("OFM region 8", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(WEIGHT_REGION, 8u);
	stop();
	expect("weight region 8", NPU_CS_PARAMETER_RANGE);

	conv_stream();
	s[0] = 0x11Fu;                     /* OFM_REGION = 0, constant */
	expect("OFM into region 0 (constant)", NPU_CS_WRITE_TO_DATA);

	conv_stream();
	reg[2].bytes = 296u;
	reg[2].writable = true;            /* writability does not matter for reads */
	expect("IFM read from a writable region", NPU_CS_OK);

	/* each register the conv uses, dropped in turn */
	static const struct { uint32_t op; int cmd1; const char *what; } drop[] = {
		{ WEIGHT_LENGTH, 1, "conv without WEIGHT_LENGTH" },
		{ SCALE_BASE, 1, "conv without SCALE_BASE" },
		{ PAD_TOP, 0, "conv without IFM_PAD_TOP" },
		{ UPSCALE, 0, "conv without IFM_UPSCALE" },
		{ KERNEL_STRIDE, 0, "conv without KERNEL_STRIDE" },
		{ IFM_DEPTH_M1, 0, "conv without IFM_DEPTH_M1" },
		{ OFM_DEPTH_M1, 0, "conv without OFM_DEPTH_M1" },
		{ 0x11Bu, 0, "conv without OFM_HEIGHT0_M1" },
		{ 0x006u, 1, "conv without IFM_STRIDE_C" },
	};
	unsigned i;

	for (i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) {
		unsigned j;
		unsigned out = 0;

		conv_stream();
		for (j = 0; j < n; j++) {
			uint32_t w = s[j];
			int is1 = (w & 0xFC00u) == CMD1;

			if ((w & 0x3FFu) == drop[i].op && is1 == drop[i].cmd1) {
				j += is1;      /* skip it (and its data) */
				continue;
			}
			s[out++] = w;
			if (is1)
				s[out++] = s[++j];
		}
		assert(out < n);
		n = out;
		expect(drop[i].what, NPU_CS_STATE_UNSET);
	}

	/* parameter ranges */
	reset();
	c0(PAD_TOP, 127u);
	c0(PAD_LEFT, 127u);
	stop();
	expect("pad top / left 127", NPU_CS_OK);
	reset();
	c0(PAD_TOP, 128u);
	stop();
	expect("pad top 128", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(PAD_LEFT, 128u);
	stop();
	expect("pad left 128", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(UPSCALE, 3u);
	stop();
	expect("upscale 3", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(KERNEL_STRIDE, 0x41u);          /* x: 1 | 1 << 1 -> stride 4 */
	stop();
	expect("kernel x stride 4", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(KERNEL_STRIDE, 0x202u);         /* y: 1 | 1 << 1 -> stride 4 */
	stop();
	expect("kernel y stride 4", NPU_CS_PARAMETER_RANGE);
	reset();
	c0(KERNEL_STRIDE, 0x1000u);
	stop();
	expect("kernel stride bit 12 (undefined)", NPU_CS_PARAMETER_RANGE);
	reset();
	c1(SCALE_LENGTH, 0xFFFFFu);
	stop();
	expect("scale length 2^20 - 1", NPU_CS_OK);
	reset();
	c1(SCALE_LENGTH, 0x100000u);
	stop();
	expect("scale length 2^20", NPU_CS_PARAMETER_RANGE);

	conv_stream();
	s[n - 2] = CONV | (1u << 16);
	expect("conv with parameter 1", NPU_CS_PARAMETER_RANGE);

	/* weights and scale from the conv, against their own regions */
	conv_stream();
	s[n - 2] = DEPTHWISE;
	expect("the conv's registers under DEPTHWISE", NPU_CS_OK);
}

static void test_commands(void)
{
	printf(" commands and the stream's end\n");
	static const uint32_t bad0[] = { 0x004u, 0x007u, 0x014u, 0x106u, 0x132u,
	                                 0x133u, 0x17Fu, 0x190u, 0x3FFu };
	static const uint32_t bad1[] = { 0x007u, 0x033u, 0x034u, 0x040u, 0x090u,
	                                 0x093u, 0x0A0u, 0x0A7u };
	unsigned i;
	char buf[64];

	for (i = 0; i < sizeof(bad0) / sizeof(bad0[0]); i++) {
		reset();
		c0(bad0[i], 0u);
		stop();
		snprintf(buf, sizeof(buf), "cmd0 0x%03x", (unsigned)bad0[i]);
		expect(buf, NPU_CS_UNKNOWN_COMMAND);
	}
	for (i = 0; i < sizeof(bad1) / sizeof(bad1[0]); i++) {
		reset();
		c1(bad1[i], 0u);
		stop();
		snprintf(buf, sizeof(buf), "cmd1 0x%03x", (unsigned)bad1[i]);
		expect(buf, NPU_CS_UNKNOWN_COMMAND);
	}
	reset();
	s[n++] = 0x8000u;
	stop();
	expect("command bits 15..10 = 100000 (reserved)", NPU_CS_UNKNOWN_COMMAND);
	reset();
	s[n++] = 0x4400u;
	s[n++] = 0u;
	stop();
	expect("command bits 15..10 = 010001 (reserved)", NPU_CS_UNKNOWN_COMMAND);

	reset();
	stop();
	s[n++] = CMD1 | 0x000u;            /* IFM_BASE0 with no data word */
	expect("cmd1 as the last word", NPU_CS_TRUNCATED_COMMAND);

	static const uint32_t ext1[] = { 0x000u, 0x015u, 0x021u, 0x023u, 0x032u,
	                                 0x086u };
	for (i = 0; i < sizeof(ext1) / sizeof(ext1[0]); i++) {
		reset();
		c1x(ext1[i], 1u, 0u);
		stop();
		snprintf(buf, sizeof(buf), "cmd1 0x%03x with address bits 47..32 = 1",
		         (unsigned)ext1[i]);
		expect(buf, NPU_CS_ADDRESS_EXTENSION);
	}

	reset();
	expect("an empty stream", NPU_CS_NO_FINAL_STOP);
	conv_stream();
	n--;
	expect("the conv without its stop", NPU_CS_NO_FINAL_STOP);
	conv_stream();
	c0(KERNEL_WAIT, 0u);
	expect("a wait after the stop", NPU_CS_NO_FINAL_STOP);
}

static void test_arguments_and_text(void)
{
	unsigned i;
	unsigned j;

	printf(" arguments and sentences\n");
	reset();
	stop();
	expect_r("NULL stream", npu_cmdstream_check(NULL, 0, reg, nreg, SHRAM, NULL),
	         NPU_CS_BAD_ARGUMENTS);
	expect_r("9 regions", npu_cmdstream_check(s, n, reg, 9, SHRAM, NULL),
	         NPU_CS_BAD_ARGUMENTS);
	expect_r("NULL regions, 1 passed", npu_cmdstream_check(s, n, NULL, 1, SHRAM, NULL),
	         NPU_CS_BAD_ARGUMENTS);
	expect_r("NULL regions, 0 passed", npu_cmdstream_check(s, n, NULL, 0, SHRAM, NULL),
	         NPU_CS_OK);
	expect_r("SHRAM 8 KB", run_shram(8192u), NPU_CS_OK);
	expect_r("SHRAM 8 KB - 1", run_shram(8191u), NPU_CS_BAD_ARGUMENTS);
	expect_r("SHRAM 48 KB + 1", run_shram(49153u), NPU_CS_BAD_ARGUMENTS);

	checks++;
	if (npu_cmdstream_refusal_text(NPU_CS_OK) != NULL) {
		printf("  OK has a sentence: FAIL\n");
		exit(1);
	}
	for (i = 1; i <= NPU_CS_NO_FINAL_STOP; i++) {
		const char *a = npu_cmdstream_refusal_text((enum npu_cs_refusal)i);

		if (a == NULL || a[0] == '\0') {
			printf("  refusal %u has no sentence: FAIL\n", i);
			exit(1);
		}
		for (j = 1; j < i; j++) {
			if (strcmp(a, npu_cmdstream_refusal_text(
			                  (enum npu_cs_refusal)j)) == 0) {
				printf("  refusals %u and %u share a sentence: FAIL\n", j, i);
				exit(1);
			}
		}
	}
	printf("  %-60s ok\n", "every refusal has its own sentence");
}

int main(void)
{
	printf("test_npu_cmdstream:\n");
	test_control();
	test_dma();
	test_lut_dma();
	test_conv();
	test_depthwise_pool();
	test_elementwise();
	test_tiles();
	test_formats();
	test_arithmetic();
	test_rules();
	test_commands();
	test_arguments_and_text();
	printf("test_npu_cmdstream: %u checks passed\n", checks);
	return 0;
}
