/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for svc/nn/nn_input_contract.{h,c} -- the camera input tensor's
 * admission test and the per-element conversion wio-lite-ai and f746g-disco
 * share (issue #162).
 *
 * THE ADMISSION TEST: every refusal by its own code, in its order -- dtype, rank,
 * batch, empty height / width, channels, int8 scale (0, -0, negative, NaN,
 * infinities, subnormals refused; the smallest and largest normal admitted),
 * int8 zero point (both ends admitted, one past each refused), float32 buffer
 * alignment (int8 is not held to it), and a byte count
 * that a 32-bit product would wrap past.
 *
 * THE CONVERSION is compared against a REFERENCE THAT SPELLS OUT THE TARGET'S
 * INSTRUCTION SEQUENCE (fmaf for VFMA.F32, then VMUL, the sign test, VADD +-0.5,
 * conversion toward zero, the zero point, saturation), with the conversion done
 * in 64 bits so the reference has no limit of its own.  It is deliberately not a
 * copy of the pre-#162 C source: on the host that source would not be fused, so
 * it would compute a different float than the target did.  Every reachable
 * 8-bit value x both normalisations x a set of scales and zero points, plus the
 * rounding helper alone at the limit's both sides and at the +-0.5 boundaries.
 *
 * Runs in round-to-nearest with gradual underflow (checked at the start): the
 * FPSCR reset defaults the firmware runs in.
 */
#include <assert.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nn.h"
#include "nn_input_contract.h"

/* float arithmetic is done in float, not in a wider type: the reference below
 * models the target's single-precision instructions step by step. */
_Static_assert(FLT_EVAL_METHOD == 0, "host must evaluate float in float");

static uint32_t bits(float f)
{
	uint32_t u;

	memcpy(&u, &f, sizeof(u));
	return u;
}

static float from_bits(uint32_t u)
{
	float f;

	memcpy(&f, &u, sizeof(f));
	return f;
}

/* ---- the reference: the target's instruction sequence, one step per line ---- */

static float ref_gain(int norm_signed)
{
	return norm_signed ? (1.0f / 127.5f) : (1.0f / 255.0f);
}

static float ref_bias(int norm_signed)
{
	return norm_signed ? -1.0f : 0.0f;
}

/* VFMA.F32 acc=bias, v * gain: one rounding. */
static float ref_f32(uint8_t v, int norm_signed)
{
	return fmaf((float)v, ref_gain(norm_signed), ref_bias(norm_signed));
}

/* From a value in quantised units: sign test, VADD +-0.5 (float), conversion
 * toward zero, + zero point, saturate.  The conversion is done in 64 bits and a
 * value past that saturates by its sign, so this has NO limit step: it is what
 * the target computed wherever the 32-bit conversion was defined, extended by
 * saturation where it was not. */
static int ref_q8(float f, int32_t zp)
{
	float g;
	long long t, q;

	assert(!isnan(f));
	g = f + (f >= 0.0f ? 0.5f : -0.5f);
	if (g >= 4611686018427387904.0f)          /* 2^62 */
		return 127;
	if (g <= -4611686018427387904.0f)
		return -128;
	t = (long long)g;                          /* toward zero */
	q = t + zp;
	if (q < -128)
		q = -128;
	if (q > 127)
		q = 127;
	return (int)q;
}

static int ref_i8(uint8_t v, int norm_signed, float inv_scale, int32_t zp)
{
	float n = ref_f32(v, norm_signed);   /* VFMA.F32 */
	float f = n * inv_scale;             /* VMUL.F32 */

	return ref_q8(f, zp);
}

/* ---- admission -------------------------------------------------------------- */

static _Alignas(16) uint8_t buf[16];

static struct nn_tensor good_f32(void)
{
	struct nn_tensor t;

	memset(&t, 0, sizeof(t));
	t.data = buf;
	t.dtype = NN_DTYPE_FLOAT32;
	t.ndim = 4;
	t.dims[0] = 1;
	t.dims[1] = 128;
	t.dims[2] = 128;
	t.dims[3] = 3;
	t.bytes = 128u * 128u * 3u * 4u;
	t.scale = 0.0f;          /* f746/wio publish 0 for an unquantised tensor */
	t.zero_point = 0;
	return t;
}

static struct nn_tensor good_i8(void)
{
	struct nn_tensor t = good_f32();

	t.dtype = NN_DTYPE_INT8;
	t.bytes = 128u * 128u * 3u;
	t.scale = 1.0f / 128.0f;
	t.zero_point = 0;
	return t;
}

static void test_admit(void)
{
	struct nn_tensor t;
	static const uint8_t bad_dtypes[] = {
		NN_DTYPE_NONE, NN_DTYPE_UINT8, NN_DTYPE_INT16, NN_DTYPE_INT32, 6, 0xFF,
	};
	static const uint8_t bad_ranks[] = { 0, 1, 2, 3, 5 };
	static const uint16_t bad_batches[] = { 0, 2, 0xFFFF };
	static const uint16_t bad_channels[] = { 0, 2, 4, 0xFFFF };
	unsigned i;
	int dt;

	t = good_f32();
	assert(nn_input_admit(&t) == NN_INPUT_OK);
	t = good_i8();
	assert(nn_input_admit(&t) == NN_INPUT_OK);

	/* 1. absent */
	assert(nn_input_admit(NULL) == NN_INPUT_ERR_ABSENT);
	t = good_f32();
	t.data = NULL;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_ABSENT);

	/* 2. dtype */
	for (i = 0; i < sizeof(bad_dtypes); i++) {
		t = good_i8();
		t.dtype = bad_dtypes[i];
		assert(nn_input_admit(&t) == NN_INPUT_ERR_DTYPE);
	}

	/* 3. rank -- f746's old 3-dimensional input included */
	for (dt = 0; dt < 2; dt++)
		for (i = 0; i < sizeof(bad_ranks); i++) {
			t = dt ? good_i8() : good_f32();
			t.ndim = bad_ranks[i];
			assert(nn_input_admit(&t) == NN_INPUT_ERR_RANK);
		}

	/* 4. batch */
	for (i = 0; i < sizeof(bad_batches) / sizeof(bad_batches[0]); i++) {
		t = good_f32();
		t.dims[0] = bad_batches[i];
		assert(nn_input_admit(&t) == NN_INPUT_ERR_BATCH);
	}

	/* 5. empty height / width */
	t = good_f32();
	t.dims[1] = 0;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_EMPTY);
	t = good_f32();
	t.dims[2] = 0;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_EMPTY);

	/* 6. channels; 1 (red only) is admitted */
	for (i = 0; i < sizeof(bad_channels) / sizeof(bad_channels[0]); i++) {
		t = good_f32();
		t.dims[3] = bad_channels[i];
		assert(nn_input_admit(&t) == NN_INPUT_ERR_CHANNELS);
	}
	t = good_i8();
	t.dims[3] = 1;
	t.bytes = 128u * 128u;
	assert(nn_input_admit(&t) == NN_INPUT_OK);

	/* 7. int8 scale: a positive NORMAL float only */
	{
		const float refused[] = {
			0.0f, -0.0f, -1.0f, -FLT_MIN, NAN, -NAN, INFINITY, -INFINITY,
			FLT_MIN / 2.0f,                 /* subnormal */
			from_bits(0x00000001u),         /* smallest subnormal */
			from_bits(0x007FFFFFu),         /* largest subnormal */
		};
		const float admitted[] = {
			FLT_MIN,                        /* smallest normal */
			FLT_MAX, 1.0f / 128.0f, 1.0f / 255.0f, 1.0f, 1e-30f, 1e30f,
		};

		for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
			t = good_i8();
			t.scale = refused[i];
			assert(nn_input_admit(&t) == NN_INPUT_ERR_SCALE);
		}
		for (i = 0; i < sizeof(admitted) / sizeof(admitted[0]); i++) {
			t = good_i8();
			t.scale = admitted[i];
			assert(nn_input_admit(&t) == NN_INPUT_OK);
			/* ...and 1/scale is finite and non-zero, as the conversion
			   relies on. */
			assert(isfinite(1.0f / admitted[i]) && 1.0f / admitted[i] > 0.0f);
		}
		/* A float32 input's scale is not looked at. */
		t = good_f32();
		t.scale = NAN;
		assert(nn_input_admit(&t) == NN_INPUT_OK);
		t.scale = -1.0f;
		assert(nn_input_admit(&t) == NN_INPUT_OK);
	}

	/* 8. int8 zero point */
	{
		const int32_t refused[] = { -129, 128, INT32_MIN, INT32_MAX, 255, -255 };
		const int32_t admitted[] = { -128, -1, 0, 1, 127 };

		for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
			t = good_i8();
			t.zero_point = refused[i];
			assert(nn_input_admit(&t) == NN_INPUT_ERR_ZERO_POINT);
		}
		for (i = 0; i < sizeof(admitted) / sizeof(admitted[0]); i++) {
			t = good_i8();
			t.zero_point = admitted[i];
			assert(nn_input_admit(&t) == NN_INPUT_OK);
		}
		t = good_f32();
		t.zero_point = INT32_MAX;   /* not looked at for float32 */
		assert(nn_input_admit(&t) == NN_INPUT_OK);
	}

	/* 9. float32 buffer alignment: every misalignment refused; int8 is not
	   held to it */
	for (i = 1; i < 4; i++) {
		t = good_f32();
		t.data = buf + i;
		assert(nn_input_admit(&t) == NN_INPUT_ERR_ALIGN);
		t = good_i8();
		t.data = buf + i;
		assert(nn_input_admit(&t) == NN_INPUT_OK);
	}
	t = good_f32();
	t.data = buf + 4;
	assert(nn_input_admit(&t) == NN_INPUT_OK);
	/* ...checked before the byte count */
	t = good_f32();
	t.data = buf + 1;
	t.bytes = 0;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_ALIGN);

	/* 10. bytes: exact admitted, one short refused */
	t = good_f32();
	t.bytes -= 1u;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_BYTES);
	t = good_i8();
	t.bytes -= 1u;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_BYTES);
	t = good_i8();
	t.bytes += 1u;   /* a longer buffer is fine */
	assert(nn_input_admit(&t) == NN_INPUT_OK);

	/* 10. ...in 64 bits: a shape whose 32-bit product wraps below the buffer. */
	{
		uint32_t wrapped32;
		uint64_t real;

		t = good_f32();
		t.dims[1] = 65535;
		t.dims[2] = 65535;
		t.dims[3] = 3;
		real = (uint64_t)65535u * 65535u * 3u * 4u;
		wrapped32 = (uint32_t)65535u * 65535u * 3u * 4u;
		t.bytes = wrapped32;
		/* the test means something only if the 32-bit check would pass */
		assert((uint64_t)wrapped32 < real && wrapped32 <= t.bytes);
		assert(nn_input_admit(&t) == NN_INPUT_ERR_BYTES);
		t.bytes = UINT32_MAX;
		assert(nn_input_admit(&t) == NN_INPUT_ERR_BYTES);
	}

	/* ORDER: the first item that applies is the one returned. */
	t = good_i8();
	t.dtype = NN_DTYPE_UINT8;
	t.ndim = 3;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_DTYPE);
	t = good_i8();
	t.ndim = 3;
	t.dims[0] = 2;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_RANK);
	t = good_i8();
	t.dims[0] = 2;
	t.dims[1] = 0;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_BATCH);
	t = good_i8();
	t.dims[1] = 0;
	t.dims[3] = 2;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_EMPTY);
	t = good_i8();
	t.dims[3] = 2;
	t.scale = 0.0f;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_CHANNELS);
	t = good_i8();
	t.scale = 0.0f;
	t.zero_point = 200;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_SCALE);
	t = good_i8();
	t.zero_point = 200;
	t.bytes = 0;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_ZERO_POINT);
	t = good_i8();
	t.data = NULL;
	t.dtype = 0;
	assert(nn_input_admit(&t) == NN_INPUT_ERR_ABSENT);
}

static void test_names(void)
{
	const char *names[12];
	int code, i, j;

	for (code = 0; code >= -10; code--) {
		names[-code] = nn_input_refusal_name(code);
		assert(names[-code] != NULL && names[-code][0] != '\0');
		assert(strcmp(names[-code], "unknown") != 0);
	}
	names[11] = nn_input_refusal_name(-11);
	assert(strcmp(names[11], "unknown") == 0);
	assert(strcmp(nn_input_refusal_name(1), "unknown") == 0);
	assert(strcmp(nn_input_refusal_name(INT32_MIN), "unknown") == 0);
	/* no two codes share a name: none is folded into another */
	for (i = 0; i <= 10; i++)
		for (j = i + 1; j <= 10; j++)
			assert(strcmp(names[i], names[j]) != 0);
}

/* ---- conversion --------------------------------------------------------------- */

static unsigned n_checked;

static void test_f32(void)
{
	int norm, v;

	for (norm = 0; norm < 2; norm++)
		for (v = 0; v < 256; v++) {
			float got = nn_input_f32((uint8_t)v, norm);
			float want = ref_f32((uint8_t)v, norm);

			assert(bits(got) == bits(want));
			/* the range is [0,1] / [-1,1] to within the gain's rounding */
			assert(got >= (norm ? -1.0f : 0.0f));
			assert(got <= 1.0f + 2.0f * FLT_EPSILON);
			n_checked++;
		}
	/* the ends */
	assert(nn_input_f32(0, 0) == 0.0f);
	assert(nn_input_f32(0, 1) == -1.0f);
	assert(fabsf(nn_input_f32(255, 0) - 1.0f) <= FLT_EPSILON);
	assert(fabsf(nn_input_f32(255, 1) - 1.0f) <= FLT_EPSILON);
}

static void test_i8_sweep(void)
{
	const float scales[] = {
		1.0f / 128.0f, 1.0f / 255.0f, 1.0f / 127.5f, 0.0078125f, 0.003921569f,
		0.0039215689f, 0.00784314f, 0.02f, 0.1f, 0.5f, 1.0f, 2.0f, 1e-3f, 1e-6f,
		FLT_MIN,      /* 1/scale = 2^126: every non-zero value saturates */
		1e-30f, FLT_MAX, 1e30f,  /* 1/scale tiny: everything rounds to 0 */
	};
	const int32_t zps[] = { -128, -127, -64, -1, 0, 1, 64, 126, 127 };
	unsigned si, zi;
	int norm, v;

	for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++) {
		float inv = 1.0f / scales[si];

		assert(isfinite(inv));
		for (zi = 0; zi < sizeof(zps) / sizeof(zps[0]); zi++)
			for (norm = 0; norm < 2; norm++)
				for (v = 0; v < 256; v++) {
					int got = nn_input_i8((uint8_t)v, norm, inv, zps[zi]);
					int want = ref_i8((uint8_t)v, norm, inv, zps[zi]);

					if (got != want) {
						fprintf(stderr, "i8 mismatch: v=%d norm=%d "
						        "scale=%a zp=%d got=%d want=%d\n",
						        v, norm, (double)scales[si],
						        (int)zps[zi], got, want);
						assert(got == want);
					}
					n_checked++;
				}
	}

	/* Saturation at the extreme scales, whatever the zero point. */
	for (zi = 0; zi < sizeof(zps) / sizeof(zps[0]); zi++) {
		float tiny_scale_inv = 1.0f / FLT_MIN;
		float huge_scale_inv = 1.0f / FLT_MAX;

		assert(nn_input_i8(1, 0, tiny_scale_inv, zps[zi]) == 127);
		assert(nn_input_i8(255, 0, tiny_scale_inv, zps[zi]) == 127);
		assert(nn_input_i8(0, 1, tiny_scale_inv, zps[zi]) == -128);
		assert(nn_input_i8(0, 0, tiny_scale_inv, zps[zi]) == zps[zi]);
		assert(nn_input_i8(255, 1, huge_scale_inv, zps[zi]) == zps[zi]);
		assert(nn_input_i8(0, 1, huge_scale_inv, zps[zi]) == zps[zi]);
	}

	/* The common case: [-1,1] at scale 1/128, zero point 0 -> v - 128 within
	   one step (the gain's rounding), saturating at 127 for v = 255. */
	for (v = 0; v < 256; v++) {
		int got = nn_input_i8((uint8_t)v, 1, 128.0f, 0);
		int lin = v - 128;

		assert(got - lin <= 1 && lin - got <= 1);
	}
	assert(nn_input_i8(0, 1, 128.0f, 0) == -128);
	assert(nn_input_i8(255, 1, 128.0f, 0) == 127);
}

static void test_q8_edges(void)
{
	const int32_t zps[] = { -128, -127, -1, 0, 1, 126, 127 };
	const float over[] = {
		255.0f, 255.5f, 256.0f, 1e9f, 2147483648.0f, 3e9f, 1e30f, FLT_MAX,
		INFINITY,
	};
	unsigned zi, i;
	int k;
	float above = nextafterf(NN_INPUT_Q8_LIMIT, INFINITY);

	assert(NN_INPUT_Q8_LIMIT == 255.0f);
	for (zi = 0; zi < sizeof(zps) / sizeof(zps[0]); zi++) {
		int32_t zp = zps[zi];

		/* Beyond the limit, on both sides: the end of int8 whatever the
		   zero point -- which is the limit's whole justification. */
		for (i = 0; i < sizeof(over) / sizeof(over[0]); i++) {
			assert(nn_input_q8(over[i], zp) == 127);
			assert(nn_input_q8(-over[i], zp) == -128);
		}
		assert(nn_input_q8(above, zp) == 127);
		assert(nn_input_q8(-above, zp) == -128);
		/* NaN: limited to the lower end, never converted. */
		assert(nn_input_q8(NAN, zp) == -128);

		/* A sweep in quarter steps across [-300, 300] (both sides of the
		   limit) agrees with the reference that has no limit at all. */
		for (k = -1200; k <= 1200; k++) {
			float f = (float)k / 4.0f;

			assert(nn_input_q8(f, zp) == ref_q8(f, zp));
			n_checked++;
		}
		/* ...and at the float neighbours of every half-integer up to the
		   limit: the +-0.5 boundaries of round-half-away-from-zero. */
		for (k = -511; k <= 511; k += 2) {
			float h = (float)k / 2.0f;
			float lo = nextafterf(h, -INFINITY), hi = nextafterf(h, INFINITY);

			assert(nn_input_q8(h, zp) == ref_q8(h, zp));
			assert(nn_input_q8(lo, zp) == ref_q8(lo, zp));
			assert(nn_input_q8(hi, zp) == ref_q8(hi, zp));
			n_checked += 3;
		}
	}

	/* The limit is not tighter than it must be: just inside it, the zero point
	   at the far end still moves the result off the int8 end. */
	assert(nn_input_q8(254.0f, -128) == 126);
	assert(nn_input_q8(254.4f, -128) == 126);
	assert(nn_input_q8(254.5f, -128) == 127);
	assert(nn_input_q8(-254.0f, 127) == -127);
	assert(nn_input_q8(-254.5f, 127) == -128);

	/* Round half away from zero. */
	assert(nn_input_q8(0.0f, 0) == 0);
	assert(nn_input_q8(-0.0f, 0) == 0);
	assert(nn_input_q8(0.25f, 0) == 0);
	assert(nn_input_q8(0.5f, 0) == 1);
	assert(nn_input_q8(-0.5f, 0) == -1);
	assert(nn_input_q8(1.5f, 0) == 2);
	assert(nn_input_q8(2.5f, 0) == 3);
	assert(nn_input_q8(-2.5f, 0) == -3);
	assert(nn_input_q8(nextafterf(1.5f, 0.0f), 0) == 1);
	assert(nn_input_q8(nextafterf(-1.5f, 0.0f), 0) == -1);
	/* Plain saturation by the zero point inside the limit. */
	assert(nn_input_q8(100.0f, 100) == 127);
	assert(nn_input_q8(-100.0f, -100) == -128);
}

int main(void)
{
	volatile float tiny = FLT_MIN;

	/* The FPSCR reset defaults the firmware runs in: round to nearest, and
	   subnormals not flushed to zero. */
	assert(fesetround(FE_TONEAREST) == 0);
	assert(fegetround() == FE_TONEAREST);
	assert(tiny / 2.0f != 0.0f);

	test_admit();
	test_names();
	test_f32();
	test_i8_sweep();
	test_q8_edges();

	printf("test_nn_input_contract: all passed (%u conversions checked)\n",
	       n_checked);
	return 0;
}
