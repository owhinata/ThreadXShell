/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The stedgeai_reloc backend's shape mapping (port/nn/nn_reloc_shape.h, issue
 * #176): an ai_buffer's B / H / W / C and element bits -> struct nn_tensor's dims,
 * ndim and bytes.  The firmware cannot be made to show any of this -- no model on
 * hand has a dimension above 65535 or an I/O tensor larger than the arena -- so the
 * boundaries are walked here, against the real header and the real nn.h.  Every
 * refusal is checked for its exact code: TENSOR, DIMS and ARENA send the operator
 * to different places and must not fold into one.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn.h"
#include "nn_reloc_shape.h"

static int g_fail;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "test_reloc_shape: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		g_fail = 1; \
	} \
} while (0)

#define BIG   0x7FFFFFFFu   /* a capacity nothing here reaches */

/* A refusal leaves the out-param as it was. */
static void refused(uint32_t B, uint32_t H, uint32_t W, uint32_t C, uint32_t bits,
                    uint32_t cap, int want, int line)
{
	struct nn_reloc_shape sh, ref;
	int rc;

	memset(&sh, 0xA5, sizeof(sh));
	memcpy(&ref, &sh, sizeof(sh));
	rc = nn_reloc_shape_map(B, H, W, C, bits, cap, &sh);
	if (rc != want || memcmp(&sh, &ref, sizeof(sh)) != 0) {
		fprintf(stderr, "test_reloc_shape: line %d: B=%u H=%u W=%u C=%u bits=%u "
		        "cap=%u -> rc %d (want %d)%s\n", line, B, H, W, C, bits, cap, rc,
		        want, rc == want ? ", out-param written" : "");
		g_fail = 1;
	}
}

static void mapped(uint32_t B, uint32_t H, uint32_t W, uint32_t C, uint32_t bits,
                   uint32_t cap, uint8_t ndim, uint16_t d0, uint16_t d1, uint16_t d2,
                   uint16_t d3, uint32_t bytes, int line)
{
	struct nn_reloc_shape sh;
	int rc;

	memset(&sh, 0xA5, sizeof(sh));
	rc = nn_reloc_shape_map(B, H, W, C, bits, cap, &sh);
	if (rc != 0 || sh.ndim != ndim || sh.dims[0] != d0 || sh.dims[1] != d1 ||
	    sh.dims[2] != d2 || sh.dims[3] != d3 || sh.bytes != bytes) {
		fprintf(stderr, "test_reloc_shape: line %d: B=%u H=%u W=%u C=%u bits=%u -> "
		        "rc %d ndim %u [%u,%u,%u,%u] bytes %u (want ndim %u [%u,%u,%u,%u] "
		        "bytes %u)\n", line, B, H, W, C, bits, rc, sh.ndim, sh.dims[0],
		        sh.dims[1], sh.dims[2], sh.dims[3], sh.bytes, ndim, d0, d1, d2, d3,
		        bytes);
		g_fail = 1;
	}
}

int main(void)
{
	/* The codes are three distinct values (folding any two hides which it was). */
	CHECK(NN_MODEL_ERR_TENSOR != NN_MODEL_ERR_DIMS);
	CHECK(NN_MODEL_ERR_TENSOR != NN_MODEL_ERR_ARENA);
	CHECK(NN_MODEL_ERR_DIMS != NN_MODEL_ERR_ARENA);

	/* Today's shapes: BlazeFace-front input [1,128,128,3] float32, a 3-D output. */
	mapped(1, 128, 128, 3, 32, BIG, 4, 1, 128, 128, 3, 128u * 128u * 3u * 4u, __LINE__);
	mapped(1, 896, 1, 16, 32, BIG, 3, 1, 896, 16, 1, 896u * 16u * 4u, __LINE__);

	/* 65535 passes on each of B / H / W / C (bits 8, so bytes is the product). */
	mapped(65535, 1, 2, 1, 8, BIG, 4, 65535, 1, 2, 1, 65535u * 2u, __LINE__);
	mapped(1, 65535, 2, 1, 8, BIG, 4, 1, 65535, 2, 1, 65535u * 2u, __LINE__);
	mapped(1, 1, 65535, 1, 8, BIG, 4, 1, 1, 65535, 1, 65535u, __LINE__);
	mapped(1, 1, 2, 65535, 8, BIG, 4, 1, 1, 2, 65535, 65535u * 2u, __LINE__);
	/* ... and on the 3-D path, where C moves to dims[2]. */
	mapped(1, 1, 1, 65535, 8, BIG, 3, 1, 1, 65535, 1, 65535u, __LINE__);

	/* 65536 is DIMS on each, never truncated to 0. */
	refused(65536, 1, 2, 1, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 65536, 2, 1, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 1, 65536, 1, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 1, 2, 65536, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 1, 1, 65536, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);  /* 3-D path */

	/* Above INT32_MAX: DIMS, not a negative int32 that happened to be refused.
	 * 0x80010000 would truncate to 0 in 16 bits, 0xFFFFFFFF to 65535. */
	refused(0x80000000u, 1, 2, 1, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 0x80010000u, 2, 1, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 1, 0xFFFFFFFFu, 1, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 1, 2, 0x80000001u, 8, BIG, NN_MODEL_ERR_DIMS, __LINE__);
	/* DIMS is decided before the size: the same shape under a cap of 0. */
	refused(65536, 1, 2, 1, 8, 0, NN_MODEL_ERR_DIMS, __LINE__);

	/* 0 is "absent" and becomes 1 before anything else (and before W's test). */
	mapped(0, 5, 7, 3, 8, BIG, 4, 1, 5, 7, 3, 105u, __LINE__);
	mapped(2, 0, 0, 0, 8, BIG, 3, 2, 1, 1, 1, 2u, __LINE__);
	mapped(0, 0, 0, 0, 32, BIG, 3, 1, 1, 1, 1, 4u, __LINE__);

	/* W == 1 collapses to 3-D [B,H,C] with dims[3] = 1; W == 2 stays 4-D. */
	mapped(3, 5, 1, 7, 8, BIG, 3, 3, 5, 7, 1, 105u, __LINE__);
	mapped(3, 5, 2, 7, 8, BIG, 4, 3, 5, 2, 7, 210u, __LINE__);

	/* No element size: TENSOR (before DIMS). */
	refused(1, 1, 2, 1, 0, BIG, NN_MODEL_ERR_TENSOR, __LINE__);
	refused(65536, 1, 2, 1, 0, BIG, NN_MODEL_ERR_TENSOR, __LINE__);
	/* Sub-byte elements keep bytes == 0 (bits / 8), as reloc_map() always did. */
	mapped(1, 4, 4, 1, 4, BIG, 4, 1, 4, 4, 1, 0u, __LINE__);
	/* ... and DIMS is still decided when the byte product is skipped: 65535 on all
	 * four passes with bytes 0 even under cap 0, a 65536 is DIMS. */
	mapped(65535, 65535, 65535, 65535, 4, 0, 4, 65535, 65535, 65535, 65535, 0u, __LINE__);
	refused(65535, 65535, 65536, 65535, 4, 0, NN_MODEL_ERR_DIMS, __LINE__);
	refused(1, 1, 2, 65536, 7, BIG, NN_MODEL_ERR_DIMS, __LINE__);

	/* The bytes bound: at capacity passes, one byte over is ARENA. */
	mapped(1, 8, 8, 3, 32, 768u, 4, 1, 8, 8, 3, 768u, __LINE__);
	refused(1, 8, 8, 3, 32, 767u, NN_MODEL_ERR_ARENA, __LINE__);
	/* The firmware's cap (RELOC_ACTS_CAP, 384 KB): 384 KB exactly, then +4 B. */
	mapped(1, 384, 256, 1, 32, 384u * 1024u, 4, 1, 384, 256, 1, 384u * 1024u, __LINE__);
	refused(1, 384, 256, 1, 32, 384u * 1024u - 1u, NN_MODEL_ERR_ARENA, __LINE__);
	refused(1, 385, 256, 1, 32, 384u * 1024u, NN_MODEL_ERR_ARENA, __LINE__);

	/* A product that does not fit 32 bits is refused, not wrapped -- even under
	 * the widest cap.  65535 x 2 x 65535 x 4 B = 0x7FFF00008, which a 32-bit product
	 * would publish as 0xFFF00008; the last line is the largest that fits. */
	refused(1, 65535, 2, 65535, 32, UINT32_MAX, NN_MODEL_ERR_ARENA, __LINE__);
	refused(65535, 65535, 65535, 65535, 120, UINT32_MAX, NN_MODEL_ERR_ARENA, __LINE__);
	refused(1, 65535, 65535, 2, 8, UINT32_MAX, NN_MODEL_ERR_ARENA, __LINE__);
	mapped(1, 65535, 65535, 1, 8, UINT32_MAX, 4, 1, 65535, 65535, 1,
	       65535u * 65535u, __LINE__);

	if (g_fail)
		return EXIT_FAILURE;
	printf("test_reloc_shape: shape mapping, DIMS / TENSOR / ARENA boundaries hold\n");
	return 0;
}
