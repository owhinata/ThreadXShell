/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn.h
 * @brief   Backend-agnostic on-device neural-network inference API
 * (owhinata/wio-lite-ai#9 P1, owhinata/stm32f746g-disco#81; one header since
 * issue #161).
 *
 * A thin tensor-in / tensor-out abstraction over one selectable inference
 * runtime.  Exactly one backend is compiled in per build (CONFIG_NN_BACKEND):
 * `null` (no runtime -- a synthetic stub that exercises the plumbing end to end
 * and always builds), `tflm` (LiteRT / TensorFlow Lite Micro with CMSIS-NN) and,
 * on f746g-disco, `stedgeai` / `stedgeai_reloc` (ST Edge AI Core).  The
 * abstraction is deliberately model-agnostic: what a model's outputs mean
 * belongs ABOVE this layer, so the same API serves any model the backend can
 * run.
 *
 * WHO USES IT: wio-lite-ai and f746g-disco.  grove-vision-ai-v2 has its own
 * Ethos-U path (port/nn/npu/) and does not include this header.
 *
 * ONE IMPLEMENTATION (issue #161): svc/nn/nn.c, for both boards.  It owns no
 * storage: its state is the board's object and its critical section, tick wait
 * and cycle counter are the board's hooks (svc/nn/nn_dispatch_state.h, defined in
 * boards/<board>/port/nn/nn_model_board.c).  A declaration marked with one
 * board's name is one only that board calls.
 *
 * Layering: no HAL, CMSIS or ThreadX here or in nn.c; the board's hooks carry
 * those.  Nothing here reaches up into shell/.
 *
 * Threading: a single model instance is NOT reentrant, and both boards have
 * more than one console that can issue `nn` commands.  Callers serialize with
 * nn_session_try_acquire() / nn_session_release() below.
 *
 * Timing: nn_run() captures the elapsed DWT cycle count for nn_last_cycles().
 * nn_run() is CPU-bound and never sleeps, so the counter does not freeze
 * mid-run (it only stops in WFI).  On wio-lite-ai the counter belongs to
 * svc/timebase.c and this layer only READS it.
 */
#ifndef NN_H
#define NN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Element type of a tensor buffer. */
enum nn_dtype {
	NN_DTYPE_NONE = 0,
	NN_DTYPE_INT8,
	NN_DTYPE_UINT8,
	NN_DTYPE_INT16,
	NN_DTYPE_INT32,
	NN_DTYPE_FLOAT32,
};

/** Maximum tensor rank and per-model I/O tensor count this API exposes. */
#define NN_MAX_DIMS 4
#define NN_MAX_IO   8

/*
 * Error codes this layer returns for its OWN failures, kept far from zero so
 * they cannot be confused with a backend's.  A backend reports why a particular
 * model was rejected with its own small negative numbers (the TFLM backend uses
 * NN_MODEL_ERR_*); those pass through unchanged, because only the backend knows
 * what went wrong inside it.  [!] No code is folded into another on the way
 * out: each refusal reaches the caller as the value that was returned.
 */
#define NN_ERR_ARG    (-90)   /**< a required argument was NULL or out of range     */
#define NN_ERR_NOSUP  (-91)   /**< the compiled-in backend does not implement this  */
#define NN_ERR_STATE  (-92)   /**< no model is open; call nn_model_open() first     */

/**
 * f746g-disco: returned by nn_session_try_acquire(), the first nn_model_open()
 * after boot, nn_model_load_region() and nn_model_reload() while
 * nn_scratch_claim() is held.  Retryable: nothing was built or changed, and the
 * same call succeeds once `sdram test` has finished.  Distinct from the
 * backends' small negatives.
 */
#define NN_ERR_SCRATCH_HELD  (-50)

/*
 * Why a backend refused a model (nn_model_reload).  These live here rather than
 * inside the backend so that the shell command reporting them stays
 * backend-agnostic: the reasons an interpreting runtime can reject a flatbuffer
 * are generic, and a command that had to know TFLM's private numbering would
 * have to be rewritten for the next backend.  nn_model_strerror() turns one
 * into a sentence.  (The tflm backend returns these on both boards since issue
 * #161; f746g-disco's stedgeai backends still return their own unnamed small
 * negatives, which do not all mean the same.)
 */
#define NN_MODEL_ERR_VERSION  (-1)  /**< schema version the runtime cannot read      */
#define NN_MODEL_ERR_OPS      (-2)  /**< the operator resolver could not be built    */
#define NN_MODEL_ERR_ARENA    (-3)  /**< activations do not fit, or an op is missing */
#define NN_MODEL_ERR_TENSOR   (-4)  /**< a tensor has no buffer or too high a rank   */
#define NN_MODEL_ERR_SHAPE    (-5)  /**< I/O tensor count outside 1..NN_MAX_IO       */
#define NN_MODEL_ERR_EMPTY    (-6)  /**< zero-length or impossibly short model       */
#define NN_MODEL_ERR_FORMAT   (-7)  /**< not a valid flatbuffer for this runtime     */
#define NN_MODEL_ERR_SLOT     (-8)  /**< not inside the region load_region() gave    */
#define NN_MODEL_ERR_ALIGN    (-9)  /**< model does not start on NN_MODEL_ALIGN       */

/*
 * Refusals only a backend that interprets a model IN PLACE gives (issue #169, the
 * Ethos-U backend of grove-vision-ai-v2).  Each is its own code because each sends
 * the operator somewhere different, and a folded one would hide which:
 *
 *   _WINDOW   the counterpart of _SLOT: the range is not inside the read window the
 *             board declares (the address, or the address plus the length).
 *   _IDENT    no "TFL3" file identifier at offset 4.  Kept apart from _FORMAT on
 *             purpose: "this is not a model" (erased flash, the wrong address) is a
 *             different answer from "these bytes claim to be a model and do not hold
 *             together", which is what an intact transfer of a file that was already
 *             corrupt on the PC gets.  (The tflm backend folds the identifier into
 *             _FORMAT and is not changed by this.)
 *   _PAYLOAD  the Ethos-U operator's driver payload is not exactly one
 *             COMMAND_STREAM action, as the last action -- the shape the driver can
 *             launch without being able to abandon the launch with the arena still
 *             owned by the NPU (svc/nn/ethosu/npu_payload.h).
 *
 * nn_model_strerror() does not word these: the backend that returns them supplies
 * the sentence (svc/nn/ethosu/) and hands every other code to nn_model_strerror().
 */
#define NN_MODEL_ERR_WINDOW   (-10) /**< not inside the board's model read window    */
#define NN_MODEL_ERR_IDENT    (-11) /**< no TFL3 file identifier at offset 4         */
#define NN_MODEL_ERR_PAYLOAD  (-12) /**< Ethos-U payload is not one final command stream */

/**
 * The alignment a model handed to nn_model_reload() must start on (issue #108,
 * f746g-disco since issue #159).
 *
 * A plugin container hands its MODEL SECTION over in place, at an offset inside
 * the staging buffer, so the model no longer always starts at the buffer's
 * (32-byte aligned) base.  A converted .tflite aligns the buffers inside it
 * relative to the start of the FILE -- 16 bytes -- and the kernels read float32
 * weights with word loads that fault on these cores if misaligned, so the file
 * must start on a boundary at least that strict for its internal alignments to
 * survive.  svc/plugin/plugin_abi.h's PLUGIN_MODEL_ALIGN is a multiple of it
 * (asserted by each board's adapter where containers are split); the backend
 * checks it anyway.
 */
#define NN_MODEL_ALIGN        16u

/**
 * A single input or output tensor.  @p data points at the backend/model-owned
 * buffer the caller fills (inputs) or reads (outputs); the caller does not own or
 * free it.  For quantized (int8/uint8) tensors @p scale and @p zero_point carry the
 * affine quantization params (real = scale * (q - zero_point)); @p scale is 0 for
 * non-quantized tensors.  Dims are most- to least-significant (e.g. NHWC:
 * dims[0]=N, dims[1]=H, dims[2]=W, dims[3]=C).
 */
struct nn_tensor {
	void    *data;                 /**< tensor buffer (backend-owned)         */
	uint32_t bytes;                /**< buffer size in bytes                  */
	uint16_t dims[NN_MAX_DIMS];    /**< shape, MSB..LSB; unused dims are 1    */
	uint8_t  ndim;                 /**< number of valid dims                  */
	uint8_t  dtype;                /**< enum nn_dtype                         */
	float    scale;                /**< quant scale (0 => not quantized)      */
	int32_t  zero_point;           /**< quant zero point                      */
};

/** Opaque model handle (defined in svc/nn/nn_dispatch_state.h, for nn.c only). */
struct nn_model;

/** Identity of the compiled-in backend, for `nn info`. */
struct nn_backend_info {
	const char *name;      /**< "null" | "tflm" | "stedgeai" | ...            */
	const char *version;   /**< backend/runtime version string, or ""         */
};

/**
 * One-time backend init.  Idempotent and cheap to call repeatedly (the shell
 * calls it lazily through nn_model_open() on first `nn` use, so no NN bring-up
 * call is needed at boot).  Returns 0 on success, <0 on failure.
 */
int nn_init(void);

/** Identity of the compiled-in backend (never NULL, valid before any open). */
const struct nn_backend_info *nn_backend(void);

/**
 * Open the (single, compiled-in) model.  Returns 0 and sets @p *out on success,
 * <0 otherwise.  The handle is a singleton; a second open returns the same
 * instance.  Safe to call concurrently from several consoles -- the one-time
 * init is serialized internally.
 *
 * ONLY THE FIRST OPEN AFTER BOOT BUILDS ANYTHING (issue #131 P16).  That is
 * where a backend with a model in the image (null; stedgeai on f746g-disco) adopts it
 * -- tflm has none since issue #159 Step 4c and opens empty; a later open --
 * from `nn info`, `nn run` or a load -- returns the singleton as it is, empty
 * after an unload included.  The first one is serialised by a latch and does
 * not return until the build is done, so a release can never run under it.
 */
int  nn_model_open(struct nn_model **out);
void nn_model_close(struct nn_model *m);

/** Human-readable model name (never NULL). */
const char *nn_model_name(const struct nn_model *m);

int  nn_input_count(const struct nn_model *m);
int  nn_output_count(const struct nn_model *m);

/** Input/output tensor @p idx, or NULL if out of range. */
struct nn_tensor *nn_input(struct nn_model *m, int idx);
struct nn_tensor *nn_output(struct nn_model *m, int idx);

/** What the open model's activations take of the arena (bytes), for the `used`
 *  line of `nn info`; 0 with no model or when the backend does not say. */
uint32_t nn_activations_bytes(const struct nn_model *m);

/** The arena the compiled-in backend reserves (bytes), whatever is open -- the
 *  `arena : N B reserved` line of `nn info` (issue #131 P3); 0 when it has none. */
uint32_t nn_arena_reserved(void);

/**
 * Whether @p m has a model: open AND describing at least one input.
 *
 * [!] NOT nn_model_open()'s success (wio-lite-ai issue #122 P2, f746g-disco
 * issue #131 P16).  The singleton stays open once the first nn_model_open() has
 * run; the TFLM backend opens with no model and publishes zero tensors (its
 * steady state before the first load and after an unload), and it refuses to
 * build a model whose input count is outside its range -- so zero inputs means
 * exactly "nothing loaded".  What `nn model unload` takes away is the model, not
 * the handle.  The `null` backend's synthetic model has inputs and is reported
 * as present.  Asks; builds nothing.
 */
int nn_model_present(const struct nn_model *m);

/**
 * nn_model_present() of the singleton, WITHOUT opening it: 0 before the first
 * nn_model_open().  Builds nothing (issue #131 step 7d) -- the shared order in
 * svc/nn/nn_core_model.c asks it for a refusal before the claim and for had_open
 * under the claim, and a question must not construct what it asks about.
 */
int nn_model_loaded(void);

/** Whether this backend has a model in the image that the first open adopts
 *  without a load (null; stedgeai on f746g-disco).  0 for tflm (since issue
 *  #159 Step 4c) and stedgeai_reloc, which are empty until `nn model load`. */
int nn_model_has_builtin(void);

/**
 * Run one inference.  Inputs must be filled first; outputs are valid on return.
 * CPU-bound and blocking (it never sleeps).  Returns 0 on success, <0 on error.
 *
 * CONTRACT -- ONE RUN MUST BE SHORTER THAN THE CYCLE COUNTER WRAPS.  Timing uses
 * the 32-bit DWT CYCCNT, which wraps every 2^32 core cycles (7.81 s at 550 MHz).
 * The unsigned delta is wrap-safe for exactly one wrap, so a run that took longer
 * does not report an error -- it silently aliases down to a small,
 * plausible-looking number.  Keep a single run below 2^31 cycles; a backend that
 * could exceed that must move timing to a wider source rather than trusting
 * this field.
 */
int nn_run(struct nn_model *m);

/**
 * DWT core cycles spent inside the most recent successful nn_run(); 0 if none,
 * or if the cycle counter was found not to be running at init.
 */
uint32_t nn_last_cycles(const struct nn_model *m);

/*
 * Runtime model swap (owhinata/wio-lite-ai#9 phase 2c,
 * owhinata/stm32f746g-disco#89 P2).  Only a backend that interprets a model in
 * RAM supports it (tflm; stedgeai_reloc on f746g-disco); the others return <0
 * from the calls below rather than pretending.
 *
 * Which model to load and where it comes from are NOT decided here: that
 * belongs to the board's adapter (a blob slot on wio-lite-ai, a file on the SD
 * card on f746g-disco).  All this API says is: here is a buffer, fill it, tell
 * me you did.
 *
 *   nn_model_load_region(&buf, &cap);    // backend's writable staging buffer
 *   ...fill buf with up to cap bytes...
 *   nn_model_reload(buf, len, "name", &after);
 *
 * THE CALLER MUST HOLD THE NN SESSION (nn_session_try_acquire) ACROSS BOTH
 * CALLS, with any live stream stopped.  Between them the staging buffer is being
 * filled, and after them the model handle and every tensor pointer in it have
 * been replaced.
 */

/**
 * Ask the backend where a model should be staged.  On success @p *buf points at a
 * backend-owned buffer of @p *cap bytes that is NOT in use by the running model, so
 * filling it is safe even while inference is loaded.  Returns 0 or <0.
 */
int nn_model_load_region(void **buf, uint32_t *cap);

/**
 * Adopt the model now sitting in the staging buffer: @p len bytes at @p data,
 * called @p name for display.
 *
 * @p data == NULL is refused (NN_ERR_ARG) and changes nothing; an unload is
 * nn_model_release().  Before the singleton exists the answer is NN_ERR_STATE;
 * while nn_scratch_claim() is held, NN_ERR_SCRATCH_HELD; on a backend that
 * cannot swap, NN_ERR_NOSUP.
 *
 * [!] WHERE @p data MAY POINT (issue #108, f746g-disco since issue #159; per
 * backend since issue #169).  Wherever the compiled-in backend declares it can
 * adopt a model from, and nowhere else.  There are two kinds:
 *
 *   - A STAGING backend (tflm): any range that lies wholly inside a buffer
 *     load_region() handed out, starts on NN_MODEL_ALIGN -- not only its base --
 *     and is not the slot the live model is in.  A container's model section is
 *     adopted in place, at its offset inside the staged container; copying it
 *     down would overwrite the container whose bytes were validated with bytes
 *     nobody validated.  Anything else is NN_MODEL_ERR_SLOT / NN_MODEL_ERR_ALIGN.
 *   - An IN-PLACE backend (ethosu): any range that lies wholly inside the read
 *     window the board declares and starts on NN_MODEL_ALIGN.  The interpreter
 *     keeps pointers INTO those bytes for as long as the model is loaded, and the
 *     backend cannot make them stay put: THE CALLER'S LEASE on the window (and on
 *     the bytes in it) is what guarantees their lifetime, so the caller holds it
 *     from before this call until the model is gone -- including a restore of the
 *     previous model.  Anything else is NN_MODEL_ERR_WINDOW / NN_MODEL_ERR_ALIGN.
 *     Such a backend has no load_region(), so nn_model_load_region() answers
 *     NN_ERR_NOSUP there.
 *
 * Either way a refusal of the range changes nothing.
 *
 * @p model_after, when not NULL, receives whether a MODEL is active once THIS
 * call has finished (nn_model_present()), not whether the singleton is open,
 * which it stays.  It is the reload's own outcome, not a readback: `nn info` on
 * another console asks without the session, so any read after this returns may
 * see a state this call did not leave.
 *
 * TRANSACTIONAL.  A model the backend cannot build is reported without
 * disturbing the one already loaded, so a truncated file costs nothing but the
 * message; when there was none, or even it could not be rebuilt, the backend is
 * left with no model.  Returns 0 or the <0 reason for the rejection.
 */
int nn_model_reload(const void *data, uint32_t len, const char *name,
                    int *model_after);

/**
 * Unload.  The backend keeps its singleton open with NO model in it (issue
 * #131 P16).  Nothing comes back by itself afterwards -- not on the next open,
 * not from `nn info` or `nn run`; only a later `nn model load` brings a model.
 * Same serialization as nn_model_reload().  0 on success, NN_ERR_NOSUP when the
 * backend cannot swap models (null, stedgeai), or the backend's own <0 code.
 */
int nn_model_release(void);

/**
 * One short sentence for a code returned by nn_model_load_region() or
 * nn_model_reload(), never NULL.  Handles the NN_MODEL_ERR_* family and this
 * layer's own NN_ERR_* codes (NN_ERR_SCRATCH_HELD included); anything else is
 * "unknown error".  wio-lite-ai's adapter prints it; f746g-disco's words its
 * refusals itself.
 */
const char *nn_model_strerror(int rc);

/*
 * Coarse single-session guard.  The singleton model and the backends are NOT
 * reentrant, and several consoles can issue `nn` commands, so only ONE
 * inference activity may use the model at a time: a one-shot `nn bench` or
 * `nn run`, or a live `nn stream`.  try_acquire() claims the session (returns
 * 0) or reports it busy (<0); release() frees it.  Interrupt-guarded and
 * thread-agnostic -- the acquiring and releasing threads may differ (stream
 * start in one console, stop in another).  No init required.  Busy is -1;
 * NN_ERR_SCRATCH_HELD while nn_scratch_claim() is held.
 */
int  nn_session_try_acquire(void);
void nn_session_release(void);

/**
 * f746g-disco: the memory owner's side of `sdram test` (issue #152).
 * .sdram.ai (the arena, the model slots) belongs to this layer, so the test
 * asks HERE before it writes over it -- the same shape as
 * camera_scratch_claim().
 *
 * nn_scratch_claim() succeeds (0) only when no model is loaded and nothing is
 * using nn: it refuses with -1 while the session is held, the first open is in
 * flight or another claim is held, and with -2 while a model is loaded.  It
 * NEVER unloads -- the operator does that with `nn model unload`.  The check
 * and the set are one PRIMASK critical section.
 *
 * While it is held nothing can put anything into .sdram.ai: the session cannot
 * be taken (so `nn model load` / `unload`, `nn bench`, `nn run` and `nn stream`
 * are all refused), the first open builds nothing, and the load region and
 * reload refuse -- each with NN_ERR_SCRATCH_HELD.  An open that already
 * happened returns the (empty) singleton as before; it builds nothing.
 * nn_scratch_release() ends it.  Interrupt-guarded and thread-agnostic.
 */
int  nn_scratch_claim(void);
void nn_scratch_release(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_H */
