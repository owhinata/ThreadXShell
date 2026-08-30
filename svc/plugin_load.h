/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_load.h
 * @brief   Container and manifest validation, board-independent (issue #101).
 *
 * Everything a loader must establish BEFORE the first instruction of a plugin
 * is fetched.  Step 1a stops here: nothing in this file copies, relocates or
 * branches, and the result it hands back carries no callable pointer.
 *
 * [!] THE RETURN TYPE IS THE GUARANTEE, NOT THE COMMENT.  @ref plugin_view is
 * plain data -- integer offsets and copied bytes.  It has no function-pointer
 * member, so "1a does not execute a plugin" is not a discipline someone has to
 * keep; there is nothing here to call.  Step 1b adds the code that turns an
 * offset into an address, and that code is where the MPU, cache and admission
 * checks live.
 *
 * [!] THE POLICY COMES FROM THE BOARD.  svc/ includes no board header and holds
 * no board address: the prelink base, the reservation capacity, the target
 * identity and the per-slot stack allowances all arrive in
 * @ref plugin_policy, which the board owns.  A constant like 0x341E0000 living
 * here would be Grove policy compiled into the f746 and wio firmwares.
 *
 * [!] REFUSALS ARE NOT FOLDED TOGETHER.  Each @ref plugin_result names one
 * reason.  "The container is malformed" would be true of every failure and
 * would tell whoever is holding the board nothing about which of them happened
 * -- and several of these are ordinary operator mistakes (a container built for
 * another board, a stale ABI) that must not read like corruption.
 */
#ifndef PLUGIN_LOAD_H
#define PLUGIN_LOAD_H

#include <stddef.h>
#include <stdint.h>

#include "plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what the board supplies -------------------------------------------- */

/**
 * The board's immutable policy for plugins.
 *
 * @ref stack_limit is what the THREAD can spare for a plugin callback, which is
 * not the same as what the plugin says it needs: a manifest that agrees with
 * the host gate's analysis can still ask for more than the panel thread has.
 * A limit of 0 refuses that slot outright.
 */
struct plugin_policy {
	uint32_t target_id;        /**< plugin_target_id(), compared exactly   */
	uint32_t link_addr;        /**< the prelink base this board reserves   */
	uint32_t capacity;         /**< bytes reserved for the load image      */
	uint32_t image_align;      /**< usually PLUGIN_IMAGE_ALIGN             */
	uint32_t caps_supported;   /**< capability bits this base implements   */
	uint32_t stack_limit[PLUGIN_SLOT_COUNT];
};

/* ---- what a caller gets back -------------------------------------------- */

/**
 * A validated container, as plain data.
 *
 * Every offset is from CONTAINER BYTE 0 -- including the ones the manifest
 * expressed relative to its image, which are rebased here so that a consumer
 * never adds two numbers it checked separately.  @ref slot entries keep the
 * Thumb bit exactly as the manifest carried it; stripping it would throw away
 * something the loader checked.
 */
struct plugin_view {
	uint32_t model_off, model_len;
	uint32_t data_off,  data_len;      /**< 0/0 when absent               */

	uint8_t  has_plugin;
	uint32_t image_off;                /**< load image, from container 0  */
	uint32_t file_size, mem_size;
	uint32_t code_off,    code_len;    /**< all rebased to container 0    */
	uint32_t data_seg_off, data_seg_len;
	uint32_t bss_off,     bss_len;
	uint32_t scratch_off, scratch_len;
	uint32_t slot[PLUGIN_SLOT_COUNT];  /**< image-relative, or ABSENT     */
	uint32_t stack[PLUGIN_SLOT_COUNT];
	uint32_t capability;
	uint32_t link_addr;
	/*
	 * The digest the container carried and this parse checked (issue #103).
	 *
	 * [!] THE ONLY HONEST IDENTITY OF THE LOADED BYTES.  The manifest's build id
	 * is a source revision, stamped at configure time -- edit a plugin, rebuild
	 * without reconfiguring, and it does not move.  The image size does not
	 * always move either: a one-character fix changes neither.  This is a CRC of
	 * the plugin section itself, so it moves whenever the bytes do, which is
	 * what somebody looking at `nn info` after a re-send actually needs.
	 */
	uint32_t digest;
	char     name[PLUGIN_NAME_MAX];
	char     build_id[PLUGIN_BUILD_ID_MAX];
};

/** What a payload turned out to be. */
enum plugin_kind {
	PLUGIN_KIND_CONTAINER = 0,  /**< magic and format both matched        */
	PLUGIN_KIND_TFLITE    = 1,  /**< bare model: "TFL3" at bytes 4..7     */
	PLUGIN_KIND_UNKNOWN   = 2,  /**< neither; the caller decides          */
};

/**
 * Every way validation can refuse.
 *
 * Ordered so that the earliest structural failures come first; the name is what
 * a console prints, so keep them short and specific.
 */
enum plugin_result {
	PLUGIN_OK = 0,
	PLUGIN_ERR_SHORT,          /**< fewer bytes than the header needs      */
	PLUGIN_ERR_MAGIC,
	PLUGIN_ERR_FORMAT,
	PLUGIN_ERR_HDR_SIZE,
	PLUGIN_ERR_ABI,            /**< abi_version is not this one            */
	PLUGIN_ERR_TOTAL_SIZE,     /**< total_size disagrees with the payload  */
	PLUGIN_ERR_RESERVED,       /**< a reserved field or pad byte was set   */
	PLUGIN_ERR_SECTION_COUNT,
	PLUGIN_ERR_SECTION_TYPE,   /**< 0, unknown, or a duplicate kind        */
	PLUGIN_ERR_SECTION_RANGE,  /**< overflow, or past the container        */
	PLUGIN_ERR_SECTION_OVERLAP,
	PLUGIN_ERR_NO_MODEL,       /**< exactly one MODEL is required          */
	PLUGIN_ERR_MODEL_ALIGN,
	PLUGIN_ERR_MANIFEST_SHORT,
	PLUGIN_ERR_MANIFEST_MAGIC,
	PLUGIN_ERR_MANIFEST_SIZE,
	PLUGIN_ERR_TARGET,         /**< built for another board                */
	PLUGIN_ERR_LINK_ADDR,      /**< prelinked somewhere this board has not */
	PLUGIN_ERR_CAPABILITY,     /**< unknown bit, or bit/slot disagreement  */
	PLUGIN_ERR_IMAGE_RANGE,    /**< image does not lie inside its section  */
	PLUGIN_ERR_IMAGE_ALIGN,
	PLUGIN_ERR_SIZES,          /**< file_size > mem_size, or > capacity    */
	PLUGIN_ERR_SEG_RANGE,      /**< a segment escapes the image            */
	PLUGIN_ERR_SEG_OVERLAP,
	PLUGIN_ERR_SLOT_MISSING,   /**< a mandatory slot is absent             */
	PLUGIN_ERR_SLOT_RANGE,     /**< a slot does not land in code           */
	PLUGIN_ERR_SLOT_THUMB,     /**< Thumb bit clear, or misaligned         */
	PLUGIN_ERR_STACK,          /**< a slot asks for more than the thread   */
	PLUGIN_ERR_DIGEST,
	PLUGIN_ERR_ARG,
};

/* ---- entry points -------------------------------------------------------- */

/**
 * @brief  Decide what a blob payload is, without validating it.
 *
 * Cheap and total: it reads at most the first eight bytes.  A caller uses it to
 * keep the pre-container path for a bare model exactly as it was -- the two
 * models already on the device were sent before containers existed and must not
 * need re-sending.
 *
 * @param buf  payload start; may be in a memory-mapped flash window
 * @param len  bytes the caller knows are readable
 */
enum plugin_kind plugin_probe(const void *buf, size_t len);

/**
 * @brief  Validate a container in full and fill in @p out.
 *
 * Runs every check listed in issue #101 before returning OK, in structural
 * order: header, section table, then -- only if a plugin section is present --
 * the manifest, its segments, its slots and its digest.
 *
 * @param buf  container start
 * @param len  bytes readable, which must equal the container's total_size
 * @param pol  the board's policy; must not be NULL
 * @param out  filled only on PLUGIN_OK, and zeroed first
 *
 * [!] A CONTAINER WITH NO PLUGIN IS VALID.  `out->has_plugin` is 0 and the model
 * (and any data section) is still usable: a label table travelling with a model
 * needs no code.  Callers must not treat "no plugin" as a failure.
 */
enum plugin_result plugin_parse(const void *buf, size_t len,
                                const struct plugin_policy *pol,
                                struct plugin_view *out);

/** Short name of a result, for a console line.  Never NULL. */
const char *plugin_result_name(enum plugin_result r);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_LOAD_H */
