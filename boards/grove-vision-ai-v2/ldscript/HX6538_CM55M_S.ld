/* ============================================================================
 * Grove Vision AI V2 (Himax HX6538, CM55M big core, Secure-only) app layout.
 *
 * Board-owned rewrite of the SDK's linker_script/gcc/TrustZone_S_ONLY.ld with
 * every address resolved to a literal (the SDK version #includes
 * WE2_device_addr.h and is run through the C preprocessor; this file is plain
 * GNU ld so the repo's build needs no preprocessing step).  Structure and
 * symbol names are kept SDK-identical on purpose -- the CMSIS startup
 * (__PROGRAM_START copy/zero tables, __StackTop/__StackLimit) and the Himax
 * image generator both consume them.
 *
 * The app is NOT XIP.  The 2nd-stage bootloader loads the ELF segments:
 *   CM55M_S_APP_ROM   ITCM  256 KB @ 0x10000000 (secure alias) -- vectors+code
 *   CM55M_S_APP_DATA  DTCM  256 KB @ 0x30000000                -- data+stack
 *   CM55M_S_SRAM      SRAM0 window from 0x3401F000 (the 2nd bootloader's own
 *                     exec address; reusing it is the SDK's protocol -- the
 *                     loader is done by the time the app runs).  EXPLICIT
 *                     placement only; nothing goes here in M-G1, and an ITCM
 *                     overflow is a link error, never a silent spill.
 *
 * The vector table stays in ITCM (RAM): the prebuilt driver archive installs
 * its ISRs at runtime via EPII_NVIC_SetVector, which writes this table.
 *
 * Deviations from the SDK script (all reviewed, M-G1 plan):
 *   - .shell_root_cmds input sections are embedded INSIDE .rodata (see there).
 *   - The FreeRTOS debug-config KEEP line is dropped (FreeRTOS is excluded
 *     from this port entirely).
 * ========================================================================= */

MEMORY
{
  CM55M_S_APP_ROM  (rx)  : ORIGIN = 0x10000000, LENGTH = 0x40000  /* ITCM 256K */
  CM55M_S_APP_DATA (rwx) : ORIGIN = 0x30000000, LENGTH = 0x40000  /* DTCM 256K */
  CM55M_S_SRAM     (rwx) : ORIGIN = 0x3401F000, LENGTH = 0x1E1000 /* SRAM0 window */
}

__HEAP_SIZE  = 0x2000;   /* newlib heap (barely used: shell state is static) */
__STACK_SIZE = 0x6000;   /* MSP: pre-kernel main + every ISR */

ENTRY(Reset_Handler)

SECTIONS
{
    /* Vector table + privileged-code groups, SDK-identical.  The privileged /
     * syscall groups are empty in this port (no FreeRTOS, no MPU partitioning)
     * but keeping the frame costs nothing and keeps the file diffable against
     * the SDK original. */
    .table : ALIGN(4)
    {
        FILL(0xff)
        __vectors_start__ = ABSOLUTE(.) ;
        KEEP(*(.vectors))
        *(.after_vectors*)

        . = ALIGN(32);
        __privileged_functions_start__ = .;
        *(privileged_functions)
        *(privileged_functions*)
        . = ALIGN(32);
        __privileged_functions_end__ = (. - 1);

        . = ALIGN(32);
        __syscalls_flash_start__ = .;
        *(freertos_system_calls)
        *(freertos_system_calls*)
        . = ALIGN(32);
        __syscalls_flash_end__ = (. - 1);
        __unprivileged_flash_start__ = .;
    } > CM55M_S_APP_ROM

    .text : ALIGN(4)
    {
       *(.text*)
       . = ALIGN(4);
       KEEP(*(.init))
       KEEP(*(.fini));

       /* .ctors */
       *crtbegin.o(.ctors)
       *crtbegin?.o(.ctors)
       *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
       *(SORT(.ctors.*))
       *(.ctors)

       /* .dtors */
       *crtbegin.o(.dtors)
       *crtbegin?.o(.dtors)
       *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
       *(SORT(.dtors.*))
       *(.dtors)
       . = ALIGN(4);

       KEEP(*(.eh_frame*))
    } > CM55M_S_APP_ROM

    .rodata : ALIGN(4)
    {
        __rodata_start = .;
        /* The image-coherence gate reads this value (sizeof(struct cli_cmd))
         * out of the firmware; `used` only stops the COMPILER from dropping
         * it -- gc-sections needs this KEEP. */
        KEEP(*(.rodata.cli_cmd_sizeof_probe))
        *(.rodata .rodata.* .constdata .constdata.*)

        /* Shell (CLI) root command table: const cli_cmd entries registered
         * from any TU via CLI_CMD_REGISTER().  KEEP survives --gc-sections;
         * SORT_BY_NAME gives a deterministic (alphabetical) order; entries
         * carry aligned(__alignof__) so the array is gap-free and walkable by
         * pointer arithmetic between the two boundary symbols.
         *
         * [!] Embedded INSIDE .rodata, not emitted as its own output section:
         * the Himax image generator extracts allocatable sections by name and
         * silently drops names it does not know -- a standalone
         * .shell_root_cmds would produce an app that boots with an EMPTY
         * command registry.  check_image_coherence.py verifies the span lands
         * inside the image's .rodata coverage. */
        . = ALIGN(4);
        PROVIDE_HIDDEN (__cli_root_cmds_start = .);
        KEEP (*(SORT_BY_NAME(.shell_root_cmds.*)))
        KEEP (*(.shell_root_cmds))
        PROVIDE_HIDDEN (__cli_root_cmds_end = .);
        . = ALIGN(4);

        __rodata_end = .;
    } > CM55M_S_APP_ROM

    /* Exception handling/unwind -- some newlib functions use this. */
    .ARM.extab : ALIGN(4)
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > CM55M_S_APP_ROM

    .ARM.exidx : ALIGN(4)
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > CM55M_S_APP_ROM

    /* CMSIS __cmsis_start copy/zero tables: .data's initial values live in
     * ITCM and are copied to DTCM by the startup, exactly as the SDK does. */
    .copy.table :
    {
        . = ALIGN(4);
        __copy_table_start__ = .;
        LONG(LOADADDR(.data));
        LONG(    ADDR(.data));
        LONG(  SIZEOF(.data)/4);
        __copy_table_end__ = .;
    } > CM55M_S_APP_ROM

    .zero.table :
    {
        . = ALIGN(4);
        __zero_table_start__ = .;
        LONG(    ADDR(.bss));
        LONG(  SIZEOF(.bss)/4);
        __zero_table_end__ = .;
    } > CM55M_S_APP_ROM

    . = ALIGN(32);
    __unprivileged_flash_end__ = (. - 1);

    /* Main DATA section (DTCM), loaded via the copy table above. */
    .data : ALIGN(4)
    {
       FILL(0xff)
       __data_start__ = .;
       . = ALIGN(32);
       __privileged_sram_start__ = .;
       *(privileged_data)
       *(privileged_data*)
       . = ALIGN(32);
       __privileged_sram_end__ = (. - 1);
       *(vtable)
       *(.data)
       *(.data.*)
       . = ALIGN(4);
       /* preinit data */
       PROVIDE_HIDDEN (__preinit_array_start = .);
       KEEP(*(.preinit_array))
       PROVIDE_HIDDEN (__preinit_array_end = .);

       . = ALIGN(4);
       /* init data */
       PROVIDE_HIDDEN (__init_array_start = .);
       KEEP(*(SORT(.init_array.*)))
       KEEP(*(.init_array))
       PROVIDE_HIDDEN (__init_array_end = .);

       . = ALIGN(4);
       /* finit data */
       PROVIDE_HIDDEN (__fini_array_start = .);
       KEEP(*(SORT(.fini_array.*)))
       KEEP(*(.fini_array))
       PROVIDE_HIDDEN (__fini_array_end = .);

       KEEP(*(.jcr*))
       . = ALIGN(4) ;
       __data_end__ = .;
    } > CM55M_S_APP_DATA

    /* SRAM0 window: explicit placement only, empty in M-G1 (see header). */
    .pic : ALIGN(4)
    {
        *(.sdio_arena)
    } > CM55M_S_SRAM

    .bss :
    {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > CM55M_S_APP_DATA

    .noinit (NOLOAD): ALIGN(4)
    {
        _noinit = .;
        PROVIDE(__start_noinit_RAM = .) ;
        PROVIDE(__start_noinit_SRAM = .) ;
        *(.noinit*)
        . = ALIGN(4) ;
        _end_noinit = .;
        PROVIDE(__end_noinit_RAM = .) ;
        PROVIDE(__end_noinit_SRAM = .) ;
    } > CM55M_S_APP_DATA

    /* newlib heap (retarget.c _sbrk walks __HeapBase..__HeapLimit). */
    .heap (COPY) :
    {
        . = ALIGN(8);
        __HeapBase = .;
        PROVIDE(__HeapBase = .);
        end = __HeapBase;
        . = . + __HEAP_SIZE;
        . = ALIGN(8);
        __HeapLimit = .;
        PROVIDE(__HeapLimit = .);
    } > CM55M_S_APP_DATA

    /* MSP stack at the top of DTCM; startup sets MSPLIM = __StackLimit. */
    .stack (ORIGIN(CM55M_S_APP_DATA) + LENGTH(CM55M_S_APP_DATA) - __STACK_SIZE) (COPY) :
    {
        . = ALIGN(8);
        __StackLimit = .;
        PROVIDE(__StackLimit = .);
        . = . + __STACK_SIZE;
        . = ALIGN(8);
        __StackTop = .;
        PROVIDE(__StackTop = .);
    } > CM55M_S_APP_DATA
    PROVIDE(__stack = __StackTop);

    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed with stack")

    _image_start = LOADADDR(.text);
    _image_end = LOADADDR(.data) + SIZEOF(.data);
    _image_size = _image_end - _image_start;
}
