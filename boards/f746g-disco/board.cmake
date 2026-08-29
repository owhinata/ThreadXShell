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

set(HAL_DIR    "${CMAKE_SOURCE_DIR}/lib/stm32f7xx_hal_driver")
set(CMSIS_DEV  "${CMAKE_SOURCE_DIR}/lib/cmsis_device_f7")
set(CMSIS_CORE "${CMAKE_SOURCE_DIR}/lib/cmsis_core")
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
    "${CMAKE_SOURCE_DIR}/svc"   # shared freestanding services: fmt.h / ymodem.h / frame.h
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
    "${CMAKE_SOURCE_DIR}/svc/frame_pipeline.c")
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
# Backend-agnostic nn layer (port/nn) over one selectable runtime.  Default
# `null` (no runtime -> the firmware always builds without the ST Edge AI Core
# toolchain or any model; the `ai` command reports a BlazeFace-shaped stub).
# `stedgeai` (X-CUBE-AI) links generated sources + libNetworkRuntime.a from a
# local ST Edge AI Core install; those ST-SLA artifacts are NOT committed (public
# repo, see .gitignore + port/nn/generated/).
# `tflm` (tflite-micro) is a C++ runtime whose interpreter + backend live in a
# separate `tflm` static lib (cmake/tflite-micro.cmake); it enables C++ only for
# that build so the default (null/stedgeai) firmware stays byte-identical and
# needs no C++ toolchain.
# `stedgeai_reloc` (X-CUBE-AI relocatable network) loads a position-independent
# `network_rel.bin` (PIC code + weights) from SD at runtime and runs it XIP via the
# ST host loader (ai_reloc_network.c) + the legacy ai_rel_network_* API -- no model is
# baked into Flash and no runtime .a is linked (the PIC kernels are embedded in the
# .bin).  Same SD-swap capability as tflm, but on the X-CUBE-AI runtime.
set(CONFIG_NN_BACKEND "null" CACHE STRING "NN inference backend: null | stedgeai | stedgeai_reloc | tflm")
set_property(CACHE CONFIG_NN_BACKEND PROPERTY STRINGS null stedgeai stedgeai_reloc tflm)

# [!] ONE VARIABLE FOR THE SHARED DECODER'S PATH (issue #97): the source list and
# the audit target below must name the same file.
get_filename_component(F746_SHARED_DECODER
                       "${CMAKE_SOURCE_DIR}/svc/blazeface.c" ABSOLUTE)
if(NOT EXISTS "${F746_SHARED_DECODER}")
    message(FATAL_ERROR
        "shared BlazeFace decoder not found:\n  ${F746_SHARED_DECODER}\n"
        "This board builds it (issue #97); it is not optional.")
endif()

set(NN_SOURCES
    "${BOARD_DIR}/port/nn/nn.c"
    "${BOARD_DIR}/port/nn/nn_svc_f746.c"   # the shared `nn` command's adapter
    "${BOARD_DIR}/port/nn/nn_camera.c"          # live camera -> inference glue
    # Model post-processing.  The decoder is SHARED with the other two boards
    # (issue #97); port/nn/nn_decoder.c is this board's half -- nn_tensor ->
    # tensor_desc, and the ownership of the decoder's state and its candidate
    # scratch, because the shared translation unit owns no storage at all.
    # Built in EVERY backend configuration, including `null`, which is what keeps
    # the unconditional --require-sdram-ai below honest.
    "${F746_SHARED_DECODER}"
    "${CMAKE_SOURCE_DIR}/svc/nn_det_record.c"
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
    "${BOARD_DIR}/cmds/cmd_camera.c"
    "${BOARD_DIR}/cmds/cmd_lcd.c"
    "${BOARD_DIR}/cmds/cmd_touch.c"
    "${BOARD_DIR}/cmds/cmd_gui.c"
    "${BOARD_DIR}/cmds/cmd_xfer.c"
    "${BOARD_DIR}/cmds/cmd_sdram.c"
    "${BOARD_DIR}/cmds/cmd_net.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_nn.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/nn_cmd_core.c"
    "${BOARD_DIR}/cmds/cmd_nn_board.c"
    "${CMAKE_SOURCE_DIR}/shell/cmds/fs_cmd_core.c"
    "${BOARD_DIR}/port/qspi/qspi_flash.c"
    "${BOARD_DIR}/port/sd/sd_card.c"
    "${BOARD_DIR}/port/camera/camera.c"
    "${BOARD_DIR}/port/camera/cam_drain.c"
    "${BOARD_DIR}/port/camera/cam_own.c"
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
    "${CMAKE_SOURCE_DIR}/svc"      # shared svc: ymodem.h (cmd_xfer.c, cmd_camera.c)
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
if(CONFIG_NN_BACKEND STREQUAL "stedgeai_reloc")
    # port/nn/nn_stedgeai_reloc.c -- the executable model slots, which MUST sit in
    # the 0xC0700000 window bsp.c region2 makes instruction-fetchable.
    list(APPEND F746_LAYOUT_REQUIRED --require-model-window g_model_slot)
endif()

# --- the shared decoder must own no storage (issue #97) -----------------------
#
# See cmake/shared_storage_gate.cmake for what this checks and why it has to be
# THIS board's compile rather than a generic one.
include("${CMAKE_SOURCE_DIR}/cmake/shared_storage_gate.cmake")
add_shared_storage_gate(NAME f746_decoder_audit SOURCE "${F746_SHARED_DECODER}"
                         IFACE bsp_iface CONSUMER shell)
add_dependencies(shell f746_decoder_audit_check)

# The same rule on the one shared `nn` command and its pure half (issue #50).
# [!] THIS BOARD IS THE ONE THE GATE MATTERS MOST FOR: it has no residency check
# that would notice a buffer quietly landing in internal SRAM, so a static added
# to a shared unit would regress placement here as a SUCCESSFUL build.
add_shared_storage_gate(NAME f746_nn_cmd_audit
                        SOURCE "${CMAKE_SOURCE_DIR}/shell/cmds/cmd_nn.c"
                        IFACE bsp_iface CONSUMER shell)
add_dependencies(shell f746_nn_cmd_audit_check)
add_shared_storage_gate(NAME f746_nn_core_audit
                        SOURCE "${CMAKE_SOURCE_DIR}/shell/cmds/nn_cmd_core.c"
                        IFACE bsp_iface CONSUMER shell)
add_dependencies(shell f746_nn_core_audit_check)

add_custom_command(TARGET shell POST_BUILD
    COMMAND "${Python3_EXECUTABLE}"
            "${BOARD_DIR}/cmake/check_f746_layout.py"
            --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
            ${F746_LAYOUT_REQUIRED}
            $<TARGET_FILE:shell>
    COMMENT "check F746 memory placement, vectors and float runtime"
    VERBATIM)
