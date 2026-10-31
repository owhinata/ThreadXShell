/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_backend.h
 * @brief   Internal vtable a concrete nn backend implements
 * (owhinata/wio-lite-ai#9 P1, owhinata/stm32f746g-disco#81; one header for
 * wio-lite-ai and f746g-disco since issue #161).
 *
 * svc/nn/nn.c dispatches the public nn.h API to exactly one backend through
 * this vtable.  Each backend (nn_null.c, tflm/nn_tflm.cc, f746g-disco's
 * nn_stedgeai*.c) defines its own private model handle type and exports a
 * single `const struct nn_backend_vt nn_backend_vt_selected`.
 *
 * HOW "EXACTLY ONE" IS ENFORCED: purely at link time.  CONFIG_NN_BACKEND adds
 * one -- and only one -- backend translation unit to the build, and nn.c's
 * extern reference below resolves against it.  There is no #ifdef fan-out
 * inside any source file.
 *
 * The vtable operates on an opaque backend handle (`void *impl`); nn.c wraps it
 * in the public `struct nn_model` and adds the cross-backend concerns.  run()
 * is the pure inference call -- it must NOT time itself; nn.c owns timing.
 *
 * OPTIONAL ENTRIES.  load_region, reload and release may be NULL (a backend
 * that cannot swap its model leaves all three that way).  nn.c checks for NULL
 * and returns NN_ERR_NOSUP itself, so no caller ever reads a vtable field:
 * "this backend cannot do that" is a value the public API returns, not a
 * branch to address 0 -- which on these parts is ITCM / vector code rather
 * than a clean fault.
 */
#ifndef NN_BACKEND_H
#define NN_BACKEND_H

#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nn_backend_vt {
	const struct nn_backend_info *info;

	/** One-time init; idempotent. 0 on success, <0 on failure. */
	int  (*init)(void);

	/** Open the singleton model; set *impl_out. 0 on success, <0 otherwise.
	 *  MUST NOT touch tensor bodies -- only fill the struct nn_tensor
	 *  descriptors.  `nn info` calls this without bringing the external memory
	 *  up, so a backend whose buffers live there would fault or stall if
	 *  open() dereferenced them. */
	int  (*open)(void **impl_out);
	void (*close)(void *impl);

	const char *(*model_name)(void *impl);
	int  (*in_count)(void *impl);
	int  (*out_count)(void *impl);
	struct nn_tensor *(*input)(void *impl, int idx);
	struct nn_tensor *(*output)(void *impl, int idx);
	/** What the open model's activations take of the arena (issue #131 P3):
	 *  0 with no model.  NOT the reservation -- that is arena_reserved. */
	uint32_t (*activations_bytes)(void *impl);

	/** Pure inference (no timing). Inputs pre-filled; 0 on success, <0 on error. */
	int  (*run)(void *impl);

	/* ---- optional: runtime model swap (owhinata/wio-lite-ai#9 phase 2c,
	 * owhinata/stm32f746g-disco#89 P2) ------------------------------------
	 *
	 * Only a backend that INTERPRETS a model at run time can implement these.
	 * Where the bytes come from (a NOR blob, a file on the SD card) is the
	 * board adapter's business; the backend only owns the buffer.
	 */

	/**
	 * Hand out the staging buffer a model should be read into, and its
	 * capacity.  The caller fills [*buf, *buf + cap) and passes a range inside
	 * it to reload().  The buffer is BACKEND-OWNED and must be the one NOT
	 * currently in use, so that filling it cannot disturb a model that is
	 * still running.  0 on success.
	 */
	int  (*load_region)(void **buf, uint32_t *cap);

	/**
	 * Rebuild the model from @p data (@p len bytes), naming it @p name for
	 * display.  nn.c never passes @p data == NULL (an unload is release()).
	 *
	 * TRANSACTIONAL: on failure the previously active model is restored -- or,
	 * when nothing was loaded before or even the restore failed, the backend
	 * stays open and EMPTY (issue #131 P16).  *@p impl_out is ALWAYS set to the
	 * resulting handle: the new model (returns 0), the restored previous one
	 * (returns <0), or the empty handle (returns <0).  Returns 0, or the <0
	 * code for why the NEW model was rejected (NN_MODEL_ERR_*), unchanged.
	 * "Empty" is a handle with no inputs; see nn_model_present().
	 */
	int  (*reload)(const void *data, uint32_t len, const char *name, void **impl_out);

	/**
	 * Unload: leave the backend open with NO model -- no inputs, the name
	 * "(none)" -- until a later reload() installs one (issue #131 P16).  0 on
	 * success, <0 otherwise.
	 */
	int  (*release)(void);

	/**
	 * The activation arena this backend RESERVES, in bytes, whatever is open:
	 * the `arena : N B reserved` line of `nn info` (issue #131 P3).  0 for a
	 * backend that has none (the `null` stub).
	 */
	uint32_t arena_reserved;

	/**
	 * 1 when a model is in the image and the first open adopts it without a
	 * load (the null stub, f746g-disco's stedgeai generated model); 0 for a
	 * backend that is empty until a load (tflm, stedgeai_reloc).
	 */
	uint8_t has_builtin;
};

/** The one backend selected at build time (provided by exactly one backend TU). */
extern const struct nn_backend_vt nn_backend_vt_selected;

#ifdef __cplusplus
}
#endif

#endif /* NN_BACKEND_H */
