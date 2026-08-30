/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_abi.h
 * @brief   The wire ABI for a model post-processing plugin shipped with its
 *          model (issue #101 = #78 Step 1a).
 *
 * A model can be swapped on the device without reflashing (issues #92/#93/#94
 * built the asset store), but the code that INTERPRETS its output cannot: the
 * firmware holds exactly one decoder.  The store scales to eleven slots and the
 * interpretation stops at one.  A container carries the model and the code that
 * reads it together, so a new model family costs no firmware change.
 *
 * [!] THIS HEADER IS THE ONLY DECLARATION OF THE FORMAT.  Three independent
 * artifact graphs consume it -- the host packer, the device decoder, and the
 * plugin linker/gate -- and each one that re-derived the layout could agree with
 * itself and disagree with the other two.  The packer GENERATES its field table
 * from this file rather than mirroring a struct format of its own, the same way
 * the blob slot capacity table is generated from the board's slot map rather
 * than scraped out of its C source.
 *
 * [!] STEP 1a NEVER EXECUTES A PLUGIN.  Nothing here yields a callable pointer:
 * every entry point is an OFFSET, and plugin_load.c returns those offsets as
 * integers inside a POD snapshot.  Turning one into a function pointer is Step
 * 1b's job, after checks this header does not contain (MPU attributes, cache
 * maintenance, the admission policy for the stack allowances).
 *
 * [!] THE GATE THAT CHECKS A PLUGIN PROVES NEITHER MEMORY SAFETY NOR THE RANGE
 * IN WHICH IT USES THE POINTERS THE BASE HANDS IT.  A plugin is REVIEWED,
 * TRUSTED NATIVE CODE with the same standing as board code -- not sandboxed
 * content.  See AGENTS.md; do not read the gate as an isolation boundary.
 *
 * WIRE RULES, WITHOUT EXCEPTION.  Every multi-byte field is unsigned, of fixed
 * width, and little-endian.  Every reserved field and every padding byte is
 * zero, and a decoder REFUSES a non-zero one rather than ignoring it: a field
 * that may hold junk today cannot be given a meaning tomorrow.  The
 * _Static_asserts at the end of this file pin sizeof and offsetof so that a
 * compiler which lays the structs out differently fails the build instead of
 * producing a firmware that disagrees with the packer.
 */
#ifndef PLUGIN_ABI_H
#define PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- version ------------------------------------------------------------- */

/**
 * The ABI this header describes.
 *
 * Compared for EXACT EQUALITY, never as a range.  Forward compatibility would
 * mean a device deciding it understands enough of a newer container to run it,
 * and the thing it would be guessing about is which bytes to execute.
 */
#define PLUGIN_ABI_VERSION      1u

/* ---- the container ------------------------------------------------------- */

/**
 * Bytes 0..3 of a container: "NNC1".
 *
 * [!] CHOSEN SO THAT IT CANNOT BE MISTAKEN FOR A BARE MODEL, AND VICE VERSA.  A
 * .tflite begins with a little-endian root-table offset at bytes 0..3 and the
 * file identifier "TFL3" at bytes 4..7.  Read as a u32 this magic is
 * 0x31434E4E -- every ASCII quadruple is at least 0x20202020, which is far past
 * the 4 MiB largest blob slot, so no payload the store can hold could carry a
 * root offset that collides with it.  The format word below is the second,
 * independent discriminator: a payload whose bytes 4..7 are "TFL3" is a bare
 * model and takes the pre-container path unchanged.
 */
#define PLUGIN_CONTAINER_MAGIC0 'N'
#define PLUGIN_CONTAINER_MAGIC1 'N'
#define PLUGIN_CONTAINER_MAGIC2 'C'
#define PLUGIN_CONTAINER_MAGIC3 '1'

/** Bytes 4..7 of a container: "PLGC".  Never "TFL3" -- see above. */
#define PLUGIN_CONTAINER_FORMAT0 'P'
#define PLUGIN_CONTAINER_FORMAT1 'L'
#define PLUGIN_CONTAINER_FORMAT2 'G'
#define PLUGIN_CONTAINER_FORMAT3 'C'

/** The identifier a bare .tflite carries at bytes 4..7. */
#define PLUGIN_TFLITE_IDENT0    'T'
#define PLUGIN_TFLITE_IDENT1    'F'
#define PLUGIN_TFLITE_IDENT2    'L'
#define PLUGIN_TFLITE_IDENT3    '3'

/**
 * Section kinds.
 *
 * Cardinality is fixed and checked: exactly one MODEL, at most one PLUGIN, at
 * most one DATA.  A container with no PLUGIN is legal and useful -- a label
 * table travelling with a model needs no code -- but two of anything is a
 * refusal, not a "last one wins".
 *
 * Value 0 is deliberately unassigned so that a zeroed table entry is refused.
 */
enum plugin_section_type {
	PLUGIN_SECTION_NONE   = 0,
	PLUGIN_SECTION_MODEL  = 1,
	PLUGIN_SECTION_PLUGIN = 2,
	PLUGIN_SECTION_DATA   = 3,
};

/** Most sections a container may declare.  Three kinds, one each. */
#define PLUGIN_SECTION_MAX      3u

/**
 * One section of a container.
 *
 * @ref offset is from the start of the CONTAINER, not from the section table,
 * so that a reader never has to add two things it validated separately.
 */
struct plugin_section {
	uint32_t type;       /**< enum plugin_section_type; 0 is refused       */
	uint32_t offset;     /**< from container byte 0                        */
	uint32_t length;     /**< bytes                                        */
	uint32_t reserved;   /**< MUST be 0                                    */
};

/**
 * The container header, at byte 0 of a blob payload.
 *
 * [!] THE DIGEST COVERS THE PLUGIN SECTION AND SITS OUTSIDE IT.  It is here, in
 * the header, rather than inside the image it describes, so that computing it
 * needs no canonicalisation step -- no "hash with this field zeroed" rule for
 * the packer and the device to implement differently.  The blob layer already
 * has its own CRC over the whole payload (issue #92); this one answers a
 * different question, namely whether the plugin image is the one the packer
 * gated, and it is CRC-32 for the same reason that one is: svc/crc32.c is
 * already linked into every board.
 *
 * [!] AND IT IS NOT A SIGNATURE.  A digest shows that the bytes that arrived
 * are the bytes that were sent.  It says nothing about PROVENANCE -- that the
 * image went through the gate at all.  What carries provenance is the process
 * constraint that the official packer runs the gate and assembles the container
 * indivisibly.  Do not describe this field as making a hand-written container
 * unrunnable, because it does not.
 */
struct plugin_container_hdr {
	uint8_t  magic[4];        /**< "NNC1"                                  */
	uint8_t  format[4];       /**< "PLGC"                                  */
	uint32_t hdr_size;        /**< == sizeof(struct plugin_container_hdr)  */
	uint32_t abi_version;     /**< == PLUGIN_ABI_VERSION, exact            */
	uint32_t total_size;      /**< whole container; == blob payload length */
	uint32_t section_count;   /**< 1..PLUGIN_SECTION_MAX                   */
	uint32_t plugin_digest;   /**< CRC-32 of the PLUGIN section; 0 if none */
	uint32_t reserved;        /**< MUST be 0                               */
	struct plugin_section sections[PLUGIN_SECTION_MAX];
};

/**
 * Alignment the MODEL section's offset must satisfy.
 *
 * The Ethos-U command stream is read from where the model lies, and the payload
 * walk that hands it over requires a 4-byte aligned address (port/npu/
 * npu_payload.c).  A container is copied nowhere before that walk, so the
 * alignment has to be true of the offset inside the container.
 */
#define PLUGIN_MODEL_ALIGN      4u

/**
 * Alignment the PLUGIN load image's base and END must satisfy.
 *
 * Cache maintenance for loaded code is by address and rounds outward to whole
 * lines, which are 32 bytes on this part.  Aligning only the start would let
 * the rounding at the far end reach into whatever sits after the reservation.
 */
#define PLUGIN_IMAGE_ALIGN      32u

/* ---- the target --------------------------------------------------------- */

/**
 * Instruction-set and calling-convention identity, as one word.
 *
 * [!] PACKED, NOT HASHED.  The plan called for one opaque word compared for
 * exact equality; a hash would satisfy that and then tell nobody WHICH
 * attribute differed when a container is refused.  These are bit fields instead,
 * so the comparison is still a single u32 equality and the refusal can still
 * name the mismatch.  The composition lives here, in the one header both the
 * packer and the device include, so the two cannot compute it differently.
 *
 * The link address is NOT folded in.  It is a full 32-bit value and would not
 * survive being packed alongside anything else; it is carried separately in the
 * manifest and compared against the board's policy in its own right.
 */
enum plugin_cpu {
	PLUGIN_CPU_NONE       = 0,
	PLUGIN_CPU_CORTEX_M7  = 1,
	PLUGIN_CPU_CORTEX_M55 = 2,
};

enum plugin_fpu {
	PLUGIN_FPU_NONE       = 0,
	PLUGIN_FPU_FPV5_SP_D16 = 1,   /**< f746g-disco                        */
	PLUGIN_FPU_FPV5_D16    = 2,   /**< wio-lite-ai                        */
	PLUGIN_FPU_FP_ARMV8    = 3,   /**< grove-vision-ai-v2 (cortex-m55)    */
};

enum plugin_float_abi {
	PLUGIN_FLOAT_ABI_SOFT = 0,
	PLUGIN_FLOAT_ABI_HARD = 1,
};

#define PLUGIN_TARGET_CPU_SHIFT       0
#define PLUGIN_TARGET_CPU_MASK        0x000000FFu
#define PLUGIN_TARGET_FPU_SHIFT       8
#define PLUGIN_TARGET_FPU_MASK        0x00000F00u
#define PLUGIN_TARGET_FLOAT_SHIFT     12
#define PLUGIN_TARGET_FLOAT_MASK      0x00003000u
#define PLUGIN_TARGET_BIG_ENDIAN      0x00004000u
#define PLUGIN_TARGET_CMSE            0x00008000u

/**
 * Compose a target word.  Both the packer and the device call this.
 *
 * Bits above CMSE are reserved and stay zero, so a container built by a future
 * packer that sets one is refused here rather than silently accepted with the
 * bit ignored.
 */
static inline uint32_t plugin_target_id(unsigned cpu, unsigned fpu,
                                        unsigned float_abi, int big_endian,
                                        int cmse)
{
	return (((uint32_t)cpu << PLUGIN_TARGET_CPU_SHIFT) & PLUGIN_TARGET_CPU_MASK)
	     | (((uint32_t)fpu << PLUGIN_TARGET_FPU_SHIFT) & PLUGIN_TARGET_FPU_MASK)
	     | (((uint32_t)float_abi << PLUGIN_TARGET_FLOAT_SHIFT)
	        & PLUGIN_TARGET_FLOAT_MASK)
	     | (big_endian ? PLUGIN_TARGET_BIG_ENDIAN : 0u)
	     | (cmse ? PLUGIN_TARGET_CMSE : 0u);
}

/** Bits a decoder must see as zero. */
#define PLUGIN_TARGET_RESERVED_MASK   0xFFFF0000u

/* ---- the plugin manifest ------------------------------------------------ */

/**
 * Entry points a plugin exports, as slot indices.
 *
 * [!] THE TABLE IS FROZEN HERE, AND THE PROTOTYPES WITH IT.  The stack gate has
 * to recognise a "typed call site" in the linked plugin, which it can only do
 * if the slot list and its signatures are fixed before any of the three artifact
 * graphs is written.  Adding a slot later is an ABI break, which is why the
 * console and panel halves are both present from the start even though Step 1a
 * calls neither.
 */
enum plugin_slot {
	PLUGIN_SLOT_ENTRY     = 0,  /**< init; receives the base vtable       */
	PLUGIN_SLOT_SHAPES_OK = 1,  /**< can this plugin read these tensors?  */
	PLUGIN_SLOT_DECODE    = 2,  /**< tensors -> the plugin's private form */
	PLUGIN_SLOT_DRAW      = 3,  /**< paint the last decode onto the panel */
	PLUGIN_SLOT_REPORT    = 4,  /**< describe the last decode to a writer */
	PLUGIN_SLOT_PARAM_SET = 5,
	PLUGIN_SLOT_PARAM_GET = 6,
	PLUGIN_SLOT_COUNT     = 7,
};

/**
 * The canonical value of an absent optional callback.
 *
 * [!] ZERO, AND ZERO IS NOT A VALID OFFSET EITHER.  The image begins with its
 * own code, so offset 0 could in principle be a function; making absence a
 * distinct sentinel and ALSO refusing 0 as a live offset means a truncated or
 * zero-filled manifest cannot present a callable-looking entry.  ENTRY, DECODE
 * and SHAPES_OK are mandatory and must not be absent.
 */
#define PLUGIN_SLOT_ABSENT      0u

/** Longest plugin name, NUL-terminated, NUL-padded to the full field. */
#define PLUGIN_NAME_MAX         32u
/** Build identifier, NUL-terminated, NUL-padded.  Printed in fault reports. */
#define PLUGIN_BUILD_ID_MAX     32u

/** Magic at the start of a plugin section: "PMAN". */
#define PLUGIN_MANIFEST_MAGIC0  'P'
#define PLUGIN_MANIFEST_MAGIC1  'M'
#define PLUGIN_MANIFEST_MAGIC2  'A'
#define PLUGIN_MANIFEST_MAGIC3  'N'

/**
 * What a plugin section declares about itself, ahead of its image.
 *
 * [!] NON-EXECUTABLE, AND VALIDATED IN FULL BEFORE ANYTHING BRANCHES.  An
 * earlier design had the plugin return this from its entry point, which meant
 * running unverified code to find out whether the code was worth running.  It
 * is data now: everything a loader must know is here, in fixed-width fields it
 * can bounds-check before the first instruction is fetched.
 *
 * THE LOAD IMAGE.  The file side is [code (text+rodata)][data initialiser] and
 * memory adds [bss][scratch] after it, so @ref file_size is code+data and
 * @ref mem_size is code+data+bss+scratch.  @ref code_off / @ref code_len are
 * carried rather than inferred: "everything that is not data or bss" would
 * include rodata and inter-section padding, and an entry point is only allowed
 * to land in executable bytes.
 *
 * STACK.  Each callback runs on a DIFFERENT THREAD'S STACK, and they are not
 * the same size: decode on the camera producer, draw on the panel thread inside
 * its guard, report on the shell thread.  The panel stack in particular is
 * small.  A self-declared number is not evidence, so the host gate derives a
 * transitive bound from the linked plugin and refuses a manifest that does not
 * match it -- see plugin_load.h for what the loader does with these, and
 * AGENTS.md for why matching is still not the same as being admissible.
 */
struct plugin_manifest {
	uint8_t  magic[4];          /**< "PMAN"                                */
	uint32_t struct_size;       /**< == sizeof(struct plugin_manifest)     */
	uint32_t abi_version;       /**< == PLUGIN_ABI_VERSION, exact          */
	uint32_t target_id;         /**< plugin_target_id(), exact             */
	uint32_t link_addr;         /**< prelink base; policy compares it      */
	uint32_t capability;        /**< bits; see PLUGIN_CAP_*                */

	uint32_t image_off;         /**< image start, from the section start   */
	uint32_t file_size;         /**< code + data initialiser               */
	uint32_t mem_size;          /**< code + data + bss + scratch           */

	uint32_t code_off;          /**< all offsets below are into the image  */
	uint32_t code_len;
	uint32_t data_off;
	uint32_t data_len;
	uint32_t bss_off;
	uint32_t bss_len;
	uint32_t scratch_off;
	uint32_t scratch_len;

	uint32_t slot[PLUGIN_SLOT_COUNT];   /**< offsets; Thumb bit in bit 0   */
	uint32_t stack[PLUGIN_SLOT_COUNT];  /**< bytes, per slot               */

	uint8_t  name[PLUGIN_NAME_MAX];
	uint8_t  build_id[PLUGIN_BUILD_ID_MAX];
	uint32_t reserved[4];       /**< MUST be 0                             */
};

/**
 * Optional capabilities a plugin declares.
 *
 * A bit set here and a slot left absent (or the reverse) is a refusal: the two
 * are two spellings of the same fact, and a loader that accepted a disagreement
 * would be choosing which one to believe.
 */
#define PLUGIN_CAP_DRAW         0x00000001u   /**< PLUGIN_SLOT_DRAW present  */
#define PLUGIN_CAP_REPORT       0x00000002u   /**< PLUGIN_SLOT_REPORT        */
#define PLUGIN_CAP_PARAMS       0x00000004u   /**< PARAM_SET and PARAM_GET   */
#define PLUGIN_CAP_KNOWN_MASK   0x00000007u

/* ---- layout pins -------------------------------------------------------- */

/*
 * [!] THESE ARE THE ABI.  A compiler that pads differently, or a field reordered
 * in a refactor, breaks the build here rather than producing a firmware whose
 * idea of the format differs from the packer's by a few bytes.  The packer
 * generates its own field table from this file, so the numbers below are the
 * only place the layout is asserted.
 */
_Static_assert(sizeof(struct plugin_section) == 16,
               "plugin_section is wire format: 4 x u32, no padding");
_Static_assert(sizeof(struct plugin_container_hdr) == 32 + 3 * 16,
               "plugin_container_hdr is wire format");
_Static_assert(offsetof(struct plugin_container_hdr, magic) == 0,
               "the magic must be at byte 0 to discriminate against .tflite");
_Static_assert(offsetof(struct plugin_container_hdr, format) == 4,
               "the format word must be at byte 4, where .tflite keeps TFL3");
_Static_assert(offsetof(struct plugin_container_hdr, sections) == 32,
               "the section table follows the fixed header");

_Static_assert(sizeof(struct plugin_manifest) ==
               24 + 12 + 32 + 4 * PLUGIN_SLOT_COUNT * 2 +
               PLUGIN_NAME_MAX + PLUGIN_BUILD_ID_MAX + 16,
               "plugin_manifest is wire format");
_Static_assert(offsetof(struct plugin_manifest, magic) == 0,
               "a manifest starts with its magic");
_Static_assert(PLUGIN_SLOT_COUNT == 7,
               "the slot table is frozen; adding one is an ABI break");
_Static_assert((PLUGIN_IMAGE_ALIGN & (PLUGIN_IMAGE_ALIGN - 1u)) == 0u,
               "image alignment is used as a mask");

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_ABI_H */
