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
#    wio-lite-ai:  cmake --build build/wio-lite-ai  --target flash      (DFU only,
#                  alias of dfu-shell; hold USER/PF1 at reset first)
#    f746g-disco:  cmake --build build/f746g-disco  --target flash      (ST-Link)
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
# The list of sentinels is the SELECTED BOARD's, from boards/<board>/
# submodules.cmake, and the fetch pathspec below is derived from it -- so there
# is no second list to keep in step, and a board only ever pays for the mirrors
# it compiles.  One shared list would make `-DBOARD=wio-lite-ai` clone the F7
# side as well (lib/guix alone is ~1.5 GB) for sources it never opens.
#
# The update is given EXPLICIT paths for a second reason: lib/mlperf-tiny is a
# ~340 MB shallow mirror behind CONFIG_MLPERF_TINY (OFF by default) with its own
# fetch-on-demand in the board's mlperf-tiny.cmake, and a bare `git submodule
# update --init` would drag it in -- git's default pathspec is every registered
# submodule.  It appears in no board's sentinel list.
#
# Adding a board means writing its boards/<board>/submodules.cmake; there is no
# central list to edit.
set(_board_submodules "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}/submodules.cmake")
if(NOT EXISTS "${_board_submodules}")
    message(FATAL_ERROR
        "BOARD=${BOARD} has no boards/${BOARD}/submodules.cmake.\n"
        "  Every board declares the upstream mirrors it builds against there;\n"
        "  see boards/wio-lite-ai/submodules.cmake for the format.")
endif()
include("${_board_submodules}")
if(NOT _submodule_sentinels)
    message(FATAL_ERROR
        "boards/${BOARD}/submodules.cmake set no _submodule_sentinels.\n"
        "  An empty list would silently skip the bootstrap and fail later as a\n"
        "  missing header, so it is rejected here instead.")
endif()
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
