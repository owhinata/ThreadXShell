/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    verify_vela_model.cc
 * @brief   Inspect a Vela-compiled model before it is flashed (issue #45).
 *
 * A HOST tool.  It answers, on the developer's machine, the questions the board
 * would otherwise answer by failing: with one kTfLiteError, over a serial line,
 * after a flash cycle of an external NOR with ~100k of them.
 *
 * WHY "CPU operators = 0" FROM VELA IS NOT ENOUGH
 *
 * That line says the whole graph was offloaded.  It says nothing about whether
 * THIS firmware can run the result, and the gap is where every interesting
 * failure lives:
 *
 *   - the firmware refuses a payload whose driver actions continue past the
 *     command stream (issue #46), because the driver can abandon a launch with
 *     the arena still owned by the NPU.  So this tool runs THE FIRMWARE'S OWN
 *     walk -- svc/nn/ethosu/npu_payload.c is compiled in, not reimplemented -- over
 *     the same bytes the driver will parse.
 *   - AllocateTensors() has to fit the model in the arena the board reserves.
 *     So this tool compiles THE BOARD'S OWN reservation -- port/nn/npu/npu_arena.c
 *     -- and plans into it.  "384.86 KiB" from Vela's summary is the NPU's SRAM
 *     figure, which is a different number from what TFLM's planner consumes.
 *   - The decoder locates outputs by shape, so the shapes and their
 *     quantisation are the model's contract with the firmware, not a detail.
 *
 * WHAT IT DOES NOT PROVE.  The command stream's compatibility with the silicon
 * is checked by the driver at INVOKE time, against registers this host does not
 * have.  The optimizer config word is printed here so it can be compared with a
 * model already known to run, but printing is all it is -- the first `nn detect`
 * on hardware is what proves it.
 *
 * Build + run:
 *   cmake --build build/grove-vision-ai-v2 --target model-tools
 *   ./build/grove-vision-ai-v2/verify_vela_model <model_vela.tflite> [--blazeface]
 */
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/schema/schema_utils.h"   /* GetBuiltinCode() */
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

/* The firmware's own payload scan: which bytes the driver parses, and whether
 * they are safe to launch.  Compiled in, not reimplemented -- npu_model_scan.h
 * says why the LOCATOR matters as much as the walk. */
#include "npu_model_scan.h"

/* And the firmware's own bounded flatbuffer check (issue #93; its limits are
 * svc/nn/tflm/nn_tflm_model_check.h's since issue #175).  This tool used to
 * construct a flatbuffers::Verifier with the DEFAULT limits, looser than the
 * board's -- so a model could pass here and be refused by the firmware's
 * backend with NN_MODEL_ERR_FORMAT, on the serial line, after the erase.
 * A host gate that accepts what the device rejects has moved the failure rather
 * than caught it. */
#include "npu_verify.h"

#include "npu_arena.h"     /* npu_arena_base() / npu_arena_bytes() */

/* The op list the firmware's backend registers (svc/nn/tflm/nn_tflm_ops.h, its
 * ethosu profile -- issue #169).  The resolver below is built from it, so the two
 * cannot register different operators. */
#include "nn_tflm_ops.h"

/* And the I/O rule the backend applies once the tensors are planned
 * (svc/nn/ethosu/nn_ethosu_io.h, issue #169): the counts and the tensors the
 * firmware refuses with NN_MODEL_ERR_SHAPE / NN_MODEL_ERR_TENSOR. */
#include "nn_ethosu_io.h"

/* And the graph walk both backends apply to the model before planning it
 * (svc/nn/tflm/nn_tflm_model_check.h, issues #175 .. #181, #183, #185): a graph
 * that does not hold together -- an I/O list, a subgraph's tensors or an
 * operator's tensor list missing, an operator list longer than
 * NN_TFLM_MAX_OP_TENSORS, an index naming a tensor that does not exist, a
 * quantization vector too long or inconsistent (Vela output carries no
 * per-channel quantization, so that part never bites a Vela model), a buffer
 * index past the buffer vector, an OfflineMemoryAllocation plan that does not
 * match the graph (every Vela model carries one, so that part runs on each), no
 * operator-code vector, a tensor type with no size, a constant whose data is
 * not exactly its shape's bytes (the command stream and the weights of every
 * Vela model are exact), data on a graph input / output or an operator output,
 * an operator naming an operator code that does not exist (the operator rules
 * of issue #185 are empty under this profile: it registers no CPU kernel)
 * (NN_MODEL_ERR_FORMAT on the board) -- a tensor of
 * too high a rank (NN_MODEL_ERR_TENSOR), or an I/O dimension outside 0..65535
 * (NN_MODEL_ERR_DIMS). */
#include "nn_tflm_model_check.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/*
 * The BlazeFace output contract, as the decoder reads it.
 *
 * port/npu/models/blazeface.c locates its four tensors BY SHAPE, so these are
 * the shapes it will look for; a model that does not carry all four would make
 * blazeface_decode() return -1 on the board and say nothing about why.  The
 * anchor counts come from the MediaPipe BlazeFace-front layout (16x16 grid with
 * 2 anchors per cell = 512, 8x8 grid with 6 = 384) and the 16 channels are
 * 4 box regressors plus 6 keypoints.
 */
struct expected_output {
	int32_t     anchors;
	int32_t     chan;
	const char *what;
};

const expected_output kBlazeFaceOutputs[] = {
	{ 512, 16, "box regressors, 16x16 grid" },
	{ 512,  1, "scores,        16x16 grid"  },
	{ 384, 16, "box regressors, 8x8 grid"   },
	{ 384,  1, "scores,         8x8 grid"   },
};

int g_fail;

/* How many of @p outs have this exact 1 x anchors x chan shape.  Split out so
 * that the check and the notice below cannot come to disagree about what the
 * decoder's contract IS -- the reason npu_model_scan.h gives for sharing the
 * locator with the firmware, applied to the two branches of this one tool. */
unsigned count_matching(const std::vector<const tflite::Tensor *> &outs,
                        const expected_output &x);

/* Does @p outs satisfy the decoder's contract exactly: four outputs, each of
 * the four shapes present once? */
bool blazeface_contract_holds(const std::vector<const tflite::Tensor *> &outs);

void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fail(const char *fmt, ...)
{
	va_list ap;

	fputs("  FAIL   ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	g_fail++;
}

bool read_file(const char *path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path, "rb");

	if (f == nullptr) {
		fprintf(stderr, "verify_vela_model: cannot open %s\n", path);
		return false;
	}

	uint8_t buf[65536];
	size_t n;

	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.insert(out.end(), buf, buf + n);

	bool ok = (ferror(f) == 0);

	fclose(f);
	if (!ok)
		fprintf(stderr, "verify_vela_model: read error on %s\n", path);
	return ok;
}

void print_shape(const tflite::Tensor *t)
{
	const auto *s = t->shape();

	printf("[");
	for (uint32_t i = 0; s != nullptr && i < s->size(); i++)
		printf("%s%d", i ? "x" : "", s->Get(i));
	printf("]");
}

/* The subgraph shape as a pair, for the BlazeFace contract: 1 x anchors x chan.
 * Returns false for anything that is not rank 3 with a leading 1. */
bool as_anchor_shape(const tflite::Tensor *t, int32_t *anchors, int32_t *chan)
{
	const auto *s = t->shape();

	if (s == nullptr || s->size() != 3u || s->Get(0) != 1)
		return false;
	*anchors = s->Get(1);
	*chan    = s->Get(2);
	return true;
}

unsigned count_matching(const std::vector<const tflite::Tensor *> &outs,
                        const expected_output &x)
{
	unsigned matches = 0;

	for (const tflite::Tensor *t : outs) {
		int32_t a = 0, c = 0;

		if (as_anchor_shape(t, &a, &c) && a == x.anchors && c == x.chan)
			matches++;
	}
	return matches;
}

bool blazeface_contract_holds(const std::vector<const tflite::Tensor *> &outs)
{
	const size_t want = sizeof kBlazeFaceOutputs / sizeof kBlazeFaceOutputs[0];

	if (outs.size() != want)
		return false;
	for (size_t e = 0; e < want; e++) {
		if (count_matching(outs, kBlazeFaceOutputs[e]) != 1u)
			return false;
	}
	return true;
}

void print_quant(const tflite::Tensor *t)
{
	const auto *q = t->quantization();

	if (q == nullptr || q->scale() == nullptr || q->scale()->size() == 0u) {
		printf("  (not quantised)");
		return;
	}
	if (q->scale()->size() != 1u) {
		printf("  per-axis, %u scales", q->scale()->size());
		return;
	}
	printf("  scale %.9g  zp %ld", static_cast<double>(q->scale()->Get(0)),
	       q->zero_point() != nullptr && q->zero_point()->size() > 0u
	           ? static_cast<long>(q->zero_point()->Get(0)) : 0L);
}

/*
 * Decode and print the OPTIMIZER_CONFIG action, if the payload carries one.
 *
 * REPORTING ONLY -- the safety property is npu_payload.c's and is checked
 * separately.  The layout is the driver's config_r / id_r (ethosu55_interface.h)
 * and the action encoding is ethosu_driver.c's; both are restated here for the
 * same reason npu_payload.c restates them, and for the weaker purpose.
 */
void print_optimizer_config(const uint8_t *data, size_t bytes)
{
	const uint32_t *w = reinterpret_cast<const uint32_t *>(data);
	const uint32_t total = static_cast<uint32_t>(bytes / 4u);

	for (uint32_t i = 1u; i + 2u < total; i++) {
		if ((w[i] & 0xFFu) != 1u)      /* ACTION_OPTIMIZER_CFG */
			continue;

		const uint32_t cfg = w[i + 1u];
		const uint32_t id  = w[i + 2u];

		printf("  optimizer config  0x%08x  macs/cc %u (2^%u = %u MACs)  "
		       "cmd_stream_version %u  shram %u KB  custom_dma %u\n",
		       cfg, (cfg >> 0) & 0xFu, (cfg >> 0) & 0xFu,
		       1u << ((cfg >> 0) & 0xFu), (cfg >> 4) & 0xFu,
		       (cfg >> 8) & 0xFFu, (cfg >> 27) & 0x1u);
		printf("  arch version      %u.%u.%u  (the driver compares major and "
		       "minor; patch is ignored)\n",
		       (id >> 28) & 0xFu, (id >> 20) & 0xFFu, (id >> 16) & 0xFu);
		return;
	}
	printf("  optimizer config  none in the payload\n");
}

}  /* namespace */

/*
 * The Ethos-U driver entry points ethosu.cc's Eval() calls.  Never reached: this
 * tool allocates tensors and never invokes.  They exist so the kernel links on a
 * host that has no NPU, and they abort rather than return so that "never
 * reached" is enforced instead of assumed.
 */
extern "C" {

struct ethosu_driver;

struct ethosu_driver *ethosu_reserve_driver(void)
{
	fprintf(stderr, "verify_vela_model: the host build must not invoke\n");
	abort();
}

void ethosu_release_driver(struct ethosu_driver *drv)
{
	(void)drv;
	fprintf(stderr, "verify_vela_model: the host build must not invoke\n");
	abort();
}

int ethosu_invoke_v3(struct ethosu_driver *drv, const void *cms, size_t cms_bytes,
                     uint64_t *base_addrs, size_t *base_addrs_size,
                     int num_tensors, void *ctx)
{
	(void)drv; (void)cms; (void)cms_bytes; (void)base_addrs;
	(void)base_addrs_size; (void)num_tensors; (void)ctx;
	fprintf(stderr, "verify_vela_model: the host build must not invoke\n");
	abort();
}

}  /* extern "C" */

/*
 * The backend's I/O rule, on the interpreter planned against the board's arena:
 * the same functions the firmware calls after AllocateTensors() (nn_ethosu_io.h),
 * so a model this passes is not refused on the board as NN_MODEL_ERR_SHAPE or
 * NN_MODEL_ERR_TENSOR after the erase and the transfer -- and the backend's
 * backstop on the planned dims (nn_tflm_tensor_dims_fit(), NN_MODEL_ERR_DIMS),
 * behind the check of the model itself that main() ran before planning.
 */
static void check_planned_io(tflite::MicroInterpreter &interp)
{
	const size_t n_in  = interp.inputs_size();
	const size_t n_out = interp.outputs_size();
	const int before   = g_fail;

	if (!nn_ethosu_io_count_ok(n_in) || !nn_ethosu_io_count_ok(n_out)) {
		fail("%zu inputs and %zu outputs; the firmware takes 1..%d of each "
		     "(NN_MODEL_ERR_SHAPE on the board)\n", n_in, n_out, NN_MAX_IO);
		return;
	}
	for (size_t i = 0; i < n_in + n_out; i++) {
		const bool is_in = i < n_in;
		const size_t k   = is_in ? i : i - n_in;
		const TfLiteTensor *t = is_in ? interp.input(k) : interp.output(k);

		if (!nn_ethosu_tensor_ok(t)) {
			fail("%s %zu has no buffer or a rank above %d "
			     "(NN_MODEL_ERR_TENSOR on the board)\n",
			     is_in ? "input" : "output", k, NN_MAX_DIMS);
			continue;
		}
		if (!nn_tflm_tensor_dims_fit(t))
			fail("%s %zu has a planned dimension outside 0..65535 "
			     "(NN_MODEL_ERR_DIMS on the board)\n",
			     is_in ? "input" : "output", k);
	}
	if (g_fail == before)
		printf("  ok     %zu input(s), %zu output(s), every one describable "
		       "(the firmware's I/O rule)\n", n_in, n_out);
}

int main(int argc, char **argv)
{
	const char *path = nullptr;
	bool blazeface = false;
	bool board_order = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--blazeface") == 0)
			blazeface = true;
		else if (strcmp(argv[i], "--board-order") == 0)
			board_order = true;
		else if (path == nullptr)
			path = argv[i];
		else
			path = nullptr;
	}
	if (path == nullptr) {
		fprintf(stderr,
		        "usage: verify_vela_model <model_vela.tflite> [--blazeface] "
		        "[--board-order]\n"
		        "  Checks that this firmware can run the model: one subgraph,\n"
		        "  ethos-u operators only, a payload the firmware will accept, int8\n"
		        "  I/O, an offline memory plan, and a layout that fits the arena\n"
		        "  the board reserves.  --blazeface also pins the four output\n"
		        "  shapes the decoder looks for -- it ADDS checks rather than\n"
		        "  naming a model, so without it a detector still passes, and\n"
		        "  says so.\n"
		        "  --board-order runs the firmware's payload scan before the graph\n"
		        "  walk, the board's order, so the first refusal printed is the\n"
		        "  one the board gives.  By default the walk is reported first,\n"
		        "  because this tool exists to diagnose.\n");
		return 2;
	}

	std::vector<uint8_t> buf;

	if (!read_file(path, buf))
		return 2;

	printf("model    : %s (%zu B)\n", path, buf.size());

	/* --- the flatbuffer itself ------------------------------------------- */

	if (buf.size() < NPU_MODEL_MIN_BYTES ||
	    !tflite::ModelBufferHasIdentifier(buf.data())) {
		printf("  FAIL   not a .tflite flatbuffer (no \"TFL3\" identifier, "
		       "or shorter than %u B)\n", NPU_MODEL_MIN_BYTES);
		printf("RESULT   : REJECT\n");
		return 1;
	}
	if (!npu_verify_model_buffer(buf.data(), buf.size())) {
		printf("  FAIL   the flatbuffer is malformed, or nests deeper than %u "
		       "or needs more than %u table visits (the firmware's limits)\n",
		       NN_TFLM_VERIFY_MAX_DEPTH, NN_TFLM_VERIFY_MAX_TABLES);
		printf("RESULT   : REJECT\n");
		return 1;
	}

	const tflite::Model *model = tflite::GetModel(buf.data());

	if (model->version() != TFLITE_SCHEMA_VERSION) {
		printf("  FAIL   schema version %u, this tree reads %d\n",
		       static_cast<unsigned>(model->version()), TFLITE_SCHEMA_VERSION);
		printf("RESULT   : REJECT\n");
		return 1;
	}
	printf("  ok     flatbuffer verifies within the firmware's limits "
	       "(depth %u, %u tables), schema version %d\n",
	       NN_TFLM_VERIFY_MAX_DEPTH, NN_TFLM_VERIFY_MAX_TABLES, TFLITE_SCHEMA_VERSION);

	/*
	 * [!] THE VERDICT IS THE FIRMWARE'S, NOT A COPY OF IT.
	 * npu_model_payload_refusal() is the function the firmware's backend calls, compiled
	 * into this tool.  It reaches the same answer on the same bytes by
	 * construction, which a reimplementation here could not promise -- and the
	 * half that is easy to get wrong is not the walk but WHICH BYTES, since
	 * the payload is input tensor 0 and not the operator's custom_options.
	 *
	 * [!] A REFUSAL STOPS THE RUN, as it stops the firmware's load (issue #179).
	 * The report after it walks every operator's payload again, so going on past
	 * a refusal would hand it the very shared-stream model the scan's length
	 * charge refuses, and the gate would take as long as the board used to.  It
	 * is passed the file's length, the length the board is given.
	 *
	 * Where it runs: by default after the graph walk and the operator-code
	 * report (this tool diagnoses, and the walk proves the lists it reads index
	 * real tensors); with --board-order right here, after the schema version and
	 * before the walk -- nn_ethosu.cc's order, so the first refusal is the board's.
	 */
	auto payload_scan = [&]() -> bool {
		const char *refusal = npu_model_payload_refusal(model, buf.size());

		if (refusal != nullptr) {
			fail("the firmware would refuse this model (NN_MODEL_ERR_PAYLOAD): %s\n",
			     refusal);
			printf("RESULT   : REJECT\n");
			return false;
		}
		printf("  ok     the firmware's payload scan accepts every operator\n");
		return true;
	};

	if (board_order && !payload_scan())
		return 1;

	/* The firmware runs this after its payload walk and before its resolver; it
	 * runs here first because everything below follows subgraph 0's I/O lists,
	 * and this is the check that proves they index real tensors.  A model this
	 * refuses is refused on the board too -- as NN_MODEL_ERR_PAYLOAD instead, if
	 * its payload is also one the firmware refuses (that check comes first there). */
	switch (nn_tflm_model_io_check(model)) {
	case 0:
		printf("  ok     every input/output dimension fits 0..65535 "
		       "(the firmware's I/O dims rule)\n");
		break;
	case NN_MODEL_ERR_SHAPE:
		printf("  FAIL   an input or output list names outside 1..%d tensors "
		       "(NN_MODEL_ERR_SHAPE on the board)\n", NN_MAX_IO);
		printf("RESULT   : REJECT\n");
		return 1;
	case NN_MODEL_ERR_TENSOR:
		printf("  FAIL   a tensor declares more than %u dimensions, or an "
		       "input/output tensor more than %d (NN_MODEL_ERR_TENSOR on the "
		       "board)\n", NN_TFLM_MAX_TENSOR_RANK, NN_MAX_DIMS);
		printf("RESULT   : REJECT\n");
		return 1;
	case NN_MODEL_ERR_DIMS:
		printf("  FAIL   an input/output tensor has a dimension outside 0..65535, "
		       "which a tensor descriptor cannot hold (NN_MODEL_ERR_DIMS on the "
		       "board)\n");
		printf("RESULT   : REJECT\n");
		return 1;
	default:
		printf("  FAIL   the graph does not hold together: subgraph 0 has no "
		       "input/output list, a subgraph has no tensors, an operator's "
		       "tensor list is missing, longer than %u, or names a tensor that "
		       "does not exist, or a tensor's quantization lengths do not (a "
		       "scale vector longer than %u, a zero-point vector neither absent, "
		       "1 long nor the scale's length, a per-channel quantized dimension "
		       "outside the tensor's rank (rank >= 2) or 0..%u (rank 0 / 1), or "
		       "the operators' scale lengths "
		       "adding up past %u), a tensor or metadata entry names a buffer "
		       "that does not exist, an OfflineMemoryAllocation plan has no "
		       "data, no 12-byte header, a tensor count other than the graph's, "
		       "fewer offsets than it claims or an offset below -1, there is "
		       "no operator-code vector, a tensor has a type with no size, a "
		       "constant's data is not exactly its shape's bytes (or it has a "
		       "negative dimension or more than %u), a graph input / output "
		       "or an operator output has data, or an operator names an operator "
		       "code that does not exist (NN_MODEL_ERR_FORMAT on the board)\n",
		       NN_TFLM_MAX_OP_TENSORS, NN_TFLM_MAX_QUANT_CHANNELS,
		       NN_TFLM_MAX_TENSOR_RANK - 1u, NN_TFLM_MAX_QUANT_REFS,
		       NN_TFLM_MAX_TENSOR_RANK);
		printf("RESULT   : REJECT\n");
		return 1;
	}

	/* Everything past here needs a subgraph, so a bad shape stops the run
	 * rather than being reported alongside checks that could not be made. */
	const auto *subgraphs = model->subgraphs();

	if (subgraphs == nullptr || subgraphs->size() != 1u) {
		printf("  FAIL   %u subgraphs; the firmware accepts exactly one\n",
		       subgraphs != nullptr ? subgraphs->size() : 0u);
		printf("RESULT   : REJECT\n");
		return 1;
	}

	const tflite::SubGraph *sub = subgraphs->Get(0);
	const auto *tensors = sub->tensors();
	const auto *ops     = sub->operators();
	const auto *codes   = model->operator_codes();

	if (tensors == nullptr || ops == nullptr || ops->size() == 0u ||
	    codes == nullptr) {
		printf("  FAIL   the subgraph has no tensors or no operators\n");
		printf("RESULT   : REJECT\n");
		return 1;
	}
	printf("  ok     1 subgraph, %u tensors, %u operators\n",
	       tensors->size(), ops->size());

	/* --- every operator is the one this port registers -------------------- */

	unsigned ethosu_ops = 0;

	for (uint32_t n = 0; n < ops->size(); n++) {
		const auto *op = ops->Get(n);
		const uint32_t ci = op != nullptr ? op->opcode_index() : 0xFFFFFFFFu;

		if (op == nullptr || ci >= codes->size()) {
			fail("operator %u names operator code %u, which does not exist\n",
			     n, ci);
			continue;
		}

		const auto *code = codes->Get(ci);

		if (code == nullptr ||
		    tflite::GetBuiltinCode(code) != tflite::BuiltinOperator_CUSTOM) {
			fail("operator %u is %s, not a custom operator.  This port "
			     "registers AddEthosU() and nothing else, so a CPU kernel "
			     "would fail AllocateTensors on the board.\n", n,
			     code != nullptr
			         ? tflite::EnumNameBuiltinOperator(tflite::GetBuiltinCode(code))
			         : "?");
			continue;
		}

		const auto *custom = code->custom_code();

		if (custom == nullptr || strcmp(custom->c_str(), "ethos-u") != 0) {
			/* The name is printed to at most 64 bytes: thousands of entries
			 * may name one operator code whose name is most of the file
			 * (issue #179), and this line is printed per entry. */
			const bool cut = custom != nullptr && custom->size() > 64u;

			fail("operator %u is custom \"%.64s%s\", not \"ethos-u\"\n", n,
			     custom != nullptr ? custom->c_str() : "<unnamed>",
			     cut ? "..." : "");
			continue;
		}
		ethosu_ops++;
	}
	if (ethosu_ops == ops->size())
		printf("  ok     every operator is the ethos-u custom operator (%u)\n",
		       ethosu_ops);

	/* --- the payload the firmware will walk ------------------------------ */

	/* Already run under --board-order (and a refusal returned there). */
	if (!board_order && !payload_scan())
		return 1;

	/* Reporting only, and it uses the same locator, so what is printed is what
	 * the driver will be handed.  Reached only when the scan accepted, so the
	 * payloads add up to at most the file's length and so does this walk. */
	for (uint32_t n = 0; n < ops->size(); n++) {
		const auto *op = ops->Get(n);
		size_t bytes = 0;
		const char *why = nullptr;
		const uint8_t *data;

		if (op == nullptr)
			continue;
		data = npu_model_command_stream(model, sub, op, &bytes, &why);
		if (data == nullptr)
			continue;              /* already reported by the refusal above */

		printf("  ok     operator %u: payload %zu B\n", n, bytes);
		print_optimizer_config(data, bytes);
	}

	/* --- the graph's edges ------------------------------------------------ */

	const auto *gin  = sub->inputs();
	const auto *gout = sub->outputs();

	if (gin == nullptr || gin->size() != 1u) {
		fail("%u graph inputs; the shell fills exactly one\n",
		     gin != nullptr ? gin->size() : 0u);
	} else {
		const int32_t i0 = gin->Get(0);

		if (i0 < 0 || static_cast<uint32_t>(i0) >= tensors->size()) {
			fail("graph input index %d is out of range\n", i0);
		} else {
			const auto *t = tensors->Get(static_cast<uint32_t>(i0));

			printf("  input  t%-3d %-6s ", i0,
			       tflite::EnumNameTensorType(t->type()));
			print_shape(t);
			print_quant(t);
			printf("\n");
			if (t->type() != tflite::TensorType_INT8)
				fail("the graph input is %s, not INT8.  Strip the leading "
				     "QUANTIZE with tflite_strip_boundary before running "
				     "vela.\n", tflite::EnumNameTensorType(t->type()));
		}
	}

	std::vector<const tflite::Tensor *> outs;

	if (gout == nullptr || gout->size() == 0u) {
		fail("the graph has no outputs\n");
	} else {
		for (uint32_t k = 0; k < gout->size(); k++) {
			const int32_t oi = gout->Get(k);

			if (oi < 0 || static_cast<uint32_t>(oi) >= tensors->size()) {
				fail("graph output %u index %d is out of range\n", k, oi);
				continue;
			}

			const auto *t = tensors->Get(static_cast<uint32_t>(oi));

			printf("  output t%-3d %-6s ", oi,
			       tflite::EnumNameTensorType(t->type()));
			print_shape(t);
			print_quant(t);
			printf("\n");
			if (t->type() != tflite::TensorType_INT8)
				fail("graph output %u is %s, not INT8.  Strip the trailing "
				     "DEQUANTIZEs with tflite_strip_boundary before running "
				     "vela.\n", k, tflite::EnumNameTensorType(t->type()));
			outs.push_back(t);
		}
	}

	/* --- the decoder's contract ------------------------------------------- */

	if (blazeface) {
		const size_t want = sizeof kBlazeFaceOutputs / sizeof kBlazeFaceOutputs[0];

		if (outs.size() != want) {
			fail("BlazeFace wants %zu outputs, this model has %zu\n",
			     want, outs.size());
		} else {
			for (size_t e = 0; e < want; e++) {
				const expected_output &x = kBlazeFaceOutputs[e];
				unsigned matches = count_matching(outs, kBlazeFaceOutputs[e]);

				if (matches != 1u)
					fail("BlazeFace wants exactly one 1x%dx%d output (%s); "
					     "found %u.  blazeface_decode() locates its tensors "
					     "by shape and would return -1.\n",
					     x.anchors, x.chan, x.what, matches);
			}
			if (g_fail == 0)
				printf("  ok     the four BlazeFace output shapes are present "
				       "and unambiguous\n");
		}
	} else if (blazeface_contract_holds(outs)) {
		/* [!] NOTICED, NOT CHECKED, AND NOT REFUSED (issue #95).
		 *
		 * `--profile cls|det` reads like it names the model and does not: it
		 * selects a CHECK SET, and `det` is `cls` plus the four shapes above.
		 * So sending the detector as `cls` passes and transmits, with the one
		 * check that is specific to it never run -- which is what happened the
		 * first time the detector went into the asset store, because picocom
		 * was still running with an earlier step's send hook.  The wrapper
		 * cannot tell: it is launched on the PC and the slot name is typed on
		 * the board.
		 *
		 * A note rather than a refusal.  Carrying these four shapes is strong
		 * evidence and not proof, and a gate that refused on it would be
		 * asserting an identity it cannot establish.  What it can honestly say
		 * is that there was a stricter check available and nobody asked for
		 * it -- before any of the file has gone down the wire. */
		printf("  [!]    this model carries the four BlazeFace output shapes "
		       "but --blazeface\n"
		       "         was not given, so they were NOT pinned "
		       "(--profile det pins them)\n");
	}

	/* --- reachability ------------------------------------------------------ */

	std::vector<bool> seen(tensors->size(), false);

	for (uint32_t n = 0; n < ops->size(); n++) {
		const auto *op = ops->Get(n);

		if (op == nullptr)
			continue;
		for (const auto *v : { op->inputs(), op->outputs(), op->intermediates() })
			for (uint32_t j = 0; v != nullptr && j < v->size(); j++) {
				const int32_t t = v->Get(j);

				if (t >= 0 && static_cast<uint32_t>(t) < seen.size())
					seen[static_cast<size_t>(t)] = true;
			}
	}
	for (const auto *v : { gin, gout })
		for (uint32_t j = 0; v != nullptr && j < v->size(); j++) {
			const int32_t t = v->Get(j);

			if (t >= 0 && static_cast<uint32_t>(t) < seen.size())
				seen[static_cast<size_t>(t)] = true;
		}

	unsigned orphans = 0;

	for (size_t i = 0; i < seen.size(); i++)
		if (!seen[i]) {
			if (orphans < 8u) {
				const auto *t = tensors->Get(static_cast<uint32_t>(i));

				printf("  orphan t%-3zu \"%s\"\n", i,
				       t != nullptr && t->name() != nullptr
				           ? t->name()->c_str() : "<unnamed>");
			}
			orphans++;
		}
	if (orphans != 0u)
		fail("%u tensor(s) are named by no operator and are not graph I/O.\n"
		     "         tflite_strip_boundary leaves the tensors it orphans in\n"
		     "         place on purpose, and vela is expected to rebuild the\n"
		     "         graph without them -- an orphan here means this file did\n"
		     "         not come out of vela, or came out of a version that does\n"
		     "         not prune.  They cost arena space the planner reserves\n"
		     "         and nothing ever reads.\n", orphans);
	else
		printf("  ok     no unreachable tensors\n");

	/* --- the offline memory plan ------------------------------------------ */

	const auto *metadata = model->metadata();
	bool have_plan = false;

	for (uint32_t i = 0; metadata != nullptr && i < metadata->size(); i++) {
		const auto *md = metadata->Get(i);

		if (md == nullptr || md->name() == nullptr ||
		    strcmp(md->name()->c_str(), "OfflineMemoryAllocation") != 0)
			continue;

		have_plan = true;

		const auto *buffers = model->buffers();
		const auto *b = buffers != nullptr && md->buffer() < buffers->size()
		                    ? buffers->Get(md->buffer()) : nullptr;
		const auto *d = b != nullptr ? b->data() : nullptr;

		/* TFLM reads word [2] as the tensor count and words [3..] as one
		 * offset per tensor, and fails AllocateTensors when the count
		 * disagrees with the subgraph (micro_allocation_info.cc,
		 * GetOfflinePlannedOffsets).  Checking it here names the mismatch.
		 * [!] The shared walk above (nn_tflm_model_io_check(), issue #181)
		 * already refused, before this runs, every plan TFLM reads that this
		 * would fail -- a buffer index out of range, no data, a short header,
		 * a count other than every subgraph's tensors together, too few
		 * offsets, an offset below -1 -- so on a model that reaches here these lines mostly print
		 * what the plan holds (strcmp also matches a name with a NUL after
		 * the 23 bytes, which TFLM and the walk do not take for the plan).
		 * They stay as the diagnostic (and still demand at least one offset
		 * and a whole number of words, which the walk does not). */
		if (d == nullptr || d->size() < 16u || (d->size() % 4u) != 0u) {
			fail("the OfflineMemoryAllocation buffer is too short to hold a "
			     "header\n");
			break;
		}

		uint32_t hdr[4];

		memcpy(hdr, d->data(), sizeof hdr);

		const uint32_t nbr = hdr[2];
		const uint32_t have = (d->size() / 4u) - 3u;

		printf("  ok     offline memory plan: %u tensor offsets\n", nbr);
		if (nbr != tensors->size())
			fail("the plan describes %u tensors, the subgraph has %u.  TFLM "
			     "compares these and fails AllocateTensors.\n",
			     nbr, tensors->size());
		if (have < nbr)
			fail("the plan's buffer holds %u offsets but claims %u\n",
			     have, nbr);
		break;
	}
	if (!have_plan)
		fail("no OfflineMemoryAllocation metadata.  Vela generates one; a model "
		     "without it did not come from vela.\n");

	/* --- signatures -------------------------------------------------------- */

	const auto *sigs = model->signature_defs();

	if (sigs == nullptr || sigs->size() == 0u) {
		/* Vela does not carry signatures through, and nothing on the board
		 * reads them.  Stated rather than checked, because a signature that
		 * SURVIVED would be the interesting case -- it would still name the
		 * pre-vela tensor indices. */
		printf("  ok     no signature_defs (vela does not carry them through; "
		       "nothing on the board reads them)\n");
	} else {
		for (uint32_t i = 0; i < sigs->size(); i++) {
			const auto *sd = sigs->Get(i);

			if (sd == nullptr || sd->subgraph_index() != 0u)
				continue;
			for (const auto *v : { sd->inputs(), sd->outputs() })
				for (uint32_t j = 0; v != nullptr && j < v->size(); j++) {
					const auto *tm = v->Get(j);
					const int32_t ti = tm != nullptr ? tm->tensor_index() : -1;

					if (ti < 0 || static_cast<uint32_t>(ti) >= tensors->size())
						fail("signature %u names tensor %d, which does not "
						     "exist.  A signature that outlived the graph it "
						     "described is worse than none.\n", i, ti);
				}
		}
		if (g_fail == 0)
			printf("  ok     signature_defs name tensors that exist\n");
	}

	/* --- does it fit the arena the board reserves? ------------------------ */
	/*
	 * The REAL reservation (port/nn/npu/npu_arena.c) is compiled into this tool,
	 * so the number planned against is the board's by construction rather than
	 * by a constant repeated here and left to drift.
	 */
	{
		tflite::MicroMutableOpResolver<NN_TFLM_OPS_TOTAL> resolver;
		bool ok = true;

#define NN_TFLM_REGISTER(enum_suffix, method) \
		ok = ok && (resolver.method() == kTfLiteOk);
		NN_TFLM_OPS_ALL(NN_TFLM_REGISTER)
#undef NN_TFLM_REGISTER

		if (!ok) {
			fail("the op resolver could not be built on the host\n");
		} else {
			tflite::MicroInterpreter interp(
				model, resolver,
				static_cast<uint8_t *>(npu_arena_base()), npu_arena_bytes());

			if (interp.AllocateTensors() != kTfLiteOk) {
				fail("AllocateTensors() failed against the board's %zu B "
				     "arena.  This is the failure that would otherwise show "
				     "up on hardware as NN_MODEL_ERR_ARENA.\n", npu_arena_bytes());
			} else {
				printf("  ok     AllocateTensors: %zu of %zu B arena used "
				       "(%zu B spare)\n",
				       interp.arena_used_bytes(), npu_arena_bytes(),
				       npu_arena_bytes() - interp.arena_used_bytes());
				check_planned_io(interp);
			}
		}
	}

	printf("RESULT   : %s\n", g_fail == 0 ? "OK" : "REJECT");
	return g_fail == 0 ? 0 : 1;
}
