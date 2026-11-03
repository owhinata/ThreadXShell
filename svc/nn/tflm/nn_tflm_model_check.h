/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_model_check.h
 * @brief   What a TFLM model must satisfy before it is planned -- the bounded
 *          flatbuffer verifier and the graph walk (structure, lengths, ranks,
 *          quantization lengths, buffer indices and the offline plan's
 *          header, constant buffers, every tensor's planned size, the operator
 *          rules, I/O dims) -- shared by both interpreting backends and by the
 *          three host gates (issues #175, #178, #180, #181, #183, #185, #188).
 *
 * TWO CHECKS, in the order a loader runs them:
 *
 *   nn_tflm_verify_model_buffer()  every offset in the buffer lands inside it,
 *                                  within the limits below (see "THE VERIFIER")
 *   nn_tflm_model_io_check()       the graph holds together (every subgraph's
 *                                  tensors, every operator's tensor lists, each
 *                                  at most NN_TFLM_MAX_OP_TENSORS long and naming
 *                                  only tensors that exist, every tensor's
 *                                  quantization vectors within their limits and
 *                                  the operators' per-channel sum within its
 *                                  own, every buffer index naming a buffer that
 *                                  exists, an offline plan that matches the
 *                                  graph, an operator-code vector, every
 *                                  tensor's type one with a size, every
 *                                  tensor's planned size an int and all of
 *                                  them together at most 2^30, every
 *                                  constant's data exactly its shape's bytes,
 *                                  no data on a tensor the graph writes, every
 *                                  operator naming an operator code that
 *                                  exists, and the operator rules below),
 *                                  subgraph 0's I/O is
 *                                  1..NN_MAX_IO per list, every tensor's rank is
 *                                  at most NN_TFLM_MAX_TENSOR_RANK (subgraph 0's
 *                                  I/O at most NN_MAX_DIMS), and the I/O
 *                                  dimensions fit a descriptor -- in that order,
 *                                  lengths before elements, so its work is a
 *                                  small constant per table the verifier visited
 *
 * THE I/O CHECK: every input and output tensor of subgraph 0 has dimensions a
 * descriptor can hold (nn.h, nn_tensor_dim_fits(): 0..NN_TENSOR_DIM_MAX, the range
 * of struct nn_tensor's 16-bit dims).  Without it a dimension of 70,000 was
 * published as 4,464 -- a shape the model does not have, which `nn info` printed
 * and every consumer of the descriptor sized its work by.
 *
 * [!] ON THE MODEL, BEFORE AllocateTensors().  TFLM's allocator builds the I/O
 * tensors' dims from these `Tensor.shape` vectors (the interpreter's I/O is
 * subgraph 0's inputs() / outputs() in both pinned trees), and this check is
 * reached before the planner runs -- which could otherwise refuse a large or
 * negative dimension as NN_MODEL_ERR_ARENA first -- and before the previous
 * interpreter is destroyed.
 *
 * [!] DECLARED SHAPES, NOT NECESSARILY THE PUBLISHED ONES.  A kernel's Prepare()
 * may rewrite an output's dims during AllocateTensors() (the eval tensor's dims
 * are the source of truth, micro_allocator.cc), so in general the planned shape
 * can differ from the declared one.  With the kernels the registered profiles hold
 * today that does not happen to graph I/O; a kernel that rewrites dims is caught
 * only by the backstop, nn_tflm_tensor_dims_fit() on the planned tensors, which
 * both backends keep with the same code.  And the host gates differ here:
 * wio-lite-ai's and f746g-disco's verify_tflite do not plan the model, so they
 * check the declared shapes only (grove-vision-ai-v2's verify_vela_model plans
 * and runs the backstop as well).  A declared negative dimension, -1 included (a
 * dimension a RESHAPE would resolve at Prepare()), is refused before this check,
 * as FORMAT, by R10 (issue #188, "OUTPUT FRAMES" below).
 *
 * [!] IT INDEXES NOTHING UNCHECKED.  A verifier proves offsets, not tensor
 * indices, and wio-lite-ai runs no on-board verifier at all: subgraph 0, its
 * tensor vector and its input / output lists must be present, and every index
 * must lie inside the tensor vector, or the answer is NN_MODEL_ERR_FORMAT (a
 * model whose structure does not hold together -- TFLM would index out of range).
 * The same holds for EVERY subgraph and EVERY operator (issue #178): TFLM's
 * planner writes subgraph_allocation_info[index] for each operator input and
 * output without a range check (micro_allocation_info.cc, MarkAllocationLifetimes)
 * and reads eval_tensors[index] for each intermediate (ValidateSubgraph), and it
 * hands a kernel a null inputs / outputs list as it found it.  So every subgraph
 * must have a tensor vector; an operator vector may be absent or empty (TFLM's
 * NumSubgraphOperators() reads that as no operators), but every operator present
 * has an inputs and an outputs list; inputs name -1 (TFLM's optional tensor) or a
 * tensor of the operator's subgraph, outputs and intermediates (which may be
 * absent) a tensor only.  NOT checked: the inputs / outputs lists of a subgraph
 * other than 0, neither their length nor their indices -- TFLM reads them only
 * through a control-flow operator (IF / WHILE / CALL_ONCE, in
 * MarkAllocationLifetimes), and no profile registers one (nn_tflm_ops.h), so a
 * model that would use them is refused by the resolver; a profile that adds one
 * has to extend this walk first.
 *
 * [!] BUFFER INDICES AND THE OFFLINE PLAN (issue #181).  TFLM indexes the model's
 * buffer vector without a range check twice, and a verifier proves neither:
 *
 *   Tensor.buffer    micro_allocator.cc GetFlatbufferTensorBuffer()
 *                    `(*buffers)[flatbuffer_tensor.buffer()]`, for every tensor
 *                    of every subgraph (AllocateTfLiteEvalTensors(), at the start
 *                    of AllocateTensors(); a model without a buffer vector is
 *                    read through null)
 *   Metadata.buffer  micro_allocation_info.cc GetOfflinePlannedOffsets()
 *                    `(*buffers)[metadata->buffer()]` for an entry named
 *                    "OfflineMemoryAllocation", then that buffer's data() with no
 *                    null check, word [2] as the tensor count and words [3..] as
 *                    one offset per tensor with no length check
 *
 * (FLATBUFFERS_ASSERT is an assert: wio-lite-ai and f746g-disco build TFLM with
 * NDEBUG, so the index reads past the vector; grove-vision-ai-v2's TFLM keeps it
 * and aborts, the console gone either way.)  So: every tensor of every subgraph
 * and every metadata entry, whatever its name, names a buffer below the buffer
 * vector's size (0 when it is absent).  Every entry whose name TFLM takes for the
 * plan -- the same test as TFLM's, exactly 23 bytes equal to
 * "OfflineMemoryAllocation", the length read first and the bytes compared with a
 * bounded memcmp -- must have data(), at least the 12-byte header, word [2] (a
 * signed int32, so a negative count is refused before any arithmetic) equal to
 * the TOTAL tensor count over all subgraphs (what TFLM compares it with,
 * info_.tensor_count; TFLM refuses a mismatch only inside AllocateTensors(),
 * which the board reports as NN_MODEL_ERR_ARENA and the wio-lite-ai /
 * f746g-disco gates, which never plan, would have passed), and room for that many
 * offsets after the header, each of them -1 or >= 0 and below
 * NN_TFLM_MAX_PLANNED_BYTES (2^30, issue #188).  Every matching entry is
 * checked: TFLM reads each and keeps the last.  Of the header only word [2] is
 * read (words [0] / [1], the version and the subgraph count, are read by no
 * one, TFLM included).  [!] THE OFFSET VALUES: TFLM's greedy planner takes only
 * -1 (kOnlinePlannedBuffer) for "plan online" and keeps any other value as a
 * fixed offset (greedy_memory_planner.cc), and GetMaximumMemorySize() adds
 * offset + size in int (micro_allocator.cc), so an offset between -size and -1
 * passes the arena check and CommitPlan() places the tensor BEFORE the arena;
 * the walk refuses any offset below -1, and one at or past 2^30, which with a
 * tensor's size could wrap that int sum (issue #188; one compare per offset, nbr
 * of them, and nbr is the tensor count, so the work stays visits x a constant).  An
 * offset's alignment and whether the tensor fits the arena stay the planner's
 * (it checks the fit).  Buffer 0 is NOT required to be empty:
 * TFLM does not rely on it (a tensor whose buffer has no data is simply not
 * constant), Vela points its non-constant tensors at buffer 0, newer converters
 * give each its own empty buffer.  And the model must have an operator-code
 * vector: micro_interpreter.cc PrepareNodeAndRegistrationDataFromFlatbuffer()
 * reads opcodes->size() without a null check (the payload scan already refused
 * one on grove-vision-ai-v2; a model without operators has no use for it
 * either way, and no converter omits it).
 *
 * [!] CONSTANT BUFFERS (issue #183).  TFLM points a tensor whose buffer has data
 * (GetFlatbufferTensorBuffer(): a data vector of size > 0) at those bytes, in
 * the model, and compares their length with nothing: BytesRequiredForTensor()
 * and TfLiteEvalTensorByteLength() (memory_helpers.cc) multiply the shape in int
 * and the element size, and no caller looks at data()->size() (a verifier proves
 * only that the vector lies in the file).  A kernel that reads a constant (a
 * filter, RESHAPE's shape) by ITS OWN eval tensor's dims, which are the
 * serialized shape, reads a short buffer under a large shape past the end of
 * the model; the walk closes that.  A kernel that reads a constant by ANOTHER
 * tensor's dims is the operator rules' (below, issue #185).  And the planner
 * does not place a tensor with data (micro_allocation_info.cc,
 * needs_allocating), so one that the graph WRITES --
 * a graph input the board fills, an operator output a kernel produces -- is
 * written in place, into the model (PSRAM staging on wio-lite-ai, SDRAM on
 * f746g-disco, the XIP window on grove-vision-ai-v2).  So, all FORMAT:
 *
 *   every tensor of every subgraph, data or not, has a type TFLM gives a size
 *   (nn_tflm_elem_bytes2(); TFLM's planner fails AllocateTensors() on any other
 *   -- ARENA on the board, a PASS on the gates that never plan);
 *   every tensor with data declares at most NN_TFLM_MAX_TENSOR_RANK dimensions
 *   (judged on the length, before any value), none negative, and its data is
 *   EXACTLY element count x element size long (1 element for no shape, INT4
 *   packed two to a byte; longer is refused as a policy -- every constant of the
 *   44 models measured is exact -- and so is a 0-element constant with data);
 *   no tensor an operator of any subgraph outputs, and no tensor of subgraph
 *   0's inputs / outputs, has data (a variable neither).
 *
 * The sparsity table is not read: TFLM micro reads a constant dense (no kernel
 * registered here densifies), so a sparse constant's compressed buffer is short
 * of its dense shape and refused.  NOT closed here: how far the NPU reads from
 * an ethos-u operator's weights (input 1) is decided by its command stream, not
 * by that tensor's length (grove-vision-ai-v2's README).  Work: per tensor entry
 * one type, one buffer table, one data length and at most
 * NN_TFLM_MAX_TENSOR_RANK dimension values; per operator entry at most
 * NN_TFLM_MAX_OP_TENSORS output tensors' buffers -- a constant per visit, so the
 * bound below holds.  The dimension VALUES of a tensor without data are read
 * only by the size bound (R10, "OUTPUT FRAMES" below) and the operator rules,
 * which compare them with bounds and with each other and never size any work by
 * them (Vela's internal tensors reach 2.2 M elements, far inside R10, and the
 * ethos-u profile has no operator rule).
 *
 * [!] OPERATOR RULES (issue #185).  Some registered kernels read a constant not
 * by its own shape but by ANOTHER tensor's dims (the output's channel count, the
 * input's depth times the depth multiplier), or by a type of their own rather
 * than the one declared (a bias read as int32 or int64 whatever its type), and
 * their Prepare() compares the two with a TFLITE_DCHECK only -- compiled out
 * by NDEBUG, which wio-lite-ai and f746g-disco build TFLM with -- or not at all.
 * So a constant exactly its own shape's bytes (above) is still read past the
 * model.  Measured in lib/tflite-micro (CMSIS-NN on, as both boards build it):
 *
 *   CONV_2D            the int8 kernel reads the filter for, and the bias as,
 *                      the OUTPUT's channel count (cmsis_nn/conv.cc,
 *                      arm_convolve_s8.c); reference (float) by filter[0].
 *                      [!] Its Prepare()'s bias check is dead: it takes the bias
 *                      with AllocateTempOutputTensor(node, kConvBiasTensor),
 *                      output 2, which is always null (cmsis_nn/conv.cc:65-66,
 *                      micro_context.cc:32-40), so the rank / length check at
 *                      :97-101 never runs (lib/ is read-only; this rule makes
 *                      the same check, and more, first).
 *   DEPTHWISE_CONV_2D  filter and bias read by input depth x depth_multiplier
 *                      on the general path and by the output's channels on the
 *                      x4 path (arm_depthwise_conv_s8.c); a depth_multiplier
 *                      of 0 (the field absent) never ends the x4 path's loop.
 *                      Prepare() checks only the scale length.
 *   FULLY_CONNECTED    the int8 kernel reads the weights and the bias by the
 *                      output's last dimension (cmsis_nn/fully_connected.cc),
 *                      reference (float) by weights[0], and no Prepare() check
 *                      relates the three; int16 reads the bias as int64.
 *   PAD / STRIDED_SLICE  the index tensors are read as int32 whatever their
 *   / TRANSPOSE        type, PAD's paddings 2 x the INPUT's rank of them while
 *                      Prepare() counts them against the OUTPUT's rank
 *                      (pad_common.cc), STRIDED_SLICE's begin / end / strides
 *                      the input's rank each with no count check
 *                      (strided_slice_common.cc) -- both inside
 *                      AllocateTensors().
 *   RESIZE_*           the size read as 2 int32 (bilinear's count unchecked)
 *                      and the output written by the size's VALUES
 *                      (reference/resize_bilinear.h, resize_nearest_neighbor.h).
 *   ADD / SUB / MUL    the broadcast loop runs over the OUTPUT's dims and steps
 *                      an input only where its dimension is not 1
 *                      (reference/broadcast_loop.h), so an input dimension that
 *                      is neither 1 nor the output's is read past; same shapes
 *                      write the input's element count (DCHECK only).
 *   CONCATENATION      each input read by its axis dimension x the output's
 *                      inner size (reference/concatenation.h; DCHECKs only).
 *
 * Every rule is FORMAT and runs in the operator loop after the operator's lists,
 * quantization and output data are proven.  R0 first, for every profile: the
 * operator's opcode_index is below the operator-code vector's length (TFLM
 * compares it only inside AllocateTensors(), so the board said ARENA and the
 * wio-lite-ai / f746g-disco gates, which read only the code vector, passed it).
 * Then the operator's kind -- max(builtin_code, deprecated_builtin_code), what
 * GetBuiltinCode() reads -- picks one rule through a switch built from
 * nn_tflm_ops.h's selected list: one case per registered operator, calling
 * nn_tflm_op_rule_<SUFFIX>(), so a profile compiles only its own operators'
 * rules (the ethos-u profile none but CUSTOM's, which is empty) and an operator
 * added to the list without a rule function does not build.  Any other kind is
 * passed here: it is not registered, which the gates' operator list and the
 * board's resolver refuse, as before.  So a board and a gate built with the
 * same profile reach the same answer; two profiles may differ (a
 * FULLY_CONNECTED that breaks R3 is FORMAT on f746g-disco and an unregistered
 * operator on wio-lite-ai's blazeface gate).
 *
 *   R1 CONV_2D            input, filter and output rank 4; filter[0] ==
 *                         output[3]; a bias (input 2, not -1, with or without
 *                         data) of exactly output[3] elements, of the type the
 *                         kernel reads for the input's: FLOAT32 / FLOAT32,
 *                         INT8 / INT32, INT16 / INT32 or INT64; and, under an
 *                         INT16 input or an INT4 filter, no grouping:
 *                         filter[3] == input[3] (those kernels size a filter
 *                         row by the input's channels, arm_convolve_s16.c:79,
 *                         arm_convolve_s4.c:89, and read groups times past the
 *                         filter; int8 with an INT8 filter handles groups,
 *                         arm_convolve_s8.c:92-99, and stays allowed); and
 *                         (issue #188) Conv2DOptions present, H, W and C of
 *                         the three and both strides at most 65535 (the
 *                         general path holds them in uint16), and output[1] /
 *                         [2] the window formula's ("OUTPUT FRAMES").
 *   R2 DEPTHWISE_CONV_2D  the three rank 4; filter[0] == 1 (the reference
 *                         DCHECK; the kernels read only filter[1..3]);
 *                         filter[3] == output[3];
 *                         depth_multiplier >= 1 (an absent or other options
 *                         table is 0, as TFLM reads it) and input[3] x
 *                         depth_multiplier == filter[3]; a bias of filter[3]
 *                         elements, FLOAT32 / FLOAT32, INT8 / INT32 or
 *                         INT16 / INT64; and (issue #188) output[1] / [2] the
 *                         window formula's, the batch the input's (a policy),
 *                         and, in H and W, (output - 1) x stride at most
 *                         32767 and the padding Prepare() derives (the
 *                         window's total, 0 when negative, halved) at most
 *                         32768 (the kernels' int16 bases, output x stride -
 *                         padding; an output of 0 is 0).
 *   R3 FULLY_CONNECTED    weights rank 2, output rank 2..4; weights[0] == the
 *                         output's last dimension; the input's elements ==
 *                         (the output's other dimensions multiplied) x
 *                         weights[1], no division, a product past 32 bits
 *                         refused before it wraps; a bias of weights[0]
 *                         elements, FLOAT32 / FLOAT32, INT8 / INT32 or
 *                         INT16 / INT64.  keep_num_dims is not read (the read
 *                         sizes follow from the shapes alone).
 *   R4 PAD                the paddings INT32; input rank == output rank (the
 *                         count, 2 x the output's rank, and the input rank <= 4
 *                         are Prepare()'s live checks); and, when the paddings
 *                         have data (issue #188), 2 x the input's rank of them
 *                         (counted before any value is read), each >= 0, and
 *                         input + before + after == output in every dimension,
 *                         added in 64 bits.
 *   R5 STRIDED_SLICE      begin, end and strides INT32, each with the input's
 *                         rank of elements.
 *   R6 TRANSPOSE          the permutation INT32; and, when it has data (issue
 *                         #188), the input's rank (at most
 *                         NN_TFLM_MAX_TENSOR_RANK) of elements before any value,
 *                         each value in [0, rank) and none repeated (a
 *                         permutation: CMSIS-NN's int8 rank-2 path ignores the
 *                         values and writes the input's n x h), the output of
 *                         that rank, and output[i] == input[perm[i]] (so its
 *                         elements are the input's).
 *   R7 RESIZE_BILINEAR /  the size INT32, rank 1, 2 elements, the output rank
 *      _NEAREST_NEIGHBOR  4; when the size has data (#183 has proven it is the
 *                         8 bytes), its two values equal output[1] and
 *                         output[2].  [!] Without data the values are not
 *                         compared: Prepare()'s live "constant" check refuses
 *                         such a model on the board, as NN_MODEL_ERR_ARENA,
 *                         while the gates, which do not plan, pass it -- a
 *                         difference that predates this walk and stays.  And
 *                         (issue #188) the input rank 4, its batch and
 *                         channels the output's (a policy: the kernels loop
 *                         over the smaller).
 *   R8 ADD / SUB / MUL    (two inputs or more: inputs 0 and 1, the only
 *                         ones the kernels read; ADD's and SUB's Prepare()
 *                         do not count the inputs) the three ranks at most
 *                         NN_TFLM_MAX_TENSOR_RANK, each input's at most the
 *                         output's; aligned on the right, each input dimension
 *                         1 or the output's, and at least one of the two the
 *                         output's (the output is their broadcast).
 *   R9 CONCATENATION      every input of the output's rank (at most
 *                         NN_TFLM_MAX_TENSOR_RANK); the axis (absent options:
 *                         0, negative: from the end) in range; every other
 *                         dimension the output's; the axis dimensions, none
 *                         negative, adding up (64 bits) to the output's.
 *
 * R10 .. R13 are issue #188's, "OUTPUT FRAMES" below.  RESHAPE's and CUSTOM's
 * rules are empty, with the reason beside them.
 * How they read: a tensor through one helper (-1 is "none", a null table entry
 * FORMAT); a list's length before its elements -- a list shorter than the kernel
 * needs, or a required tensor that is -1, is left to Prepare()'s live check;
 * a rank before its dimensions, every rank judged by the rule itself (the rank
 * rule, TENSOR, runs later, so a rank-7 convolution input is FORMAT here); an
 * options table's type before its field.  What is NOT added: what Prepare()
 * already refuses with a live TF_LITE_ENSURE (conv's batch, the input-channel
 * division) -- except the counts and values PAD's and TRANSPOSE's rules read
 * (issue #188): a rule that reads values judges their count first, even where
 * Prepare() would too.  The rules compare dimension values and never size work
 * by them (CONCATENATION's at most NN_TFLM_MAX_OP_TENSORS x
 * NN_TFLM_MAX_TENSOR_RANK, the most any rule reads).
 *
 * [!] OUTPUT FRAMES (issue #188).  The planner gives each tensor a frame in the
 * arena of its declared shape x TFLM's element size, and some kernels decide how
 * much they WRITE by something else -- the input's shape, the options, a
 * tensor's values -- comparing it with the output's shape in a TFLITE_DCHECK or
 * MatchingFlatSize() only (NDEBUG on both boards).  Past the frame lies another
 * tensor of the plan, then the allocator's own persistent records (the eval
 * tensors' data / dims pointers, the next kernel writes through them); before
 * it, at the arena's start, the model's slot.  What was read in lib/tflite-micro
 * (CMSIS-NN on), against what the issue assumed:
 *
 *   1. Pool and RESIZE with a batch or channel count other than the output's do
 *      NOT write past it: MatchingDim() returns min(a, b) after its DCHECK
 *      (kernels/internal/types.h), and the reference, integer and CMSIS-NN pools
 *      and both RESIZE kernels loop over that minimum.  A pool writes past its
 *      frame below rank 4, where its loop counts come from past the shape.
 *   2. "A kernel that loops over the output's H and W stays inside it" is false
 *      under CMSIS-NN: CONV_2D's 1x1 fast path sizes its rows by the INPUT's
 *      n x h x w (arm_convolve_1x1_s8_fast.c), and the depthwise opt path
 *      memsets the scratch by an output H past the formula, with its row and
 *      column bases held in int16 (arm_depthwise_conv_s8_opt.c): output x
 *      stride - padding, so a padding past 32768 wraps them as surely as a
 *      stride does.  Hence the window rule below, and R2's two int16 bounds.
 *   3. A size past int is not RESHAPE's alone: the planner multiplies every
 *      tensor's shape in int (memory_helpers.cc), so [1, 65536, 65537, 1] int8
 *      is planned as 64 KB and a pool / conv / pad over its dims writes 4 GB.
 *      Every shape equality below rests on R10.
 *   4. Found here: PAD with a negative padding moves CMSIS-NN arm_pad_s8()'s
 *      write BEFORE the output (Prepare()'s live output == input + before +
 *      after is int: a negative value or a wrapped sum passes); MEAN's float
 *      ResolveAxis() writes one MeanParams::axis[4] entry per axis, unchecked,
 *      on the stack; TRANSPOSE's int8 kernel loops over the input's dims and
 *      writes by the output's strides, and Prepare() never compares the two.
 *
 *   R10 every tensor     no dimension negative (-1 included: TFLM's int size of
 *                        such a shape, rounded up to 16 in size_t, is a 0-byte
 *                        buffer, so the planner would not refuse it), the
 *                        product read in TFLM's order and refused before a
 *                        multiplication would pass INT32_MAX, and elements x
 *                        TfLiteTypeSizeOf() (INT4 1 byte) at most
 *                        NN_TFLM_MAX_TENSOR_BYTES (INT32_MAX - 15, so the size
 *                        rounded up to 16 is an int too).  Every subgraph, every
 *                        profile (grove-vision-ai-v2 included), in the tensor
 *                        loop after the type and before the constant's length.
 *                        A rank above NN_TFLM_MAX_TENSOR_RANK is left to the rank
 *                        rule (TENSOR), neither judged nor counted.
 *   R10b the sum         each tensor entry's size rounded up to 16, in one
 *                        64-bit sum over every subgraph (a shared table counted
 *                        per entry, the larger side), at most
 *                        NN_TFLM_MAX_PLANNED_BYTES (2^30); and every offline-plan
 *                        offset below 2^30 (above).  The greedy planner adds
 *                        offsets and sizes in int, so with both, offset + size
 *                        stays below 2^31.
 *   R11 QUANTIZE /       input 0 and output 0 (neither -1) of at most
 *       DEQUANTIZE /     NN_TFLM_MAX_TENSOR_RANK, with equal element counts (the
 *       RELU / RELU6 /   reference kernels write the INPUT's count; SOFTMAX's
 *       LOGISTIC /       CMSIS-NN kernel writes the input's outer x min(last),
 *       LEAKY_RELU /     inside the output when the counts are equal).  The
 *       SOFTMAX          shapes may differ.
 *   R12a MAX_POOL_2D /   input and output rank 4; R12b, Pool2DOptions present,
 *        AVERAGE_POOL_2D output[1] / [2] the window formula's (filter_height
 *                        / _width, the strides, dilation 1), the batch and the
 *                        channels the input's (a policy, as for RESIZE).
 *   R13 MEAN             the axis (input 1) INT32 and WITH data (without, the
 *                        walk cannot know which dims are reduced: refused), at
 *                        most NN_TFLM_MAX_MEAN_AXES elements counted before any
 *                        value (applied to every type), the input's rank at most
 *                        NN_TFLM_MAX_TENSOR_RANK, each value in [-rank, rank)
 *                        (negative from the end), a repeated axis once; the
 *                        output the input with each axis 1 under keep_dims
 *                        (ReducerOptions; absent or another table: false), else
 *                        the input with the axes removed, ranks equal.  The
 *                        reference kernel adds into positions it builds from the
 *                        input's dims outside the axes.
 *   R4 / R6 (above)      PAD's and TRANSPOSE's values.
 *   the window           (CONV_2D, DEPTHWISE_CONV_2D, the pools; R1 / R2 above)
 *                        TFLM's ComputeOutSize(): eff = (filter - 1) x
 *                        dilation + 1, SAME out = (in + stride - 1) / stride,
 *                        VALID out = (in + stride - eff) / stride.  Stride,
 *                        dilation and filter at least 1, in and the output not
 *                        negative, padding SAME or VALID and the options table
 *                        present (an absent one reads as stride 0), eff, the
 *                        numerator and Prepare()'s padding (out - 1) x stride +
 *                        eff - in inside int32 (computed in 64 bits), a 32-bit
 *                        division truncating as TFLM's does, and the result
 *                        equal to the declared output.  A negative result is
 *                        refused too.
 *
 * RESHAPE needs no rule: under R10 Prepare()'s live element-count compare and
 * the memcpy of the input's bytes are exact (the -1 it would resolve is R10's).
 * NOT bounded here -- what remains: the scratch buffers a kernel requests in its
 * Prepare() (RequestScratchBufferInArena()) are sized by the kernel from the
 * dims, so neither their sizes nor their sum is in R10b; the planner's arena
 * check is what stops them.  [!] The dimension values are read by the size bound
 * and the operator rules' compares only, and no work is sized by them.
 *
 * Structure is checked BEFORE anything else, so a model
 * broken in two ways is FORMAT; then the count (SHAPE), the rank (TENSOR) and the
 * values (DIMS).  On a board without a verifier this is the trust GetModel() and
 * AllocateTensors() already extend to the bytes, with the index bounds now in
 * front of them.
 *
 * [!] LENGTHS BEFORE ELEMENTS: WHAT IT BOUNDS OF THE LOAD'S WORK (issue #178).  The
 * verifier checks a scalar vector (a shape, an operator's tensor list) in O(1)
 * and does not count it as a table, and it accepts many vector entries naming
 * one table: a 535 KB file with one tensor (a 65,000-entry shape) named by 1,900
 * tensor entries and one operator (65,000 inputs) named by 2,000 operator entries
 * passed the 16 / 4096 verifier and the #175 walk, and then cost TFLM ~1.2e8
 * shape reads and ~1.3e8 list reads per pass in AllocateTensors() -- the
 * verifier's table count bounded none of it.  So this walk reads each scalar
 * vector's LENGTH before any element and refuses one that is too long:
 *
 *   per tensor entry    one rank compare (shape()->size()) and, at rank
 *                       NN_TFLM_MAX_TENSOR_RANK or less, at most that many
 *                       dimension values, one multiply-and-compare each, one
 *                       element-size compare and a 64-bit add (R10 / R10b,
 *                       issue #188; Vela's internal tensors reach 2.2 M
 *                       elements, far inside the bound)
 *   per operator entry  three length compares, then at most
 *                       3 x NN_TFLM_MAX_OP_TENSORS index reads, and as many
 *                       scale-vector length reads (issue #180)
 *   per tensor entry    the scale and zero-point vectors' lengths and, when
 *   with quantization   per-channel, the quantized dimension against the rank
 *                       (rank >= 2) or NN_TFLM_MAX_TENSOR_RANK (rank 0 / 1)
 *                       (issue #180; no scale or zero-point VALUE is read)
 *   per tensor entry    one buffer-index compare (issue #181)
 *   per metadata entry  one buffer-index compare, a name-length compare and, for
 *                       the plan, a 23-byte compare, one buffer, one data length
 *                       and one header word (issue #181) -- a metadata entry is a
 *                       table, so one verifier visit each
 *   per tensor, once    for each plan entry, one offset-value compare: the plan
 *   per plan            holds exactly one offset per tensor of the model
 *                       (issue #181)
 *   per tensor entry    one type, one buffer and its data length; with data,
 *                       the rank compared, then at most NN_TFLM_MAX_TENSOR_RANK
 *                       dimension values (issue #183)
 *   per operator entry  for each output index, one tensor, one buffer and its
 *                       data length (issue #183)
 *   per operator entry  one opcode_index compare, one operator code's two
 *                       fields, and its rule: at most NN_TFLM_MAX_OP_TENSORS
 *                       tensors (4 but for CONCATENATION), their ranks
 *                       compared before any dimension, at most
 *                       (NN_TFLM_MAX_OP_TENSORS + 1) x NN_TFLM_MAX_TENSOR_RANK
 *                       dimension values, one options field and, for RESIZE,
 *                       two data words (issue #185); for PAD, TRANSPOSE and MEAN
 *                       at most 2 x NN_TFLM_MAX_TENSOR_RANK data words, counted
 *                       before they are read, and for a convolution or a pool
 *                       a few options fields and two window formulas (issue
 *                       #188)
 *
 * Every tensor and operator entry is one table visit of the verifier (a shared
 * subgraph's are visited once per entry naming it), so behind the 16 / 4096
 * verifier the walk reads at most 4096 x 3 x NN_TFLM_MAX_OP_TENSORS = 786,432
 * list elements, and what it passes bounds TFLM's own passes over the SAME
 * vectors -- shapes at most NN_TFLM_MAX_TENSOR_RANK per tensor entry, operator
 * lists the same 3 x 64 per operator entry, so the shape, operator-list and
 * index reads of a load are visits x a small constant.  The same holds for the
 * zero-point copies (issue #180): TFLM copies a tensor's zero points (one int
 * per scale entry) each time a kernel's Prepare() takes the tensor, frees the
 * copy per node, and so is stopped by neither the arena nor the verifier (a
 * quantization table is one more visit): one 65,000-entry scale vector named by
 * 2,000 tensor entries, or one 4,096-entry tensor named by 4,000 operator
 * entries, passed the verifier and the #178 walk.  After this walk the scale
 * vector of any tensor is at most NN_TFLM_MAX_QUANT_CHANNELS long and the
 * lengths named by all operator entries add up to at most
 * NN_TFLM_MAX_QUANT_REFS, so TFLM's zero-point copies total at most
 * NN_TFLM_MAX_QUANT_REFS per load (times the small number of times a kernel
 * takes one input again), plus 2 x NN_MAX_IO x NN_TFLM_MAX_QUANT_CHANNELS for
 * subgraph 0's I/O.  [!] NOT the kernels' own per-channel work: conv /
 * depthwise Prepare() size their multiplier arrays and loops by the filter's
 * dimension VALUE (a per-tensor scale is broadcast), and Invoke() loops over
 * planned tensors -- both bounded by the arena they allocate from, not by the
 * file and not by this walk.  That is not all of
 * AllocateTensors(): MarkAllocationLifetimes() walks every scratch request for
 * each operator (O(N x M), TFLM's own comment) and the greedy memory planner is
 * quadratic in its buffers, both counted in tensors and operators, so the whole
 * load is bounded by visits squared x small constants, not visits x a constant.
 *
 * [!] THE ONE EXCEPTION TO "LENGTHS FIRST": subgraph 0's two I/O lists are
 * walked once by nn_tflm_io_list_ok() BEFORE their count is judged (per entry
 * one index compare, then its tensor's buffer and data length, issue #183;
 * O(the list's length): at most ~128K entries in a 512 KB file, read once, never
 * multiplied by sharing).  It runs after every subgraph's tensors (which prove
 * the buffer indices it follows) and before the count, so a model broken in two
 * ways stays FORMAT rather than SHAPE.
 *
 * wio-lite-ai runs no verifier: there nothing bounds the number
 * of entries but the file, the trust described above.  (grove-vision-ai-v2's payload
 * walk per operator entry is bounded by its own scan, issue #179.)
 *
 * THE VERIFIER (moved here from svc/nn/ethosu/npu_verify.h, which delegates to
 * it).  flatbuffers' defaults are depth 64 and a million tables; the limits here
 * are 16 and 4096, with the nested-flatbuffer check off (a no-op for this schema:
 * neither pinned schema_generated.h has a nested_flatbuffer field).  What each
 * limit bounds -- the reasoning this file used to state differently:
 *
 *   [!] DEPTH IS BOUNDED BY THE SCHEMA, NOT BY max_depth.  The generated table
 *   verifiers are type-directed: Tensor::Verify() calls
 *   QuantizationParameters::Verify() because the schema says so, whatever the
 *   bytes are, and only a table start counts a level.  Both pinned schemas
 *   (lib/tflite-micro's and grove-vision-ai-v2's SDK tflmtag2412 schema.fbs,
 *   tensorflow/compiler/mlir/lite/schema/) have no cycle among tables; the
 *   longest chain is 6: Model > SubGraph > Tensor > SparsityParameters >
 *   DimensionMetadata > Int32Vector.  So no buffer, valid or hostile, drives the
 *   recursion past 6 and max_depth 16 is not what bounds the stack: it is the
 *   guard for a future schema that grows a cycle.  Reproduce with
 *   cmake/fixtures/model_io/schema_table_depth.py <schema.fbs> (union-aware; the
 *   boards' host tests run it on the schema each gate was built from and fail
 *   if a cycle appears or the chain exceeds NN_TFLM_VERIFY_MAX_DEPTH).
 *
 *   max_tables IS THE VERIFIER'S OWN WORK BOUND, and through the walk the load's
 *   (see "LENGTHS BEFORE ELEMENTS" above): the number of table VERIFICATION
 *   VISITS (each table start counts once per visit, not every byte the verifier
 *   reads).
 *   flatbuffers does not detect shared sub-tables, so a small crafted buffer can
 *   point many vector entries at one table at several levels and multiply the
 *   visits; a million would let a <= 512 KB file keep a console verifying for a
 *   long time.  4096 is a compatibility policy over the models found, not a proof
 *   for every model: measured on the host, every pinned and SDK model needs depth
 *   4 and at most 785 tables (TFLM mobilenet_v2 224, 3.98 MB; blazeface int8 640,
 *   a Vela model 19..697), so 4096 leaves >= 5x.  A legitimate model above it is
 *   refused as malformed -- NN_MODEL_ERR_FORMAT on the board, the verifier's
 *   REJECT on the host gates.
 *
 *   STACK.  The deepest verifier chain is the schema's, the same under any
 *   limit: measured (-fstack-usage, the boards' own compile lines) about 300 B
 *   below the caller on f746g-disco (Model 72, SubGraph 48, Tensor 40, Sparsity
 *   48, DimensionMetadata 40, SparseIndexVector 28, Int32Vector 24), and on
 *   grove-vision-ai-v2 24 B frames or less along the same chain (its deepest
 *   recorded path, a FLATBUFFERS_ASSERT reaching fprintf, is 740 B) -- not the
 *   "~4.1 KB at depth 64" an earlier comment derived by assuming depth followed
 *   the data.  The whole `nn model load` chain down this branch on f746g-disco
 *   is about 1.4 KB of the console's 4,096 B (1.6 KB with the 208 B exception
 *   reserve; the board's README has the figures).
 *
 * [!] ONE DEFINITION, FIVE CALLERS: svc/nn/tflm/nn_tflm.cc (wio-lite-ai,
 * f746g-disco), svc/nn/ethosu/nn_ethosu.cc (grove-vision-ai-v2), and the host gates
 * that exist to reach the board's verdict before a transfer -- both boards'
 * scripts/verify_tflite.cc and grove-vision-ai-v2's scripts/verify_vela_model.cc.
 * A gate that passed what the board refuses would only have moved the failure to
 * the serial line.  Each host rule lists this header (and nn.h) in its DEPENDS.
 * The verifier has four of them: nn_tflm.cc under NN_TFLM_VERIFY (f746g-disco;
 * wio-lite-ai builds it out), npu_verify.h (grove-vision-ai-v2's firmware and
 * verify_vela_model, behind its length floor) and both verify_tflite.
 *
 * C++ (the schema's accessors, TfLiteTensor).  Forced inline (below), no state, no
 * storage; the including function decides where the code lands.
 */
#ifndef NN_TFLM_MODEL_CHECK_H
#define NN_TFLM_MODEL_CHECK_H

#include "flatbuffers/verifier.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/c/common.h"
/* RuntimeShape::kMaxSmallSize, the rank NN_TFLM_MAX_TENSOR_RANK is tied to. */
#include "tensorflow/lite/kernels/internal/runtime_shape.h"

#include "nn.h"   /* NN_MODEL_ERR_*, NN_MAX_IO / NN_MAX_DIMS, nn_tensor_dim_fits() */
#include "nn_tflm_ops.h"   /* NN_TFLM_OPS_ALL: the operators the rules switch over (issue #185) */

#include <stddef.h>
#include <stdint.h>
#include <string.h>   /* memcmp */

/* [!] FORCED INLINE.  Left to the inliner, grove-vision-ai-v2's build outlined the
 * two list helpers into .text, which is ITCM there, and reached them through
 * veneers from its .text_cold load path (that board's README, "Cold code in
 * SRAM").  Inlined, every byte of this check is in the calling function, wherever
 * the caller's board places that. */
#define NN_TFLM_CHECK_INLINE static inline __attribute__((always_inline))

/* [!] THE ONE EXCEPTION: the operator rules' helpers (issue #185).  They are
 * compiled only under a profile that registers a CPU operator -- the ethos-u
 * profile has CUSTOM's empty rule alone, which stays forced inline -- so none of
 * them reaches grove-vision-ai-v2's ITCM; forced inline, each rule got its own
 * copy of them and wio-lite-ai's Flash paid 1.2 KB more.  Kept out of line
 * (noinline): left to the inliner they still cost 272 B more there.  The rules
 * R4 .. R9 (step 2) and R11 .. R13 (issue #188) are spelled the same way; R10 /
 * R10b run in the tensor loop, which grove-vision-ai-v2 compiles, and stay
 * forced inline. */
#define NN_TFLM_RULE_HELPER static inline __attribute__((noinline))

/** Verifier limits: nesting levels (schema-bounded at 6, see the file header) and
 *  table verification visits (the work bound; measured <= 785). */
#define NN_TFLM_VERIFY_MAX_DEPTH   16u
#define NN_TFLM_VERIFY_MAX_TABLES  4096u

/*
 * The longest tensor list (inputs, outputs or intermediates) one operator may
 * carry (issue #178).  TFLM has no such constant (no kMaxTensorsPerOp in either
 * pinned tree), so it is derived: measured over every pinned, SDK, TFLM-bundled
 * and MLPerf model on the host, the longest list is 24 (inputs of
 * UNIDIRECTIONAL_SEQUENCE_LSTM, which no profile registers) and 9 among
 * operators a profile registers (an ethos-u operator of the SDK's yolo11); the
 * schema's largest fixed arity, BIDIRECTIONAL_SEQUENCE_LSTM, is 48 inputs.  64
 * passes all of them; a list past it is no model this runtime runs and is
 * refused as NN_MODEL_ERR_FORMAT, as the verifier's own limit is.
 */
#define NN_TFLM_MAX_OP_TENSORS     64u

/*
 * The highest rank any tensor may declare (issue #178): TFLM's
 * RuntimeShape::kMaxSmallSize (tensorflow/lite/kernels/internal/runtime_shape.h),
 * the size of the fixed dims_[] array kernels copy a shape into.  TFLM checks it
 * with TFLITE_DCHECK only, which a release build compiles out, so a rank-7 tensor
 * would overrun that array.  Only the rank is read by this rule: the dimension values
 * of a tensor without data are not judged by it (a constant's are multiplied out
 * against its buffer, issue #183, and a constant of a higher rank is FORMAT; the
 * size bound multiplies them at this rank or less, issue #188; the operator
 * rules compare some operators' tensors' values after checking their ranks
 * themselves, issue #185).
 * Subgraph 0's I/O keeps the tighter NN_MAX_DIMS, the descriptor's.
 */
#define NN_TFLM_MAX_TENSOR_RANK    6u

/*
 * The longest per-channel quantization one tensor may carry (issue #180): its
 * scale vector's length, the `channels` TFLM's allocator copies a zero point for
 * (micro_allocator.cc, InitializeTfLiteTensorFromFlatbuffer(): an int array of
 * that length in the arena, filled element by element).  TFLM has no such
 * constant and the verifier checks a scalar vector in O(1), so it is derived:
 * measured over the 43 models found on the host that parse (the pinned assets,
 * the Grove SDK's model zoo, TFLM's bundled models, MLPerf Tiny and a few ST
 * model zoo files), the longest scale vector is 1,280 (TFLM's mobilenet_v2 224
 * and ST's mobilenetv2_a035_128, a final FULLY_CONNECTED's weights), the pinned blazeface
 * int8 96, and Vela output carries none at all (every Vela model's scale vectors
 * are 1 long).  4096 passes all of them with 3.2x; a longer one is refused as
 * NN_MODEL_ERR_FORMAT.  The zero-point vector is 0, 1 or the scale's length (TFLM
 * reads zero_point[0] for every channel when the two differ, silently), so the
 * same cap bounds it.
 */
#define NN_TFLM_MAX_QUANT_CHANNELS 4096u

/*
 * The sum, over every subgraph and every operator entry, of the scale-vector
 * lengths of the tensors the operator's inputs / outputs / intermediates name
 * (issue #180): a bound on TFLM's zero-point COPIES (not on the kernels' own
 * per-channel work, which the arena bounds -- see the file header).  The cap above bounds one
 * tensor; TFLM copies a tensor's zero points again each time a kernel's Prepare()
 * asks for it (AllocateTempInputTensor() and friends, micro_context.cc; the temp
 * copy is freed per node, so the arena never runs out and nothing else stops
 * it), and many operator entries may name one tensor, so per-tensor alone left
 * operators x NN_TFLM_MAX_QUANT_CHANNELS.  Measured over the same 43 models: the largest sum
 * is 34,292 (TFLM's 3.98 MB mobilenet_v2, which fits no slot), 14,233 for a model
 * that fits one, 4,539 for the pinned blazeface int8.  65536 passes all of them
 * with 1.9x over the largest; a model above it is refused as
 * NN_MODEL_ERR_FORMAT.  Subgraph 0's I/O tensors, which TFLM copies once into the
 * persistent arena, are at most 2 x NN_MAX_IO x NN_TFLM_MAX_QUANT_CHANNELS more.
 */
#define NN_TFLM_MAX_QUANT_REFS     65536u

/*
 * The largest tensor, in bytes, TFLM's planner can be handed (issue #188, R10):
 * INT32_MAX - 15.  The planner computes a tensor's bytes as an int product
 * (memory_helpers.cc, TfLiteEvalTensorByteLength() / BytesRequiredForTensor()),
 * rounds it up to 16 (micro_allocator.cc, AlignSizeUp(), the 16 of
 * MicroArenaBufferAlignment()) and passes the result to AddBuffer(int size), so
 * a tensor past this wraps somewhere on that way -- [1, 65536, 65537, 1] int8
 * is a 64 KB buffer to the planner, and a kernel that loops over the dimensions
 * writes 4 GB into it.
 */
#define NN_TFLM_MAX_TENSOR_BYTES   0x7FFFFFF0u

/*
 * The sum, over every tensor entry of every subgraph, of those sizes rounded up
 * to 16, and the bound on an offline plan's offsets (issue #188, R10b): 2^30.
 * The greedy planner adds offsets and sizes in int (greedy_memory_planner.cc,
 * GetMaximumMemorySize()), so one tensor's bound alone lets the sum wrap; with
 * every size at most 2^30 in all, every online offset is at most that sum and
 * every offline one below 2^30, offset + size stays below 2^31.  A bound for
 * int arithmetic, not an arena size: no board's arena is above 512 KB and the
 * planner still judges the fit.
 */
#define NN_TFLM_MAX_PLANNED_BYTES  0x40000000u

/*
 * The most axes MEAN may name (issue #188, R13): the float kernel's
 * ResolveAxis() writes each one into MeanParams::axis[4] (reduce_common.cc,
 * types.h) with no count check, on the stack.
 */
#define NN_TFLM_MAX_MEAN_AXES      4u

static_assert(NN_TFLM_MAX_PLANNED_BYTES <= NN_TFLM_MAX_TENSOR_BYTES,
              "the sum's bound cannot exceed one tensor's");
static_assert(NN_TFLM_MAX_QUANT_CHANNELS <= NN_TFLM_MAX_QUANT_REFS,
              "one tensor's channels must fit the per-load sum");
static_assert(NN_TFLM_MAX_TENSOR_RANK == (unsigned)tflite::RuntimeShape::kMaxSmallSize,
              "the rank bound is TFLM's RuntimeShape capacity in the tree built against");
static_assert((unsigned)NN_MAX_DIMS <= NN_TFLM_MAX_TENSOR_RANK,
              "an I/O tensor's rank bound cannot exceed every tensor's");

/*
 * Does @p len bytes at @p buf hold a .tflite flatbuffer that is safe to follow,
 * within the limits above?  Nothing here says the model MEANS anything; it says
 * every offset lands inside the buffer, which has to be true before the first
 * accessor after tflite::GetModel() (a cast) follows one.  The caller has checked
 * that @p len covers the identifier it reads at offset 4.
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_verify_model_buffer(const uint8_t *buf, size_t len)
{
	flatbuffers::Verifier::Options opts;

	opts.max_depth                = NN_TFLM_VERIFY_MAX_DEPTH;
	opts.max_tables               = NN_TFLM_VERIFY_MAX_TABLES;
	opts.check_nested_flatbuffers = false;

	flatbuffers::Verifier v(buf, len, opts);

	return tflite::VerifyModelBuffer(v);
}

/* A subgraph's tensor vector, as the schema types it. */
typedef flatbuffers::Vector<flatbuffers::Offset<tflite::Tensor>> nn_tflm_tensor_vec;

/* An operator's tensor list (its length already within NN_TFLM_MAX_OP_TENSORS)
 * names only tensors of its subgraph -- or -1, TFLM's optional tensor, where
 * @p optional_ok (inputs). */
NN_TFLM_CHECK_INLINE bool nn_tflm_op_list_indices_ok(const flatbuffers::Vector<int32_t> *list,
                                              uint32_t ntensors, bool optional_ok)
{
	for (uint32_t j = 0; list && j < list->size(); j++) {
		const int32_t ti = list->Get(j);

		if (ti == -1 && optional_ok)
			continue;
		if (ti < 0 || (uint32_t)ti >= ntensors)
			return false;
	}
	return true;
}

/* A subgraph's operator vector, as the schema types it. */
typedef flatbuffers::Vector<flatbuffers::Offset<tflite::Operator>> nn_tflm_op_vec;

/* SubGraph::operators(), spelled through Table::GetPointer<const void *>: the
 * same read of the same field (the offset field's target, or null when absent);
 * a generated table IS a flatbuffers::Table at the same address (a private base,
 * which is how GetRoot<> reaches it too).  [!] grove-vision-ai-v2: the generated
 * accessor, called from here as well as from SubGraph::Verify(), was outlined
 * into .text (ITCM, +16 B); the void instance is one that build already outlines
 * for other callers, so the walk adds no ITCM byte (issue #178; the board's
 * README, "Cold code in SRAM"). */
NN_TFLM_CHECK_INLINE const nn_tflm_op_vec *nn_tflm_sg_operators(const tflite::SubGraph *sg)
{
	const flatbuffers::Table *t = reinterpret_cast<const flatbuffers::Table *>(sg);

	return static_cast<const nn_tflm_op_vec *>(
		t->GetPointer<const void *>(tflite::SubGraph::VT_OPERATORS));
}

/* An optional tensor list's length, 0 when it is absent. */
NN_TFLM_CHECK_INLINE uint32_t nn_tflm_list_len(const flatbuffers::Vector<int32_t> *list)
{
	return list ? list->size() : 0u;
}

/* Tensor::quantization(), spelled through Table::GetPointer<const void *> for the
 * same reason as nn_tflm_sg_operators() above: [!] grove-vision-ai-v2's build
 * outlined the generated accessor into .text (ITCM, +16 B and a veneer) once the
 * walk called it (issue #180); the void instance adds no ITCM byte. */
NN_TFLM_CHECK_INLINE const tflite::QuantizationParameters *
nn_tflm_tensor_quant(const tflite::Tensor *t)
{
	const flatbuffers::Table *tb = reinterpret_cast<const flatbuffers::Table *>(t);

	return static_cast<const tflite::QuantizationParameters *>(
		tb->GetPointer<const void *>(tflite::Tensor::VT_QUANTIZATION));
}

/* Tensor::shape(), spelled the same way and for the same reason: called from the
 * quantization check as well, the generated accessor was outlined into .text
 * (ITCM, +8 B and a veneer) on grove-vision-ai-v2 (issue #180). */
NN_TFLM_CHECK_INLINE const flatbuffers::Vector<int32_t> *
nn_tflm_tensor_shape(const tflite::Tensor *t)
{
	const flatbuffers::Table *tb = reinterpret_cast<const flatbuffers::Table *>(t);

	return static_cast<const flatbuffers::Vector<int32_t> *>(
		tb->GetPointer<const void *>(tflite::Tensor::VT_SHAPE));
}

/* A tensor's scale-vector LENGTH, 0 when it carries no quantization or no scale
 * -- the channels TFLM would copy for it (it copies only when the zero-point
 * vector is non-empty too; counting every scale vector is the larger, safe
 * side).  No element is read. */
NN_TFLM_CHECK_INLINE uint32_t nn_tflm_quant_len(const tflite::Tensor *t)
{
	const tflite::QuantizationParameters *q = t ? nn_tflm_tensor_quant(t) : nullptr;

	return (q && q->scale()) ? q->scale()->size() : 0u;
}

/*
 * One tensor's quantization holds together (issue #180), by LENGTHS only: the
 * scale vector is at most NN_TFLM_MAX_QUANT_CHANNELS long; the zero-point vector
 * is absent, 1 long or exactly the scale's length (TFLM otherwise reads
 * zero_point[0] for every channel, silently, and the kernels' equal-length check
 * runs on the copy TFLM resized); and a per-channel tensor (scale longer than 1)
 * names a quantized dimension inside its declared rank when that rank is 2 or
 * more (0 <= qd < shape()->size()), or 0 <= qd < NN_TFLM_MAX_TENSOR_RANK when the
 * rank is 0 or 1.  TFLM copies qd unchecked, aliases a tensor's dims to the
 * serialized shape vector (no padding, micro_allocator.cc), and the filter's
 * kernels (kernel_util.cc PopulateConvolutionQuantizationParams(),
 * fully_connected_common.cc) read dims->data[qd] without checking the rank, so
 * qd past the rank reads past the shape.  Rank 0 / 1 is excepted because the
 * converter emits rank-1 per-channel BIAS tensors with the filter's qd (TFLM's
 * person_detect: 14 of them, scale 8..256 long, qd 3), which no kernel indexes
 * by qd -- a filter is never rank 1, but the walk cannot tell the roles without
 * the operators' semantics; for those, qd stays a small constant.  A per-tensor
 * quantization (scale 0 or 1 long) may name any qd: nothing reads it.  Neither the scale values nor the
 * dimension values are read: whether the scale's length matches dims[qd] is the
 * kernels' check (they make it), and a rank above NN_TFLM_MAX_TENSOR_RANK is
 * TENSOR, judged later.
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_tensor_quant_ok(const tflite::Tensor *t)
{
	const tflite::QuantizationParameters *q = t ? nn_tflm_tensor_quant(t) : nullptr;

	if (!q)
		return true;

	const uint32_t sl = q->scale() ? q->scale()->size() : 0u;
	const uint32_t zl = q->zero_point() ? q->zero_point()->size() : 0u;

	if (sl > NN_TFLM_MAX_QUANT_CHANNELS)
		return false;
	if (zl > 1u && zl != sl)
		return false;
	if (sl > 1u) {
		const int32_t qd = q->quantized_dimension();
		const flatbuffers::Vector<int32_t> *shape = nn_tflm_tensor_shape(t);
		const uint32_t rank = shape ? shape->size() : 0u;
		const uint32_t qd_end = rank >= 2u ? rank : NN_TFLM_MAX_TENSOR_RANK;

		if (qd < 0 || (uint32_t)qd >= qd_end)
			return false;
	}
	return true;
}

/* Add the scale-vector lengths of the tensors @p list names (its indices already
 * checked; -1 names none) to @p *qrefs, false once the sum passes
 * NN_TFLM_MAX_QUANT_REFS.  One length read per index.  Each addend is at most
 * NN_TFLM_MAX_QUANT_CHANNELS (the operator's subgraph's tensors passed
 * nn_tflm_tensor_quant_ok() first) and the sum stops just past the limit, so it
 * cannot wrap. */
NN_TFLM_CHECK_INLINE bool nn_tflm_op_list_quant_add(const flatbuffers::Vector<int32_t> *list,
                                             const nn_tflm_tensor_vec *tensors,
                                             uint32_t *qrefs)
{
	for (uint32_t j = 0; list && j < list->size(); j++) {
		const int32_t ti = list->Get(j);

		if (ti < 0)
			continue;
		*qrefs += nn_tflm_quant_len(tensors->Get((uint32_t)ti));
		if (*qrefs > NN_TFLM_MAX_QUANT_REFS)
			return false;
	}
	return true;
}

/* The model's buffer and metadata vectors, as the schema types them. */
typedef flatbuffers::Vector<flatbuffers::Offset<tflite::Buffer>> nn_tflm_buffer_vec;
typedef flatbuffers::Vector<flatbuffers::Offset<tflite::Metadata>> nn_tflm_metadata_vec;

/* Model::buffers() / metadata() / operator_codes(), spelled through
 * Table::GetPointer<const void *> for the reason nn_tflm_sg_operators() gives:
 * [!] grove-vision-ai-v2's build outlined each generated accessor into .text
 * (ITCM, +20 B and a veneer each) once the walk called it (issue #181). */
NN_TFLM_CHECK_INLINE const void *nn_tflm_model_field(const tflite::Model *model,
                                                    flatbuffers::voffset_t field)
{
	const flatbuffers::Table *t = reinterpret_cast<const flatbuffers::Table *>(model);

	return t->GetPointer<const void *>(field);
}

/* Entry @p i (below the vector's size, which the caller has checked) of a
 * vector of tables: the read Vector<Offset<T>>::Get() makes -- the element is an
 * offset relative to its own slot -- spelled out because [!] grove-vision-ai-v2
 * outlined Get() into .text (ITCM, +48 B and a veneer) for each new table type
 * the walk indexed, Buffer and Metadata (issue #181).  Get()'s
 * FLATBUFFERS_ASSERT on the index is the caller's range check here. */
template <typename T>
NN_TFLM_CHECK_INLINE const T *nn_tflm_vec_table(
	const flatbuffers::Vector<flatbuffers::Offset<T>> *v, uint32_t i)
{
	const uint8_t *p = v->Data() + (size_t)i * sizeof(flatbuffers::uoffset_t);

	return reinterpret_cast<const T *>(p + flatbuffers::ReadScalar<flatbuffers::uoffset_t>(p));
}

/* Tensor::type(), spelled through Table::GetField<int8_t> -- the same read
 * of the same field with the schema's default (FLOAT32, 0) -- for the reason
 * nn_tflm_sg_operators() gives: [!] grove-vision-ai-v2 keeps no generated
 * accessor the walk calls out of line in .text (ITCM) (issue #183). */
NN_TFLM_CHECK_INLINE int nn_tflm_tensor_type(const tflite::Tensor *t)
{
	const flatbuffers::Table *tb = reinterpret_cast<const flatbuffers::Table *>(t);

	return tb->GetField<int8_t>(tflite::Tensor::VT_TYPE, 0);
}

/* Tensor::buffer(), spelled through Table::GetField<int32_t> (default 0) and
 * cast back: the same four bytes of the same field.  [!] grove-vision-ai-v2:
 * once the walk called it from three places, the generated accessor was
 * outlined into .text (ITCM, +16 B and a veneer), and so was GetField<uint32_t>
 * when spelled with the field's own type; the int32_t instance is one that
 * build already outlines for other callers, so this adds no ITCM byte
 * (issue #183). */
NN_TFLM_CHECK_INLINE uint32_t nn_tflm_tensor_buffer(const tflite::Tensor *t)
{
	const flatbuffers::Table *tb = reinterpret_cast<const flatbuffers::Table *>(t);

	return (uint32_t)tb->GetField<int32_t>(tflite::Tensor::VT_BUFFER, 0);
}

/*
 * A tensor type's element size in HALF bytes (issue #183): TFLM's
 * TfLiteTypeSizeOf() (memory_helpers.cc, the same in both pinned trees) doubled,
 * so that INT4 -- 1 byte per element to that function, but stored as a constant
 * two elements per byte and read so by the kernels (UnpackDenseInt4IntoInt8(),
 * ceil(n / 2) bytes) -- has a size too.  0 for a type TFLM gives no size
 * (STRING, VARIANT, a value neither pinned schema names): TFLM's planner calls
 * TfLiteEvalTensorByteLength() on every tensor and fails AllocateTensors() on
 * one, which the board reports as NN_MODEL_ERR_ARENA and the wio-lite-ai /
 * f746g-disco gates, which never plan, would have passed.  Only the enum values
 * both pinned schemas share are named.
 */
NN_TFLM_CHECK_INLINE uint32_t nn_tflm_elem_bytes2(int type)
{
	switch (type) {
	case tflite::TensorType_INT4:
		return 1u;
	case tflite::TensorType_UINT8:
	case tflite::TensorType_INT8:
	case tflite::TensorType_BOOL:
		return 2u;
	case tflite::TensorType_FLOAT16:
	case tflite::TensorType_BFLOAT16:
	case tflite::TensorType_INT16:
	case tflite::TensorType_UINT16:
		return 4u;
	case tflite::TensorType_FLOAT32:
	case tflite::TensorType_INT32:
	case tflite::TensorType_UINT32:
	case tflite::TensorType_RESOURCE:
		return 8u;
	case tflite::TensorType_FLOAT64:
	case tflite::TensorType_INT64:
	case tflite::TensorType_UINT64:
	case tflite::TensorType_COMPLEX64:
		return 16u;
	case tflite::TensorType_COMPLEX128:
		return 32u;
	default:
		return 0u;
	}
}

/* The data vector TFLM points tensor @p t at, or null when it has none: the
 * test of GetFlatbufferTensorBuffer() (micro_allocator.cc) -- a Buffer table,
 * a data vector, and a size other than 0 -- on buffer @p bi, which the caller
 * has checked is below @p buffers' size (so @p buffers is present). */
NN_TFLM_CHECK_INLINE const flatbuffers::Vector<uint8_t> *
nn_tflm_buffer_data(const nn_tflm_buffer_vec *buffers, uint32_t bi)
{
	const tflite::Buffer *b = nn_tflm_vec_table(buffers, bi);
	const flatbuffers::Vector<uint8_t> *d = b ? b->data() : nullptr;

	return (d && d->size() != 0u) ? d : nullptr;
}

/*
 * A constant's data is exactly as long as its declared shape says (issue #183):
 * the element count -- 1 for a missing or empty shape, as TFLM reads a scalar,
 * else the product of the dimensions -- times the element size, ceil(n / 2) for
 * INT4, equal to @p size, the data vector's length.  TFLM points the tensor at
 * the data without comparing the two (micro_allocator.cc; memory_helpers.cc's
 * byte counts multiply the shape in int and never look at the buffer), and a
 * kernel that reads a constant by its own dims, which are the serialized shape,
 * reads a short buffer under a large shape past its end (a constant read by
 * another tensor's dims is the operator rules', issue #185).  A longer buffer is refused too
 * (a policy: every constant measured is exact, and Grove's payload scan holds
 * its command stream to the same equality).  Lengths before elements: a rank
 * above NN_TFLM_MAX_TENSOR_RANK is refused before any dimension is read (TENSOR
 * judges ranks later, but a constant's dimensions are read here, so it is FORMAT
 * here); a negative dimension is refused; the product is bounded BEFORE each
 * multiplication by the elements @p size can hold (cap, clamped to 32 bits,
 * which refuses only more), so it never wraps -- in int, as TFLM computes it, a
 * [65537, 65537] int8 constant is 131,073 bytes.  At most
 * NN_TFLM_MAX_TENSOR_RANK values read.  @p eb2 is nn_tflm_elem_bytes2() of the
 * tensor's type, not 0.
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_const_len_ok(const tflite::Tensor *t, uint32_t eb2,
                                          uint32_t size)
{
	const flatbuffers::Vector<int32_t> *shape = nn_tflm_tensor_shape(t);
	const uint32_t rank = shape ? shape->size() : 0u;

	if (rank > NN_TFLM_MAX_TENSOR_RANK)
		return false;

	/* eb2 is 1 (INT4) or an even size: 32-bit divisions only (a 64-bit one is
	 * a library call on the boards). */
	const uint32_t cap = eb2 == 1u ? (size > UINT32_MAX / 2u ? (uint32_t)UINT32_MAX : 2u * size)
	                               : size / (eb2 / 2u);
	uint32_t n = 1u;

	for (uint32_t d = 0; d < rank; d++) {
		const int32_t v = shape->Get(d);

		if (v < 0)
			return false;
		if (v != 0 && n > cap / (uint32_t)v)
			return false;
		n *= (uint32_t)v;
	}
	return ((uint64_t)n * eb2 + 1u) / 2u == size;
}

/*
 * R10 and R10b (issue #188; the file header, "OUTPUT FRAMES"): the size TFLM's
 * planner gives tensor @p t fits an int, and the sum @p *planned of every size
 * so far stays at most NN_TFLM_MAX_PLANNED_BYTES.  The dimensions are read in
 * TFLM's order, from the first; a negative one is refused (TFLM's int product
 * of a shape with a -1 is negative, which the 16-byte rounding in size_t turns
 * into a buffer of 0 bytes, so the planner would not refuse it), and the product
 * is refused BEFORE a multiplication would pass INT32_MAX, so no partial
 * product wraps either ([65536, 65536, 0] is refused although it ends at 0).
 * Then the elements times TFLM's own element size -- TfLiteTypeSizeOf(): INT4
 * is 1 byte here, not the half byte of a packed constant -- at most
 * NN_TFLM_MAX_TENSOR_BYTES, so the size rounded up to 16 is an int too.  A rank
 * above NN_TFLM_MAX_TENSOR_RANK is left to the rank rule (TENSOR, later) and
 * neither judged nor counted.  At most NN_TFLM_MAX_TENSOR_RANK values read, two
 * 32-bit divisions; @p eb2 is nn_tflm_elem_bytes2() of the tensor's type, not 0.
 * Forced inline like the rest of the tensor loop: grove-vision-ai-v2 compiles it.
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_tensor_size_ok(const tflite::Tensor *t, uint32_t eb2,
                                             uint64_t *planned)
{
	const flatbuffers::Vector<int32_t> *shape = nn_tflm_tensor_shape(t);
	const uint32_t rank = shape ? shape->size() : 0u;

	if (rank > NN_TFLM_MAX_TENSOR_RANK)
		return true;

	uint32_t n = 1u;

	for (uint32_t d = 0; d < rank; d++) {
		const int32_t v = shape->Get(d);

		if (v < 0)
			return false;
		if (v != 0 && n > 0x7FFFFFFFu / (uint32_t)v)
			return false;
		n *= (uint32_t)v;
	}

	const uint32_t ps = (eb2 + 1u) / 2u;   /* TfLiteTypeSizeOf(): INT4 is 1 */

	if (n > NN_TFLM_MAX_TENSOR_BYTES / ps)
		return false;
	*planned += (n * ps + 15u) & ~15u;
	return *planned <= NN_TFLM_MAX_PLANNED_BYTES;
}

/* Does tensor @p ti of @p tensors (both checked) have data -- a tensor TFLM
 * would not plan into the arena but write in place, in the model (issue #183)? */
NN_TFLM_CHECK_INLINE bool nn_tflm_tensor_has_data(const nn_tflm_tensor_vec *tensors,
                                             uint32_t ti,
                                             const nn_tflm_buffer_vec *buffers)
{
	const tflite::Tensor *t = tensors->Get(ti);

	return t && nn_tflm_buffer_data(buffers, nn_tflm_tensor_buffer(t)) != nullptr;
}

/* No tensor an operator's outputs list names (its indices already checked) has
 * data: TFLM's planner does not place a tensor with data (micro_allocation_info.cc,
 * needs_allocating), so the kernel would write its output into the model --
 * PSRAM staging on wio-lite-ai, SDRAM on f746g-disco, the XIP window on
 * grove-vision-ai-v2 (issue #183).  A variable too: the walk does not lean on
 * AllocateVariables() moving it. */
NN_TFLM_CHECK_INLINE bool nn_tflm_op_outputs_no_data(const flatbuffers::Vector<int32_t> *list,
                                                const nn_tflm_tensor_vec *tensors,
                                                const nn_tflm_buffer_vec *buffers)
{
	for (uint32_t j = 0; list && j < list->size(); j++)
		if (nn_tflm_tensor_has_data(tensors, (uint32_t)list->Get(j), buffers))
			return false;
	return true;
}

/* ------------------------------------------------------------------------
 * The operator rules (issue #185; the file header, "OPERATOR RULES").
 * ------------------------------------------------------------------------ */

/* The model's operator-code vector, as the schema types it. */
typedef flatbuffers::Vector<flatbuffers::Offset<tflite::OperatorCode>> nn_tflm_opcode_vec;

/* Operator::opcode_index(), spelled through Table::GetField<int32_t> (default 0)
 * and cast back -- the same four bytes -- for the reason nn_tflm_tensor_buffer()
 * gives: [!] grove-vision-ai-v2 keeps no generated accessor the walk calls out of
 * line in .text (ITCM) (issue #185). */
NN_TFLM_CHECK_INLINE uint32_t nn_tflm_op_opcode_index(const tflite::Operator *op)
{
	const flatbuffers::Table *tb = reinterpret_cast<const flatbuffers::Table *>(op);

	return (uint32_t)tb->GetField<int32_t>(tflite::Operator::VT_OPCODE_INDEX, 0);
}

/* An operator code's kind as GetBuiltinCode() reads it (schema_utils.cc): the
 * larger of builtin_code and deprecated_builtin_code (a converter older than
 * the 127-operator limit wrote only the second).  Both read through
 * Table::GetField, for the same reason. */
NN_TFLM_CHECK_INLINE int32_t nn_tflm_opcode_kind(const tflite::OperatorCode *c)
{
	const flatbuffers::Table *tb = reinterpret_cast<const flatbuffers::Table *>(c);
	const int32_t b = tb->GetField<int32_t>(tflite::OperatorCode::VT_BUILTIN_CODE, 0);
	const int32_t d = tb->GetField<int8_t>(tflite::OperatorCode::VT_DEPRECATED_BUILTIN_CODE, 0);

	return b > d ? b : d;
}

/* An operator's options table when its union type is @p type, else null -- the
 * caller then reads TFLM's defaults (0), as TFLM's parser does when the table is
 * absent.  The type is read before the table, both through Table::GetField /
 * GetPointer. */
NN_TFLM_RULE_HELPER const flatbuffers::Table *nn_tflm_op_options(const tflite::Operator *op,
                                                                 tflite::BuiltinOptions type)
{
	const flatbuffers::Table *tb = reinterpret_cast<const flatbuffers::Table *>(op);

	if (tb->GetField<int8_t>(tflite::Operator::VT_BUILTIN_OPTIONS_TYPE, 0) != (int8_t)type)
		return nullptr;
	return static_cast<const flatbuffers::Table *>(
		tb->GetPointer<const void *>(tflite::Operator::VT_BUILTIN_OPTIONS));
}

/* Entry @p j of operator list @p list (j below the list's length, every index
 * already checked against the subgraph's tensors): *@p t is the tensor, or null
 * for -1, TFLM's optional tensor.  false for a null table entry (FORMAT).  The
 * one way every rule takes a tensor. */
NN_TFLM_RULE_HELPER bool nn_tflm_op_tensor(const flatbuffers::Vector<int32_t> *list, uint32_t j,
                                            const nn_tflm_tensor_vec *tensors,
                                            const tflite::Tensor **t)
{
	const int32_t ti = list->Get(j);

	*t = nullptr;
	if (ti < 0)
		return true;
	*t = tensors->Get((uint32_t)ti);
	return *t != nullptr;
}

/* A tensor's declared rank, 0 for no shape (a scalar, as TFLM reads it). */
NN_TFLM_RULE_HELPER uint32_t nn_tflm_rank(const tflite::Tensor *t)
{
	const flatbuffers::Vector<int32_t> *shape = nn_tflm_tensor_shape(t);

	return shape ? shape->size() : 0u;
}

/* Dimension @p d of @p t, below its rank (the caller has checked the rank). */
NN_TFLM_RULE_HELPER int32_t nn_tflm_dim(const tflite::Tensor *t, uint32_t d)
{
	return nn_tflm_tensor_shape(t)->Get(d);
}

/* *@p n x @p v, false for a negative @p v or a product past 32 bits -- judged
 * BEFORE the multiplication, so it never wraps (a 32-bit division: a 64-bit one
 * is a library call on the boards). */
NN_TFLM_RULE_HELPER bool nn_tflm_count_mul(uint32_t *n, int32_t v)
{
	if (v < 0)
		return false;
	if (v != 0 && *n > UINT32_MAX / (uint32_t)v)
		return false;
	*n *= (uint32_t)v;
	return true;
}

/* The product of dimensions [@p from, @p to) of @p t (to at most its rank,
 * which is at most NN_TFLM_MAX_TENSOR_RANK) in *@p n, false as
 * nn_tflm_count_mul() is. */
NN_TFLM_RULE_HELPER bool nn_tflm_dims_count(const tflite::Tensor *t, uint32_t from, uint32_t to,
                                             uint32_t *n)
{
	*n = 1u;
	for (uint32_t d = from; d < to; d++)
		if (!nn_tflm_count_mul(n, nn_tflm_dim(t, d)))
			return false;
	return true;
}

/* Is (@p in, @p bias) a pair of types the kernels read the bias as?  int8 reads
 * int32, float32 float32, int16 int64 -- and CONV_2D's int16 also int32
 * (@p i16_i32: it picks by the declared type, cmsis_nn/conv.cc Eval(), and so
 * does the reference kernel; DEPTHWISE_CONV_2D and FULLY_CONNECTED read int64
 * with CMSIS-NN on, cmsis_nn/depthwise_conv.cc, fully_connected.cc:388).  Any
 * other pair -- an INT8 bias under an int8 input is read four times its length
 * -- is refused. */
NN_TFLM_RULE_HELPER bool nn_tflm_bias_type_ok(int in, int bias, bool i16_i32)
{
	switch (in) {
	case tflite::TensorType_FLOAT32:
		return bias == tflite::TensorType_FLOAT32;
	case tflite::TensorType_INT8:
		return bias == tflite::TensorType_INT32;
	case tflite::TensorType_INT16:
		return bias == tflite::TensorType_INT64 ||
		       (i16_i32 && bias == tflite::TensorType_INT32);
	default:
		return false;
	}
}

/* A bias (@p b, null when the operator has none) of exactly @p want elements --
 * @p want a dimension value, so a negative one matches no bias -- and of a type
 * the kernel reads for input type @p in.  Its rank is judged first. */
NN_TFLM_RULE_HELPER bool nn_tflm_bias_ok(const tflite::Tensor *b, int32_t want, int in,
                                          bool i16_i32)
{
	if (!b)
		return true;

	const uint32_t r = nn_tflm_rank(b);
	uint32_t n;

	if (r > NN_TFLM_MAX_TENSOR_RANK || want < 0)
		return false;
	if (!nn_tflm_dims_count(b, 0u, r, &n) || n != (uint32_t)want)
		return false;
	return nn_tflm_bias_type_ok(in, nn_tflm_tensor_type(b), i16_i32);
}

/* The tensors a convolution-like operator reads: inputs 0 (the input), 1 (the
 * filter / weights) and 2 (the bias, optional) and output 0.  false for a null
 * table entry (FORMAT); *@p apply false when the rule does not apply -- fewer
 * than 2 inputs or no output, or a required one -1 (Prepare()'s live
 * TF_LITE_ENSURE refuses those).  The lengths are read before any element. */
NN_TFLM_RULE_HELPER bool nn_tflm_op_xfby(const tflite::Operator *op,
                                          const nn_tflm_tensor_vec *tensors,
                                          const tflite::Tensor **x, const tflite::Tensor **f,
                                          const tflite::Tensor **b, const tflite::Tensor **y,
                                          bool *apply)
{
	const flatbuffers::Vector<int32_t> *in = op->inputs();
	const flatbuffers::Vector<int32_t> *out = op->outputs();

	*apply = false;
	*b = nullptr;
	if (in->size() < 2u || out->size() < 1u)
		return true;
	if (!nn_tflm_op_tensor(in, 0u, tensors, x) || !nn_tflm_op_tensor(in, 1u, tensors, f) ||
	    !nn_tflm_op_tensor(out, 0u, tensors, y))
		return false;
	if (in->size() > 2u && !nn_tflm_op_tensor(in, 2u, tensors, b))
		return false;
	*apply = *x && *f && *y;
	return true;
}

/*
 * The window rule (issue #188, step 2; the file header, "OUTPUT FRAMES"): is
 * @p want the output size TFLM's ComputeOutSize() (kernels/padding.h) gives for
 * an input of @p in, a filter of @p filter, @p stride, @p dilation and
 * @p padding (the schema's Padding value)?  The same formula --
 *
 *   eff   = (filter - 1) x dilation + 1
 *   SAME  : out = (in + stride - 1) / stride
 *   VALID : out = (in + stride - eff) / stride
 *
 * -- judged only where TFLM's int arithmetic does not wrap: stride, dilation
 * and filter at least 1 (TFLM reads stride 0 as an output of 0 and an unknown
 * padding as 0, neither a real output), in and want not negative, padding SAME
 * or VALID; eff, the numerator and the padding Prepare() derives, (want - 1) x
 * stride + eff - in, computed in 64 bits (additions and multiplications only)
 * and refused outside int32; then a 32-bit division, truncating toward 0 as
 * TFLM's does, a negative result refused and the result equal to @p want.
 * @p total gets that padding's total as ComputePaddingWithOffset() keeps it (0
 * when negative; Prepare() pads by total / 2), set whatever the answer.
 */
NN_TFLM_RULE_HELPER bool nn_tflm_window_ok(int32_t in, int32_t filter, int32_t stride,
                                           int32_t dilation, int padding, int32_t want,
                                           int64_t *total)
{
	*total = 0;
	if (stride < 1 || dilation < 1 || filter < 1 || in < 0 || want < 0)
		return false;
	if (padding != tflite::Padding_SAME && padding != tflite::Padding_VALID)
		return false;

	const int64_t eff = (int64_t)(filter - 1) * dilation + 1;
	const int64_t num = padding == tflite::Padding_SAME ? (int64_t)in + stride - 1
	                                                    : (int64_t)in + stride - eff;
	const int64_t pad = ((int64_t)want - 1) * stride + eff - in;

	if (eff > INT32_MAX || num < INT32_MIN || num > INT32_MAX ||
	    pad < INT32_MIN || pad > INT32_MAX)
		return false;
	*total = pad > 0 ? pad : 0;

	const int32_t out = (int32_t)num / stride;

	return out >= 0 && out == want;
}

/* The H and W of a convolution or a pool: input [1] / [2], filter height /
 * width @p fh / @p fw, the options' strides and dilations, output [1] / [2]
 * (all three tensors rank 4, checked by the caller).  @p total gets the H and
 * the W padding totals (nn_tflm_window_ok()). */
NN_TFLM_RULE_HELPER bool nn_tflm_window_hw_ok(const tflite::Tensor *x, const tflite::Tensor *y,
                                              int32_t fh, int32_t fw, const flatbuffers::Table *o,
                                              flatbuffers::voffset_t vt_padding,
                                              flatbuffers::voffset_t vt_stride_h,
                                              flatbuffers::voffset_t vt_stride_w,
                                              int32_t dil_h, int32_t dil_w,
                                              int64_t total[2])
{
	const int padding = o->GetField<int8_t>(vt_padding, 0);
	const int32_t sh = o->GetField<int32_t>(vt_stride_h, 0);
	const int32_t sw = o->GetField<int32_t>(vt_stride_w, 0);

	return nn_tflm_window_ok(nn_tflm_dim(x, 1u), fh, sh, dil_h, padding, nn_tflm_dim(y, 1u),
	                         &total[0]) &&
	       nn_tflm_window_ok(nn_tflm_dim(x, 2u), fw, sw, dil_w, padding, nn_tflm_dim(y, 2u),
	                         &total[1]);
}

/* R1 (the file header). */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_CONV_2D(const tflite::Operator *op,
                                                  const nn_tflm_tensor_vec *tensors,
                                                  const nn_tflm_buffer_vec *buffers)
{
	(void)buffers;
	const tflite::Tensor *x, *f, *b, *y;
	bool apply;

	if (!nn_tflm_op_xfby(op, tensors, &x, &f, &b, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (nn_tflm_rank(x) != 4u || nn_tflm_rank(f) != 4u || nn_tflm_rank(y) != 4u)
		return false;
	if (nn_tflm_dim(f, 0u) != nn_tflm_dim(y, 3u))
		return false;
	/* Grouped convolution (filter[3] < input[3]) only on the int8 path that
	 * handles groups: the int16 and INT4-filter kernels size one filter row by
	 * the INPUT's channels (arm_convolve_s16.c:79, arm_convolve_s4.c:89), so
	 * they read groups times past the filter; Prepare() checks only
	 * input[3] % filter[3] == 0. */
	if ((nn_tflm_tensor_type(x) == tflite::TensorType_INT16 ||
	     nn_tflm_tensor_type(f) == tflite::TensorType_INT4) &&
	    nn_tflm_dim(f, 3u) != nn_tflm_dim(x, 3u))
		return false;

	/* Issue #188: H, W and C of the three and the strides at most 65535 (the
	 * CMSIS-NN general path holds them in uint16, arm_convolve_s8.c), and the
	 * output's H and W the window formula's (the 1x1 fast path sizes its
	 * rows by the INPUT's n x h x w). */
	const flatbuffers::Table *o = nn_tflm_op_options(op, tflite::BuiltinOptions_Conv2DOptions);
	int64_t pad[2];

	if (!o)
		return false;
	for (uint32_t d = 1; d < 4u; d++)
		if (nn_tflm_dim(x, d) > 65535 || nn_tflm_dim(f, d) > 65535 ||
		    nn_tflm_dim(y, d) > 65535)
			return false;
	if (o->GetField<int32_t>(tflite::Conv2DOptions::VT_STRIDE_H, 0) > 65535 ||
	    o->GetField<int32_t>(tflite::Conv2DOptions::VT_STRIDE_W, 0) > 65535)
		return false;
	if (!nn_tflm_window_hw_ok(x, y, nn_tflm_dim(f, 1u), nn_tflm_dim(f, 2u), o,
	                          tflite::Conv2DOptions::VT_PADDING,
	                          tflite::Conv2DOptions::VT_STRIDE_H,
	                          tflite::Conv2DOptions::VT_STRIDE_W,
	                          o->GetField<int32_t>(tflite::Conv2DOptions::VT_DILATION_H_FACTOR, 1),
	                          o->GetField<int32_t>(tflite::Conv2DOptions::VT_DILATION_W_FACTOR, 1),
	                          pad))
		return false;
	return nn_tflm_bias_ok(b, nn_tflm_dim(y, 3u), nn_tflm_tensor_type(x), true);
}

/* R2 (the file header). */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_DEPTHWISE_CONV_2D(const tflite::Operator *op,
                                                            const nn_tflm_tensor_vec *tensors,
                                                            const nn_tflm_buffer_vec *buffers)
{
	(void)buffers;
	const tflite::Tensor *x, *f, *b, *y;
	bool apply;

	if (!nn_tflm_op_xfby(op, tensors, &x, &f, &b, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (nn_tflm_rank(x) != 4u || nn_tflm_rank(f) != 4u || nn_tflm_rank(y) != 4u)
		return false;

	const int32_t fc = nn_tflm_dim(f, 3u);

	/* filter[0] is 1, as the reference kernel DCHECKs (the kernels read only
	 * filter[1..3]; this holds the model to what they assume). */
	if (nn_tflm_dim(f, 0u) != 1 || fc != nn_tflm_dim(y, 3u))
		return false;

	const flatbuffers::Table *o =
		nn_tflm_op_options(op, tflite::BuiltinOptions_DepthwiseConv2DOptions);
	const int32_t dm = o ? o->GetField<int32_t>(
		tflite::DepthwiseConv2DOptions::VT_DEPTH_MULTIPLIER, 0) : 0;
	uint32_t n = 1u;
	int64_t pad[2];

	if (dm < 1)
		return false;
	if (!nn_tflm_count_mul(&n, nn_tflm_dim(x, 3u)) || !nn_tflm_count_mul(&n, dm) ||
	    fc < 0 || n != (uint32_t)fc)
		return false;

	/* Issue #188: the output's H and W the window formula's (the opt path
	 * memsets the scratch by an output H past it), the batch the input's
	 * (a policy: the kernels loop over the smaller), and in H and W the
	 * kernels' int16 row / column bases, output x stride - padding
	 * (arm_depthwise_conv_s8_opt.c), inside int16: (output - 1) x stride at
	 * most 32767 (an output of 0 counts as 0) and Prepare()'s padding, the
	 * total halved, at most 32768. */
	if (!nn_tflm_window_hw_ok(x, y, nn_tflm_dim(f, 1u), nn_tflm_dim(f, 2u), o,
	                          tflite::DepthwiseConv2DOptions::VT_PADDING,
	                          tflite::DepthwiseConv2DOptions::VT_STRIDE_H,
	                          tflite::DepthwiseConv2DOptions::VT_STRIDE_W,
	                          o->GetField<int32_t>(
	                                  tflite::DepthwiseConv2DOptions::VT_DILATION_H_FACTOR, 1),
	                          o->GetField<int32_t>(
	                                  tflite::DepthwiseConv2DOptions::VT_DILATION_W_FACTOR, 1),
	                          pad))
		return false;
	if (nn_tflm_dim(x, 0u) != nn_tflm_dim(y, 0u))
		return false;
	for (uint32_t d = 1; d < 3u; d++) {
		const int32_t od = nn_tflm_dim(y, d);
		const int32_t st = o->GetField<int32_t>(d == 1u ?
			tflite::DepthwiseConv2DOptions::VT_STRIDE_H :
			tflite::DepthwiseConv2DOptions::VT_STRIDE_W, 0);

		if (od > 0 && ((int64_t)od - 1) * st > 32767)
			return false;
		if (pad[d - 1u] / 2 > 32768)
			return false;
	}
	return nn_tflm_bias_ok(b, fc, nn_tflm_tensor_type(x), false);
}

/* R3 (the file header). */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_FULLY_CONNECTED(const tflite::Operator *op,
                                                          const nn_tflm_tensor_vec *tensors,
                                                          const nn_tflm_buffer_vec *buffers)
{
	(void)buffers;
	const tflite::Tensor *x, *w, *b, *y;
	bool apply;

	if (!nn_tflm_op_xfby(op, tensors, &x, &w, &b, &y, &apply))
		return false;
	if (!apply)
		return true;

	const uint32_t rx = nn_tflm_rank(x);
	const uint32_t ry = nn_tflm_rank(y);

	if (nn_tflm_rank(w) != 2u || ry < 2u || ry > 4u || rx > NN_TFLM_MAX_TENSOR_RANK)
		return false;
	if (nn_tflm_dim(w, 0u) != nn_tflm_dim(y, ry - 1u))
		return false;

	uint32_t outer, nx;

	if (!nn_tflm_dims_count(y, 0u, ry - 1u, &outer) ||
	    !nn_tflm_count_mul(&outer, nn_tflm_dim(w, 1u)))
		return false;
	if (!nn_tflm_dims_count(x, 0u, rx, &nx) || nx != outer)
		return false;
	return nn_tflm_bias_ok(b, nn_tflm_dim(w, 0u), nn_tflm_tensor_type(x), false);
}

/* The tensors an operator's input @p j and output 0 name: false for a null
 * table entry (FORMAT); *@p apply false when the rule does not apply -- the
 * inputs list no longer than @p j or no output, or either one -1 (Prepare()'s
 * live TF_LITE_ENSURE refuses those).  Lengths before elements. */
NN_TFLM_RULE_HELPER bool nn_tflm_op_in_out(const tflite::Operator *op, uint32_t j,
                                           const nn_tflm_tensor_vec *tensors,
                                           const tflite::Tensor **x, const tflite::Tensor **y,
                                           bool *apply)
{
	const flatbuffers::Vector<int32_t> *in = op->inputs();
	const flatbuffers::Vector<int32_t> *out = op->outputs();

	*apply = false;
	if (in->size() <= j || out->size() < 1u)
		return true;
	if (!nn_tflm_op_tensor(in, j, tensors, x) || !nn_tflm_op_tensor(out, 0u, tensors, y))
		return false;
	*apply = *x && *y;
	return true;
}

/* An index tensor (paddings, begin / end / strides, a permutation, a size) is
 * INT32 -- the kernels read it as int32 whatever its declared type -- and, when
 * @p want is not negative, has exactly @p want elements (rank judged first). */
NN_TFLM_RULE_HELPER bool nn_tflm_index_tensor_ok(const tflite::Tensor *t, int32_t want)
{
	if (nn_tflm_tensor_type(t) != tflite::TensorType_INT32)
		return false;
	if (want < 0)
		return true;

	const uint32_t r = nn_tflm_rank(t);
	uint32_t n;

	if (r > NN_TFLM_MAX_TENSOR_RANK)
		return false;
	return nn_tflm_dims_count(t, 0u, r, &n) && n == (uint32_t)want;
}

/* R4 (the file header). */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_PAD(const tflite::Operator *op,
                                             const nn_tflm_tensor_vec *tensors,
                                             const nn_tflm_buffer_vec *buffers)
{
	const tflite::Tensor *x, *p, *y;
	bool apply;

	if (!nn_tflm_op_in_out(op, 1u, tensors, &p, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (!nn_tflm_op_tensor(op->inputs(), 0u, tensors, &x))
		return false;
	if (!x)
		return true;
	if (!nn_tflm_index_tensor_ok(p, -1) || nn_tflm_rank(x) != nn_tflm_rank(y))
		return false;

	/* Issue #188: with data, the paddings' count (2 x the input's rank) before
	 * any value, each value >= 0, and input + before + after == output per
	 * dimension in 64 bits (Prepare()'s live check adds them in int). */
	const flatbuffers::Vector<uint8_t> *d =
		nn_tflm_buffer_data(buffers, nn_tflm_tensor_buffer(p));
	const uint32_t r = nn_tflm_rank(x);

	if (!d)
		return true;
	if (r > NN_TFLM_MAX_TENSOR_RANK || !nn_tflm_index_tensor_ok(p, (int32_t)(2u * r)))
		return false;
	for (uint32_t i = 0; i < r; i++) {
		const int32_t before = flatbuffers::ReadScalar<int32_t>(d->data() + 8u * i);
		const int32_t after = flatbuffers::ReadScalar<int32_t>(d->data() + 8u * i + 4u);

		if (before < 0 || after < 0)
			return false;
		if ((int64_t)nn_tflm_dim(x, i) + before + after != (int64_t)nn_tflm_dim(y, i))
			return false;
	}
	return true;
}

/* R5 (the file header). */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_STRIDED_SLICE(const tflite::Operator *op,
                                                       const nn_tflm_tensor_vec *tensors,
                                                       const nn_tflm_buffer_vec *buffers)
{
	const flatbuffers::Vector<int32_t> *in = op->inputs();
	const tflite::Tensor *x, *t[3];

	(void)buffers;
	if (in->size() < 4u)
		return true;
	if (!nn_tflm_op_tensor(in, 0u, tensors, &x))
		return false;
	for (uint32_t j = 0; j < 3u; j++)
		if (!nn_tflm_op_tensor(in, 1u + j, tensors, &t[j]))
			return false;
	if (!x || !t[0] || !t[1] || !t[2])
		return true;

	const uint32_t rx = nn_tflm_rank(x);

	if (rx > NN_TFLM_MAX_TENSOR_RANK)
		return false;
	for (uint32_t j = 0; j < 3u; j++)
		if (!nn_tflm_index_tensor_ok(t[j], (int32_t)rx))
			return false;
	return true;
}

/* R6 (the file header). */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_TRANSPOSE(const tflite::Operator *op,
                                                   const nn_tflm_tensor_vec *tensors,
                                                   const nn_tflm_buffer_vec *buffers)
{
	const tflite::Tensor *x, *p, *y;
	bool apply;

	if (!nn_tflm_op_in_out(op, 1u, tensors, &p, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (!nn_tflm_index_tensor_ok(p, -1))
		return false;
	if (!nn_tflm_op_tensor(op->inputs(), 0u, tensors, &x))
		return false;
	if (!x)
		return true;

	/* Issue #188: with data, the permutation's count (the input's rank) before
	 * any value, each value in [0, rank) and none repeated, and output[i] ==
	 * input[perm[i]] (the int8 kernel loops over the input's dims and writes
	 * by the output's strides; its rank-2 path ignores the values and writes
	 * the input's n x h, so a repeated value is refused, not counted once as
	 * MEAN's axes are: the output is then the input's elements reordered). */
	const flatbuffers::Vector<uint8_t> *d =
		nn_tflm_buffer_data(buffers, nn_tflm_tensor_buffer(p));
	const uint32_t r = nn_tflm_rank(x);
	uint32_t seen = 0;

	if (!d)
		return true;
	if (r > NN_TFLM_MAX_TENSOR_RANK || !nn_tflm_index_tensor_ok(p, (int32_t)r) ||
	    nn_tflm_rank(y) != r)
		return false;
	for (uint32_t i = 0; i < r; i++) {
		const int32_t v = flatbuffers::ReadScalar<int32_t>(d->data() + 4u * i);

		if (v < 0 || (uint32_t)v >= r || ((seen >> (uint32_t)v) & 1u))
			return false;
		seen |= 1u << (uint32_t)v;
		if (nn_tflm_dim(y, i) != nn_tflm_dim(x, (uint32_t)v))
			return false;
	}
	return true;
}

/* R7, both RESIZE operators (the file header).  The size's two values are read
 * only when it has data, which #183 has proven is exactly 8 bytes here (INT32,
 * 2 elements); without data the values are not compared (Prepare()'s live
 * "constant" check refuses such a model on the board as NN_MODEL_ERR_ARENA, and
 * the gates, which do not plan, pass it). */
NN_TFLM_RULE_HELPER bool nn_tflm_resize_ok(const tflite::Operator *op,
                                           const nn_tflm_tensor_vec *tensors,
                                           const nn_tflm_buffer_vec *buffers)
{
	const tflite::Tensor *sz, *y;
	bool apply;

	if (!nn_tflm_op_in_out(op, 1u, tensors, &sz, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (!nn_tflm_index_tensor_ok(sz, 2) || nn_tflm_rank(sz) != 1u || nn_tflm_rank(y) != 4u)
		return false;

	/* Issue #188: the input rank 4 (its rank judged first), and its batch and
	 * channels the output's -- a policy, not a bound: the kernels loop over
	 * the smaller (MatchingDim()). */
	const tflite::Tensor *x;

	if (!nn_tflm_op_tensor(op->inputs(), 0u, tensors, &x))
		return false;
	if (x && (nn_tflm_rank(x) != 4u || nn_tflm_dim(x, 0u) != nn_tflm_dim(y, 0u) ||
	          nn_tflm_dim(x, 3u) != nn_tflm_dim(y, 3u)))
		return false;

	const flatbuffers::Vector<uint8_t> *d =
		nn_tflm_buffer_data(buffers, nn_tflm_tensor_buffer(sz));

	if (!d)
		return true;
	return flatbuffers::ReadScalar<int32_t>(d->data()) == nn_tflm_dim(y, 1u) &&
	       flatbuffers::ReadScalar<int32_t>(d->data() + 4) == nn_tflm_dim(y, 2u);
}

NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_RESIZE_BILINEAR(const tflite::Operator *op,
                                                         const nn_tflm_tensor_vec *tensors,
                                                         const nn_tflm_buffer_vec *buffers)
{
	return nn_tflm_resize_ok(op, tensors, buffers);
}

NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_RESIZE_NEAREST_NEIGHBOR(const tflite::Operator *op,
                                                                 const nn_tflm_tensor_vec *tensors,
                                                                 const nn_tflm_buffer_vec *buffers)
{
	return nn_tflm_resize_ok(op, tensors, buffers);
}

/* R8, ADD / SUB / MUL with two inputs or more -- inputs 0 and 1 judged, the
 * only two the kernels read (CMSIS-NN ADD's and SUB's Prepare() do not count
 * the inputs, cmsis_nn/add.cc:295-298, sub_common.cc; MUL's requires 2,
 * mul_common.cc:52) -- (the file header): the three ranks at
 * most NN_TFLM_MAX_TENSOR_RANK, each input's at most the output's, and, aligned
 * on the right (a missing leading dimension is 1), each input dimension 1 or the
 * output's and at least one of the two the output's -- the output is the
 * broadcast of the two, which is what the kernels loop over. */
NN_TFLM_RULE_HELPER bool nn_tflm_binary_ok(const tflite::Operator *op,
                                           const nn_tflm_tensor_vec *tensors)
{
	const flatbuffers::Vector<int32_t> *in = op->inputs();
	const tflite::Tensor *a, *b, *y;
	bool apply;

	if (in->size() < 2u)
		return true;
	if (!nn_tflm_op_in_out(op, 1u, tensors, &b, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (!nn_tflm_op_tensor(in, 0u, tensors, &a))
		return false;
	if (!a)
		return true;

	const uint32_t ra = nn_tflm_rank(a), rb = nn_tflm_rank(b), ry = nn_tflm_rank(y);

	if (ry > NN_TFLM_MAX_TENSOR_RANK || ra > ry || rb > ry)
		return false;
	for (uint32_t i = 0; i < ry; i++) {
		const int32_t o = nn_tflm_dim(y, i);
		const int32_t da = i >= ry - ra ? nn_tflm_dim(a, i - (ry - ra)) : 1;
		const int32_t db = i >= ry - rb ? nn_tflm_dim(b, i - (ry - rb)) : 1;

		if ((da != o && da != 1) || (db != o && db != 1))
			return false;
		if (da != o && db != o)
			return false;
	}
	return true;
}

NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_ADD(const tflite::Operator *op,
                                             const nn_tflm_tensor_vec *tensors,
                                             const nn_tflm_buffer_vec *buffers)
{
	(void)buffers;
	return nn_tflm_binary_ok(op, tensors);
}

NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_SUB(const tflite::Operator *op,
                                             const nn_tflm_tensor_vec *tensors,
                                             const nn_tflm_buffer_vec *buffers)
{
	(void)buffers;
	return nn_tflm_binary_ok(op, tensors);
}

NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_MUL(const tflite::Operator *op,
                                             const nn_tflm_tensor_vec *tensors,
                                             const nn_tflm_buffer_vec *buffers)
{
	(void)buffers;
	return nn_tflm_binary_ok(op, tensors);
}

/* R9 (the file header): every input (a -1 among them leaves the rule to
 * Prepare()) of the output's rank, at most NN_TFLM_MAX_TENSOR_RANK; the axis
 * (ConcatenationOptions, 0 when absent; a negative one counts from the end) in
 * range; every other dimension the output's; the inputs' axis dimensions, none
 * negative, adding up (64 bits, at most NN_TFLM_MAX_OP_TENSORS addends) to the
 * output's. */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_CONCATENATION(const tflite::Operator *op,
                                                       const nn_tflm_tensor_vec *tensors,
                                                       const nn_tflm_buffer_vec *buffers)
{
	const flatbuffers::Vector<int32_t> *in = op->inputs();
	const tflite::Tensor *x, *y;
	bool apply;

	(void)buffers;
	if (!nn_tflm_op_in_out(op, 0u, tensors, &x, &y, &apply))
		return false;
	if (!apply)
		return true;
	for (uint32_t j = 1; j < in->size(); j++) {
		if (!nn_tflm_op_tensor(in, j, tensors, &x))
			return false;
		if (!x)
			return true;
	}

	const uint32_t ry = nn_tflm_rank(y);

	if (ry > NN_TFLM_MAX_TENSOR_RANK)
		return false;

	const flatbuffers::Table *o =
		nn_tflm_op_options(op, tflite::BuiltinOptions_ConcatenationOptions);
	int32_t axis = o ? o->GetField<int32_t>(tflite::ConcatenationOptions::VT_AXIS, 0) : 0;

	if (axis < 0)
		axis += (int32_t)ry;
	if (axis < 0 || (uint32_t)axis >= ry)
		return false;

	int64_t sum = 0;

	for (uint32_t j = 0; j < in->size(); j++) {
		nn_tflm_op_tensor(in, j, tensors, &x);   /* proven above */
		if (nn_tflm_rank(x) != ry)
			return false;
		for (uint32_t d = 0; d < ry; d++) {
			const int32_t v = nn_tflm_dim(x, d);

			if (d == (uint32_t)axis) {
				if (v < 0)
					return false;
				sum += v;
			} else if (v != nn_tflm_dim(y, d)) {
				return false;
			}
		}
	}
	return sum == (int64_t)nn_tflm_dim(y, (uint32_t)axis);
}

/* R11 (issue #188; the file header, "OUTPUT FRAMES"): an elementwise operator
 * (input 0 and output 0, neither -1) writes the INPUT's element count into its
 * output -- the reference kernels loop over ElementCount(input) or
 * MatchingFlatSize(input, output), whose comparison is a DCHECK -- so the two
 * element counts are equal (ranks at most NN_TFLM_MAX_TENSOR_RANK first; the
 * shapes may differ). */
NN_TFLM_RULE_HELPER bool nn_tflm_unary_ok(const tflite::Operator *op,
                                          const nn_tflm_tensor_vec *tensors)
{
	const tflite::Tensor *x, *y;
	bool apply;

	if (!nn_tflm_op_in_out(op, 0u, tensors, &x, &y, &apply))
		return false;
	if (!apply)
		return true;

	const uint32_t rx = nn_tflm_rank(x), ry = nn_tflm_rank(y);
	uint32_t nx, ny;

	if (rx > NN_TFLM_MAX_TENSOR_RANK || ry > NN_TFLM_MAX_TENSOR_RANK)
		return false;
	return nn_tflm_dims_count(x, 0u, rx, &nx) && nn_tflm_dims_count(y, 0u, ry, &ny) &&
	       nx == ny;
}

/* R12a (issue #188): a pool's input and output are rank 4 -- its Prepare()
 * checks neither, and below rank 4 RuntimeShape::Dims(3) reads past the shape
 * (an uninitialized stack word), which sets the kernel's loop counts.  R12b
 * (step 2): Pool2DOptions present, the output's H and W the window formula's
 * (filter_height / _width, the strides, dilation 1), and the batch and the
 * channels the input's -- a policy, not a bound: the kernels loop over the
 * smaller (MatchingDim()). */
NN_TFLM_RULE_HELPER bool nn_tflm_pool_ok(const tflite::Operator *op,
                                         const nn_tflm_tensor_vec *tensors)
{
	const tflite::Tensor *x, *y;
	bool apply;

	if (!nn_tflm_op_in_out(op, 0u, tensors, &x, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (nn_tflm_rank(x) != 4u || nn_tflm_rank(y) != 4u)
		return false;

	const flatbuffers::Table *o = nn_tflm_op_options(op, tflite::BuiltinOptions_Pool2DOptions);
	int64_t pad[2];

	if (!o)
		return false;
	if (!nn_tflm_window_hw_ok(x, y,
	                          o->GetField<int32_t>(tflite::Pool2DOptions::VT_FILTER_HEIGHT, 0),
	                          o->GetField<int32_t>(tflite::Pool2DOptions::VT_FILTER_WIDTH, 0), o,
	                          tflite::Pool2DOptions::VT_PADDING,
	                          tflite::Pool2DOptions::VT_STRIDE_H,
	                          tflite::Pool2DOptions::VT_STRIDE_W, 1, 1, pad))
		return false;
	return nn_tflm_dim(x, 0u) == nn_tflm_dim(y, 0u) && nn_tflm_dim(x, 3u) == nn_tflm_dim(y, 3u);
}

/* The rules that are one helper over the operator and its subgraph's tensors. */
#define NN_TFLM_OP_RULE_AS(e, helper)                                                 \
	NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_##e(const tflite::Operator *op,      \
	                                             const nn_tflm_tensor_vec *tensors, \
	                                             const nn_tflm_buffer_vec *buffers) \
	{                                                                             \
		(void)buffers;                                                        \
		return helper(op, tensors);                                           \
	}
NN_TFLM_OP_RULE_AS(QUANTIZE, nn_tflm_unary_ok)
NN_TFLM_OP_RULE_AS(DEQUANTIZE, nn_tflm_unary_ok)
NN_TFLM_OP_RULE_AS(RELU, nn_tflm_unary_ok)
NN_TFLM_OP_RULE_AS(RELU6, nn_tflm_unary_ok)
NN_TFLM_OP_RULE_AS(LOGISTIC, nn_tflm_unary_ok)
NN_TFLM_OP_RULE_AS(LEAKY_RELU, nn_tflm_unary_ok)
NN_TFLM_OP_RULE_AS(SOFTMAX, nn_tflm_unary_ok)
NN_TFLM_OP_RULE_AS(MAX_POOL_2D, nn_tflm_pool_ok)
NN_TFLM_OP_RULE_AS(AVERAGE_POOL_2D, nn_tflm_pool_ok)
#undef NN_TFLM_OP_RULE_AS

/* R13 MEAN (issue #188; the file header): the axis INT32 and WITH data (the
 * reference kernel builds the output's positions from the input's dims outside
 * the axes and adds into them, so an axis the walk cannot read is refused), at
 * most NN_TFLM_MAX_MEAN_AXES elements (the count before any value), each in
 * [-rank, rank) of the input (rank at most NN_TFLM_MAX_TENSOR_RANK; negative
 * counts from the end), a repeated axis counted once; and the output's shape the
 * reduced input's: with keep_dims (ReducerOptions; absent or another table:
 * false) the input's with 1 at each axis, without it the input's with the axes
 * removed, the rank included. */
NN_TFLM_RULE_HELPER bool nn_tflm_op_rule_MEAN(const tflite::Operator *op,
                                              const nn_tflm_tensor_vec *tensors,
                                              const nn_tflm_buffer_vec *buffers)
{
	const tflite::Tensor *x, *a, *y;
	bool apply;

	if (!nn_tflm_op_in_out(op, 1u, tensors, &a, &y, &apply))
		return false;
	if (!apply)
		return true;
	if (!nn_tflm_op_tensor(op->inputs(), 0u, tensors, &x))
		return false;
	if (!x)
		return true;
	if (nn_tflm_tensor_type(a) != tflite::TensorType_INT32)
		return false;

	const flatbuffers::Vector<uint8_t> *d =
		nn_tflm_buffer_data(buffers, nn_tflm_tensor_buffer(a));
	const uint32_t ra = nn_tflm_rank(a), rx = nn_tflm_rank(x), ry = nn_tflm_rank(y);
	uint32_t na, mask = 0;

	if (!d)
		return false;
	if (ra > NN_TFLM_MAX_TENSOR_RANK || !nn_tflm_dims_count(a, 0u, ra, &na) ||
	    na > NN_TFLM_MAX_MEAN_AXES || rx > NN_TFLM_MAX_TENSOR_RANK)
		return false;
	for (uint32_t j = 0; j < na; j++) {
		int32_t v = flatbuffers::ReadScalar<int32_t>(d->data() + 4u * j);

		if (v < 0)
			v += (int32_t)rx;
		if (v < 0 || (uint32_t)v >= rx)
			return false;
		mask |= 1u << (uint32_t)v;
	}

	const flatbuffers::Table *o = nn_tflm_op_options(op, tflite::BuiltinOptions_ReducerOptions);
	const bool keep = o && o->GetField<uint8_t>(tflite::ReducerOptions::VT_KEEP_DIMS, 0) != 0;
	uint32_t j = 0;

	for (uint32_t i = 0; i < rx; i++) {
		const bool reduced = (mask >> i) & 1u;

		if (reduced && !keep)
			continue;
		if (j >= ry || nn_tflm_dim(y, j) != (reduced ? 1 : nn_tflm_dim(x, i)))
			return false;
		j++;
	}
	return j == ry;
}

/* The empty rules, one line each saying why. */
#define NN_TFLM_OP_RULE_NONE(e)                                                       \
	NN_TFLM_CHECK_INLINE bool nn_tflm_op_rule_##e(const tflite::Operator *op,      \
	                                              const nn_tflm_tensor_vec *tensors, \
	                                              const nn_tflm_buffer_vec *buffers) \
	{                                                                             \
		(void)op;                                                             \
		(void)tensors;                                                        \
		(void)buffers;                                                        \
		return true;                                                          \
	}
NN_TFLM_OP_RULE_NONE(RESHAPE)          /* under R10 Prepare()'s live element-count compare and the memcpy's size are exact */
NN_TFLM_OP_RULE_NONE(CUSTOM)           /* ethos-u: the NPU reads what the command stream says (issue #184) */
#undef NN_TFLM_OP_RULE_NONE

/* The rule of the operator whose kind is @p kind, chosen among the registered
 * operators only (nn_tflm_ops.h's selected list, one case each); any other kind
 * passes here and is refused by the resolver and the gates' operator list. */
NN_TFLM_CHECK_INLINE bool nn_tflm_op_rule_ok(int32_t kind, const tflite::Operator *op,
                                             const nn_tflm_tensor_vec *tensors,
                                             const nn_tflm_buffer_vec *buffers)
{
	switch (kind) {
#define NN_TFLM_OP_RULE_CASE(e, m)               \
	case tflite::BuiltinOperator_##e:        \
		return nn_tflm_op_rule_##e(op, tensors, buffers);
	NN_TFLM_OPS_ALL(NN_TFLM_OP_RULE_CASE)
#undef NN_TFLM_OP_RULE_CASE
	default:
		return true;
	}
}

/*
 * One subgraph holds together: a tensor vector, every tensor naming a buffer
 * below the model's buffer count (issue #181), of a type with a size, of a
 * planned size that fits an int and keeps @p *planned, every earlier subgraph's
 * sizes with it, at most NN_TFLM_MAX_PLANNED_BYTES (issue #188) and, when
 * its buffer has data, exactly as long as its shape says (issue #183), and every operator present with
 * an inputs and an outputs list, each of its three lists at most
 * NN_TFLM_MAX_OP_TENSORS long -- all three LENGTHS judged before any element is
 * read -- and then every index in range; every tensor's quantization holding
 * together (nn_tflm_tensor_quant_ok(), before the operators), and the scale
 * lengths the operators name adding up, with every earlier subgraph's, to at most
 * NN_TFLM_MAX_QUANT_REFS in @p *qrefs (issue #180), no operator output with
 * data (issue #183), and every operator naming an entry of @p codes (present)
 * and passing its kind's rule (issue #185).  Per tensor entry: a few length
 * reads, one type and, for a constant, at most NN_TFLM_MAX_TENSOR_RANK
 * dimension values; per operator entry: three compares, at most 3 x 3 x
 * NN_TFLM_MAX_OP_TENSORS reads and one rule (a constant).
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_subgraph_ok(const tflite::SubGraph *sg,
                                         const nn_tflm_buffer_vec *buffers,
                                         const nn_tflm_opcode_vec *codes,
                                         uint32_t *qrefs, uint64_t *planned)
{
	const nn_tflm_tensor_vec *tensors = sg ? sg->tensors() : nullptr;

	if (!tensors)
		return false;

	const nn_tflm_op_vec *ops = nn_tflm_sg_operators(sg);
	const uint32_t nt = tensors->size();
	const uint32_t nb = buffers ? buffers->size() : 0u;

	for (uint32_t i = 0; i < nt; i++) {
		const tflite::Tensor *t = tensors->Get(i);

		if (t) {
			const uint32_t bi = nn_tflm_tensor_buffer(t);

			if (bi >= nb)
				return false;

			/* Every tensor's type has a size, data or not; then a
			 * constant's length (the buffer index is proven above). */
			const uint32_t eb2 = nn_tflm_elem_bytes2(nn_tflm_tensor_type(t));
			const flatbuffers::Vector<uint8_t> *d;

			if (eb2 == 0u)
				return false;
			/* R10 / R10b, before the constant's length (issue #188). */
			if (!nn_tflm_tensor_size_ok(t, eb2, planned))
				return false;
			d = nn_tflm_buffer_data(buffers, bi);
			if (d && !nn_tflm_const_len_ok(t, eb2, d->size()))
				return false;
		}
		if (!nn_tflm_tensor_quant_ok(t))
			return false;
	}

	for (uint32_t i = 0; ops && i < ops->size(); i++) {
		const tflite::Operator *op = ops->Get(i);

		if (!op || !op->inputs() || !op->outputs())
			return false;
		if (op->inputs()->size() > NN_TFLM_MAX_OP_TENSORS ||
		    op->outputs()->size() > NN_TFLM_MAX_OP_TENSORS ||
		    nn_tflm_list_len(op->intermediates()) > NN_TFLM_MAX_OP_TENSORS)
			return false;
		if (!nn_tflm_op_list_indices_ok(op->inputs(), nt, true) ||
		    !nn_tflm_op_list_indices_ok(op->outputs(), nt, false) ||
		    !nn_tflm_op_list_indices_ok(op->intermediates(), nt, false))
			return false;
		if (!nn_tflm_op_list_quant_add(op->inputs(), tensors, qrefs) ||
		    !nn_tflm_op_list_quant_add(op->outputs(), tensors, qrefs) ||
		    !nn_tflm_op_list_quant_add(op->intermediates(), tensors, qrefs))
			return false;
		if (!nn_tflm_op_outputs_no_data(op->outputs(), tensors, buffers))
			return false;

		/* R0, then the kind's rule (issue #185). */
		const uint32_t ci = nn_tflm_op_opcode_index(op);

		if (ci >= codes->size())
			return false;

		const tflite::OperatorCode *code = nn_tflm_vec_table(codes, ci);

		if (!code || !nn_tflm_op_rule_ok(nn_tflm_opcode_kind(code), op, tensors, buffers))
			return false;
	}
	return true;
}

/*
 * One of subgraph 0's I/O lists names only tensors that exist, none of them with
 * data (issue #183): TFLM does not plan a tensor with data into the arena, so the
 * board would write a graph input, and the kernel a graph output, in place, into
 * the model.  Each entry: its index compared, then its tensor, its buffer (the
 * buffer index already proven by nn_tflm_subgraph_ok()) and the data length --
 * one pass, the list's only one before its count is judged (the file header,
 * "THE ONE EXCEPTION").
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_ok(const flatbuffers::Vector<int32_t> *list,
                                      const nn_tflm_tensor_vec *tensors,
                                      const nn_tflm_buffer_vec *buffers)
{
	if (!list)
		return false;
	for (uint32_t j = 0; j < list->size(); j++) {
		const int32_t ti = list->Get(j);

		if (ti < 0 || (uint32_t)ti >= tensors->size())
			return false;
		if (nn_tflm_tensor_has_data(tensors, (uint32_t)ti, buffers))
			return false;
	}
	return true;
}

/* The metadata name of TFLM's offline memory plan (kOfflineMemAllocMetadata,
 * micro_allocation_info.cc). */
#define NN_TFLM_OFFLINE_PLAN_NAME "OfflineMemoryAllocation"

/* Is @p md the entry TFLM's GetOfflinePlannedOffsets() takes for the offline
 * plan?  The same test (a strncmp bounded by the shorter length, then the
 * lengths equal): the name's length is read first, and only an exactly 23-byte
 * name is compared, 23 bytes -- never a search for a terminator. */

NN_TFLM_CHECK_INLINE bool nn_tflm_md_is_offline_plan(const tflite::Metadata *md)
{
	const size_t len = sizeof NN_TFLM_OFFLINE_PLAN_NAME - 1u;   /* 23 */
	const flatbuffers::String *name = md->name();

	return name && name->size() == len &&
	       memcmp(name->data(), NN_TFLM_OFFLINE_PLAN_NAME, len) == 0;
}

/*
 * The model's metadata holds together (issue #181): every entry names a buffer
 * below @p nb, and every entry TFLM takes for the offline plan names one whose
 * data has the 12-byte header, a tensor count (word [2], a signed int32) equal
 * to @p total_tensors -- every subgraph's, as TFLM counts -- and room for that
 * many int32 offsets after the header, each -1 (plan online) or >= 0 and below
 * NN_TFLM_MAX_PLANNED_BYTES (issue #188).  Per
 * entry: two compares and, for the plan, a 23-byte compare, one length, one
 * header word and then one compare per offset (the count already equals the
 * model's tensors, so a plan costs a constant per tensor entry).
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_metadata_ok(const tflite::Model *model, uint32_t nb,
                                         uint64_t total_tensors)
{
	const nn_tflm_metadata_vec *metadata = static_cast<const nn_tflm_metadata_vec *>(
		nn_tflm_model_field(model, tflite::Model::VT_METADATA));
	const nn_tflm_buffer_vec *buffers = static_cast<const nn_tflm_buffer_vec *>(
		nn_tflm_model_field(model, tflite::Model::VT_BUFFERS));

	for (uint32_t i = 0; metadata && i < metadata->size(); i++) {
		const tflite::Metadata *md = nn_tflm_vec_table(metadata, i);

		if (!md || md->buffer() >= nb)
			return false;
		if (!nn_tflm_md_is_offline_plan(md))
			continue;

		/* nb > 0 here, so buffers is present. */
		const tflite::Buffer *b = nn_tflm_vec_table(buffers, md->buffer());
		const flatbuffers::Vector<uint8_t> *d = b ? b->data() : nullptr;

		if (!d || d->size() < 12u)
			return false;

		const int32_t nbr = flatbuffers::ReadScalar<int32_t>(d->data() + 8);

		if (nbr < 0 || (uint64_t)nbr != total_tensors)
			return false;
		if ((d->size() - 12u) / 4u < (uint32_t)nbr)
			return false;
		/* Only -1 is "plan online"; any other negative offset would be
		 * placed before the arena, and one at or past
		 * NN_TFLM_MAX_PLANNED_BYTES could wrap the planner's int sum
		 * (the file header, issue #188). */
		for (uint32_t j = 0; j < (uint32_t)nbr; j++) {
			const int32_t off = flatbuffers::ReadScalar<int32_t>(d->data() + 12u + 4u * j);

			if (off < -1 || off >= (int32_t)NN_TFLM_MAX_PLANNED_BYTES)
				return false;
		}
	}
	return true;
}

/* Every tensor of @p sg (it has passed nn_tflm_subgraph_ok()) declares at most
 * NN_TFLM_MAX_TENSOR_RANK dimensions: one length compare per tensor entry, no
 * dimension read. */
NN_TFLM_CHECK_INLINE bool nn_tflm_subgraph_rank_ok(const tflite::SubGraph *sg)
{
	const nn_tflm_tensor_vec *tensors = sg->tensors();

	for (uint32_t i = 0; i < tensors->size(); i++) {
		const tflite::Tensor *t = tensors->Get(i);
		const flatbuffers::Vector<int32_t> *shape = t ? t->shape() : nullptr;

		if (shape && shape->size() > NN_TFLM_MAX_TENSOR_RANK)
			return false;
	}
	return true;
}

/* @p list (it has passed nn_tflm_io_list_ok()) names 1..NN_MAX_IO tensors --
 * NN_MODEL_ERR_SHAPE's rule, the count the backends check again after planning. */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_count_ok(const flatbuffers::Vector<int32_t> *list)
{
	return list->size() >= 1u && list->size() <= (uint32_t)NN_MAX_IO;
}

/* Every tensor in @p list (count already within NN_MAX_IO) declares at most
 * NN_MAX_DIMS dimensions -- the I/O half of NN_MODEL_ERR_TENSOR's rank rule.  A
 * missing shape is a scalar (rank 0), as TFLM reads it. */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_rank_ok(const flatbuffers::Vector<int32_t> *list,
                                           const nn_tflm_tensor_vec *tensors)
{
	for (uint32_t j = 0; j < list->size(); j++) {
		const tflite::Tensor *t = tensors->Get((uint32_t)list->Get(j));
		const flatbuffers::Vector<int32_t> *shape = t ? t->shape() : nullptr;

		if (shape && shape->size() > (uint32_t)NN_MAX_DIMS)
			return false;
	}
	return true;
}

/* Every dimension of every tensor in @p list fits a descriptor.  The list has
 * passed the count and rank checks, so this reads at most NN_MAX_IO x
 * NN_MAX_DIMS values. */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_dims_fit(const flatbuffers::Vector<int32_t> *list,
                                            const nn_tflm_tensor_vec *tensors)
{
	for (uint32_t j = 0; j < list->size(); j++) {
		const tflite::Tensor *t = tensors->Get((uint32_t)list->Get(j));
		const flatbuffers::Vector<int32_t> *shape = t ? t->shape() : nullptr;

		for (uint32_t d = 0; shape && d < shape->size(); d++)
			if (!nn_tensor_dim_fits(shape->Get(d)))
				return false;
	}
	return true;
}

/*
 * The model, before it is planned, in this order: 0, or
 *   NN_MODEL_ERR_FORMAT  subgraph 0, its tensors or an I/O list missing, or an
 *                        I/O index outside the tensor vector; any subgraph
 *                        without tensors; an operator without an inputs or
 *                        outputs list, with a list longer than
 *                        NN_TFLM_MAX_OP_TENSORS, or naming a tensor that does not
 *                        exist; a tensor's scale vector longer than
 *                        NN_TFLM_MAX_QUANT_CHANNELS, its zero-point vector
 *                        neither 0, 1 nor the scale's length, or a per-channel
 *                        quantized dimension outside its rank (rank >= 2) or
 *                        outside 0..NN_TFLM_MAX_TENSOR_RANK-1 (rank 0 / 1);
 *                        the operators'
 *                        scale lengths adding up past NN_TFLM_MAX_QUANT_REFS;
 *                        no operator-code vector; a tensor or metadata entry
 *                        naming a buffer past the buffer vector; an
 *                        "OfflineMemoryAllocation" entry whose buffer has no
 *                        data, no 12-byte header, a tensor count other than
 *                        every subgraph's tensors together, fewer offsets
 *                        than that count, or an offset below -1 (issue #181)
 *                        or at or past 2^30 (issue #188);
 *                        a tensor of a type TFLM gives no size; a tensor of a
 *                        rank at most NN_TFLM_MAX_TENSOR_RANK with a negative
 *                        dimension or a planned size past
 *                        NN_TFLM_MAX_TENSOR_BYTES, or all of them together past
 *                        NN_TFLM_MAX_PLANNED_BYTES (issue #188); a tensor with
 *                        data whose length is not its shape's bytes, with a
 *                        negative dimension or of a rank above
 *                        NN_TFLM_MAX_TENSOR_RANK; data on an operator output or
 *                        on subgraph 0's I/O (issue #183); an operator naming
 *                        an operator code past the vector, or breaking its
 *                        kind's rule (issues #185, #188) (structure)
 *   NN_MODEL_ERR_SHAPE   an I/O list naming outside 1..NN_MAX_IO tensors (count)
 *   NN_MODEL_ERR_TENSOR  any tensor declaring more than NN_TFLM_MAX_TENSOR_RANK
 *                        dimensions, or an I/O tensor more than NN_MAX_DIMS (rank)
 *   NN_MODEL_ERR_DIMS    an I/O dimension above NN_TENSOR_DIM_MAX (value; a
 *                        negative one is R10's FORMAT, issue #188)
 * SHAPE and TENSOR are the codes the backends give the same failures after
 * planning (they keep those checks as the backstop); judged here they only come
 * sooner.  The order is also what bounds the work (the file header, "LENGTHS
 * BEFORE ELEMENTS"): the I/O index check is one comparison per list entry (and
 * one tensor's buffer, issue #183), every
 * other vector's length is judged before its elements are read, and everything
 * after the structure reads at most NN_MAX_IO x NN_MAX_DIMS values per I/O list.
 * Without the count and rank checks first, one tensor named ~65,000 times with a
 * ~65,000-entry shape -- scalar vectors, which the verifier checks in O(1) and
 * does not count as tables -- passed the verifier and cost ~4e9 iterations here
 * (issue #175 review).
 */
NN_TFLM_CHECK_INLINE int nn_tflm_model_io_check(const tflite::Model *model)
{
	const auto *subgraphs = model->subgraphs();

	if (!subgraphs || subgraphs->size() == 0u)
		return NN_MODEL_ERR_FORMAT;

	const tflite::SubGraph *sg = subgraphs->Get(0);
	const nn_tflm_tensor_vec *tensors = sg ? sg->tensors() : nullptr;

	if (!tensors)
		return NN_MODEL_ERR_FORMAT;

	const nn_tflm_opcode_vec *codes = static_cast<const nn_tflm_opcode_vec *>(
		nn_tflm_model_field(model, tflite::Model::VT_OPERATOR_CODES));

	if (!codes)
		return NN_MODEL_ERR_FORMAT;

	/* The per-channel sum runs across every subgraph: TFLM prepares them all.
	 * So does its tensor count, which an offline plan must match (64 bits: each
	 * addend is a vector's length, and wio-lite-ai has no verifier to bound how
	 * many subgraphs there are). */
	const nn_tflm_buffer_vec *buffers = static_cast<const nn_tflm_buffer_vec *>(
		nn_tflm_model_field(model, tflite::Model::VT_BUFFERS));
	const uint32_t nb = buffers ? buffers->size() : 0u;
	uint32_t qrefs = 0;
	uint64_t total_tensors = 0;
	uint64_t planned = 0;   /* R10b: every subgraph's sizes, as TFLM plans them all */

	for (uint32_t i = 0; i < subgraphs->size(); i++) {
		const tflite::SubGraph *s = subgraphs->Get(i);

		if (!nn_tflm_subgraph_ok(s, buffers, codes, &qrefs, &planned))
			return NN_MODEL_ERR_FORMAT;
		total_tensors += s->tensors()->size();
	}
	/* Subgraph 0's I/O after every tensor's buffer index is proven (issue #183):
	 * all FORMAT, so moving it here changes no answer. */
	if (!nn_tflm_io_list_ok(sg->inputs(), tensors, buffers) ||
	    !nn_tflm_io_list_ok(sg->outputs(), tensors, buffers))
		return NN_MODEL_ERR_FORMAT;
	if (!nn_tflm_metadata_ok(model, nb, total_tensors))
		return NN_MODEL_ERR_FORMAT;
	if (!nn_tflm_io_list_count_ok(sg->inputs()) ||
	    !nn_tflm_io_list_count_ok(sg->outputs()))
		return NN_MODEL_ERR_SHAPE;
	if (!nn_tflm_io_list_rank_ok(sg->inputs(), tensors) ||
	    !nn_tflm_io_list_rank_ok(sg->outputs(), tensors))
		return NN_MODEL_ERR_TENSOR;
	for (uint32_t i = 0; i < subgraphs->size(); i++)
		if (!nn_tflm_subgraph_rank_ok(subgraphs->Get(i)))
			return NN_MODEL_ERR_TENSOR;
	if (!nn_tflm_io_list_dims_fit(sg->inputs(), tensors) ||
	    !nn_tflm_io_list_dims_fit(sg->outputs(), tensors))
		return NN_MODEL_ERR_DIMS;
	return 0;
}

/* The backstop on a planned tensor, just before its descriptor is published: the
 * same rule over the dims TFLM built.  Its rank has passed NN_MODEL_ERR_TENSOR's
 * check. */
NN_TFLM_CHECK_INLINE bool nn_tflm_tensor_dims_fit(const TfLiteTensor *t)
{
	for (int i = 0; t->dims && i < t->dims->size; i++)
		if (!nn_tensor_dim_fits((int32_t)t->dims->data[i]))
			return false;
	return true;
}

#endif /* NN_TFLM_MODEL_CHECK_H */
