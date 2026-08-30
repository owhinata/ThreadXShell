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
 * The app is NOT XIP.  The 2nd-stage bootloader loads each ELF section at its
 * VMA (there are no AT> clauses in this file, so LMA == VMA throughout):
 *   CM55M_S_APP_ROM   ITCM  256 KB @ 0x10000000 (secure alias) -- vectors+code
 *   CM55M_S_APP_DATA  DTCM  256 KB @ 0x30000000                -- data+stack
 *   CM55M_S_SRAM_LDR  SRAM  188 KB @ 0x3401F000 -- the window the 2nd-stage
 *                     bootloader itself executes from.  NOLOAD ONLY.
 *   CM55M_S_SRAM      SRAM  1.7 MB @ 0x3404D000 -- .rodata plus the NOLOAD
 *                     buffers.  The only region here that may hold LOADABLE
 *                     content; see the MEMORY block for why the floor is
 *                     where it is.
 * Placement in both SRAM regions is EXPLICIT ONLY -- an ITCM overflow is a
 * link error, never a silent spill.
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

  /* The SRAM window is split in two, and the split is the point (issue #29).
   *
   * The 2nd-stage bootloader EXECUTES from the bottom of this window while it
   * is loading us: it lives at BOOT2NDLOADER_BASE (0x3401F000) spanning
   * BOOT2NDLOADER_SIZE (0x18000), i.e. up to 0x34037000.  A LOADABLE section
   * placed there would have the loader writing over the code it is running.
   * The SDK's own NN script rounds that hazard up to 0x3404D000 and starts its
   * loadable SRAM there; this port adopts the same floor.
   *
   * Expressing it as two regions rather than as a comment plus careful
   * ordering is deliberate.  Placement here used to be safe only because the
   * section order happened to be right -- exactly the silent failure #29 warned
   * about -- and "put your section after the other ones" is not a property a
   * reviewer can check at a glance.  Now the region name carries the rule, and
   * check_placement_budget.py enforces the half a linker script cannot: that no
   * section with CONTENTS lands below the floor. */
  CM55M_S_SRAM_LDR (rwx) : ORIGIN = 0x3401F000, LENGTH = 0x2E000  /* NOLOAD ONLY */
  CM55M_S_SRAM     (rwx) : ORIGIN = 0x3404D000, LENGTH = 0x1B3000 /* loadable OK */
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

    /* .rodata lives in SRAM, not ITCM (issue #29).  This is the whole point of
     * the region split above: it hands ~30 KB of ITCM back for code, which is
     * what M-G3c's TFLM needs, and it is the answer the SDK's own NN linker
     * script arrived at (Algo_NoTrustZone.ld puts .rodata in SRAM0).
     *
     * It is placed FIRST in the region so its address is the region base and
     * does not drift every time one of the NOLOAD buffers below changes size.
     *
     * [!] This is the only LOADABLE section in the SRAM window, which is why
     * #43 had to prove on hardware that the 2nd-stage bootloader actually
     * writes one: every other section here is NOLOAD, so nothing had ever
     * exercised that path.  It does (16 KB probes at 0x340d7c20 and in DTCM
     * both matched the host-computed CRC32 of their ELF bytes).
     *
     * The stake is the command registry embedded below: it rides along inside
     * .rodata precisely because the image generator drops output section names
     * it does not recognise, and an empty registry would take away the console
     * that would report it.  check_image_coherence.py byte-verifies the section
     * reached the .img and that the registry span lies inside it. */
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
    } > CM55M_S_SRAM

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
     * check_placement_budget.py pins symbol -> size -> section -> SRAM.
     *
     * It is the resident of the loader window (issue #29).  Something had to
     * be, or the 188,416 B the 2nd bootloader reserves would simply be lost,
     * and the framebuffer is the best-shaped candidate: NOLOAD, so the loader
     * never writes it; a fixed 150 KB, so it fits with room to spare; and
     * needed only by the SPI DMA long after the loader has finished. */
    .lcd_fb (NOLOAD) : ALIGN(32)
    {
        __lcd_fb_start = .;
        KEEP(*(.lcd_fb))
        __lcd_fb_end = .;
    } > CM55M_S_SRAM_LDR

    /* High-water mark of the loader window, for `free` (issue #26's rule
     * applied to the second region).  Same order-sensitivity as __sram_end. */
    __sram_ldr_end = .;

    /* The camera's WDMA3 landing buffers (issues #35, #59).  TWO of 320x240
     * planar B/G/R = 225 KB each, 450 KB in all: the write-DMA fills one while
     * the CPU reads the other, which is what took the active frame time off
     * the preview's critical path.  In SRAM for the same reason as the
     * framebuffer above -- the DMA engines cannot see TCM -- and 32-byte
     * aligned because the CPU invalidates one buffer before reading each
     * frame, and an invalidate that begins mid-cache-line either misses bytes
     * or discards a neighbour's; the row stride is a whole number of lines
     * (asserted in cam_dp.c) so the invalidate can never touch the buffer the
     * DMA is writing next door. */
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


    /* The NN tensor arena (issue #44).  450 KB, the size the SDK's own
     * MobileNet classification app uses for this model.
     *
     * In SRAM because the Ethos-U55 is an AXI bus master and the arena is the
     * memory it and the CPU share.  Whether the NPU can reach CM55M TCM at all
     * is UNVERIFIED -- the Ethos-U55 TRM does not describe this SoC's
     * interconnect and the SVD describes registers, not routing -- so the
     * reason to put it here is that the donor app does, not a proof that the
     * alternative fails.  Same failure mode as the camera and LCD buffers if
     * that reasoning is wrong: no fault, just a transfer that never happens.
     *
     * NOLOAD: it is scratch, it is 450 KB, and flashing that many zeros to the
     * external NOR every time would be a waste of a part with ~100k cycles.
     * The interpreter writes every byte it reads.
     *
     * ALIGN(32) and a 32-byte multiple in size: the CPU cleans and invalidates
     * this range around every inference, and maintenance that starts or ends
     * mid-cache-line either misses bytes or discards a neighbour's.  The
     * Ethos-U driver documents the same requirement for the base pointers it
     * is handed. */
    .nn_arena (NOLOAD) : ALIGN(32)
    {
        __nn_arena_start = .;
        KEEP(*(.nn_arena))
        __nn_arena_end = .;
    } > CM55M_S_SRAM

    /* The blob staging buffer (issue #92).  64 KB between a YMODEM transfer
     * and the NOR: bytes accumulate until a chunk is full and then one write
     * transaction takes the whole chunk, because every transaction takes the
     * memory-mapped window down and back up and writes the part's non-volatile
     * status register twice on the way.
     *
     * [!] IN SRAM FOR THE BUDGET, NOT FOR REACHABILITY -- unlike .lcd_fb,
     * .cam_raw and .cam_slots above, nothing hands this buffer to a DMA
     * engine.  nor_write.c copies out of it a program page at a time.  It is
     * here because 64 KB is a large bite out of the ~152 KB heap-to-stack gap
     * in DTCM and a small one out of this window.
     *
     * NOLOAD and KEEP for the same reasons as its neighbours: zeros in the
     * image are NOR wear, and a reservation --gc-sections dropped would let
     * the next section take the address the placement gate says is reserved. */
    .blob_stage (NOLOAD) : ALIGN(32)
    {
        __blob_stage_start = .;
        KEEP(*(.blob_stage))
        __blob_stage_end = .;
    } > CM55M_S_SRAM

    /* High-water mark of the SEQUENTIALLY PLACED part of the SRAM window, for
     * the `free` command (issue #26).  MUST stay after every bump-placed
     * section assigned to CM55M_S_SRAM -- the location counter here still
     * belongs to that region.  Order-sensitive by nature, so
     * check_placement_budget.py fails the build if any such section ever
     * extends past this symbol; that is what stops the report going stale the
     * way it did when .sram_bench was added and `free` kept printing 0.
     *
     * [!] RENAMED FROM __sram_end IN ISSUE #101, BECAUSE IT NO LONGER MEANS
     * "the end of SRAM usage".  .plugin below is pinned to a fixed address at
     * the TOP of the window, so the region now has two occupied spans with a
     * growth gap between them, and a single high-water mark cannot describe
     * that.  Reading the old name would have made `free` count the gap as used
     * and the gate reject the reservation for sitting past the mark. */
    __sram_seq_end = .;

    /*
     * The plugin load image's reservation (issue #101 = #78 Step 1a).
     *
     * [!] A FIXED ABSOLUTE ADDRESS, AND THAT IS THE POINT.  A plugin is
     * PRELINKED for this base -- the loader services no relocations -- so the
     * address is part of the ABI that the host packer and the plugin's own link
     * both bake in.  Placed sequentially it would move the day .rodata or
     * .nn_arena grew, and every plugin ever built would silently target the
     * wrong window.  Anchoring at the TOP of the region instead leaves the
     * sequential sections above free to grow into the gap below it.
     *
     * NOLOAD: the image arrives over the wire at run time, so there is nothing
     * to put in the boot image.  check_image_coherence.py only carries
     * ALLOC+CONTENTS sections, so this reservation never reaches the image
     * generator.
     *
     * The size is 128 KiB as an opening number: the shared decoder links to
     * ~2.5 KB, and the rest is data, bss and a band-sized raster scratch --
     * a full-frame raster is deliberately NOT provided for, because the panel
     * callback's per-draw budget means a full frame is painted in bands anyway.
     * Step 1b revisits it against the first real plugin's measured sizes.
     *
     * [!] THE END MATTERS AS MUCH AS THE START.  Cache maintenance for loaded
     * code is by address and rounds outward to whole 32-byte lines, so a
     * reservation whose end were ragged would let that rounding reach into
     * whatever followed it.  Both edges are 32-byte multiples here, and
     * plugin_load.c refuses a mem_size that is not.
     */
    .plugin 0x341E0000 (NOLOAD) :
    {
        __plugin_start = .;
        . += 0x20000;
        __plugin_end = .;
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

    /* The loader window must stay free of anything the loader would write.
     * A cheap first line only -- ld can check the ONE section this file knows
     * about, but it cannot tell a NOLOAD section from a loadable one, so it
     * cannot state the actual rule.  check_placement_budget.py does that by
     * reading the section flags out of the linked ELF, which is also why this
     * ASSERT is not the thing to reach for when adding a section here. */
    ASSERT(__sram_ldr_end <= ORIGIN(CM55M_S_SRAM_LDR) + LENGTH(CM55M_S_SRAM_LDR),
           "loader window overflowed -- .lcd_fb no longer fits below 0x3404D000")
    ASSERT(__rodata_start >= ORIGIN(CM55M_S_SRAM),
           ".rodata must not start below the loadable-SRAM floor")

    /* The growth gap has not been consumed (issue #101).  The sequential
     * sections grow upward and .plugin is pinned at the top, so this is the one
     * thing ld itself can state about the two of them: the day a buffer grows
     * past 0x341E0000 the build stops here rather than overlapping a
     * reservation whose address is baked into every plugin ever linked. */
    ASSERT(__sram_seq_end <= __plugin_start,
           "SRAM sequential sections grew into the .plugin reservation")
    ASSERT(__plugin_end == ORIGIN(CM55M_S_SRAM) + LENGTH(CM55M_S_SRAM),
           ".plugin must be anchored at the top of the SRAM window")

    /* The SDK's scripts define _image_start/_image_end/_image_size here.  They
     * are NOT carried over (issue #29 audit): nothing reads them -- not the
     * CMSIS startup, not we2_local_image_gen, not any gate -- and they were
     * already meaningless before .rodata moved, because .data has no AT>
     * clause so LOADADDR(.data) is its DTCM address and the subtraction
     * spanned two disjoint memories (_image_size read 0x20000610, ~537 MB).
     * Moving .rodata to SRAM makes the image genuinely non-contiguous, so
     * keeping a symbol that looks like a size but is not would be a trap. */
}
