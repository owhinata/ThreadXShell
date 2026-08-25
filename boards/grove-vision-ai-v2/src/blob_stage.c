/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_stage.c
 * @brief   The blob staging reservation (#92).
 *
 * A translation unit of its own, holding one array.  The reservation lands
 * before the code that fills it because it is a claim on SRAM: what the
 * placement gate pins here is that 64 KB of the loadable window belongs to
 * this and nothing else, which is a thing to settle while the window still has
 * 430 KB spare rather than when a transfer needs it.
 *
 * See blob_stage.h for why it is here rather than in DTCM, and for what the
 * size does and does not promise.
 */
#include "blob_stage.h"

#include "nor_span.h"   /* NOR_PROGRAM_PAGE -- the vendor's program page */

/* `used` so the compiler keeps it and KEEP in the linker script so
 * --gc-sections does too: until the transfer lands (#49 Step 2, items 6 and 7)
 * nothing references this, and a reservation that was garbage-collected would
 * let the next section take the address the gate says is reserved. */
uint8_t blob_stage_buf[BLOB_STAGE_BYTES]
	__attribute__((used, section(".blob_stage"), aligned(32)));

/* 32-byte aligned to match its neighbours, and NOT because anything requires
 * it: the CPU is the only thing that touches this buffer, so there is no cache
 * maintenance on it and no DMA alignment to satisfy.  Kept uniform so that a
 * reader comparing the SRAM reservations does not have to wonder which of them
 * mean something by it. */
_Static_assert(BLOB_STAGE_BYTES % 32u == 0u,
               "staging size must be a whole number of cache lines");
/* A chunk is programmed a page at a time, so a staging area that was not a
 * whole number of pages would end every chunk with a short program for no
 * reason.  The page comes from nor_span.h, which is where it was measured. */
_Static_assert(BLOB_STAGE_BYTES % NOR_PROGRAM_PAGE == 0u,
               "staging size must be a whole number of program pages");
