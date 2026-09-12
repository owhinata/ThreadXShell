# ============================================================================
#  Fetch one pinned model into the build tree (issue #107).
#
#  Run as `cmake -P`, from a build-time custom command -- NOT at configure time.
#  A model is an optional input: a tree with no access to the model host must
#  still configure and still build the firmware, which is the shape issue #94
#  recorded after a gate blocked the plain flash it was not protecting.
#
#  Arguments (all -D):
#     URL      git remote to fetch from
#     COMMIT   exact commit, fetched directly -- never a branch tip
#     PATH_IN  path of the file inside that repository
#     SHA256   the CONTENT hash, which is the authority here
#     OUT      where to publish the verified file
#     WORK     a private staging directory this invocation owns
#
#  [!] THE CONTENT HASH IS NOT BELT AND BRACES.  The model is stored with Git
#  LFS, and the failure mode is silent: with git-lfs absent the checkout leaves a
#  131-byte pointer file, exit code 0, no diagnostic.  The pointer is detected
#  below for the sake of the MESSAGE -- "install git-lfs" is a better sentence
#  than "hash mismatch" when that is what happened -- but the hash is what
#  decides, because a pointer is only one of the ways the wrong bytes arrive.
#
#  [!] AND NOTHING IS PUBLISHED UNTIL IT HAS PASSED.  Staging is private to this
#  invocation and the verified file is renamed into place, so an interrupted
#  fetch cannot leave something at OUT that looks finished.  The clone directory
#  is never evidence of completion.
# ============================================================================

foreach(_v URL COMMIT PATH_IN SHA256 OUT WORK)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "fetch_model: -D${_v}= is required")
    endif()
endforeach()

find_package(Git REQUIRED)

# Private, and replaced rather than resumed: a half-finished clone from an
# interrupted run must not be mistaken for a usable one.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(_run_git)
    execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
                    WORKING_DIRECTORY "${WORK}"
                    RESULT_VARIABLE _rc
                    OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "fetch_model: git ${ARGN}\n  failed (${_rc}) in ${WORK}\n${_err}\n"
            "  If the pinned commit no longer exists (garbage-collected, or the\n"
            "  repository moved), do NOT retarget this at a branch tip -- that\n"
            "  would silently change which model this board ships.  Either pass\n"
            "  -DGROVE_ASSET_<NAME>_FILE=<path to a local copy>, or update URL,\n"
            "  COMMIT and SHA256 together as a reviewed change.")
    endif()
endfunction()

_run_git(init -q)
_run_git(remote add origin "${URL}")
_run_git(config core.sparseCheckout true)
_run_git(fetch -q --depth 1 --filter=blob:none origin "${COMMIT}")
_run_git(sparse-checkout set --no-cone "/${PATH_IN}")
_run_git(checkout -q FETCH_HEAD)

set(_got "${WORK}/${PATH_IN}")
if(NOT EXISTS "${_got}")
    message(FATAL_ERROR
        "fetch_model: ${PATH_IN} is not in ${URL} at ${COMMIT}")
endif()

# [!] A COMPLETE POINTER, not a first line or a substring.  Matching loosely
# would label an ordinary corrupt download "install git-lfs" and send the next
# person after the wrong thing.  A file that is not a pointer falls through to
# the hash, which is where a plain mismatch belongs.
file(SIZE "${_got}" _got_size)
if(_got_size LESS 1024)
    file(READ "${_got}" _head)
    # The WHOLE file must be the canonical three lines and nothing else.  A
    # looser match would label an ordinary corrupt download "install git-lfs" and
    # send the next person after the wrong thing; anything that is not exactly a
    # pointer falls through to the hash, where a plain mismatch belongs.
    if(_head MATCHES "^version https://git-lfs\\.github\\.com/spec/v1\noid sha256:[0-9a-f]+\nsize [0-9]+\n$")
        message(FATAL_ERROR
            "fetch_model: ${PATH_IN} came back as a ${_got_size} B Git LFS\n"
            "  POINTER, not the model.  git-lfs is not installed, or the smudge\n"
            "  filter was skipped.  Install git-lfs (and `git lfs install`), or\n"
            "  pass -DGROVE_ASSET_<NAME>_FILE=<path to a local copy>.\n"
            "  The pointer names the content this build expects:\n${_head}")
    endif()
endif()

file(SHA256 "${_got}" _got_sha)
if(NOT _got_sha STREQUAL SHA256)
    message(FATAL_ERROR
        "fetch_model: ${PATH_IN} is not the pinned content.\n"
        "  want: ${SHA256}\n  got:  ${_got_sha}  (${_got_size} B)\n"
        "  The pin names the exact bytes this board was verified against; a\n"
        "  different model is a different model even if it is newer.")
endif()

# Verified, so publish -- on the same filesystem, so the rename is atomic and
# OUT never exists holding unverified bytes.
get_filename_component(_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
file(RENAME "${_got}" "${OUT}")
file(REMOVE_RECURSE "${WORK}")
message(STATUS "fetch_model: ${OUT} (${_got_size} B, sha256 verified)")
