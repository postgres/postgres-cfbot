/*-------------------------------------------------------------------------
 *
 * s_lock.c
 *	   Implementation of spinlocks.
 *
 * When waiting for a contended spinlock we loop tightly for awhile, then
 * delay using pg_usleep() and try again.  Preferably, "awhile" should be a
 * small multiple of the maximum time we expect a spinlock to be held.  100
 * iterations seems about right as an initial guess.  However, on a
 * uniprocessor the loop is a waste of cycles, while in a multi-CPU scenario
 * it's usually better to spin a bit longer than to call the kernel, so we try
 * to adapt the spin loop count depending on whether we seem to be in a
 * uniprocessor or multiprocessor.
 *
 * Note: you might think MIN_SPINS_PER_DELAY should be just 1, but you'd
 * be wrong; there are platforms where that can result in a "stuck
 * spinlock" failure.  This has been seen particularly on Alphas; it seems
 * that the first TAS after returning from kernel space will always fail
 * on that hardware.
 *
 * Once we do decide to block, we use randomly increasing pg_usleep()
 * delays. The first delay is 1 msec, then the delay randomly increases to
 * about one second, after which we reset to 1 msec and start again.  The
 * idea here is that in the presence of heavy contention we need to
 * increase the delay, else the spinlock holder may never get to run and
 * release the lock.  (Consider situation where spinlock holder has been
 * nice'd down in priority by the scheduler --- it will not get scheduled
 * until all would-be acquirers are sleeping, so if we always use a 1-msec
 * sleep, there is a real possibility of starvation.)  But we can't just
 * clamp the delay to an upper bound, else it would take a long time to
 * make a reasonable number of tries.
 *
 * We time out and declare error after NUM_DELAYS delays (thus, exactly
 * that many tries).  With the given settings, this will usually take 2 or
 * so minutes.  It seems better to fix the total number of tries (and thus
 * the probability of unintended failure) than to fix the total time
 * spent.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/s_lock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "common/pg_prng.h"
#include "port/atomics.h"
#include "port/spin_delay.h"
#include "storage/spin.h"
#include "utils/wait_event.h"

#define MIN_SPINS_PER_DELAY 10
#define MAX_SPINS_PER_DELAY 1000
#define NUM_DELAYS			1000
#define MIN_DELAY_USEC		1000L
#define MAX_DELAY_USEC		1000000L

static int	spins_per_delay = DEFAULT_SPINS_PER_DELAY;


/*
 * s_lock_stuck() - complain about a stuck spinlock
 */
static void
s_lock_stuck(const char *file, int line, const char *func)
{
	if (!func)
		func = "(unknown)";
	elog(PANIC, "stuck spinlock detected at %s, %s:%d",
		 func, file, line);
}

/*
 * s_lock(lock) - platform-independent portion of waiting for a spinlock.
 */
int
s_lock(volatile slock_t *lock, const char *file, int line, const char *func)
{
	SpinDelayStatus delayStatus;

	init_spin_delay(&delayStatus, file, line, func);

	for (;;)
	{
		bool		try_to_set;

#ifdef PG_SPIN_TRY_RELAXED
		/*
		 * Test-and-test-and-set: read the lock first and only attempt the
		 * exchange when it looks free, to avoid taking cache-line ownership on
		 * every spin.  PG_SPIN_TRY_RELAXED is defined only where this pre-read
		 * is cheap (x86/x86_64 TSO, where a seq_cst load is a plain load).
		 */
		try_to_set = (pg_atomic_read_u32(lock) == 0);
#else
		try_to_set = true;
#endif
		if (try_to_set && pg_atomic_exchange_u32(lock, 1) == 0)
			break;
		perform_spin_delay(&delayStatus);
	}

	finish_spin_delay(&delayStatus);

	return delayStatus.delays;
}

/*
 * Wait while spinning on a contended spinlock.
 */
void
perform_spin_delay(SpinDelayStatus *status)
{
	/* CPU-specific delay each time through the loop */
	pg_spin_delay();

	/* Block the process every spins_per_delay tries */
	if (++(status->spins) >= spins_per_delay)
	{
		if (++(status->delays) > NUM_DELAYS)
			s_lock_stuck(status->file, status->line, status->func);

		if (status->cur_delay == 0) /* first time to delay? */
			status->cur_delay = MIN_DELAY_USEC;

		/*
		 * Once we start sleeping, the overhead of reporting a wait event is
		 * justified. Actively spinning easily stands out in profilers, but
		 * sleeping with an exponential backoff is harder to spot...
		 *
		 * We might want to report something more granular at some point, but
		 * this is better than nothing.
		 */
		pgstat_report_wait_start(WAIT_EVENT_SPIN_DELAY);
		pg_usleep(status->cur_delay);
		pgstat_report_wait_end();

		/* increase delay by a random fraction between 1X and 2X */
		status->cur_delay += (int) (status->cur_delay *
									pg_prng_double(&pg_global_prng_state) + 0.5);
		/* wrap back to minimum delay when max is exceeded */
		if (status->cur_delay > MAX_DELAY_USEC)
			status->cur_delay = MIN_DELAY_USEC;

		status->spins = 0;
	}
}

/*
 * After acquiring a spinlock, update estimates about how long to loop.
 *
 * If we were able to acquire the lock without delaying, it's a good
 * indication we are in a multiprocessor.  If we had to delay, it's a sign
 * (but not a sure thing) that we are in a uniprocessor. Hence, we
 * decrement spins_per_delay slowly when we had to delay, and increase it
 * rapidly when we didn't.  It's expected that spins_per_delay will
 * converge to the minimum value on a uniprocessor and to the maximum
 * value on a multiprocessor.
 *
 * Note: spins_per_delay is local within our current process. We want to
 * average these observations across multiple backends, since it's
 * relatively rare for this function to even get entered, and so a single
 * backend might not live long enough to converge on a good value.  That
 * is handled by the two routines below.
 */
void
finish_spin_delay(SpinDelayStatus *status)
{
	if (status->cur_delay == 0)
	{
		/* we never had to delay */
		if (spins_per_delay < MAX_SPINS_PER_DELAY)
			spins_per_delay = Min(spins_per_delay + 100, MAX_SPINS_PER_DELAY);
	}
	else
	{
		if (spins_per_delay > MIN_SPINS_PER_DELAY)
			spins_per_delay = Max(spins_per_delay - 1, MIN_SPINS_PER_DELAY);
	}
}

/*
 * Set local copy of spins_per_delay during backend startup.
 *
 * NB: this has to be pretty fast as it is called while holding a spinlock
 */
void
set_spins_per_delay(int shared_spins_per_delay)
{
	spins_per_delay = shared_spins_per_delay;
}

/*
 * Update shared estimate of spins_per_delay during backend exit.
 *
 * NB: this has to be pretty fast as it is called while holding a spinlock
 */
int
update_spins_per_delay(int shared_spins_per_delay)
{
	/*
	 * We use an exponential moving average with a relatively slow adaption
	 * rate, so that noise in any one backend's result won't affect the shared
	 * value too much.  As long as both inputs are within the allowed range,
	 * the result must be too, so we need not worry about clamping the result.
	 *
	 * We deliberately truncate rather than rounding; this is so that single
	 * adjustments inside a backend can affect the shared estimate (see the
	 * asymmetric adjustment rules above).
	 */
	return (shared_spins_per_delay * 15 + spins_per_delay) / 16;
}
