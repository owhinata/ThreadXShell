/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the thread->instance registry rules (issue #81,
 * shell/core/cli_registry.c).
 *
 * This test is the ONLY thing that can check these rules.  cli_register_thread()
 * and cli_unregister_thread() are TX_DISABLE wrappers in cli_core.c, which
 * cannot be compiled on the host (ThreadX services, an MRS on IPSR), and every
 * refusal below is unreachable from healthy firmware: no caller registers a
 * thread or an instance twice.  A test can build the damaged table by hand,
 * which is what keeps this from being a gate nobody has seen fail.
 *
 *   1. add into an empty table, and a second distinct pair,
 *   2. every refusal, and the FIXED priority when the conditions overlap:
 *      bad arg -> dup thread -> dup instance -> full,
 *   3. a refusal never touches the table (byte-for-byte),
 *   4. remove clears both fields, leaves other entries byte-for-byte alone,
 *      and is a no-op for NULL / an absent thread,
 *   5. remove SWEEPS: a hand-built table with the same thread twice comes back
 *      with neither, which is the postcondition first-match-break could not
 *      give,
 *   6. add reuses a slot freed by remove,
 *   7. occupancy is `sh`: a slot with a stale thread and no instance is free,
 *      is reused (repairing it) and duplicates nothing -- pinned from three
 *      angles because that is the one place the rules could be read two ways.
 *
 * Not checked here, deliberately: the publish-last / retract-first store order.
 * A unit test observes only the state on return, so that ordering is an
 * implementation rule held by review (see cli_registry.h), not a tested one.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_registry.h"

#define SLOTS 4

static TX_THREAD           threads[SLOTS + 2];
static struct cli_instance insts[SLOTS + 2];

/* Byte-for-byte comparison of a whole table: "the refusal changed nothing" is
 * about the bytes, not about the entries a getter would report. */
static int same_table(const struct cli_thread_map *a,
                      const struct cli_thread_map *b, size_t n)
{
	return memcmp(a, b, n * sizeof *a) == 0;
}

static size_t count_used(const struct cli_thread_map *m, size_t n)
{
	size_t i, used = 0;
	for (i = 0; i < n; i++)
		if (m[i].sh != NULL)
			used++;
	return used;
}

int main(void)
{
	struct cli_thread_map map[SLOTS];
	struct cli_thread_map before[SLOTS];
	enum cli_reg_status   st;
	size_t                n;

	/* 1. empty table -> first slot; a second distinct pair -> next slot */
	memset(map, 0, sizeof map);
	st = cli_reg_add(map, SLOTS, &threads[0], &insts[0]);
	assert(st == CLI_REG_OK);
	assert(map[0].thread == &threads[0] && map[0].sh == &insts[0]);

	st = cli_reg_add(map, SLOTS, &threads[1], &insts[1]);
	assert(st == CLI_REG_OK);
	assert(map[1].thread == &threads[1] && map[1].sh == &insts[1]);
	assert(count_used(map, SLOTS) == 2);

	/* 2a. NULL arguments are refused before anything else is examined */
	memcpy(before, map, sizeof map);
	assert(cli_reg_add(map, SLOTS, NULL, &insts[2]) == CLI_REG_ERR_ARG);
	assert(cli_reg_add(map, SLOTS, &threads[2], NULL) == CLI_REG_ERR_ARG);
	assert(cli_reg_add(map, SLOTS, NULL, NULL) == CLI_REG_ERR_ARG);
	assert(same_table(map, before, SLOTS));

	/* 2b. the same PAIR again: refused as a duplicate THREAD, not instance --
	 *     it matches both tests, and the fixed order decides. */
	assert(cli_reg_add(map, SLOTS, &threads[0], &insts[0]) == CLI_REG_ERR_DUP_THREAD);
	assert(same_table(map, before, SLOTS));

	/* 2c. same thread, different instance -> duplicate thread */
	assert(cli_reg_add(map, SLOTS, &threads[0], &insts[3]) == CLI_REG_ERR_DUP_THREAD);
	assert(same_table(map, before, SLOTS));

	/* 2d. different thread, same instance -> duplicate instance */
	assert(cli_reg_add(map, SLOTS, &threads[3], &insts[0]) == CLI_REG_ERR_DUP_INSTANCE);
	assert(same_table(map, before, SLOTS));

	/* 2e. full table */
	assert(cli_reg_add(map, SLOTS, &threads[2], &insts[2]) == CLI_REG_OK);
	assert(cli_reg_add(map, SLOTS, &threads[3], &insts[3]) == CLI_REG_OK);
	assert(count_used(map, SLOTS) == SLOTS);
	memcpy(before, map, sizeof map);
	assert(cli_reg_add(map, SLOTS, &threads[4], &insts[4]) == CLI_REG_ERR_FULL);
	assert(same_table(map, before, SLOTS));

	/* 2f. FULL *and* duplicate must report the duplicate.  Deciding from the
	 *     first match, or bailing out at the first free-slot scan, would call
	 *     this "full" and send the reader after the wrong thing. */
	assert(cli_reg_add(map, SLOTS, &threads[1], &insts[4]) == CLI_REG_ERR_DUP_THREAD);
	assert(cli_reg_add(map, SLOTS, &threads[4], &insts[1]) == CLI_REG_ERR_DUP_INSTANCE);
	assert(same_table(map, before, SLOTS));

	/* 2g. a damaged table holding BOTH kinds of duplicate in DIFFERENT entries:
	 *     the answer must not depend on which entry the scan met first.  Build
	 *     it so the instance duplicate sits earlier than the thread one. */
	memset(map, 0, sizeof map);
	map[0].thread = &threads[0]; map[0].sh = &insts[0];   /* matches sh   below */
	map[1].thread = &threads[1]; map[1].sh = &insts[1];   /* matches thread below */
	memcpy(before, map, sizeof map);
	assert(cli_reg_add(map, SLOTS, &threads[1], &insts[0]) == CLI_REG_ERR_DUP_THREAD);
	assert(same_table(map, before, SLOTS));
	/* ...and with the entry order swapped, the same answer */
	memset(map, 0, sizeof map);
	map[0].thread = &threads[1]; map[0].sh = &insts[1];
	map[1].thread = &threads[0]; map[1].sh = &insts[0];
	memcpy(before, map, sizeof map);
	assert(cli_reg_add(map, SLOTS, &threads[1], &insts[0]) == CLI_REG_ERR_DUP_THREAD);
	assert(same_table(map, before, SLOTS));

	/* 2h. OCCUPANCY IS `sh`.  A slot holding a stale thread with no instance is
	 *     FREE -- it registers nothing, so it duplicates nothing.  Only
	 *     corruption can produce that pair (cli_reg_remove clears both), and
	 *     reusing the slot repairs it; refusing would keep the damage and fail
	 *     cli_start() for ever.  Pinned from three angles so the rule cannot be
	 *     read the other way. */
	memset(map, 0, sizeof map);
	map[0].thread = &threads[0];          /* stale thread, sh == NULL */
	assert(cli_reg_add(map, SLOTS, &threads[0], &insts[0]) == CLI_REG_OK);
	assert(map[0].thread == &threads[0] && map[0].sh == &insts[0]);
	assert(count_used(map, SLOTS) == 1);  /* healed, not duplicated */

	/*     ...the instance IS registered elsewhere, the thread is not: the
	 *     answer is the instance, and that does not violate the fixed order --
	 *     there is no thread duplicate to outrank it. */
	memset(map, 0, sizeof map);
	map[0].thread = &threads[0];                          /* free (sh NULL) */
	map[1].thread = &threads[1]; map[1].sh = &insts[1];   /* occupied */
	memcpy(before, map, sizeof map);
	assert(cli_reg_add(map, SLOTS, &threads[0], &insts[1]) == CLI_REG_ERR_DUP_INSTANCE);
	assert(same_table(map, before, SLOTS));

	/*     ...but an OCCUPIED entry with that thread still outranks it. */
	memset(map, 0, sizeof map);
	map[0].thread = &threads[0];                          /* free (sh NULL) */
	map[1].thread = &threads[1]; map[1].sh = &insts[1];   /* instance match */
	map[2].thread = &threads[0]; map[2].sh = &insts[2];   /* thread match */
	memcpy(before, map, sizeof map);
	assert(cli_reg_add(map, SLOTS, &threads[0], &insts[1]) == CLI_REG_ERR_DUP_THREAD);
	assert(same_table(map, before, SLOTS));

	/* 3. remove: clears both fields of the match, leaves the rest untouched */
	memset(map, 0, sizeof map);
	assert(cli_reg_add(map, SLOTS, &threads[0], &insts[0]) == CLI_REG_OK);
	assert(cli_reg_add(map, SLOTS, &threads[1], &insts[1]) == CLI_REG_OK);
	assert(cli_reg_add(map, SLOTS, &threads[2], &insts[2]) == CLI_REG_OK);

	n = cli_reg_remove(map, SLOTS, &threads[1]);
	assert(n == 1);
	assert(map[1].thread == NULL && map[1].sh == NULL);
	assert(map[0].thread == &threads[0] && map[0].sh == &insts[0]);
	assert(map[2].thread == &threads[2] && map[2].sh == &insts[2]);

	/* 4. no-ops: NULL, and a thread that is not in the table */
	memcpy(before, map, sizeof map);
	assert(cli_reg_remove(map, SLOTS, NULL) == 0);
	assert(cli_reg_remove(map, SLOTS, &threads[5]) == 0);
	assert(same_table(map, before, SLOTS));

	/* 5. add reuses the hole rather than appending */
	assert(cli_reg_add(map, SLOTS, &threads[3], &insts[3]) == CLI_REG_OK);
	assert(map[1].thread == &threads[3] && map[1].sh == &insts[3]);

	/* 6. THE SWEEP.  Hand-build the damage cli_reg_add() now refuses to create:
	 *    the same thread in two entries.  First-match-break would clear one and
	 *    leave an orphan holding a slot for a thread that is gone -- which is
	 *    the whole defect of issue #81.  The postcondition is that NO entry has
	 *    that thread, so both go, and the bystander is untouched. */
	memset(map, 0, sizeof map);
	map[0].thread = &threads[0]; map[0].sh = &insts[0];
	map[1].thread = &threads[1]; map[1].sh = &insts[1];   /* bystander */
	map[2].thread = &threads[0]; map[2].sh = &insts[0];   /* the duplicate */

	n = cli_reg_remove(map, SLOTS, &threads[0]);
	assert(n == 2);
	assert(map[0].thread == NULL && map[0].sh == NULL);
	assert(map[2].thread == NULL && map[2].sh == NULL);
	assert(map[1].thread == &threads[1] && map[1].sh == &insts[1]);
	/* the postcondition, stated as the code states it */
	{
		size_t i;
		for (i = 0; i < SLOTS; i++)
			assert(map[i].thread != &threads[0]);
	}

	/* ...and the same thread bound to DIFFERENT instances still sweeps: remove
	 * matches on the thread, which is why the thread is the key. */
	memset(map, 0, sizeof map);
	map[0].thread = &threads[0]; map[0].sh = &insts[0];
	map[1].thread = &threads[0]; map[1].sh = &insts[1];
	assert(cli_reg_remove(map, SLOTS, &threads[0]) == 2);
	assert(count_used(map, SLOTS) == 0);

	/* 7. every status renders a reason, including a value from outside the enum */
	assert(cli_reg_strerror(CLI_REG_OK) != NULL);
	assert(cli_reg_strerror(CLI_REG_ERR_ARG) != NULL);
	assert(cli_reg_strerror(CLI_REG_ERR_DUP_THREAD) != NULL);
	assert(cli_reg_strerror(CLI_REG_ERR_DUP_INSTANCE) != NULL);
	assert(cli_reg_strerror(CLI_REG_ERR_FULL) != NULL);
	assert(cli_reg_strerror((enum cli_reg_status)-99) != NULL);

	/* 8. the failure codes are negative and distinct, which is what lets the
	 *    existing `!= 0` call sites stay correct while carrying a reason. */
	assert(CLI_REG_OK == 0);
	assert(CLI_REG_ERR_ARG < 0 && CLI_REG_ERR_DUP_THREAD < 0);
	assert(CLI_REG_ERR_DUP_INSTANCE < 0 && CLI_REG_ERR_FULL < 0);
	assert(CLI_REG_ERR_ARG != CLI_REG_ERR_DUP_THREAD);
	assert(CLI_REG_ERR_DUP_THREAD != CLI_REG_ERR_DUP_INSTANCE);
	assert(CLI_REG_ERR_DUP_INSTANCE != CLI_REG_ERR_FULL);

	printf("test_registry: all assertions passed\n");
	return 0;
}
