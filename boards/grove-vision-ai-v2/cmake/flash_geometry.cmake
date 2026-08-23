# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Fixed external-NOR geometry for Grove Vision AI V2 (issue #85).
#
# [!] EVERYTHING HERE IS A MEASUREMENT, NOT A KNOB.
#
# These five numbers describe the part and the resident bootloader.  They were
# CACHE STRING entries when the layout first shrank, and that was a fail-open:
# the SAME values both declare the layout and configure the check over it, so
# one -D weakened the rule and its verification together.  All four of these
# configure cleanly and pass the layout check:
#
#   -DGROVE_FW_SLOT_SIZE=0x200000   a 1.7 MB artifact is accepted as firmware,
#                                   which the bootloader refuses with ERR_IMAGE_SZ
#   -DGROVE_FW_SLOTS=1              blob starts at 0x100000, so a blob write
#                                   destroys the inactive firmware slot
#   -DGROVE_SLOT_HDR_COPIES=1       the slot-header reservation covers only
#                                   0xFFF000, putting the BACKUP header at
#                                   0xFFE000 inside blob-tail
#   -DGROVE_ERASE_GRAN=0x100        the reservation shrinks to 512 B at
#                                   0xFFFE00 so BOTH headers land inside
#                                   blob-tail, AND the destroyed-footprint
#                                   rounding starts under-reporting, because the
#                                   bootloader erases in 4 KB sectors
#   -DGROVE_FLASH_SIZE=0x2000000    the slot-header reservation moves off the
#                                   end of the real part, so BOTH real headers
#                                   land inside blob-tail
#
# [!] GROVE_ERASE_GRAN AND GROVE_SLOT_HDR_COPIES ARE TWO FACTS, NOT ONE
# (issue #88).  Until the erase granularity was measured, the slot-header
# reservation was written as "one erase block", which silently made the header
# span a function of the footprint rounding.  Tightening the rounding to the
# measured 4 KB would then have shrunk the reservation to a single sector and
# dropped the backup header into blob-tail -- exactly the -D above that this
# file already refused.  They are separate measurements now: how much a write
# destroys, and how many copies of the header the bootloader keeps.
#
# So they are plain variables, and an inherited or command-line cache entry that
# disagrees is a hard error rather than something that silently wins.  An entry
# that AGREES is accepted and dropped: a build directory configured before this
# file existed carries the same values, and making those trees refuse to
# configure would buy nothing.
#
# Host test: test/test_flash_geometry.py drives this file through a real
# `cmake` configure, because the refusal is the enforcement and a rule nobody
# has watched fail is worth as little as no rule.

# name  value  why-it-is-fixed
set(_grove_flash_geometry
    GROVE_FLASH_SIZE   0x1000000
      "the fitted 128 Mbit NOR is 16 MB, and the bootloader reports flash size[5] = FLASH_SIZE_128Mb"
    GROVE_ERASE_GRAN   0x1000
      "the 2nd bootloader erases in 4 KB sectors and only in 4 KB sectors -- measured, see below"
    GROVE_SLOT_HDR_COPIES 2
      "the 2nd bootloader keeps the slot header at flash_end-0x1000 and a backup at flash_end-0x2000"
    GROVE_FW_SLOT_SIZE 0x100000
      "the bootloader prints `Image max size 0x00100000` on every flash"
    GROVE_FW_SLOTS     2
      "the bootloader alternates two A/B slots, at 0x0 and 0x100000")

while(_grove_flash_geometry)
    list(POP_FRONT _grove_flash_geometry _g_name _g_value _g_why)

    if(DEFINED CACHE{${_g_name}})
        set(_g_got "$CACHE{${_g_name}}")
        # Compare as numbers: 0x10000 and 65536 are the same measurement.
        if(NOT _g_got MATCHES "^(0[xX][0-9a-fA-F]+|[0-9]+)$")
            message(FATAL_ERROR
                "grove: ${_g_name} is fixed board geometry, not a setting, and "
                "'${_g_got}' is not even a number.\n"
                "  ${_g_name} = ${_g_value} -- ${_g_why}.\n"
                "  Remove it from the cache (or use a fresh build directory).")
        endif()
        math(EXPR _g_got_n "${_g_got}")
        math(EXPR _g_want_n "${_g_value}")
        if(NOT _g_got_n EQUAL _g_want_n)
            message(FATAL_ERROR
                "grove: ${_g_name} is fixed board geometry, not a setting.\n"
                "  requested ${_g_got}, but it is ${_g_value} -- ${_g_why}.\n"
                "  Overriding it would move the declared layout AND the check "
                "over that layout together, which is how a blob write reaches "
                "a firmware slot or the bootloader's slot header.\n"
                "  Remove it from the cache (or use a fresh build directory).")
        endif()
        # Agrees: drop it so the plain variable below is unambiguously the value.
        unset(${_g_name} CACHE)
    endif()

    set(${_g_name} "${_g_value}")
endwhile()
unset(_g_name)
unset(_g_value)
unset(_g_why)
unset(_g_got)
unset(_g_got_n)
unset(_g_want_n)

# --- derived purely from the measurements above ------------------------------

# [!] It USED to be a cache entry, so a build directory configured before #85
# still carries the old 0xB70000.  A derived variable shadows it, which is worse
# than either alternative: the layout would be right while the cache read wrong,
# and `-DGROVE_FW_RESERVED=...` would be accepted and ignored.  Drop it.
unset(GROVE_FW_RESERVED CACHE)
math(EXPR GROVE_FW_RESERVED "${GROVE_FW_SLOT_SIZE} * ${GROVE_FW_SLOTS}"
     OUTPUT_FORMAT HEXADECIMAL)

# [!] THE LAST BLOCK IS THE BOOTLOADER'S SLOT HEADER.  Nothing may ever write it
# (issue #85).  This is not a precaution about an unidentified marker -- the 1st
# bootloader's own code says what it is:
#
#     sub.w  r0, r4, #0xC6000000   ; 0xC6000000 == -0x3A000000, so + XIP base
#     sub.w  r1, r0, #4096         ; flash_end - 0x1000
#     movs   r2, #20               ; a 20-byte record
#     bl     <read>
#     movs   r1, #18
#     bl     <checksum>            ; over the first 18 bytes
#     ldrh   r1, [r4, #18]         ; compared with the u16 at +18
#     movw/movt r1, 0x414D4948     ; "HIMA"
#     movw/movt r1, 0x32455758     ; "XWE2"
#     <on any mismatch>            ; adr -> "slot_header invalid !!"
#
# On hardware it reads, at flash_end - 0x1000 and again at flash_end - 0x2000
# (the backup the 2nd bootloader writes after a burn):
#
#     "HIMAXWE2" | u32 0x00000000 | u32 0x00000002 | u16 0x0001 | u16 checksum
#                  ^ the slot offset the boot log prints
#
# The address is COMPUTED from the flash size the bootloader detects at runtime,
# which is why no literal 0xFFF000 appears anywhere in its binary -- and the
# magic is built with movw/movt, so a byte search for it finds nothing either.
#
# Destroying it is not a hard brick: the checksum fails, the bootloader says so
# and falls back to slot 0.  But if the live image is the one in slot 1, that
# fallback silently boots the PREVIOUS build.
#
# [!] AND THE 2ND BOOTLOADER DOES NOT ONLY READ IT -- IT REWRITES BOTH COPIES
# after every burn, which is where the span comes from (issue #88).  Its own
# arithmetic, disassembled at base 0x3401F000:
#
#     erase_range(0, flash_size - 0x1000,  flash_size - 1);      write(.., 20)
#     erase_range(0, flash_size - 0x2000,  flash_size - 0x1001); write(.., 20)
#
# and erase_range() walks `addr & ~0xFFF` in 4 KB steps.  So the destroyed span
# is exactly the last TWO 4 KB sectors and nothing below them.  That is the
# reservation: GROVE_SLOT_HDR_COPIES sectors, not "one erase block" -- see the
# header of this file for why conflating the two was a fail-open.
math(EXPR GROVE_SLOT_HDR_RESERVED
     "${GROVE_ERASE_GRAN} * ${GROVE_SLOT_HDR_COPIES}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR GROVE_SLOT_HDR_ADDR "${GROVE_FLASH_SIZE} - ${GROVE_SLOT_HDR_RESERVED}"
     OUTPUT_FORMAT HEXADECIMAL)

# For the host test, and for anyone who wants the numbers without reading CMake.
if(DEFINED GROVE_FLASH_GEOMETRY_REPORT)
    file(WRITE "${GROVE_FLASH_GEOMETRY_REPORT}"
         "GROVE_FLASH_SIZE=${GROVE_FLASH_SIZE}\n"
         "GROVE_ERASE_GRAN=${GROVE_ERASE_GRAN}\n"
         "GROVE_SLOT_HDR_COPIES=${GROVE_SLOT_HDR_COPIES}\n"
         "GROVE_FW_SLOT_SIZE=${GROVE_FW_SLOT_SIZE}\n"
         "GROVE_FW_SLOTS=${GROVE_FW_SLOTS}\n"
         "GROVE_FW_RESERVED=${GROVE_FW_RESERVED}\n"
         "GROVE_SLOT_HDR_ADDR=${GROVE_SLOT_HDR_ADDR}\n"
         "GROVE_SLOT_HDR_RESERVED=${GROVE_SLOT_HDR_RESERVED}\n")
endif()
