/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nn.c
 * @brief   `nn` command: one-shot Ethos-U55 classification (issues #44, #40).
 *
 *   nn info               what is loaded, and the arena budget
 *   nn open <name>        bring the NPU up and parse the blob of that name
 *   nn open --addr <a> <n>  ... or a raw address and length
 *   nn close              release it
 *   nn run                capture one frame, preprocess, infer, print top-5
 *   nn detect             the same, decoded as BlazeFace face boxes (#45)
 *   nn thresh [<milli>]   the detector's score threshold
 *
 * SINGLE OWNER.  The interpreter, the driver state, the camera buffer and the
 * arena are all static, and the shell runs foreground and background jobs
 * concurrently (`nn run &` while another command types).  So this carries an
 * ownership gate of exactly the shape CoreMark's does -- a PRIMASK-guarded
 * test-and-set, not a ThreadX object, because it also has to protect the
 * open/close transitions that create and destroy those objects.  bench_gate is
 * not this: it checks that the clock a benchmark divides by is trustworthy,
 * which is a different question.
 *
 * CACHE.  Not here any more (issue #46).  Maintenance is the port's, in
 * npu_cache.c, hung off the driver's inference_begin/end callbacks -- the only
 * two instants where the arena's owner actually changes hands, both of them
 * inside Invoke().  From out here every choice is wrong: cleaning the input
 * before the call is too early (the ethos-u kernel writes arena scratch after
 * it) and invalidating outputs after it is too late (TFLM writes the arena
 * before Invoke() returns).
 *
 * FIELD OF VIEW.  The largest centred square of the frame -- 240x240 of the
 * camera's 320x240 -- is SCALED into the input (issue #48).  It used to be a
 * 128x128 centre CROP, a field of view so narrow the detector was nearly
 * useless at any normal working distance.  The resize is scalar and lives in
 * port/npu/nn_preproc.c, where it is HOST-TESTED -- that is the reason, and it
 * is the whole reason.  This used to say that linking the vendor's Helium
 * routine would put predicated MVE in the image, back when a gate barred that;
 * issue #42 deleted the gate and lifted the ban, so the sentence outlived the
 * rule it cited.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "WE2_device.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

#include "blob.h"
#include "nor_flash.h"    /* NOR_XIP_BASE -- one spelling of the alias base */
#include "npu.h"
#include "npu_hw.h"
#include "nn_overlay.h"
#include "nn_preproc.h"
#include "blazeface.h"
#include "camera.h"
#include "cam_dp.h"
#include "cam_sensor.h"
#include "cam_lcd_sink.h"

#include "tx_api.h"        /* tx_time_get(): ThreadX ticks, 1 ms here */

/*
 * Where the models live: IN THE ASSET STORE, BY NAME (issue #93).
 *
 * They used to live at two addresses board.cmake compiled in, and `--target
 * flash-model-cls|det` was the only way to change one -- which meant the host
 * build tree, and for the detector (which cannot be committed) reproducing a
 * whole pipeline first.  Now `nn open cls` asks the store for the blob called
 * `cls`, and putting a new one there is `blob write` over the console.
 *
 * The raw form survives as `nn open --addr <addr> <len>`, spelled with a flag
 * so that a name can never be read as an address or the other way round.  It is
 * how you look at a model somebody put somewhere the store does not carve, and
 * during Step 4a it is also how the old fixed addresses are still reachable, to
 * check that a blob and the model it replaced infer the same thing.
 *
 * [!] AND THE RAW FORM TAKES A LENGTH, WHICH IS NOT CEREMONY.  npu_open()'s
 * bounds check IS the length (npu.h); "the rest of the window" would put every
 * other slot and the bootloader's own block inside the valid reference range of
 * a malformed model, which is not a bounds check at all.
 *
 * [!] BUT THE RAW FORM IS BOUNDED BY THE WINDOW AND NOT BY A SLOT, and that is
 * a property to state rather than a gap to find later.  `--addr` deliberately
 * skips the store: no name lookup, no header, no CRC.  So a length that starts
 * inside one slot and reaches into the next one is accepted, if the bytes
 * happen to verify -- npu_open() has no slot map and is not being given one.
 * It cannot reach past the end of the 16 MB alias, which is the guarantee the
 * raw form makes.  That is the right shape for what it is FOR: reading a model
 * somebody put somewhere the store does not carve, including the fixed
 * addresses the models were flashed to before #93, which are outside the
 * writable interval entirely.  Slot isolation is what `nn open <name>` is for.
 */

/* --- ownership ----------------------------------------------------------- */

static volatile uint8_t nn_busy;

static int nn_try_acquire(void)
{
	uint32_t pm = __get_PRIMASK();
	int ok;

	__disable_irq();
	ok = !nn_busy;
	if (ok)
		nn_busy = 1u;
	__set_PRIMASK(pm);
	return ok;
}

static void nn_release(void)
{
	nn_busy = 0u;
}

/* --- state --------------------------------------------------------------- */

static uint8_t  nn_open_done;
static uint32_t nn_model_addr;
static uint32_t nn_model_len;
/** What was typed, so `nn info` can say where the open model came from: the
 *  blob's name, or "--addr" for the raw form.  A label and never a key -- the
 *  slot it resolved to is recorded next to it and the name is not looked up
 *  again. */
static char     nn_model_from[BLOB_NAME_MAX + 1];
static int      nn_model_slot;      /**< -1 for the raw form */

/* --- resolving a model in the asset store -------------------------------- */

/**
 * Where an `nn open` is pointed.
 *
 * Filled in by the argument parser before any hardware is touched, and for the
 * name form the address and length are still zero at that point: they come from
 * the blob header, under the lease, once there is one.
 */
struct nn_source {
	const char *name;   /**< NULL for the raw form            */
	uint32_t    addr;   /**< absolute, in the XIP alias       */
	uint32_t    len;
};

/**
 * Read every slot into a view, under the caller's lease.
 *
 * [!] IT REFUSES ON THE FIRST SLOT IT CANNOT READ rather than resolving from
 * what it got.  A scan with a hole in it is a scan that cannot say a name is
 * unique, and "not found" is exactly the wrong answer to give about a slot
 * nobody looked at -- the writer's blob_choose_target() has the same rule for
 * the same reason.
 */
static int nn_scan_slots(struct cli_instance *sh, uint32_t token,
                         const char *name, struct blob_slot_view *v,
                         unsigned *count)
{
	unsigned n = blob_map_count(), i;

	if (n == 0u || n > BLOB_MAX_SLOTS) {
		cli_error(sh, "nn: the slot table has %u slots, which this build "
		              "cannot scan\r\n", n);
		return -1;
	}
	for (i = 0u; i < n; i++) {
		struct blob_info info;
		int rc = blob_stat_leased(i, token, &info, NULL);

		if (rc != BLOB_OK) {
			cli_error(sh, "nn: slot %u unreadable (%s)\r\n", i,
			          rc == BLOB_ERR_FAULT ? "the NOR port is faulted; "
			                                 "a reset is required"
			          : rc == BLOB_ERR_BUSY ? "the flash is busy"
			          : rc == BLOB_ERR_MAP  ? "the slot table does not fit "
			                                  "the writable interval"
			                                : "bad slot");
			return -1;
		}
		v[i].state      = info.state;
		v[i].name_match = (uint8_t)(info.state == BLOB_VALID &&
		                            strcmp(info.name, name) == 0);
	}
	*count = n;
	return 0;
}

/**
 * Turn a blob name into the address and length npu_open() will be given.
 *
 * [!] THE ORDER HERE IS THE POINT (issue #93), and it is the order the plan
 * review settled on after the first draft got it wrong:
 *
 *   the caller already holds the NPU lease  ->  scan every slot  ->  resolve
 *   the name  ->  CHECK THE PAYLOAD'S CRC  ->  take the length from the header
 *   that check ran against  ->  hand it to npu_open()
 *
 * with the lease never dropped in between.  Two things make each step
 * necessary.  BLOB_VALID is a statement about the HEADER only (blob_state.h):
 * it says a header decoded, not that the payload beneath it is the one that
 * header describes -- so a model must be verified before it is parsed, and it
 * is `nn` and not the store that has to ask.  And blob_stat()'s lease is taken
 * and RETURNED per slot, so resolving through it would leave a gap between the
 * answer and the parse in which a background `blob write` could replace the
 * very slot that was chosen.
 */
static int nn_resolve_blob(struct cli_instance *sh, uint32_t token,
                           struct nn_source *src)
{
	struct blob_slot_view v[BLOB_MAX_SLOTS];
	struct blob_info info;
	unsigned count = 0u, slot = 0u;
	uint32_t computed = 0u, payload = 0u;
	enum blob_lookup found;
	int rc;

	if (nn_scan_slots(sh, token, src->name, v, &count) != 0)
		return -1;

	found = blob_resolve_name(v, count, &slot);
	if (found != BLOB_LOOKUP_FOUND) {
		cli_error(sh, "nn: '%s': %s\r\n", src->name,
		          blob_lookup_name(found));
		if (found == BLOB_LOOKUP_NONE)
			cli_error(sh, "nn: (blob list shows what is there; "
			              "blob write %s <slot> puts it there)\r\n",
			          src->name);
		return -1;
	}

	/* [!] AND THE CRC IS CHECKED WITH THE SAME LEASE STILL OUT.  The payload
	 * is read where it lies for as long as the model is open, so what is being
	 * established is not "these bytes were right once" but "these bytes are
	 * right and cannot change while I hold this". */
	rc = blob_verify_leased(slot, token, &info, &computed);
	if (rc != BLOB_OK) {
		if (rc == BLOB_ERR_CRC)
			cli_error(sh, "nn: slot %u ('%s') fails its CRC "
			              "(stored %08lx, flash %08lx); refusing to parse it\r\n",
			          slot, src->name, (unsigned long)info.crc32,
			          (unsigned long)computed);
		else
			cli_error(sh, "nn: slot %u ('%s') could not be verified (%d)\r\n",
			          slot, src->name, rc);
		return -1;
	}

	/* The length comes from the header blob_verify_leased() actually checked
	 * against, and not from a second stat: those two could disagree, and the
	 * one that matters is the one the CRC was compared under. */
	payload = blob_payload_addr(slot);
	if (payload == 0u || info.length == 0u) {
		cli_error(sh, "nn: slot %u ('%s') has no payload to parse\r\n",
		          slot, src->name);
		return -1;
	}
	src->addr = NOR_XIP_BASE + payload;
	src->len  = info.length;
	nn_model_slot = (int)slot;
	return 0;
}

/* --- preprocessing ------------------------------------------------------- */

/*
 * Camera frame -> input tensor.
 *
 * The arithmetic is nn_preproc's (issue #48): crop the largest centred
 * rectangle with the input's aspect ratio, scale it in, convert planar B/G/R to
 * interleaved RGB, and shift uint8 to int8.  What is left here is the part that
 * needs a shell instance to complain to.
 *
 * @param geom  filled in, because the caller needs the same geometry to report
 *              the field of view and to map boxes back to frame pixels -- one
 *              transform, computed once.
 */
static int nn_fill_input(struct cli_instance *sh, const uint8_t *raw,
                         const struct npu_tensor *in,
                         struct nn_preproc_geom *geom)
{
	uint32_t w, h;

	if (in->rank != 4 || in->dims[3] != 3) {
		cli_error(sh, "nn: model input is not HxWx3 (rank %u)\r\n", in->rank);
		return -1;
	}
	h = (uint32_t)in->dims[1];
	w = (uint32_t)in->dims[2];

	/* No "does it fit the frame" test any more: the input is SCALED, so an
	 * input larger than the frame is an ordinary upscale.  The old code
	 * refused it. */
	if (nn_preproc_geom(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, w, h, geom) != 0) {
		cli_error(sh, "nn: cannot fit a %lux%lu input to a %ux%u frame\r\n",
		          (unsigned long)w, (unsigned long)h,
		          CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT);
		return -1;
	}
	if (in->bytes < (size_t)w * h * 3u) {
		cli_error(sh, "nn: input tensor is shorter than its own shape\r\n");
		return -1;
	}

	if (nn_preproc_fill(raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, geom,
	                    (uint8_t *)in->data) != 0) {
		cli_error(sh, "nn: preprocessing refused the frame\r\n");
		return -1;
	}
	return 0;
}

/* The field of view, printed with every result: a score read without knowing
 * what the model was shown is not a result. */
static void nn_print_fov(struct cli_instance *sh,
                         const struct nn_preproc_geom *g)
{
	cli_print(sh, "    %lux%lu centre crop of %ux%u at +%lu+%lu, scaled to "
	              "%lux%lu\r\n",
	          (unsigned long)g->w, (unsigned long)g->h,
	          CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	          (unsigned long)g->x, (unsigned long)g->y,
	          (unsigned long)g->dst_w, (unsigned long)g->dst_h);
}

/* --- reporting ----------------------------------------------------------- */

static void nn_print_tensor(struct cli_instance *sh, const char *what,
                            const struct npu_tensor *t)
{
	cli_print(sh, "%-6s %s", what, npu_type_name(t->type));
	for (unsigned i = 0; i < t->rank; i++)
		cli_print(sh, "%s%ld", i == 0u ? " [" : "x", (long)t->dims[i]);
	cli_print(sh, "]  %lu B  scale %d/1e6  zp %ld\r\n",
	          (unsigned long)t->bytes, (int)(t->scale * 1000000.0f),
	          (long)t->zero_point);
}

/* Top-N over an int8 output vector, by insertion -- N is 5 and the vector is a
 * class count, so nothing cleverer earns its code size. */
static void nn_print_top(struct cli_instance *sh, const struct npu_tensor *out)
{
	enum { TOP_N = 5 };
	int      best_i[TOP_N];
	int8_t   best_v[TOP_N];
	unsigned n = 0;
	const int8_t *v = (const int8_t *)out->data;
	size_t count = out->bytes;

	for (unsigned i = 0; i < TOP_N; i++) {
		best_i[i] = -1;
		best_v[i] = -128;
	}
	for (size_t i = 0; i < count; i++) {
		for (unsigned k = 0; k < TOP_N; k++) {
			if (v[i] > best_v[k]) {
				for (unsigned j = TOP_N - 1u; j > k; j--) {
					best_v[j] = best_v[j - 1u];
					best_i[j] = best_i[j - 1u];
				}
				best_v[k] = v[i];
				best_i[k] = (int)i;
				if (n < TOP_N)
					n++;
				break;
			}
		}
	}
	for (unsigned k = 0; k < n; k++) {
		/* Dequantised score, in thousandths, so no %f is needed on a path that
		 * would otherwise pull float printf into an inference report. */
		long milli = (long)(((float)best_v[k] - (float)out->zero_point)
		                    * out->scale * 1000.0f);

		cli_print(sh, "  #%u  class %-4d  raw %4d  score %ld/1000\r\n",
		          k + 1u, best_i[k], (int)best_v[k], milli);
	}
}

/* --- subcommands --------------------------------------------------------- */

static int cmd_nn_info(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor t;

	(void)argc;
	(void)argv;

	/* [!] BEHIND THE GATE, like every other subcommand (issue #45).  It used
	 * not to be, which was wrong rather than merely untidy: nn_busy also
	 * covers open and close, and the teardown rewrites npu_hw_ready() and the
	 * wrapped-IRQ set that the lines below walk.  Reading hardware state while
	 * another job is dismantling it is a race, and the report it produces is
	 * of a machine that no longer exists.
	 *
	 * When the gate refuses, only facts that CANNOT be in flight are printed:
	 * the arena reservation is a link-time constant. */
	if (!nn_try_acquire()) {
		cli_print(sh, "npu      busy (another nn job holds it)\r\n");
		cli_print(sh, "arena    %lu B reserved @%p\r\n",
		          (unsigned long)npu_arena_bytes(), npu_arena_base());
		return 0;
	}

	cli_print(sh, "npu      %s\r\n",
	          npu_hw_ready() ? "up (secure, privileged)" : "down");
	if (!npu_hw_ready() && npu_hw_fail_reason() != NULL)
		cli_print(sh, "         last refusal: %s\r\n", npu_hw_fail_reason());
	cli_print(sh, "arena    %lu B reserved @%p\r\n",
	          (unsigned long)npu_arena_bytes(), npu_arena_base());

	if (npu_hw_ready()) {
		int lines[8];
		unsigned n = npu_hw_wrapped_irqs(lines, 8u);

		/* The EPK rule is that nothing may be enabled but unaccounted.  The
		 * bring-up enforces it by refusing, so this is the state of that
		 * enforcement rather than a check -- but an invariant with no readout
		 * is one nobody verifies. */
		cli_print(sh, "irq      ");
		for (unsigned i = 0; i < n; i++)
			cli_print(sh, "%s%d", i ? ", " : "", lines[i]);
		cli_print(sh, "%s (wrapped for cpu%% accounting)\r\n",
		          n ? "" : "none");
	}

	cli_print(sh, "detect   score threshold %u/1000 (nn thresh <1..999>)\r\n",
	          blazeface_get_thresh_milli());

	if (!nn_open_done) {
		cli_print(sh, "model    not open (nn open <name>)\r\n");
		nn_release();
		return 0;
	}
	if (nn_model_from[0] != '\0')
		cli_print(sh, "model    '%s' from slot %d, 0x%08lx, %lu B\r\n",
		          nn_model_from, nn_model_slot, (unsigned long)nn_model_addr,
		          (unsigned long)nn_model_len);
	else
		cli_print(sh, "model    0x%08lx, %lu B (--addr)\r\n",
		          (unsigned long)nn_model_addr, (unsigned long)nn_model_len);
	cli_print(sh, "arena    %lu B used by this layout\r\n",
	          (unsigned long)npu_arena_used());
	if (npu_input(&t) == NPU_OK)
		nn_print_tensor(sh, "input", &t);
	for (unsigned i = 0; i < npu_output_count(); i++)
		if (npu_output(i, &t) == NPU_OK)
			nn_print_tensor(sh, "output", &t);
	nn_release();
	return 0;
}

/* The two spellings, parsed before any hardware is touched.  `--addr` is a
 * flag and not a shape the parser guesses at: a bare token could be a name or a
 * hex number depending on what somebody called their blob, and the reading it
 * gets must not depend on that. */
static int nn_parse_open_args(struct cli_instance *sh, int argc, char **argv,
                              struct nn_source *src)
{
	memset(src, 0, sizeof(*src));

	if (argc == 2 && strcmp(argv[1], "--addr") != 0) {
		enum blob_name_verdict nv = blob_name_check(argv[1], NULL);

		if (nv != BLOB_NAME_OK) {
			cli_error(sh, "nn: '%s' is not a blob name (%s)\r\n", argv[1],
			          blob_name_verdict_name(nv));
			return -1;
		}
		src->name = argv[1];
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "--addr") == 0) {
		if (cli_parse_u32(argv[2], &src->addr) != 0) {
			cli_error(sh, "nn: bad address '%s'\r\n", argv[2]);
			return -1;
		}
		if (cli_parse_u32(argv[3], &src->len) != 0) {
			cli_error(sh, "nn: bad length '%s'\r\n", argv[3]);
			return -1;
		}
		/* Refused here in this command's own words rather than as a bare
		 * NPU_ERR_MODEL_ADDR from three layers down, because at this end of
		 * it the operator can see which of the two numbers is wrong. */
		if (src->len < npu_model_len_min() ||
		    src->len > npu_model_len_max(src->addr)) {
			cli_error(sh, "nn: length %lu is not between %lu and %lu for "
			              "0x%08lx\r\n",
			          (unsigned long)src->len,
			          (unsigned long)npu_model_len_min(),
			          (unsigned long)npu_model_len_max(src->addr),
			          (unsigned long)src->addr);
			return -1;
		}
		return 0;
	}
	cli_error(sh, "nn: usage: nn open <name> | nn open --addr <addr> <len>\r\n");
	return -1;
}

static int cmd_nn_open(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_source src;
	int rc;

	if (nn_parse_open_args(sh, argc, argv, &src) != 0)
		return -1;

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (nn_open_done) {
		cli_error(sh, "nn: already open (nn close first)\r\n");
		nn_release();
		return -1;
	}

	/* [!] THE BRING-UP COMES BEFORE THE LOOKUP, AND THAT IS THE ORDERING THE
	 * WHOLE COMMAND IS BUILT AROUND (issue #93).  npu_hw_init() takes the
	 * flash reader lease, so from here to the end of the model's life the
	 * window is up and no writer can take the part -- a reservation needs the
	 * lease mask empty (nor_state.h).  Resolving a name first and bringing the
	 * hardware up afterwards would put a gap between the answer and the parse,
	 * and the answer is an ADDRESS. */
	if (npu_hw_init() != 0) {
		cli_error(sh, "nn: %s\r\n",
		          npu_hw_fail_reason() ? npu_hw_fail_reason() : "bring-up failed");
		nn_release();
		return -1;
	}

	nn_model_slot = -1;
	if (src.name != NULL &&
	    nn_resolve_blob(sh, npu_hw_flash_lease(), &src) != 0) {
		/* Every failure from here down leaves the hardware down: an NPU that
		 * is up with no model is a state nothing below here would ever use,
		 * and it would hold the flash lease against `blob write` for as long
		 * as it lasted. */
		npu_hw_deinit();
		nn_release();
		return -1;
	}

	rc = npu_open(src.addr, src.len, npu_arena_base(), npu_arena_bytes());
	if (rc != NPU_OK) {
		cli_error(sh, "nn: %s (0x%08lx, %lu B)\r\n", npu_status_name(rc),
		          (unsigned long)src.addr, (unsigned long)src.len);
		npu_hw_deinit();
		nn_release();
		return -1;
	}

	nn_open_done  = 1u;
	nn_model_addr = src.addr;
	nn_model_len  = src.len;
	if (src.name != NULL) {
		(void)strncpy(nn_model_from, src.name, sizeof(nn_model_from) - 1u);
		nn_model_from[sizeof(nn_model_from) - 1u] = '\0';
		cli_print(sh, "nn: '%s' from slot %d open at 0x%08lx, %lu B, "
		              "arena %lu/%lu B\r\n",
		          nn_model_from, nn_model_slot, (unsigned long)src.addr,
		          (unsigned long)src.len, (unsigned long)npu_arena_used(),
		          (unsigned long)npu_arena_bytes());
	} else {
		nn_model_from[0] = '\0';
		cli_print(sh, "nn: model at 0x%08lx open, %lu B, arena %lu/%lu B\r\n",
		          (unsigned long)src.addr, (unsigned long)src.len,
		          (unsigned long)npu_arena_used(),
		          (unsigned long)npu_arena_bytes());
	}
	nn_release();
	return 0;
}

static int cmd_nn_close(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	npu_close();
	npu_hw_deinit();
	nn_open_done     = 0u;
	nn_model_addr    = 0u;
	nn_model_len     = 0u;
	nn_model_slot    = -1;
	nn_model_from[0] = '\0';
	nn_release();
	cli_print(sh, "nn: closed\r\n");
	return 0;
}

/*
 * One frame from the camera into the model's input tensor.
 *
 * Shared by `nn run` and `nn detect`, which differ only in how the OUTPUT is
 * read.  camera_capture() quiesces the datapath on both the success and the
 * failure path, so the camera is never left streaming into a buffer the NPU is
 * about to be pointed at; it also refuses while a preview is running, which is
 * the camera/NPU concurrency question answered by the layer that owns it.
 */
static int nn_capture_and_fill(struct cli_instance *sh,
                               const struct npu_tensor *in,
                               struct nn_preproc_geom *geom)
{
	int rc = camera_capture();

	if (rc != 0) {
		cli_error(sh, "nn: capture failed (%d)\r\n", rc);
		return -1;
	}
	return nn_fill_input(sh, camera_raw_frame(), in, geom);
}

static int cmd_nn_run(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor in, out;
	struct nn_preproc_geom geom;
	uint32_t t0, t1;
	int rc;

	(void)argc;
	(void)argv;

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (!nn_open_done) {
		cli_error(sh, "nn: no model open\r\n");
		nn_release();
		return -1;
	}
	if (npu_input(&in) != NPU_OK) {
		cli_error(sh, "nn: no input tensor\r\n");
		nn_release();
		return -1;
	}

	if (nn_capture_and_fill(sh, &in, &geom) != 0) {
		nn_release();
		return -1;
	}

	/* No cache maintenance here (issue #46).  It moved into the port, where the
	 * driver's own lifecycle callbacks put it at the only two instants that are
	 * correct: the whole arena is cleaned immediately before the command stream
	 * is launched, and invalidated once completion is confirmed -- both before
	 * TFLM resumes.  Anything done from out here is either too early (the
	 * ethos-u kernel writes arena scratch after it) or too late (TFLM writes the
	 * arena before Invoke() returns). */
	t0 = (uint32_t)tx_time_get();
	rc = npu_invoke();
	t1 = (uint32_t)tx_time_get();

	if (rc != NPU_OK) {
		cli_error(sh, "nn: %s\r\n", npu_status_name(rc));
		nn_release();
		return -1;
	}

	cli_print(sh, "nn: inference %lu ms\r\n", (unsigned long)(t1 - t0));
	nn_print_fov(sh, &geom);

	/* The outputs are already visible: the port invalidated the whole arena
	 * once completion was confirmed, before TFLM resumed. */
	if (npu_output(0u, &out) == NPU_OK)
		nn_print_top(sh, &out);

	nn_release();
	return 0;
}

/* --- face detection (issue #45) ------------------------------------------- */

/* Enough descriptors for any model this port will hand to a decoder; BlazeFace
 * needs four.  A model with more outputs than this is reported rather than
 * silently truncated, because a decoder that locates tensors by shape would
 * otherwise fail with "not BlazeFace-shaped" and never say why. */
#define NN_MAX_OUTPUTS 8

/*
 * Does the model's input quantisation match what nn_fill_input() produces?
 *
 * [!] THIS IS NOT A FORMALITY.  nn_fill_input() writes `pixel - 128` -- a fixed
 * shift, not a quantisation using the tensor's parameters -- which is exactly
 * right for scale 1/255 with zero point -128 and progressively wrong for
 * anything else.  BlazeFace-front 128 is quantised that way, and being wrong
 * here does not look like an error: the boxes are still boxes, just in the
 * wrong places, on an image nobody can see.  So it is checked, and a mismatch
 * refuses rather than warns.
 *
 * The comparison is on the scale in millionths so that no float formatting is
 * needed on this path; 1/255 is 3921.6e-6 and the window is about one percent,
 * which distinguishes "this model" from "a differently quantised model" without
 * depending on how the converter rounded.
 */
static int nn_input_quant_ok(struct cli_instance *sh, const struct npu_tensor *in)
{
	long micro = (long)(in->scale * 1000000.0f + 0.5f);

	if (!npu_tensor_is_int8(in->type)) {
		cli_error(sh, "nn: detect needs an int8 input, this model has %s\r\n",
		          npu_type_name(in->type));
		return -1;
	}
	if (in->zero_point != -128 || micro < 3882 || micro > 3961) {
		cli_error(sh, "nn: this model wants scale %ld/1e6 zp %ld, but the frame "
		              "is filled as (pixel - 128), which is scale 3922/1e6 "
		              "zp -128\r\n",
		          micro, (long)in->zero_point);
		return -1;
	}
	return 0;
}

static int cmd_nn_detect(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor in;
	struct npu_tensor outs[NN_MAX_OUTPUTS];
	struct bf_det det[BF_MAX_DET];
	struct nn_preproc_geom geom;
	unsigned n_out, i;
	uint32_t t0, t1;
	long peak;
	int rc = -1, nd;

	(void)argc;
	(void)argv;

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}

	/* One exit from here down, so the gate is released exactly once however
	 * this ends -- the failure paths outnumber the success path. */
	if (!nn_open_done) {
		cli_error(sh, "nn: no model open\r\n");
		goto out;
	}
	if (npu_input(&in) != NPU_OK) {
		cli_error(sh, "nn: no input tensor\r\n");
		goto out;
	}
	if (nn_input_quant_ok(sh, &in) != 0)
		goto out;

	n_out = npu_output_count();
	if (n_out > NN_MAX_OUTPUTS) {
		cli_error(sh, "nn: model has %u outputs, this command reads %u\r\n",
		          n_out, (unsigned)NN_MAX_OUTPUTS);
		goto out;
	}
	for (i = 0; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			cli_error(sh, "nn: output %u is unreadable\r\n", i);
			goto out;
		}

	if (nn_capture_and_fill(sh, &in, &geom) != 0)
		goto out;

	/* No cache maintenance here either -- see the note in cmd_nn_run(). */
	t0 = (uint32_t)tx_time_get();
	if (npu_invoke() != NPU_OK) {
		cli_error(sh, "nn: inference failed\r\n");
		goto out;
	}
	t1 = (uint32_t)tx_time_get();

	/* The descriptors were taken before the run; the POINTERS in them are
	 * arena addresses that do not move, and the port made the contents visible
	 * before Invoke() returned. */
	nd = blazeface_decode(outs, n_out, det, BF_MAX_DET);
	if (nd < 0) {
		cli_error(sh, "nn: the open model is not BlazeFace-shaped (wants 1x512x16, "
		              "1x512x1, 1x384x16 and 1x384x1 int8 outputs)\r\n");
		goto out;
	}

	/* Integers only, all the way down: no %f on a path that would otherwise
	 * drag float formatting into a detection report. */
	cli_print(sh, "nn: detect %lu ms, %d face(s)\r\n",
	          (unsigned long)(t1 - t0), nd);
	nn_print_fov(sh, &geom);

	for (int k = 0; k < nd; k++) {
		struct nn_preproc_box b;

		/* [!] FRAME pixels, through the same transform the overlay draws
		 * with (issue #48).  They used to be pixels of the crop, which
		 * made the console and the panel two coordinate systems that had
		 * to be reconciled by hand -- and the printed numbers were the
		 * ones that could not be checked against anything. */
		if (nn_preproc_box(&geom, det[k].x, det[k].y, det[k].w,
		                   det[k].h, &b) != 0) {
			cli_print(sh, "  #%d  box off-frame  score %ld/1000\r\n",
			          k + 1, (long)(det[k].score * 1000.0f));
			continue;
		}
		cli_print(sh, "  #%d  box %ld,%ld  %ldx%ld px  score %ld/1000\r\n",
		          k + 1, (long)b.x0, (long)b.y0,
		          (long)(b.x1 - b.x0), (long)(b.y1 - b.y0),
		          (long)(det[k].score * 1000.0f));
	}

	/* [!] ALWAYS, even at zero detections -- especially at zero detections.
	 * "no faces" and "the threshold is above everything the model produced"
	 * are different states and the count alone cannot tell them apart.  The
	 * peak is over ALL 896 anchors (blazeface.c never cuts the scan short), so
	 * it is the real maximum and not the maximum of a prefix. */
	peak = (long)(blazeface_last_max_score() * 1000.0f);
	cli_print(sh, "    peak raw %ld/1000, threshold %ld/1000 raw "
	              "(= %u/1000 after sigmoid)\r\n",
	          peak, (long)(blazeface_get_thresh_logit() * 1000.0f),
	          blazeface_get_thresh_milli());
	cli_print(sh, "    %d anchor(s) over threshold, %d kept of %d, %d after NMS\r\n",
	          blazeface_last_npass(), blazeface_last_nkept(),
	          (int)BF_MAX_CAND, nd);
	rc = 0;
out:
	nn_release();
	return rc;
}

/* --- live preview with boxes (issue #48) ---------------------------------- */

/*
 * `nn preview`: the camera on the panel, with the faces drawn on it.
 *
 * The work happens on the CAMERA PRODUCER THREAD, inside the sink's consume()
 * (port/npu/nn_overlay.c).  This function only starts it, waits, and stops it.
 * That is why the whole body is bracketed by the ownership gate: the gate is a
 * lease on the NPU held by this command while another thread uses it, which is
 * what keeps `nn close` from dismantling an interpreter mid-frame.
 *
 * [!] THE TEARDOWN IS NOT SYMMETRIC WITH THE SETUP, on purpose.  A stop that
 * does not return CAM_OK means the producer is still running, so the sink is
 * NOT detached and the gate is NOT released -- see camera.h.  Leaving `nn`
 * leased until reboot is the correct trade against a producer executing inside
 * a sink somebody just unlinked.
 */
static int cmd_nn_preview(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor in;
	struct npu_tensor outs[NN_MAX_OUTPUTS];
	struct nn_overlay_stats ns;
	struct camera_stats st;
	uint32_t frames = 0u;   /* 0 = until Ctrl+C, as `camera preview` */
	uint32_t before;
	unsigned n_out, i;
	ULONG t0, t1, ticks;
	int rc, stop_rc;

	if (argc > 1 && cli_parse_u32(argv[1], &frames) != 0) {
		cli_error(sh, "nn: bad frame count '%s'\r\n", argv[1]);
		return -1;
	}

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (!nn_open_done) {
		cli_error(sh, "nn: no model open (nn open det)\r\n");
		nn_release();
		return -1;
	}

	/*
	 * [!] EVERY CHECK BEFORE THE STREAM STARTS.
	 *
	 * A preview that starts and then fails on every frame is a panel
	 * showing a live picture with no boxes and no explanation -- the exact
	 * failure this whole command exists to make visible.  So the input
	 * quantisation and the output shapes are settled here, where refusing
	 * costs nothing and can say why.
	 */
	if (npu_input(&in) != NPU_OK) {
		cli_error(sh, "nn: no input tensor\r\n");
		nn_release();
		return -1;
	}
	if (nn_input_quant_ok(sh, &in) != 0) {
		nn_release();
		return -1;
	}
	n_out = npu_output_count();
	if (n_out > NN_MAX_OUTPUTS) {
		cli_error(sh, "nn: model has %u outputs, this command reads %u\r\n",
		          n_out, (unsigned)NN_MAX_OUTPUTS);
		nn_release();
		return -1;
	}
	for (i = 0; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			cli_error(sh, "nn: output %u is unreadable\r\n", i);
			nn_release();
			return -1;
		}
	if (!blazeface_shapes_ok(outs, n_out)) {
		cli_error(sh, "nn: the open model is not BlazeFace-shaped "
		              "(nn open det)\r\n");
		nn_release();
		return -1;
	}

	/*
	 * [!] ONE CALL, AND THEREFORE ONE FAILURE (issue #63).
	 *
	 * This used to attach the sink and then start the stream, and a start that
	 * came back BUSY meant a stream was already running WITH THIS SINK
	 * ATTACHED -- a producer could be inside consume() at that moment, so the
	 * sink could not be detached and the NPU could not be released, and this
	 * command had to leave both held for good.  It was reachable: `camera
	 * bench` starts a stream owning no sink, so it could land between the two
	 * calls.
	 *
	 * The camera does both under its API mutex now, so a failure here means
	 * nothing was attached and nothing started: release the gate and go.  The
	 * lease still has to survive the STOP side, which is a different problem
	 * (#48) and is handled below.
	 */
	rc = cam_lcd_sink_attach_and_stream(nn_overlay_arm());
	if (rc != CAM_OK) {
		if (rc == CAM_ERR_BUSY)
			cli_error(sh, "nn: a preview is already running, or "
			              "another command owns the camera\r\n");
		else
			cli_error(sh, "nn: preview start failed (%d)\r\n", rc);
		nn_release();
		return -1;
	}

	camera_stream_stats(&st);
	before = st.frames;
	t0 = tx_time_get();

	/* Only waiting happens here: capture, inference and the blit are all on
	 * the producer.  One poll per tick notices Ctrl+C and costs nothing
	 * against a ~115 ms frame. */
	for (;;) {
		if (cli_cancel_requested(sh))
			break;
		camera_stream_stats(&st);
		if (!st.streaming)
			break;                      /* the producer gave up */
		if (frames != 0u && (st.frames - before) >= frames)
			break;
		if (cli_sleep(sh, 1u) != 0)
			break;
	}

	t1 = tx_time_get();
	ticks = t1 - t0;

	/* [!] BEFORE the stop, always: it is what stops the frame in flight
	 * from starting an inference the join would then have to wait for. */
	nn_overlay_request_stop();
	stop_rc = camera_stream_stop();

	if (stop_rc != CAM_OK) {
		/*
		 * No confirmed stop, so nothing is torn down and the gate is
		 * kept: detaching now would unlink a sink the producer may be
		 * inside.  That much is the same for every failure -- but what
		 * the operator is told is NOT, and saying the poisoned version
		 * of it for a lock collision was a plain lie (issue #65).
		 */
		if (stop_rc == CAM_ERR_LOCKED)
			cli_error(sh, "nn: the camera API stayed locked, so the "
			              "stop was never requested;\r\n"
			              "    nothing was torn down, the sink stays "
			              "attached and nn stays held.\r\n"
			              "    A reboot is what clears it\r\n");
		else
			cli_error(sh, "nn: the camera did not stop (%d); it is "
			              "now unusable until reboot, and nn stays "
			              "held\r\n", stop_rc);
		return -1;
	}

	/*
	 * [!] AND THE DETACH IS THE SECOND HALF OF THE STOP (issue #57).
	 *
	 * The blit runs on the panel thread now, so a confirmed producer stop no
	 * longer proves that nothing is using this frame: detach unlinks the
	 * sink and then drains that thread.  A drain that does not finish means
	 * a thread may still be inside draw(), reading the detections -- and
	 * releasing the NPU there would let `nn close` dismantle an interpreter
	 * underneath it, which is the same trade this command already makes for
	 * a producer that never came back.  So the lease is released only after
	 * BOTH halves are confirmed.
	 */
	stop_rc = cam_lcd_sink_detach();
	if (stop_rc != CAM_OK) {
		cli_error(sh, "nn: the panel thread did not finish (%d); the "
		              "preview is unusable until reboot, and nn stays "
		              "held\r\n", stop_rc);
		return -1;
	}
	nn_release();

	if (cli_cancel_requested(sh)) {
		/* Cancelled: the shared core discards output produced while
		 * cancel_req is set, so a summary would never arrive.  The
		 * dispatcher's "^C" is the feedback, as in `camera preview`. */
		return 0;
	}

	camera_stream_stats(&st);
	nn_overlay_stats(&ns);

	if (st.fault != NULL) {
		cli_error(sh, "nn: preview stopped: %s\r\n", st.fault);
		return -1;
	}

	{
		uint32_t got = st.frames - before;
		uint32_t ms = (uint32_t)((ticks * 1000u) /
		                         TX_TIMER_TICKS_PER_SECOND);

		cli_print(sh, "%lu frame(s) in %lu ms", (unsigned long)got,
		          (unsigned long)ms);
		if (ms != 0u)
			cli_print(sh, " = %lu.%lu fps",
			          (unsigned long)(got * 1000u / ms),
			          (unsigned long)((got * 10000u / ms) % 10u));
		cli_print(sh, "\r\n");
		cli_print(sh, "%lu inference(s), %lu face(s) drawn, last %lu ms\r\n",
		          (unsigned long)ns.inferences,
		          (unsigned long)ns.detections,
		          (unsigned long)ns.last_ms);
		/* Both counts matter and neither is an error on its own: frames
		 * are skipped by a pending stop, and the panel being busy is a
		 * dropped frame rather than a failure. */
		if (ns.skipped || ns.errors)
			cli_print(sh, "%lu frame(s) skipped, %lu refused\r\n",
			          (unsigned long)ns.skipped,
			          (unsigned long)ns.errors);
	}
	return 0;
}

static int cmd_nn_thresh(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t milli;
	int rc = 0;

	/* Behind the gate because the threshold is static state that a running
	 * decode reads; setting it mid-decode would apply to part of a frame. */
	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (argc > 1) {
		if (cli_parse_u32(argv[1], &milli) != 0) {
			cli_error(sh, "nn: bad threshold '%s'\r\n", argv[1]);
			rc = -1;
			goto out;
		}
		/* 0 and 1000 are the poles of the inverse sigmoid: the logit the
		 * decoder compares against is undefined there, so they are refused
		 * rather than clamped -- a silently clamped threshold is a setting
		 * that does not do what it says. */
		if (blazeface_set_thresh_milli(milli) != 0) {
			cli_error(sh, "nn: threshold must be 1..999 (milli-probability)\r\n");
			rc = -1;
			goto out;
		}
	}
	cli_print(sh, "nn: detect threshold %u/1000 (raw %ld/1000)\r\n",
	          blazeface_get_thresh_milli(),
	          (long)(blazeface_get_thresh_logit() * 1000.0f));
out:
	nn_release();
	return rc;
}

CLI_SUBCMD_SET_CREATE(nn_subcmds,
	CLI_CMD(info,  NULL, "what is loaded and what the arena costs", cmd_nn_info),
	CLI_CMD_ARG_USAGE(open, NULL, "bring the NPU up and parse the model",
	                  "<name> | --addr <flash-addr> <len>", cmd_nn_open, 2, 2),
	CLI_CMD(close, NULL, "release the model and the NPU",           cmd_nn_close),
	CLI_CMD(run,   NULL, "capture one frame and classify it",       cmd_nn_run),
	CLI_CMD(detect, NULL, "capture one frame and find faces",       cmd_nn_detect),
	CLI_CMD_ARG_USAGE(preview, NULL,
	                  "live preview with face boxes, Ctrl+C to stop",
	                  "[frames]", cmd_nn_preview, 1, 1),
	CLI_CMD_ARG_USAGE(thresh, NULL, "detection score threshold",
	                  "[<1..999>]", cmd_nn_thresh, 1, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nn, nn_subcmds,
                 "Ethos-U55 inference: classification and face detection",
                 NULL, 1, 0);
