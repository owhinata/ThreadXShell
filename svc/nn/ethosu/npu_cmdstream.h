/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_cmdstream.h
 * @brief   Where an Ethos-U55 command stream makes the NPU read and write
 *          (issue #184).
 *
 * Plain C over plain words, like npu_payload.h: no TFLite type and no driver
 * header reaches this, so the host test and the host gate compile the real
 * implementation.  Finding the stream and the tensors behind each base pointer
 * is the caller's job (npu_model_scan.cc); deciding whether every address the
 * stream can make the NPU touch stays inside them is this one's.
 *
 * WHY IT IS NEEDED.  A region (base pointer) is a number the NPU ADDS to an
 * offset, not a limit.  The ethos-u kernel hands the driver a size per base
 * pointer, but the driver gives those sizes only to the cache hooks (which this
 * port ignores) -- the NPU itself is told addresses and nothing else, and its
 * accesses are outside the CPU's MPU.  So whatever the command stream's DMA
 * and operation descriptors say is what the NPU reads and writes, including
 * memory that is not the model or the arena at all.  This walk reconstructs,
 * command by command, the range each DMA and each operation can reach, and
 * refuses the stream unless every range lies inside the tensor that region
 * stands for.
 *
 * WHAT IT DOES NOT CLAIM.  The ranges are a conservative upper bound built from
 * the Ethos-U55 Technical Reference Manual (102420_0200_02) and Vela's own
 * address model, not something measured against the hardware.  A stream shape
 * whose range this file cannot write down is refused rather than guessed.
 */
#ifndef NPU_CMDSTREAM_H
#define NPU_CMDSTREAM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The NPU has 8 region registers a stream can name (TRM Table 4-112: 0-7). */
#define NPU_CS_MAX_REGIONS 8u

/** One base pointer the ethos-u kernel hands the driver. */
struct npu_cs_region {
	/** Byte size of the tensor behind this base pointer. */
	uint32_t bytes;
	/** True only when the tensor carries no constant data, so the NPU may
	 *  write it (an operation's output or a DMA destination). */
	bool writable;
};

/**
 * The NPU configuration the stream was compiled for: facts of the board, not of
 * the walk.  The board states them once (grove-vision-ai-v2: npu_hw.h, beside
 * the Vela --accelerator-config they belong to).
 */
struct npu_cs_config {
	/** The internal SHRAM (TRM 4.2.11, 8-48 KB); bounds the one internal DMA
	 *  accepted, a lookup table load into its last 2 KB. */
	uint32_t shram_bytes;
	/** The minimum output block of TRM 4.9 Table 4-139 for the configuration
	 *  (width and height 1 or 2, depth 4 or 8); every OFM block size the
	 *  stream sets must be a multiple of it. */
	uint8_t min_block_width;
	uint8_t min_block_height;
	uint8_t min_block_depth;
};

/** Optional report: the range the stream reaches in one region. */
struct npu_cs_extent {
	int64_t lo;     /**< lowest offset touched */
	int64_t hi;     /**< one past the highest byte touched */
	bool touched;   /**< false: nothing in this region was touched */
};

/**
 * Why a stream is refused.  One value per rule, so the host gate and the
 * firmware can say the same thing (npu_cmdstream_refusal_text()).
 */
enum npu_cs_refusal {
	NPU_CS_OK = 0,
	NPU_CS_BAD_ARGUMENTS,      /**< NULL stream / table / configuration,
	                            *   > 8 regions, a SHRAM size outside
	                            *   8-48 KB or a minimum block that is not
	                            *   in TRM Table 4-139 */
	NPU_CS_UNKNOWN_COMMAND,    /**< C1: not in both the TRM and the driver */
	NPU_CS_TRUNCATED_COMMAND,  /**< C1: a cmd1's data word is past the end */
	NPU_CS_ADDRESS_EXTENSION,  /**< C2: address bits 47..32 not zero */
	NPU_CS_REGION_NOT_PASSED,  /**< C3: region >= the number passed */
	NPU_CS_DMA_SOURCE_INTERNAL,/**< C3: a DMA reading NPU-internal memory */
	NPU_CS_DMA_STRIDE_MODE,    /**< C3: a DMA that is not 1D */
	NPU_CS_DMA_DEST_INTERNAL,  /**< U5: a DMA into NPU-internal memory that
	                            *   is not a lookup table load inside the
	                            *   last 2 KB of the SHRAM */
	NPU_CS_STATE_UNSET,        /**< C4b: an operation uses a register the
	                            *   stream never set */
	NPU_CS_BELOW_REGION,       /**< C4: an access before offset 0 */
	NPU_CS_PAST_REGION,        /**< C4: an access past the tensor's end */
	NPU_CS_WRITE_TO_DATA,      /**< C5: a write into a constant tensor */
	NPU_CS_PARAMETER_RANGE,    /**< C6: a value the TRM gives no meaning */
	NPU_CS_NO_FINAL_STOP,      /**< C7: the last command is not a stop */
};

/**
 * Check every address @p words can make the NPU read or write.
 *
 * @param words     the command stream exactly as the driver hands it to the
 *                  NPU (QBASE), as 32-bit words
 * @param nwords    its length in words (QSIZE / 4)
 * @param regions   one entry per base pointer the kernel passes, in the
 *                  kernel's order (inputs 1.. then outputs, cut at 8)
 * @param nregions  how many entries @p regions has (<= NPU_CS_MAX_REGIONS)
 * @param config   the NPU configuration the board compiles models for (its
 *                  SHRAM size and minimum output block)
 * @param extent    NULL, or @p nregions entries to receive the range the
 *                  stream reaches in each region (filled as far as the walk
 *                  got, also on refusal)
 *
 * State is on the stack only (about 0.6 KB measured at -Os on the board
 * toolchain); the walk reads each word once.
 */
enum npu_cs_refusal npu_cmdstream_check(const uint32_t *words, uint32_t nwords,
                                        const struct npu_cs_region *regions,
                                        uint32_t nregions,
                                        const struct npu_cs_config *config,
                                        struct npu_cs_extent *extent);

/**
 * The sentence for @p r -- the same one in the firmware and the host gate.
 * NULL for NPU_CS_OK; a generic sentence for a value outside the enum.
 */
const char *npu_cmdstream_refusal_text(enum npu_cs_refusal r);

#ifdef __cplusplus
}
#endif

#endif /* NPU_CMDSTREAM_H */
