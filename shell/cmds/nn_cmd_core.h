/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_cmd_core.h
 * @brief   The parts of the shared `nn` command that are pure logic (issue #50).
 *
 * Split out of cmd_nn.c for the same reason fs_cmd_core.c is split out of the
 * filesystem commands: what is left here takes values and returns values, so a
 * host test can drive it without a shell instance, a board, or a model.  The
 * argument grammar and the number formatting are exactly the parts that are easy
 * to get quietly wrong and impossible to check by looking at a console.
 *
 * Everything here is reentrant and owns no storage.
 */
#ifndef NN_CMD_CORE_H
#define NN_CMD_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "nn_svc.h"
#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decompose @p v into sign, integer part and six fraction digits.
 *
 * svc/fmt.c implements no `%f` and no precision flag, so every fractional value
 * in this firmware is printed as scaled integers.  Both boards that had an `ai`
 * command grew their own copy of this, one for a quantisation scale and one for
 * a dequantised output, and the scale copy carried a fixed bug for a while: a
 * parts-per-million form that printed 1.5 as "0.1500000", silently wrong for
 * every scale >= 1, which output tensors routinely have.  One function now, used
 * for both, so there is one thing to be right.
 *
 * [!] THE CLAMP IS BEFORE THE CAST, not after.  float -> uint32_t is undefined
 * for NaN and for anything outside the destination's range, and this is reached
 * for precisely when a model is already suspect -- it must not be the thing that
 * then misbehaves.
 *
 * @param sign  set to "-" or ""
 * @return 0, or -1 if @p v is NaN -- which a caller prints as "nan" rather than
 *         as some number, because a NaN in a tensor is a diagnosis and printing
 *         "0.000000" for it hides the one thing worth seeing
 */
int nn_f32_parts(float v, const char **sign, uint32_t *ip, uint32_t *frac);

/** Name of a @ref tensor_dtype, always non-NULL ("?" for one this build does
 *  not know, which is what an unsupported tag must read as). */
const char *nn_dtype_name(uint8_t dtype);

/** Bytes per element of @p dtype, or 0 when that is not known -- a caller must
 *  treat 0 as "cannot walk this buffer" rather than as an empty tensor. */
uint32_t nn_dtype_size(uint8_t dtype);

/**
 * Write @p t's shape as "1x128x128x3" into @p buf.
 *
 * A rank of 0 is written as "?" : the boards refuse to truncate a higher-rank
 * tensor into four dimensions, so a zero rank means "not representable" and must
 * not print as a plausible-looking empty shape.
 *
 * @return @p buf, always NUL-terminated
 */
char *nn_shape_str(const struct tensor_desc *t, char *buf, size_t cap);

/**
 * Parse the argument list of `nn model load` into a tagged @ref nn_spec.
 *
 * @p argv is the list AFTER the `load` word, @p argc its length.  The grammar is
 * closed and decidable without knowing the board:
 *
 *   --name <name> | --slot <index> | --path <path> | builtin | --addr <a> <len>
 *
 * [!] A BARE STRING IS NOT ACCEPTED, and that is the point.  The same word means
 * a blob name on one board, a filesystem path on another, and nothing at all on
 * a third -- so a parser that took one would have to guess per board, which is
 * board knowledge in shell/ by a quieter route.  Making the caller say which
 * namespace they mean is what lets a board refuse one it does not have instead
 * of half-implementing it.
 *
 * [!] THE RAW FORM'S LENGTH IS MANDATORY.  It is the bound handed to the
 * FlatBuffer verifier before anything walks the payload; "the rest of the
 * window" is not a bound, and this repo forbids it.  A zero length is refused
 * here rather than left for a board to discover.
 *
 * @return NN_SVC_OK, NN_SVC_ERR_ARG (missing or malformed operand) or
 *         NN_SVC_ERR_SPEC (not a tag this grammar has at all)
 */
int nn_spec_parse(int argc, char *const argv[], struct nn_spec *out);

/** One-line human name for a @ref nn_claim, for the line a command prints when
 *  a claim was not returned.  Always non-NULL. */
const char *nn_claim_name(uint8_t claim);

/** One-line human name for a @ref nn_model_state.  Always non-NULL. */
const char *nn_model_state_name(uint8_t state);

/** Generic name for a status code, used when the board wrote no `detail`
 *  alongside it.  Always non-NULL. */
const char *nn_status_name(int status);

/**
 * Which sentence `nn stream stats` puts on its `last` line.
 *
 * [!] HERE BECAUSE NOTHING GATES WHAT A COMMAND PRINTS (issue #105).  This
 * project has learned twice that a refactor can leave the machinery correct and
 * the words wrong, and the words are what an operator acts on -- the four cases
 * below send a reader to four different places.  Making the CHOICE a pure
 * function is what lets a host test pin all four; the printing stays in
 * cmd_nn.c, where it belongs.
 *
 * The distinctions are the ones issues #97 and #99 established and must not be
 * folded back together:
 *
 *   - "never decoded" is not "decoded nothing".  A stream that has not finished
 *     its first frame reporting "0 results" reads as a working decoder finding
 *     nothing;
 *   - a stream that HAS inferred and then stopped has retired its result on
 *     purpose, so that a stopped stream does not leave a stale annotation on
 *     view.  Saying "nothing decoded yet" after hundreds of frames reads as a
 *     broken decoder;
 *   - a negative count is not a count.  It is the decoder saying the open model
 *     is not one it recognises, which calls for loading a different model rather
 *     than for looking at the picture.  Printing it as "-1 result(s)" hands the
 *     reader arithmetic to do on a sentinel.
 */
enum nn_last_kind {
	NN_LAST_NEVER = 0,      /**< no decode has completed yet               */
	NN_LAST_RETIRED,        /**< there were decodes; the stream stopped    */
	NN_LAST_UNRECOGNISED,   /**< the decoder does not know this model      */
	NN_LAST_COUNT,          /**< a real, non-negative item count           */
};

/** Choose, from exactly the three fields the shared struct carries. */
enum nn_last_kind nn_last_kind_of(uint8_t last_valid, uint32_t infers,
                                  int32_t last_ndet);

/**
 * The parenthetical for a kind that has no number, or NULL for
 * @ref NN_LAST_COUNT, which prints its count instead.
 *
 * [!] THE NOUN IS NEUTRAL.  A decoder's result is private to it -- that is the
 * whole of issue #78 -- so this counts ITEMS.  A face detector returns boxes and
 * a classifier plugin returns how many entries of its class vector it kept, and
 * the shared command cannot tell which it has.  `nn dets` reaches the decoder's
 * own report, which can.
 */
const char *nn_last_text(enum nn_last_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* NN_CMD_CORE_H */
