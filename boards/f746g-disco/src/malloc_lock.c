/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    malloc_lock.c
 * @brief   Thread-safe newlib heap: back __malloc_lock/__malloc_unlock with a
 *          ThreadX mutex.
 *
 * The stock newlib-nano __malloc_lock/__malloc_unlock forward to the retargetable
 * lock hooks (__retarget_lock_acquire/release_recursive), which are no-op `bx lr`
 * stubs on this build -- so malloc/free/realloc/mallinfo (and printf's %f float
 * conversion, which allocates a scratch buffer) are NOT thread-safe.  This
 * firmware has more than one heap user in more than one thread: `membench`
 * allocates its SRAM row, `free` reads mallinfo, CoreMark's report prints %f, and
 * any of them can run in a background-job worker while another console runs the
 * next one.  Concurrent allocations race the arena and corrupt it.
 *
 * Provide strong __malloc_lock/__malloc_unlock so the linker resolves malloc's
 * reference here instead of pulling libc's mlock.o, and serialise every heap
 * operation on a single priority-inheriting mutex.
 *
 * POLICY: the newlib heap must NOT be used from ISR or ThreadX timer-callback
 * context.  The guard below only SKIPS the lock there (a mutex cannot be taken
 * from an ISR); it does NOT make an ISR malloc safe.  Today no ISR touches the
 * heap (USART1 -> the CLI rings, SysTick -> HAL_IncTick + the tx timer, DMA/DCMI
 * -> preallocated buffers, the fault path logs to the DTCM ring), so this holds.
 *
 * Clean-room glue; adapted from boards/wio-lite-ai/src/malloc_lock.c.
 */
#include "tx_api.h"
#include "bsp.h"

/* newlib passes a struct _reent * we ignore; forward-declare to avoid <reent.h>. */
struct _reent;

static TX_MUTEX      g_heap_mutex;
static volatile UINT g_heap_lock_ready;

/*
 * Called from tx_application_define(), i.e. single-threaded and before the
 * scheduler runs.
 *
 * FAIL-STOP on tx_mutex_create() failure, which is where this differs from the
 * wio-lite-ai original (issue #8 carries the same change back).  Creating a
 * statically allocated mutex before the scheduler can only fail on a caller
 * error -- a corrupted control block, a double create -- so continuing would
 * mean running the rest of the firmware with an unserialised heap that LOOKS
 * fine and corrupts its arena under concurrency.  Stopping in Error_Handler()
 * turns that into the one symptom nobody can miss.
 */
void malloc_lock_init(void)
{
	if (tx_mutex_create(&g_heap_mutex, "heap", TX_INHERIT) != TX_SUCCESS)
		Error_Handler();
	g_heap_lock_ready = 1u;
}

/* True only in genuine post-scheduler THREAD context.  IPSR==0 excludes ISR/exception
 * context: the Cortex-M7/GNU port leaves _tx_thread_current_ptr pointing at the
 * interrupted thread during an ISR, so tx_thread_identify() alone cannot detect it.
 * tx_thread_identify()!=TX_NULL then excludes the pre-scheduler init context
 * (tx_application_define), where tx_mutex_get() would return TX_CALLER_ERROR.  Before
 * the scheduler runs everything is single-threaded, so skipping the lock is safe. */
static inline int heap_lock_usable(void)
{
	unsigned int ipsr;

	__asm__ volatile("MRS %0, IPSR" : "=r"(ipsr));
	return (g_heap_lock_ready != 0u) && (ipsr == 0u) &&
	       (tx_thread_identify() != TX_NULL);
}

void __malloc_lock(struct _reent *r)
{
	(void)r;
	if (heap_lock_usable())
		(void)tx_mutex_get(&g_heap_mutex, TX_WAIT_FOREVER);
}

void __malloc_unlock(struct _reent *r)
{
	(void)r;
	if (heap_lock_usable())
		(void)tx_mutex_put(&g_heap_mutex);
}
