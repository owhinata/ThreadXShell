/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    tflite_strip_boundary.cc
 * @brief   Strip the boundary type conversions off a .tflite (issue #45).
 *
 * A HOST tool.  Nothing here is ever compiled into a firmware image; it is
 * built with the host compiler against the SAME tflite-micro tree, at the SAME
 * pinned SDK commit, that the firmware's tflm_obj uses -- so the schema this
 * model is unpacked and repacked through is exactly the schema the board reads
 * it back with.
 *
 * WHY THIS EXISTS
 *
 * ST model zoo's blazeface_front_128_int8.tflite has int8 WEIGHTS and float32
 * I/O: a leading QUANTIZE converts the float input to int8, and four trailing
 * DEQUANTIZEs convert the outputs back.  Vela offloads 93.8% of it -- 75
 * operators onto the NPU -- and the five it leaves on the CPU are exactly those
 * boundary conversions.  They are not arithmetic; they are the graph's edges
 * changing type.
 *
 * This board registers ONE operator (MicroMutableOpResolver<1>, AddEthosU) and
 * intends to keep it that way, so a model that needs a CPU kernel does not run.
 * Removing the conversions from the FILE is what makes the model fit: the
 * firmware already fills the input as int8 (nn_fill_input()'s uint8->int8 shift
 * is precisely this model's scale 1/255, zero point -128) and the decoder reads
 * int8 outputs through their own scale and zero point.  Same weights, same
 * arithmetic, same detections -- the conversions move from the graph to the two
 * places that were doing them anyway.
 *
 * WHAT IT DOES NOT DO: DELETE TENSORS
 *
 * The tensors the removed operators produced and consumed stay in the table as
 * ORPHANS.  That is the whole reason this tool is short.  Deleting a tensor
 * renumbers every index above it, and the reference sites are only mostly
 * enumerable -- an unrecognised metadata buffer or a custom operator's opaque
 * options could hold an index nothing here could find.  The sibling tool on the
 * Wio board spends most of its length on that renumbering because it had to.
 * Leaving orphans costs a few hundred bytes in an intermediate file that is
 * then fed to Vela, which rebuilds the graph from scratch.
 *
 * [!] "Vela prunes the orphans" is an OBSERVATION, not a contract.  Nothing
 * here depends on it, and the gate that inspects Vela's output
 * (verify_vela_model) checks what actually came out rather than assuming.
 *
 * WHAT IT REFUSES, AND WHY IT REFUSES RATHER THAN COPES
 *
 * Removing operators shifts every operator index above them, and it changes
 * which tensors are reachable.  So:
 *
 *   - metadata is an ALLOWLIST of names known to hold no graph indices.  An
 *     unfamiliar name is exactly the case that cannot be reasoned about: the
 *     flatbuffer verifier checks structure and never meaning.
 *     "OfflineMemoryAllocation" is the known-bad case and is doubly wrong here
 *     -- tflite-micro reads it as a tensor count plus one offset per tensor
 *     index, and a model that already carries one has already been through
 *     Vela, which is not this tool's input.
 *   - debug_metadata_index, on the subgraph or on any operator, is an index
 *     into a structure this tool does not parse.
 *   - a CUSTOM operator's options are an opaque blob that could hold anything.
 *
 * Refusing costs a message.  Converting a file whose metadata now lies costs a
 * debugging session on a board whose only symptom is one kTfLiteError.
 *
 * [!] THE FLATBUFFER BUILDER NEEDS AN EXPLICIT ALLOCATOR.
 * Upstream flatbuffers lets a null allocator mean "use the default one".  The
 * copy vendored into tflite-micro does not -- its Allocate() dereferences the
 * pointer unconditionally (third_party/flatbuffers/include/flatbuffers/
 * default_allocator.h), because tflite-micro does not want a new/delete-based
 * default.  A default-constructed FlatBufferBuilder therefore segfaults on the
 * first Pack(), with a stack that points at the schema and not at the mistake.
 *
 * Build + run:
 *   cmake --build build/grove-vision-ai-v2 --target model-tools
 *   ./build/grove-vision-ai-v2/tflite_strip_boundary in.tflite stripped.tflite
 *   vela --accelerator-config ethos-u55-64 --output-dir . stripped.tflite
 *   ./build/grove-vision-ai-v2/verify_vela_model stripped_vela.tflite
 */
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/schema/schema_utils.h"   /* GetBuiltinCode() */
/*
 * For TFLITE_SCHEMA_VERSION only -- this tool never builds an interpreter.
 * Taken from the runtime's own header rather than restated: unpacking and
 * repacking sends the model THROUGH this tree's generated schema, and anything
 * the tree does not model is dropped on the way out.
 */
#include "tensorflow/lite/micro/micro_interpreter.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/*
 * Metadata names known to carry nothing indexed by tensor or by operator.
 * An ALLOWLIST, not a denylist: an unrecognised name is the case that must stop
 * the tool rather than slip through it.
 *
 *   min_runtime_version   a version string.  (It may now overstate what the
 *                         model needs, since operators were removed; that is
 *                         not a correctness problem for any runtime.)
 *   CONVERSION_METADATA   provenance written by the converter.
 *   TFLITE_METADATA       the tflite-support ModelMetadata.  Its input/output
 *                         metadata corresponds to SubGraph.inputs/outputs BY
 *                         POSITION and holds no indices; this transform changes
 *                         neither the count nor the order of either.
 */
const char *const kIndexFreeMetadata[] = {
	"min_runtime_version",
	"CONVERSION_METADATA",
	"TFLITE_METADATA",
};

const char *type_name(tflite::TensorType t)
{
	const char *n = tflite::EnumNameTensorType(t);

	return n != nullptr ? n : "?";
}

bool read_file(const char *path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path, "rb");

	if (f == nullptr) {
		fprintf(stderr, "tflite_strip_boundary: cannot open %s\n", path);
		return false;
	}

	uint8_t buf[65536];
	size_t n;

	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.insert(out.end(), buf, buf + n);

	bool ok = (ferror(f) == 0);

	fclose(f);
	if (!ok)
		fprintf(stderr, "tflite_strip_boundary: read error on %s\n", path);
	return ok;
}

bool write_file(const char *path, const uint8_t *p, size_t n)
{
	FILE *f = fopen(path, "wb");

	if (f == nullptr) {
		fprintf(stderr, "tflite_strip_boundary: cannot create %s\n", path);
		return false;
	}

	bool ok = (fwrite(p, 1, n, f) == n);

	if (fclose(f) != 0)
		ok = false;
	if (!ok)
		fprintf(stderr, "tflite_strip_boundary: write error on %s\n", path);
	return ok;
}

void print_tensor(const tflite::SubGraphT *sg, int32_t idx, const char *tag)
{
	if (idx < 0 || static_cast<size_t>(idx) >= sg->tensors.size()) {
		printf("  %-8s <tensor %d out of range>\n", tag, idx);
		return;
	}

	const tflite::TensorT *t = sg->tensors[static_cast<size_t>(idx)].get();

	printf("  %-8s t%-4d %-8s [", tag, idx, type_name(t->type));
	for (size_t i = 0; i < t->shape.size(); i++)
		printf("%s%d", i ? "x" : "", t->shape[i]);
	printf("]");

	const tflite::QuantizationParametersT *q = t->quantization.get();

	if (q != nullptr && q->scale.size() == 1u && q->zero_point.size() == 1u)
		printf("  scale %.9g  zp %d",
		       static_cast<double>(q->scale[0]),
		       static_cast<int>(q->zero_point[0]));
	else if (q != nullptr && q->scale.size() > 1u)
		printf("  per-axis, %zu scales", q->scale.size());
	printf("   \"%s\"\n", t->name.c_str());
}

/* Which operators name @p idx as an input?  Used to prove that the tensor a
 * boundary operator converts is not ALSO consumed somewhere else -- removing
 * the operator would then remove a value the rest of the graph still wants. */
size_t consumer_count(const tflite::SubGraphT *sg, int32_t idx)
{
	size_t n = 0;

	for (const auto &op : sg->operators)
		for (int32_t t : op->inputs)
			if (t == idx)
				n++;
	return n;
}

/* Which operator produces @p idx?  Returns the operator index, or -1 for none;
 * sets @p ambiguous if more than one does (a malformed graph, but this tool
 * must not pick one of them arbitrarily). */
int64_t producer_of(const tflite::SubGraphT *sg, int32_t idx, bool *ambiguous)
{
	int64_t found = -1;

	*ambiguous = false;
	for (size_t i = 0; i < sg->operators.size(); i++)
		for (int32_t t : sg->operators[i]->outputs)
			if (t == idx) {
				if (found >= 0) {
					*ambiguous = true;
					return found;
				}
				found = static_cast<int64_t>(i);
			}
	return found;
}

/*
 * GetBuiltinCode() rather than reading a field: the schema keeps a deprecated
 * 8-bit code alongside the current one and the right answer is the maximum of
 * the pair, so reading either alone misidentifies operators.  The overload for
 * the unpacked object is linked from the tree (schema_utils.cc), not
 * reimplemented -- the firmware's npu_tflm.cc identifies operators with the
 * same function.
 */
tflite::BuiltinOperator builtin_of(const tflite::ModelT &m,
                                   const tflite::OperatorT *op)
{
	if (op->opcode_index >= m.operator_codes.size())
		return tflite::BuiltinOperator_CUSTOM;
	return tflite::GetBuiltinCode(m.operator_codes[op->opcode_index].get());
}

/* A boundary operator is a single-input, single-output type conversion whose
 * output nothing else in the graph shares.  Anything else wearing the same
 * opcode is not what this transform assumes it is. */
bool boundary_shape_ok(const tflite::OperatorT *op)
{
	return op->inputs.size() == 1u && op->outputs.size() == 1u &&
	       op->intermediates.empty() &&
	       op->inputs[0] >= 0 && op->outputs[0] >= 0;
}

}  /* namespace */

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr,
		        "usage: tflite_strip_boundary <in.tflite> <out.tflite>\n"
		        "  Removes the leading QUANTIZE and the trailing DEQUANTIZEs so\n"
		        "  that the model's own int8 tensors are the graph input and\n"
		        "  outputs.  Deletes no tensors, so no index is renumbered.\n"
		        "  Feed the result to vela; run verify_vela_model on THAT.\n");
		return 2;
	}

	std::vector<uint8_t> buf;

	if (!read_file(argv[1], buf))
		return 2;

	printf("in       : %s (%zu B)\n", argv[1], buf.size());

	/* Do not transform a file that cannot be trusted.  A partial check is
	 * worse than none, because it reports that the file was examined. */
	if (buf.size() < 8u || !tflite::ModelBufferHasIdentifier(buf.data())) {
		printf("RESULT   : REJECT -- not a .tflite flatbuffer (no \"TFL3\").\n");
		return 1;
	}
	{
		flatbuffers::Verifier v(buf.data(), buf.size());

		if (!tflite::VerifyModelBuffer(v)) {
			printf("RESULT   : REJECT -- the input flatbuffer is malformed.\n"
			       "           Check where it came from (interrupted download,\n"
			       "           a converter still writing, a git-lfs pointer).\n");
			return 1;
		}
	}

	const tflite::Model *fb = tflite::GetModel(buf.data());

	if (fb->version() != TFLITE_SCHEMA_VERSION) {
		printf("RESULT   : REJECT -- schema version %u, this tree reads %d.\n",
		       static_cast<unsigned>(fb->version()), TFLITE_SCHEMA_VERSION);
		return 1;
	}

	tflite::ModelT m;

	fb->UnPackTo(&m);

	/* --- refusals, all of them before anything is modified --------------- */

	for (const auto &md : m.metadata) {
		bool known = false;

		for (const char *ok : kIndexFreeMetadata)
			if (md->name == ok)
				known = true;
		if (!known) {
			printf("RESULT   : REJECT -- metadata \"%s\".  This transform removes\n"
			       "           operators, which shifts operator indices and changes\n"
			       "           which tensors are reachable; the flatbuffer verifier\n"
			       "           checks structure and never meaning, so a metadata\n"
			       "           buffer this tool does not recognise could silently\n"
			       "           start describing the wrong thing.\n"
			       "           \"OfflineMemoryAllocation\" is the known case, and it\n"
			       "           also means the input has ALREADY been through vela --\n"
			       "           run this on the model as the converter produced it.\n",
			       md->name.empty() ? "<unnamed>" : md->name.c_str());
			return 1;
		}
	}

	if (m.operator_codes.empty()) {
		printf("RESULT   : REJECT -- the model declares no operator codes.\n");
		return 1;
	}
	for (size_t i = 0; i < m.operator_codes.size(); i++) {
		const tflite::OperatorCodeT *c = m.operator_codes[i].get();

		if (tflite::GetBuiltinCode(c) == tflite::BuiltinOperator_CUSTOM) {
			printf("RESULT   : REJECT -- operator code %zu is CUSTOM (\"%s\").  Its\n"
			       "           options are an opaque blob, so this tool cannot know\n"
			       "           whether they carry indices that removing operators\n"
			       "           would invalidate.\n",
			       i, c->custom_code.c_str());
			return 1;
		}
	}

	if (m.subgraphs.size() != 1u) {
		printf("RESULT   : REJECT -- %zu subgraphs; this tool rewrites\n"
		       "           single-subgraph models only.\n", m.subgraphs.size());
		return 1;
	}

	tflite::SubGraphT *sg = m.subgraphs[0].get();

	if (sg->debug_metadata_index != -1) {
		printf("RESULT   : REJECT -- the subgraph references debug metadata, which\n"
		       "           this tool does not parse and will not reindex.\n");
		return 1;
	}
	for (size_t i = 0; i < sg->operators.size(); i++)
		if (sg->operators[i]->debug_metadata_index != -1) {
			printf("RESULT   : REJECT -- operator %zu references debug metadata,\n"
			       "           which this tool does not parse and will not reindex.\n",
			       i);
			return 1;
		}

	if (sg->inputs.size() != 1u) {
		printf("RESULT   : REJECT -- %zu graph inputs; this transform is written\n"
		       "           for a single input.\n", sg->inputs.size());
		return 1;
	}
	if (sg->outputs.empty()) {
		printf("RESULT   : REJECT -- the graph has no outputs.\n");
		return 1;
	}

	/* --- locate the head QUANTIZE ---------------------------------------- */

	const int32_t old_in = sg->inputs[0];
	bool ambiguous = false;

	if (old_in < 0 || static_cast<size_t>(old_in) >= sg->tensors.size()) {
		printf("RESULT   : REJECT -- graph input index %d is out of range.\n", old_in);
		return 1;
	}
	if (consumer_count(sg, old_in) != 1u) {
		printf("RESULT   : REJECT -- the graph input feeds %zu operators; the\n"
		       "           leading QUANTIZE must be its only consumer, or removing\n"
		       "           it would take a value the rest of the graph still uses.\n",
		       consumer_count(sg, old_in));
		return 1;
	}

	int64_t head = -1;

	for (size_t i = 0; i < sg->operators.size() && head < 0; i++)
		for (int32_t t : sg->operators[i]->inputs)
			if (t == old_in) {
				head = static_cast<int64_t>(i);
				break;
			}
	if (head < 0) {
		printf("RESULT   : REJECT -- nothing consumes the graph input.\n");
		return 1;
	}

	const tflite::OperatorT *hop = sg->operators[static_cast<size_t>(head)].get();

	if (builtin_of(m, hop) != tflite::BuiltinOperator_QUANTIZE) {
		printf("RESULT   : REJECT -- the graph input feeds %s, not QUANTIZE.\n"
		       "           This model's input is already whatever it is; there is\n"
		       "           no boundary conversion here to strip.\n",
		       tflite::EnumNameBuiltinOperator(builtin_of(m, hop)));
		return 1;
	}
	if (!boundary_shape_ok(hop)) {
		printf("RESULT   : REJECT -- the leading QUANTIZE is not a plain 1-in 1-out\n"
		       "           conversion.\n");
		return 1;
	}

	const int32_t new_in = hop->outputs[0];

	if (sg->tensors[static_cast<size_t>(old_in)]->type != tflite::TensorType_FLOAT32 ||
	    sg->tensors[static_cast<size_t>(new_in)]->type != tflite::TensorType_INT8) {
		printf("RESULT   : REJECT -- the leading QUANTIZE is %s -> %s, not\n"
		       "           FLOAT32 -> INT8.\n",
		       type_name(sg->tensors[static_cast<size_t>(old_in)]->type),
		       type_name(sg->tensors[static_cast<size_t>(new_in)]->type));
		return 1;
	}
	/* The tensor that becomes the graph input must not also be produced
	 * somewhere else, and must not already be a graph output. */
	(void)producer_of(sg, new_in, &ambiguous);
	if (ambiguous) {
		printf("RESULT   : REJECT -- more than one operator produces tensor %d.\n",
		       new_in);
		return 1;
	}

	/* --- locate one tail DEQUANTIZE per graph output --------------------- */

	std::vector<int64_t> tails;          /* operator indices, one per output   */
	std::vector<int32_t> new_outs;       /* what each output is repointed to   */

	for (size_t k = 0; k < sg->outputs.size(); k++) {
		const int32_t out = sg->outputs[k];

		if (out < 0 || static_cast<size_t>(out) >= sg->tensors.size()) {
			printf("RESULT   : REJECT -- graph output %zu index %d is out of range.\n",
			       k, out);
			return 1;
		}

		const int64_t tail = producer_of(sg, out, &ambiguous);

		if (ambiguous) {
			printf("RESULT   : REJECT -- more than one operator produces graph\n"
			       "           output %zu (tensor %d).\n", k, out);
			return 1;
		}
		if (tail < 0) {
			printf("RESULT   : REJECT -- graph output %zu (tensor %d) is produced by\n"
			       "           no operator.\n", k, out);
			return 1;
		}

		const tflite::OperatorT *top = sg->operators[static_cast<size_t>(tail)].get();

		if (builtin_of(m, top) != tflite::BuiltinOperator_DEQUANTIZE) {
			printf("RESULT   : REJECT -- graph output %zu is produced by %s, not\n"
			       "           DEQUANTIZE.  Every output must be a boundary\n"
			       "           conversion or this transform would change what the\n"
			       "           model computes, not just where the conversion lives.\n",
			       k, tflite::EnumNameBuiltinOperator(builtin_of(m, top)));
			return 1;
		}
		if (!boundary_shape_ok(top)) {
			printf("RESULT   : REJECT -- the DEQUANTIZE for output %zu is not a plain\n"
			       "           1-in 1-out conversion.\n", k);
			return 1;
		}
		if (consumer_count(sg, out) != 0u) {
			printf("RESULT   : REJECT -- graph output %zu (tensor %d) is also consumed\n"
			       "           inside the graph, so its DEQUANTIZE cannot be removed.\n",
			       k, out);
			return 1;
		}

		const int32_t qt = top->inputs[0];

		if (sg->tensors[static_cast<size_t>(out)]->type != tflite::TensorType_FLOAT32 ||
		    sg->tensors[static_cast<size_t>(qt)]->type != tflite::TensorType_INT8) {
			printf("RESULT   : REJECT -- the DEQUANTIZE for output %zu is %s -> %s,\n"
			       "           not INT8 -> FLOAT32.\n", k,
			       type_name(sg->tensors[static_cast<size_t>(qt)]->type),
			       type_name(sg->tensors[static_cast<size_t>(out)]->type));
			return 1;
		}
		/* Two graph outputs must not collapse onto one tensor: the caller
		 * distinguishes them by shape, and a duplicate would also make the
		 * operator-removal list contain the same index twice. */
		for (size_t j = 0; j < new_outs.size(); j++)
			if (new_outs[j] == qt) {
				printf("RESULT   : REJECT -- graph outputs %zu and %zu would both\n"
				       "           become tensor %d.\n", j, k, qt);
				return 1;
			}
		/* The int8 tensor is about to become a graph output.  If something
		 * downstream still reads it that is fine -- it stays produced by the
		 * same operator -- but it must not be the head QUANTIZE's output as
		 * well, which would make one tensor both input and output. */
		if (qt == new_in) {
			printf("RESULT   : REJECT -- tensor %d would be both the graph input and\n"
			       "           graph output %zu.\n", qt, k);
			return 1;
		}
		tails.push_back(tail);
		new_outs.push_back(qt);
	}

	/* --- report, then rewrite -------------------------------------------- */

	printf("subgraph : %zu tensors, %zu operators\n",
	       sg->tensors.size(), sg->operators.size());
	printf("input\n");
	print_tensor(sg, old_in, "was");
	print_tensor(sg, new_in, "now");
	for (size_t k = 0; k < sg->outputs.size(); k++) {
		printf("output %zu\n", k);
		print_tensor(sg, sg->outputs[k], "was");
		print_tensor(sg, new_outs[k], "now");
	}

	/* Repoint the SignatureDef tensor maps by the SAME rule.  A signature
	 * whose map still named the float tensors would describe a model that no
	 * longer exists -- and tooling reads signatures in preference to the raw
	 * subgraph lists. */
	size_t sig_in = 0, sig_out = 0;

	for (const auto &sd : m.signature_defs) {
		if (sd->subgraph_index != 0u)
			continue;
		for (const auto &tm : sd->inputs)
			if (tm->tensor_index == old_in) {
				tm->tensor_index = new_in;
				sig_in++;
			}
		for (const auto &tm : sd->outputs)
			for (size_t k = 0; k < sg->outputs.size(); k++)
				if (tm->tensor_index == sg->outputs[k]) {
					tm->tensor_index = new_outs[k];
					sig_out++;
				}
	}

	sg->inputs[0] = new_in;
	for (size_t k = 0; k < sg->outputs.size(); k++)
		sg->outputs[k] = new_outs[k];

	/* Erase the operators from the highest index down, so the earlier indices
	 * in the list stay valid as the vector shrinks. */
	std::vector<int64_t> drop = tails;

	drop.push_back(head);
	for (size_t i = 0; i < drop.size(); i++)
		for (size_t j = i + 1u; j < drop.size(); j++)
			if (drop[j] > drop[i])
				std::swap(drop[i], drop[j]);
	for (int64_t idx : drop)
		sg->operators.erase(sg->operators.begin() + static_cast<ptrdiff_t>(idx));

	printf("removed  : 1 QUANTIZE + %zu DEQUANTIZE -> %zu operators left\n",
	       tails.size(), sg->operators.size());
	printf("signature: %zu input map(s), %zu output map(s) repointed\n",
	       sig_in, sig_out);
	printf("tensors  : %zu, unchanged -- the boundary tensors stay as orphans so\n"
	       "           no index is renumbered.  vela rebuilds the graph anyway.\n",
	       sg->tensors.size());

	/* --- repack ----------------------------------------------------------- */

	flatbuffers::DefaultAllocator allocator;
	flatbuffers::FlatBufferBuilder fbb(1024, &allocator);

	tflite::FinishModelBuffer(fbb, tflite::Model::Pack(fbb, &m));

	/* Verify what was actually produced, not what was intended.  Repacking
	 * goes through this tree's generated schema, and this is the only place
	 * that would notice if it dropped something on the way out. */
	{
		flatbuffers::Verifier v(fbb.GetBufferPointer(), fbb.GetSize());

		if (!tflite::VerifyModelBuffer(v)) {
			printf("RESULT   : REJECT -- the REPACKED flatbuffer does not verify.\n"
			       "           Nothing was written.\n");
			return 1;
		}
	}

	if (!write_file(argv[2], fbb.GetBufferPointer(), fbb.GetSize()))
		return 2;

	printf("out      : %s (%u B)\n", argv[2], fbb.GetSize());
	printf("RESULT   : OK -- now run vela on it, then verify_vela_model on THAT.\n"
	       "           vela --accelerator-config ethos-u55-64 %s\n", argv[2]);
	return 0;
}
