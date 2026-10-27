# ============================================================================
#  Wio Lite AI -- the upstream submodules this board builds against.
#
#  Included by the top-level CMakeLists.txt BEFORE board.cmake, which derives
#  both the "is anything missing?" test and the explicit fetch pathspec from this
#  one list.  Sentinel FILES, not directories: an uninitialised submodule is an
#  empty directory that exists, so a directory test would never fire.
#
#  Per board rather than one shared list because the fetch is derived from it.
#  A single list covering every board would make an ordinary `-DBOARD=wio-lite-ai`
#  configure clone the F7-side mirrors as well -- lib/guix alone is ~1.5 GB -- for
#  a build that never compiles a line of them.
#
#  lib/mlperf-tiny is deliberately absent: it is a ~340 MB shallow mirror behind
#  CONFIG_MLPERF_TINY (OFF by default) whose own fetch-on-demand lives in
#  cmake/mlperf-tiny.cmake, so a default configure must neither be triggered by
#  its absence nor drag it in.  lib/tflite-micro is absent for the same reason:
#  only the tflm backend reads it, and cmake/tflm_submodule.cmake fetches it on
#  first use, so a CONFIG_NN_BACKEND=null configure stays offline.  The ST Cube
#  set (H7 HAL, CMSIS device, CMSIS core) is not a submodule at all: board.cmake
#  pins it per board and fetches it with cmake/fetch_upstream.cmake (issue #136).
# ============================================================================
set(_submodule_sentinels
    "lib/tinyusb/src/tusb.h"
    "lib/threadx/common/src/tx_thread_create.c"
    "lib/coremark/core_main.c"
    "lib/netxduo/common/src/nx_ip_create.c"
    "lib/flashdb/src/fdb_kvdb.c"
    "lib/filex/common/src/fx_media_open.c")
