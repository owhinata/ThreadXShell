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

# The functions, and why every git call in them goes through FETCH_UPSTREAM_GIT
# (issue #158), are in himax_sdk_fetch.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/himax_sdk_fetch.cmake")

if(GROVE_SDK_DIR)
    grove_sdk_check_dir("${GROVE_SDK_DIR}" "${GROVE_SDK_PIN}")
    set(GROVE_SDK_ROOT "${GROVE_SDK_DIR}")
else()
    set(GROVE_SDK_ROOT "${BOARD_DIR}/sdk")
    grove_sdk_fetch("${GROVE_SDK_ROOT}" "${GROVE_SDK_URL}" "${GROVE_SDK_PIN}")
endif()
grove_sdk_probe("${GROVE_SDK_ROOT}")

set(GROVE_SDK_APP_DIR "${GROVE_SDK_ROOT}/EPII_CM55M_APP_S")
