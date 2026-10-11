/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_arena.c
 * @brief   The NN tensor arena (issue #44).
 *
 * Its own translation unit so the reservation is one obvious thing rather than
 * a large array hiding among the interpreter glue, and so the size constant
 * sits next to the reasoning for it.
 *
 * 450 KB is the figure the SDK's own MobileNet classification app uses for this
 * model; it is a property of the model, not a tuning knob, and AllocateTensors
 * says so at runtime by failing if it is short.  `nn` reports what the layout
 * actually consumed, which is how the two numbers get compared on hardware.
 *
 * In SRAM, statically reserved, NOLOAD -- see the .nn_arena block in the linker
 * script for why each of those, and check_placement_budget.py for the gate that
 * pins symbol -> size -> section -> region.
 */
#include "npu.h"

#define NPU_ARENA_BYTES (450u * 1024u)

/* The CPU cleans and invalidates exactly this span around every inference, and
 * the Ethos-U driver documents that base pointers it is handed must be cache
 * line aligned.  A span that started or ended mid-line would either miss bytes
 * or discard a neighbour's, so the alignment is enforced at both ends. */
_Static_assert(NPU_ARENA_BYTES % 32u == 0u,
               "arena size must be a whole number of cache lines");

/* `used` so the compiler keeps it, KEEP in the linker script so --gc-sections
 * does too.  Not static: the placement gate looks the symbol up by name, and a
 * local symbol is exactly what LTO was observed renaming (see #40's Step 1.5
 * note on cam_raw_buf.lto_priv.0). */
uint8_t nn_arena[NPU_ARENA_BYTES]
	__attribute__((used, section(".nn_arena"), aligned(32)));

void *npu_arena_base(void)
{
	return nn_arena;
}

size_t npu_arena_bytes(void)
{
	return sizeof(nn_arena);
}
