/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_tflm_bufs.c
 * @brief   Where this board places the tflm backend's buffers
 * (owhinata/wio-lite-ai#9 P2c; the backend is svc/nn/tflm/ since issue #161).
 *
 * THE POINT OF THIS FILE IS THAT IT IS C.  The buffers are used only from C++
 * (svc/nn/tflm/nn_tflm.cc), but defined there they would be invisible to the
 * post-link gate that guards the cacheable PSRAM carve-out
 * (cmake/check_psram_ai_residency.py matches residents by their plain names).  The
 * argument is written out once, in svc/nn/tflm/nn_tflm_mem.h, which declares what is
 * defined here and so type-checks each definition against it.
 *
 * Both buffers live in .psram_ai, the top 2 MB of the OCTOSPI1 window that app/mpu.c
 * region 3 maps Normal write-back cacheable (owhinata/wio-lite-ai#9 phase 2a).  That
 * is the right home for exactly the reason the rest of the firmware avoids it: this
 * is bulk, CPU-ONLY working set.  No bus master may ever touch either buffer -- not
 * the DMA that fills a camera frame, not the SDMMC IDMA -- because in this window a
 * master's write is coherent at the bus and stale in the D-cache, intermittently,
 * only under cache pressure.  The model bytes arrive here by CPU copy out of the NOR
 * driver's indirect reads, and the arena is never anything but CPU-touched.
 *
 * NOLOAD, so nothing zeroes them at reset: neither buffer may assume it starts as
 * zeros.  Neither does.  The arena is planned and written by TFLM's own allocator
 * before any kernel reads it, and a model slot is only ever read over the exact byte
 * range that was just written into it and CRC-checked.
 *
 * Carve-out budget (.psram_ai is 2 MB, 0x90600000..0x90800000):
 *     nn_tflm_arena        512 KB   (NN_TFLM_ARENA_KB, a cache variable)
 *     nn_tflm_model_buf  2x512 KB   (NN_TFLM_MODEL_KB; owhinata/wio-lite-ai#55: was 2x256)
 *     psram_ai_bench_buf    64 KB   (shell/cmds/cmd_membench.c)
 *     ----------------------------
 *                        1,600 KB of 2,048 KB
 * The linker script ASSERTs the total fits (_psram_ai_end <= 0x90800000);
 * cmake/check_psram_ai_residency.py checks that these are the objects in there.
 *
 * A slot is 512 KB so it swallows a WHOLE blob payload (BLOB_PAYLOAD_MAX = 520,192 B,
 * src/blob.h): any blob that fits a slot on the NOR fits a slot here, and
 * `nn model load` is never refused for a reason `blob list` cannot show.
 */
#include "nn_tflm_mem.h"

#include "mem_sections.h"   /* PSRAM_AI: the cacheable, CPU-only carve-out */

/* 32-byte aligned: one D-cache line on this part.  In a cacheable region that is not
 * cosmetic -- it keeps the arena and a model slot from sharing a line, so a write to
 * one cannot dirty a line the other half occupies. */
uint8_t nn_tflm_arena[NN_TFLM_ARENA_BYTES] PSRAM_AI __attribute__((aligned(32)));

/* Also 32, which subsumes the 16-byte alignment tflite::GetModel() needs to read the
 * flatbuffer's root table in place (NN_MODEL_ALIGN). */
uint8_t nn_tflm_model_buf[NN_TFLM_MODEL_SLOTS][NN_TFLM_MODEL_MAX]
	PSRAM_AI __attribute__((aligned(32)));

/* The staging capacity load_region() hands out (svc/nn/tflm/nn_tflm_mem.h): one slot. */
const uint32_t nn_tflm_stage_cap = NN_TFLM_MODEL_MAX;
