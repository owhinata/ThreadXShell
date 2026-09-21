/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.c
 * @brief   Which decoder is in force.  See nn_active.h.
 */
#define LOG_TAG "nn"
#include "log.h"

#include "nn_active.h"

#include "nn_decoder.h"      /* the resident decoder's threshold */
#include "nn_desc.h"         /* nn_tensor -> tensor_desc */
#include "plugin_run.h"

#include <string.h>

/* ---- tensors ------------------------------------------------------------- */

/*
 * The outputs reach a plugin as svc/tensor.h descriptors, which is the contract
 * issue #97 established so that one decoder can read any board's tensors.  The
 * conversion is nn_desc_of(), reused rather than repeated: a second translation
 * could disagree with `nn out` and `nn info` about what a tensor is.
 *
 * A hole in the output set becomes a zeroed descriptor -- UNSUPPORTED -- for
 * the same reason the resident path does it: it is not a model-shape problem,
 * and letting the decoder decide whether the tensors it wants are still there
 * reaches the same answer by a defensible route.
 */
static unsigned to_desc(struct nn_model *m, struct tensor_desc *d, unsigned cap)
{
	int n = nn_output_count(m);
	unsigned i, out;

	if (n < 0)
		n = 0;
	out = (unsigned)n;
	if (out > cap)
		out = cap;
	for (i = 0u; i < out; i++) {
		struct nn_tensor *t = nn_output(m, (int)i);

		if (t == NULL)
			memset(&d[i], 0, sizeof d[i]);
		else
			nn_desc_of(&d[i], t);
	}
	return out;
}

/* ---- the branch ---------------------------------------------------------- */

int nn_active_is_plugin(void)
{
	return plugin_run_active() && plugin_run_slot(PLUGIN_SLOT_DECODE) != NULL;
}

int nn_active_shapes_ok(struct nn_model *m)
{
	plugin_shapes_ok_fn fn =
		(plugin_shapes_ok_fn)plugin_run_slot(PLUGIN_SLOT_SHAPES_OK);

	if (m == NULL)
		return 0;
	if (nn_active_is_plugin() && fn != NULL) {
		struct tensor_desc d[NN_MAX_IO];
		unsigned n = to_desc(m, d, NN_MAX_IO);

		return fn(d, n);
	}
	/* The resident decoder answers per frame, as it always has. */
	return 1;
}

int nn_active_decode(struct nn_model *m)
{
	plugin_decode_fn fn = (plugin_decode_fn)plugin_run_slot(PLUGIN_SLOT_DECODE);

	if (m == NULL)
		return BF_ERR_ARG;
	if (nn_active_is_plugin() && fn != NULL) {
		struct tensor_desc d[NN_MAX_IO];
		unsigned n = to_desc(m, d, NN_MAX_IO);

		return fn(d, n);
	}
	/*
	 * A backstop, not a path: the worker asks nn_active_is_plugin() first and
	 * calls the resident decoder otherwise.  It answers "no decoder is bound"
	 * rather than pretending -- and deliberately not BF_ERR_MODEL, which means
	 * "not a detector" and routes to the shared class report.
	 */
	return BF_ERR_UNINIT;
}

void nn_active_draw(const struct plugin_painter *paint)
{
	plugin_draw_fn fn = (plugin_draw_fn)plugin_run_slot(PLUGIN_SLOT_DRAW);

	if (nn_active_is_plugin() && fn != NULL && paint != NULL)
		fn(paint);
}

int nn_active_can_draw(void)
{
	if (!nn_active_is_plugin())
		return 1;        /* the resident overlay draws its boxes */
	return plugin_run_slot(PLUGIN_SLOT_DRAW) != NULL;
}

int nn_active_can_report(void)
{
	if (!nn_active_is_plugin())
		return 0;        /* the resident result IS the boxes already printed */
	return plugin_run_slot(PLUGIN_SLOT_REPORT) != NULL;
}

int nn_active_report(nn_svc_write_fn write, void *ctx)
{
	plugin_report_fn fn = (plugin_report_fn)plugin_run_slot(PLUGIN_SLOT_REPORT);
	struct plugin_printer out;

	if (!nn_active_is_plugin() || fn == NULL || write == NULL)
		return 0;

	out.ctx   = ctx;
	out.write = write;
	return fn(&out);
}

/* ---- the threshold ------------------------------------------------------- */

#define NN_ACTIVE_PARAM_THRESH_MILLI 0u

unsigned nn_active_get_thresh_milli(void)
{
	plugin_param_get_fn fn =
		(plugin_param_get_fn)plugin_run_slot(PLUGIN_SLOT_PARAM_GET);
	uint32_t v = 0u;

	if (nn_active_is_plugin()) {
		if (fn != NULL && fn(NN_ACTIVE_PARAM_THRESH_MILLI, &v) == 0)
			return (unsigned)v;
		/* A plugin with no threshold has none -- borrowing the resident
		 * decoder's would report a number nothing is deciding with. */
		return NN_SVC_THRESH_NONE;
	}
	return nn_decoder_get_thresh_milli();
}

int nn_active_set_thresh_milli(unsigned milli)
{
	plugin_param_set_fn fn =
		(plugin_param_set_fn)plugin_run_slot(PLUGIN_SLOT_PARAM_SET);

	if (nn_active_is_plugin()) {
		if (fn == NULL)
			return NN_ACTIVE_THRESH_NO_DECODER;
		return fn(NN_ACTIVE_PARAM_THRESH_MILLI, (uint32_t)milli) == 0
		               ? NN_ACTIVE_THRESH_OK : NN_ACTIVE_THRESH_REFUSED;
	}
	return nn_decoder_set_thresh_milli(milli) == BF_OK
	               ? NN_ACTIVE_THRESH_OK : NN_ACTIVE_THRESH_REFUSED;
}

/* ---- the base vtable ----------------------------------------------------- */

/*
 * The transform.  See the header for why it is a constant on this board.
 *
 * Non-finite coordinates are rejected before anything is cast: a degenerate
 * model produces them, and the conversion of a NaN to an integer is undefined.
 * The comparison form catches NaN because every comparison with one is false.
 */
int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out)
{
	const float sw = (float)NN_ACTIVE_FRAME_W;
	const float sh = (float)NN_ACTIVE_FRAME_H;
	float x1f, y1f;

	(void)ctx;
	if (out == NULL)
		return -1;
	if (!(x > -1000.0f && x < 1000.0f) || !(y > -1000.0f && y < 1000.0f) ||
	    !(w > -1000.0f && w < 1000.0f) || !(h > -1000.0f && h < 1000.0f))
		return -1;

	x1f = (x + w) * sw;
	y1f = (y + h) * sh;
	out->x0 = (int32_t)(x * sw);
	out->y0 = (int32_t)(y * sh);
	out->x1 = (int32_t)x1f;
	out->y1 = (int32_t)y1f;
	return 0;
}

/*
 * The producer thread has no console, so this is the only way a decode failure
 * can explain itself.
 *
 * [!] COPIED AND TERMINATED, NOT PRINTED WITH A PRECISION.  This said
 * `LOG_INF("plugin: %.*s", (int)len, s)`, and svc/fmt.c implements neither a
 * precision nor `*` -- deliberately, it is a clean-room minimal formatter --
 * so the plugin's explanation came out as the format string plus whatever the
 * varargs were read as.  The bytes a plugin hands over are not NUL-terminated,
 * so they have to be copied somewhere that is.  The buffer is charged to
 * WIO_PLUGIN_VENEER_BASE_COST, which is derived with it included.
 */
static void nn_plugin_log(void *ctx, const char *s, size_t len)
{
	char line[64];
	size_t n;

	(void)ctx;
	if (s == NULL || len == 0u)
		return;
	n = len < sizeof line - 1u ? len : sizeof line - 1u;
	memcpy(line, s, n);
	line[n] = '\0';
	LOG_INF("plugin: %s%s", line, n < len ? " ..." : "");
}

static const struct plugin_base_api nn_plugin_base = {
	.version  = PLUGIN_ABI_VERSION,
	.size     = sizeof(struct plugin_base_api),
	.ctx      = NULL,
	.log      = nn_plugin_log,
	.to_frame = nn_active_to_frame,
};

const struct plugin_base_api *nn_active_base(void)
{
	return &nn_plugin_base;
}
