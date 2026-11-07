/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for `nn model load <container>` on this board's tflm build
 * (issue #159 = #78 Step 4a; from the QSPI asset store since issue #194):
 * port/nn/nn_svc_f746_model.c compiled WITH CONFIG_NN_BACKEND_TFLM and this
 * board's policy macros, over the real svc/plugin/plugin_load.c, svc/crc32.c,
 * svc/nn/nn.c and the shared load order, against a stub backend that refuses
 * any model pointer outside its staging buffer or off NN_MODEL_ALIGN -- the
 * contract the tflm backend keeps -- and a stand-in store (nn_blob_stub.c).
 *
 * The containers are built here, byte by byte, the way
 * shell/test/test_plugin_load.c builds its fixture: a wio-shaped container
 * (target word, link address) with this board's facts, and the same one with
 * wio's target word -- the blob an operator would send by mistake.
 *
 * What it pins:
 *   - a bare model takes the old path: the backend gets the buffer base and
 *     the whole payload, and `nn info` says no container is open;
 *   - a container this policy accepts hands the backend its MODEL SECTION in
 *     place (buffer base + model_off, model_len), commits its claims -- "from"
 *     names the slot and the blob -- and starts its plugin through the loader,
 *     under the plugin lease, with the buffer the payload was read into as the
 *     source region; `nn info` says "running" while the loader has it, and
 *     "validated, not loaded" when the loader refused it (the model stays
 *     open: issue #122 D6);
 *   - [!] ONE STAGING BUFFER (issue #191): the backend hands its buffer out only
 *     with no model open (the stub refuses otherwise, as nn_tflm.cc does), and
 *     the payload is read only after the shared order took the open model and
 *     its plugin down -- so a container built for another target, a truncated
 *     one, one whose plugin section was damaged and a copy whose CRC disagrees
 *     with its header, each refused with its own reason before the backend is
 *     touched, end with NO model and no claims (EMPTY), and so does a container
 *     the backend refuses;
 *   - a damaged container magic is not a container: it falls through as a bare
 *     model and the backend decides (here: refuses, EMPTY);
 *   - a lease that cannot be had still refuses before anything changes: the
 *     model, its plugin and its claims stay;
 *   - a bare load and an unload clear the claims.
 */
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "nn.h"
#include "nn_backend.h"
#include "nn_camera.h"
#include "nn_blob_stub.h"
#include "nn_svc.h"
#include "plugin_abi.h"
#include "plugin_lease_api.h"
#include "plugin_run_core.h"
#include "sdram.h"
#include "tx_api.h"

int            test_tx_masked;

/* The linker's reservation markers, as nn_svc_info_extra() names them. */
uint8_t __plugin_start[16], __plugin_end[16];

static int invalidations;

bool nn_camera_running(void) { return false; }
void nn_camera_record_invalidate(void) { invalidations++; }
int sdram_is_up(void) { return 1; }

/* ---- the loader and the lease (issue #159 Step 4b), stubbed ---------------
 * This test is about the container split and the claims; what the loader does
 * with the image is shell/test/test_plugin_run_lease.c's.  Here it records
 * what it was handed, and answers as told. */
static int      lease_free = 1, lease_held_now;
static unsigned lease_takes, lease_gives;
static unsigned pl_loads, pl_unloads;
static int      pl_active, pl_refuse_next;
static const void *pl_container, *pl_stage;
static uint32_t pl_cap;

int plugin_lease_take(void)
{
	if (!lease_free)
		return 0;
	lease_takes++;
	lease_held_now = 1;
	return 1;
}

void plugin_lease_give(void)
{
	assert(lease_held_now);
	lease_gives++;
	lease_held_now = 0;
}

enum plugin_run_result plugin_run_start(const struct plugin_view *v,
                                        const void *container,
                                        const void *stage, uint32_t cap)
{
	(void)v;
	assert(lease_held_now);
	pl_loads++;
	pl_container = container;
	pl_stage     = stage;
	pl_cap       = cap;
	if (pl_refuse_next) {
		pl_refuse_next = 0;
		pl_active = 0;
		return PLUGIN_RUN_MPU;
	}
	pl_active = 1;
	return PLUGIN_RUN_OK;
}

const char *plugin_run_why(enum plugin_run_result r)
{
	(void)r;
	return "refused by the test";
}

void plugin_run_unload(void)
{
	assert(lease_held_now);
	pl_unloads++;
	pl_active = 0;
}

int plugin_run_active(void)
{
	return pl_active;
}

/* ---- the staging slot and the stub backend ------------------------------- */

#define SLOT_CAP 4096u
static alignas(16) uint8_t slot[SLOT_CAP];

static struct {
	int adopted, has, reloads, refuse_next, releases;
	int region_refused;   /* load_region() asked with a model open */
	const void *data;
	uint32_t len;
	struct nn_tensor in;
} stub;

static int st_init(void) { return 0; }
static int st_open(void **impl)
{
	stub.adopted = 1;   /* nothing in the image (issue #159 Step 4c) */
	*impl = &stub;
	return 0;
}
static void st_close(void *impl) { (void)impl; }
static const char *st_name(void *impl) { (void)impl; return stub.has ? "m" : "(none)"; }
static int st_in_count(void *impl) { (void)impl; return stub.has ? 1 : 0; }
static struct nn_tensor *st_tensor(void *impl, int idx)
{
	(void)impl;
	return (stub.has && idx == 0) ? &stub.in : NULL;
}
static uint32_t st_acts(void *impl) { (void)impl; return stub.has ? 1234u : 0u; }
static int st_run(void *impl) { (void)impl; return 0; }
/* What the buffer will hold, asked with a model open (issue #191). */
static int st_cap(uint32_t *cap)
{
	*cap = SLOT_CAP;
	return 0;
}
/* The tflm backend's one buffer (issue #191): refused while a model is open,
 * nothing written. */
static int st_region(void **buf, uint32_t *cap)
{
	if (stub.has) {
		stub.region_refused++;
		return NN_MODEL_ERR_SLOT;
	}
	*buf = slot;
	*cap = SLOT_CAP;
	return 0;
}
/* The tflm backend's contract (nn_tflm.cc): inside the slot, 16-aligned. */
static int st_reload(const void *data, uint32_t len, const char *name,
                     void **impl)
{
	const uintptr_t p = (uintptr_t)data, b = (uintptr_t)slot;

	(void)name;
	*impl = &stub;
	stub.reloads++;
	assert(data != NULL);
	assert(p >= b && p - b < SLOT_CAP && len <= SLOT_CAP - (p - b));
	assert((p & (NN_MODEL_ALIGN - 1u)) == 0u);
	if (stub.refuse_next) {
		stub.refuse_next = 0;
		return -7;
	}
	stub.data = data;
	stub.len  = len;
	stub.has  = 1;
	return 0;
}
static int st_release(void)
{
	stub.releases++;
	stub.has = 0;
	return 0;
}

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ "stub", "" },
	.init = st_init, .open = st_open, .close = st_close,
	.model_name = st_name, .in_count = st_in_count, .out_count = st_in_count,
	.input = st_tensor, .output = st_tensor, .activations_bytes = st_acts,
	.run = st_run, .load_region = st_region, .reload = st_reload,
	.release = st_release, .arena_reserved = 524288u, .has_builtin = 0u,
	.stage_capacity = st_cap,
	.staging_is_open = 1u,   /* the tflm backend's one buffer (issue #200) */
};

/* ---- the blob -------------------------------------------------------------- */

#define HDR_SZ    ((uint32_t)sizeof(struct plugin_container_hdr))
#define MAN_SZ    ((uint32_t)sizeof(struct plugin_manifest))
#define CODE      256u
#define BSS       64u
#define ALIGN_UP(v, a) (((v) + (a) - 1u) & ~((a) - 1u))
#define PL_OFF    HDR_SZ
#define PL_LEN    (MAN_SZ + CODE)
#define MODEL_OFF ALIGN_UP(PL_OFF + PL_LEN, PLUGIN_MODEL_ALIGN)
#define MODEL_LEN 160u
#define TOTAL     (MODEL_OFF + MODEL_LEN)

static uint8_t file[SLOT_CAP];
static uint32_t file_len;

static void wr32(uint32_t at, uint32_t v)
{
	file[at + 0u] = (uint8_t)v;
	file[at + 1u] = (uint8_t)(v >> 8);
	file[at + 2u] = (uint8_t)(v >> 16);
	file[at + 3u] = (uint8_t)(v >> 24);
}

#define HF(f)    ((uint32_t)offsetof(struct plugin_container_hdr, f))
#define SF(i, f) (HF(sections) + (uint32_t)(i) * \
                  (uint32_t)sizeof(struct plugin_section) + \
                  (uint32_t)offsetof(struct plugin_section, f))
#define MF(f)    (PL_OFF + (uint32_t)offsetof(struct plugin_manifest, f))

/* A container with this board's facts (or @p target's), its digest stamped. */
static void build_container(uint32_t target)
{
	unsigned i;

	memset(file, 0, sizeof file);
	file[0] = PLUGIN_CONTAINER_MAGIC0; file[1] = PLUGIN_CONTAINER_MAGIC1;
	file[2] = PLUGIN_CONTAINER_MAGIC2; file[3] = PLUGIN_CONTAINER_MAGIC3;
	file[4] = PLUGIN_CONTAINER_FORMAT0; file[5] = PLUGIN_CONTAINER_FORMAT1;
	file[6] = PLUGIN_CONTAINER_FORMAT2; file[7] = PLUGIN_CONTAINER_FORMAT3;
	wr32(HF(hdr_size), HDR_SZ);
	wr32(HF(abi_version), PLUGIN_ABI_VERSION);
	wr32(HF(total_size), TOTAL);
	wr32(HF(section_count), 2u);
	wr32(SF(0, type), PLUGIN_SECTION_PLUGIN);
	wr32(SF(0, offset), PL_OFF);
	wr32(SF(0, length), PL_LEN);
	wr32(SF(1, type), PLUGIN_SECTION_MODEL);
	wr32(SF(1, offset), MODEL_OFF);
	wr32(SF(1, length), MODEL_LEN);

	file[MF(magic) + 0u] = PLUGIN_MANIFEST_MAGIC0;
	file[MF(magic) + 1u] = PLUGIN_MANIFEST_MAGIC1;
	file[MF(magic) + 2u] = PLUGIN_MANIFEST_MAGIC2;
	file[MF(magic) + 3u] = PLUGIN_MANIFEST_MAGIC3;
	wr32(MF(struct_size), MAN_SZ);
	wr32(MF(abi_version), PLUGIN_ABI_VERSION);
	wr32(MF(target_id), target);
	wr32(MF(link_addr), F746_PLUGIN_BASE);
	wr32(MF(capability), PLUGIN_CAP_DRAW);
	wr32(MF(image_off), MAN_SZ);
	wr32(MF(file_size), CODE);
	wr32(MF(mem_size), CODE + BSS);
	wr32(MF(code_off), 0u);
	wr32(MF(code_len), CODE);
	wr32(MF(data_off), CODE);
	wr32(MF(data_len), 0u);
	wr32(MF(bss_off), CODE);
	wr32(MF(bss_len), BSS);
	wr32(MF(scratch_off), CODE + BSS);
	wr32(MF(scratch_len), 0u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_ENTRY,     0x01u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_SHAPES_OK, 0x21u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_DECODE,    0x41u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_DRAW,      0x61u);
	for (i = PLUGIN_SLOT_REPORT; i < PLUGIN_SLOT_COUNT; i++)
		wr32(MF(slot) + 4u * i, PLUGIN_SLOT_ABSENT);
	for (i = 0u; i <= PLUGIN_SLOT_DRAW; i++)
		wr32(MF(stack_own) + 4u * i, 256u);
	wr32(MF(stack_accounting), PLUGIN_STACK_ACCOUNTING);
	memcpy(file + MF(name), "fixture", 7);
	memcpy(file + MF(build_id), "abc1234", 7);
	for (i = 0u; i < CODE; i++)
		file[PL_OFF + MAN_SZ + i] = (uint8_t)(i * 7u + 1u);
	/* The model section: a .tflite's identifier, then filler. */
	memcpy(file + MODEL_OFF + 4u, "TFL3", 4);
	for (i = 8u; i < MODEL_LEN; i++)
		file[MODEL_OFF + i] = (uint8_t)(i ^ 0x5Au);
	wr32(HF(plugin_digest), crc32_update(0u, file + PL_OFF, PL_LEN));
	file_len = TOTAL;
}

static void build_bare(void)
{
	unsigned i;

	memset(file, 0, sizeof file);
	memcpy(file + 4, "TFL3", 4);
	for (i = 8u; i < 300u; i++)
		file[i] = (uint8_t)i;
	file_len = 300u;
}

/* CRC-32/ISO-HDLC bit by bit, written apart from svc/crc32.c so the equality
 * below is not one implementation agreeing with itself. */
static uint32_t crc_ref(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	uint32_t i;
	int b;

	for (i = 0u; i < n; i++) {
		c ^= p[i];
		for (b = 0; b < 8; b++)
			c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
	}
	return ~c;
}

/* ---- the console's side --------------------------------------------------- */

/* [!] The payload is read only once the open model and its plugin are down
 * (issue #191), under the lease. */
static void on_read(void)
{
	assert(!stub.has && !pl_active && lease_held_now);
}

static struct nn_op_result res;
static enum nn_model_state state;

/* Store the blob built in file[] under @p name in slot 0 (CRC of exactly those
 * bytes, as `blob write` stamps it), then load it by name. */
static void load(const char *name)
{
	struct nn_spec spec;

	nn_blob_stub_reset();
	nn_blob_stub.on_read = on_read;
	nn_blob_stub_put(0u, name, file, file_len);
	memset(&spec, 0, sizeof spec);
	memset(&res, 0, sizeof res);
	strncpy(spec.name, name, NN_SPEC_NAME_MAX);
	nn_svc_model_load(&spec, &res, &state);
	assert(test_tx_masked == 0);
	assert(!nn_blob_stub.held && nn_blob_stub.acquires == nn_blob_stub.releases);
}

static char out[2048];
static size_t out_len;

static int sink(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	assert(out_len + len < sizeof out);
	memcpy(out + out_len, s, len);
	out_len += len;
	out[out_len] = '\0';
	return (int)len;
}

/* What `nn info` does: the copy, then the board's lines checked against it. */
static const char *info_extra(void)
{
	struct nn_svc_info i;

	out_len = 0u;
	out[0] = '\0';
	nn_svc_info(&i);
	nn_svc_info_extra(&i, sink, NULL);
	assert(test_tx_masked == 0);
	return out;
}

int main(void)
{
	char want[160];
	uint32_t crc, rl;
	struct nn_svc_info i;

	nn_svc_info(&i);   /* boot: no model (issue #159 Step 4c) */
	assert(i.model_active == 0u);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	/* A bare model: the old path, buffer base and the whole payload. */
	build_bare();
	load("m");
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW);
	assert(stub.data == slot && stub.len == 300u);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	/* A container this board's policy accepts: the model section in place. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("fixture");
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW);
	assert(stub.data == slot + MODEL_OFF && stub.len == MODEL_LEN);
	/* The whole payload read and checked against the header's CRC, which is
	 * the CRC of the bytes built (an independent implementation). */
	assert(nn_blob_stub.last_read_len == TOTAL);
	crc = crc_ref(file, TOTAL);
	assert(nn_blob_stub.info[0].crc32 == crc);
	snprintf(want, sizeof want, "  from  : slot 0, blob 'fixture'\r\n");
	info_extra();
	printf("%s", out);
	assert(strstr(out, want) != NULL);
	snprintf(want, sizeof want, "plugin  : fixture (build abc1234, crc %08x), "
	         "running\r\n",
	         (unsigned)crc32_update(0u, file + PL_OFF, PL_LEN));
	assert(strstr(out, want) != NULL);
	/* [!] Started once, under the lease, from the slot the reader filled. */
	assert(pl_loads == 1u && pl_container == slot && pl_stage == slot &&
	       pl_cap == SLOT_CAP);
	assert(lease_takes == lease_gives && !lease_held_now);
	assert(strstr(out, "  image : 256 B file / 320 B mem") != NULL);
	assert(strstr(out, "link 0x20048000") != NULL);

	/* [!] Refusals before the backend, each with its own reason (issue #191):
	 * the file is read over the open model's buffer, so the model, its plugin
	 * and its claims are already down -- EMPTY, and the release runs again. */
	rl = (uint32_t)stub.reloads;
	build_container(0x1201u);   /* wio's word */
	load("wio");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(strcmp(res.detail, "slot 0 is a container this firmware "
	                          "refuses: built for another target") == 0);
	assert(!stub.has && !pl_active);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	build_container(F746_PLUGIN_TARGET_ID);
	file_len = TOTAL - 32u;     /* truncated before it was sent */
	load("short");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(strstr(res.detail, "size disagrees with payload") != NULL);

	build_container(F746_PLUGIN_TARGET_ID);
	file[PL_OFF + MAN_SZ + 10u] ^= 0xFFu;   /* a damaged plugin byte */
	load("bad");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(strstr(res.detail, "digest mismatch") != NULL);
	assert((uint32_t)stub.reloads == rl);

	/* [!] A copy that disagrees with the header's CRC (the QSPI read, the bus
	 * or the SDRAM), over an open model: refused before the split, EMPTY. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("fixture");
	assert(res.status == NN_SVC_OK && stub.has);
	{
		struct nn_spec spec;

		nn_blob_stub.info[0].crc32 ^= 1u;
		memset(&spec, 0, sizeof spec);
		memset(&res, 0, sizeof res);
		strcpy(spec.name, "fixture");
		nn_svc_model_load(&spec, &res, &state);
		assert(res.status == NN_SVC_ERR_HW && state == NN_MODEL_EMPTY);
		assert(strstr(res.detail, "CRC32 mismatch") != NULL);
		assert(!stub.has && !pl_active);
		assert(nn_blob_stub.acquires == nn_blob_stub.releases);
	}
	assert((uint32_t)stub.reloads == rl + 1u);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	/* A damaged magic is not a container: a bare model, which the backend
	 * then refuses -- over an open model, EMPTY, and the claims go. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("fixture");
	assert(res.status == NN_SVC_OK && stub.has && pl_active);
	rl = (uint32_t)stub.releases;
	build_container(F746_PLUGIN_TARGET_ID);
	file[0] ^= 0xFFu;
	stub.refuse_next = 1;
	load("magic");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(stub.data == slot + MODEL_OFF);
	/* the take-down's release, then the EMPTY ending's */
	assert((uint32_t)stub.releases == rl + 2u && !stub.has);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	/* A container the backend refuses, over an open model: EMPTY, no claims. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("fixture");
	assert(res.status == NN_SVC_OK && stub.has);
	build_container(F746_PLUGIN_TARGET_ID);
	stub.refuse_next = 1;
	load("again");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(!stub.has && !pl_active);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	/* [!] The loader refuses the plugin: the model IS open (no rollback), the
	 * load says it did not give what was asked, and `nn info` says the plugin
	 * was validated and is not running. */
	build_container(F746_PLUGIN_TARGET_ID);
	pl_refuse_next = 1;
	load("refused");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_NEW);
	assert(strstr(res.detail, "slot 0: refused by the test") != NULL);
	info_extra();
	assert(strstr(out, "validated, not loaded\r\n") != NULL);
	assert(strstr(out, "  from  : slot 0, blob 'refused'") != NULL);

	/* A lease that cannot be had: BUSY before anything changes. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("fixture");
	assert(res.status == NN_SVC_OK && pl_active);
	rl = (uint32_t)stub.reloads;
	lease_free = 0;
	build_container(F746_PLUGIN_TARGET_ID);
	load("busy");
	lease_free = 1;
	assert(res.status == NN_SVC_ERR_BUSY);
	assert(strstr(res.detail, "busy") != NULL);
	assert((uint32_t)stub.reloads == rl && pl_active && stub.has);
	info_extra();
	assert(strstr(out, "  from  : slot 0, blob 'fixture'") != NULL);

	/* A bare load clears the claims and unloads the plugin; so does an
	 * unload. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("fixture");
	assert(res.status == NN_SVC_OK);
	build_bare();
	load("m");
	assert(res.status == NN_SVC_OK);
	assert(!pl_active);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);
	build_container(F746_PLUGIN_TARGET_ID);
	load("fixture");
	memset(&res, 0, sizeof res);
	nn_svc_model_unload(&res);
	assert(res.status == NN_SVC_OK);
	assert(!pl_active);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);
	assert(lease_takes == lease_gives && !lease_held_now);
	/* [!] The buffer was never asked for with a model open. */
	assert(stub.region_refused == 0);

	printf("test_nn_model_container: OK\n");
	return 0;
}
