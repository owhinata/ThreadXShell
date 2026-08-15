/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu.h
 * @brief   Ethos-U55 inference, C surface (issue #44).
 *
 * The interpreter underneath is C++ (TFLite Micro), and it stays there: this
 * header is the only thing the shell command and the rest of the port see, so
 * no C++ type or header escapes into the board's C sources.
 *
 * Everything here is SINGLE-OWNER.  The interpreter, the driver state and the
 * arena are all static, so exactly one caller may hold the NPU at a time -- the
 * caller is responsible for the ownership gate (the shell runs foreground and
 * background jobs concurrently, so this is not theoretical).  The functions
 * below do not serialise themselves; calling them from two threads is a bug the
 * layer above must prevent.
 */
#ifndef NPU_H
#define NPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Status codes.  Negative is failure; the value says which stage refused. */
enum npu_status {
	NPU_OK               =  0,
	NPU_ERR_STATE        = -1,  /**< called out of order (already open / not open) */
	NPU_ERR_MODEL_ADDR   = -2,  /**< model address not in the flash read window */
	NPU_ERR_MODEL_MAGIC  = -3,  /**< no TFL3 identifier where the model should be */
	NPU_ERR_SCHEMA       = -4,  /**< flatbuffer schema version mismatch */
	NPU_ERR_OPS          = -5,  /**< the op resolver rejected the operator set */
	NPU_ERR_ARENA        = -6,  /**< AllocateTensors failed: arena too small */
	NPU_ERR_TENSORS      = -7,  /**< unexpected input/output tensor shape or type */
	NPU_ERR_INVOKE       = -8,  /**< the interpreter reported a run failure */
};

/** What a tensor looks like, flattened enough to describe over a C boundary. */
struct npu_tensor {
	void    *data;      /**< in the arena; valid only while the model is open */
	size_t   bytes;
	int32_t  dims[4];   /**< trailing unused entries are 0 */
	uint8_t  rank;
	int8_t   type;      /**< TfLiteType, opaque here (see npu_type_name()) */
	int32_t  zero_point;
	float    scale;
};

/**
 * Parse the model in place and lay out the arena.
 *
 * @param model_addr  address of the flatbuffer, IN THE MEMORY-MAPPED FLASH
 *                    READ WINDOW.  It is parsed where it lies and never copied,
 *                    so it must stay readable for as long as the model is open.
 * @param arena       cache-line-aligned scratch, NPU-visible (i.e. not TCM)
 * @param arena_bytes size of @p arena
 *
 * Validates the address range and the flatbuffer identifier BEFORE walking any
 * of it: an erased flash region reads as 0xFF and would otherwise be followed
 * as offsets.  On any failure nothing is left constructed.
 */
int npu_open(uint32_t model_addr, void *arena, size_t arena_bytes);

/** Tear down the interpreter.  Safe to call when not open. */
void npu_close(void);

/** Describe input/output tensor 0.  Only valid while open. */
int npu_input(struct npu_tensor *out);
int npu_output(unsigned index, struct npu_tensor *out);

/** How many outputs the model has (0 when not open). */
unsigned npu_output_count(void);

/**
 * Run one inference.  Blocks the calling thread; other shell threads keep
 * running, because the driver's semaphore is a ThreadX one (see npu_rtos.c).
 *
 * Cache maintenance around the run belongs to the CALLER: clean the input
 * before calling, invalidate the outputs after this returns.  The vendor hooks
 * that would otherwise do it are deliberately neutered -- see npu_cache.c.
 */
int npu_invoke(void);

/** Arena bytes actually used by the current layout (0 when not open). */
size_t npu_arena_used(void);

/** The statically reserved arena (npu_arena.c).  Caller passes it to npu_open;
 *  it is not taken implicitly, so a test can hand in something else. */
void  *npu_arena_base(void);
size_t npu_arena_bytes(void);

/** Human-readable names, for the shell to print. */
const char *npu_status_name(int status);
const char *npu_type_name(int8_t type);

#ifdef __cplusplus
}
#endif

#endif /* NPU_H */
