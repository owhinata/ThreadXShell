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

/** Generic name for a @ref NN_SVC_OK status code, used when the board's own
 *  nn_svc_strerror() has nothing more specific.  Always non-NULL. */
const char *nn_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* NN_CMD_CORE_H */
