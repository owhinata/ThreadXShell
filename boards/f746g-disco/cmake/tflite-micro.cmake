# tflite-micro (TFLM) C++ backend for CONFIG_NN_BACKEND=tflm (Epic owhinata/stm32f746g-disco#80 P3,
# owhinata/stm32f746g-disco#86).
#
# Included ONLY from the tflm branch of board.cmake, so C++ is enabled for this
# build alone; a null/stedgeai firmware never sees enable_language(CXX) and stays
# C-only.  All C++ is confined to this `tflm` STATIC lib so the
# final shell link keeps the C driver (gcc) and does not auto-pull full libstdc++.
#
# The tflite-micro sources come from the lib/tflite-micro submodule (#134), pinned by
# its gitlink, fetched here on first use (no board's sentinel list names it, so a
# null configure stays offline) and re-checked against the gitlink on every build.  At CONFIGURE time the pinned commit is written out into the build dir
# (cmake/tflm_submodule.cmake -- never generated inside lib/) and upstream's own
# `create_tflm_tree.py` runs there (it drives the tflite-micro Makefile to list library
# sources and download the header-only third-party deps flatbuffers/gemmlowp/ruy -- so
# configure needs the network) into a
# self-contained, glob-able tree under the build dir.  We then compile that tree with
# OUR exact flags (so the ABI -- cortex-m7 / fpv5-sp-d16 / hard / -fno-exceptions -- is
# fully ours).  Only the tflm build pays the generation cost, and it is cached
# (SHA-keyed stamp) after the first run.

enable_language(CXX)

# --- Pinned upstream: the lib/tflite-micro gitlink ---------------------------
include("${CMAKE_SOURCE_DIR}/cmake/tflm_submodule.cmake")
tflm_submodule_pin(TFLM_GIT_SHA)                  # fetches if uninitialised; fatal unless HEAD == gitlink
set(TFLM_SRC  "${CMAKE_BINARY_DIR}/tflm-src")     # git archive of the pinned commit
set(TFLM_ROOT "${CMAKE_BINARY_DIR}/tflm-tree")    # generated self-contained source tree
set(TFLM_GEN   "${TFLM_SRC}/tensorflow/lite/micro/tools/project_generation/create_tflm_tree.py")

# --- Optimized kernels: CMSIS-NN vs reference (Epic owhinata/stm32f746g-disco#80 P3 M2b, owhinata/stm32f746g-disco#88) ---
# ON (default) generates the tree with OPTIMIZED_KERNEL_DIR=cmsis_nn: the tflm
# Makefile downloads ARM-software/CMSIS-NN (+ CMSIS_6 core, pinned by its own
# ext_libs/*_download.sh) and lists the cmsis_nn kernel wrappers instead of the
# reference conv/depthwise_conv/add/pad/pooling.  On Cortex-M7 (armv7e-m) GCC
# defines __ARM_FEATURE_DSP, so the SIMD int8 path is compiled (a compile-time
# canary in nn_tflm.cc asserts it).  OFF keeps pure reference kernels (M2a) for
# an `nn bench` A/B.  The choice is part of the tree-generation stamp key, so
# flipping it regenerates the tree.
set(NN_TFLM_CMSIS_NN ON CACHE BOOL "Use CMSIS-NN optimized kernels in the tflm backend (#88 M2b)")
if(NN_TFLM_CMSIS_NN)
    set(TFLM_KERNEL_TAG "cmsisnn")
    set(TFLM_GEN_EXTRA "--makefile_options=OPTIMIZED_KERNEL_DIR=cmsis_nn")
else()
    set(TFLM_KERNEL_TAG "ref")
    set(TFLM_GEN_EXTRA "")
endif()
set(TFLM_STAMP "${TFLM_ROOT}/.tflm-generated-${TFLM_GIT_SHA}-${TFLM_KERNEL_TAG}")

# --- Python venv (numpy/Pillow are required by TFLM's Makefile evaluation) ----
# Create the venv if missing here; the dependency INSTALL happens in the generation
# block below (not gated on venv creation) so a pre-existing venv that predates the
# numpy/Pillow requirement -- e.g. a docs-only .venv, or one left half-installed by an
# interrupted run -- gets healed instead of silently skipped.
set(TFLM_VENV "${CMAKE_SOURCE_DIR}/.venv")
set(TFLM_PY   "${TFLM_VENV}/bin/python3")
if(NOT EXISTS "${TFLM_PY}")
    find_program(TFLM_HOST_PY NAMES python3 python REQUIRED)
    message(STATUS "tflm: creating ${TFLM_VENV}")
    execute_process(COMMAND "${TFLM_HOST_PY}" -m venv "${TFLM_VENV}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: python venv creation failed (need python3 + venv)")
    endif()
endif()

# --- Export + generate the tree (SHA-keyed stamp: only runs once per pin) -----
# The stamp is written ONLY after a fully successful generation, and each attempt starts
# from a clean export + a fresh dependency install, so an interrupted run self-heals on
# the next configure (no stale partial third-party downloads, no half-populated venv).
if(NOT EXISTS "${TFLM_STAMP}")
    message(STATUS "tflm: generating the source tree from lib/tflite-micro @ ${TFLM_GIT_SHA} "
                   "(one-time, a few minutes -- downloads flatbuffers/gemmlowp/ruy)")

    # Ensure numpy/Pillow are present (idempotent when already satisfied); heals a venv
    # created before these were added to requirements.txt.
    execute_process(
        COMMAND "${TFLM_VENV}/bin/pip" install -q --disable-pip-version-check
                -r "${BOARD_DIR}/requirements.txt"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: pip install -r requirements.txt failed")
    endif()

    # Fresh export each attempt: upstream's third-party extractor skips a download dir
    # that merely exists, so a previously interrupted download would never self-repair
    # in place.
    tflm_submodule_export("${TFLM_GIT_SHA}" "${TFLM_SRC}")

    file(REMOVE_RECURSE "${TFLM_ROOT}")
    execute_process(
        # TFLM_GEN_EXTRA is "--makefile_options=OPTIMIZED_KERNEL_DIR=cmsis_nn" (CMSIS-NN)
        # or empty (reference).  With cmsis_nn the Makefile downloads CMSIS/CMSIS-NN and
        # swaps in the optimized kernel wrappers.  Prepend the venv to PATH so the
        # Makefile's own `python3` subprocess (generate_cc_arrays.py) resolves
        # numpy/Pillow -- running create_tflm_tree.py with the venv python alone is not
        # enough, as make spawns a fresh `python3`.
        COMMAND "${CMAKE_COMMAND}" -E env "PATH=${TFLM_VENV}/bin:$ENV{PATH}"
                "${TFLM_PY}" "${TFLM_GEN}" -e hello_world ${TFLM_GEN_EXTRA} "${TFLM_ROOT}"
        WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${TFLM_ROOT}/tensorflow/lite/micro/micro_interpreter.cc")
        message(FATAL_ERROR "tflm: create_tflm_tree.py failed (need numpy/Pillow in ${TFLM_VENV})")
    endif()
    file(WRITE "${TFLM_STAMP}" "${TFLM_GIT_SHA}\n")
endif()

# --- No built-in model (issue #159 Step 4c) ----------------------------------
# Until Step 4c this file fetched the pinned BlazeFace at BUILD time and compiled
# it into Flash as a C array (189,816 B).  The firmware now carries no model and
# no decoder: a model -- bare, or in a plugin container that brings its decoder --
# is copied to the SD card and loaded with `nn model load --path`, and the pinned
# BlazeFace reaches the board as the `asset-blazeface` container (board.cmake).
# So a tflm build no longer needs the network at build time; configure still
# does, for the tflite-micro tree above.  cmake/fetch_model.cmake stays: the
# asset rule uses it.
#
# [!] NN_TFLM_MODEL_OVERRIDE AND THE OLD NN_TFLM_MODEL ARE GONE WITH IT.  A tree
# that still holds either is told so once, rather than silently building a
# firmware without the model the operator asked for.
foreach(_old NN_TFLM_MODEL NN_TFLM_MODEL_OVERRIDE)
    if(DEFINED CACHE{${_old}} AND NOT "$CACHE{${_old}}" STREQUAL "")
        message(WARNING
            "tflm: ${_old} is IGNORED since issue #159 Step 4c -- this tree still "
            "holds `$CACHE{${_old}}`.  The firmware carries no built-in model any "
            "more: copy the model (or its container, `--target asset-blazeface`) "
            "to the SD card and `nn model load --path /<file>`.  To silence this, "
            "pass -U${_old}.")
    endif()
endforeach()

# --- Compile the generated tree with our flags (ABI = ours) ------------------
# create_tflm_tree emits only library sources under tensorflow/, but exclude the
# test-support cluster (helpers + mock/fake context) + any *_test.cc belt-and-suspenders.
# signal/*.cc are NOT compiled (no signal ops registered); their headers stay on the
# include path (micro_mutable_op_resolver.h #includes signal/micro/kernels/{irfft,rfft}.h).
file(GLOB_RECURSE TFLM_LIB_SOURCES CONFIGURE_DEPENDS
     "${TFLM_ROOT}/tensorflow/*.cc"
     "${TFLM_ROOT}/tensorflow/*.c")
list(FILTER TFLM_LIB_SOURCES EXCLUDE REGEX
     "(_test\\.cc|test_helpers\\.cc|test_helper_custom_ops\\.cc|mock_micro_graph\\.cc|fake_micro_context\\.cc)$")

# CMSIS-NN library sources (C), copied into the tree by create_tflm_tree under
# third_party/cmsis_nn/Source when OPTIMIZED_KERNEL_DIR=cmsis_nn.  The optimized
# kernel wrappers (tensorflow/lite/micro/kernels/cmsis_nn/*.cc) are already caught
# by the glob above -- create_tflm_tree lists them INSTEAD of the reference ones,
# so there is no duplicate registration.  Drop the f16/f32 sources: we build with
# ARM_NN_ENABLE_F16=0 / F32=0 (matches the Makefile default), and _f16.c needs
# hardware/flags this SP-FPU core lacks.
if(NN_TFLM_CMSIS_NN)
    file(GLOB_RECURSE TFLM_CMSIS_NN_SOURCES CONFIGURE_DEPENDS
         "${TFLM_ROOT}/third_party/cmsis_nn/Source/*.c")
    list(FILTER TFLM_CMSIS_NN_SOURCES EXCLUDE REGEX "(_f16|_f32|arm_nntables_flt)\\.c$")
    list(APPEND TFLM_LIB_SOURCES ${TFLM_CMSIS_NN_SOURCES})
endif()

add_library(tflm STATIC
    ${TFLM_LIB_SOURCES}
    "${BOARD_DIR}/port/nn/tflm/cxx_runtime.cc"   # noexcept operator new/delete + traps
    "${BOARD_DIR}/port/nn/tflm/nn_tflm.cc")      # extern "C" nn_backend_vt_selected

target_link_libraries(tflm PRIVATE bsp_iface)   # MCU_OPTS (fpv5-sp-d16) + CMSIS/HAL includes

# The tree above was generated at configure time for TFLM_GIT_SHA; a superproject
# checkout that moves only the gitlink does not re-configure, so every build first
# re-checks gitlink == submodule HEAD == TFLM_GIT_SHA (cmake/tflm_submodule.cmake).
tflm_submodule_build_check(tflm_pin_check tflm "${TFLM_GIT_SHA}")

target_include_directories(tflm PRIVATE
    "${TFLM_ROOT}"                                   # "tensorflow/..." and "signal/..."
    "${TFLM_ROOT}/third_party/flatbuffers/include"   # "flatbuffers/..."
    "${TFLM_ROOT}/third_party/gemmlowp"              # "fixedpoint/..."
    "${TFLM_ROOT}/third_party/ruy"                   # "ruy/..."
    "${BOARD_DIR}/port/nn")                   # nn.h / nn_backend.h for the bridge

# TF_LITE_STATIC_MEMORY changes TFLM's context/tensor structs across the API boundary,
# so it must be visible to every TU that includes TFLM headers -> PUBLIC.  Stripping
# error strings + NDEBUG remove MicroPrintf/assert -> no stdio/_write/abort.
target_compile_definitions(tflm
    PUBLIC  TF_LITE_STATIC_MEMORY
    PRIVATE TF_LITE_STRIP_ERROR_STRINGS NDEBUG)

# CMSIS-NN: add the downloaded CMSIS_6 core + CMSIS-NN include roots (mirrors the
# tflm Makefile's INCLUDES for cmsis_nn.inc) and the F16/F32 gates.  NN_TFLM_CMSIS_NN
# lets nn_tflm.cc assert __ARM_FEATURE_DSP at compile time.  These are PRIVATE to the
# tflm lib (no HAL/repo-CMSIS TU here), so the matched CMSIS_6 + CMSIS-NN headers win.
if(NN_TFLM_CMSIS_NN)
    target_include_directories(tflm PRIVATE
        "${TFLM_ROOT}/third_party/cmsis"
        "${TFLM_ROOT}/third_party/cmsis/CMSIS/Core/Include"
        "${TFLM_ROOT}/third_party/cmsis_nn"
        "${TFLM_ROOT}/third_party/cmsis_nn/Include")
    # -DCMSIS_NN is what OPTIMIZED_KERNEL_DIR=cmsis_nn adds in the tflm Makefile
    # (uppercased dir name): it switches the kernel headers (pooling.h, conv.h, ...)
    # from inline reference registrations to extern declarations, so the cmsis_nn
    # *.cc supply the definitions.  Without it every optimized kernel TU redefines
    # the reference inline version.  PRIVATE = visible to the whole tflm lib.
    target_compile_definitions(tflm PRIVATE
        CMSIS_NN ARM_NN_ENABLE_F16=0 ARM_NN_ENABLE_F32=0 NN_TFLM_CMSIS_NN=1)
endif()

set_target_properties(tflm PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF)

# Bare-metal C++: no exceptions/RTTI, single-threaded static-init (no __cxa_guard_*),
# no cxa_atexit registration, no unwind tables.  -O2 to match the rest of the build.
# The C++-only flags are scoped to CXX so the CMSIS-NN C sources (cmsis_nn build)
# don't warn "valid for C++ but not for C".
target_compile_options(tflm PRIVATE
    -fno-unwind-tables -fno-asynchronous-unwind-tables -O2
    -Wno-unused-parameter -Wno-sign-compare -Wno-maybe-uninitialized
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-fno-threadsafe-statics;-fno-use-cxa-atexit>)

# Provide the nano C++ runtime archives + libm to whatever links `tflm`, ordered
# AFTER tflm's own objects.  cxx_runtime.cc defines our own operator new/delete, so
# the linker resolves operator new from this archive and never extracts
# libstdc++_nano's throwing operator new (which would drag in __cxa_throw /
# _Unwind_* / bad_alloc).  libm (`m`) is needed because the final link is forced onto
# the C driver (gcc), which -- unlike g++ -- does not auto-link it, and TFLM's
# QuantizeMultiplier uses `round` (double, soft-float on this fpv5-sp-d16 core).
target_link_libraries(tflm PUBLIC stdc++_nano supc++_nano m)

# --- Host-side model checker (scripts/verify_tflite.cc, issue #159) -----------
# Built with the HOST compiler against the same generated tree, so it runs the
# identical tflite::VerifyModelBuffer() the firmware does, and checks a model's
# operators against port/nn/tflm/nn_tflm_ops.h -- the list the firmware
# registers from.  The asset build uses it as this board's model gate from Step
# 4b (cmake/add_asset.cmake's MODEL_VERIFIER); until then it is the
# `verify-model` target, NOT in ALL (compiling schema_generated.h takes a few
# seconds and most builds never need it).  A custom command because every CMake
# target in this project cross-compiles.
find_program(HOST_CXX NAMES c++ g++ clang++)
if(HOST_CXX)
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/verify_tflite"
        COMMAND "${HOST_CXX}" -std=c++17 -O1 -w
                -I "${TFLM_ROOT}"
                -I "${TFLM_ROOT}/third_party/flatbuffers/include"
                -I "${TFLM_ROOT}/third_party/gemmlowp"
                -I "${TFLM_ROOT}/third_party/ruy"
                -I "${BOARD_DIR}/port/nn/tflm"
                "${BOARD_DIR}/scripts/verify_tflite.cc"
                # GetBuiltinCode(): reads an operator code correctly across the
                # schema's deprecated/current field pair.
                "${TFLM_ROOT}/tensorflow/compiler/mlir/lite/schema/schema_utils.cc"
                -o "${CMAKE_BINARY_DIR}/verify_tflite"
        DEPENDS "${BOARD_DIR}/scripts/verify_tflite.cc"
                "${BOARD_DIR}/port/nn/tflm/nn_tflm_ops.h"
        COMMENT "host c++ -> verify_tflite (checks a .tflite before you copy it)"
        VERBATIM)
    add_custom_target(verify-model DEPENDS "${CMAKE_BINARY_DIR}/verify_tflite")
endif()
