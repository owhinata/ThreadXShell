/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_cmd_core.c
 * @brief   Pure logic behind the shared `nn` command.  See nn_cmd_core.h.
 */
#include "nn_cmd_core.h"

#include <string.h>

#include "cli.h"   /* cli_parse_u32 -- this firmware's own number parser */

/* The scaled-integer form every fractional value in this firmware is printed
   with.  Six fraction digits, so the divisor is a million. */
#define NN_FRAC_DIGITS 1000000.0f
#define NN_FRAC_UNITS  1000000u

int nn_f32_parts(float v, const char **sign, uint32_t *ip, uint32_t *frac)
{
	if (sign == NULL || ip == NULL || frac == NULL)
		return -1;

	*sign = "";
	*ip = 0u;
	*frac = 0u;

	if (v != v)          /* the only value that is not equal to itself */
		return -1;
	if (v < 0.0f) {
		*sign = "-";
		v = -v;
	}
	/* Clamp BEFORE the cast -- see the header. */
	if (v >= NN_FRAC_DIGITS) {
		*ip = NN_FRAC_UNITS - 1u;
		*frac = NN_FRAC_UNITS - 1u;
		return 0;
	}
	*ip = (uint32_t)v;
	*frac = (uint32_t)((v - (float)*ip) * NN_FRAC_DIGITS + 0.5f);
	if (*frac >= NN_FRAC_UNITS) {   /* the rounding carried */
		(*ip)++;
		*frac -= NN_FRAC_UNITS;
	}
	return 0;
}

const char *nn_dtype_name(uint8_t dtype)
{
	switch (dtype) {
	case TENSOR_DTYPE_INT8:    return "int8";
	case TENSOR_DTYPE_UINT8:   return "uint8";
	case TENSOR_DTYPE_INT16:   return "int16";
	case TENSOR_DTYPE_INT32:   return "int32";
	case TENSOR_DTYPE_FLOAT32: return "f32";
	default:                   return "?";
	}
}

uint32_t nn_dtype_size(uint8_t dtype)
{
	switch (dtype) {
	case TENSOR_DTYPE_INT8:
	case TENSOR_DTYPE_UINT8:   return 1u;
	case TENSOR_DTYPE_INT16:   return 2u;
	case TENSOR_DTYPE_INT32:
	case TENSOR_DTYPE_FLOAT32: return 4u;
	default:                   return 0u;
	}
}

char *nn_shape_str(const struct tensor_desc *t, char *buf, size_t cap)
{
	size_t pos = 0;
	unsigned i;

	if (buf == NULL || cap == 0u)
		return buf;
	buf[0] = '\0';
	if (t == NULL || t->rank == 0u) {
		/* Not representable -- never a plausible-looking empty shape. */
		if (cap >= 2u) {
			buf[0] = '?';
			buf[1] = '\0';
		}
		return buf;
	}

	for (i = 0; i < t->rank && i < TENSOR_MAX_DIMS; i++) {
		char num[12];
		size_t len;
		int32_t d = t->dims[i];
		uint32_t mag = (d < 0) ? (uint32_t)(-(int64_t)d) : (uint32_t)d;
		size_t j = 0;

		/* Decimal, most significant first, without pulling in a formatter. */
		do {
			num[j++] = (char)('0' + (mag % 10u));
			mag /= 10u;
		} while (mag != 0u && j < sizeof(num) - 1u);

		len = j + ((i != 0u) ? 1u : 0u) + ((d < 0) ? 1u : 0u);
		if (pos + len + 1u > cap)
			break;
		if (i != 0u)
			buf[pos++] = 'x';
		if (d < 0)
			buf[pos++] = '-';
		while (j-- > 0u)
			buf[pos++] = num[j];
	}
	buf[pos] = '\0';
	return buf;
}

/* Copy a name into the spec's own storage, refusing one too long to hold rather
   than truncating it: a truncated name can still match a different entry. */
static int nn_spec_set_name(struct nn_spec *out, const char *name)
{
	size_t len;

	if (name == NULL)
		return NN_SVC_ERR_ARG;
	len = strlen(name);
	if (len == 0u || len > NN_SPEC_NAME_MAX)
		return NN_SVC_ERR_ARG;
	memcpy(out->name, name, len);
	out->name[len] = '\0';
	return NN_SVC_OK;
}

int nn_spec_parse(int argc, char *const argv[], struct nn_spec *out)
{
	if (out == NULL || (argc > 0 && argv == NULL))
		return NN_SVC_ERR_ARG;

	memset(out, 0, sizeof(*out));
	out->tag = (uint8_t)NN_SPEC_NONE;

	if (argc < 1)
		return NN_SVC_ERR_ARG;

	if (strcmp(argv[0], "builtin") == 0) {
		if (argc != 1)
			return NN_SVC_ERR_ARG;
		out->tag = (uint8_t)NN_SPEC_BUILTIN;
		return NN_SVC_OK;
	}

	if (strcmp(argv[0], "--name") == 0) {
		if (argc != 2)
			return NN_SVC_ERR_ARG;
		out->tag = (uint8_t)NN_SPEC_NAME;
		return nn_spec_set_name(out, argv[1]);
	}

	if (strcmp(argv[0], "--slot") == 0) {
		if (argc != 2 || cli_parse_u32(argv[1], &out->slot) != 0)
			return NN_SVC_ERR_ARG;
		out->tag = (uint8_t)NN_SPEC_SLOT;
		return NN_SVC_OK;
	}

	if (strcmp(argv[0], "--path") == 0) {
		if (argc != 2 || argv[1][0] == '\0')
			return NN_SVC_ERR_ARG;
		out->tag = (uint8_t)NN_SPEC_PATH;
		out->path = argv[1];    /* argv outlives the command; not copied */
		return NN_SVC_OK;
	}

	if (strcmp(argv[0], "--addr") == 0) {
		/* [!] Both operands, always.  The length is the verifier's bound. */
		if (argc != 3 ||
		    cli_parse_u32(argv[1], &out->addr) != 0 ||
		    cli_parse_u32(argv[2], &out->len) != 0)
			return NN_SVC_ERR_ARG;
		if (out->len == 0u)
			return NN_SVC_ERR_ARG;
		out->tag = (uint8_t)NN_SPEC_ADDR;
		return NN_SVC_OK;
	}

	/* Not a tag this grammar has -- distinct from a malformed operand, because
	   the two send a reader to different places. */
	return NN_SVC_ERR_SPEC;
}

const char *nn_claim_name(uint8_t claim)
{
	switch (claim) {
	case NN_CLAIM_NONE:      return "none";
	case NN_CLAIM_CALLER:    return "caller";
	case NN_CLAIM_RETRYABLE: return "retryable";
	case NN_CLAIM_TERMINAL:  return "terminal";
	default:                 return "?";
	}
}

const char *nn_model_state_name(uint8_t state)
{
	switch (state) {
	case NN_MODEL_EMPTY:    return "empty";
	case NN_MODEL_NEW:      return "new";
	case NN_MODEL_PREVIOUS: return "previous";
	default:                return "?";
	}
}

const char *nn_status_name(int status)
{
	switch (status) {
	case NN_SVC_OK:            return "ok";
	case NN_SVC_ERR_ARG:       return "bad argument";
	case NN_SVC_ERR_NOSUP:     return "not supported on this board";
	case NN_SVC_ERR_SPEC:      return "not a model source this board takes";
	case NN_SVC_ERR_STATE:     return "wrong state (is a model loaded?)";
	case NN_SVC_ERR_BUSY:      return "busy (another nn command is running)";
	case NN_SVC_ERR_CANCEL:    return "cancelled";
	case NN_SVC_ERR_HW:        return "the hardware refused or did not settle";
	case NN_SVC_ERR_STALE:     return "the reading moved while it was taken";
	case NN_SVC_ERR_GEN:       return "that stream has been replaced by another";
	default:                   return "failed";
	}
}
