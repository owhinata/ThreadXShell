# ============================================================================
#  Grove Vision AI V2 -- the upstream submodules this board builds against.
#
#  Included by the top-level CMakeLists.txt BEFORE board.cmake; see the header of
#  boards/wio-lite-ai/submodules.cmake for why the list is per board and why the
#  entries are sentinel FILES rather than directories.
#
#  This list is deliberately short.  The board's vendor layer -- the Himax
#  WiseEye2 SDK (HAL-equivalent drivers, CMSIS with core_cm55.h, startup,
#  image/flash tooling) -- is NOT a lib/ submodule: it is fetched at configure
#  time by boards/grove-vision-ai-v2/cmake/himax_sdk.cmake into
#  boards/grove-vision-ai-v2/sdk/ (git-ignored, pinned by commit, read-only).
#  That was an explicit project decision (2026-08-13): the SDK is a 480 MB tree
#  with prebuilt driver archives, and mirroring it as a submodule would tax
#  every clone for sources only this board opens.
#
#  So the mirrors this board compiles from lib/ are Eclipse ThreadX itself
#  (ports/cortex_m55/gnu + common core + the execution profile kit) and EEMBC
#  CoreMark (the `coremark` command, issue #25).
# ============================================================================
set(_submodule_sentinels
    "lib/threadx/common/src/tx_thread_create.c"
    "lib/coremark/core_main.c")
