/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    verify_tflite.cc
 * @brief   Check a .tflite on the PC before sending it to the board
 * (owhinata/wio-lite-ai#9 P2c).
 *
 * A HOST tool.  It is never compiled into any firmware; it is built with the host
 * compiler against the SAME tflite-micro tree, at the SAME pinned SHA, that
 * cmake/tflite-micro.cmake already fetched for the `tflm` build.  That matters more
 * than anything else about it:
 *
 *   [!] IT RUNS tflite::VerifyModelBuffer() ITSELF, not an approximation of it.
 *
 * The firmware can be built with NN_TFLM_VERIFY=ON and pay 22,936 B of a 384 KB
 * partition to run that function on the board.  Running it here instead costs the
 * board nothing and catches the problem EARLIER -- before `blob write` erases a slot
 * and before a ~90 s YMODEM transfer -- with a real error message on a machine that
 * has a screen.  The on-board knob remains for models that arrive from somewhere other
 * than a PC you are sitting at.
 *
 * A hand-written "sanity check" would not have been an acceptable substitute, on the
 * PC any more than on the board.  Partial flatbuffer validation is worse than none: it
 * passes the malformed files it does not happen to cover while reporting that the
 * model was checked.  The only version of this worth having is the identical code.
 *
 * WHAT IT CHECKS, in the order the board would:
 *   1. length, and that it fits a blob slot payload AND the backend's staging buffer
 *   2. the "TFL3" file identifier at offset 4
 *   3. tflite::VerifyModelBuffer() -- the full structural walk, BOUNDED by the
 *      limits f746g-disco's and grove-vision-ai-v2's firmware (this board's builds it out)
 *      verify with: nn_tflm_verify_model_buffer(), svc/nn/tflm/nn_tflm_model_check.h
 *      (depth 16, 4096 table visits; issue #175)
 *   4. the schema version the runtime understands
 *   5. that the graph holds together (every subgraph's tensors, every operator's
 *      tensor lists, at most 64 long and naming tensors that exist, every
 *      tensor's quantization lengths and the operators' per-channel sum, every
 *      buffer index, the offline plan's header, the operator-code vector,
 *      every tensor's type, every constant's data length against its shape, no
 *      data on a tensor the graph writes, every operator's code index, and
 *      the operator rules this profile's operators have: CONV_2D,
 *      DEPTHWISE_CONV_2D and FULLY_CONNECTED's tensors fitting together),
 *      every tensor's rank, and that every
 *      DECLARED dimension of subgraph 0's I/O tensors fits a descriptor
 *      (0..65535) -- the FIRMWARE'S OWN check, svc/nn/tflm/nn_tflm_model_check.h,
 *      compiled in (issues #175, #178, #180, #181, #183, #185)
 *   6. [!] that every operator the model uses is one this FIRMWARE registered
 *
 * Step 6 is the one the board cannot do well.  TFLM's AllocateTensors() returns a
 * single kTfLiteError for "an operator is missing" and "the activations do not fit",
 * and with TF_LITE_STRIP_ERROR_STRINGS there is no message to tell them apart -- so
 * `nn model load` has to report both possibilities and let you guess.  Here the
 * missing operator is named.  The list comes from svc/nn/tflm/nn_tflm_ops.h, the same
 * header the firmware registers from, so the two cannot drift apart.
 *
 * It also prints the CRC-32/ISO-HDLC of the file, which is what `blob list` and
 * `nn model load` display, so the "is this the file I sent?" question is answered by
 * the same command.
 *
 * Build + run:  cmake --build build-tflm --target verify-model
 *               ./build-tflm/verify_tflite <model.tflite>
 */
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/schema/schema_utils.h"   /* GetBuiltinCode()          */
/* For TFLITE_SCHEMA_VERSION, taken from the runtime's own header rather than restated
 * here.  A restated constant is a constant that can fall out of step with the pinned
 * tree, and this tool's only reason to exist is that it cannot. */
#include "tensorflow/lite/micro/micro_interpreter.h"

#include "nn_tflm_ops.h"
/* The board's own check of the model's graph and I/O (issues #175, #178): the
 * function the tflm backend calls before it plans a model, so this gate and the
 * board reach the same verdict on the same bytes. */
#include "nn_tflm_model_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* Mirrors app/blob.h (BLOB_SLOT_SIZE - BLOB_HDR_SIZE) and
 * cmake/tflite-micro.cmake (NN_TFLM_MODEL_KB).  Restated rather than included
 * because those sources belong to the firmware's world; the numbers are checked against
 * their sources by the message this prints when a model is too big. */
static const uint32_t kBlobPayloadMax = 520192u;
static const uint32_t kStagingMax     = 512u * 1024u;

/* CRC-32/ISO-HDLC, bit by bit -- the same value FlashDB's fdb_calc_crc32() produces on
 * the board and Python's zlib.crc32() produces on this machine.  Written out rather
 * than linked from zlib so this tool builds with nothing but a C++ compiler. */
static uint32_t crc32_iso_hdlc(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
	}
	return ~crc;
}

static bool read_file(const char *path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "verify_tflite: cannot open %s\n", path);
		return false;
	}
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.insert(out.end(), buf, buf + n);
	bool ok = (ferror(f) == 0);
	fclose(f);
	if (!ok)
		fprintf(stderr, "verify_tflite: read error on %s\n", path);
	return ok;
}

/* The operators this firmware configuration registers, from the shared X-macro list. */
static const tflite::BuiltinOperator kRegistered[] = {
#define NN_TFLM_OP_ENUM(enum_suffix, method) tflite::BuiltinOperator_##enum_suffix,
	NN_TFLM_OPS_ALL(NN_TFLM_OP_ENUM)
#undef NN_TFLM_OP_ENUM
};

/*
 * Which NN_TFLM_OPS profile this checker was built for.  It is printed beside the
 * operator count, so it has to be derived from the SAME defines that select the list
 * above -- a label that says "blazeface" next to eleven registered operators is the
 * kind of confidently-wrong output this tool exists to prevent elsewhere.
 */
static const char *const kProfileName =
#if defined(NN_TFLM_OPS_EXTENDED)
	"extended";
#elif defined(NN_TFLM_OPS_MLPERF_PROFILE)
	"mlperf";
#else
	"blazeface";
#endif

/*
 * What to tell the operator when a model needs an operator this build did not
 * register: the next profile up, or -- when there is no next profile -- that widening
 * is not the answer.  `extended` is the widest one there is, so advising it from an
 * `extended` build would send someone to rebuild with the settings they already have.
 *
 * NULL means "no wider profile exists"; the REJECT message branches on it.
 */
static const char *const kNextProfile =
#if defined(NN_TFLM_OPS_EXTENDED)
	nullptr;
#elif defined(NN_TFLM_OPS_MLPERF_PROFILE)
	"extended";
#else
	"mlperf";
#endif

static bool is_registered(tflite::BuiltinOperator op)
{
	for (size_t i = 0; i < sizeof kRegistered / sizeof kRegistered[0]; i++)
		if (kRegistered[i] == op)
			return true;
	return false;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr,
		        "usage: verify_tflite <model.tflite>\n"
		        "  Checks a model against THIS firmware configuration before you send\n"
		        "  it with `blob write <slot>` + `sb <model.tflite>`.\n");
		return 2;
	}

	std::vector<uint8_t> buf;
	if (!read_file(argv[1], buf))
		return 2;

	const uint32_t len = (uint32_t)buf.size();
	printf("file    : %s\n", argv[1]);
	printf("size    : %u B\n", len);
	printf("crc32   : %08X   (compare with `blob list` / `nn model load`)\n",
	       crc32_iso_hdlc(buf.data(), buf.size()));

	/* 1. length ---------------------------------------------------------- */
	if (len < 8u) {
		printf("RESULT  : REJECT -- too short to be a flatbuffer (%u B)\n", len);
		return 1;
	}
	if (len > kBlobPayloadMax) {
		printf("RESULT  : REJECT -- %u B exceeds a blob slot payload (%u B).\n"
		       "          Widen BLOB_SLOT_SIZE in app/blob.h, or use a smaller model.\n",
		       len, kBlobPayloadMax);
		return 1;
	}
	if (len > kStagingMax) {
		printf("RESULT  : REJECT -- %u B exceeds the backend staging slot (%u B).\n"
		       "          Widen NN_TFLM_MODEL_KB in cmake/tflite-micro.cmake.\n",
		       len, kStagingMax);
		return 1;
	}

	/* 2. file identifier -------------------------------------------------- */
	if (!tflite::ModelBufferHasIdentifier(buf.data())) {
		printf("RESULT  : REJECT -- no \"TFL3\" identifier at offset 4; this is not a\n"
		       "          .tflite flatbuffer.\n");
		return 1;
	}

	/* 3. the structural walk, with the boards' limits (issue #175) --------- */
	if (!nn_tflm_verify_model_buffer(buf.data(), (size_t)len)) {
		printf("RESULT  : REJECT -- the flatbuffer is malformed, or nests deeper than %u\n"
		       "          or needs more than %u table visits (the boards' verifier\n"
		       "          limits, svc/nn/tflm/nn_tflm_model_check.h).\n",
		       NN_TFLM_VERIFY_MAX_DEPTH, NN_TFLM_VERIFY_MAX_TABLES);
		printf("          Malformed is what a file truncated on THIS machine looks like:\n"
		       "          the CRC32 above would still have matched on the board, because\n"
		       "          the transfer would have carried the damage faithfully.\n"
		       "          Check the source of the file (interrupted download, a\n"
		       "          converter still writing, a git-lfs pointer).\n");
		return 1;
	}

	printf("verify  : ok -- within depth %u / %u table visits (the boards' limits)\n",
	       NN_TFLM_VERIFY_MAX_DEPTH, NN_TFLM_VERIFY_MAX_TABLES);

	const tflite::Model *model = tflite::GetModel(buf.data());

	/* 4. schema version --------------------------------------------------- */
	if (model->version() != TFLITE_SCHEMA_VERSION) {
		printf("RESULT  : REJECT -- schema version %u, this runtime reads %d.\n",
		       (unsigned)model->version(), TFLITE_SCHEMA_VERSION);
		return 1;
	}

	/* 5. the graph's structure and ranks, the I/O dims (#175 .. #185) */
	switch (nn_tflm_model_io_check(model)) {
	case 0:
		break;
	case NN_MODEL_ERR_SHAPE:
		printf("RESULT  : REJECT -- an input or output list names outside 1..%d\n"
		       "          tensors.  The board refuses it as NN_MODEL_ERR_SHAPE (%d).\n",
		       NN_MAX_IO, NN_MODEL_ERR_SHAPE);
		return 1;
	case NN_MODEL_ERR_TENSOR:
		printf("RESULT  : REJECT -- a tensor declares more than %u dimensions, or an\n"
		       "          input/output tensor more than %d.  The board refuses it as\n"
		       "          NN_MODEL_ERR_TENSOR (%d).\n",
		       NN_TFLM_MAX_TENSOR_RANK, NN_MAX_DIMS, NN_MODEL_ERR_TENSOR);
		return 1;
	case NN_MODEL_ERR_DIMS:
		printf("RESULT  : REJECT -- an input/output tensor has a dimension outside\n"
		       "          0..65535, which a tensor descriptor (16 bits) cannot hold.\n"
		       "          The board refuses it as NN_MODEL_ERR_DIMS (%d).\n",
		       NN_MODEL_ERR_DIMS);
		return 1;
	default:
		printf("RESULT  : REJECT -- the graph does not hold together: subgraph 0 has\n"
		       "          no input/output list, a subgraph has no tensors, an\n"
		       "          operator's tensor list is missing, longer than %u, or names\n"
		       "          a tensor that does not exist, or a tensor's quantization\n"
		       "          lengths do not (a scale vector longer than %u, a zero-point\n"
		       "          vector neither absent, 1 long nor the scale's length, a\n"
		       "          per-channel quantized dimension outside the tensor's rank\n"
		       "          (rank >= 2) or 0..%u (rank 0 / 1), or the\n"
		       "          operators' scale lengths adding up past %u), a tensor or\n"
		       "          metadata entry names a buffer that does not exist, an\n"
		       "          OfflineMemoryAllocation plan has no data, no 12-byte\n"
		       "          header, a tensor count other than the graph's, fewer\n"
		       "          offsets than it claims or an offset below -1, there is\n"
		       "          no operator-code vector, a tensor has a type with no\n"
		       "          size, a constant's data is not exactly its shape's\n"
		       "          bytes (or it has a negative dimension or more than %u),\n"
		       "          a graph input / output or an operator output has data,\n"
		       "          an operator names an operator code that does not exist,\n"
		       "          or a CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED\n"
		       "          operator's tensors do not fit together (their ranks,\n"
		       "          the filter's or weights' channels against the output's,\n"
		       "          the depth multiplier, the bias's length and type).  The\n"
		       "          board refuses it as NN_MODEL_ERR_FORMAT (%d).\n",
		       NN_TFLM_MAX_OP_TENSORS, NN_TFLM_MAX_QUANT_CHANNELS,
		       NN_TFLM_MAX_TENSOR_RANK - 1u, NN_TFLM_MAX_QUANT_REFS,
		       NN_TFLM_MAX_TENSOR_RANK, NN_MODEL_ERR_FORMAT);
		return 1;
	}
	printf("io dims : ok -- every declared input/output dimension fits 0..65535\n");

	/* 6. operator coverage ------------------------------------------------ */
	std::vector<std::string> missing;
	const auto *codes = model->operator_codes();
	if (codes) {
		for (uint32_t i = 0; i < codes->size(); i++) {
			const tflite::OperatorCode *c = codes->Get(i);
			if (!c)
				continue;
			tflite::BuiltinOperator op = tflite::GetBuiltinCode(c);
			if (op == tflite::BuiltinOperator_CUSTOM) {
				missing.push_back(std::string("CUSTOM:") +
				                  (c->custom_code() ? c->custom_code()->str() : "?"));
			} else if (!is_registered(op)) {
				missing.push_back(tflite::EnumNameBuiltinOperator(op));
			}
		}
	}

	const auto *subgraphs = model->subgraphs();
	printf("schema  : version %u, %u subgraph(s), %u distinct operator(s)\n",
	       (unsigned)model->version(),
	       subgraphs ? (unsigned)subgraphs->size() : 0u,
	       codes ? (unsigned)codes->size() : 0u);
	printf("ops set : %d registered by this build (NN_TFLM_OPS=%s)\n",
	       (int)(sizeof kRegistered / sizeof kRegistered[0]), kProfileName);

	if (!missing.empty()) {
		printf("RESULT  : REJECT -- the model uses %zu operator(s) this build did not\n"
		       "          register:\n", missing.size());
		for (const std::string &m : missing)
			printf("            %s\n", m.c_str());
		printf("          On the board this would fail as NN_MODEL_ERR_ARENA, whose\n"
		       "          message cannot tell a missing operator from an arena that is\n"
		       "          too small -- which is exactly why this check is here.\n");
		if (kNextProfile != nullptr)
			printf("          Rebuild with -DNN_TFLM_OPS=%s, or re-export the model.\n",
			       kNextProfile);
		else
			printf("          This is already the widest NN_TFLM_OPS profile, so the\n"
			       "          operator has to be added to nn_tflm_ops.h or the model\n"
			       "          re-exported without it.\n");
		return 1;
	}

	/*
	 * Deliberately narrow wording.  What was established is: the flatbuffer is
	 * structurally sound, the schema version is one this runtime reads, the graph's
	 * DECLARED I/O shapes pass the board's own pre-planning check (the planned
	 * ones, which a kernel's Prepare() could change, are not seen here), and every
	 * BUILTIN OPERATOR CODE it references is in the registered set; of the kernels'
	 * shape / type constraints, the walk's operator rules (issue #185) hold only
	 * CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED's filter, weights, bias and
	 * output together.  What was NOT established, and cannot be from here, is that
	 * each kernel's Prepare() accepts the rest of this model's tensor shapes and
	 * types, that the operator VERSIONS are
	 * ones these kernels implement, or that the activations fit the arena -- all three
	 * are settled by AllocateTensors() on the board, against the real arena size.
	 *
	 * Saying "safe to send" would overstate that, and an overstated pass is the same
	 * failure this whole tool exists to avoid: a check that reports more confidence
	 * than it earned.
	 */
	printf("RESULT  : PASS -- structurally valid, schema version %d, declared I/O\n"
	       "          shapes fit 0..65535, convolution / fully connected tensors\n"
	       "          fit together, and no unregistered builtin operators.\n"
	       "          Remaining checks happen on the board when `nn model load`\n"
	       "          calls AllocateTensors(): the planned I/O shapes, operator\n"
	       "          versions, each kernel's other shape/type constraints, and\n"
	       "          whether the activations fit the arena.\n", TFLITE_SCHEMA_VERSION);
	return 0;
}
