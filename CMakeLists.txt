# ============================================================================
#  ThreadX Shell -- multi-board Eclipse ThreadX + shell console firmware.
#
#  ONE BUILD DIRECTORY = ONE BOARD.  The board is chosen at configure time and
#  everything board-specific lives behind boards/<board>/board.cmake, which this
#  file includes: the targets, the linker script, the peripheral options and the
#  post-build gates are all declared there.  What stays here is what every board
#  shares -- the project, the toolchain preconditions, the submodule bootstrap
#  and the generated version header.
#
#  Configure (downloads the ARM toolchain into ./tools on first run):
#    cmake -B build/wio-lite-ai -G Ninja \
#          -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
#          -DBOARD=wio-lite-ai
#  Build:
#    cmake --build build/wio-lite-ai
#  Flash: board-specific -- see boards/<board>/board.cmake and the board README.
#    wio-lite-ai: cmake --build build/wio-lite-ai --target dfu-shell   (DFU only)
#
#  The build directory is build/<board>/ by convention; variant trees of the same
#  board get a suffix (build/wio-lite-ai-tflm).  All of build/ is git-ignored.
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# --- Board selection --------------------------------------------------------
# A cache STRING and not an option(): the value is a directory name under
# boards/.  There is deliberately NO default.  Defaulting to whichever board
# happened to be ported first is how a build silently produces an image for the
# wrong target, and on the wio-lite-ai the cost of discovering that is a DFU
# write to a part with a ~10k erase endurance and exactly one surviving board.
set(BOARD "" CACHE STRING "Target board: a directory name under boards/")

file(GLOB _board_cmakes RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/boards"
     "${CMAKE_CURRENT_SOURCE_DIR}/boards/*/board.cmake")
set(_boards "")
foreach(_bc IN LISTS _board_cmakes)
    get_filename_component(_b "${_bc}" DIRECTORY)
    list(APPEND _boards "${_b}")
endforeach()
list(SORT _boards)
string(REPLACE ";" "\n    " _boards_pretty "${_boards}")

if(NOT BOARD)
    message(FATAL_ERROR
        "No board selected.  Configure with -DBOARD=<board>.\n"
        "  Available boards:\n    ${_boards_pretty}")
endif()
set(BOARD_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}/board.cmake")
if(NOT EXISTS "${BOARD_CMAKE}")
    message(FATAL_ERROR
        "BOARD=${BOARD} has no boards/${BOARD}/board.cmake.\n"
        "  Available boards:\n    ${_boards_pretty}")
endif()

project(ThreadXShell C ASM)

set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# REQUIRED, not optional: the boards' post-build residency checks
# (boards/<board>/cmake/check_*_residency.py) are correctness gates, and a gate
# that quietly skips itself when its interpreter is missing is worse than no
# gate at all -- it reports the same "nothing to see here" as a passing run.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# --- Submodule bootstrap ----------------------------------------------------
# Sentinel files, not directory existence: an uninitialised submodule is an
# EMPTY directory that exists, so testing the directory would never fire.
#
# lib/mlperf-tiny is deliberately absent from this list, and the update below is
# given EXPLICIT paths because of it.  It is a ~340 MB shallow mirror behind
# CONFIG_MLPERF_TINY (OFF by default) whose own fetch-on-demand lives in the
# board's mlperf-tiny.cmake, so an ordinary default configure must neither be
# triggered by its absence nor drag it in -- and a bare `git submodule update
# --init` would do the latter, since git's default pathspec is every registered
# submodule.
#
# The list covers every submodule the ported boards build against.  Adding a
# board that needs a new one (M2: the F7 HAL and CMSIS device) means adding its
# sentinel here -- the fetch list is derived from these, so there is no second
# list to keep in step.
set(_submodule_sentinels
    "lib/stm32h7xx_hal_driver/Src/stm32h7xx_hal.c"
    "lib/cmsis_core/Include/core_cm7.h"
    "lib/cmsis_device_h7/Include/stm32h725xx.h"
    "lib/tinyusb/src/tusb.h"
    "lib/threadx/common/src/tx_thread_create.c"
    "lib/coremark/core_main.c"
    "lib/netxduo/common/src/nx_ip_create.c"
    "lib/flashdb/src/fdb_kvdb.c"
    "lib/filex/common/src/fx_media_open.c")
set(_need_submodules FALSE)
set(_submodule_paths "")
foreach(_s IN LISTS _submodule_sentinels)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
        set(_need_submodules TRUE)
    endif()
    # "lib/<name>/some/sentinel.c" -> "lib/<name>"
    string(REGEX REPLACE "^(lib/[^/]+)/.*$" "\\1" _p "${_s}")
    list(APPEND _submodule_paths "${_p}")
endforeach()
list(REMOVE_DUPLICATES _submodule_paths)
if(_need_submodules)
    message(STATUS "Initializing git submodules ...")
    execute_process(
        COMMAND git submodule update --init -- ${_submodule_paths}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _sm)
    if(NOT _sm EQUAL 0)
        message(FATAL_ERROR "git submodule update failed")
    endif()
endif()

# --- The board ---------------------------------------------------------------
# Declares BOARD_DIR, BOARD_FW_NAME and GEN_DIR, and builds the board's targets.
set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}")
include("${BOARD_CMAKE}")

# --- cli_version.h -----------------------------------------------------------
# AFTER the board: configure_file() evaluates immediately, so BOARD_FW_NAME (set
# by board.cmake) and GEN_DIR (likewise) have to exist by the time this runs.
# GEN_DIR is already on the board's include path, which is what lets
# cmd_system.c say #include "cli_version.h".
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --always --dirty --tags
    OUTPUT_VARIABLE CLI_GIT_DESC OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE _gitrc)
if(NOT _gitrc EQUAL 0)
    set(CLI_GIT_DESC "unknown")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cli_version.h.in"
               "${GEN_DIR}/cli_version.h" @ONLY)
