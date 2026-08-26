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

#include <stdbool.h>
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
	NPU_ERR_PAYLOAD      = -9,  /**< driver actions continue past the command
	                                 stream, so a launch could be abandoned with
	                                 the arena still owned by the NPU (#46) */
	/**
	 * The flatbuffer does not hold together within its declared length: an
	 * offset leads outside the buffer, a vector runs past its end, the
	 * nesting is deeper or wider than the limits allow (#93).
	 *
	 * [!] SEPARATE FROM _MAGIC AND _SCHEMA ON PURPOSE.  Those two say "this is
	 * not a model" and "it is a model this build cannot read"; this one says
	 * "these bytes claim to be a model and are internally broken", which is
	 * the answer an operator gets after a transfer that arrived intact -- the
	 * blob CRC is of the stream that was sent, so a file that was corrupt
	 * before it left the PC verifies perfectly and fails HERE.
	 */
	NPU_ERR_MODEL_FORMAT = -10,
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
 * @param model_len   how many bytes of model there are at @p model_addr
 * @param arena       cache-line-aligned scratch, NPU-visible (i.e. not TCM)
 * @param arena_bytes size of @p arena
 *
 * Validates the address range, the length, the flatbuffer identifier and then
 * the whole flatbuffer BEFORE walking any of it as a model: an erased flash
 * region reads as 0xFF and would otherwise be followed as offsets.  On any
 * failure nothing is left constructed.
 *
 * [!] @p model_len IS THE BOUNDS CHECK AND IT IS NOT OPTIONAL (#93).
 * tflite::GetModel() is a cast; every accessor after it follows an offset out
 * of the buffer, and until #93 there was no buffer -- only an address with
 * "enough room for a header" behind it, so a malformed model could reference
 * the neighbouring slot, the rest of the store, or the bootloader's own block
 * at the top of the part.  The length is what turns that into a bounded
 * question, and the caller has to have it from somewhere that KNOWS: a blob
 * header, or an operator who typed it.  Passing "the rest of the window" would
 * satisfy the arithmetic and check nothing.
 */
int npu_open(uint32_t model_addr, uint32_t model_len, void *arena,
             size_t arena_bytes);

/** The shortest and longest @p model_len npu_open() will consider, so that a
 *  caller can refuse with its own vocabulary before getting here -- and so the
 *  host gate and the shell agree on the floor.  The floor exists because the
 *  file identifier is read at offset 4: a shorter length would have the
 *  identifier check reading outside the bounds the caller declared. */
uint32_t npu_model_len_min(void);
uint32_t npu_model_len_max(uint32_t model_addr);

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
 * CACHE MAINTENANCE IS NOT THE CALLER'S (issue #46).  The whole arena changes
 * hands at two instants inside Invoke() -- immediately before the command
 * stream is launched and once completion is confirmed -- and the port hangs the
 * clean and the invalidate off the driver's callbacks there (npu_cache.c).
 * Doing anything from outside is either too early (the ethos-u kernel writes
 * arena scratch afterwards) or too late (TFLM writes the arena before Invoke()
 * returns).  By the time this returns, the outputs are visible.
 */
int npu_invoke(void);

/**
 * Is @p type the int8 element type?
 *
 * npu_tensor::type is an opaque TfLiteType on this side of the boundary, on
 * purpose -- no TFLite header reaches the C sources.  A model decoder still has
 * to know that a tensor holds int8 before casting its buffer, so the question
 * is answered by a function whose implementation asserts against the real
 * enumerator, rather than by a public constant that would be a second copy of
 * it waiting to disagree.
 */
bool npu_tensor_is_int8(int8_t type);

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
