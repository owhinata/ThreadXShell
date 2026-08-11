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
#  its absence nor drag it in.
# ============================================================================
set(_submodule_sentinels
    "lib/stm32h7xx_hal_driver/Src/stm32h7xx_hal.c"
    "lib/cmsis_core/Include/core_cm7.h"
    "lib/cmsis_device_h7/Include/stm32h725xx.h"
    "lib/tinyusb/src/tusb.h"
    "lib/threadx/common/src/tx_thread_create.c"
    "lib/coremark/core_main.c"
    "lib/netxduo/common/src/nx_ip_create.c"
    "lib/flashdb/src/fdb_kvdb.c"
    "lib/filex/common/src/fx_media_open.c")
