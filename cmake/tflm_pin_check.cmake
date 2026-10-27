# ============================================================================
#  Build-time pin check for lib/tflite-micro (issue #134).  Run with cmake -P by
#  the target tflm_submodule_build_check() adds; see cmake/tflm_submodule.cmake.
#
#  Inputs: SRC_ROOT (the superproject), SUB_DIR (lib/tflite-micro), PIN (the
#  commit this build tree was configured with, i.e. the tree it generated).
#
#  Fails unless the superproject's gitlink, the submodule's HEAD and PIN are the
#  same commit.  A configure dependency on the submodule's HEAD file already
#  re-runs configure for a checkout inside the submodule; this catches the case
#  it cannot see -- a superproject checkout (or reset, or staged update) that
#  moves only the gitlink -- without making .git/index a configure dependency.
#  Reads only: neither command below writes the index.
# ============================================================================
# git runs through FETCH_UPSTREAM_GIT, as at configure (cmake/tflm_submodule.cmake).
# The log level hides the "Found Git" line find_package would print on every build.
set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)
include("${CMAKE_CURRENT_LIST_DIR}/fetch_upstream.cmake")
unset(CMAKE_MESSAGE_LOG_LEVEL)

foreach(_v SRC_ROOT SUB_DIR PIN)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "tflm_pin_check: ${_v} is not set")
    endif()
endforeach()

set(_gitlink "<unreadable>")
execute_process(
    COMMAND ${FETCH_UPSTREAM_GIT} -C "${SRC_ROOT}" ls-files -s -- lib/tflite-micro
    OUTPUT_VARIABLE _ls OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rc ERROR_QUIET)
if(_rc EQUAL 0 AND _ls MATCHES "^160000 ([0-9a-f]+) 0\tlib/tflite-micro$")
    set(_gitlink "${CMAKE_MATCH_1}")
endif()

# Same test as configure: git must answer for the submodule, not the superproject.
set(_head "<unreadable>")
execute_process(
    COMMAND ${FETCH_UPSTREAM_GIT} -C "${SUB_DIR}" rev-parse --show-toplevel
    OUTPUT_VARIABLE _top OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rc ERROR_QUIET)
if(_rc EQUAL 0)
    file(REAL_PATH "${_top}" _top)
    file(REAL_PATH "${SUB_DIR}" _want)
    if(_top STREQUAL _want)
        execute_process(
            COMMAND ${FETCH_UPSTREAM_GIT} -C "${SUB_DIR}" rev-parse --verify -q HEAD
            OUTPUT_VARIABLE _h OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _rc ERROR_QUIET)
        if(_rc EQUAL 0 AND _h MATCHES "^[0-9a-f]+$")
            set(_head "${_h}")
        endif()
    endif()
endif()

if(NOT _gitlink STREQUAL PIN OR NOT _head STREQUAL PIN)
    message(FATAL_ERROR
        "tflm: lib/tflite-micro no longer matches the commit this build tree was "
        "configured with.\n"
        "  superproject gitlink (git ls-files -s): ${_gitlink}\n"
        "  submodule HEAD       (git rev-parse):   ${_head}\n"
        "  configured pin       (TFLM_GIT_SHA):    ${PIN}\n"
        "  If the gitlink moved, bring the submodule along and re-configure:\n"
        "    git submodule update -- lib/tflite-micro && cmake <build dir>\n"
        "  (the re-configure regenerates the tflm tree for the new pin).")
endif()
