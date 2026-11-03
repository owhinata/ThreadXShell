/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_load.c
 * @brief   What the firmware and the host share besides the parse: probing a
 *          payload and naming a result.  See plugin_load.h.
 *
 * The parse itself is svc/plugin/plugin_load_body.h, entered through
 * plugin_load_board.c (firmware) or plugin_load_host.c (host), issue #165.
 * Nothing here takes or reads a policy, so both links take this file as is.
 */
#include "plugin_load.h"

/* ---- probing ------------------------------------------------------------- */

enum plugin_kind plugin_probe(const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;

	if (buf == NULL || len < 8u)
		return PLUGIN_KIND_UNKNOWN;

	/* A bare model is recognised by its identifier, which sits where our
	 * format word does.  Testing it FIRST means a legacy payload can never be
	 * mistaken for a container even if some future magic were chosen badly. */
	if (p[4] == PLUGIN_TFLITE_IDENT0 && p[5] == PLUGIN_TFLITE_IDENT1 &&
	    p[6] == PLUGIN_TFLITE_IDENT2 && p[7] == PLUGIN_TFLITE_IDENT3)
		return PLUGIN_KIND_TFLITE;

	if (p[0] == PLUGIN_CONTAINER_MAGIC0 && p[1] == PLUGIN_CONTAINER_MAGIC1 &&
	    p[2] == PLUGIN_CONTAINER_MAGIC2 && p[3] == PLUGIN_CONTAINER_MAGIC3 &&
	    p[4] == PLUGIN_CONTAINER_FORMAT0 && p[5] == PLUGIN_CONTAINER_FORMAT1 &&
	    p[6] == PLUGIN_CONTAINER_FORMAT2 && p[7] == PLUGIN_CONTAINER_FORMAT3)
		return PLUGIN_KIND_CONTAINER;

	return PLUGIN_KIND_UNKNOWN;
}

const char *plugin_result_name(enum plugin_result r)
{
	switch (r) {
	case PLUGIN_OK:                  return "ok";
	case PLUGIN_ERR_SHORT:           return "too short";
	case PLUGIN_ERR_MAGIC:           return "bad magic";
	case PLUGIN_ERR_FORMAT:          return "bad format word";
	case PLUGIN_ERR_HDR_SIZE:        return "bad header size";
	case PLUGIN_ERR_ABI:             return "abi mismatch";
	case PLUGIN_ERR_TOTAL_SIZE:      return "size disagrees with payload";
	case PLUGIN_ERR_RESERVED:        return "reserved field set";
	case PLUGIN_ERR_SECTION_COUNT:   return "bad section count";
	case PLUGIN_ERR_SECTION_TYPE:    return "bad or duplicate section";
	case PLUGIN_ERR_SECTION_RANGE:   return "section out of range";
	case PLUGIN_ERR_SECTION_OVERLAP: return "sections overlap";
	case PLUGIN_ERR_NO_MODEL:        return "no model section";
	case PLUGIN_ERR_MODEL_ALIGN:     return "model misaligned";
	case PLUGIN_ERR_MANIFEST_SHORT:  return "manifest truncated";
	case PLUGIN_ERR_MANIFEST_MAGIC:  return "bad manifest magic";
	case PLUGIN_ERR_MANIFEST_SIZE:   return "bad manifest size";
	case PLUGIN_ERR_TARGET:          return "built for another target";
	case PLUGIN_ERR_LINK_ADDR:       return "prelinked elsewhere";
	case PLUGIN_ERR_CAPABILITY:      return "capability disagreement";
	case PLUGIN_ERR_IMAGE_RANGE:     return "image out of range";
	case PLUGIN_ERR_IMAGE_ALIGN:     return "image misaligned";
	case PLUGIN_ERR_SIZES:           return "bad image sizes";
	case PLUGIN_ERR_SEG_RANGE:       return "segment out of range";
	case PLUGIN_ERR_SEG_OVERLAP:     return "segments overlap";
	case PLUGIN_ERR_SLOT_MISSING:    return "mandatory slot absent";
	case PLUGIN_ERR_SLOT_RANGE:      return "slot outside code";
	case PLUGIN_ERR_SLOT_THUMB:      return "slot not thumb";
	case PLUGIN_ERR_STACK:           return "stack request refused";
	case PLUGIN_ERR_STACK_ACCOUNTING: return "stack declared under another analysis";
	case PLUGIN_ERR_DIGEST:          return "digest mismatch";
	case PLUGIN_ERR_ARG:             return "bad argument";
	}
	return "unknown";
}
