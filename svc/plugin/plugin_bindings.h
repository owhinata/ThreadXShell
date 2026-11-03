/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_bindings.h
 * @brief   The six firmware functions a plugin can reach, in one table the
 *          board defines under a fixed name (issue #165).
 *
 * A plugin calls the firmware through six veneers (asset/common/plugin_base.c):
 * log and to_frame through the base it was handed at entry, rect, fill_rect and
 * blit through the painter draw() is handed, write through the printer report()
 * is handed.  The stack below each veneer is the firmware's, charged at the
 * declared VENEER_BASE_COST and checked against the shipped image by
 * cmake/check_veneer_base_cost.py.
 *
 * [!] THE TABLE IS WHAT THE PLUGIN IS HANDED, AND WHAT THE BUILD READS.  The
 * base at the head of @ref plugin_bindings is the very object entry() receives
 * (the loader passes &plugin_board_bindings.base, and takes no base argument),
 * and the painter and printer are filled from the table's own members by
 * plugin_painter_init() and plugin_printer_init() below, which take a context
 * from the board and no function.  So the six words of plugin_board_bindings in
 * the shipped ELF are the six functions behind the veneers, and the check
 * derives its roots from them; no list of names is stated anywhere else.
 * Until #165 the board named the six to the check by hand (ROOTS) and handed
 * the plugin whatever its code built, and the two were not tied.
 *
 * [!] WHICH WORD IS WHICH VENEER TRAVELS IN @ref plugin_bind_probe, with each
 * offset taken from the struct, so the check transcribes no layout.  The pairs
 * are held against the plugin side (which member each veneer calls through) by
 * a host test, shell/test/test_plugin_bindings.c -- a second source, since an
 * offset taken from the struct is right whatever name is written beside it.
 *
 * What this does not prove: trusted firmware code that calls a plugin's draw()
 * with a painter it built itself, without plugin_painter_init(), or that writes
 * the const table through a cast.  CLAUDE.md: the gate proves a stack bound,
 * not the absence of such code.
 */
#ifndef PLUGIN_BINDINGS_H
#define PLUGIN_BINDINGS_H

#include <stddef.h>
#include <stdint.h>

#include "plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The board's bindings.  [!] @ref base FIRST: it is handed to entry() as is,
 * and the plugin keeps that pointer and calls log / to_frame through it.
 */
struct plugin_bindings {
	struct plugin_base_api base;
	void (*rect)(void *ctx, const struct plugin_rect *r, uint16_t rgb565,
	             uint16_t stroke);
	void (*fill_rect)(void *ctx, const struct plugin_rect *r, uint16_t rgb565);
	void (*blit)(void *ctx, const struct plugin_rect *r, const uint16_t *src,
	             uint32_t src_stride, int32_t key);
	int (*write)(void *ctx, const char *s, size_t len);
};

_Static_assert(offsetof(struct plugin_bindings, base) == 0,
               "the base handed to entry() is the head of the table");
#ifndef __cplusplus
#define PLUGIN_BIND_SAME_(a, b) \
	__builtin_types_compatible_p(__typeof__(a), __typeof__(b))
_Static_assert(PLUGIN_BIND_SAME_(((struct plugin_bindings *)0)->rect,
                                 ((struct plugin_painter *)0)->rect) &&
               PLUGIN_BIND_SAME_(((struct plugin_bindings *)0)->fill_rect,
                                 ((struct plugin_painter *)0)->fill_rect) &&
               PLUGIN_BIND_SAME_(((struct plugin_bindings *)0)->blit,
                                 ((struct plugin_painter *)0)->blit) &&
               PLUGIN_BIND_SAME_(((struct plugin_bindings *)0)->write,
                                 ((struct plugin_printer *)0)->write),
               "each member has the type of the vtable member it fills");
#undef PLUGIN_BIND_SAME_
#endif

/**
 * The board's table.  Exactly one, `const`, not `static` or weak: the loader
 * and the two init functions below read it by this name, and so does the
 * build (cmake/check_veneer_base_cost.py).
 */
extern const struct plugin_bindings plugin_board_bindings;

/* ---- which word is which veneer ----------------------------------------- */

#define PLUGIN_BIND_COUNT    6u
#define PLUGIN_BIND_NAME_MAX 24u
/* "PBND" */
#define PLUGIN_BIND_PROBE_MAGIC 0x444E4250u

struct plugin_bind_field {
	uint32_t offset;                    /**< in struct plugin_bindings      */
	char     veneer[PLUGIN_BIND_NAME_MAX]; /**< NUL-terminated veneer name  */
};

struct plugin_bind_probe {
	uint32_t magic;                     /**< PLUGIN_BIND_PROBE_MAGIC        */
	uint32_t size;                      /**< sizeof(struct plugin_bindings) */
	uint32_t count;                     /**< PLUGIN_BIND_COUNT              */
	struct plugin_bind_field field[PLUGIN_BIND_COUNT];
};

/** Defined once, by svc/plugin/plugin_bindings.c; a link root. */
extern const struct plugin_bind_probe plugin_bind_probe;

/* ---- the vtables a callback is handed ------------------------------------ */

/**
 * @brief  Fill in a painter from the table: version and size first, then
 *         @p ctx (the board's drawing target) and the three primitives.
 */
void plugin_painter_init(struct plugin_painter *p, void *ctx);

/**
 * @brief  Fill in a printer from the table: version and size first, then
 *         @p ctx (where the text goes) and the write primitive.
 */
void plugin_printer_init(struct plugin_printer *o, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_BINDINGS_H */
