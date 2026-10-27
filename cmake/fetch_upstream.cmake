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
#  for the trees they pin per board (the ST Cube set -- HAL and CMSIS device --
#  and a CMSIS core at the Cube set's version or a newer one); the upstreams every
#  board shares at one version stay lib/ submodules.
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
#  its HEAD must be the pin and DETACHED (on a branch, a commit moves the branch
#  ref and leaves the HEAD file -- the configure dependency below -- untouched),
#  `git status` must be empty (tracked edits, untracked and ignored files all
#  count -- the working tree is what gets compiled), no index entry may be marked
#  assume-unchanged (status trusts the mark and stops looking at the file), the
#  sparse patterns must be exactly SPARSE (none when SPARSE is not given) and the
#  skip-worktree marks exactly what those patterns exclude (a marked file is
#  invisible to status, so a mark inside the patterns hides a deleted file), and
#  every PROBE must exist.  Any deviation is FATAL with the remedy spelled out.
#  It is deliberately not "fetch again": a tree someone checked out to another
#  commit or edited by hand is a decision the build must not silently undo, and
#  the trees are read-only by project rule exactly like lib/.
#
#  [!] EVERY GIT CALL GOES THROUGH FETCH_UPSTREAM_GIT, which clears the variables
#  that point git at another repository (GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE,
#  GIT_OBJECT_DIRECTORY, GIT_ALTERNATE_OBJECT_DIRECTORIES, GIT_COMMON_DIR) and
#  turns replace refs off.  A configure run from a shell with GIT_DIR set (a git
#  hook, `git rebase -x`) would otherwise check -- and fetch into -- the
#  superproject, and `git replace` would let another commit answer for the pin.
#  It also clears the two variables that carry configuration in the environment
#  (issue #158): GIT_CONFIG_PARAMETERS, which `git -c <name>=<value>` exports to
#  every command it starts, and GIT_CONFIG_COUNT, which makes git read the
#  numbered GIT_CONFIG_KEY_<n> / GIT_CONFIG_VALUE_<n> pairs.  Inherited from a
#  parent `git -c url.<x>.insteadOf=... rebase -x`, they would retarget a fetch
#  at another remote or run hooks from another core.hooksPath.  The line is:
#  what git itself hands to its children is cleared; what a person sets on
#  purpose (GIT_CONFIG_GLOBAL, GIT_CONFIG_SYSTEM, GIT_TEMPLATE_DIR) and the
#  configuration FILES (system, global, the repository's own) are still read:
#  git-lfs installs its smudge filter in the system or global file, and the
#  model fetch depends on it.  Every git call in the build goes through this prefix:
#  cmake/tflm_submodule.cmake, cmake/tflm_pin_check.cmake,
#  cmake/fetch_model.cmake, the Grove SDK fetch (boards/grove-vision-ai-v2/
#  cmake/himax_sdk_fetch.cmake), the submodule bootstrap and `git describe` in
#  CMakeLists.txt, the boards' plugin build ids and the MLPerf Tiny submodule.
#  cmake/fixtures/run_git_env_tests.py runs the model and SDK fetches with each
#  of these variables set.
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
#  The checkout's HEAD file is a configure dependency: since HEAD must be detached,
#  every checkout or commit inside the tree rewrites that file and re-runs
#  configure, and with it the checks above.  A file edited without git is caught
#  by the next configure, not by a build.
# ============================================================================
include_guard(GLOBAL)

find_package(Git REQUIRED)

# The one way the build starts git (see the header for who uses it): a COMMAND
# prefix, so a pipeline can use it as well as a single call.  GIT_TERMINAL_PROMPT=0:
# a mistyped GitHub URL asks for credentials instead of failing, which would hang
# a configure with nobody at the terminal.
#
# GIT_CONFIG_PARAMETERS is git's internal channel for `git -c` (not listed in
# git(1); it is how a git process hands -c to the commands it runs).  The
# GIT_CONFIG_COUNT / GIT_CONFIG_KEY_<n> / GIT_CONFIG_VALUE_<n> set is documented
# in git-config(1), ENVIRONMENT: "If GIT_CONFIG_COUNT is set to a positive number,
# all environment pairs ... up to that number will be added", and an empty or
# absent count processes no pairs -- so clearing the count is enough, and the
# numbered keys, which cannot be listed in advance, are never read.
set(FETCH_UPSTREAM_GIT
    "${CMAKE_COMMAND}" -E env
    --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
    --unset=GIT_OBJECT_DIRECTORY --unset=GIT_ALTERNATE_OBJECT_DIRECTORIES
    --unset=GIT_COMMON_DIR
    --unset=GIT_CONFIG_PARAMETERS --unset=GIT_CONFIG_COUNT
    GIT_TERMINAL_PROMPT=0 GIT_NO_REPLACE_OBJECTS=1
    "${GIT_EXECUTABLE}")

# Runs git in <dir> with the arguments after it; FATAL (naming the command) on
# failure.
function(_fetch_upstream_git dir)
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${dir}" ${ARGN}
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

# Runs git in <dir>; sets <rc_var> to its exit code and <out_var> to its stdout
# (stderr dropped).  For the checks, which turn a failure into a reason.
function(_fetch_upstream_ask dir rc_var out_var)
    execute_process(
        COMMAND ${FETCH_UPSTREAM_GIT} -C "${dir}" ${ARGN}
        OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_QUIET)
    set(${rc_var} "${_rc}" PARENT_SCOPE)
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to "" when <dir> is a usable checkout of <commit> with every
# probe present and exactly the sparse patterns <sparse> (a list; empty means not
# sparse), and otherwise to one sentence saying what is wrong with it.
function(_fetch_upstream_check dir commit probes sparse out_var)
    # Its OWN checkout: asked inside an empty directory (or a directory whose
    # .git was deleted), git walks up and answers for this repository instead.
    _fetch_upstream_ask("${dir}" _rc _top rev-parse --show-toplevel)
    string(STRIP "${_top}" _top)
    if(_rc EQUAL 0)
        file(REAL_PATH "${_top}" _top)
    endif()
    file(REAL_PATH "${dir}" _want)
    if(NOT _rc EQUAL 0 OR NOT _top STREQUAL _want)
        set(${out_var} "it is not a git checkout of its own" PARENT_SCOPE)
        return()
    endif()
    _fetch_upstream_ask("${dir}" _rc _head rev-parse --verify -q HEAD)
    string(STRIP "${_head}" _head)
    if(NOT _rc EQUAL 0)
        set(${out_var} "its HEAD cannot be read" PARENT_SCOPE)
        return()
    endif()
    if(NOT _head STREQUAL commit)
        set(${out_var} "it is checked out at ${_head}, not at the pin" PARENT_SCOPE)
        return()
    endif()
    # Detached: exit 1 is "HEAD is not a symbolic ref"; 0 is a branch.
    _fetch_upstream_ask("${dir}" _rc _ref symbolic-ref -q HEAD)
    string(STRIP "${_ref}" _ref)
    if(_rc EQUAL 0)
        set(${out_var} "its HEAD is the branch ${_ref}, not the detached pin" PARENT_SCOPE)
        return()
    elseif(NOT _rc EQUAL 1)
        set(${out_var} "git symbolic-ref HEAD failed in it" PARENT_SCOPE)
        return()
    endif()
    # HEAD proves the parent commit only; the files compiled are the working tree.
    # --ignored: an ignored file is still a file a header search can find.
    _fetch_upstream_ask("${dir}" _rc _dirty
        status --porcelain --untracked-files=all --ignored)
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
    _fetch_upstream_ask("${dir}" _rc _on config --bool --get core.sparseCheckout)
    string(STRIP "${_on}" _on)
    if(sparse STREQUAL "" AND _on STREQUAL "true")
        set(${out_var} "it is a sparse checkout, but no SPARSE patterns were asked for"
            PARENT_SCOPE)
        return()
    endif()
    # status above is only as good as the index marks it trusts.  Every entry must
    # be "H " (checked out) or "S " (skip-worktree); a lower-case tag is
    # assume-unchanged, anything else is an unmerged or otherwise odd entry.
    _fetch_upstream_ask("${dir}" _rc _tags ls-files -v)
    if(NOT _rc EQUAL 0)
        set(${out_var} "git ls-files failed in it" PARENT_SCOPE)
        return()
    endif()
    set(_tags "\n${_tags}")
    if(sparse STREQUAL "")
        set(_odd "\n([^H]|H[^ ])")
    else()
        set(_odd "\n([^HS]|[HS][^ ])")
    endif()
    if(_tags MATCHES "${_odd}([^\n]*)")
        string(STRIP "${CMAKE_MATCH_0}" _line)
        set(${out_var}
            "its index marks '${_line}' (assume-unchanged or skip-worktree hides a file from git status)"
            PARENT_SCOPE)
        return()
    endif()
    if(NOT sparse STREQUAL "")
        # The patterns, exactly and in order: a narrowed set would still pass
        # status and could still hold every PROBE.
        _fetch_upstream_ask("${dir}" _rc _have sparse-checkout list)
        string(REPLACE ";" "\n" _asked "${sparse}")
        if(NOT _on STREQUAL "true" OR NOT _rc EQUAL 0 OR NOT _have STREQUAL "${_asked}\n")
            string(STRIP "${_have}" _have)
            string(REPLACE "\n" " " _have "${_have}")
            set(${out_var}
                "its sparse patterns are '${_have}', not the SPARSE '${sparse}'"
                PARENT_SCOPE)
            return()
        endif()
        # And the marks match the patterns: the paths the patterns keep are
        # exactly the "H " entries (so every "S " entry is one they exclude).
        # git itself answers which paths the patterns keep.
        execute_process(
            COMMAND ${FETCH_UPSTREAM_GIT} -C "${dir}" ls-files
            COMMAND ${FETCH_UPSTREAM_GIT} -C "${dir}" sparse-checkout check-rules
            OUTPUT_VARIABLE _kept RESULTS_VARIABLE _rcs ERROR_QUIET)
        string(REGEX REPLACE "\nS [^\n]*" "" _marked "${_tags}")
        string(REPLACE "\nH " "\n" _marked "${_marked}")
        if(NOT _rcs STREQUAL "0;0")
            set(${out_var}
                "git sparse-checkout check-rules failed in it (git 2.42 or later is needed)"
                PARENT_SCOPE)
            return()
        endif()
        if(NOT _marked STREQUAL "\n${_kept}")
            set(${out_var}
                "its skip-worktree marks do not match its sparse patterns"
                PARENT_SCOPE)
            return()
        endif()
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
        # The staging directory is ours to delete; a symlink there points at
        # something that is not, and is left for a person to look at.
        if(IS_SYMLINK "${_stage}")
            message(FATAL_ERROR
                "fetch_upstream: ${_stage} is a symlink; it is not removed (nor\n"
                "  what it points at).  Delete it by hand to fetch ${_repo}.")
        endif()
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
        _fetch_upstream_check("${_stage}" "${_fu_COMMIT}" "${_fu_PROBE}" "${_fu_SPARSE}" _why)
        if(NOT _why STREQUAL "")
            message(FATAL_ERROR
                "fetch_upstream: the fresh checkout of ${_fu_URL} @ ${_fu_COMMIT}\n"
                "  in ${_stage} failed its checks: ${_why}")
        endif()
        file(RENAME "${_stage}" "${_dest}")
    endif()

    _fetch_upstream_check("${_dest}" "${_fu_COMMIT}" "${_fu_PROBE}" "${_fu_SPARSE}" _why)
    if(NOT _why STREQUAL "")
        message(FATAL_ERROR
            "fetch_upstream: ${_dest}\n"
            "  is not the pinned checkout of ${_fu_URL}:\n"
            "  ${_why}.\n"
            "  The build does not repair or re-fetch an existing tree (it is\n"
            "  read-only, like lib/).  Delete the directory to fetch it again; to\n"
            "  build another version, change COMMIT in board.cmake instead.")
    endif()

    _fetch_upstream_ask("${_dest}" _rc _head_file
        rev-parse --path-format=absolute --git-path HEAD)
    string(STRIP "${_head_file}" _head_file)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_head_file}")
        message(FATAL_ERROR "fetch_upstream: cannot locate the HEAD file of ${_dest}")
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
                 CMAKE_CONFIGURE_DEPENDS "${_head_file}")
    set(${out_var} "${_dest}" PARENT_SCOPE)
endfunction()
