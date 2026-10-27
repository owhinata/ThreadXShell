# ============================================================================
#  tflite-micro from the lib/tflite-micro submodule (issue #134).
#
#  Shared by every board whose tflm backend generates its source tree with
#  upstream's create_tflm_tree.py (f746g-disco, wio-lite-ai).  Nothing below
#  knows which board is asking: the board keeps its own stamp, venv and
#  generation step, this file only answers "which commit" and "give me a
#  private copy of it".
#
#  [!] THE GENERATOR NEVER RUNS INSIDE THE SUBMODULE.  create_tflm_tree.py
#  downloads its third-party dependencies (~300 MB) into a fixed path INSIDE
#  the tflite-micro tree it is run from, and lib/ is read-only.  So the pinned
#  commit is written out with `git archive` into the build directory and the
#  generator runs there -- a fresh copy on every attempt, which is also what
#  lets an interrupted download heal on the next configure (upstream skips a
#  download directory that merely exists).  `git archive` takes the COMMIT, so
#  uncommitted edits in the submodule's work tree never reach the build.
#
#  [!] THE PIN IS THE SUPERPROJECT'S GITLINK.  tflm_submodule_pin() refuses a
#  submodule whose checked-out HEAD is not the commit the superproject records
#  for it (in the index, so a staged pin bump is what gets built), exactly as
#  the SHA constant this replaced refused anything else.  Bumping the pin is
#  advancing the submodule and committing the gitlink; the submodule's HEAD
#  file is a configure dependency, so the next build re-configures.
#
#  [!] FETCHED BY ITS USER, NOT BY THE BOARD'S SENTINEL LIST.  Only the tflm
#  backend reads lib/tflite-micro, so it is in no boards/<board>/submodules.cmake:
#  a CONFIG_NN_BACKEND=null configure must stay offline.  tflm_submodule_pin()
#  initialises the submodule itself when it finds it uninitialised -- the same
#  fetch-on-demand as lib/mlperf-tiny.
#
#  [!] THE GITLINK IS RE-CHECKED ON EVERY BUILD.  The HEAD file as a configure
#  dependency catches a checkout INSIDE the submodule, but not a superproject
#  checkout that moves only the gitlink (the submodule's HEAD file is untouched).
#  The superproject's .git/index cannot be that dependency instead: `git status`
#  rewrites it, so nearly every git command would re-configure.  So
#  tflm_submodule_build_check() adds a target in ALL that compares the gitlink,
#  the submodule HEAD and the pin this tree was configured with, and the tflm
#  library waits for it.
#
#  Configure still needs the network and Python: the generator's dependency
#  downloads happen whether or not tflite-micro itself was cloned.
#
#  Every git call here (and in cmake/tflm_pin_check.cmake) starts git through
#  FETCH_UPSTREAM_GIT from cmake/fetch_upstream.cmake (issue #136), so a GIT_DIR or
#  GIT_WORK_TREE inherited from the shell cannot make it answer for, or check
#  out into, another repository.
# ============================================================================
include("${CMAKE_CURRENT_LIST_DIR}/fetch_upstream.cmake")

set(TFLM_SUBMODULE_DIR "${CMAKE_SOURCE_DIR}/lib/tflite-micro")

# Sets <out_var> to TRUE when git, asked inside lib/tflite-micro, answers for the
# submodule itself.  An uninitialised submodule is an empty directory INSIDE the
# superproject, where `git -C` silently answers for the superproject instead.
function(_tflm_submodule_is_init out_var)
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${TFLM_SUBMODULE_DIR}" rev-parse --show-toplevel
        OUTPUT_VARIABLE _top OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc ERROR_QUIET)
    file(REAL_PATH "${TFLM_SUBMODULE_DIR}" _want)
    if(_rc EQUAL 0)
        file(REAL_PATH "${_top}" _top)
    endif()
    if(_rc EQUAL 0 AND _top STREQUAL _want)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Sets <out_var> to the pinned commit, after checking the submodule's HEAD
# against the superproject's gitlink.  Fatal if they differ or either is unreadable.
function(tflm_submodule_pin out_var)
    _tflm_submodule_is_init(_init)
    if(NOT _init)
        message(STATUS "tflm: fetching the lib/tflite-micro submodule (~40 MB) ...")
        execute_process(
            COMMAND ${FETCH_UPSTREAM_GIT} submodule update --init -- lib/tflite-micro
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE _rc)
        _tflm_submodule_is_init(_init)
        if(NOT _rc EQUAL 0 OR NOT _init)
            message(FATAL_ERROR
                "tflm: could not initialise the lib/tflite-micro submodule; run\n"
                "    git submodule update --init -- lib/tflite-micro")
        endif()
    endif()
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${TFLM_SUBMODULE_DIR}" rev-parse --verify -q HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT _head MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "tflm: cannot read the checked-out commit of lib/tflite-micro")
    endif()
    # Mode 160000 is a gitlink; anything else means lib/tflite-micro is not a
    # submodule of this repository at all.
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${CMAKE_SOURCE_DIR}" ls-files -s -- lib/tflite-micro
        OUTPUT_VARIABLE _ls OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT _ls MATCHES "^160000 ([0-9a-f]+) 0\tlib/tflite-micro$")
        message(FATAL_ERROR
            "tflm: the superproject records no gitlink for lib/tflite-micro "
            "(git ls-files -s said: '${_ls}')")
    endif()
    set(_pin "${CMAKE_MATCH_1}")
    if(NOT _head STREQUAL _pin)
        message(FATAL_ERROR
            "tflm: lib/tflite-micro is checked out at ${_head},\n"
            "  but this repository pins ${_pin}.\n"
            "  Restore the pin with\n"
            "    git submodule update -- lib/tflite-micro\n"
            "  or, to move the pin, commit the new gitlink (git add lib/tflite-micro).")
    endif()
    # The HEAD file changes on every checkout of the submodule, so moving it
    # re-runs configure (and with it the check above and the stamp lookup).
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${TFLM_SUBMODULE_DIR}" rev-parse --path-format=absolute --git-path HEAD
        OUTPUT_VARIABLE _head_file OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_head_file}")
        message(FATAL_ERROR "tflm: cannot locate the HEAD file of lib/tflite-micro")
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
                 CMAKE_CONFIGURE_DEPENDS "${_head_file}")
    set(${out_var} "${_pin}" PARENT_SCOPE)
endfunction()

# Replaces <dest> with the files of <sha> from the submodule (no .git).
function(tflm_submodule_export sha dest)
    set(_tar "${dest}.tar")
    file(REMOVE_RECURSE "${dest}")
    file(REMOVE "${_tar}")
    file(MAKE_DIRECTORY "${dest}")
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${TFLM_SUBMODULE_DIR}" archive --format=tar -o "${_tar}" "${sha}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git archive ${sha} of lib/tflite-micro failed")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${_tar}"
        WORKING_DIRECTORY "${dest}" RESULT_VARIABLE _rc)
    file(REMOVE "${_tar}")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: extracting lib/tflite-micro @ ${sha} failed")
    endif()
endfunction()

# Adds <check_target> to ALL: on every build it compares the superproject's gitlink,
# the submodule's checked-out HEAD and <sha> (the pin this tree was configured
# with), and fails the build unless all three agree.  <user_target> -- the
# library compiled from the generated tree -- waits for it.
function(tflm_submodule_build_check check_target user_target sha)
    add_custom_target(${check_target} ALL
        COMMAND "${CMAKE_COMMAND}"
                "-DSRC_ROOT=${CMAKE_SOURCE_DIR}" "-DSUB_DIR=${TFLM_SUBMODULE_DIR}"
                "-DPIN=${sha}"
                -P "${CMAKE_SOURCE_DIR}/cmake/tflm_pin_check.cmake"
        COMMENT "tflm: check lib/tflite-micro against the configured pin"
        VERBATIM)
    add_dependencies(${user_target} ${check_target})
endfunction()
