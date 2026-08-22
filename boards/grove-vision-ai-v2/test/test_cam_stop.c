/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the camera port's decision tables (issues #65, #72, #77,
 * port/camera/cam_state.c).
 *
 * WHY THIS EXISTS.  camera_stream_stop() has to separate three answers that all
 * look like "the camera is not streaming": the port is poisoned, there was
 * nothing to stop, and the mutex never came free.  Only the middle one may
 * report success, because by camera.h's rule success is what permits the caller
 * to detach a sink -- and a sink unlinked while a lost producer is inside it is
 * the failure the whole CAM_ST_LOST state exists to prevent.
 *
 * None of that can be produced from a console.  The dangerous case in
 * particular -- a stop that waited for the API mutex and woke up owning it with
 * the port already poisoned by ANOTHER stop's join timeout -- needs two stops
 * overlapping on a board with one shell.  So the table is a pure function and
 * this walks it.
 *
 * [!] The vector that carries the whole point is HELD + CAM_ST_LOST.  LOST is
 * also "not streaming", so the moment a "not streaming -> success" shortcut is
 * put back in front of the poison test, that line fails -- which is what this
 * file is for.
 *
 * [!] AND HERE IS WHAT IT DOES NOT COVER, because a test that is believed to
 * prove more than it does is worse than no test.  This compiles cam_state.c and
 * nothing else: camera_stream_stop() itself is never built or run here, so a
 * shortcut added AHEAD of the call, a mutex released twice, or a success
 * returned after a failed join would all leave this file green.  What holds
 * those down is that the decision has exactly one implementation and the
 * function that calls it is short -- not this test.  What this test does hold
 * down is the table itself, including the two ways it could be made to fail
 * open by someone extending an enum: see the "future member" vectors below.
 *
 * The same limit applies to the bus routing added for issue #77, and it is
 * worth naming the specific misimplementations that would pass everything here:
 *
 *   - cam_bus_enter() reading cam_state and passing it on the paths where the
 *     mutex was NOT obtained.  The table ignores it, so every vector still
 *     passes -- while the unlocked read that issue #77 is about has been put
 *     back at the one place a later edit would start believing it.
 *   - cam_bus_enter() returning a negative code without releasing a mutex it
 *     did obtain.  Reachable only on the CAM_ST_LOST row, which is exactly the
 *     row that is hardest to reach, and it leaks the API for good.
 *   - a caller testing `owner != CAM_BUS_PRODUCER` where it means
 *     `owner == CAM_BUS_DIRECT`.  Identical today; a licence to touch the bus
 *     the moment a third owner exists.
 *
 * All three are outside the table by construction -- they are in the ten lines
 * that call it.  What holds them down is the contract written on
 * cam_bus_enter() and the fact that it is the only entry, not this file.
 *
 * [!] And the fourth is the one that actually happened: an entry point that
 * never asks the table at all.  camera_stream_start() was left on the old
 * "enter, then test for STREAMING" shape when the other seven were converted,
 * and every vector below still passed -- because the table it bypasses is
 * perfectly correct.  Nothing here can see a caller that does not call.  What
 * found it was reading every entry point again afterwards, which is the only
 * thing that can -- and issue #80 was a second instance, found the same way: a
 * shell command reaching past camera.h into cam_dp.h to change datapath
 * configuration the producer consumes.  That one was invisible to a sweep for
 * I2C and bring-up because it touches no register at all.
 */
#include <stdio.h>

#include "cam_state.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

static const char *acq_name(enum cam_api_acquire a)
{
	switch (a) {
	case CAM_ACQ_HELD:        return "held";
	case CAM_ACQ_UNAVAILABLE: return "unavailable";
	case CAM_ACQ_ERROR:       return "error";
	}
	return "?";
}

static const char *owner_name(enum cam_bus_owner o)
{
	switch (o) {
	case CAM_BUS_REFUSE_STATE: return "refuse-state";
	case CAM_BUS_REFUSE_BUSY:  return "refuse-busy";
	case CAM_BUS_PRODUCER:     return "producer";
	case CAM_BUS_DIRECT:       return "direct";
	}
	return "?";
}

static const char *state_name(enum cam_state s)
{
	switch (s) {
	case CAM_ST_DOWN:      return "down";
	case CAM_ST_READY:     return "ready";
	case CAM_ST_STREAMING: return "streaming";
	case CAM_ST_FAULTED:   return "faulted";
	case CAM_ST_LOST:      return "lost";
	}
	return "?";
}

static const char *action_name(enum cam_stop_action a)
{
	switch (a) {
	case CAM_STOP_REFUSE_STATE:  return "refuse-state";
	case CAM_STOP_REFUSE_LOCKED: return "refuse-locked";
	case CAM_STOP_ALREADY:       return "already";
	case CAM_STOP_JOIN:          return "join";
	}
	return "?";
}

static void expect(enum cam_api_acquire acq, enum cam_state st,
                   enum cam_stop_action want)
{
	enum cam_stop_action got = cam_stop_decide(acq, st);

	CHECK(got == want, "decide(%s, %s) = %s, want %s",
	      acq_name(acq), state_name(st), action_name(got),
	      action_name(want));
}

/* ---- before the mutex ---------------------------------------------------- */

static void test_may_acquire(void)
{
	static const enum cam_state live[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
	};
	unsigned i;

	/* Nothing was ever created: there is no mutex to take. */
	for (i = 0u; i < sizeof live / sizeof live[0]; i++)
		CHECK(!cam_api_may_acquire(0, live[i]),
		      "may_acquire(no objects, %s) allowed the mutex",
		      state_name(live[i]));
	CHECK(!cam_api_may_acquire(0, CAM_ST_LOST),
	      "may_acquire(no objects, lost) allowed the mutex");

	/* [!] Poisoned refuses BEFORE the mutex (issue #48): a lost producer
	 * may still hold things, so a refused call must never queue. */
	CHECK(!cam_api_may_acquire(1, CAM_ST_LOST),
	      "may_acquire(lost) would have queued on the mutex");

	for (i = 0u; i < sizeof live / sizeof live[0]; i++)
		CHECK(cam_api_may_acquire(1, live[i]),
		      "may_acquire(%s) refused a usable port",
		      state_name(live[i]));
}

/* ---- after the attempt --------------------------------------------------- */

static void test_decide(void)
{
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	unsigned i;

	/*
	 * The mutex never came free.  Nothing was asked, so the state was never
	 * read under the lock and cannot be believed -- the answer must not
	 * depend on it, for ANY value, and must never be success.
	 */
	for (i = 0u; i < sizeof every / sizeof every[0]; i++)
		expect(CAM_ACQ_UNAVAILABLE, every[i], CAM_STOP_REFUSE_LOCKED);

	/* A mutex that failed for another reason is a broken kernel object,
	 * not contention: kept apart so "could not ask" stays narrow. */
	for (i = 0u; i < sizeof every / sizeof every[0]; i++)
		expect(CAM_ACQ_ERROR, every[i], CAM_STOP_REFUSE_STATE);

	/* Held: the state is now readable, and this is the real table. */
	expect(CAM_ACQ_HELD, CAM_ST_STREAMING, CAM_STOP_JOIN);
	expect(CAM_ACQ_HELD, CAM_ST_DOWN,      CAM_STOP_ALREADY);
	expect(CAM_ACQ_HELD, CAM_ST_READY,     CAM_STOP_ALREADY);
	expect(CAM_ACQ_HELD, CAM_ST_FAULTED,   CAM_STOP_ALREADY);

	/*
	 * [!] THE ONE THAT MATTERS.  Poisoned is also "not streaming", so a
	 * shortcut placed ahead of the poison test would answer ALREADY here --
	 * a confirmed stop that never happened, which the caller would act on
	 * by detaching a sink the lost producer may still be inside.
	 */
	expect(CAM_ACQ_HELD, CAM_ST_LOST, CAM_STOP_REFUSE_STATE);
}

/* ---- the property the callers actually rely on --------------------------- */

static void test_only_already_is_success(void)
{
	static const enum cam_api_acquire acqs[] = {
		CAM_ACQ_HELD, CAM_ACQ_UNAVAILABLE, CAM_ACQ_ERROR,
	};
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	unsigned a, s;

	/*
	 * camera.h: only CAM_OK permits a detach, and the stop returns CAM_OK
	 * for exactly two actions -- ALREADY, and JOIN when the producer
	 * acknowledges.  So no combination that leaves a producer possibly
	 * running may come back as ALREADY.
	 */
	for (a = 0u; a < sizeof acqs / sizeof acqs[0]; a++)
		for (s = 0u; s < sizeof every / sizeof every[0]; s++) {
			enum cam_stop_action got =
				cam_stop_decide(acqs[a], every[s]);

			if (every[s] == CAM_ST_LOST ||
			    acqs[a] != CAM_ACQ_HELD)
				CHECK(got != CAM_STOP_ALREADY,
				      "decide(%s, %s) reported a stop that "
				      "never happened",
				      acq_name(acqs[a]), state_name(every[s]));
		}
}

/* ---- the enums are going to grow ----------------------------------------- */

/*
 * [!] A MEMBER ADDED LATER MUST REFUSE, not inherit the friendliest answer.
 *
 * Both enums are the kind that grows: a second poison state, an acquisition
 * that was interrupted rather than timed out.  The dangerous default is the
 * silent one -- an unknown acquisition treated as "we hold the mutex" answers
 * from the state table, and an unknown state treated as "not streaming" answers
 * ALREADY, which is a CONFIRMED STOP.  Either would be a caller detaching a
 * sink on the strength of an enumerator nobody had thought about yet.
 *
 * The next unused value stands in for that future member.  -Wall names the file
 * when the state switch grows a member with no case; this is the other half,
 * for the value that reaches the code before anyone updates it.
 */
static void test_future_members(void)
{
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	enum cam_api_acquire future_acq = (enum cam_api_acquire)3;
	enum cam_state future_state = (enum cam_state)5;
	unsigned i;

	for (i = 0u; i < sizeof every / sizeof every[0]; i++)
		CHECK(cam_stop_decide(future_acq, every[i]) ==
		      CAM_STOP_REFUSE_STATE,
		      "an unknown acquisition + %s did not refuse",
		      state_name(every[i]));

	CHECK(cam_stop_decide(CAM_ACQ_HELD, future_state) ==
	      CAM_STOP_REFUSE_STATE,
	      "an unknown state reported a stop that never happened");
	CHECK(cam_stop_decide(CAM_ACQ_UNAVAILABLE, future_state) ==
	      CAM_STOP_REFUSE_LOCKED,
	      "an unknown state changed what a lock unavailability means");

	/*
	 * Same two halves for the bus table (issue #77), where the friendliest
	 * answer is the dangerous one: an unknown acquisition treated as "we hold
	 * it" answers from the state table, and an unknown state treated as "not
	 * streaming" answers CAM_BUS_DIRECT -- a licence to drive the sensor bus,
	 * granted on the strength of an enumerator nobody has thought about yet.
	 */
	for (i = 0u; i < sizeof every / sizeof every[0]; i++)
		CHECK(cam_bus_decide(future_acq, every[i]) ==
		      CAM_BUS_REFUSE_STATE,
		      "an unknown acquisition + %s did not refuse the bus",
		      state_name(every[i]));

	CHECK(cam_bus_decide(CAM_ACQ_HELD, future_state) ==
	      CAM_BUS_REFUSE_STATE,
	      "an unknown state was handed the sensor bus");
	CHECK(cam_bus_decide(CAM_ACQ_UNAVAILABLE, future_state) ==
	      CAM_BUS_REFUSE_BUSY,
	      "an unknown state changed what a held mutex means");

	/*
	 * cam_api_may_acquire() is deliberately NOT pinned for an unknown state.
	 * Either answer is defensible -- a second poison state would want a
	 * refusal, and anything else would not -- and a test that guessed would
	 * fail on the commit that decides correctly.  The refusal that matters
	 * is CAM_ST_LOST's, above; the fail-closed answer after the mutex is
	 * what covers the rest.
	 */
}

/* ---- who owns the sensor bus (issue #77) --------------------------------- */

static void expect_bus(enum cam_api_acquire acq, enum cam_state st,
                       enum cam_bus_owner want)
{
	enum cam_bus_owner got = cam_bus_decide(acq, st);

	CHECK(got == want, "bus_decide(%s, %s) = %s, want %s",
	      acq_name(acq), state_name(st), owner_name(got),
	      owner_name(want));
}

/*
 * WHY THIS TABLE EXISTS AT ALL.  Five entry points chose between "queue this
 * for the producer" and "write it over I2C myself" by reading cam_state BEFORE
 * taking the API mutex.  camera_stream_start() publishes CAM_ST_STREAMING while
 * HOLDING that mutex, so such a test can be overtaken by an entire stream start
 * -- and the caller then takes the by-now-free mutex, finds cam_bringup()
 * returning "already up", and drives the vendor CIS driver at the same time as
 * the producer.  The decision has to be made on the far side of the acquire.
 *
 * [!] AND IT CANNOT BE TESTED ON THE BOARD.  It needs a stream start to run to
 * completion inside the gap between two statements of another thread, and this
 * board has one console whose background jobs run BELOW the foreground shell
 * under TX_NO_TIME_SLICE -- `cmd &; cmd2` cannot even get the two threads into
 * that order (proved while verifying issue #74).  So this file is the only
 * place the table is ever exercised.
 */
static void test_bus_decide(void)
{
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	unsigned i;

	/*
	 * The mutex was not obtained, so cam_state was never read under it and
	 * the answer must not depend on it -- for ANY value.  Contention is
	 * retryable and says nothing about the port; a mutex that failed for any
	 * other reason is a broken kernel object and is NOT retryable.  Keeping
	 * them apart is what stops a corrupt mutex being reported as "busy" and
	 * sending the operator into a retry loop against a fault.
	 */
	for (i = 0u; i < sizeof every / sizeof every[0]; i++) {
		expect_bus(CAM_ACQ_UNAVAILABLE, every[i], CAM_BUS_REFUSE_BUSY);
		expect_bus(CAM_ACQ_ERROR, every[i], CAM_BUS_REFUSE_STATE);
	}

	/* Held: the state is now readable, and this is the real table. */
	expect_bus(CAM_ACQ_HELD, CAM_ST_STREAMING, CAM_BUS_PRODUCER);
	expect_bus(CAM_ACQ_HELD, CAM_ST_DOWN,      CAM_BUS_DIRECT);
	expect_bus(CAM_ACQ_HELD, CAM_ST_READY,     CAM_BUS_DIRECT);
	expect_bus(CAM_ACQ_HELD, CAM_ST_FAULTED,   CAM_BUS_DIRECT);

	/*
	 * [!] THE ONE THAT MATTERS, and it is NOT the same hazard as the stop's.
	 *
	 * There is no ordering to get wrong here: LOST and STREAMING are distinct
	 * enumerators, so no amount of swapping equality tests can confuse them.
	 * What fails open is a WIDER test -- `st != CAM_ST_STREAMING -> direct`
	 * reads identically on every state anyone has thought about and hands
	 * CAM_BUS_DIRECT to a poisoned port.
	 *
	 * And a poisoned port IS reachable with the mutex held, even though the
	 * poison test runs before the acquire (issue #48): a caller passes that
	 * test while a stream runs, is preempted, a stop takes the mutex, fails
	 * its join, writes CAM_ST_LOST and releases -- and the caller then
	 * acquires and reads it.  Direct there means cam_bringup(), which accepts
	 * only READY and STREAMING as already-up, TEARING THE PORT DOWN AND
	 * REBUILDING IT under a producer that never acknowledged a stop.  That is
	 * the single action CAM_ST_LOST exists to prevent.
	 */
	expect_bus(CAM_ACQ_HELD, CAM_ST_LOST, CAM_BUS_REFUSE_STATE);
}

/* ---- the property the bus callers actually rely on ----------------------- */

static void test_only_named_states_may_act(void)
{
	static const enum cam_api_acquire acqs[] = {
		CAM_ACQ_HELD, CAM_ACQ_UNAVAILABLE, CAM_ACQ_ERROR,
	};
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	unsigned a, s;

	/*
	 * CAM_BUS_DIRECT is the permission to drive a driver with no locking of
	 * its own, on a bus another thread may own.  So it is the answer worth
	 * stating as a property rather than only as a row: it may be reached ONLY
	 * with the mutex held, and ONLY for a state somebody named.
	 */
	for (a = 0u; a < sizeof acqs / sizeof acqs[0]; a++)
		for (s = 0u; s < sizeof every / sizeof every[0]; s++) {
			enum cam_bus_owner got =
				cam_bus_decide(acqs[a], every[s]);
			int may = (acqs[a] == CAM_ACQ_HELD) &&
			          (every[s] == CAM_ST_DOWN ||
			           every[s] == CAM_ST_READY ||
			           every[s] == CAM_ST_FAULTED);

			CHECK((got == CAM_BUS_DIRECT) == may,
			      "bus_decide(%s, %s) = %s: direct is %s here",
			      acq_name(acqs[a]), state_name(every[s]),
			      owner_name(got), may ? "required" : "forbidden");

			/* The producer route hands work over instead of doing
			 * it, so it too must never be reached without the
			 * mutex -- a queue write raced against the producer's
			 * claim is a lost update, not a refusal. */
			if (acqs[a] != CAM_ACQ_HELD)
				CHECK(got != CAM_BUS_PRODUCER,
				      "bus_decide(%s, %s) queued without the "
				      "mutex", acq_name(acqs[a]),
				      state_name(every[s]));
		}
}

/*
 * The drain verdict (issue #72).
 *
 * Neither failing vector can be produced on hardware.  A thread that never comes
 * back needs a wedged panel; a thread that comes back still holding a pipeline
 * slot needs a consume() that returns without putting -- which is a bug, not an
 * input.  So the only place the two are ever distinguished is here.
 *
 * [!] They MUST stay distinguished.  "The thread is somewhere unknown" and "the
 * thread is idle and the ring is one slot short" are different diagnoses with
 * different next steps, and collapsing them into "drain failed" is what left the
 * second one invisible until a later stream ran out of slots.
 */
static void test_drain_verdict(void)
{
	CHECK(cam_drain_decide(0, 0) == CAM_DRAIN_DONE,
	      "a clean drain holding nothing must release");

	/* A thread that never came back: the count is a reading of bookkeeping a
	 * live thread is still moving, so it must not be reported as the fault
	 * whatever it says. */
	CHECK(cam_drain_decide(-1, 0) == CAM_DRAIN_THREAD,
	      "a failed drain with a zero count is still a thread failure");
	CHECK(cam_drain_decide(-1, 1) == CAM_DRAIN_THREAD,
	      "a failed drain must not be reported as a leaked pin");
	CHECK(cam_drain_decide(-1, 99) == CAM_DRAIN_THREAD,
	      "the thread failure outranks any count");

	/* Idle, but holding: the count has stopped moving, so this is real. */
	CHECK(cam_drain_decide(0, 1) == CAM_DRAIN_PINNED,
	      "one unreleased slot must be caught");
	CHECK(cam_drain_decide(0, 7) == CAM_DRAIN_PINNED,
	      "several unreleased slots must be caught");

	/* Nothing but DONE may let the owner release what the sink reads. */
	CHECK(cam_drain_decide(1, 0) != CAM_DRAIN_DONE,
	      "a positive drain code is not success");
}

int main(void)
{
	test_may_acquire();
	test_decide();
	test_only_already_is_success();
	test_bus_decide();
	test_only_named_states_may_act();
	test_future_members();
	test_drain_verdict();

	if (failures != 0) {
		printf("test_cam_stop: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_stop: ok\n");
	return 0;
}
