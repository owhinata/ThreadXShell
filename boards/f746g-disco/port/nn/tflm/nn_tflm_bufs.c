/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_bufs.c
 * @brief   Where this board places the tflm backend's buffers (issue #161; they were
 *          anonymous-namespace objects of port/nn/tflm/nn_tflm.cc before).
 *
 * C, so the symbols are the plain names cmake/check_f746_layout.py requires in SDRAM
 * bank3 (`--require-sdram-ai nn_tflm_arena / nn_tflm_model_buf`, board.cmake); the
 * argument is in svc/nn/tflm/nn_tflm_mem.h, which declares what is defined here and so
 * type-checks each definition against it.
 *
 * Both live in .sdram.ai: SDRAM bank3 (0xC0600000, 2 MB), FMC, MPU cacheable WBWA and
 * CPU-ONLY (owhinata/stm32f746g-disco#6) -- no DMA writes either, so no cache
 * maintenance is ever owed.  The section is NOLOAD (the FMC is not up at reset), so
 * neither buffer may assume it starts as zeros, and neither does: TFLM's allocator
 * writes the arena before any kernel reads it, and a slot is only read over the range
 * the SD read just wrote.  Since issue #130 the camera producer writes the model's
 * input tensor straight into the arena, so the arena's bank3 require is also the
 * placement guarantee for those writes.
 *
 * Bank3 under tflm: the slots at 0xC0600000 (2 x 512 KB), then the arena at
 * 0xC0700000 (512 KB), 1.5 MB of 2 MB; the linker ASSERT enforces the fit.  [!] The
 * compiler emits these two in the REVERSE of their order here, which is what puts the
 * slots first -- the addresses the README lists; check them in shell.map after any
 * edit of this file.
 */
#include "nn_tflm_mem.h"

/* The activation arena (512 KB, NN_TFLM_ARENA_KB): BlazeFace-front 128 int8 plans
 * ~470 KB of it; `nn info` prints the reservation and the use separately. */
uint8_t nn_tflm_arena[NN_TFLM_ARENA_BYTES]
	__attribute__((aligned(32), section(".sdram.ai")));

/* The two SD model slots (owhinata/stm32f746g-disco#89 P2), 16-aligned for
 * tflite::GetModel() (NN_MODEL_ALIGN). */
uint8_t nn_tflm_model_buf[NN_TFLM_MODEL_SLOTS][NN_TFLM_MODEL_MAX]
	__attribute__((aligned(16), section(".sdram.ai")));

/* The staging capacity load_region() hands out, and the object the asset build reads
 * back out of shell.elf (board.cmake STAGE_SYMBOL nn_tflm_stage_cap): one slot. */
const uint32_t nn_tflm_stage_cap = NN_TFLM_MODEL_MAX;
