# ============================================================================
#  Fetch one upstream source tree at a pinned commit (issue #136).
#
#      fetch_upstream(<out_var>
#          URL    <git remote>
#          COMMIT <40-hex commit>
#          PROBE  <file relative to the tree> ...
#          [SPARSE <non-cone sparse-checkout pattern> ...])
#
#  Sets <out_var> to the checkout, which is
#
#      ${CMAKE_SOURCE_DIR}/upstream/<repo>-<commit>
#
#  where <repo> is the last component of URL without ".git".  Used by the boards
#  for the trees they pin per board (the ST Cube set: HAL, CMSIS device, CMSIS
#  core); the upstreams every board shares at one version stay lib/ submodules.
#  Nothing below knows which board is asking.
#
#  [!] ONE PLACE, IN THE SOURCE TREE, NAMED BY THE COMMIT.  Every build directory
#  of this clone reads the same checkout, so a tree is fetched once and every
#  later configure -- any board, any build directory -- is offline.  The commit is
#  in the name, so two boards pinning the same commit share one directory, and a
#  pin bump fetches a NEW directory beside the old one instead of moving a
#  checkout under a build that still points at it.  upstream/ is git-ignored, like
#  tools/ and the Grove SDK checkout.
#
#  [!] FETCHED ONCE, THEN ONLY CHECKED -- NEVER REPAIRED.  A directory that exists
#  is validated and nothing else: it must be its own git checkout (an empty
#  directory inside this repository would otherwise let the SUPERPROJECT answer),
#  its HEAD must be the pin, `git status` must be empty (tracked edits, untracked
#  and ignored files all count -- the working tree is what gets compiled), and
#  every PROBE must exist.  Any deviation is FATAL with the remedy spelled out.
#  It is deliberately not "fetch again": a tree someone checked out to another
#  commit or edited by hand is a decision the build must not silently undo, and
#  the trees are read-only by project rule exactly like lib/.
#
#  [!] A DIRECTORY THAT EXISTS WAS COMPLETE.  The fetch happens in a private
#  staging directory (<dest>.partial) and is renamed into place only after it has
#  passed the same checks, so an interrupted fetch leaves no <dest> -- and the
#  next configure starts the staging over.  Deleting <dest> is therefore the one
#  way to ask for a fresh copy.
#
#  [!] CONCURRENT CONFIGURES SERIALISE ON <dest>.lock (file(LOCK), released when
#  the holder exits, so a crashed configure leaves no stale lock).  Two build
#  directories configured in parallel would otherwise both fetch into the same
#  staging directory.
#
#  The commit is fetched DIRECTLY (`git fetch <url> <sha>`), shallow and with
#  --filter=blob:none, so only the blobs of the checked-out (sparse) paths come
#  over the network.  A tag is only ever a comment next to the SHA: tags move.
#  The checkout's HEAD file is a configure dependency, so a checkout inside the
#  tree re-runs configure and with it the checks above.
# ============================================================================
include_guard(GLOBAL)

find_package(Git REQUIRED)

# Runs git in <dir> with the arguments after it; FATAL (naming the command) on
# failure.  GIT_TERMINAL_PROMPT=0: a mistyped GitHub URL asks for credentials
# instead of failing, which would hang a configure with nobody at the terminal.
function(_fetch_upstream_git dir)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env GIT_TERMINAL_PROMPT=0
                "${GIT_EXECUTABLE}" -C "${dir}" ${ARGN}
        RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_QUIET)
    if(NOT _rc EQUAL 0)
        string(REPLACE ";" " " _cmd "${ARGN}")
        message(FATAL_ERROR
            "fetch_upstream: git ${_cmd}\n"
            "  failed (${_rc}) in ${dir}\n${_err}"
            "  If the pinned commit no longer exists upstream, do NOT retarget\n"
            "  this at a branch or tag -- change the COMMIT in board.cmake as a\n"
            "  reviewed change.")
    endif()
endfunction()

# Sets <out_var> to "" when <dir> is a usable checkout of <commit> with every
# probe present, and otherwise to one sentence saying what is wrong with it.
function(_fetch_upstream_check dir commit probes out_var)
    # Its OWN checkout: asked inside an empty directory (or a directory whose
    # .git was deleted), git walks up and answers for this repository instead.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse --show-toplevel
        OUTPUT_VARIABLE _top OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc ERROR_QUIET)
    if(_rc EQUAL 0)
        file(REAL_PATH "${_top}" _top)
    endif()
    file(REAL_PATH "${dir}" _want)
    if(NOT _rc EQUAL 0 OR NOT _top STREQUAL _want)
        set(${out_var} "it is not a git checkout of its own" PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse --verify -q HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        set(${out_var} "its HEAD cannot be read" PARENT_SCOPE)
        return()
    endif()
    if(NOT _head STREQUAL commit)
        set(${out_var} "it is checked out at ${_head}, not at the pin" PARENT_SCOPE)
        return()
    endif()
    # HEAD proves the parent commit only; the files compiled are the working tree.
    # --ignored: an ignored file is still a file a header search can find.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" status --porcelain
                --untracked-files=all --ignored
        OUTPUT_VARIABLE _dirty RESULT_VARIABLE _rc ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        set(${out_var} "git status failed in it" PARENT_SCOPE)
        return()
    endif()
    if(NOT _dirty STREQUAL "")
        string(REPLACE "\n" "\n      " _dirty "${_dirty}")
        set(${out_var}
            "its working tree differs from the pinned commit:\n      ${_dirty}"
            PARENT_SCOPE)
        return()
    endif()
    foreach(_p IN LISTS probes)
        if(NOT EXISTS "${dir}/${_p}")
            set(${out_var} "it has no ${_p} (wrong repository, or a sparse pattern that excludes it)"
                PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(fetch_upstream out_var)
    cmake_parse_arguments(PARSE_ARGV 1 _fu "" "URL;COMMIT" "PROBE;SPARSE")
    if(_fu_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fetch_upstream: unexpected arguments: ${_fu_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT _fu_URL)
        message(FATAL_ERROR "fetch_upstream: URL is required")
    endif()
    # A full SHA, never a tag or an abbreviation: both can come to mean another commit.
    if(NOT _fu_COMMIT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "fetch_upstream: COMMIT '${_fu_COMMIT}' for ${_fu_URL} is not a commit SHA")
    endif()
    string(LENGTH "${_fu_COMMIT}" _len)
    if(NOT _len EQUAL 40)
        message(FATAL_ERROR
            "fetch_upstream: COMMIT '${_fu_COMMIT}' for ${_fu_URL} must be the full 40-digit SHA")
    endif()
    # Without a probe a truncated or foreign tree passes every other check.
    if(NOT _fu_PROBE)
        message(FATAL_ERROR "fetch_upstream: at least one PROBE is required (${_fu_URL})")
    endif()

    string(REGEX REPLACE "/+$" "" _repo "${_fu_URL}")
    string(REGEX REPLACE "^.*[/:]" "" _repo "${_repo}")
    string(REGEX REPLACE "\\.git$" "" _repo "${_repo}")
    if(NOT _repo MATCHES "^[A-Za-z0-9._-]+$")
        message(FATAL_ERROR "fetch_upstream: cannot derive a directory name from ${_fu_URL}")
    endif()

    set(_root "${CMAKE_SOURCE_DIR}/upstream")
    set(_dest "${_root}/${_repo}-${_fu_COMMIT}")
    set(_stage "${_dest}.partial")
    file(MAKE_DIRECTORY "${_root}")

    # Say so when someone else holds it, so a configure that sits there waiting
    # for a parallel fetch does not look like a hang.
    file(LOCK "${_dest}.lock" GUARD FUNCTION TIMEOUT 0 RESULT_VARIABLE _lrc)
    if(NOT _lrc STREQUAL "0")
        message(STATUS "fetch_upstream: waiting for another configure to finish with ${_dest}")
        file(LOCK "${_dest}.lock" GUARD FUNCTION TIMEOUT 1800 RESULT_VARIABLE _lrc)
        if(NOT _lrc STREQUAL "0")
            message(FATAL_ERROR
                "fetch_upstream: could not lock ${_dest}.lock: ${_lrc}")
        endif()
    endif()

    if(NOT EXISTS "${_dest}" AND NOT IS_SYMLINK "${_dest}")
        message(STATUS "fetch_upstream: fetching ${_repo} @ ${_fu_COMMIT} ...")
        file(REMOVE_RECURSE "${_stage}")
        file(MAKE_DIRECTORY "${_stage}")
        _fetch_upstream_git("${_stage}" init -q)
        _fetch_upstream_git("${_stage}" remote add origin "${_fu_URL}")
        _fetch_upstream_git("${_stage}" fetch -q --depth 1 --filter=blob:none
                            origin "${_fu_COMMIT}")
        if(_fu_SPARSE)
            _fetch_upstream_git("${_stage}" sparse-checkout set --no-cone ${_fu_SPARSE})
        endif()
        _fetch_upstream_git("${_stage}" checkout -q --detach "${_fu_COMMIT}")
        _fetch_upstream_check("${_stage}" "${_fu_COMMIT}" "${_fu_PROBE}" _why)
        if(NOT _why STREQUAL "")
            message(FATAL_ERROR
                "fetch_upstream: the fresh checkout of ${_fu_URL} @ ${_fu_COMMIT}\n"
                "  in ${_stage} failed its checks: ${_why}")
        endif()
        file(RENAME "${_stage}" "${_dest}")
    endif()

    _fetch_upstream_check("${_dest}" "${_fu_COMMIT}" "${_fu_PROBE}" _why)
    if(NOT _why STREQUAL "")
        message(FATAL_ERROR
            "fetch_upstream: ${_dest}\n"
            "  is not the pinned checkout of ${_fu_URL}:\n"
            "  ${_why}.\n"
            "  The build does not repair or re-fetch an existing tree (it is\n"
            "  read-only, like lib/).  Delete the directory to fetch it again; to\n"
            "  build another version, change COMMIT in board.cmake instead.")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_dest}" rev-parse --path-format=absolute
                --git-path HEAD
        OUTPUT_VARIABLE _head_file OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc ERROR_QUIET)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_head_file}")
        message(FATAL_ERROR "fetch_upstream: cannot locate the HEAD file of ${_dest}")
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
                 CMAKE_CONFIGURE_DEPENDS "${_head_file}")
    set(${out_var} "${_dest}" PARENT_SCOPE)
endfunction()
