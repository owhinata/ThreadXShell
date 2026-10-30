/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for `nn model load --path <container>` on this board's tflm build
 * (issue #159 = #78 Step 4a): port/nn/nn_svc_f746_model.c compiled WITH
 * CONFIG_NN_BACKEND_TFLM and this board's policy macros, over the real
 * svc/plugin/plugin_load.c, svc/crc32.c, port/nn/nn_container.c, port/nn/nn.c
 * and the shared load order, against a stub backend that refuses any model
 * pointer outside its staging slot or off NN_MODEL_ALIGN -- the contract the
 * tflm backend keeps.
 *
 * The containers are built here, byte by byte, the way
 * shell/test/test_plugin_load.c builds its fixture: a wio-shaped container
 * (target word, link address) with this board's facts, and the same one with
 * wio's target word -- the file an operator would copy by mistake.
 *
 * What it pins:
 *   - a bare model takes the old path: the backend gets the slot base and the
 *     whole file, and `nn info` says no container is open;
 *   - a container this policy accepts hands the backend its MODEL SECTION in
 *     place (slot base + model_off, model_len), commits its claims, and `nn
 *     info` prints them as "validated, not loaded" -- Step 4a starts no plugin;
 *   - [!] THE TWO CRCs ARE ONE VALUE: what the reader prints
 *     (nn_container_crc() over the bytes it returned, as cmds/cmd_nn_board.c
 *     does) is what `nn info` prints beside the claims, and both equal an
 *     independent bitwise CRC-32/ISO-HDLC of the file;
 *   - a container built for another target, a truncated one and one whose
 *     plugin section was damaged are refused before the backend is touched,
 *     each with its own reason, and leave the previous model and its claims;
 *   - a damaged container magic is not a container: it falls through as a bare
 *     model and the backend decides (here: refuses);
 *   - a bare load and an unload clear the claims; a refused reload that
 *     restored the previous model keeps the previous claims.
 */
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "nn.h"
#include "nn_backend.h"
#include "nn_camera.h"
#include "nn_container.h"
#include "nn_svc.h"
#include "plugin_abi.h"
#include "sdram.h"
#include "tx_api.h"
#include "stm32f7xx_hal.h"

DWT_Type       test_dwt = { .CTRL = DWT_CTRL_NOCYCCNT_Msk };
CoreDebug_Type test_coredebug;
int            test_tx_masked;

/* The linker's reservation markers, as nn_svc_info_extra() names them. */
uint8_t __plugin_start[16], __plugin_end[16];

UINT tx_thread_sleep(ULONG ticks)
{
	(void)ticks;
	assert(!"the open latch was contended in a single-threaded test");
	return 0;
}

static int invalidations;

bool nn_camera_running(void) { return false; }
void nn_camera_record_invalidate(void) { invalidations++; }
int sdram_is_up(void) { return 1; }

/* ---- the staging slot and the stub backend ------------------------------- */

#define SLOT_CAP 4096u
static alignas(16) uint8_t slot[SLOT_CAP];

static struct {
	int adopted, has, reloads, refuse_next;
	const void *data;
	uint32_t len;
	struct nn_tensor in;
} stub;

static int st_init(void) { return 0; }
static int st_open(void **impl)
{
	if (!stub.adopted) {
		stub.adopted = 1;
		stub.has     = 1;
	}
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
static int st_region(void **buf, uint32_t *cap)
{
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
	stub.has = 0;
	return 0;
}

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ "stub", "" },
	.init = st_init, .open = st_open, .close = st_close,
	.model_name = st_name, .in_count = st_in_count, .out_count = st_in_count,
	.input = st_tensor, .output = st_tensor, .activations_bytes = st_acts,
	.run = st_run, .load_region = st_region, .reload = st_reload,
	.release = st_release, .arena_reserved = 524288u, .has_builtin = 1u,
};

/* ---- the file on the card ------------------------------------------------ */

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

/* What cmds/cmd_nn_board.c does: the whole file into the slot, then -- for a
 * container -- nn_container_crc() over exactly the bytes it returns. */
static int reader_said_container;
static uint32_t reader_crc;

static int st_read(void *ctx, const char *path, void *buf, uint32_t cap,
                   uint32_t *len)
{
	(void)ctx; (void)path;
	assert(buf == slot && cap == SLOT_CAP && file_len <= cap);
	memcpy(buf, file, file_len);
	*len = file_len;
	reader_said_container = nn_container_crc(buf, *len, &reader_crc);
	return 0;
}

static struct nn_op_result res;
static enum nn_model_state state;

static void load(const char *path)
{
	struct nn_spec spec;

	memset(&spec, 0, sizeof spec);
	memset(&res, 0, sizeof res);
	spec.tag  = NN_SPEC_PATH;
	spec.path = path;
	nn_svc_model_load(&spec, st_read, NULL, &res, &state);
	assert(test_tx_masked == 0);
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

static const char *info_extra(void)
{
	out_len = 0u;
	out[0] = '\0';
	nn_svc_info_extra(sink, NULL);
	assert(test_tx_masked == 0);
	return out;
}

int main(void)
{
	char want[160];
	uint32_t crc, rl;
	struct nn_svc_info i;

	nn_svc_info(&i);   /* boot: the built-in model */
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	/* A bare model: the old path, slot base and the whole file. */
	build_bare();
	load("/m.tflite");
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW);
	assert(!reader_said_container);
	assert(stub.data == slot && stub.len == 300u);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	/* A container this board's policy accepts: the model section in place. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("/fixture.nnc");
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW);
	assert(stub.data == slot + MODEL_OFF && stub.len == MODEL_LEN);
	assert(reader_said_container);
	/* [!] ONE VALUE: the reader's, `nn info`'s, and an independent CRC. */
	crc = crc_ref(file, TOTAL);
	assert(reader_crc == crc);
	snprintf(want, sizeof want, "  from  : /fixture.nnc, %u B, file crc32 "
	         "%08X\r\n", (unsigned)TOTAL, (unsigned)crc);
	info_extra();
	printf("%s", out);
	assert(strstr(out, want) != NULL);
	snprintf(want, sizeof want, "plugin  : fixture (build abc1234, crc %08x), "
	         "validated, not loaded\r\n",
	         (unsigned)crc32_update(0u, file + PL_OFF, PL_LEN));
	assert(strstr(out, want) != NULL);
	assert(strstr(out, "  image : 256 B file / 320 B mem") != NULL);
	assert(strstr(out, "link 0x20048000") != NULL);

	/* Refusals before the backend: their own reasons; the previous model and
	 * its claims stay. */
	rl = (uint32_t)stub.reloads;
	build_container(0x1201u);   /* wio's word */
	load("/wio.nnc");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(strcmp(res.detail, "/wio.nnc is a container this firmware "
	                          "refuses: built for another target") == 0);
	assert(reader_said_container && reader_crc == crc_ref(file, TOTAL));

	build_container(F746_PLUGIN_TARGET_ID);
	file_len = TOTAL - 32u;     /* truncated on the card */
	load("/short.nnc");
	assert(res.status == NN_SVC_ERR_ARG);
	assert(strstr(res.detail, "size disagrees with payload") != NULL);

	build_container(F746_PLUGIN_TARGET_ID);
	file[PL_OFF + MAN_SZ + 10u] ^= 0xFFu;   /* a damaged plugin byte */
	load("/bad.nnc");
	assert(res.status == NN_SVC_ERR_ARG);
	assert(strstr(res.detail, "digest mismatch") != NULL);
	assert((uint32_t)stub.reloads == rl);
	info_extra();
	assert(strstr(out, "  from  : /fixture.nnc,") != NULL);

	/* A damaged magic is not a container: a bare model, which the backend
	 * then refuses -- the previous model and its claims stay. */
	build_container(F746_PLUGIN_TARGET_ID);
	file[0] ^= 0xFFu;
	stub.refuse_next = 1;
	load("/magic.nnc");
	assert(!reader_said_container);
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(stub.data == slot + MODEL_OFF);
	info_extra();
	assert(strstr(out, "  from  : /fixture.nnc,") != NULL);

	/* A container the backend refuses (it restores the previous one, which is
	 * this same fixture's model): the previous claims stay. */
	build_container(F746_PLUGIN_TARGET_ID);
	stub.refuse_next = 1;
	load("/again.nnc");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	info_extra();
	assert(strstr(out, "  from  : /fixture.nnc,") != NULL);

	/* A bare load clears the claims; so does an unload. */
	build_container(F746_PLUGIN_TARGET_ID);
	load("/fixture.nnc");
	assert(res.status == NN_SVC_OK);
	build_bare();
	load("/m.tflite");
	assert(res.status == NN_SVC_OK);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);
	build_container(F746_PLUGIN_TARGET_ID);
	load("/fixture.nnc");
	memset(&res, 0, sizeof res);
	nn_svc_model_unload(&res);
	assert(res.status == NN_SVC_OK);
	assert(strstr(info_extra(), "plugin  : (none) -- reservation") != NULL);

	printf("test_nn_model_container: OK\n");
	return 0;
}
