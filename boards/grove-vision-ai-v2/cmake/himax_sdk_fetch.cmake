# ============================================================================
#  Himax WiseEye2 SDK acquisition -- the functions (issue #158).
#
#  Split out of himax_sdk.cmake so that a fixture (cmake/fixtures/
#  run_git_env_tests.py) runs THESE functions against a small file://
#  upstream.  The pin and the URL stay literals in himax_sdk.cmake, the one
#  caller: they are arguments here, never cache variables a configure could
#  override.  The design rules are in himax_sdk.cmake's header.
#
#  [!] EVERY GIT CALL GOES THROUGH FETCH_UPSTREAM_GIT (cmake/fetch_upstream.cmake),
#  never a bare ${GIT_EXECUTABLE}.  With GIT_DIR and GIT_WORK_TREE inherited from
#  the shell (a git hook, `git rebase -x`), the HEAD check reads the
#  SUPERPROJECT's HEAD, which is never the pin -- so the fetch path runs, `init`
#  re-initialises the superproject, `fetch` writes the SDK's objects and a
#  shallow file into it, and `checkout --detach --force` detaches its HEAD and
#  writes the SDK over its working tree.  An inherited `git -c` (GIT_CONFIG_*)
#  could point the fetch at another remote with url.<x>.insteadOf.
# ============================================================================
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/fetch_upstream.cmake")

# Returns the checkout's HEAD in ${out_var}, or "" if the dir is not a git tree.
function(_grove_sdk_head dir out_var)
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${dir}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        set(_head "")
    endif()
    set(${out_var} "${_head}" PARENT_SCOPE)
endfunction()

# HEAD == pin proves only the checkout's parent commit; the files CMake
# actually compiles/links/copies are the WORKING TREE.  A modified or
# untracked-shadowed tree at the right HEAD would silently build unreviewed
# SDK contents, so any deviation (tracked edits or untracked files) is fatal.
function(_grove_sdk_assert_clean dir)
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${dir}" status --porcelain
        OUTPUT_VARIABLE _dirty RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "git status failed in the Himax SDK tree at ${dir}")
    endif()
    if(NOT _dirty STREQUAL "")
        string(REPLACE "\n" "\n    " _dirty_pretty "${_dirty}")
        message(FATAL_ERROR
            "Himax SDK tree at ${dir} deviates from the pinned commit\n"
            "(the tree is read-only by project invariant -- HEAD alone does\n"
            "not prove the sources; the working tree is what gets built):\n"
            "    ${_dirty_pretty}\n"
            "  Restore it (git -C <dir> checkout -- . ; remove untracked\n"
            "  files) or point GROVE_SDK_DIR at a clean checkout.")
    endif()
endfunction()

# A user-supplied checkout (GROVE_SDK_DIR): validate, never mutate.
function(grove_sdk_check_dir dir pin)
    _grove_sdk_head("${dir}" _sdk_head)
    if(NOT _sdk_head STREQUAL pin)
        message(FATAL_ERROR
            "GROVE_SDK_DIR=${dir} is not at the pinned SDK commit.\n"
            "  HEAD:   ${_sdk_head}\n"
            "  pinned: ${pin}\n"
            "  Check out the pin there, or unset GROVE_SDK_DIR to let the build\n"
            "  fetch its own copy into boards/grove-vision-ai-v2/sdk/.")
    endif()
    _grove_sdk_assert_clean("${dir}")
endfunction()

# The build's own checkout at <root>: fetched when its HEAD is not <pin>, then
# checked clean either way.
function(grove_sdk_fetch root url pin)
    _grove_sdk_head("${root}" _sdk_head)
    if(NOT _sdk_head STREQUAL pin)
        message(STATUS
            "Fetching Himax WiseEye2 SDK @ ${pin} (~480 MB on first fetch) ...")
        file(MAKE_DIRECTORY "${root}")
        # init is idempotent; a previous partial fetch is simply retried on top.
        execute_process(
            COMMAND ${FETCH_UPSTREAM_GIT} -C "${root}" init -q
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "git init failed in ${root}")
        endif()
        execute_process(
            COMMAND ${FETCH_UPSTREAM_GIT} -C "${root}"
                    fetch --depth 1 "${url}" "${pin}"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "Himax SDK fetch failed.\n"
                "  command: git fetch --depth 1 ${url} ${pin}\n"
                "  workdir: ${root}\n"
                "  Check network access, then re-run the configure.  To use an\n"
                "  existing checkout instead, pass -DGROVE_SDK_DIR=<path>.")
        endif()
        execute_process(
            COMMAND ${FETCH_UPSTREAM_GIT} -C "${root}"
                    checkout -q --detach --force "${pin}"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "Himax SDK checkout failed.\n"
                "  command: git checkout --detach --force ${pin}\n"
                "  workdir: ${root}")
        endif()
        _grove_sdk_head("${root}" _sdk_head)
        if(NOT _sdk_head STREQUAL pin)
            message(FATAL_ERROR
                "Himax SDK checkout verification failed.\n"
                "  HEAD:   ${_sdk_head}\n"
                "  pinned: ${pin}\n"
                "  url:    ${url}")
        endif()
    endif()
    _grove_sdk_assert_clean("${root}")
endfunction()

# Sanity: the files this board actually compiles/links must exist.  A truncated
# or foreign tree fails HERE with a named cause, not later as a missing header.
function(grove_sdk_probe root)
    foreach(_probe
            "EPII_CM55M_APP_S/device/WE2_core.c"
            "EPII_CM55M_APP_S/prebuilt_libs/gnu/libdriver.a"
            "we2_image_gen_local/we2_local_image_gen"
            "xmodem/xmodem_send.py")
        if(NOT EXISTS "${root}/${_probe}")
            message(FATAL_ERROR
                "Himax SDK tree at ${root} is missing ${_probe}.\n"
                "  The checkout is incomplete or not the expected repository.")
        endif()
    endforeach()
endfunction()
