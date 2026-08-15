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

    /* CMSIS __cmsis_start copy/zero tables, kept SDK-identical.
     *
     * [!] The copy table is a NO-OP here, and it is worth knowing why before
     * anyone relies on it.  .data is placed with `> CM55M_S_APP_DATA` and no
     * AT> clause, so its LMA equals its VMA -- there is no staging copy in
     * ITCM.  LOADADDR(.data) == ADDR(.data) == 0x30000000, which makes the
     * entry below a memcpy of .data onto itself.  What actually fills DTCM is
     * the 2nd-stage bootloader, which loads each section at its VMA (the image
     * generator records .data with vma 0x30000000).
     *
     * That is also the evidence that a LOADABLE section with a DTCM VMA works
     * on this part: .data is one, on every boot.  The zero table is real work
     * -- .bss is NOBITS and nothing else clears it. */
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

    /* membench's ITCM working set (issue #25).  NOLOAD: it must occupy ITCM
     * address space without adding 4 KB of zeros to the flashed image, and
     * NOBITS sections carry no CONTENTS so check_image_coherence.py does not
     * expect to find them in the generated .img.  Placed after every
     * PROGBITS section in this region, so the reservation lands in the ITCM
     * TAIL and cannot move code, rodata or the copy/zero tables.
     *
     * KEEP because --gc-sections would otherwise drop the buffer as unused
     * (nothing outside cmd_membench.c references it), and the point of a
     * dedicated section is that check_placement_budget.py can assert the
     * measurement buffer really is IN the memory whose name it prints. */
    .itcm_bench (NOLOAD) : ALIGN(32)
    {
        __itcm_bench_start = .;
        KEEP(*(.itcm_bench))
        __itcm_bench_end = .;
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

    /* TEMPORARY (issue #43): DTCM half of the loadable-placement probe.
     * LOADABLE, unlike every other section this port adds -- that is the whole
     * point.  DTCM itself is already proven (see the .copy.table note above:
     * .data is a loadable DTCM section on every boot); what this tests is the
     * remaining unknown, that the image generator carries an ADDITIONAL
     * section whose NAME it has never seen.  The generator drops names it does
     * not know, which is why the command registry hides inside .rodata.
     * check_image_coherence.py fails the build if it gets dropped, so that
     * half of the answer arrives without flashing.
     * Removed once #29 has chosen the destination. */
    .dtcm_probe : ALIGN(32)
    {
        __dtcm_probe_start = .;
        KEEP(*(.dtcm_probe))
        __dtcm_probe_end = .;
    } > CM55M_S_APP_DATA

    /* SRAM0 window: explicit placement only (see header).  Both sections here
     * are content-free -- .pic has no members yet, and the membench buffer is
     * NOLOAD -- so this region still contributes nothing to the image. */
    .pic : ALIGN(4)
    {
        *(.sdio_arena)
    } > CM55M_S_SRAM

    /* membench's SRAM0 working set (issue #25).  64 KB, the only bench buffer
     * big enough to overflow the L1 D-cache, and the only user of the SRAM0
     * window in this milestone.  NOLOAD + KEEP for the same reasons as
     * .itcm_bench above. */
    .sram_bench (NOLOAD) : ALIGN(32)
    {
        __sram_bench_start = .;
        KEEP(*(.sram_bench))
        __sram_bench_end = .;
    } > CM55M_S_SRAM

    /* The ST7789 framebuffer (issue #30).  240x320 RGB565 = 150 KB, in SRAM
     * because that is the memory the DMA controller reaches -- ITCM and DTCM
     * are CPU-private on this part, and a framebuffer placed there would not
     * fault, it would simply never appear on the panel.
     *
     * NOLOAD for the same reason as the bench buffers, but the stake is bigger:
     * 150 KB of zeros in the image would be 150 KB written to the external NOR
     * on every flash, on the part this project deliberately does not wear out.
     * Because it is NOLOAD its contents survive a reset and start undefined --
     * every producer writes every pixel it claims.  KEEP so --gc-sections
     * cannot drop the reservation and let something else land on top.
     * check_placement_budget.py pins symbol -> size -> section -> SRAM. */
    .lcd_fb (NOLOAD) : ALIGN(32)
    {
        __lcd_fb_start = .;
        KEEP(*(.lcd_fb))
        __lcd_fb_end = .;
    } > CM55M_S_SRAM

    /* The camera's WDMA3 landing buffer (issue #35).  320x240 planar B/G/R =
     * 225 KB, written by the datapath's write-DMA and read by the CPU one
     * frame at a time.  In SRAM for the same reason as the framebuffer above --
     * the DMA engines cannot see TCM -- and 32-byte aligned because the CPU has
     * to invalidate the whole buffer before reading each frame, and an
     * invalidate that begins mid-cache-line either misses bytes or discards a
     * neighbour's. */
    .cam_raw (NOLOAD) : ALIGN(32)
    {
        __cam_raw_start = .;
        KEEP(*(.cam_raw))
        __cam_raw_end = .;
    } > CM55M_S_SRAM

    /* The frame pipeline's slots (issue #35).  Two 320x240 RGB565 frames, the
     * converted output the LCD sink consumes.  Two and not one because
     * frame_pipeline_acquire() will not hand back the slot it last published --
     * that is what keeps a pull reader's frame from being recycled underneath
     * it, and with a single slot the second acquire would fail for ever. */
    .cam_slots (NOLOAD) : ALIGN(32)
    {
        __cam_slots_start = .;
        KEEP(*(.cam_slots))
        __cam_slots_end = .;
    } > CM55M_S_SRAM

    /* TEMPORARY (issue #43): SRAM half of the loadable-placement probe, and
     * the real subject of the experiment.  Every other section in this window
     * is NOLOAD, so nothing loadable has EVER been placed in SRAM on this
     * board -- and the 2nd-stage bootloader executes out of the bottom of this
     * same window (from ORIGIN, spanning BOOT2NDLOADER_SIZE = 0x18000) while it
     * runs, which is why the SDK's own NN script starts loadable SRAM no lower
     * than 0x3404D000.
     *
     * Placed last, after every NOLOAD reservation, so it lands around
     * 0x340D7C20 -- above the loader reserve, and representative of where a
     * relocated .rodata would actually sit.  Before __sram_end so the
     * placement gate's high-water invariant still covers it.
     * Removed once #29 has chosen the destination. */
    .sram_probe : ALIGN(32)
    {
        __sram_probe_start = .;
        KEEP(*(.sram_probe))
        __sram_probe_end = .;
    } > CM55M_S_SRAM

    /* High-water mark of the SRAM window, for the `free` command (issue #26).
     * MUST stay after every section assigned to CM55M_S_SRAM -- the location
     * counter here still belongs to that region, and the next output section
     * switches it back to DTCM.  Order-sensitive by nature, so
     * check_placement_budget.py fails the build if any allocated section ever
     * extends past this symbol; that is what stops the report going stale the
     * way it did when .sram_bench was added and `free` kept printing 0. */
    __sram_end = .;

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

    /* membench's DTCM working set (issue #25).  NOLOAD + KEEP, same reasoning
     * as .itcm_bench; placed before .heap so it only pushes the heap base up
     * (DTCM has ~180 KB of slack between __HeapLimit and __StackLimit, which
     * check_placement_budget.py enforces a floor on). */
    .dtcm_bench (NOLOAD) : ALIGN(32)
    {
        __dtcm_bench_start = .;
        KEEP(*(.dtcm_bench))
        __dtcm_bench_end = .;
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
