/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Link script for a Grove plugin image (issue #101 = #78 Step 1a).
 *
 * [!] PRELINKED AT AN ABSOLUTE ADDRESS.  The loader services no relocations and
 * builds no veneers, so this image must already be correct for the one place it
 * will ever live: the .plugin reservation the firmware's own script pins at
 * 0x341E0000.  The two scripts state that address independently -- a plugin that
 * read it out of the firmware would agree with whatever it was handed, and
 * check_placement_budget.py exists precisely to catch the day they diverge.
 *
 * THE IMAGE IS FLAT, AND ITS SHAPE IS THE ABI'S.  svc/plugin_abi.h defines the
 * file side as [code (text+rodata)][data initialiser] with [bss][scratch]
 * following in memory only, so the sections are laid out in exactly that order
 * and the boundary symbols below are what the packer turns into the manifest's
 * code/data/bss/scratch offsets.  Reordering anything here changes the format.
 */
MEMORY
{
  PLUGIN (rwx) : ORIGIN = 0x341E0000, LENGTH = 128K
}

ENTRY(plugin_slot_table)

SECTIONS
{
    .text :
    {
        __plugin_code_start = .;
        *(.text .text.*)
        *(.rodata .rodata.*)

        /* [!] KEEP, BECAUSE NOTHING IN THE IMAGE REFERS TO IT.  The loader
         * reaches the entry points through the manifest's offsets, so
         * --gc-sections would drop the table and every function it names.  It is
         * also what the packer reads to fill those offsets in. */
        . = ALIGN(4);
        __plugin_slots_start = .;
        KEEP(*(.plugin_slots))
        __plugin_slots_end = .;

        . = ALIGN(4);
        __plugin_code_end = .;
    } > PLUGIN

    .data : ALIGN(4)
    {
        __plugin_data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __plugin_data_end = .;
    } > PLUGIN

    /* End of what travels in the container. */
    __plugin_file_end = .;

    .bss (NOLOAD) : ALIGN(4)
    {
        __plugin_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __plugin_bss_end = .;
    } > PLUGIN

    /*
     * mem_size must be a whole number of cache lines: maintenance for loaded
     * code is by address and rounds outward to 32-byte lines, so a ragged end
     * would let that rounding reach past what the plugin owns.  Rounding up HERE
     * rather than in the packer keeps the one declaration of the image's shape
     * in the script that produced it.  plugin_load.c refuses a mem_size that is
     * not a multiple, so a script that dropped this line fails on the device.
     */
    . = ALIGN(32);
    __plugin_mem_end = .;

    /*
     * [!] ONLY .comment IS DISCARDED, AND THE REST IS THE GATE'S JOB.
     *
     * An earlier revision discarded .init_array, .ARM.exidx, .ARM.extab and
     * .eh_frame here as well, with a comment claiming the host gate would check
     * for the same shapes anyway.  That was exactly backwards: a section thrown
     * away by the linker is not in the ELF, so the gate could never see one and
     * its whole section whitelist was decoration.  The fixture that was supposed
     * to prove the gate rejects unwind tables passed instead -- caught only
     * because the negative test asserted a REJECTION rather than a clean run.
     *
     * So they are left in place.  A plugin that grows a constructor array or an
     * unwind table now reaches check_plugin_image.py and is refused by name,
     * which is both louder and true.  .comment carries no semantics and is
     * dropped because it would otherwise bloat every image with toolchain
     * version strings.
     */
    /DISCARD/ :
    {
        *(.comment)
    }

    ASSERT(__plugin_mem_end <= ORIGIN(PLUGIN) + LENGTH(PLUGIN),
           "plugin image does not fit the .plugin reservation")
    ASSERT(__plugin_slots_end > __plugin_slots_start,
           "the slot table was garbage-collected -- KEEP is not working")
}
