# ============================================================================
#  Himax WiseEye2 SDK acquisition -- configure-time, pinned, no submodule.
#
#  Fetches HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2 at an exact commit
#  into boards/grove-vision-ai-v2/sdk/ (git-ignored).  The pin matches the
#  donor build environment (~/work/grove-vision-ai-v2) that proved flashing on
#  the one physical board, so the driver archives, image generator and xmodem
#  flow here are byte-identical to what already works.
#
#  Design rules (from the reviewed M-G1 plan):
#   - Fetch the SHA DIRECTLY (git fetch <url> <sha>).  A default-branch shallow
#     clone stops containing the pin the moment upstream moves on.
#   - Any failure or a HEAD mismatch is FATAL_ERROR, with the command, URL and
#     full SHA in the message.  Never fall through to a half-populated tree.
#   - GROVE_SDK_DIR (cache PATH, default empty) points at an existing checkout
#     to skip the network entirely -- but it must sit at the pinned commit.
#     No fallback to _ref/ or to any path outside this repository.
#   - The nested CMSIS-CV submodule is NOT initialised: nothing in this board
#     builds it, and it would drag another mirror in.
#
#  Provides: GROVE_SDK_ROOT       -- the repo checkout root
#            GROVE_SDK_APP_DIR    -- ${GROVE_SDK_ROOT}/EPII_CM55M_APP_S
# ============================================================================

set(GROVE_SDK_PIN "933810ccd5deb1adf2ae8005a8dfca78fc0cb63a")
set(GROVE_SDK_URL "https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2.git")

set(GROVE_SDK_DIR "" CACHE PATH
    "Existing Seeed_Grove_Vision_AI_Module_V2 checkout to use instead of fetching (must be at the pinned commit)")

find_package(Git REQUIRED)

# Returns the checkout's HEAD in ${out_var}, or "" if the dir is not a git tree.
function(_grove_sdk_head dir out_var)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
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
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" status --porcelain
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

if(GROVE_SDK_DIR)
    # User-supplied checkout: validate, never mutate.
    _grove_sdk_head("${GROVE_SDK_DIR}" _sdk_head)
    if(NOT _sdk_head STREQUAL GROVE_SDK_PIN)
        message(FATAL_ERROR
            "GROVE_SDK_DIR=${GROVE_SDK_DIR} is not at the pinned SDK commit.\n"
            "  HEAD:   ${_sdk_head}\n"
            "  pinned: ${GROVE_SDK_PIN}\n"
            "  Check out the pin there, or unset GROVE_SDK_DIR to let the build\n"
            "  fetch its own copy into boards/grove-vision-ai-v2/sdk/.")
    endif()
    _grove_sdk_assert_clean("${GROVE_SDK_DIR}")
    set(GROVE_SDK_ROOT "${GROVE_SDK_DIR}")
else()
    set(GROVE_SDK_ROOT "${BOARD_DIR}/sdk")
    _grove_sdk_head("${GROVE_SDK_ROOT}" _sdk_head)
    if(NOT _sdk_head STREQUAL GROVE_SDK_PIN)
        message(STATUS
            "Fetching Himax WiseEye2 SDK @ ${GROVE_SDK_PIN} (~480 MB on first fetch) ...")
        file(MAKE_DIRECTORY "${GROVE_SDK_ROOT}")
        # init is idempotent; a previous partial fetch is simply retried on top.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${GROVE_SDK_ROOT}" init -q
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "git init failed in ${GROVE_SDK_ROOT}")
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${GROVE_SDK_ROOT}"
                    fetch --depth 1 "${GROVE_SDK_URL}" "${GROVE_SDK_PIN}"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "Himax SDK fetch failed.\n"
                "  command: git fetch --depth 1 ${GROVE_SDK_URL} ${GROVE_SDK_PIN}\n"
                "  workdir: ${GROVE_SDK_ROOT}\n"
                "  Check network access, then re-run the configure.  To use an\n"
                "  existing checkout instead, pass -DGROVE_SDK_DIR=<path>.")
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${GROVE_SDK_ROOT}"
                    checkout -q --detach --force "${GROVE_SDK_PIN}"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "Himax SDK checkout failed.\n"
                "  command: git checkout --detach --force ${GROVE_SDK_PIN}\n"
                "  workdir: ${GROVE_SDK_ROOT}")
        endif()
        _grove_sdk_head("${GROVE_SDK_ROOT}" _sdk_head)
        if(NOT _sdk_head STREQUAL GROVE_SDK_PIN)
            message(FATAL_ERROR
                "Himax SDK checkout verification failed.\n"
                "  HEAD:   ${_sdk_head}\n"
                "  pinned: ${GROVE_SDK_PIN}\n"
                "  url:    ${GROVE_SDK_URL}")
        endif()
    endif()
    _grove_sdk_assert_clean("${GROVE_SDK_ROOT}")
endif()

set(GROVE_SDK_APP_DIR "${GROVE_SDK_ROOT}/EPII_CM55M_APP_S")

# Sanity: the files this board actually compiles/links must exist.  A truncated
# or foreign tree fails HERE with a named cause, not later as a missing header.
foreach(_probe
        "EPII_CM55M_APP_S/device/WE2_core.c"
        "EPII_CM55M_APP_S/prebuilt_libs/gnu/libdriver.a"
        "we2_image_gen_local/we2_local_image_gen"
        "xmodem/xmodem_send.py")
    if(NOT EXISTS "${GROVE_SDK_ROOT}/${_probe}")
        message(FATAL_ERROR
            "Himax SDK tree at ${GROVE_SDK_ROOT} is missing ${_probe}.\n"
            "  The checkout is incomplete or not the expected repository.")
    endif()
endforeach()
