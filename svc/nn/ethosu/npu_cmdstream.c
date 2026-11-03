/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_cmdstream.c
 * @brief   Where an Ethos-U55 command stream makes the NPU read and write
 *          (issue #184).  See npu_cmdstream.h.
 *
 * References, cited by section below:
 *   TRM  = Arm Ethos-U55 NPU Technical Reference Manual 102420_0200_02
 *   DRV  = ethos_u_core_driver/src/ethosu55_interface.h (the SDK's pinned copy,
 *          tflmtag2412_u55tag2411), cmd0_opcode / cmd1_opcode
 *   VELA = ethosu/vela/register_command_stream_util.py get_address() /
 *          get_strides(), the generator whose streams this port runs
 *
 * [!] THE OPCODES BELOW ARE RESTATED FROM DRV, NOT INCLUDED.  The driver's
 * interface header is C++ (enum class) and pulls in the whole register map, and
 * svc/ includes no SDK header.  A command is accepted only when it is in DRV's
 * U55 list AND in TRM Table 4-112 / 4-113: DRV lists cmd0 0x132/0x133
 * (DMA0_SIZE0/1) and cmd1 0x0A0-0x0A7 (USER_DEFINED) that the TRM marks reserved,
 * and the TRM lists cmd1 0x033/0x034 (DMA0_SKIP0/1) and 0x090-0x093
 * (WEIGHT1 / SCALE1, the second core of an Ethos-U65) that DRV's U55 list does
 * not have.  Each of those is refused: a command one source does not define is
 * one whose effect on addresses this file cannot write down.
 *
 * [!] EVERY RANGE IS COMPUTED IN 64 BITS.  Offsets, lengths and strides are
 * 32-bit words straight out of flash and dimensions reach 2^18, so a 32-bit
 * product wraps to a small number that looks in bounds.  Every term below is at
 * most 2^18 * 2^32 = 2^50 and there are at most five of them, so an int64_t
 * holds every sum exactly.
 *
 * [!] STATE IS ON THE STACK.  The NPU keeps every NPU_SET_* value until it is
 * overwritten (TRM 4.6.2 Table 4-111); the walk mirrors that in a local array
 * plus a "has been set" bit per register.  Nothing here is static: the TU is
 * shared and audited for mutable storage like every svc/ file.
 */
#include "npu_cmdstream.h"

#include <stddef.h>

/* ---- command word layout: TRM 4.6 Table 4-109 ----------------------------
 * bits 15..10 = 000000 -> cmd0 (no data word), 010000 -> cmd1 (one data word
 * follows); every other combination is reserved.  bits 9..0 name the command,
 * bits 31..16 are its parameter (for a cmd1: address bits 47..32). */
#define CMD_KIND_MASK      0xFC00u
#define CMD0_KIND          0x0000u
#define CMD1_KIND          0x4000u
#define CMD_OP_MASK        0x03FFu

/* ---- cmd0 operations (DRV cmd0_opcode; TRM 4.6.4 Table 4-112) ------------ */
#define OP_STOP            0x000u
#define OP_IRQ             0x001u
#define OP_CONV            0x002u
#define OP_DEPTHWISE       0x003u
#define OP_POOL            0x005u
#define OP_ELEMENTWISE     0x006u
#define OP_DMA_START       0x010u
#define OP_DMA_WAIT        0x011u
#define OP_KERNEL_WAIT     0x012u
#define OP_PMU_MASK        0x013u

/* POOL modes (TRM Table 4-112, 0x005): 0 max, 1 average, 2 REDUCE_SUM. */
#define POOL_MODE_REDUCE_SUM 2u
#define POOL_MODE_LAST       2u
/* ELEMENTWISE modes (TRM Table 4-112, 0x006): 0 Mul .. 9 SHL.  5 LReLU, 6 ABS
 * and 7 CLZ are the unary ones (TRM 4.8.2 Table 4-126) and read no IFM2. */
#define EW_MODE_LRELU      5u
#define EW_MODE_CLZ        7u
#define EW_MODE_LAST       9u

/* ---- the registers an operation's address depends on -------------------- */
enum reg {
	/* cmd0, 16-bit parameter (TRM Table 4-112) */
	R_IFM_PAD_TOP,          /* 0x100 */
	R_IFM_PAD_LEFT,         /* 0x101 */
	R_IFM_DEPTH_M1,         /* 0x104 */
	R_IFM_PRECISION,        /* 0x105 */
	R_IFM_UPSCALE,          /* 0x107 */
	R_IFM_WIDTH0_M1,        /* 0x10A */
	R_IFM_HEIGHT0_M1,       /* 0x10B */
	R_IFM_HEIGHT1_M1,       /* 0x10C */
	R_IFM_REGION,           /* 0x10F */
	R_OFM_WIDTH_M1,         /* 0x111 */
	R_OFM_HEIGHT_M1,        /* 0x112 */
	R_OFM_DEPTH_M1,         /* 0x113 */
	R_OFM_PRECISION,        /* 0x114 */
	R_OFM_WIDTH0_M1,        /* 0x11A */
	R_OFM_HEIGHT0_M1,       /* 0x11B */
	R_OFM_HEIGHT1_M1,       /* 0x11C */
	R_OFM_REGION,           /* 0x11F */
	R_KERNEL_WIDTH_M1,      /* 0x120 */
	R_KERNEL_HEIGHT_M1,     /* 0x121 */
	R_KERNEL_STRIDE,        /* 0x122 */
	R_WEIGHT_REGION,        /* 0x128 */
	R_SCALE_REGION,         /* 0x129 */
	R_DMA0_SRC_REGION,      /* 0x130 */
	R_DMA0_DST_REGION,      /* 0x131 */
	R_IFM2_BROADCAST,       /* 0x180 */
	R_IFM2_PRECISION,       /* 0x185 */
	R_IFM2_WIDTH0_M1,       /* 0x18A */
	R_IFM2_HEIGHT0_M1,      /* 0x18B */
	R_IFM2_HEIGHT1_M1,      /* 0x18C */
	R_IFM2_REGION,          /* 0x18F */
	/* cmd1, 32-bit data word (TRM 4.6.5 Table 4-113).  BASE0..3 stay
	 * consecutive: fm_reach() indexes them as base0 + tile. */
	R_IFM_BASE0,            /* 0x000 .. 0x003 */
	R_IFM_BASE1,
	R_IFM_BASE2,
	R_IFM_BASE3,
	R_IFM_STRIDE_X,         /* 0x004 */
	R_IFM_STRIDE_Y,         /* 0x005 */
	R_IFM_STRIDE_C,         /* 0x006 */
	R_OFM_BASE0,            /* 0x010 .. 0x013 */
	R_OFM_BASE1,
	R_OFM_BASE2,
	R_OFM_BASE3,
	R_OFM_STRIDE_X,         /* 0x014 */
	R_OFM_STRIDE_Y,         /* 0x015 */
	R_OFM_STRIDE_C,         /* 0x016 */
	R_WEIGHT_BASE,          /* 0x020 */
	R_WEIGHT_LENGTH,        /* 0x021 */
	R_SCALE_BASE,           /* 0x022 */
	R_SCALE_LENGTH,         /* 0x023 */
	R_DMA0_SRC,             /* 0x030 */
	R_DMA0_DST,             /* 0x031 */
	R_DMA0_LEN,             /* 0x032 */
	R_IFM2_BASE0,           /* 0x080 .. 0x083 */
	R_IFM2_BASE1,
	R_IFM2_BASE2,
	R_IFM2_BASE3,
	R_IFM2_STRIDE_X,        /* 0x084 */
	R_IFM2_STRIDE_Y,        /* 0x085 */
	R_IFM2_STRIDE_C,        /* 0x086 */
	R_COUNT
};

_Static_assert(R_COUNT <= 64, "the set-mask is one uint64_t");

/* A command the walk knows but whose value no address depends on. */
#define SLOT_IGNORED  (-1)
/* A command the walk does not know: refused (C1). */
#define SLOT_UNKNOWN  (-2)

#define BIT(r)        ((uint64_t)1 << (r))

/* What each kind of access reads before it can be computed (C4b).  A register
 * an operation does not use is not in its set and is never read for it. */
#define M_FM(p)       (BIT(R_##p##_REGION) | BIT(R_##p##_BASE0) | \
                       BIT(R_##p##_BASE1) | BIT(R_##p##_BASE2) | \
                       BIT(R_##p##_BASE3) | BIT(R_##p##_STRIDE_X) | \
                       BIT(R_##p##_STRIDE_Y) | BIT(R_##p##_STRIDE_C) | \
                       BIT(R_##p##_WIDTH0_M1) | BIT(R_##p##_HEIGHT0_M1) | \
                       BIT(R_##p##_HEIGHT1_M1) | BIT(R_##p##_PRECISION))
#define M_OFM         (M_FM(OFM) | BIT(R_OFM_WIDTH_M1) | \
                       BIT(R_OFM_HEIGHT_M1) | BIT(R_OFM_DEPTH_M1))
#define M_IFM         (M_FM(IFM) | BIT(R_IFM_DEPTH_M1))
#define M_IFM2        (M_FM(IFM2))
#define M_KERNEL      (BIT(R_KERNEL_WIDTH_M1) | BIT(R_KERNEL_HEIGHT_M1) | \
                       BIT(R_KERNEL_STRIDE) | BIT(R_IFM_PAD_TOP) | \
                       BIT(R_IFM_PAD_LEFT) | BIT(R_IFM_UPSCALE))
#define M_WEIGHT      (BIT(R_WEIGHT_REGION) | BIT(R_WEIGHT_BASE) | \
                       BIT(R_WEIGHT_LENGTH))
#define M_SCALE       (BIT(R_SCALE_REGION) | BIT(R_SCALE_BASE) | \
                       BIT(R_SCALE_LENGTH))
#define M_DMA         (BIT(R_DMA0_SRC_REGION) | BIT(R_DMA0_DST_REGION) | \
                       BIT(R_DMA0_SRC) | BIT(R_DMA0_DST) | BIT(R_DMA0_LEN))

/* ---- field layouts, TRM Table 4-112 -------------------------------------- */
/* IFM / IFM2_PRECISION b[3:2], OFM_PRECISION b[2:1]: 0 = 8, 1 = 16, 2 = 32 bit,
 * 3 reserved.  b[7:6] on all three: 0 = NHWC, 1 = NHCWB16, 2-3 reserved.
 * OFM_PRECISION b8: 0 = per-channel scale / bias, 1 = global scale. */
#define IFM_PREC_SHIFT     2u
#define OFM_PREC_SHIFT     1u
#define PREC_RESERVED      3u
#define FORMAT_SHIFT       6u
#define FORMAT_NHCWB16     1u
#define OFM_GLOBAL_SCALE   0x100u
/* IFM2_BROADCAST: b0 H, b1 W, b2 C, b6 operand order, b7 scalar. */
#define BCAST_H            0x01u
#define BCAST_W            0x02u
#define BCAST_C            0x04u
#define BCAST_SCALAR       0x80u
#define BCAST_DEFINED      0xC7u
/* KERNEL_STRIDE: stride - 1 = b0 | b[8:6] << 1 (x), b1 | b[11:9] << 1 (y);
 * "supported stride range is 1 to 3".  b[15:12] are not defined. */
#define KSTRIDE_DEFINED    0x0FFFu
#define STRIDE_MAX         3u
/* IFM_PAD_TOP / LEFT: 0-127.  IFM_UPSCALE: 0 none, 1 nearest, 2 zeros. */
#define PAD_TL_MAX         127u
#define UPSCALE_MAX        2u
/* *_REGION: 0-7. */
#define REGION_MAX         7u
/* DMA0_SRC/DST_REGION: b[7:0] region (b8 = 0) or core mask (b8 = 1), b8
 * internal, b[10:9] stride mode (0 = 1D).  b[15:11] are not defined. */
#define DMA_REGION_FIELD   0x00FFu
#define DMA_INTERNAL       0x0100u
#define DMA_MODE_SHIFT     9u
#define DMA_MODE_MASK      3u
#define DMA_DEFINED        0x07FFu
/* The LUT page: the last 2 KB of the SHRAM, written with core mask 3 (the value
 * Vela emits, BASE_PTR_INDEX_MEM2MEM = (1 << 8) | 3). */
#define LUT_BYTES          2048u
#define LUT_CORE_MASK      3u
/* SHRAM_SIZE in TRM 4.2.11 CONFIG: 8-48 KB. */
#define SHRAM_MIN_BYTES    (8u * 1024u)
#define SHRAM_MAX_BYTES    (48u * 1024u)
/* SCALE_LENGTH: "unsigned 20 bits" (TRM Table 4-113, 0x023). */
#define SCALE_LENGTH_MAX   0xFFFFFu

/* The NPU's memory layout unit: one brick is 16 channels (TRM Table 4-113,
 * *_STRIDE_C "between channel blocks (of 16 bytes each block)"). */
#define BRICK              16u

struct cs_state {
	uint64_t set;
	uint32_t v[R_COUNT];
};

struct cs_walk {
	struct cs_state s;
	const struct npu_cs_region *regions;
	uint32_t nregions;
	uint32_t shram_bytes;
	struct npu_cs_extent *extent;
};

/* One feature map's registers (IFM, IFM2 or OFM). */
struct fm_desc {
	uint8_t region, base0, stride_x, stride_y, stride_c;
	uint8_t width0, height0, height1, precision, prec_shift;
};

static const struct fm_desc FM_IFM = {
	R_IFM_REGION, R_IFM_BASE0, R_IFM_STRIDE_X, R_IFM_STRIDE_Y, R_IFM_STRIDE_C,
	R_IFM_WIDTH0_M1, R_IFM_HEIGHT0_M1, R_IFM_HEIGHT1_M1, R_IFM_PRECISION,
	IFM_PREC_SHIFT,
};
static const struct fm_desc FM_IFM2 = {
	R_IFM2_REGION, R_IFM2_BASE0, R_IFM2_STRIDE_X, R_IFM2_STRIDE_Y,
	R_IFM2_STRIDE_C, R_IFM2_WIDTH0_M1, R_IFM2_HEIGHT0_M1, R_IFM2_HEIGHT1_M1,
	R_IFM2_PRECISION, IFM_PREC_SHIFT,
};
static const struct fm_desc FM_OFM = {
	R_OFM_REGION, R_OFM_BASE0, R_OFM_STRIDE_X, R_OFM_STRIDE_Y, R_OFM_STRIDE_C,
	R_OFM_WIDTH0_M1, R_OFM_HEIGHT0_M1, R_OFM_HEIGHT1_M1, R_OFM_PRECISION,
	OFM_PREC_SHIFT,
};

/* cmd0 NPU_SET_* -> register (TRM Table 4-112 x DRV cmd0_opcode). */
static int cmd0_slot(uint32_t op)
{
	switch (op) {
	case 0x100u: return R_IFM_PAD_TOP;
	case 0x101u: return R_IFM_PAD_LEFT;
	case 0x102u:                      /* IFM_PAD_RIGHT: see ifm_extent() */
	case 0x103u: return SLOT_IGNORED; /* IFM_PAD_BOTTOM: likewise */
	case 0x104u: return R_IFM_DEPTH_M1;
	case 0x105u: return R_IFM_PRECISION;
	case 0x107u: return R_IFM_UPSCALE;
	case 0x109u: return SLOT_IGNORED; /* IFM_ZERO_POINT */
	case 0x10Au: return R_IFM_WIDTH0_M1;
	case 0x10Bu: return R_IFM_HEIGHT0_M1;
	case 0x10Cu: return R_IFM_HEIGHT1_M1;
	case 0x10Du: return SLOT_IGNORED; /* IFM_IB_END (SHRAM) */
	case 0x10Fu: return R_IFM_REGION;
	case 0x111u: return R_OFM_WIDTH_M1;
	case 0x112u: return R_OFM_HEIGHT_M1;
	case 0x113u: return R_OFM_DEPTH_M1;
	case 0x114u: return R_OFM_PRECISION;
	case 0x115u:                      /* OFM_BLK_WIDTH_M1 (SHRAM block) */
	case 0x116u:                      /* OFM_BLK_HEIGHT_M1 */
	case 0x117u:                      /* OFM_BLK_DEPTH_M1 */
	case 0x118u: return SLOT_IGNORED; /* OFM_ZERO_POINT */
	case 0x11Au: return R_OFM_WIDTH0_M1;
	case 0x11Bu: return R_OFM_HEIGHT0_M1;
	case 0x11Cu: return R_OFM_HEIGHT1_M1;
	case 0x11Fu: return R_OFM_REGION;
	case 0x120u: return R_KERNEL_WIDTH_M1;
	case 0x121u: return R_KERNEL_HEIGHT_M1;
	case 0x122u: return R_KERNEL_STRIDE;
	case 0x124u:                      /* ACC_FORMAT */
	case 0x125u:                      /* ACTIVATION (a LUT is SHRAM) */
	case 0x126u:                      /* ACTIVATION_MIN */
	case 0x127u: return SLOT_IGNORED; /* ACTIVATION_MAX */
	case 0x128u: return R_WEIGHT_REGION;
	case 0x129u: return R_SCALE_REGION;
	case 0x12Du:                      /* AB_START (SHRAM) */
	case 0x12Fu: return SLOT_IGNORED; /* BLOCKDEP */
	case 0x130u: return R_DMA0_SRC_REGION;
	case 0x131u: return R_DMA0_DST_REGION;
	/* 0x132 / 0x133 DMA0_SIZE0/1: in DRV, reserved in TRM -> unknown. */
	case 0x180u: return R_IFM2_BROADCAST;
	case 0x181u: return SLOT_IGNORED; /* IFM2_SCALAR (a value, no address) */
	case 0x185u: return R_IFM2_PRECISION;
	case 0x189u: return SLOT_IGNORED; /* IFM2_ZERO_POINT */
	case 0x18Au: return R_IFM2_WIDTH0_M1;
	case 0x18Bu: return R_IFM2_HEIGHT0_M1;
	case 0x18Cu: return R_IFM2_HEIGHT1_M1;
	case 0x18Du: return SLOT_IGNORED; /* IFM2_IB_START (SHRAM) */
	case 0x18Fu: return R_IFM2_REGION;
	default:     return SLOT_UNKNOWN;
	}
}

/* cmd1 -> register (TRM Table 4-113 x DRV cmd1_opcode). */
static int cmd1_slot(uint32_t op)
{
	switch (op) {
	case 0x000u: return R_IFM_BASE0;
	case 0x001u: return R_IFM_BASE1;
	case 0x002u: return R_IFM_BASE2;
	case 0x003u: return R_IFM_BASE3;
	case 0x004u: return R_IFM_STRIDE_X;
	case 0x005u: return R_IFM_STRIDE_Y;
	case 0x006u: return R_IFM_STRIDE_C;
	case 0x010u: return R_OFM_BASE0;
	case 0x011u: return R_OFM_BASE1;
	case 0x012u: return R_OFM_BASE2;
	case 0x013u: return R_OFM_BASE3;
	case 0x014u: return R_OFM_STRIDE_X;
	case 0x015u: return R_OFM_STRIDE_Y;
	case 0x016u: return R_OFM_STRIDE_C;
	case 0x020u: return R_WEIGHT_BASE;
	case 0x021u: return R_WEIGHT_LENGTH;
	case 0x022u: return R_SCALE_BASE;
	case 0x023u: return R_SCALE_LENGTH;
	case 0x024u:                      /* OFM_SCALE: parameter is a shift */
	case 0x025u:                      /* OPA_SCALE: likewise */
	case 0x026u: return SLOT_IGNORED; /* OPB_SCALE */
	case 0x030u: return R_DMA0_SRC;
	case 0x031u: return R_DMA0_DST;
	case 0x032u: return R_DMA0_LEN;
	/* 0x033 / 0x034 DMA0_SKIP0/1: TRM only (2D / 3D DMA) -> unknown. */
	case 0x080u: return R_IFM2_BASE0;
	case 0x081u: return R_IFM2_BASE1;
	case 0x082u: return R_IFM2_BASE2;
	case 0x083u: return R_IFM2_BASE3;
	case 0x084u: return R_IFM2_STRIDE_X;
	case 0x085u: return R_IFM2_STRIDE_Y;
	case 0x086u: return R_IFM2_STRIDE_C;
	/* 0x090-0x093 WEIGHT1 / SCALE1: TRM only (second core) -> unknown.
	 * 0x0A0-0x0A7 USER_DEFINED: DRV only -> unknown. */
	default:     return SLOT_UNKNOWN;
	}
}

static uint32_t stride_x_of(uint32_t k)
{
	return ((k & 1u) | (((k >> 6) & 7u) << 1)) + 1u;
}

static uint32_t stride_y_of(uint32_t k)
{
	return (((k >> 1) & 1u) | (((k >> 9) & 7u) << 1)) + 1u;
}

/* C6: a value the TRM gives a meaning to, for every register an address
 * depends on (TRM Table 4-112 / 4-113 ranges).  Out of range is refused, not
 * masked: how the hardware reads an undefined value is not written down. */
static bool value_ok(int slot, uint32_t v)
{
	switch (slot) {
	case R_IFM_REGION:
	case R_OFM_REGION:
	case R_IFM2_REGION:
	case R_WEIGHT_REGION:
	case R_SCALE_REGION:
		return v <= REGION_MAX;
	case R_IFM_PRECISION:
	case R_IFM2_PRECISION:
		return ((v >> IFM_PREC_SHIFT) & 3u) != PREC_RESERVED &&
		       ((v >> FORMAT_SHIFT) & 3u) <= FORMAT_NHCWB16;
	case R_OFM_PRECISION:
		return ((v >> OFM_PREC_SHIFT) & 3u) != PREC_RESERVED &&
		       ((v >> FORMAT_SHIFT) & 3u) <= FORMAT_NHCWB16;
	case R_IFM_UPSCALE:
		return v <= UPSCALE_MAX;
	case R_IFM_PAD_TOP:
	case R_IFM_PAD_LEFT:
		return v <= PAD_TL_MAX;
	case R_KERNEL_STRIDE:
		return (v & ~KSTRIDE_DEFINED) == 0u &&
		       stride_x_of(v) <= STRIDE_MAX && stride_y_of(v) <= STRIDE_MAX;
	case R_DMA0_SRC_REGION:
	case R_DMA0_DST_REGION:
		return (v & ~DMA_DEFINED) == 0u &&
		       ((v & DMA_INTERNAL) != 0u ||
		        (v & DMA_REGION_FIELD) <= REGION_MAX);
	case R_IFM2_BROADCAST:
		return (v & ~BCAST_DEFINED) == 0u;
	case R_SCALE_LENGTH:
		return v <= SCALE_LENGTH_MAX;
	default:
		return true;
	}
}

/* One axis of a feature map: coordinates 0 .. count-1, each step any of
 * @p steps.  Adds the most negative and the most positive reach. */
static void axis(int64_t *lo, int64_t *hi, uint32_t count,
                 const int64_t *steps, unsigned nsteps)
{
	int64_t n = (int64_t)count - 1;
	int64_t mn = 0;
	int64_t mx = 0;
	unsigned i;

	for (i = 0u; i < nsteps; i++) {
		int64_t t = n * steps[i];

		if (t < mn)
			mn = t;
		if (t > mx)
			mx = t;
	}
	*lo += mn;
	*hi += mx;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

/*
 * The byte range [*lo, *hi) of an h x w x d feature map (C4).
 *
 * Address of element (y, x, c) in tile t (VELA get_address(), the tile and
 * stride registers of TRM Table 4-112 / 4-113):
 *     BASE[t] + y * STRIDE_Y + x * sx + (c / 16) * sc + (c % 16) * elem
 * with sx = STRIDE_X, sc = 16 * elem for NHWC and sx = 16 * elem,
 * sc = STRIDE_C for NHCWB16.  Tiles (TRM Table 4-112 0x10A-0x10C, 4-113 BASE0-3):
 * tile 0 top-left, 1 top-right from column WIDTH0, 2 bottom-left from row
 * HEIGHT0, 3 bottom-right from column WIDTH0 and row HEIGHT1, each with its
 * coordinates restarting at 0.
 *
 * Conservative on purpose (U4), so a register Vela happens to set to the
 * implied value cannot hide a different one:
 *   - a stride word counts both as signed and as unsigned 32-bit (the extension
 *     bits that would say which are refused to be anything but 0, C2);
 *   - NHCWB16 counts STRIDE_X as well as 16 * elem;
 *   - NHWC counts STRIDE_C per 16 channels as well as packed channels, and
 *     the depth rounds up to a whole brick of 16 (covers the 8-aligned
 *     IFM_MEMBLK_DEPTH of TRM 4.9 Table 4-140);
 *   - NHCWB16 covers the whole last brick.
 * Only tiles some coordinate actually lands in are counted; their BASE is still
 * required to be set (M_FM).
 */
static void fm_reach(const struct cs_state *s, const struct fm_desc *f,
                     uint32_t h, uint32_t w, uint32_t d,
                     int64_t *lo, int64_t *hi)
{
	uint32_t prec = s->v[f->precision];
	int64_t elem = (int64_t)1 << ((prec >> f->prec_shift) & 3u);
	bool brick = ((prec >> FORMAT_SHIFT) & 3u) == FORMAT_NHCWB16;
	uint32_t blocks = (d + BRICK - 1u) / BRICK;
	int64_t last = brick ? (int64_t)BRICK * elem
	                     : (int64_t)blocks * BRICK * elem;
	uint32_t w0 = s->v[f->width0] + 1u;
	uint32_t h0 = s->v[f->height0] + 1u;
	uint32_t h1 = s->v[f->height1] + 1u;
	uint32_t ry = s->v[f->stride_y];
	uint32_t rx = s->v[f->stride_x];
	uint32_t rc = s->v[f->stride_c];
	const int64_t sy[2] = { (int32_t)ry, (int64_t)ry };
	const int64_t sx[3] = { (int32_t)rx, (int64_t)rx, (int64_t)BRICK * elem };
	const int64_t sc[2] = { (int32_t)rc, (int64_t)rc };
	unsigned t;

	*lo = INT64_MAX;
	*hi = INT64_MIN;
	for (t = 0u; t < 4u; t++) {
		uint32_t rows;
		uint32_t cols;
		int64_t tl;
		int64_t th;

		switch (t) {
		case 0u:
			rows = min_u32(h, h0);
			cols = min_u32(w, w0);
			break;
		case 1u:
			if (w <= w0)
				continue;
			rows = min_u32(h, h1);
			cols = w - w0;
			break;
		case 2u:
			if (h <= h0)
				continue;
			rows = h - h0;
			cols = min_u32(w, w0);
			break;
		default:
			if (w <= w0 || h <= h1)
				continue;
			rows = h - h1;
			cols = w - w0;
			break;
		}
		tl = (int64_t)s->v[f->base0 + t];
		th = tl + last;
		axis(&tl, &th, rows, sy, 2u);
		axis(&tl, &th, cols, sx, brick ? 3u : 2u);
		axis(&tl, &th, blocks, sc, 2u);
		if (tl < *lo)
			*lo = tl;
		if (th > *hi)
			*hi = th;
	}
}

/*
 * IFM rows (or columns) a kernel operation reads for @p ofm output rows: TRM
 * 4.9 Table 4-140, ifm_get_height(OFM_HEIGHT, dilated_kernel_height - PAD_TOP
 * - PAD_BOTTOM) = ceil(((OFM_HEIGHT - 1) * stride + border) / upscale).
 *
 * KERNEL_*_M1 IS ALREADY THE DILATED SIZE - 1 (TRM Table 4-112 0x120 / 0x121:
 * "Set (dilated_kernel_width-1) = (kernel_width-1)*kernel_x_dilation"; VELA's
 * generator writes dilation * (kernel - 1)), so the dilation bits of
 * KERNEL_STRIDE are not applied a second time.
 *
 * Conservative (U4): PAD_BOTTOM / PAD_RIGHT are not subtracted, and at least
 * one row is read however large the padding.
 */
static uint32_t ifm_extent(uint32_t ofm, uint32_t stride, uint32_t kernel_m1,
                           uint32_t pad, bool upscale)
{
	int64_t n = ((int64_t)ofm - 1) * (int64_t)stride +
	            (int64_t)kernel_m1 + 1 - (int64_t)pad;

	if (n < 1)
		n = 1;
	if (upscale)
		n = (n + 1) / 2;
	return (uint32_t)n;
}

/* C3, C5 and C4 for one access, in that order. */
static enum npu_cs_refusal touch(struct cs_walk *k, uint32_t region,
                                 int64_t lo, int64_t hi, bool write)
{
	if (region >= k->nregions)
		return NPU_CS_REGION_NOT_PASSED;
	if (write && !k->regions[region].writable)
		return NPU_CS_WRITE_TO_DATA;
	if (lo < 0)
		return NPU_CS_BELOW_REGION;
	if (hi > (int64_t)k->regions[region].bytes)
		return NPU_CS_PAST_REGION;
	if (k->extent != NULL) {
		struct npu_cs_extent *e = &k->extent[region];

		if (!e->touched || lo < e->lo)
			e->lo = lo;
		if (!e->touched || hi > e->hi)
			e->hi = hi;
		e->touched = true;
	}
	return NPU_CS_OK;
}

static bool has(const struct cs_state *s, uint64_t mask)
{
	return (s->set & mask) == mask;
}

/* A [base, base + length) stream: weights (TRM 4.7.2, whose end is its byte
 * length, Table 4-113 0x021) or scale / bias (Table 4-113 0x023). */
static enum npu_cs_refusal stream_touch(struct cs_walk *k, int region_reg,
                                        int base_reg, int length_reg)
{
	int64_t b = (int64_t)k->s.v[base_reg];

	return touch(k, k->s.v[region_reg], b, b + (int64_t)k->s.v[length_reg],
	             false);
}

/*
 * U5: a DMA into NPU-internal memory (SHRAM), which Vela emits to load an
 * activation lookup table.  It never reaches system memory -- what this file
 * protects -- but it is accepted only in the one shape Vela produces: core mask
 * 3, a length of 1 .. 2048 bytes, all of it inside the last 2 KB of the SHRAM
 * (TRM 4.9.1 Table 4-141: "LUT ... must be in the last 2KB of the SHRAM";
 * Table 4-112 0x125 ACTIVATION: tables at n * 256 in that page).  Anything else
 * internal is refused, because the TRM does not say where it would land.
 *
 * The SHRAM size is the caller's: it is a property of the configuration the
 * board compiles models for, not of this walk.  TRM 4.9.1 says the 32 and 64
 * configurations use no LUT -- that is in the context of its SHRAM layout
 * formula; Vela (Arm's own compiler) does emit LUTs for ethos-u55-64.
 */
static enum npu_cs_refusal lut_dma(const struct cs_walk *k, uint32_t mask,
                                   int64_t dst, int64_t len)
{
	int64_t shram = (int64_t)k->shram_bytes;

	/* 0 < len <= 2048 follows from the two bounds on [dst, dst + len). */
	if (mask != LUT_CORE_MASK || len <= 0 ||
	    dst < shram - (int64_t)LUT_BYTES || dst + len > shram)
		return NPU_CS_DMA_DEST_INTERNAL;
	return NPU_CS_OK;
}

/* NPU_OP_DMA_START (TRM Table 4-112 0x010 / 0x130 / 0x131, Table 4-113
 * 0x030-0x032): reads [SRC, SRC + LEN) and writes [DST, DST + LEN). */
static enum npu_cs_refusal dma_op(struct cs_walk *k)
{
	const struct cs_state *s = &k->s;
	uint32_t sr;
	uint32_t dr;
	int64_t src;
	int64_t dst;
	int64_t len;
	enum npu_cs_refusal r;

	if (!has(s, M_DMA))
		return NPU_CS_STATE_UNSET;
	sr = s->v[R_DMA0_SRC_REGION];
	dr = s->v[R_DMA0_DST_REGION];
	if ((sr & DMA_INTERNAL) != 0u)
		return NPU_CS_DMA_SOURCE_INTERNAL;   /* "must be 0 for external" */
	if (((sr >> DMA_MODE_SHIFT) & DMA_MODE_MASK) != 0u)
		return NPU_CS_DMA_STRIDE_MODE;
	if (((dr >> DMA_MODE_SHIFT) & DMA_MODE_MASK) != 0u)
		return NPU_CS_DMA_STRIDE_MODE;

	src = (int64_t)s->v[R_DMA0_SRC];
	dst = (int64_t)s->v[R_DMA0_DST];
	len = (int64_t)s->v[R_DMA0_LEN];
	r = touch(k, sr & DMA_REGION_FIELD, src, src + len, false);
	if (r != NPU_CS_OK)
		return r;
	if ((dr & DMA_INTERNAL) != 0u)
		return lut_dma(k, dr & DMA_REGION_FIELD, dst, len);
	return touch(k, dr & DMA_REGION_FIELD, dst, dst + len, true);
}

/*
 * NPU_OP_CONV / DEPTHWISE / POOL / ELEMENTWISE.
 *
 * What each one uses (C4b; a register outside its set is not read):
 *   all four       OFM (write) and IFM (read), with IFM_DEPTH_M1
 *   not ELEMENTWISE  KERNEL_WIDTH_M1 / HEIGHT_M1 / STRIDE, IFM_PAD_TOP / LEFT,
 *                  IFM_UPSCALE (TRM 4.9 Table 4-140)
 *   CONV, DEPTHWISE  the weight stream and the scale stream
 *   POOL, ELEMENTWISE  the scale stream when OFM_PRECISION b8 = 0 (per channel);
 *                  otherwise also counted when SCALE_LENGTH is set and not 0
 *   binary ELEMENTWISE  IFM2_BROADCAST; IFM2 itself unless b7 (scalar)
 *
 * IFM extent: ELEMENTWISE reads the OFM's height and width (no kernel); the
 * others ifm_extent().  IFM depth (TRM Table 4-140 IFM_MEMBLK_DEPTH): CONV and
 * REDUCE_SUM read IFM_DEPTH_M1 + 1 channels; DEPTHWISE, max / average POOL and
 * ELEMENTWISE follow the OFM's depth, taken here as the larger of the two.
 * IFM2 has the IFM's shape with 1 on each broadcast axis (TRM Table 4-112
 * 0x180, 4.8.2 Table 4-126).
 */
static enum npu_cs_refusal kernel_op(struct cs_walk *k, uint32_t op,
                                     uint32_t mode)
{
	const struct cs_state *s = &k->s;
	bool ew = op == OP_ELEMENTWISE;
	bool binary = ew && !(mode >= EW_MODE_LRELU && mode <= EW_MODE_CLZ);
	bool weights = op == OP_CONV || op == OP_DEPTHWISE;
	uint64_t need = M_OFM | M_IFM;
	bool scale;
	bool ifm2 = false;
	uint32_t oh;
	uint32_t ow;
	uint32_t od;
	uint32_t ih;
	uint32_t iw;
	uint32_t id;
	int64_t lo;
	int64_t hi;
	enum npu_cs_refusal r;

	if (!ew)
		need |= M_KERNEL;
	if (weights)
		need |= M_WEIGHT | M_SCALE;
	if (binary)
		need |= BIT(R_IFM2_BROADCAST);
	if (!has(s, need))
		return NPU_CS_STATE_UNSET;

	if (weights)
		scale = true;
	else if ((s->v[R_OFM_PRECISION] & OFM_GLOBAL_SCALE) == 0u)
		scale = true;                 /* per channel: must be set */
	else
		scale = (s->set & BIT(R_SCALE_LENGTH)) != 0u &&
		        s->v[R_SCALE_LENGTH] != 0u;
	if (binary && (s->v[R_IFM2_BROADCAST] & BCAST_SCALAR) == 0u)
		ifm2 = true;
	if ((scale && !has(s, M_SCALE)) || (ifm2 && !has(s, M_IFM2)))
		return NPU_CS_STATE_UNSET;

	oh = s->v[R_OFM_HEIGHT_M1] + 1u;
	ow = s->v[R_OFM_WIDTH_M1] + 1u;
	od = s->v[R_OFM_DEPTH_M1] + 1u;

	fm_reach(s, &FM_OFM, oh, ow, od, &lo, &hi);
	r = touch(k, s->v[R_OFM_REGION], lo, hi, true);
	if (r != NPU_CS_OK)
		return r;

	if (ew) {
		ih = oh;
		iw = ow;
	} else {
		uint32_t ks = s->v[R_KERNEL_STRIDE];
		bool up = s->v[R_IFM_UPSCALE] != 0u;

		ih = ifm_extent(oh, stride_y_of(ks), s->v[R_KERNEL_HEIGHT_M1],
		                s->v[R_IFM_PAD_TOP], up);
		iw = ifm_extent(ow, stride_x_of(ks), s->v[R_KERNEL_WIDTH_M1],
		                s->v[R_IFM_PAD_LEFT], up);
	}
	id = s->v[R_IFM_DEPTH_M1] + 1u;
	if (!(op == OP_CONV || (op == OP_POOL && mode == POOL_MODE_REDUCE_SUM)) &&
	    od > id)
		id = od;
	fm_reach(s, &FM_IFM, ih, iw, id, &lo, &hi);
	r = touch(k, s->v[R_IFM_REGION], lo, hi, false);
	if (r != NPU_CS_OK)
		return r;

	if (ifm2) {
		uint32_t bc = s->v[R_IFM2_BROADCAST];

		fm_reach(s, &FM_IFM2, (bc & BCAST_H) ? 1u : ih,
		         (bc & BCAST_W) ? 1u : iw, (bc & BCAST_C) ? 1u : id,
		         &lo, &hi);
		r = touch(k, s->v[R_IFM2_REGION], lo, hi, false);
		if (r != NPU_CS_OK)
			return r;
	}
	if (weights) {
		r = stream_touch(k, R_WEIGHT_REGION, R_WEIGHT_BASE,
		                 R_WEIGHT_LENGTH);
		if (r != NPU_CS_OK)
			return r;
	}
	if (scale)
		return stream_touch(k, R_SCALE_REGION, R_SCALE_BASE,
		                    R_SCALE_LENGTH);
	return NPU_CS_OK;
}

/* An NPU_OP_* (cmd0 below 0x100).  *stop is set for NPU_OP_STOP. */
static enum npu_cs_refusal operation(struct cs_walk *k, uint32_t op,
                                     uint32_t param, bool *stop)
{
	switch (op) {
	case OP_STOP:
		*stop = true;                 /* param is an IRQ mask */
		return NPU_CS_OK;
	case OP_IRQ:
	case OP_DMA_WAIT:
	case OP_KERNEL_WAIT:
	case OP_PMU_MASK:
		return NPU_CS_OK;             /* no memory access */
	case OP_DMA_START:
		/* param = 16 * channel; the U55 has channel 0 only. */
		if (param != 0u)
			return NPU_CS_PARAMETER_RANGE;
		return dma_op(k);
	case OP_CONV:
	case OP_DEPTHWISE:
		if (param != 0u)
			return NPU_CS_PARAMETER_RANGE;
		return kernel_op(k, op, 0u);
	case OP_POOL:
		if (param > POOL_MODE_LAST)
			return NPU_CS_PARAMETER_RANGE;
		return kernel_op(k, op, param);
	case OP_ELEMENTWISE:
		if (param > EW_MODE_LAST)
			return NPU_CS_PARAMETER_RANGE;
		return kernel_op(k, op, param);
	default:
		return NPU_CS_UNKNOWN_COMMAND;
	}
}

enum npu_cs_refusal npu_cmdstream_check(const uint32_t *words, uint32_t nwords,
                                        const struct npu_cs_region *regions,
                                        uint32_t nregions,
                                        uint32_t shram_bytes,
                                        struct npu_cs_extent *extent)
{
	struct cs_walk k;
	uint32_t i = 0u;
	bool stop = false;

	if (words == NULL || nregions > NPU_CS_MAX_REGIONS ||
	    (regions == NULL && nregions != 0u) ||
	    shram_bytes < SHRAM_MIN_BYTES || shram_bytes > SHRAM_MAX_BYTES)
		return NPU_CS_BAD_ARGUMENTS;

	k.s.set = 0u;
	k.regions = regions;
	k.nregions = nregions;
	k.shram_bytes = shram_bytes;
	k.extent = extent;
	if (extent != NULL) {
		uint32_t r;

		for (r = 0u; r < nregions; r++) {
			extent[r].lo = 0;
			extent[r].hi = 0;
			extent[r].touched = false;
		}
	}

	while (i < nwords) {
		uint32_t w = words[i];
		uint32_t kind = w & CMD_KIND_MASK;
		uint32_t op = w & CMD_OP_MASK;
		uint32_t param = w >> 16;
		int slot;

		stop = false;
		if (kind == CMD1_KIND) {
			uint32_t data;

			if (nwords - i < 2u)
				return NPU_CS_TRUNCATED_COMMAND;
			slot = cmd1_slot(op);
			if (slot == SLOT_UNKNOWN)
				return NPU_CS_UNKNOWN_COMMAND;
			data = words[i + 1u];
			i += 2u;
			if (slot == SLOT_IGNORED)
				continue;
			/* C2: the U55 has 32-bit addresses (TRM 4.6.5: "for those
			 * ending with '_32' the Ethos-U55 NPU supports only 32-bit
			 * addresses"). */
			if (param != 0u)
				return NPU_CS_ADDRESS_EXTENSION;
			if (!value_ok(slot, data))
				return NPU_CS_PARAMETER_RANGE;
			k.s.v[slot] = data;
			k.s.set |= BIT(slot);
			continue;
		}
		if (kind != CMD0_KIND)
			return NPU_CS_UNKNOWN_COMMAND;
		i += 1u;
		if (op < 0x100u) {
			enum npu_cs_refusal r = operation(&k, op, param, &stop);

			if (r != NPU_CS_OK)
				return r;
			continue;
		}
		slot = cmd0_slot(op);
		if (slot == SLOT_UNKNOWN)
			return NPU_CS_UNKNOWN_COMMAND;
		if (slot == SLOT_IGNORED)
			continue;
		if (!value_ok(slot, param))
			return NPU_CS_PARAMETER_RANGE;
		k.s.v[slot] = param;
		k.s.set |= BIT(slot);
	}
	/* C7 (U8).  Not a range rule: the driver waits for the IRQ the stop
	 * raises, and a stream without one would only ever reach the timeout. */
	return stop ? NPU_CS_OK : NPU_CS_NO_FINAL_STOP;
}

const char *npu_cmdstream_refusal_text(enum npu_cs_refusal r)
{
	switch (r) {
	case NPU_CS_OK:
		return NULL;
	case NPU_CS_BAD_ARGUMENTS:
		return "the command stream check was given no stream, more "
		       "than 8 regions or an impossible SHRAM size";
	case NPU_CS_UNKNOWN_COMMAND:
		return "the command stream has a command the Ethos-U55 does not "
		       "define";
	case NPU_CS_TRUNCATED_COMMAND:
		return "the command stream ends inside a command's data word";
	case NPU_CS_ADDRESS_EXTENSION:
		return "the command stream sets address bits above 32, which the "
		       "Ethos-U55 does not have";
	case NPU_CS_REGION_NOT_PASSED:
		return "the command stream uses a region the operator passes no "
		       "tensor for";
	case NPU_CS_DMA_SOURCE_INTERNAL:
		return "the command stream starts a DMA from NPU-internal memory";
	case NPU_CS_DMA_STRIDE_MODE:
		return "the command stream starts a DMA that is not a plain 1D "
		       "copy";
	case NPU_CS_DMA_DEST_INTERNAL:
		return "the command stream starts a DMA into NPU-internal memory "
		       "outside the last 2 KB lookup table page";
	case NPU_CS_STATE_UNSET:
		return "the command stream starts an operation that uses a "
		       "register the stream never set";
	case NPU_CS_BELOW_REGION:
		return "the command stream reaches before the start of a "
		       "region's tensor";
	case NPU_CS_PAST_REGION:
		return "the command stream reaches past the end of a region's "
		       "tensor";
	case NPU_CS_WRITE_TO_DATA:
		return "the command stream writes a region whose tensor holds "
		       "constant data";
	case NPU_CS_PARAMETER_RANGE:
		return "the command stream sets a value the Ethos-U55 manual "
		       "gives no meaning to";
	case NPU_CS_NO_FINAL_STOP:
		return "the command stream does not end with a stop command";
	default:
		return "the command stream was refused for an unnamed reason";
	}
}
