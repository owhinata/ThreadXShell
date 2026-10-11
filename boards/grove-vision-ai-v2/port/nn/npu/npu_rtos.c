/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_rtos.c
 * @brief   ThreadX behind the Ethos-U driver's RTOS hooks (issue #44).
 *
 * The driver ships weak implementations of its mutex and semaphore hooks and
 * expects an RTOS port to override them.  Its defaults are unusable here for
 * two independent reasons:
 *
 *   - they `malloc` the objects, and this port's rule is that permanent state
 *     is statically allocated;
 *   - `ethosu_semaphore_take()` spins on `__WFE()`, which would burn the whole
 *     core for the length of an inference.  The console would stop answering,
 *     `thread` would show 100% in a task nobody started, and Ctrl+C would not
 *     land.  With a ThreadX semaphore the inference thread SUSPENDS and the
 *     shell keeps running -- which is the difference between "the NPU is busy"
 *     and "the board is hung", and issue #40 wrongly assumed we would have to
 *     choose the latter.
 *
 * OBJECT COUNT.  The driver creates a global mutex, a global semaphore AND a
 * semaphore per registered driver, so a single static object is not enough --
 * hence a small pool.  Creation returns NULL when the pool is exhausted rather
 * than falling back to the heap, and the bring-up treats that as a hard
 * failure, because a partially-created set is exactly the half-initialised
 * state `nn` must never be able to observe.
 *
 * ISR SAFETY.  ethosu_semaphore_give() runs from the NPU interrupt.
 * tx_semaphore_put() is callable from an ISR and guards its own state with
 * TX_DISABLE/TX_RESTORE, and this port leaves TX_PORT_USE_BASEPRI undefined so
 * those are PRIMASK-based -- an ISR cannot preempt the critical section it
 * needs.  take() is only ever called from a thread.
 */
#include "npu.h"
#include "npu_hw.h"

#include "ethosu_driver.h"   /* ETHOSU_SEMAPHORE_WAIT_FOREVER */
#include "tx_api.h"

/* One driver today.  The pool is sized for the objects that driver asks for
 * (global mutex + global semaphore + its own semaphore) with one spare, so a
 * second registered NPU would fail loudly at create() rather than corrupt
 * anything. */
#define NPU_MUTEX_SLOTS 2
#define NPU_SEM_SLOTS   4

struct mutex_slot {
	TX_MUTEX mutex;
	uint8_t  used;
};

struct sem_slot {
	TX_SEMAPHORE sem;
	uint8_t      used;
};

static struct mutex_slot mutex_pool[NPU_MUTEX_SLOTS];
static struct sem_slot   sem_pool[NPU_SEM_SLOTS];

/* Claiming a slot must be atomic against the shell's other threads.  PRIMASK
 * rather than a ThreadX object, because this runs before any NPU object exists
 * and must not itself need one. */
static void *slot_claim(void *pool, size_t stride, unsigned count,
                        size_t used_off)
{
	uint8_t *base = (uint8_t *)pool;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	for (unsigned i = 0; i < count; i++) {
		uint8_t *slot = base + (size_t)i * stride;
		uint8_t *used = slot + used_off;

		if (*used == 0u) {
			*used = 1u;
			TX_RESTORE
			return slot;
		}
	}
	TX_RESTORE
	return NULL;
}

void *ethosu_mutex_create(void)
{
	struct mutex_slot *s = slot_claim(mutex_pool, sizeof(mutex_pool[0]),
	                                  NPU_MUTEX_SLOTS,
	                                  offsetof(struct mutex_slot, used));

	if (s == NULL)
		return NULL;
	/* TX_NO_INHERIT: nothing here runs at a priority where inversion matters,
	 * and inheritance would let a held NPU mutex reshuffle the shell's
	 * priorities as a side effect. */
	if (tx_mutex_create(&s->mutex, (CHAR *)"npu", TX_NO_INHERIT) != TX_SUCCESS) {
		s->used = 0u;
		return NULL;
	}
	return &s->mutex;
}

void ethosu_mutex_destroy(void *mutex)
{
	struct mutex_slot *s = (struct mutex_slot *)mutex;

	if (s == NULL)
		return;
	(void)tx_mutex_delete(&s->mutex);
	s->used = 0u;
}

int ethosu_mutex_lock(void *mutex)
{
	if (mutex == NULL)
		return -1;
	return tx_mutex_get((TX_MUTEX *)mutex, TX_WAIT_FOREVER) == TX_SUCCESS
	       ? 0 : -1;
}

int ethosu_mutex_unlock(void *mutex)
{
	if (mutex == NULL)
		return -1;
	return tx_mutex_put((TX_MUTEX *)mutex) == TX_SUCCESS ? 0 : -1;
}

void *ethosu_semaphore_create(void)
{
	struct sem_slot *s = slot_claim(sem_pool, sizeof(sem_pool[0]),
	                                NPU_SEM_SLOTS,
	                                offsetof(struct sem_slot, used));

	if (s == NULL)
		return NULL;
	/* Initial count 0, matching the driver's own default implementation: the
	 * driver gives once per registered device to seed the reservation pool. */
	if (tx_semaphore_create(&s->sem, (CHAR *)"npu", 0u) != TX_SUCCESS) {
		s->used = 0u;
		return NULL;
	}
	return &s->sem;
}

void ethosu_semaphore_destroy(void *sem)
{
	struct sem_slot *s = (struct sem_slot *)sem;

	if (s == NULL)
		return;
	(void)tx_semaphore_delete(&s->sem);
	s->used = 0u;
}

int ethosu_semaphore_take(void *sem, uint64_t timeout)
{
	ULONG wait;

	if (sem == NULL)
		return -1;

	/* The driver's timeout is an opaque scalar it passes straight through, so
	 * this port defines the unit: ThreadX ticks (1 ms).  ETHOSU_SEMAPHORE_WAIT
	 * _INFERENCE is set on the command line to a finite value -- the driver
	 * header would otherwise default it to "forever", and a lost NPU interrupt
	 * would suspend the calling shell job with no way back. */
	if (timeout == ETHOSU_SEMAPHORE_WAIT_FOREVER)
		wait = TX_WAIT_FOREVER;
	else if (timeout > 0xFFFFFFFEuLL)
		wait = 0xFFFFFFFEuL;
	else
		wait = (ULONG)timeout;

	return tx_semaphore_get((TX_SEMAPHORE *)sem, wait) == TX_SUCCESS ? 0 : -1;
}

int ethosu_semaphore_give(void *sem)
{
	if (sem == NULL)
		return -1;
	/* Called from the NPU ISR.  See the ISR SAFETY note at the top. */
	return tx_semaphore_put((TX_SEMAPHORE *)sem) == TX_SUCCESS ? 0 : -1;
}
