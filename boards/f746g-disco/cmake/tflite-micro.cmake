# tflite-micro (TFLM) C++ backend for CONFIG_NN_BACKEND=tflm (Epic owhinata/stm32f746g-disco#80 P3,
# owhinata/stm32f746g-disco#86).
#
# Included ONLY from the tflm branch of board.cmake, so C++ is enabled for this
# build alone; a null/stedgeai firmware never sees enable_language(CXX) and stays
# C-only.  All C++ is confined to this `tflm` STATIC lib so the
# final shell link keeps the C driver (gcc) and does not auto-pull full libstdc++.
#
# The tflite-micro sources are NOT vendored.  At CONFIGURE time we fetch the pinned
# upstream commit and run its own `create_tflm_tree.py` (which drives the tflite-micro
# Makefile to list library sources, download the header-only third-party deps
# flatbuffers/gemmlowp/ruy, and generate the model-data C array) into a self-contained,
# glob-able tree under the build dir.  We then compile that tree with OUR exact flags
# (so the ABI -- cortex-m7 / fpv5-sp-d16 / hard / -fno-exceptions -- is fully ours).
# This mirrors the repo's "download the toolchain at configure" convention; only the
# tflm build pays the fetch cost, and it is cached (SHA-keyed stamp) after the first run.

enable_language(CXX)

# --- Pinned upstream ---------------------------------------------------------
set(TFLM_GIT_URL "https://github.com/tensorflow/tflite-micro.git")
set(TFLM_GIT_SHA "e142972d4f4382f77faa8212b7c70b37d28b9cb9")   # tflite-micro main, 2026-07
set(TFLM_SRC  "${CMAKE_BINARY_DIR}/tflm-src")     # shallow clone at the pinned SHA
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

# --- Fetch + generate the tree (SHA-keyed stamp: only runs once per pin) ------
# The stamp is written ONLY after a fully successful generation, and each attempt starts
# from a clean clone + a fresh dependency install, so an interrupted run self-heals on
# the next configure (no stale partial third-party downloads, no half-populated venv).
if(NOT EXISTS "${TFLM_STAMP}")
    message(STATUS "tflm: fetching tflite-micro @ ${TFLM_GIT_SHA} and generating the source tree "
                   "(one-time, a few minutes -- clones + downloads flatbuffers/gemmlowp/ruy)")

    # Ensure numpy/Pillow are present (idempotent when already satisfied); heals a venv
    # created before these were added to requirements.txt.
    execute_process(
        COMMAND "${TFLM_VENV}/bin/pip" install -q --disable-pip-version-check
                -r "${BOARD_DIR}/requirements.txt"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: pip install -r requirements.txt failed")
    endif()

    # Fresh clone each attempt: upstream's third-party extractor skips a download dir
    # that merely exists, so a previously interrupted download would never self-repair
    # in place.  Wiping the clone also avoids any stale remote/HEAD idempotency issues.
    file(REMOVE_RECURSE "${TFLM_SRC}")
    file(MAKE_DIRECTORY "${TFLM_SRC}")
    execute_process(COMMAND git init -q WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git init failed")
    endif()
    execute_process(COMMAND git remote add origin "${TFLM_GIT_URL}"
                    WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git remote add failed")
    endif()
    execute_process(COMMAND git fetch --depth 1 -q origin "${TFLM_GIT_SHA}"
                    WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git fetch ${TFLM_GIT_SHA} failed (network + reachable SHA needed)")
    endif()
    execute_process(COMMAND git checkout -q -f FETCH_HEAD
                    WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git checkout FETCH_HEAD failed")
    endif()

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

# --- BlazeFace model -> compilable byte array (BUILD time, issue #130) -------
# The .tflite is NOT committed (public repo, model-zoo license -- see .gitignore
# *.tflite).  The built-in model is the SAME pinned upstream file wio-lite-ai and
# grove-vision-ai-v2 ship as their blazeface asset: fetched at build time by the
# shared cmake/fetch_model.cmake (exact commit, content SHA256 decides, a Git LFS
# pointer is refused, nothing is published until it has passed), then turned into
# a C++ TU inside the `tflm` lib with a fixed symbol name (g_blazeface_model_data)
# by our own generator (cmake/gen_model_array.py: no numpy/Pillow, alignas(16)).
#
# [!] THE ARRAY DEPENDS ON THE VERIFIED FILE, NOT ON A PATH THAT MIGHT HOLD IT.
# Generation is a custom command whose input is the fetch's OUTPUT, so a pin that
# can no longer be fetched (commit gone, LFS pointer, different bytes) fails the
# build instead of leaving an older array -- or no model at all -- in the image.
#
# [!] A DELIBERATE DIFFERENCE FROM THE ASSET RULE (issue #94): an asset's fetch is
# outside ALL so a tree that cannot reach the model host still builds firmware.
# Here the model is part of the firmware image, so the default (tflm) firmware
# needs the network once -- it already does, for tflite-micro itself above.  A
# CONFIG_NN_BACKEND=null build never includes this file and stays offline.
#
# Nothing under the build may reference _ref/ (CLAUDE.md), so there is still no
# path default: the pin is the default.
set(TFLM_MODEL_URL     "https://github.com/STMicroelectronics/stm32ai-modelzoo.git")
set(TFLM_MODEL_COMMIT  "1423c78953a830903485135febe1dd98ff31aed8")
set(TFLM_MODEL_PATH_IN "face_detection/facedetect_front/Public_pretrainedmodel_public_dataset/widerface/blazeface_front_128/blazeface_front_128_int8.tflite")
set(TFLM_MODEL_SHA256  "e803bb4e93b10f7a19d4243bcc39698599a723f3128e19d3f90e0b1c0bc88dd8")

set(TFLM_MODEL_DIR    "${CMAKE_BINARY_DIR}/tflm-model")
set(TFLM_MODEL_CC     "${TFLM_MODEL_DIR}/blazeface_model_data.cc")
set(TFLM_MODEL_H      "${TFLM_MODEL_DIR}/blazeface_model_data.h")
set(TFLM_MODEL_SYMBOL "g_blazeface_model_data")
set(TFLM_MODEL_GEN    "${BOARD_DIR}/cmake/gen_model_array.py")

# A local model instead of the pin, for trying another .tflite.  NOT hash-checked
# (an override deliberately supplies different content) -- the same contract as
# the asset rule's override.  Empty = the pin.
#
# [!] RENAMED FROM NN_TFLM_MODEL, AND THE OLD NAME IS IGNORED LOUDLY.  Until #130
# that variable was required and existing trees hold it pointing into _ref/ (the
# donor's layout).  Reading it would keep those trees building from a path no
# clone has, and from a file nobody verified -- silently, since the build would
# look exactly like the pinned one.  A new name means only someone who asks for
# an override gets one; the warning is so an operator who did mean it is told.
set(NN_TFLM_MODEL_OVERRIDE "" CACHE FILEPATH
    "Local .tflite for the tflm built-in model instead of the pinned upstream one (not hash-checked)")
if(DEFINED CACHE{NN_TFLM_MODEL} AND NOT "$CACHE{NN_TFLM_MODEL}" STREQUAL "")
    message(WARNING
        "tflm: NN_TFLM_MODEL is IGNORED since issue #130 -- this tree still holds "
        "`${NN_TFLM_MODEL}`.  The built-in model is now the pinned upstream "
        "BlazeFace (commit ${TFLM_MODEL_COMMIT}, sha256 verified).  To build from "
        "a local file instead, pass -DNN_TFLM_MODEL_OVERRIDE=<path to .tflite>; "
        "to silence this, pass -UNN_TFLM_MODEL.")
endif()

if(NN_TFLM_MODEL_OVERRIDE)
    if(NOT EXISTS "${NN_TFLM_MODEL_OVERRIDE}")
        message(FATAL_ERROR "tflm: model not found:\n  ${NN_TFLM_MODEL_OVERRIDE}\n"
                            "Fix -DNN_TFLM_MODEL_OVERRIDE, or clear it to use the pin.")
    endif()
    set(TFLM_MODEL_SRC "${NN_TFLM_MODEL_OVERRIDE}")
    # ninja reruns on a NEWER input only, so a file replaced by an OLDER copy at
    # the same path would keep the old array -- and a configure-time record
    # would not see it either, since configure reruns on a newer file too.  So
    # the change is detected at BUILD time: a step that runs on every build
    # hashes the file's CONTENT into override.id, rewriting it only when the hash
    # differs (restat: an unchanged record is not a reason to regenerate), and
    # the array depends on that record.  This detects a change; it is not a
    # check of WHAT the content is -- the override stays unverified.
    set(TFLM_MODEL_ID "${TFLM_MODEL_DIR}/override.id")
    set(TFLM_MODEL_ID_SCRIPT "${TFLM_MODEL_DIR}/override_id.cmake")
    file(CONFIGURE OUTPUT "${TFLM_MODEL_ID_SCRIPT}" CONTENT [=[
file(SHA256 "${SRC}" _h)
set(_want "${SRC}|${_h}
")
set(_have "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _have)
endif()
if(NOT _have STREQUAL _want)
    file(WRITE "${OUT}" "${_want}")
endif()
]=] @ONLY)
    # A custom TARGET, not a command: it runs on every build, and declaring the
    # record as its BYPRODUCT is what gives ninja the edge to the array below.
    add_custom_target(tflm_override_id
        BYPRODUCTS "${TFLM_MODEL_ID}"
        COMMAND "${CMAKE_COMMAND}" "-DSRC=${TFLM_MODEL_SRC}" "-DOUT=${TFLM_MODEL_ID}"
                -P "${TFLM_MODEL_ID_SCRIPT}"
        COMMENT "tflm: hash the override model"
        VERBATIM)
    set(TFLM_MODEL_SRC_DEPENDS "${TFLM_MODEL_SRC}" "${TFLM_MODEL_ID}")
    set(TFLM_MODEL_VERIFY "")     # an override is not hash-checked: no --sha256
    message(STATUS "tflm: built-in model OVERRIDDEN by ${TFLM_MODEL_SRC} (not the pin, not hash-checked)")
else()
    set(TFLM_MODEL_SRC "${TFLM_MODEL_DIR}/pinned.tflite")
    add_custom_command(
        OUTPUT "${TFLM_MODEL_SRC}"
        COMMAND "${CMAKE_COMMAND}"
                "-DURL=${TFLM_MODEL_URL}" "-DCOMMIT=${TFLM_MODEL_COMMIT}"
                "-DPATH_IN=${TFLM_MODEL_PATH_IN}" "-DSHA256=${TFLM_MODEL_SHA256}"
                "-DOUT=${TFLM_MODEL_SRC}" "-DWORK=${TFLM_MODEL_DIR}/fetch-work"
                "-DOVERRIDE=NN_TFLM_MODEL_OVERRIDE"
                -P "${CMAKE_SOURCE_DIR}/cmake/fetch_model.cmake"
        DEPENDS "${CMAKE_SOURCE_DIR}/cmake/fetch_model.cmake"
        COMMENT "tflm: fetch the pinned BlazeFace model"
        VERBATIM)
    set(TFLM_MODEL_SRC_DEPENDS "${TFLM_MODEL_SRC}")
    # [!] AND RE-VERIFIED AT USE: the fetch checked the file when it published
    # it, the generator checks the bytes it emits.  The same variable feeds
    # both, so the pin is written once.  An empty value reaches the generator
    # as an empty --sha256, which it refuses.
    set(TFLM_MODEL_VERIFY --sha256 "${TFLM_MODEL_SHA256}")
    message(STATUS "tflm: built-in model = pinned BlazeFace front 128 int8 (fetched at build time)")
endif()

add_custom_command(
    OUTPUT "${TFLM_MODEL_CC}" "${TFLM_MODEL_H}"
    COMMAND "${Python3_EXECUTABLE}" "${TFLM_MODEL_GEN}"
            "${TFLM_MODEL_SRC}" "${TFLM_MODEL_CC}" "${TFLM_MODEL_H}" "${TFLM_MODEL_SYMBOL}"
            ${TFLM_MODEL_VERIFY}
    DEPENDS ${TFLM_MODEL_SRC_DEPENDS} "${TFLM_MODEL_GEN}"
    COMMENT "tflm: generate the built-in model array"
    VERBATIM)

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
    "${TFLM_MODEL_CC}"                                  # generated BlazeFace model bytes
    "${TFLM_MODEL_H}"     # generated too: listed so nn_tflm.cc waits for it
    "${BOARD_DIR}/port/nn/tflm/cxx_runtime.cc"   # noexcept operator new/delete + traps
    "${BOARD_DIR}/port/nn/tflm/nn_tflm.cc")      # extern "C" nn_backend_vt_selected

if(TARGET tflm_override_id)
    add_dependencies(tflm tflm_override_id)
endif()

target_link_libraries(tflm PRIVATE bsp_iface)   # MCU_OPTS (fpv5-sp-d16) + CMSIS/HAL includes

target_include_directories(tflm PRIVATE
    "${TFLM_ROOT}"                                   # "tensorflow/..." and "signal/..."
    "${TFLM_ROOT}/third_party/flatbuffers/include"   # "flatbuffers/..."
    "${TFLM_ROOT}/third_party/gemmlowp"              # "fixedpoint/..."
    "${TFLM_ROOT}/third_party/ruy"                   # "ruy/..."
    "${TFLM_MODEL_DIR}"                              # generated "blazeface_model_data.h"
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
