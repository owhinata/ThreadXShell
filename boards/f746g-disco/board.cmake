# ============================================================================
#  STM32F746G-DISCO (STM32F746NGH6) board definition.
#
#  Included by the top-level CMakeLists.txt with BOARD=f746g-disco.  It owns
#  everything board-specific: the MCU flags, the HAL/CMSIS pins, the linker
#  script, the peripheral options, the firmware and the post-build gates.
#
#  Firmware:
#    shell -- ThreadX + the interactive CLI console over the USART1 VCP
#             (PA9/PB7, 115200 8N1 -> /dev/ttyACM0), plus a second instance over
#             telnet once `net` is up.  Benchmarks (CoreMark, membench) are shell
#             commands, not separate images.
#
#  Flash over ST-Link:  cmake --build build/f746g-disco --target flash
#
#  Unlike the wio-lite-ai port this board configures its own clock tree (216 MHz
#  from the 25 MHz HSE) in src/bsp.c, runs from the start of internal flash, and
#  has no bootloader to inherit anything from -- so there is no equivalent of
#  that board's "do not touch RCC" rule here.
# ============================================================================

# The name the `system` command prints; substituted into cmake/cli_version.h.in
# by the top-level CMakeLists.txt AFTER this file has been included.  The donor
# repository hard-coded this same string into its template.
set(BOARD_FW_NAME "ThreadX Shell")

# --- No LTO on this board ---------------------------------------------------
# Not a preference -- a correctness precondition.  The placement rules for this
# firmware are enforced by ASSERTs inside ldscript/STM32F746NGHx_FLASH.ld, and
# every one of them is written against a linker symbol or an input-section
# pattern (`.sdram.cam`, `_ssdram_eth`, ...) that only survives because
# -fdata-sections gives each object its own section under its own name.  LTO is
# free to rename and merge exactly those, at which point the selectors stop
# matching, the sections link EMPTY, and the ASSERTs pass while asserting
# nothing.  The camera arena silently sharing an FMC bank with the LTDC scan-out
# surface is not a build error; it is a frame-rate mystery months later.
#
# So refuse to configure rather than produce that image.  Both delivery routes
# are covered, and both in their per-config forms as well: CMake appends
# CMAKE_C_FLAGS_<CONFIG> AFTER CMAKE_C_FLAGS, and
# CMAKE_INTERPROCEDURAL_OPTIMIZATION_<CONFIG> overrides the generic variable, so
# checking only the generic names would miss `-DCMAKE_BUILD_TYPE=Release
# -DCMAKE_C_FLAGS_RELEASE=-flto`.
#
# (boards/wio-lite-ai builds WITH LTO and guards the same class of hazard with
# post-link residency checkers instead.  This board has a checker too -- see
# cmake/check_f746_layout.py at the bottom -- but its linker script carries far
# more of the invariant, so the cheaper answer is to keep the precondition.)
#
# One observed wrinkle, so the next person is not sent looking for a compiler
# bug: a bare `-DCMAKE_C_FLAGS=-flto` never reaches this check.  project() runs
# first, and CMake's compiler-version probe compiles a file that then contains
# only LTO IR, so the version comes back mangled ("GNU 15.21") and configure dies
# in CMakeCommonCompilerMacros.cmake with a message about
# CMAKE_C_STANDARD_COMPUTED_DEFAULT.  Configure still refuses, which is the
# outcome that matters, and the moment the flags let the probe succeed
# (-ffat-lto-objects, or a CMake that probes differently) this check is reached
# and reports the real reason.  Verified for both shapes.
set(_f746_lto_configs "")
if(CMAKE_CONFIGURATION_TYPES)
    set(_f746_lto_configs ${CMAKE_CONFIGURATION_TYPES})
elseif(CMAKE_BUILD_TYPE)
    set(_f746_lto_configs "${CMAKE_BUILD_TYPE}")
endif()
set(_f746_lto_vars CMAKE_INTERPROCEDURAL_OPTIMIZATION)
set(_f746_flag_vars CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
foreach(_cfg IN LISTS _f746_lto_configs)
    string(TOUPPER "${_cfg}" _cfg_up)
    list(APPEND _f746_lto_vars "CMAKE_INTERPROCEDURAL_OPTIMIZATION_${_cfg_up}")
    list(APPEND _f746_flag_vars "CMAKE_C_FLAGS_${_cfg_up}" "CMAKE_CXX_FLAGS_${_cfg_up}")
endforeach()
foreach(_v IN LISTS _f746_lto_vars)
    if(${_v})
        message(FATAL_ERROR
            "${_v} is ON, but this board must not be built with LTO.\n"
            "  The linker-script ASSERTs in ldscript/STM32F746NGHx_FLASH.ld are\n"
            "  written against symbols and input-section names that LTO renames;\n"
            "  under LTO they would pass while checking nothing.  Re-configure\n"
            "  with -D${_v}=OFF.")
    endif()
endforeach()
foreach(_v IN LISTS _f746_flag_vars)
    if("${${_v}}" MATCHES "(^| )-flto")
        message(FATAL_ERROR
            "${_v} contains -flto, but this board must not be built with LTO.\n"
            "  See the comment on this check in boards/f746g-disco/board.cmake:\n"
            "  the linker-script ASSERTs stop protecting anything under LTO.\n"
            "  ${_v} = ${${_v}}")
    endif()
endforeach()

# --- Target / common build options -----------------------------------------
# Cortex-M7 with the SINGLE-precision FPU: the STM32F746 has fpv5-sp-d16, so
# double-precision arithmetic (CoreMark's %f score line) goes through the
# software routines -- see the __aeabi_d* check in cmake/check_f746_layout.py.
set(MCU_OPTS
    -mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard)

# The ST Cube set -- HAL and CMSIS device -- is pinned per board and fetched by
# cmake/fetch_upstream.cmake into upstream/<repo>-<commit> (issue #136), not taken
# from lib/: ST pairs these two per Cube package, so each board names its own
# commits.  CMSIS core is fetched the same way, at the Cube set's version or a
# newer one (the board README says which and why).  The SHA is the pin; the tag
# beside it is only a comment.
# The trees are read-only, like lib/.
include("${CMAKE_SOURCE_DIR}/cmake/fetch_upstream.cmake")
fetch_upstream(HAL_DIR                          # v1.3.3-11-gdaf38ec (no tag)
    URL    "https://github.com/STMicroelectronics/stm32f7xx_hal_driver"
    COMMIT "daf38ecc98298d8434a5fab8f5da35e34e4c1695"
    SPARSE "/Inc/" "/Src/" "/LICENSE.md"
    PROBE  "Src/stm32f7xx_hal.c" "Inc/stm32f7xx_hal_conf_template.h"
           "LICENSE.md")
fetch_upstream(CMSIS_DEV                        # v1.2.10
    URL    "https://github.com/STMicroelectronics/cmsis_device_f7"
    COMMIT "2352e888e821aa0f4fe549bd5ea81d29c67a3222"
    SPARSE "/Include/" "/Source/Templates/" "/LICENSE.md"
    PROBE  "Include/stm32f746xx.h" "Source/Templates/system_stm32f7xx.c"
           "Source/Templates/gcc/startup_stm32f746xx.s" "LICENSE.md")
fetch_upstream(CMSIS_CORE                       # v5.6.0_cm7
    URL    "https://github.com/STMicroelectronics/cmsis-core"
    COMMIT "04fe7bb2025811f10848ce14be810dde981934cf"
    SPARSE "/Include/" "/LICENSE.txt"
    PROBE  "Include/core_cm7.h" "LICENSE.txt")
set(LDSCRIPT   "${BOARD_DIR}/ldscript/STM32F746NGHx_FLASH.ld")

# stm32f7xx_hal_conf.h is generated from the upstream template (its default
# HSE_VALUE of 25 MHz already matches the board crystal).
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
configure_file("${HAL_DIR}/Inc/stm32f7xx_hal_conf_template.h"
               "${GEN_DIR}/stm32f7xx_hal_conf.h" COPYONLY)

# Compile-time gate for the IWDG watchdog + `wdt` command.  ON by default; build
# with -DBSP_ENABLE_IWDG=OFF to drop the IWDG/LSI/petter entirely.  Defined on
# bsp_iface (below) -- NOT on the shell target -- because src/bsp.c is compiled
# in the `common` object library, which only sees bsp_iface's defines.
option(BSP_ENABLE_IWDG "Enable the IWDG watchdog and wdt shell command" ON)

# Usage requirements shared by every target (includes, defs, MCU flags, link).
add_library(bsp_iface INTERFACE)
target_include_directories(bsp_iface INTERFACE
    "${BOARD_DIR}/include"
    "${CMAKE_SOURCE_DIR}/svc"   # shared freestanding services: fmt.h / ymodem.h
    "${CMAKE_SOURCE_DIR}/svc/nn"
    "${CMAKE_SOURCE_DIR}/svc/plugin"
    "${CMAKE_SOURCE_DIR}/svc/camera"   # frame.h
    "${BOARD_DIR}/svc"          # this board's services: log.h / timebase.h
    "${GEN_DIR}"
    "${HAL_DIR}/Inc"
    "${CMSIS_DEV}/Include"
    "${CMSIS_CORE}/Include")
target_compile_definitions(bsp_iface INTERFACE USE_HAL_DRIVER STM32F746xx
    BSP_ENABLE_IWDG=$<BOOL:${BSP_ENABLE_IWDG}>    # reaches common + shell
    # Two shared-core knobs this board pins.  They are on bsp_iface -- i.e. on
    # EVERY translation unit -- because both are consumed inside _Static_assert
    # conditions in shell/include/cli_config.h, and a define that reached only
    # some TUs would make the same header assert different bounds in different
    # objects.
    #
    # CLI_CPU_CYCLES_PER_US: what the board's udelay() counts per microsecond.
    #   Here that is TIM2, NOT the DWT cycle counter: svc/timebase.c runs TIM2
    #   free at 2*PCLK1 = 108 MHz (APB1 /4 with TIMPRE=0, RM0385) and multiplies
    #   by 108.  The core clock is 216 MHz; passing that would make `usleep` wait
    #   half as long as asked and set the overflow bound to the wrong value.  The
    #   donor had no knob here and hardcoded the same 108 into its assert.
    #
    # CLI_INSTANCE_TIME_SLICE=0: keep the donor's TX_NO_TIME_SLICE scheduling.
    #   The shared core defaults to a 10-tick slice so two consoles round-robin,
    #   but this port also inherits command implementations that are not
    #   re-entrant across instances -- coremark / membench / nn_run share static
    #   state and the DWT cycle counter (issue #4).  0 maps to TX_NO_TIME_SLICE
    #   in cli_core.c, keeping the concurrency window exactly as narrow as it was
    #   on the donor.  Turning slicing on belongs with the re-entrancy work, not
    #   with the port.
    CLI_CPU_CYCLES_PER_US=108
    CLI_INSTANCE_TIME_SLICE=0)
target_compile_options(bsp_iface INTERFACE
    ${MCU_OPTS} -Wall -fdata-sections -ffunction-sections -g -gdwarf-2)
target_link_options(bsp_iface INTERFACE
    ${MCU_OPTS} -specs=nano.specs -specs=nosys.specs "-T${LDSCRIPT}"
    -Wl,--gc-sections -Wl,--print-memory-usage)

# --- Common object library (HAL + CMSIS + board bring-up) ------------------
# Compile every HAL driver source; each file self-guards on its
# HAL_<module>_MODULE_ENABLED define, so unused ones become empty objects.
file(GLOB HAL_SOURCES "${HAL_DIR}/Src/*.c")
list(FILTER HAL_SOURCES EXCLUDE REGEX "_template\\.c$")

add_library(common OBJECT
    ${HAL_SOURCES}
    "${CMSIS_DEV}/Source/Templates/system_stm32f7xx.c"
    "${CMSIS_DEV}/Source/Templates/gcc/startup_stm32f746xx.s"
    "${BOARD_DIR}/src/bsp.c")
target_link_libraries(common PUBLIC bsp_iface)
target_compile_options(common PRIVATE -O2)

# --- Freestanding service layer --------------------------------------------
# fmt (clean-room printf), ymodem and frame_pipeline are board-independent and
# come from the shared svc/ tree; log (DTCM RAM ring) and timebase (TIM2 free-run
# + udelay) are this board's and live under boards/f746g-disco/svc.  Both halves
# depend on HAL/CMSIS only.  They sit below port/ so drivers take log/udelay from
# here instead of reaching up into shell/ or src/ (one-way layering:
# HAL/CMSIS/ThreadX <- svc <- port <- shell/src).  Linked into the shell exe;
# bsp.c (in `common`) resolves timebase_init() at the final link.
add_library(svc_obj OBJECT
    "${CMAKE_SOURCE_DIR}/svc/fmt.c"
    "${BOARD_DIR}/svc/log.c"
    "${BOARD_DIR}/svc/timebase.c"
    "${CMAKE_SOURCE_DIR}/svc/ymodem.c"
    "${CMAKE_SOURCE_DIR}/svc/camera/frame_pipeline.c")
target_link_libraries(svc_obj PUBLIC bsp_iface)
target_compile_options(svc_obj PRIVATE -O2)

# --- Helper: post-build artifacts + flash target ---------------------------
# Single firmware, so the flash target is simply `flash` (not flash-<tgt>).
function(firmware_finalize tgt)
    target_link_options(${tgt} PRIVATE -Wl,-Map=${tgt}.map,--cref)
    set_target_properties(${tgt} PROPERTIES LINK_DEPENDS "${LDSCRIPT}")
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary -S $<TARGET_FILE:${tgt}> ${tgt}.bin
        COMMAND "${CMAKE_OBJCOPY}" -O ihex   $<TARGET_FILE:${tgt}> ${tgt}.hex
        COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:${tgt}>
        BYPRODUCTS ${tgt}.bin ${tgt}.hex
        COMMENT "objcopy -> ${tgt}.bin / ${tgt}.hex")
    # --connect-under-reset: hold the MCU in reset while attaching.  Required
    # since TX_ENABLE_WFI -- the running firmware sleeps in WFI when idle, and
    # the old onboard ST-Link (V2J28x) cannot connect to a sleeping core without
    # reset (st-flash otherwise fails with "Can not connect to target / unknown
    # chip id").  Harmless for non-sleeping firmware.
    add_custom_target(flash
        COMMAND st-flash --connect-under-reset --reset write ${tgt}.bin 0x08000000
        DEPENDS ${tgt}
        USES_TERMINAL
        COMMENT "Flashing ${tgt} over ST-Link")
endfunction()

# --- CoreMark object library (run as the shell `coremark` command) ----------
# Built once at -O3 and linked into the shell firmware below; cmd_coremark.c
# calls coremark_main().  core_main.c is compiled with -Dmain=coremark_main so
# its main() does not clash with the firmware main() in src/main.c.  MEM_METHOD=
# MEM_STATIC keeps the TOTAL_DATA_SIZE block in .bss (off the shell thread
# stack) as a static array, so no malloc is pulled in.
set(CMK_DIR "${CMAKE_SOURCE_DIR}/lib/coremark")
add_library(coremark_obj OBJECT
    "${CMK_DIR}/core_list_join.c"
    "${CMK_DIR}/core_main.c"
    "${CMK_DIR}/core_matrix.c"
    "${CMK_DIR}/core_state.c"
    "${CMK_DIR}/core_util.c"
    "${BOARD_DIR}/port/coremark/core_portme.c")
target_link_libraries(coremark_obj PUBLIC bsp_iface)   # HAL_GetTick for timing
target_include_directories(coremark_obj PRIVATE
    "${CMK_DIR}" "${BOARD_DIR}/port/coremark")
target_compile_definitions(coremark_obj PRIVATE
    ITERATIONS=0 PERFORMANCE_RUN=1 TOTAL_DATA_SIZE=2000
    MEM_METHOD=MEM_STATIC MEM_LOCATION="STATIC")
target_compile_options(coremark_obj PRIVATE -O3 -funroll-loops)
# Rename CoreMark's main() so it does not collide with the firmware main().
set_source_files_properties("${CMK_DIR}/core_main.c" PROPERTIES
    COMPILE_DEFINITIONS "main=coremark_main")

# --- Eclipse ThreadX core + Cortex-M7/GNU port -----------------------------
# Shared by the shell firmware below.  ThreadX supplies its own PendSV_Handler
# (there is no SVC user), so a stm32f7xx_it.c is not part of the build; the
# startup file's weak Default_Handler catches every other vector.
set(TX_DIR  "${CMAKE_SOURCE_DIR}/lib/threadx")
set(TX_PORT "${TX_DIR}/ports/cortex_m7/gnu")
file(GLOB TX_CORE "${TX_DIR}/common/src/*.c")
list(FILTER TX_CORE EXCLUDE REGEX "tx_misra\\.c$")
file(GLOB TX_ASM "${TX_PORT}/src/*.S")
list(FILTER TX_ASM EXCLUDE REGEX "tx_misra\\.S$")

# --- FileX + LevelX (QSPI NOR filesystem) -----------------------------------
# Same pattern as ThreadX: GLOB the cores, configure via port/<lib>/<x>_user.h
# headers (FX/LX_INCLUDE_USER_DEFINE_FILE below).  NAND support and the RAM
# simulator/demo drivers are excluded -- this firmware is NOR-only (lx_nor_*)
# and ships its own driver glue in port/levelx + port/filex.  gc-sections
# drops the unused FileX feature paths from the final image.
set(FX_DIR "${CMAKE_SOURCE_DIR}/lib/filex")
set(LX_DIR "${CMAKE_SOURCE_DIR}/lib/levelx")
file(GLOB FX_CORE "${FX_DIR}/common/src/*.c")
list(FILTER FX_CORE EXCLUDE REGEX "simulat")            # RAM-disk demo drivers
file(GLOB LX_CORE "${LX_DIR}/common/src/*.c")
list(FILTER LX_CORE EXCLUDE REGEX "lx_nand|simulat")    # NOR only

# --- Eclipse ThreadX GUIX (display GUI framework) ---------------------------
# Same pattern as ThreadX/FileX: GLOB the core, configure via port/guix/gx_user.h
# (GX_INCLUDE_USER_DEFINE_FILE below).  The Cortex-M7/GNU port is header-only
# (ports/cortex_m7/gnu/inc/gx_port.h) -- GUIX binds to ThreadX, so there is no
# port asm/src to compile.  binres (binary-resource loaders) are excluded: this
# firmware links its widgets/fonts statically, and gc-sections drops the rest of
# the unused GUIX feature paths from the image.
set(GX_DIR "${CMAKE_SOURCE_DIR}/lib/guix")
file(GLOB GX_CORE "${GX_DIR}/common/src/*.c")
list(FILTER GX_CORE EXCLUDE REGEX "gx_binres")          # no binary resource loader

# --- NetX Duo (IPv4 TCP/IP) -------------------------------------------------
# eclipse-threadx/netxduo (MIT), same GLOB pattern as ThreadX/FileX/GUIX.  The
# IPv6 sources self-guard on FEATURE_NX_IPV6 (neutralised by NX_DISABLE_IPV6 in
# port/netxduo/nx_user.h) -> empty objects that --gc-sections drops.  Only the
# RAM/simulator demo driver is excluded; this firmware ships its own clean-room
# port/netxduo ETH driver glue over the STM32 ETH MAC.
set(NX_DIR "${CMAKE_SOURCE_DIR}/lib/netxduo")
file(GLOB NX_CORE "${NX_DIR}/common/src/*.c")
list(FILTER NX_CORE EXCLUDE REGEX "nx_ram_network_driver")

# --- Shell core object library ----------------------------------------------
# The board-independent core comes from the shared shell/ tree; the UART backend
# reaches for the HAL and USART1, so it lives under this board's backend/.
#
# Collected into an OBJECT library named shell_obj (NOT `shell` -- the executable
# below takes that name).  Because cli_instance.h pulls in tx_api.h, the objlib
# MUST be compiled with the same TX_INCLUDE_USER_DEFINE_FILE + port/threadx as
# the ThreadX core linked into the exe, or the TX_THREAD / event-flags / mutex
# layouts disagree (ABI mismatch).
add_library(shell_obj OBJECT
    "${CMAKE_SOURCE_DIR}/shell/core/cli_core.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_complete.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_registry.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_edit.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_history.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_job.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_parse.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_printf.c"
    "${CMAKE_SOURCE_DIR}/shell/core/cli_session.c"
    "${BOARD_DIR}/backend/cli_backend_uart.c"
    "${CMAKE_SOURCE_DIR}/shell/backend/cli_backend_dummy.c")
target_link_libraries(shell_obj PUBLIC bsp_iface)
target_include_directories(shell_obj
    PUBLIC  "${CMAKE_SOURCE_DIR}/shell/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/shell/core"
            "${CMAKE_SOURCE_DIR}/shell/backend"   # cli_uart_ring.h + the dummy backend
            "${BOARD_DIR}/backend"                # cli_backend_uart.h
            "${TX_DIR}/common/inc" "${TX_PORT}/inc"
            "${TX_DIR}/utility/execution_profile_kit"   # tx_api.h auto-includes tx_execution_profile.h (EPK)
            "${BOARD_DIR}/port/threadx")
target_compile_definitions(shell_obj PRIVATE TX_INCLUDE_USER_DEFINE_FILE)
target_compile_options(shell_obj PRIVATE -O2)

# (cli_version.h is generated by the top-level CMakeLists.txt, right after this
# file is included -- it needs BOARD_FW_NAME, set at the top of it.  GEN_DIR is
# already on the bsp_iface include path, so cmd_system.c can say
# #include "cli_version.h".)

# Compile-time gate for the dangerous shell commands (reboot, devmem), spec 12.
# ON by default for the demo; production builds pass -DCLI_ENABLE_DANGEROUS_CMDS=OFF.
# A cache var alone does NOT reach the compiler, so forward it explicitly to the
# define cli_config.h expects.
option(CLI_ENABLE_DANGEROUS_CMDS "Build the dangerous shell commands (reboot, devmem)" ON)

# Upper bound (bytes) on a single `devmem dump`; overrides the cli_config.h
# default.  STRING (not option) because it carries a number, forwarded below.
set(CLI_DEVMEM_DUMP_MAX_LEN "256" CACHE STRING "Max bytes per devmem dump")

# --- On-device NN inference backend ----------------------------------------
# Backend-agnostic nn layer (port/nn) over one selectable runtime.
# `tflm` (tflite-micro, the DEFAULT since issue #130) is a C++ runtime whose
# interpreter + backend live in a separate `tflm` static lib
# (cmake/tflite-micro.cmake); it enables C++ only for that build.  Its built-in
# model is the pinned upstream BlazeFace the other two boards ship, fetched and
# sha256-verified at BUILD time -- so the default build needs the network (it
# already did for tflite-micro itself).
# `null` (no runtime -> builds offline, with no toolchain or model beyond the
# cross compiler; the `nn` command reports a BlazeFace-shaped stub whose
# inference does nothing).  It was the default until #130.
# `stedgeai` (X-CUBE-AI) links generated sources + libNetworkRuntime.a from a
# local ST Edge AI Core install; those ST-SLA artifacts are NOT committed (public
# repo, see .gitignore + port/nn/generated/).
# `stedgeai_reloc` (X-CUBE-AI relocatable network) loads a position-independent
# `network_rel.bin` (PIC code + weights) from SD at runtime and runs it XIP via the
# ST host loader (ai_reloc_network.c) + the legacy ai_rel_network_* API -- no model is
# baked into Flash and no runtime .a is linked (the PIC kernels are embedded in the
# .bin).  Same SD-swap capability as tflm, but on the X-CUBE-AI runtime.
#
# [!] A CACHE DEFAULT DOES NOT REACH AN EXISTING TREE.  A build directory first
# configured while `null` was the default keeps `null`, and nothing about its
# build says so.  So: a tree that existed before this rule was seen (CMakeCache.txt
# already on disk at configure time, and no F746_NN_DEFAULT_SEEN in it) and holds
# `null` gets a WARNING once, and every `null` configure gets a STATUS line.  The
# value is NOT forced -- `null` may be a choice, and overwriting it would break the
# one tree that chose it.  A fresh configure with -DCONFIG_NN_BACKEND=null is not
# warned (there was no earlier default for it to have inherited).
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt" AND NOT DEFINED CACHE{F746_NN_DEFAULT_SEEN})
    set(_f746_nn_tree_predates_tflm_default TRUE)
else()
    set(_f746_nn_tree_predates_tflm_default FALSE)
endif()
set(CONFIG_NN_BACKEND "tflm" CACHE STRING "NN inference backend: tflm | null | stedgeai | stedgeai_reloc")
set_property(CACHE CONFIG_NN_BACKEND PROPERTY STRINGS tflm null stedgeai stedgeai_reloc)
if(CONFIG_NN_BACKEND STREQUAL "null")
    if(_f746_nn_tree_predates_tflm_default)
        message(WARNING
            "f746g-disco: the default NN backend is now `tflm` (issue #130), but "
            "this build directory was configured earlier and keeps "
            "CONFIG_NN_BACKEND=null from its cache -- its firmware runs no "
            "inference.  To follow the new default, re-configure with "
            "-DCONFIG_NN_BACKEND=tflm or use a fresh build directory.  If null is "
            "what you want, nothing needs to change (this warning is shown once "
            "per tree).")
    endif()
    message(STATUS "f746g-disco: CONFIG_NN_BACKEND=null (not the default `tflm`; no inference runtime)")
endif()
set(F746_NN_DEFAULT_SEEN "tflm" CACHE INTERNAL
    "This tree has been configured since the NN backend default became tflm (#130)")

# [!] ONE VARIABLE FOR THE SHARED DECODER'S PATH (issue #97): the source list and
# the audit target below must name the same file.
get_filename_component(F746_SHARED_DECODER
                       "${CMAKE_SOURCE_DIR}/svc/nn/blazeface.c" ABSOLUTE)
if(NOT EXISTS "${F746_SHARED_DECODER}")
    message(FATAL_ERROR
        "shared BlazeFace decoder not found:\n  ${F746_SHARED_DECODER}\n"
        "This board builds it (issue #97); it is not optional.")
endif()

set(NN_SOURCES
    "${BOARD_DIR}/port/nn/nn.c"
    "${BOARD_DIR}/port/nn/nn_svc_f746.c"   # the shared `nn` command's adapter
    "${BOARD_DIR}/port/nn/nn_svc_f746_model.c"  # ...its info / load / unload (#131)
    "${BOARD_DIR}/port/nn/nn_camera.c"          # live camera -> inference glue: console side
    "${BOARD_DIR}/port/nn/nn_producer.c"        # ...its camera producer thread (#140)
    "${BOARD_DIR}/port/nn/nn_worker.c"          # ...its inference worker thread (#140)
    "${BOARD_DIR}/port/nn/nn_sess_release.c"    # when the session may go back (#130)
    "${BOARD_DIR}/port/nn/nn_decode_count.c"    # how a decode is counted (#130 6c)
    # Model post-processing.  The decoder is SHARED with the other two boards
    # (issue #97); port/nn/nn_decoder.c is this board's half -- nn_tensor ->
    # tensor_desc, and the ownership of the decoder's state and its candidate
    # scratch, because the shared translation unit owns no storage at all.
    # Built in EVERY backend configuration, including `null`, which is what keeps
    # the unconditional --require-sdram-ai below honest.
    "${F746_SHARED_DECODER}"
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_det_record.c"
    # The top classes of a model the decoder does not recognise, taken by the
    # worker before its next inference (issue #121, decision D3).
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_top.c"
    "${BOARD_DIR}/port/nn/nn_decoder.c")
if(CONFIG_NN_BACKEND STREQUAL "null")
    list(APPEND NN_SOURCES "${BOARD_DIR}/port/nn/nn_null.c")
elseif(CONFIG_NN_BACKEND STREQUAL "stedgeai")
    # X-CUBE-AI / ST Edge AI Core backend.  Links the ST-SLA GCC/Cortex-M7
    # runtime static library + the `stedgeai generate` output under
    # port/nn/generated/ (both .gitignored).  Regenerate the model with:
    #   $STEDGEAI_ROOT/Utilities/linux/stedgeai generate --model M.tflite \
    #       --target stm32f7 --type tflite --name network \
    #       --output boards/f746g-disco/port/nn/generated
    set(STEDGEAI_ROOT "/opt/ST/STEdgeAI/4.0" CACHE PATH "ST Edge AI Core install root")
    set(STEDGEAI_RT "${STEDGEAI_ROOT}/Middlewares/ST/AI/Lib/GCC/ARMCortexM7/NetworkRuntime1201_CM7_GCC.a")
    set(STEDGEAI_INC "${STEDGEAI_ROOT}/Middlewares/ST/AI/Inc")
    if(NOT EXISTS "${STEDGEAI_RT}")
        message(FATAL_ERROR "stedgeai runtime not found:\n  ${STEDGEAI_RT}\nSet -DSTEDGEAI_ROOT=<install>")
    endif()
    if(NOT EXISTS "${BOARD_DIR}/port/nn/generated/network.c")
        message(FATAL_ERROR "generated model missing under boards/f746g-disco/port/nn/generated/ -- run `stedgeai generate`")
    endif()
    list(APPEND NN_SOURCES
        "${BOARD_DIR}/port/nn/nn_stedgeai.c"
        "${BOARD_DIR}/port/nn/generated/network.c"
        "${BOARD_DIR}/port/nn/generated/network_data.c")
elseif(CONFIG_NN_BACKEND STREQUAL "stedgeai_reloc")
    # X-CUBE-AI relocatable network backend.  Compiles the ST host loader
    # (Middlewares/ST/AI/Reloc/Src/ai_reloc_network.c, ST-SLA -> NOT committed) + our
    # backend TU.  NO runtime .a is linked: the PIC kernels + weights live inside the
    # SD-loaded network_rel.bin (generated offline with scripts/gen-reloc-model.sh).
    # No port/nn/generated model is needed (the model is loaded from SD at runtime).
    set(STEDGEAI_ROOT "/opt/ST/STEdgeAI/4.0" CACHE PATH "ST Edge AI Core install root")
    set(STEDGEAI_INC "${STEDGEAI_ROOT}/Middlewares/ST/AI/Inc")
    set(STEDGEAI_RELOC_INC "${STEDGEAI_ROOT}/Middlewares/ST/AI/Reloc/Inc")
    set(STEDGEAI_RELOC_LOADER "${STEDGEAI_ROOT}/Middlewares/ST/AI/Reloc/Src/ai_reloc_network.c")
    if(NOT EXISTS "${STEDGEAI_RELOC_LOADER}")
        message(FATAL_ERROR "stedgeai reloc loader not found:\n  ${STEDGEAI_RELOC_LOADER}\nSet -DSTEDGEAI_ROOT=<install>")
    endif()
    list(APPEND NN_SOURCES
        "${BOARD_DIR}/port/nn/nn_stedgeai_reloc.c"
        "${STEDGEAI_RELOC_LOADER}")
elseif(CONFIG_NN_BACKEND STREQUAL "tflm")
    # TFLM (tflite-micro) C++ backend.  The C++ backend (nn_tflm.cc providing
    # nn_backend_vt_selected) + the interpreter + the vendored tflite-micro tree are
    # built into a `tflm` static lib below (cmake/tflite-micro.cmake) and linked into
    # shell.  Nothing is added to NN_SOURCES here on purpose: shell's own TUs stay C,
    # so the final link keeps the C driver (gcc) and does NOT pull the full libstdc++.
    # nn.c's undefined ref to nn_backend_vt_selected pulls nn_tflm.o from the lib.
else()
    message(FATAL_ERROR "CONFIG_NN_BACKEND must be 'null', 'stedgeai', 'stedgeai_reloc' or 'tflm'")
endif()

add_executable(shell
    "${BOARD_DIR}/src/main.c"
    "${BOARD_DIR}/src/fault.c"
    "${BOARD_DIR}/src/iwdg.c"
    "${BOARD_DIR}/src/malloc_lock.c"
    "${BOARD_DIR}/src/retarget.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_dmesg.c"
    "${BOARD_DIR}/cmds/cmd_crash.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_builtin.c"
    "${BOARD_DIR}/cmds/cmd_system.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_thread.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_console.c"
    "${BOARD_DIR}/cmds/cmd_free.c"
    "${BOARD_DIR}/cmds/cmd_membench.c"
    "${BOARD_DIR}/cmds/cmd_devmem.c"
    "${BOARD_DIR}/cmds/cmd_coremark.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_sleep.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_watch.c"
    "${BOARD_DIR}/cmds/cmd_wdt.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_jobs.c"
    "${BOARD_DIR}/cmds/cmd_qspi.c"
    "${BOARD_DIR}/cmds/cmd_fs.c"
    "${BOARD_DIR}/cmds/cmd_sd.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_camera.c"  # the one shared command (#143)
    "${BOARD_DIR}/cmds/cmd_camera_board.c"
    "${BOARD_DIR}/cmds/cmd_lcd.c"
    "${BOARD_DIR}/cmds/cmd_touch.c"
    "${BOARD_DIR}/cmds/cmd_gui.c"
    "${BOARD_DIR}/cmds/cmd_xfer.c"
    "${BOARD_DIR}/cmds/cmd_sdram.c"
    "${BOARD_DIR}/cmds/cmd_net.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_nn.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/nn_cmd_core.c"
    "${BOARD_DIR}/cmds/cmd_nn_board.c"
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_stream_life.c"
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_core.c"
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_core_frame.c"   # the frame path (#130)
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_handoff.c"      # ...and its hand-over word
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_swap.c"         # where a load ends (#131)
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_core_model.c"   # ...and the order around it (#131)
    "${CMAKE_SOURCE_DIR}/svc/nn/nn_report.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/fs_cmd_core.c"
    "${BOARD_DIR}/port/qspi/qspi_flash.c"
    "${BOARD_DIR}/port/sd/sd_card.c"
    "${BOARD_DIR}/port/camera/camera.c"
    "${BOARD_DIR}/port/camera/camera_svc_f746.c"  # the shared `camera` command's adapter (#143)
    "${BOARD_DIR}/port/camera/camera_names.c"     # ...and the words it shares with cmds/
    "${BOARD_DIR}/port/camera/cam_drain.c"
    "${BOARD_DIR}/port/camera/cam_own.c"
    "${CMAKE_SOURCE_DIR}/svc/camera/cam_step.c"      # a step of the camera sink (#142)
    "${BOARD_DIR}/port/camera/cam_sink.c"     # ...and the sink that runs them
    "${BOARD_DIR}/port/camera/cam_hold_rules.c"  # this board's holder rules (#142)
    "${CMAKE_SOURCE_DIR}/svc/camera/cam_holders.c"   # who holds the stream open (#142)
    "${BOARD_DIR}/port/ltdc/ltdc_display.c"
    "${BOARD_DIR}/port/touch/touch.c"
    "${BOARD_DIR}/port/sdram/sdram.c"
    "${BOARD_DIR}/port/eth/eth_link.c"
    "${BOARD_DIR}/port/eth/eth_phy.c"
    "${BOARD_DIR}/port/netxduo/nx_eth_driver.c"
    "${BOARD_DIR}/port/netxduo/nx_glue.c"
    "${BOARD_DIR}/port/netxduo/nx_shell.c"
    "${BOARD_DIR}/port/netxduo/nx_mjpeg.c"
    "${NX_DIR}/addons/dhcp/nxd_dhcp_client.c"
    "${BOARD_DIR}/port/guix/guix_display.c"
    "${BOARD_DIR}/port/guix/guix_touch.c"
    "${BOARD_DIR}/port/guix/guix_glue.c"
    "${BOARD_DIR}/ui/guix_camera_ui.c"
    "${CMAKE_SOURCE_DIR}/lib/ov5640/ov5640.c"
    "${CMAKE_SOURCE_DIR}/lib/ov5640/ov5640_reg.c"
    "${BOARD_DIR}/port/levelx/lx_nor_qspi_driver.c"
    "${BOARD_DIR}/port/filex/fx_lx_nor_driver.c"
    "${BOARD_DIR}/port/filex/fx_sd_driver.c"
    "${BOARD_DIR}/port/filex/fs_glue.c"
    "${BOARD_DIR}/port/filex/sd_fs_glue.c"
    "${BOARD_DIR}/port/threadx/tx_glue.c"
    ${NN_SOURCES}                                                     # nn layer + backend
    "${TX_DIR}/utility/execution_profile_kit/tx_execution_profile.c"  # EPK (`thread` cpu%)
    ${TX_CORE}
    ${TX_ASM}
    ${FX_CORE}
    ${LX_CORE}
    ${GX_CORE}
    ${NX_CORE})
target_link_libraries(shell PRIVATE common shell_obj coremark_obj svc_obj)
target_include_directories(shell PRIVATE
    "${TX_DIR}/common/inc" "${TX_PORT}/inc"
    "${TX_DIR}/utility/execution_profile_kit"   # EPK header (`thread` cpu%)
    "${BOARD_DIR}/port/threadx"
    "${BOARD_DIR}/port/qspi"
    "${BOARD_DIR}/port/sd"
    "${BOARD_DIR}/port/camera"
    "${BOARD_DIR}/port/ltdc"
    "${BOARD_DIR}/port/touch"
    "${BOARD_DIR}/port/sdram"
    "${BOARD_DIR}/port/eth"
    "${BOARD_DIR}/port/nn"
    "${BOARD_DIR}/port/plugin"     # the plugin binding, lease and painter (#159)
    "${CMAKE_SOURCE_DIR}/lib/ov5640"
    "${FX_DIR}/common/inc" "${FX_DIR}/ports/cortex_m7/gnu/inc"
    "${LX_DIR}/common/inc"
    "${GX_DIR}/common/inc" "${GX_DIR}/ports/cortex_m7/gnu/inc"
    "${NX_DIR}/common/inc" "${NX_DIR}/ports/cortex_m7/gnu/inc"
    "${NX_DIR}/addons/dhcp"
    "${BOARD_DIR}/port/netxduo"    # nx_user.h + NetX ETH driver glue
    "${BOARD_DIR}/port/filex"
    "${BOARD_DIR}/port/levelx"
    "${BOARD_DIR}/port/guix"
    "${BOARD_DIR}/ui"              # ui/ presentation layer: guix_camera_ui.h
    "${CMAKE_SOURCE_DIR}/shell/include"
    "${CMAKE_SOURCE_DIR}/shell/core"      # cli_instance.h -> cli_internal.h
    "${CMAKE_SOURCE_DIR}/shell/backend"   # CLI_BACKEND_*_DEFINE macros
    "${BOARD_DIR}/backend"                # cli_backend_uart.h
    # The shared commands' own headers.  This board's cmds/ live in a different
    # directory from shell/cmds/ now, so cmd_fs.c / cmd_sd.c / cmd_ai.c can no
    # longer reach fs_cmd_core.h through the "same directory first" rule for
    # quoted includes.  Nothing else is in there, so no existing include can be
    # shadowed by adding it.
    "${CMAKE_SOURCE_DIR}/shell/cmds"
    "${CMAKE_SOURCE_DIR}/svc"      # shared svc: ymodem.h (cmd_xfer.c, cmd_camera_board.c)
    "${CMAKE_SOURCE_DIR}/svc/nn"
    "${CMAKE_SOURCE_DIR}/svc/plugin"
    "${CMAKE_SOURCE_DIR}/svc/camera"
    "${BOARD_DIR}/svc")            # this board's svc: log.h / timebase.h
target_compile_definitions(shell PRIVATE TX_INCLUDE_USER_DEFINE_FILE
    FX_INCLUDE_USER_DEFINE_FILE           # port/filex/fx_user.h
    LX_INCLUDE_USER_DEFINE_FILE           # port/levelx/lx_user.h
    GX_INCLUDE_USER_DEFINE_FILE           # port/guix/gx_user.h
    NX_INCLUDE_USER_DEFINE_FILE           # port/netxduo/nx_user.h
    NX_DHCP_CLIENT_USER_CREATE_PACKET_POOL  # DHCP reuses the shared pool
    NX_DHCP_THREAD_PRIORITY=13            # below the IP thread (12)
    CLI_ENABLE_DANGEROUS_CMDS=$<BOOL:${CLI_ENABLE_DANGEROUS_CMDS}>
    CLI_DEVMEM_DUMP_MAX_LEN=${CLI_DEVMEM_DUMP_MAX_LEN}
    CLI_INSTANCE_STACK_SIZE=4096          # headroom: coremark runs in this thread
    CLI_BG_JOB_STACK_SIZE=4096)           # ... and in a bg worker: coremark &
target_compile_options(shell PRIVATE -O2)
target_link_options(shell PRIVATE -u _printf_float)   # CoreMark %f score line

# X-CUBE-AI backend: generated + ST AI runtime include paths and the ST-SLA
# GCC/Cortex-M7 runtime static library.  Only when selected.
if(CONFIG_NN_BACKEND STREQUAL "stedgeai")
    target_include_directories(shell PRIVATE
        "${BOARD_DIR}/port/nn/generated"
        "${STEDGEAI_INC}")
    target_link_libraries(shell PRIVATE "${STEDGEAI_RT}")
    target_compile_definitions(shell PRIVATE CONFIG_NN_BACKEND_STEDGEAI=1)
endif()

# X-CUBE-AI relocatable backend: legacy ai_rel_network_* + ST host loader.  Add the
# ST AI + Reloc include paths and mark the backend; NO runtime .a is linked (PIC
# kernels are inside the SD-loaded .bin).  The ST loader TU gets APP_DEBUG=0 (drops
# its printf trace) scoped to that source only -- and we do NOT define STM32F7 for it,
# so in XIP mode it has zero HAL/CMSIS dependency (its only SCB_CleanDCache is
# COPY-mode-guarded; our backend does the cache maintenance itself).
if(CONFIG_NN_BACKEND STREQUAL "stedgeai_reloc")
    target_include_directories(shell PRIVATE "${STEDGEAI_INC}" "${STEDGEAI_RELOC_INC}")
    target_compile_definitions(shell PRIVATE CONFIG_NN_BACKEND_STEDGEAI_RELOC=1)
    # src/bsp.c (MPU region2 = the XIP exec window) is compiled in the `common`
    # object library, which only sees its own defines -- NOT shell's -- so the reloc
    # flag must be set on `common` too, else bsp.c disables the exec window even for
    # the reloc build and XIP faults.
    target_compile_definitions(common PRIVATE CONFIG_NN_BACKEND_STEDGEAI_RELOC=1)
    set_source_files_properties("${STEDGEAI_RELOC_LOADER}" PROPERTIES
        COMPILE_DEFINITIONS "APP_DEBUG=0")
endif()

# TFLM (tflite-micro) C++ backend.  Builds the `tflm` static lib (C++ interpreter +
# fetched tree + generated BlazeFace model + nn_tflm/cxx_runtime) and links it + the
# nano C++ runtime archives.  The final shell link stays on the C driver (no C++ TU
# in shell itself).
if(CONFIG_NN_BACKEND STREQUAL "tflm")
    include("${BOARD_DIR}/cmake/tflite-micro.cmake")
    target_link_libraries(shell PRIVATE tflm)
    target_compile_definitions(shell PRIVATE CONFIG_NN_BACKEND_TFLM=1)
    # Keep the final link on the C driver (gcc): shell's own TUs are all C, and
    # linking the CXX `tflm` lib would otherwise make CMake pick the g++ driver,
    # which auto-adds the FULL libstdc++ (exceptions).  We link the nano C++ archives
    # explicitly (in cmake/tflite-micro.cmake) instead.  LINKER_LANGUAGE C makes the
    # driver choice match that design.
    set_target_properties(shell PROPERTIES LINKER_LANGUAGE C)
endif()

firmware_finalize(shell)

# ---------------------------------------------------------------------------
# The plugin target word (issue #159 = #78 Step 4a)
# ---------------------------------------------------------------------------
#
# cortex-m7 / fpv5-sp-d16 / hard float / little endian, no CMSE (this part has
# no Security Extension), per plugin_target_id() in svc/plugin/plugin_abi.h.
# Checked at both ends rather than trusted: port/nn/nn_svc_f746.c static-asserts
# it against this firmware's own predefined macros (svc/plugin/plugin_target.h,
# also the only check of the CMSE bit) -- and since those macros are exactly a
# Cortex-M4 with FPv4's too, cmake/check_target_word.py checks it against the
# LINKED image's .ARM.attributes, whose Tag_FP_arch tells FPv5 from FPv4.
#
# Every backend: it describes the environment this firmware provides, whether
# or not this build can load a model.
set(F746_PLUGIN_TARGET_ID "0x1101")
target_compile_definitions(shell PRIVATE
    F746_PLUGIN_TARGET_ID=${F746_PLUGIN_TARGET_ID})
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/check_target_word.py"
            $<TARGET_FILE:shell> --target-id "${F746_PLUGIN_TARGET_ID}"
    COMMENT "check the plugin target word against the firmware image"
    VERBATIM)

# ---------------------------------------------------------------------------
# The .plugin reservation (issue #159 = #78 Step 4a)
# ---------------------------------------------------------------------------
#
# The top 32 KB of internal SRAM, 0x20048000..0x20050000, in every backend (the
# linker script has it whatever the backend, so its gate -- in
# cmake/check_f746_layout.py, below -- runs whatever the backend).
#
# [!] WRITTEN OUT AGAIN HERE, ON PURPOSE.  This is what Step 4b's add_plugin()
# tells the shared image gate.  The same reservation is stated in the linker
# script, in ldscript/plugin_memory.ld (what a plugin links against) and in
# cmake/check_f746_layout.py, each as its own literal: a gate that read its
# expected value out of what it checks would pass anything.  Do not derive one
# from another.
set(F746_PLUGIN_GATE_BASE 0x20048000)
set(F746_PLUGIN_GATE_END  0x20050000)
set(F746_PLUGIN_MEMORY_LD "${BOARD_DIR}/ldscript/plugin_memory.ld")

# ---------------------------------------------------------------------------
# Plugin containers: deliver, validate, RUN (issue #159 Steps 4a and 4b)
# ---------------------------------------------------------------------------
#
# `nn model load --path <x.nnc>` recognises a container, validates it with this
# board's policy (svc/plugin/plugin_load.c -- the validator the host asset build
# runs too), hands its model section to the backend in place and, since Step
# 4b, copies its plugin into .plugin and starts it: the plugin then decodes on
# the nn worker, draws on the GUIX thread and reports to a console, every entry
# under the plugin lease.  The resident decoder stays as the no-plugin fallback
# until Step 4c.  See the board README.
#
# tflm only: the other backends cannot hand a model section to an interpreter
# in place, so a container has nowhere to go there.
if(CONFIG_NN_BACKEND STREQUAL "tflm")
    # The policy's address and capacity, for the firmware's validator.  The
    # reservation's address is still stated independently elsewhere (above);
    # check_f746_layout.py holds this F746_PLUGIN_BASE to the linked
    # __plugin_start (--policy-link-addr, below).
    set(F746_PLUGIN_BASE "0x20048000")
    set(F746_PLUGIN_MAX  "32768")

    # What each thread may lend a plugin callback -- wio-lite-ai's policy to
    # start (plan #159 D6).  The honest figure is "thread stack - depth at the
    # call site - asynchronous reserve - margin", the call-site depth being what
    # `nn stream stats` measures ("at call"); the asynchronous reserve is 208 B
    # on this core and ThreadX port (PM0253 2.3.7: the 26-word extended frame
    # plus its alignment pad, and ThreadX's r4-r11 / s16-s31 / LR save).  Each
    # allowance is asserted below its thread's stack in the firmware, and the
    # README's table records the measured depths it is checked against.
    include("${CMAKE_SOURCE_DIR}/cmake/plugin_stack_table.cmake")
    plugin_stack_table(
        DEFINE_ON  shell
        ALLOWANCES F746_PLUGIN_STACK_NN_WORK=1024
                   F746_PLUGIN_STACK_GUIX=1024
                   F746_PLUGIN_STACK_SHELL=1024
        SLOTS      entry=F746_PLUGIN_STACK_SHELL
                   shapes_ok=F746_PLUGIN_STACK_SHELL
                   decode=F746_PLUGIN_STACK_NN_WORK
                   draw=F746_PLUGIN_STACK_GUIX
                   report=F746_PLUGIN_STACK_SHELL
                   param_set=F746_PLUGIN_STACK_SHELL
                   param_get=F746_PLUGIN_STACK_SHELL)
    # The plugin's own string sink (asset/common), as wio-lite-ai's.
    set(F746_PLUGIN_SBUF_WRITE_MAX 64)

    target_sources(shell PRIVATE
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_load.c"   # the device's validator
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_info.c"   # `nn info`'s stack lines
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_claims.c" # ...and the claims around them
        "${CMAKE_SOURCE_DIR}/svc/crc32.c"                # the digest and the load CRC
        "${BOARD_DIR}/port/nn/nn_container.c"            # ...over the bytes read
        # Step 4b: the loader, the lease, the painter and the decoder branch.
        # What is board-independent is svc/'s (shared with wio-lite-ai); what
        # touches this board's hardware or RTOS is port/plugin/ and port/nn/.
        "${CMAKE_SOURCE_DIR}/svc/nn/nn_active_core.c"    # the one branch point
        "${CMAKE_SOURCE_DIR}/svc/nn/nn_active_lease.c"   # ...its lease-checked entries
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_exec.c"   # the loader
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_run_core.c" # ...its lease checks
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_mpu_v7m.c"  # the Armv7-M MPU verdict
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_paint_budget.c"
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_paint_span.c"
        "${CMAKE_SOURCE_DIR}/svc/rect_geom.c"
        "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_lease_miss.c"
        "${BOARD_DIR}/port/nn/nn_active.c"                # this board's facts
        "${BOARD_DIR}/port/nn/nn_panel.c"                 # the GUIX draw
        "${BOARD_DIR}/port/plugin/plugin_run.c"           # MPU read-back, caches
        "${BOARD_DIR}/port/plugin/plugin_lease.c"         # the lease (ThreadX)
        "${BOARD_DIR}/port/plugin/plugin_paint.c"         # CPU stores, view buffer
        "${BOARD_DIR}/src/fault_plugin.c")                # a plugin's fault: reset
    target_compile_definitions(shell PRIVATE
        F746_PLUGIN_BASE=${F746_PLUGIN_BASE}u
        F746_PLUGIN_MAX=${F746_PLUGIN_MAX}u)
    # (F746_PLUGIN_STACK_* are defined by plugin_stack_table(), and
    # PLUGIN_VENEER_BASE_COST by veneer_cost_gate() below.)

    # Entry points a plugin may never reach on this board.  A plugin links no
    # firmware code at all (-nostdlib, and the gate refuses an undefined
    # symbol), so this matters for what a plugin might DEFINE under one of these
    # names: the internal-flash and option-byte writers, the QSPI NOR writers
    # (the filesystem), the clock and supply configuration, and the MPU / cache
    # / NVIC state the base owns.
    set(F746_PLUGIN_FORBIDDEN
        HAL_FLASH_Unlock
        HAL_FLASH_Program
        HAL_FLASHEx_Erase
        HAL_FLASH_OB_Unlock
        HAL_FLASH_OB_Launch
        HAL_FLASHEx_OBProgram
        qspi_flash_write
        qspi_flash_erase_sector
        qspi_flash_erase_subsector
        qspi_flash_erase_chip
        HAL_RCC_OscConfig
        HAL_RCC_ClockConfig
        HAL_RCCEx_PeriphCLKConfig
        HAL_PWREx_EnableOverDrive
        HAL_MPU_ConfigRegion
        HAL_MPU_Enable
        HAL_MPU_Disable
        ARM_MPU_Enable
        ARM_MPU_Disable
        ARM_MPU_SetRegion
        SCB_EnableDCache
        SCB_DisableDCache
        SCB_InvalidateICache
        NVIC_EnableIRQ
        NVIC_DisableIRQ)

    # What the base itself may spend below one veneer, charged at each indirect
    # call because the gate cannot see across it.  DERIVED FROM THE SHIPPED IMAGE
    # AND CHECKED, EVERY BUILD (cmake/check_veneer_base_cost.py, issue #112):
    # the build fails if the stack below any veneer exceeds this.  wio-lite-ai's
    # declaration to start -- over-estimating is the safe direction, since a
    # larger charge makes a plugin's requirement larger, not smaller.  It
    # REPLACES Step 4a's provisional c (F746_PLUGIN_VENEER_COST_4A), which
    # nothing checked.
    set(F746_PLUGIN_VENEER_BASE_COST 640)

    # The firmware side of that charge: the function this board binds behind
    # each veneer (nn_plugin_base in port/nn/nn_active.c, the painter in
    # port/plugin/plugin_paint.c, the report sink in svc/nn/nn_report.c), the
    # charge -- the SAME variable every add_plugin() below is given -- and where
    # the toolchain's archives live.  `flash` and every container's pack wait
    # for the check, and for the policy read back out of shell.elf.
    get_filename_component(_f746_gcc_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(F746_TOOLCHAIN_ROOT "${_f746_gcc_bin}" DIRECTORY)
    include("${CMAKE_SOURCE_DIR}/cmake/veneer_cost_gate.cmake")
    veneer_cost_gate(
        FIRMWARE shell
        MAP      "${CMAKE_BINARY_DIR}/shell.map"
        DECLARED ${F746_PLUGIN_VENEER_BASE_COST}
        ROOTS    pl_base_log=nn_plugin_log
                 pl_base_to_frame=nn_active_to_frame
                 pl_paint_rect=paint_rect
                 pl_paint_fill_rect=paint_fill_rect
                 pl_paint_blit=paint_blit
                 pl_print_write=nn_report_write
        PREBUILT_ROOTS "${F746_TOOLCHAIN_ROOT}"
        DELIVERY flash)

    set(F746_PLUGIN_COMMON "${CMAKE_SOURCE_DIR}/asset/common")
    # The firmware's own architecture, and only that.  The float ABI is the
    # firmware's, because a plugin's callbacks are called with it.
    set(F746_PLUGIN_ARCH_FLAGS
        -mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard)
    set(F746_PLUGIN_CFLAGS
        ${F746_PLUGIN_ARCH_FLAGS}
        -Os -std=c11 -Wall -Wextra -Werror
        -ffreestanding -fno-builtin -fno-common
        -ffunction-sections -fdata-sections
        -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
        -fstack-usage
        -I "${CMAKE_SOURCE_DIR}/svc" -I "${CMAKE_SOURCE_DIR}/svc/nn" -I "${CMAKE_SOURCE_DIR}/svc/plugin" -I "${CMAKE_SOURCE_DIR}/svc/camera" -I "${F746_PLUGIN_COMMON}")

    include("${CMAKE_SOURCE_DIR}/cmake/add_plugin.cmake")

    # The shared BlazeFace decoder, compiled into the blazeface plugin and audited
    # there as AUDIT_SHARED (the firmware still links it too, as the resident
    # decoder, until Step 4c -- F746_SHARED_DECODER above).
    set(F746_PLUGIN_ELFS "")
    add_plugin(blazeface
        CFLAGS ${F746_PLUGIN_CFLAGS}
        ARCH_FLAGS ${F746_PLUGIN_ARCH_FLAGS}
        MEMORY_LD "${F746_PLUGIN_MEMORY_LD}"
        IMAGE_BASE ${F746_PLUGIN_GATE_BASE}
        IMAGE_END  ${F746_PLUGIN_GATE_END}
        FORBIDDEN  ${F746_PLUGIN_FORBIDDEN}
        VENEER_BASE_COST ${F746_PLUGIN_VENEER_BASE_COST}
        TARGET_ID  ${F746_PLUGIN_TARGET_ID}
        OUT_DIR "${CMAKE_BINARY_DIR}/plugin"
        OUT_VAR F746_PLUGIN_ELFS
        SOURCES "${F746_SHARED_DECODER}"
        AUDIT_SHARED "${F746_SHARED_DECODER}"
        SLOTS   entry shapes_ok decode draw report param_set param_get
        SBUF_WRITE_MAX ${F746_PLUGIN_SBUF_WRITE_MAX})

    # Built to prove the build and to measure it.  No asset carries it: no
    # CIFAR-10 model this backend runs is pinned in the tree (Grove's is a
    # Vela-compiled Ethos-U model), as on wio-lite-ai.
    add_plugin(cifar10
        CFLAGS ${F746_PLUGIN_CFLAGS}
        ARCH_FLAGS ${F746_PLUGIN_ARCH_FLAGS}
        MEMORY_LD "${F746_PLUGIN_MEMORY_LD}"
        IMAGE_BASE ${F746_PLUGIN_GATE_BASE}
        IMAGE_END  ${F746_PLUGIN_GATE_END}
        FORBIDDEN  ${F746_PLUGIN_FORBIDDEN}
        VENEER_BASE_COST ${F746_PLUGIN_VENEER_BASE_COST}
        TARGET_ID  ${F746_PLUGIN_TARGET_ID}
        OUT_DIR "${CMAKE_BINARY_DIR}/plugin"
        OUT_VAR F746_PLUGIN_ELFS
        SLOTS   entry shapes_ok decode draw report
        SBUF_WRITE_MAX ${F746_PLUGIN_SBUF_WRITE_MAX})

    add_custom_target(plugin ALL DEPENDS ${F746_PLUGIN_ELFS})

    # --- the container chain's host tools --------------------------------------
    #   abi_layout        svc/plugin/plugin_abi.h's layout as JSON, for the packer
    #   verify_container  svc/plugin/plugin_load.c -- the device's validator -- on
    #                     the host, so a container cannot pass here and fail there
    # (No slot table: this board's destination is a file on the card, fit-checked
    # against the staging capacity read back from shell.elf.)
    set(F746_ABI_LAYOUT_JSON "${CMAKE_BINARY_DIR}/plugin/abi_layout.json")
    set(F746_CONTAINER_VERIFIER "${CMAKE_BINARY_DIR}/verify_container")
    set(F746_PACKER "${CMAKE_SOURCE_DIR}/asset/tools/pack_container.py")

    find_program(F746_HOST_CC NAMES cc gcc clang)
    if(F746_HOST_CC)
        add_custom_command(
            OUTPUT "${F746_ABI_LAYOUT_JSON}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/plugin"
            COMMAND "${F746_HOST_CC}" -std=c11 -O1 -I "${CMAKE_SOURCE_DIR}/svc" -I "${CMAKE_SOURCE_DIR}/svc/nn" -I "${CMAKE_SOURCE_DIR}/svc/plugin" -I "${CMAKE_SOURCE_DIR}/svc/camera"
                    "${CMAKE_SOURCE_DIR}/asset/tools/abi_layout.c"
                    -o "${CMAKE_BINARY_DIR}/plugin/abi_layout"
            COMMAND "${CMAKE_BINARY_DIR}/plugin/abi_layout"
                    > "${F746_ABI_LAYOUT_JSON}"
            DEPENDS "${CMAKE_SOURCE_DIR}/asset/tools/abi_layout.c"
                    "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_abi.h"
            COMMENT "host cc -> abi_layout.json (the packer's view of the ABI)"
            VERBATIM)
        add_custom_command(
            OUTPUT "${F746_CONTAINER_VERIFIER}"
            COMMAND "${F746_HOST_CC}" -std=c11 -O1 -Wall -Wextra
                    -I "${CMAKE_SOURCE_DIR}/svc" -I "${CMAKE_SOURCE_DIR}/svc/nn" -I "${CMAKE_SOURCE_DIR}/svc/plugin" -I "${CMAKE_SOURCE_DIR}/svc/camera"
                    "${CMAKE_SOURCE_DIR}/asset/tools/verify_container.c"
                    "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_load.c"
                    "${CMAKE_SOURCE_DIR}/svc/crc32.c"
                    -o "${F746_CONTAINER_VERIFIER}"
            DEPENDS "${CMAKE_SOURCE_DIR}/asset/tools/verify_container.c"
                    "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_load.c"
                    "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_load.h"
                    "${CMAKE_SOURCE_DIR}/svc/plugin/plugin_abi.h"
            COMMENT "host cc -> verify_container (the device's own validator)"
            VERBATIM)
        add_custom_target(container-tools ALL
            DEPENDS "${F746_ABI_LAYOUT_JSON}" "${F746_CONTAINER_VERIFIER}")
    else()
        message(STATUS "f746g-disco: no host C compiler -- asset-* targets will refuse")
    endif()

    execute_process(COMMAND ${FETCH_UPSTREAM_GIT} -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
                    OUTPUT_VARIABLE F746_PLUGIN_BUILD_ID
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT F746_PLUGIN_BUILD_ID)
        set(F746_PLUGIN_BUILD_ID "nogit")
    endif()

    # --- sendable assets ----------------------------------------------------------
    #
    # `--target asset-<name>` produces build/<board>/asset/<name>.nnc: a container
    # that has already been through every gate, published only after passing
    # (cmake/build_asset.py), and a receipt.  This board's destination is a FILE
    # copied to the SD card (plan #159 D4/D5): its fit is checked against the
    # staging capacity the shipped shell.elf holds in nn_tflm_stage_cap -- the
    # object the backend's load_region() hands out as the read limit
    # (port/nn/tflm/nn_tflm.cc) -- and the closing check is the CRC the load
    # prints over the whole file it read, compared with the receipt's.
    #
    # [!] THE GATES RUN AT BUILD TIME, NOT AT COPY TIME: nothing checks that the
    # file copied is the file built -- the CRC the load prints is the only
    # "built bytes = stored bytes" check.  Neither the asset nor its fetch is in
    # ALL (issue #94's lesson).
    if(HOST_CXX)
        set(F746_MODEL_VERIFIER "${CMAKE_BINARY_DIR}/verify_tflite")
    else()
        # Left EMPTY on purpose: build_asset.py refuses on an empty verifier
        # rather than publishing an unchecked model.
        set(F746_MODEL_VERIFIER "")
    endif()
    include("${CMAKE_SOURCE_DIR}/cmake/add_asset.cmake")

    function(f746_add_asset _name)
        cmake_parse_arguments(A ""
            "PLUGIN;FILE_NAME;FILE;URL;COMMIT;PATH_IN;SHA256" "" ${ARGN})
        if(A_UNPARSED_ARGUMENTS)
            message(FATAL_ERROR
                "f746_add_asset(${_name}): unrecognised: ${A_UNPARSED_ARGUMENTS}")
        endif()
        set(_pin "")
        foreach(_k FILE URL COMMIT PATH_IN SHA256)
            if(DEFINED A_${_k})
                list(APPEND _pin ${_k} "${A_${_k}}")
            endif()
        endforeach()
        # No ingest: the model as the zoo ships it (tflm with CMSIS-NN reads it).
        asset_model_source(${_name}
            OVERRIDE_VAR F746_ASSET_${_name}_FILE OUT_VAR _src ${_pin})
        add_asset(${_name}
            MODEL "${_src}"  MODEL_SHA256 "${_src_SHA256}"
            PLUGIN ${A_PLUGIN}
            FILE_NAME "${A_FILE_NAME}"  STAGE_SYMBOL nn_tflm_stage_cap
            PACKER "${F746_PACKER}"  LAYOUT "${F746_ABI_LAYOUT_JSON}"
            MODEL_VERIFIER "${F746_MODEL_VERIFIER}"
            CONTAINER_VERIFIER "${F746_CONTAINER_VERIFIER}"
            BUILD_ID "${F746_PLUGIN_BUILD_ID}"
            TARGET_ID "${F746_PLUGIN_TARGET_ID}"
            LINK_ADDR "${F746_PLUGIN_BASE}"
            CAPACITY "${F746_PLUGIN_MAX}"
            # The board has no receive path of its own: the file reaches the
            # card through a PC card reader, and the load reads it whole.
            RECEIPT_STEPS "nn model load --path /{file}   (once the file is on the card)"
            RECEIPT_TRANSFER "on the PC:" "copy the path above to the card's root as {file}"
            RECEIPT_VERIFY   "the load:"
                "must print crc32 {crc} -- that is how you know the bytes"
                "that were built are the bytes that were read.")
    endfunction()

    # The file wio-lite-ai and Grove pin -- the same commit and SHA-256, from the
    # ST model zoo -- and the model this board's built-in array is generated
    # from (cmake/tflite-micro.cmake).  Licence: model-zoo weights, fetched into
    # the build tree only, never committed.
    f746_add_asset(blazeface
        PLUGIN  blazeface  FILE_NAME blazeface.nnc
        URL     "https://github.com/STMicroelectronics/stm32ai-modelzoo.git"
        COMMIT  "1423c78953a830903485135febe1dd98ff31aed8"
        PATH_IN "face_detection/facedetect_front/Public_pretrainedmodel_public_dataset/widerface/blazeface_front_128/blazeface_front_128_int8.tflite"
        SHA256  "e803bb4e93b10f7a19d4243bcc39698599a723f3128e19d3f90e0b1c0bc88dd8")
endif()

# --- Post-link placement gate ------------------------------------------------
# The linker script carries most of this board's placement invariant in ASSERTs,
# but ASSERTs can only see the section BOUNDARIES, and this script defines those
# boundaries unconditionally (`.sdram` is one output section with _ssdram_cam /
# _ssdram_eth / ... always emitted, whether or not anything landed in them).  So
# an object that quietly loses its section attribute -- a refactor, an #ifdef, a
# renamed input pattern -- moves to .bss, every ASSERT still passes, and the
# camera DMA arena is simply not where the driver believes it is.
#
# check_f746_layout.py closes that by checking the linked image for the SYMBOLS
# instead: each required object must resolve to an address inside its region, and
# a symbol that is missing entirely fails rather than passing vacuously.  It also
# verifies the three interrupt handlers this firmware must own are STRONG
# definitions distinct from Default_Handler and actually installed in the vector
# table -- the stock startup file supplies all three as weak aliases, so "is it
# defined?" would be answered yes by a build that lost ThreadX's PendSV.
#
# Configuration-dependent residents are named HERE rather than in the script, for
# the reason wio-lite-ai's checkers document: a required symbol the build never
# compiled reports as "no such object in the image", which reads like a placement
# regression and is not one.  The condition on each line is the one that adds its
# source above.
set(F746_LAYOUT_REQUIRED "")
# [!] A GATE THIS BOARD DID NOT HAVE (issue #97).  The decoder's candidate scratch
# has always lived in .sdram.ai, and nothing checked it: the built-in required list
# in check_f746_layout.py never named it, `.sdram.ai` uses KEEP so --gc-sections
# cannot even produce the "no such object" hint, and the linker ASSERTs bound only
# where the section STARTS -- an empty .sdram.ai satisfies every one of them.
# Losing the attribute would have moved the buffer into internal SRAM with the
# build still green.  Wio's equivalent gate catches exactly that class; this board
# was the one where it would have been silent.  Unconditional, because the decoder
# is compiled in every backend configuration.
list(APPEND F746_LAYOUT_REQUIRED --require-sdram-ai nn_dec_scratch)
# [!] AND THE STATE MUST STAY OUT.  The requirement above says nothing about the
# decoder's state, and moving `nn_dec` / `nn_dec_ready` into the arena would leave
# every gate green -- while the whole .sdram output section is NOLOAD, so the
# threshold would come up holding the previous run's bytes and a `ready` flag that
# followed it would survive a warm reset still saying ready, skipping
# initialisation over stale state.
list(APPEND F746_LAYOUT_REQUIRED
     --forbid-sdram nn_dec --forbid-sdram nn_dec_ready)
if(CONFIG_NN_BACKEND STREQUAL "null")
    # port/nn/nn_null.c -- the stub backend's input buffer in the NN arena.
    list(APPEND F746_LAYOUT_REQUIRED --require-sdram-ai null_in_buf)
endif()
if(CONFIG_NN_BACKEND STREQUAL "tflm")
    # port/nn/tflm/nn_tflm.cc -- the activation arena (g_arena) and the two SD
    # model slots (g_sd_model_buf), the tflm backend's whole bank3 footprint
    # (issue #130).  Both are in an ANONYMOUS namespace, so nm (demangling is not
    # requested) reports them as the local symbols spelled below, as GCC mangles
    # them: `12_GLOBAL__N_1` is the anonymous namespace, `L` marks internal
    # linkage, and the length prefix is part of the name.  Spelling them mangled
    # is deliberate -- renaming either object, or moving it out of the namespace,
    # makes the name match NOTHING, which the gate reports as "no such object" (a
    # failure), never as a pass.  (The first build of this rule, written without
    # the `L`, failed exactly that way.)
    list(APPEND F746_LAYOUT_REQUIRED
         --require-sdram-ai _ZN12_GLOBAL__N_1L7g_arenaE
         --require-sdram-ai _ZN12_GLOBAL__N_1L14g_sd_model_bufE)
    # The plugin policy's link address -- F746_PLUGIN_BASE, the value compiled
    # into the policy as -DF746_PLUGIN_BASE -- is the linked __plugin_start
    # (issue #159 Step 4b): a fifth statement of the reservation's address, and
    # the one the device refuses containers by.
    list(APPEND F746_LAYOUT_REQUIRED --policy-link-addr ${F746_PLUGIN_BASE})
endif()
if(CONFIG_NN_BACKEND STREQUAL "stedgeai_reloc")
    # port/nn/nn_stedgeai_reloc.c -- the executable model slots, which MUST sit in
    # the 0xC0700000 window bsp.c region2 makes instruction-fetchable.
    list(APPEND F746_LAYOUT_REQUIRED --require-model-window g_model_slot)
endif()

# Shared service storage checks are derived after this file by the root CMake.

add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}"
            "${BOARD_DIR}/cmake/check_f746_layout.py"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            ${F746_LAYOUT_REQUIRED}
            $<TARGET_FILE:shell>
    COMMENT "check F746 memory placement, vectors and float runtime"
    VERBATIM)
