/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_load.c
 * @brief   Container and manifest validation.  See plugin_load.h.
 *
 * [!] THE BYTES ARE DECODED, NOT CAST.  A container is read where it lies -- on
 * Grove that is a memory-mapped flash window -- so it carries no alignment
 * guarantee, and the format is defined as little-endian rather than as "however
 * this compiler lays a struct out".  Every field is fetched a byte at a time
 * from an offsetof() into the wire structs, which keeps plugin_abi.h the single
 * declaration of the layout while making the reads safe on any alignment.
 *
 * [!] EVERY RANGE CHECK IS WRITTEN SO THAT THE CHECK ITSELF CANNOT WRAP.  The
 * form is `off <= total && len <= total - off`, never `off + len <= total`: the
 * addition is exactly what an attacker-shaped -- or, far more likely here,
 * simply corrupt -- pair of numbers would overflow, and then the comparison
 * that was supposed to be the guard passes.
 *
 * [!] NOTHING HERE PRODUCES A CALLABLE ADDRESS.  Slot values stay integers all
 * the way into struct plugin_view.  That is the whole of "Step 1a does not
 * execute a plugin": not a rule to be kept, but the absence of anything to call.
 */
#include "plugin_load.h"

#include <string.h>

#include "crc32.h"

/* ---- little-endian field reads ------------------------------------------- */

static uint32_t rd32(const uint8_t *base, size_t off)
{
	const uint8_t *p = base + off;

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * `off .. off+len` lies inside `total`, computed without an addition that could
 * wrap.  A zero length is in range anywhere inside the object.
 */
static int fits(uint32_t off, uint32_t len, uint32_t total)
{
	return off <= total && len <= total - off;
}

/* Both ranges have already passed fits(), so the sums below cannot wrap. */
static int overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	if (alen == 0u || blen == 0u)
		return 0;
	return a < b + blen && b < a + alen;
}

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

/* ---- the section table --------------------------------------------------- */

struct sect {
	uint32_t type, off, len;
};

static enum plugin_result read_sections(const uint8_t *p, uint32_t total,
                                        uint32_t count, struct sect *out,
                                        int *have_model, int *have_plugin,
                                        int *have_data)
{
	size_t base = offsetof(struct plugin_container_hdr, sections);
	uint32_t i, j;

	*have_model = *have_plugin = *have_data = -1;

	for (i = 0u; i < count; i++) {
		size_t at = base + (size_t)i * sizeof(struct plugin_section);
		int *slot;

		out[i].type = rd32(p, at + offsetof(struct plugin_section, type));
		out[i].off  = rd32(p, at + offsetof(struct plugin_section, offset));
		out[i].len  = rd32(p, at + offsetof(struct plugin_section, length));

		if (rd32(p, at + offsetof(struct plugin_section, reserved)) != 0u)
			return PLUGIN_ERR_RESERVED;

		switch (out[i].type) {
		case PLUGIN_SECTION_MODEL:  slot = have_model;  break;
		case PLUGIN_SECTION_PLUGIN: slot = have_plugin; break;
		case PLUGIN_SECTION_DATA:   slot = have_data;   break;
		default:
			/* Includes PLUGIN_SECTION_NONE: a zeroed entry is a refusal,
			 * not an empty slot to be skipped over. */
			return PLUGIN_ERR_SECTION_TYPE;
		}
		if (*slot >= 0)
			return PLUGIN_ERR_SECTION_TYPE;   /* a duplicate kind */
		*slot = (int)i;

		/* A section must be non-empty: a zero-length model or plugin is a
		 * malformed container, not a present-but-empty one. */
		if (out[i].len == 0u)
			return PLUGIN_ERR_SECTION_RANGE;
		if (!fits(out[i].off, out[i].len, total))
			return PLUGIN_ERR_SECTION_RANGE;
		/* Nothing may reach back into the header and its table. */
		if (out[i].off < base + (size_t)count * sizeof(struct plugin_section))
			return PLUGIN_ERR_SECTION_RANGE;
	}

	for (i = 0u; i < count; i++)
		for (j = i + 1u; j < count; j++)
			if (overlaps(out[i].off, out[i].len, out[j].off, out[j].len))
				return PLUGIN_ERR_SECTION_OVERLAP;

	return PLUGIN_OK;
}

/* ---- the manifest -------------------------------------------------------- */

/* Mandatory slots.  A plugin that cannot be initialised, cannot say whether it
 * understands a model, or cannot decode one is not a plugin. */
static int slot_mandatory(unsigned i)
{
	return i == PLUGIN_SLOT_ENTRY || i == PLUGIN_SLOT_SHAPES_OK ||
	       i == PLUGIN_SLOT_DECODE;
}

/* The capability bit that must accompany an optional slot, or 0 if the slot is
 * mandatory (and therefore governed by no bit). */
static uint32_t slot_cap(unsigned i)
{
	switch (i) {
	case PLUGIN_SLOT_DRAW:      return PLUGIN_CAP_DRAW;
	case PLUGIN_SLOT_REPORT:    return PLUGIN_CAP_REPORT;
	case PLUGIN_SLOT_PARAM_SET: return PLUGIN_CAP_PARAMS;
	case PLUGIN_SLOT_PARAM_GET: return PLUGIN_CAP_PARAMS;
	default:                    return 0u;
	}
}

static enum plugin_result check_manifest(const uint8_t *p, uint32_t sect_off,
                                         uint32_t sect_len,
                                         const struct plugin_policy *pol,
                                         struct plugin_view *out)
{
	const uint8_t *m = p + sect_off;
	uint32_t struct_size, target, cap, image_off, file_size, mem_size;
	uint32_t seg_off[4], seg_len[4];
	unsigned i, j;

	if (sect_len < sizeof(struct plugin_manifest))
		return PLUGIN_ERR_MANIFEST_SHORT;

	if (m[0] != PLUGIN_MANIFEST_MAGIC0 || m[1] != PLUGIN_MANIFEST_MAGIC1 ||
	    m[2] != PLUGIN_MANIFEST_MAGIC2 || m[3] != PLUGIN_MANIFEST_MAGIC3)
		return PLUGIN_ERR_MANIFEST_MAGIC;

	struct_size = rd32(m, offsetof(struct plugin_manifest, struct_size));
	if (struct_size != (uint32_t)sizeof(struct plugin_manifest))
		return PLUGIN_ERR_MANIFEST_SIZE;

	if (rd32(m, offsetof(struct plugin_manifest, abi_version)) !=
	    PLUGIN_ABI_VERSION)
		return PLUGIN_ERR_ABI;

	for (i = 0u; i < 4u; i++)
		if (rd32(m, offsetof(struct plugin_manifest, reserved) + i * 4u) != 0u)
			return PLUGIN_ERR_RESERVED;

	target = rd32(m, offsetof(struct plugin_manifest, target_id));
	if ((target & PLUGIN_TARGET_RESERVED_MASK) != 0u)
		return PLUGIN_ERR_RESERVED;
	if (target != pol->target_id)
		return PLUGIN_ERR_TARGET;

	out->link_addr = rd32(m, offsetof(struct plugin_manifest, link_addr));
	if (out->link_addr != pol->link_addr)
		return PLUGIN_ERR_LINK_ADDR;

	cap = rd32(m, offsetof(struct plugin_manifest, capability));
	if ((cap & ~PLUGIN_CAP_KNOWN_MASK) != 0u)
		return PLUGIN_ERR_CAPABILITY;
	if ((cap & ~pol->caps_supported) != 0u)
		return PLUGIN_ERR_CAPABILITY;

	image_off = rd32(m, offsetof(struct plugin_manifest, image_off));
	file_size = rd32(m, offsetof(struct plugin_manifest, file_size));
	mem_size  = rd32(m, offsetof(struct plugin_manifest, mem_size));

	/* The image follows the manifest inside the same section. */
	if (image_off < struct_size)
		return PLUGIN_ERR_IMAGE_RANGE;
	if (!fits(image_off, file_size, sect_len))
		return PLUGIN_ERR_IMAGE_RANGE;

	if (file_size > mem_size)
		return PLUGIN_ERR_SIZES;
	if (mem_size > pol->capacity)
		return PLUGIN_ERR_SIZES;

	/* Alignment is a property of where the image will LIE, and the far end
	 * matters as much as the near one: cache maintenance rounds outward, so a
	 * reservation whose end is not on a line boundary lets that rounding reach
	 * whatever follows it. */
	if (pol->image_align == 0u ||
	    (pol->link_addr & (pol->image_align - 1u)) != 0u ||
	    (mem_size & (pol->image_align - 1u)) != 0u)
		return PLUGIN_ERR_IMAGE_ALIGN;

	seg_off[0] = rd32(m, offsetof(struct plugin_manifest, code_off));
	seg_len[0] = rd32(m, offsetof(struct plugin_manifest, code_len));
	seg_off[1] = rd32(m, offsetof(struct plugin_manifest, data_off));
	seg_len[1] = rd32(m, offsetof(struct plugin_manifest, data_len));
	seg_off[2] = rd32(m, offsetof(struct plugin_manifest, bss_off));
	seg_len[2] = rd32(m, offsetof(struct plugin_manifest, bss_len));
	seg_off[3] = rd32(m, offsetof(struct plugin_manifest, scratch_off));
	seg_len[3] = rd32(m, offsetof(struct plugin_manifest, scratch_len));

	/* code and data have initialisers and so must lie in the FILE part; bss
	 * and scratch exist only in memory and are bounded by mem_size. */
	if (!fits(seg_off[0], seg_len[0], file_size) ||
	    !fits(seg_off[1], seg_len[1], file_size) ||
	    !fits(seg_off[2], seg_len[2], mem_size) ||
	    !fits(seg_off[3], seg_len[3], mem_size))
		return PLUGIN_ERR_SEG_RANGE;

	/* Code is not optional: there is nothing to execute without it. */
	if (seg_len[0] == 0u)
		return PLUGIN_ERR_SEG_RANGE;

	for (i = 0u; i < 4u; i++)
		for (j = i + 1u; j < 4u; j++)
			if (overlaps(seg_off[i], seg_len[i], seg_off[j], seg_len[j]))
				return PLUGIN_ERR_SEG_OVERLAP;

	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++) {
		uint32_t v = rd32(m, offsetof(struct plugin_manifest, slot) + i * 4u);
		uint32_t st = rd32(m, offsetof(struct plugin_manifest, stack) + i * 4u);
		uint32_t cbit = slot_cap(i);
		uint32_t at;

		if (v == PLUGIN_SLOT_ABSENT) {
			if (slot_mandatory(i))
				return PLUGIN_ERR_SLOT_MISSING;
			if (cbit != 0u && (cap & cbit) != 0u)
				return PLUGIN_ERR_CAPABILITY;
			/* An absent slot declares no stack: one canonical spelling of
			 * absence, so a stale number cannot survive here. */
			if (st != 0u)
				return PLUGIN_ERR_STACK;
			out->slot[i]  = PLUGIN_SLOT_ABSENT;
			out->stack[i] = 0u;
			continue;
		}

		if (cbit != 0u && (cap & cbit) == 0u)
			return PLUGIN_ERR_CAPABILITY;

		/* Thumb state is not an implementation detail on this part: a cleared
		 * bit 0 would be a branch to Arm state, which these cores do not have,
		 * and the fault it produces would point at the plugin rather than at
		 * the tool that built it. */
		if ((v & 1u) == 0u)
			return PLUGIN_ERR_SLOT_THUMB;

		at = v & ~1u;
		if (at < seg_off[0] || at - seg_off[0] >= seg_len[0])
			return PLUGIN_ERR_SLOT_RANGE;

		if (st == 0u || st > pol->stack_limit[i])
			return PLUGIN_ERR_STACK;

		out->slot[i]  = v;
		out->stack[i] = st;
	}

	/* PARAM_SET and PARAM_GET share one bit, so neither may stand alone. */
	if ((out->slot[PLUGIN_SLOT_PARAM_SET] == PLUGIN_SLOT_ABSENT) !=
	    (out->slot[PLUGIN_SLOT_PARAM_GET] == PLUGIN_SLOT_ABSENT))
		return PLUGIN_ERR_CAPABILITY;

	memcpy(out->name, m + offsetof(struct plugin_manifest, name),
	       PLUGIN_NAME_MAX);
	memcpy(out->build_id, m + offsetof(struct plugin_manifest, build_id),
	       PLUGIN_BUILD_ID_MAX);
	/* Bounded strings: a missing terminator is the caller's problem otherwise,
	 * and the fault reporter prints these. */
	out->name[PLUGIN_NAME_MAX - 1u] = '\0';
	out->build_id[PLUGIN_BUILD_ID_MAX - 1u] = '\0';

	out->capability   = cap;
	out->file_size    = file_size;
	out->mem_size     = mem_size;
	out->image_off    = sect_off + image_off;
	out->code_off     = out->image_off + seg_off[0];
	out->code_len     = seg_len[0];
	out->data_seg_off = out->image_off + seg_off[1];
	out->data_seg_len = seg_len[1];
	out->bss_off      = out->image_off + seg_off[2];
	out->bss_len      = seg_len[2];
	out->scratch_off  = out->image_off + seg_off[3];
	out->scratch_len  = seg_len[3];
	return PLUGIN_OK;
}

/* ---- the whole thing ----------------------------------------------------- */

static enum plugin_result parse_into(const uint8_t *p, size_t len,
                                     const struct plugin_policy *pol,
                                     struct plugin_view *out)
{
	struct sect s[PLUGIN_SECTION_MAX];
	int i_model, i_plugin, i_data;
	uint32_t count, total, digest;
	enum plugin_result rc;

	if (len < sizeof(struct plugin_container_hdr))
		return PLUGIN_ERR_SHORT;
	/* A container is addressed with 32-bit offsets throughout. */
	if (len > 0xFFFFFFFFu)
		return PLUGIN_ERR_TOTAL_SIZE;

	if (p[0] != PLUGIN_CONTAINER_MAGIC0 || p[1] != PLUGIN_CONTAINER_MAGIC1 ||
	    p[2] != PLUGIN_CONTAINER_MAGIC2 || p[3] != PLUGIN_CONTAINER_MAGIC3)
		return PLUGIN_ERR_MAGIC;
	if (p[4] != PLUGIN_CONTAINER_FORMAT0 || p[5] != PLUGIN_CONTAINER_FORMAT1 ||
	    p[6] != PLUGIN_CONTAINER_FORMAT2 || p[7] != PLUGIN_CONTAINER_FORMAT3)
		return PLUGIN_ERR_FORMAT;

	if (rd32(p, offsetof(struct plugin_container_hdr, hdr_size)) !=
	    (uint32_t)sizeof(struct plugin_container_hdr))
		return PLUGIN_ERR_HDR_SIZE;
	if (rd32(p, offsetof(struct plugin_container_hdr, abi_version)) !=
	    PLUGIN_ABI_VERSION)
		return PLUGIN_ERR_ABI;
	if (rd32(p, offsetof(struct plugin_container_hdr, reserved)) != 0u)
		return PLUGIN_ERR_RESERVED;

	/* The declared size must be the size that arrived.  Accepting a shorter
	 * declaration would validate a prefix and leave the rest unaccounted; a
	 * longer one would validate ranges that are not there. */
	total = rd32(p, offsetof(struct plugin_container_hdr, total_size));
	if (total != (uint32_t)len)
		return PLUGIN_ERR_TOTAL_SIZE;

	count = rd32(p, offsetof(struct plugin_container_hdr, section_count));
	if (count == 0u || count > PLUGIN_SECTION_MAX)
		return PLUGIN_ERR_SECTION_COUNT;

	rc = read_sections(p, total, count, s, &i_model, &i_plugin, &i_data);
	if (rc != PLUGIN_OK)
		return rc;

	if (i_model < 0)
		return PLUGIN_ERR_NO_MODEL;
	if ((s[i_model].off & (PLUGIN_MODEL_ALIGN - 1u)) != 0u)
		return PLUGIN_ERR_MODEL_ALIGN;

	out->model_off = s[i_model].off;
	out->model_len = s[i_model].len;
	if (i_data >= 0) {
		out->data_off = s[i_data].off;
		out->data_len = s[i_data].len;
	}

	digest = rd32(p, offsetof(struct plugin_container_hdr, plugin_digest));

	if (i_plugin < 0) {
		/* No plugin, so no digest: one spelling of absence. */
		if (digest != 0u)
			return PLUGIN_ERR_DIGEST;
		return PLUGIN_OK;
	}

	rc = check_manifest(p, s[i_plugin].off, s[i_plugin].len, pol, out);
	if (rc != PLUGIN_OK)
		return rc;

	/* Last, because it is the only check that reads the whole section, and
	 * every cheaper refusal above should come out first. */
	if (crc32_update(0u, p + s[i_plugin].off, s[i_plugin].len) != digest)
		return PLUGIN_ERR_DIGEST;

	out->digest     = digest;
	out->has_plugin = 1u;
	return PLUGIN_OK;
}

/*
 * [!] THE VIEW IS ZEROED AGAIN ON EVERY REFUSAL.  check_manifest() fills fields
 * as it validates them, so a failure part-way leaves a view that is partly
 * populated and wholly untrustworthy -- and the header promises the caller that
 * it is filled only on PLUGIN_OK.  Making the wrapper the single exit keeps that
 * promise true rather than relying on callers to check the code first, which is
 * exactly the discipline that fails at 3am.
 */
enum plugin_result plugin_parse(const void *buf, size_t len,
                                const struct plugin_policy *pol,
                                struct plugin_view *out)
{
	enum plugin_result rc;

	if (buf == NULL || pol == NULL || out == NULL)
		return PLUGIN_ERR_ARG;

	memset(out, 0, sizeof(*out));
	rc = parse_into((const uint8_t *)buf, len, pol, out);
	if (rc != PLUGIN_OK)
		memset(out, 0, sizeof(*out));
	return rc;
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
	case PLUGIN_ERR_DIGEST:          return "digest mismatch";
	case PLUGIN_ERR_ARG:             return "bad argument";
	}
	return "unknown";
}
