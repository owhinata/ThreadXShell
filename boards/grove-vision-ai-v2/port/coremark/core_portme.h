/*
 * CoreMark port for Grove Vision AI V2 (Himax HX6538, Cortex-M55 @ 400 MHz,
 * bare-metal ThreadX shell app).  Timing uses the 1 ms ThreadX tick
 * (tx_time_get); output uses printf over the UART0 console (retargeted by the
 * shell backend's _write).
 * Derived from EEMBC's barebones core_portme.h (Apache-2.0).
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>

/* Platform capabilities */
#define HAS_FLOAT   1   /* secs_ret is double; needs float-enabled printf   */
#define HAS_TIME_H  0   /* no <time.h>                                      */
#define USE_CLOCK   0   /* custom timing in core_portme.c                   */
#define HAS_STDIO   1   /* <stdio.h> available                              */
#define HAS_PRINTF  1   /* maps ee_printf -> printf (retargeted to UART0)   */

/* Report strings.  A CoreMark score is only comparable against another run
 * that used the same memory method, the same placement and the same compiler
 * flags -- so state them here and in the board README rather than letting the
 * number travel alone.  -fno-tree-vectorize is not cosmetic: -mcpu=cortex-m55
 * enables MVE, and the ThreadX M55 port does not save/restore VPR across a
 * context switch, so predicated MVE must not reach the image (the build's
 * check_mve_predication.py gate enforces that). */
#ifndef COMPILER_VERSION
#ifdef __GNUC__
#define COMPILER_VERSION "GCC" __VERSION__
#else
#define COMPILER_VERSION "unknown"
#endif
#endif
#ifndef FLAGS_STR
#define FLAGS_STR "-O3 -funroll-loops -fno-tree-vectorize " \
                  "-mcpu=cortex-m55 -mfloat-abi=hard (scalar; code+data in TCM)"
#endif
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS FLAGS_STR
#endif
#ifndef MEM_LOCATION
#define MEM_LOCATION "STATIC (DTCM .bss)"
#endif

/* Data types (ee_ptr_int must hold a pointer) */
typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef ee_u32         ee_ptr_int;
typedef size_t         ee_size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

/* Timing return type */
#define CORETIMETYPE ee_u32
typedef ee_u32 CORE_TICKS;

#ifndef SEED_METHOD
#define SEED_METHOD SEED_VOLATILE
#endif

/* MEM_STATIC: the working set is a 2000-byte .bss array in DTCM.  The newlib
 * heap on this board is 8 KB total (ldscript __HEAP_SIZE), so asking malloc for
 * a quarter of it on every run -- the wio port's MEM_MALLOC choice -- would be
 * a poor trade here; DTCM has ~180 KB spare.  Set on the command line by
 * board.cmake; this is the fallback so the header stands alone. */
#ifndef MEM_METHOD
#define MEM_METHOD MEM_STATIC
#endif

#ifndef MULTITHREAD
#define MULTITHREAD 1   /* MEM_STATIC requires exactly one context */
#define USE_PTHREAD 0
#define USE_FORK    0
#define USE_SOCKET  0
#endif

#ifndef MAIN_HAS_NOARGC
#define MAIN_HAS_NOARGC 1   /* no argc/argv on bare metal */
#endif

#ifndef MAIN_HAS_NORETURN
#define MAIN_HAS_NORETURN 0
#endif

/* Must be 1 for this simple single-context port */
extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S
{
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) \
    && !defined(VALIDATION_RUN)
#if (TOTAL_DATA_SIZE == 1200)
#define PROFILE_RUN 1
#elif (TOTAL_DATA_SIZE == 2000)
#define PERFORMANCE_RUN 1
#else
#define VALIDATION_RUN 1
#endif
#endif

int ee_printf(const char *fmt, ...);

#endif /* CORE_PORTME_H */
