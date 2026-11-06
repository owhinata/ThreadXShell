/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_mem.h
 * @brief   What the board PLACES for the tflm backend: the activation arena, the
 *          one model staging buffer and the staging capacity object
 *          (owhinata/wio-lite-ai#9 P2c; shared by wio-lite-ai and f746g-disco since
 *          issue #161; one buffer since issue #191).
 *
 * svc/nn/tflm/nn_tflm.cc owns no storage (cmake/check_no_mutable_storage.py audits it
 * per board), and where these buffers live is a fact of each board's memory map:
 * wio-lite-ai's cacheable, CPU-only PSRAM carve-out (.psram_ai), f746g-disco's SDRAM
 * bank3 (.sdram.ai).  So the board DEFINES them, with its placement attribute, in
 * boards/<board>/port/nn/tflm/nn_tflm_bufs.c, and this header declares them by name.
 * The board's definition includes this header, so the compiler checks that the type
 * -- and with it the size -- of each definition is the one declared here.
 *
 * [!] THE DEFINITIONS ARE IN C, NOT C++, ON PURPOSE (wio-lite-ai#9 P2c, two review
 * rounds).  The placement gates (wio-lite-ai's check_psram_ai_residency.py,
 * f746g-disco's check_f746_layout.py --require-sdram-ai) match residents by exact
 * symbol name.  A C++ file-scope or anonymous-namespace object is emitted under a
 * mangled name, so a gate written in C's terms would see "no such object" -- or, after
 * a rename, check nothing.  And `extern "C" uint8_t buf[N];` in C++ is a DECLARATION
 * ([dcl.link]), not a definition.  Keeping the definitions in a C translation unit
 * makes the symbol names the plain ones these gates name.
 *
 * [!] ONE COMPILE DEFINITION PER SIZE, and no fallback: a missing one is a build
 * error rather than a default that differs from what the board's cmake meant.
 *   NN_TFLM_ARENA_KB   the activation arena reservation (the `arena : N B reserved`
 *                      line of `nn info`; what a model USES is reported separately)
 *   NN_TFLM_MODEL_KB   the one staging buffer
 */
#ifndef NN_TFLM_MEM_H
#define NN_TFLM_MEM_H

#include <stdint.h>

#ifndef NN_TFLM_ARENA_KB
#error "NN_TFLM_ARENA_KB is not defined: the board's cmake/tflite-micro.cmake passes it"
#endif
#ifndef NN_TFLM_MODEL_KB
#error "NN_TFLM_MODEL_KB is not defined: the board's cmake/tflite-micro.cmake passes it"
#endif

#define NN_TFLM_ARENA_BYTES  ((uint32_t)(NN_TFLM_ARENA_KB) * 1024u)

/*
 * ONE staging buffer, and it is where the open model lives (issue #191; two
 * double-buffered slots before).  load_region() hands it out only while no model is
 * open, so a load takes the open model down before it reads the new bytes in
 * (svc/nn/nn_core_model.h, staging_is_open), and a refused load ends with no model.
 * That is the price, and it is a small one: the previous model is not lost, it is
 * still in the board's store and is loaded again by name (a blob slot on
 * wio-lite-ai, a file on f746g-disco).  What it buys is the whole region for one
 * model -- twice the largest model of the two-slot layout -- while the arena keeps
 * its reservation and the rest of the carve-out stays spare.  This is the backend's
 * policy, not a board fact.
 */
#define NN_TFLM_MODEL_MAX    ((uint32_t)(NN_TFLM_MODEL_KB) * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/** TFLM's activation arena.  The board's placement; CPU-only. */
extern uint8_t nn_tflm_arena[NN_TFLM_ARENA_BYTES];

/** The model staging buffer.  The board's placement; CPU-only; at least
 *  NN_MODEL_ALIGN-aligned (tflite::GetModel() reads the root table in place). */
extern uint8_t nn_tflm_model_buf[NN_TFLM_MODEL_MAX];

/**
 * [!] THE STAGING CAPACITY, AS ONE OBJECT THE DEVICE READS AND THE HOST CAN READ
 * BACK (f746g-disco issue #159 Step 4b).  load_region() hands it out as the read
 * limit, and stage_capacity() answers it while a model is still open (issue #191:
 * a board checks a length against it before the open model goes down), both
 * through a volatile read so the limit is THIS object and not a constant the
 * compiler folded beside it; the post-link cmake/check_blob_staging.py reads the
 * same object's bytes out of the shipped shell.elf to refuse a slot larger than
 * it (issue #194).  The board defines it as NN_TFLM_MODEL_MAX.
 */
extern const uint32_t nn_tflm_stage_cap;

#ifdef __cplusplus
}
#endif

#endif /* NN_TFLM_MEM_H */
