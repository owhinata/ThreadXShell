/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_ethosu_state.h
 * @brief   The Ethos-U backend's state, and the facts the board gives it
 *          (issue #169).
 *
 * svc/nn/ethosu/nn_ethosu.cc owns no mutable storage, like every shared
 * translation unit (cmake/check_no_mutable_storage.py audits it, and a C++ global
 * constructor counts).  The board DEFINES the state below as one named object
 * (grove-vision-ai-v2: port/nn/npu/nn_ethosu_board.cc) and the backend reaches it
 * through the extern declaration here, resolved at link time -- the shape
 * svc/nn/tflm/nn_tflm_state.h has for the tflm backend.
 *
 * C++, because the state holds TFLM objects' storage, whose sizes only C++ knows.
 * Zero-initialised and never constructed at boot: the interpreter and the op
 * resolver are placement-new'd into the raw storage below on first use, after the
 * caches and the NPU exist.
 *
 * [!] THE OP PROFILE IS PART OF THE TYPE.  The resolver's size is the selected
 * list's count (nn_tflm_ops.h), so a translation unit that included this with a
 * different profile would lay the object out differently from the one that defines
 * it.  Every includer therefore needs NN_TFLM_OPS_ETHOSU_PROFILE, and the header
 * refuses to compile without it.
 */
#ifndef NN_ETHOSU_STATE_H
#define NN_ETHOSU_STATE_H

#ifndef NN_TFLM_OPS_ETHOSU_PROFILE
#error "the Ethos-U backend's state needs NN_TFLM_OPS_ETHOSU_PROFILE=1 (the board's cmake)"
#endif

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

#include "nn.h"
#include "nn_tflm_ops.h"

#include <cstdint>
#include <type_traits>

/* One operator, the ethos-u custom op (nn_tflm_ops.h's ethosu profile). */
using nn_ethosu_op_resolver = tflite::MicroMutableOpResolver<NN_TFLM_OPS_TOTAL>;

/* The model description handed out as the opaque backend handle: the same members
 * nn_tflm_glue.h publishes into. */
struct nn_ethosu_model {
	struct nn_tensor in[NN_MAX_IO];
	struct nn_tensor out[NN_MAX_IO];
	int      n_in;
	int      n_out;
	uint32_t used;      /* arena_used_bytes() after AllocateTensors(); 0 with no model */
	const char *name;   /* always &nn_ethosu_state::model_name[0] once open           */
	bool     open;
};

struct nn_ethosu_state {
	/* The handle and the display name it points at. */
	nn_ethosu_model tm;
	char            model_name[64];

	/* The flatbuffer the live interpreter is reading IN PLACE -- inside the
	 * board's read window, kept there by the caller's lease (nn.h) -- or nullptr
	 * when no model is loaded. */
	const void *active_model;
	uint32_t    active_len;

	/* The interpreter, constructed in place on each successful build; nullptr when
	 * none exists. */
	tflite::MicroInterpreter *interp;
	alignas(tflite::MicroInterpreter) uint8_t interp_buf[sizeof(tflite::MicroInterpreter)];

	/* The op resolver, constructed in place on first use and never destroyed;
	 * nullptr until it has been built whole. */
	nn_ethosu_op_resolver *resolver;
	alignas(nn_ethosu_op_resolver) uint8_t resolver_buf[sizeof(nn_ethosu_op_resolver)];
};

static_assert(std::is_trivially_default_constructible<nn_ethosu_state>::value,
              "the ethosu state must be zero-initialised, never constructed at boot");
static_assert(std::is_trivially_destructible<nn_ethosu_state>::value,
              "the ethosu state must register no destructor (no atexit)");
static_assert(std::is_aggregate<nn_ethosu_state>::value,
              "the ethosu state is plain storage, not an object with a constructor");

/** What the board tells the backend about itself. */
struct nn_ethosu_board_facts {
	/* Prepended to every loaded model's display name ("" for blob names).  `(none)`
	 * is never prefixed. */
	const char *name_prefix;
	/* The memory-mapped read window a model must lie wholly inside: the board's
	 * one statement of it (grove-vision-ai-v2: nor_flash.h's NOR_XIP_BASE and
	 * NOR_SIZE).  The backend holds no address of its own. */
	uint32_t window_base;
	uint32_t window_size;
};

extern "C" {
/** The board's one instance. */
extern nn_ethosu_state nn_ethosu_board_state;
/** The board's facts (same file). */
extern const struct nn_ethosu_board_facts nn_ethosu_board_facts;
}

#endif /* NN_ETHOSU_STATE_H */
