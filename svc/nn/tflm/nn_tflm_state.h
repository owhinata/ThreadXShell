/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_state.h
 * @brief   The tflm backend's state, and the facts the board gives it (issue #161).
 *
 * svc/nn/tflm/nn_tflm.cc is the one TFLM backend of wio-lite-ai and f746g-disco, and
 * like every shared translation unit it owns no mutable storage
 * (cmake/check_no_mutable_storage.py audits it per board, and a C++ global
 * constructor counts: it lands in .init_array).  The board therefore DEFINES the
 * state below as one named object, in boards/<board>/port/nn/tflm/nn_tflm_board.cc,
 * and the backend reaches it through the extern declaration here -- resolved at link
 * time, the way nn_backend_vt_selected and nn_model_board_state are.  Nothing is
 * registered at init: a pointer the shared file remembered would itself be mutable
 * storage in the shared file.  The arena and the model slots are not in it: they are
 * placed by the board in C (nn_tflm_mem.h says why).
 *
 * C++, because the state holds TFLM objects' storage, whose sizes only C++ knows.
 *
 * [!] NO DYNAMIC INITIALISATION.  The board's object is zero-initialised (.bss) and
 * nothing runs before main(): the interpreter and the op resolver are placement-new'd
 * into the raw storage below on first use.  A constructor at __libc_init_array time
 * would run before the MPU, the caches and the external memory exist.  The
 * static_asserts below are what keeps the TYPE that way; the audit (.init_array is
 * writable) and wio-lite-ai's check_cxx_runtime.py are what keep the IMAGE that way.
 *
 * [!] ONLY nn_tflm.cc AND THE BOARD'S DEFINITION INCLUDE THIS.
 */
#ifndef NN_TFLM_STATE_H
#define NN_TFLM_STATE_H

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

#include "nn.h"
#include "nn_tflm_ops.h"

#include <cstdint>
#include <type_traits>

/* The resolver the selected op profile needs, sized by the profile's own count
 * (nn_tflm_ops.h) rather than by a hand-written constant that could fall behind it. */
using nn_tflm_op_resolver = tflite::MicroMutableOpResolver<NN_TFLM_OPS_TOTAL>;

/* The model description handed out as the opaque backend handle. */
struct nn_tflm_model {
	struct nn_tensor in[NN_MAX_IO];
	struct nn_tensor out[NN_MAX_IO];
	int      n_in;
	int      n_out;
	uint32_t used;      /* arena_used_bytes() after AllocateTensors(); 0 with no model */
	const char *name;   /* always &nn_tflm_state::model_name[0] once open                */
	bool     open;
};

struct nn_tflm_state {
	/* The handle and the display name it points at (backend-owned, so `nn info`
	 * never prints through a pointer into a command's argv). */
	nn_tflm_model tm;
	char          model_name[64];

	/* The flatbuffer the live interpreter is reading, or nullptr when no model is
	 * loaded; `active_slot` is which nn_tflm_model_buf slot it lies in, and means
	 * something only while `active_model` is set (zero is the boot state of every
	 * field, so "none" is the null pointer, not a -1 a zeroed object cannot hold). */
	const void *active_model;
	uint32_t    active_len;
	uint32_t    active_slot;

	/* The interpreter, constructed in place on each successful build; nullptr when
	 * none exists.  ~MicroInterpreter() is not trivial, so a rebuild destroys the
	 * previous one first. */
	tflite::MicroInterpreter *interp;
	alignas(tflite::MicroInterpreter) uint8_t interp_buf[sizeof(tflite::MicroInterpreter)];

	/* The op resolver, constructed in place on first use and never destroyed;
	 * nullptr until it has been built whole. */
	nn_tflm_op_resolver *resolver;
	alignas(nn_tflm_op_resolver) uint8_t resolver_buf[sizeof(nn_tflm_op_resolver)];
};

static_assert(std::is_trivially_default_constructible<nn_tflm_state>::value,
              "the tflm state must be zero-initialised, never constructed at boot");
static_assert(std::is_trivially_destructible<nn_tflm_state>::value,
              "the tflm state must register no destructor (no atexit)");
static_assert(std::is_aggregate<nn_tflm_state>::value,
              "the tflm state is plain storage, not an object with a constructor");

/** What the board tells the backend about itself (beyond placement and state). */
struct nn_tflm_board_facts {
	/* Prepended to every loaded model's display name: f746g-disco's "sd:" (its
	 * models come from the SD card), "" on wio-lite-ai.  `(none)` is never prefixed. */
	const char *name_prefix;
};

extern "C" {
/** The board's one instance (boards/<board>/port/nn/tflm/nn_tflm_board.cc). */
extern nn_tflm_state nn_tflm_board_state;
/** The board's facts (same file). */
extern const struct nn_tflm_board_facts nn_tflm_board_facts;
}

#endif /* NN_TFLM_STATE_H */
