/* -------------------------------------------------------------------------
 *
 * pgstat_lock.c
 *	  Implementation of lock statistics.
 *
 * This file contains the implementation of lock statistics.  It is kept
 * separate from pgstat.c to enforce the line between the statistics
 * access / storage implementation and the details about individual types
 * of statistics.
 *
 * Lock statistics use a per-backend dshash to avoid double-counting. Each
 * backend flushes lock stats (waits, wait_time, fastpath_exceeded per lock
 * tag type) to its own entry in the dshash. The global pg_stat_lock view
 * aggregates the global stats (which holds stats from exited backends) plus
 * all live per-backend entries.
 *
 * Copyright (c) 2021-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_lock.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"

static PgStat_PendingLock PendingLockStats;
static bool have_lockstats = false;

PgStat_Lock *
pgstat_fetch_stat_lock(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_LOCK);

	return &pgStatLocal.snapshot.lock;
}

/*
 * Simpler wrapper of pgstat_lock_flush_cb()
 */
void
pgstat_lock_flush(bool nowait)
{
	(void) pgstat_lock_flush_cb(nowait);
}

/*
 * Flush out locally pending lock statistics to the per-backend dshash entry.
 *
 * If no stats have been recorded, this function returns false.
 *
 * If nowait is true, this function returns true if the lock could not be
 * acquired. Otherwise, return false.
 */
bool
pgstat_lock_flush_cb(bool nowait)
{
	PgStatShared_LockBackendEntry *entry;

	if (!have_lockstats)
		return false;

	entry = pgstat_lock_my_per_backend_entry(PGSTAT_KIND_LOCK, nowait);

	if (entry == NULL)
		return nowait;

	for (int i = 0; i <= LOCKTAG_LAST_TYPE; i++)
	{
#define LOCKSTAT_ACC(fld) \
	(entry->stats.stats[i].fld += PendingLockStats.stats[i].fld)
		LOCKSTAT_ACC(waits);
		LOCKSTAT_ACC(wait_time);
		LOCKSTAT_ACC(fastpath_exceeded);
#undef LOCKSTAT_ACC
	}

	LWLockRelease(&entry->header.lock);

	memset(&PendingLockStats, 0, sizeof(PendingLockStats));
	have_lockstats = false;

	return false;
}

void
pgstat_lock_init_shmem_cb(void *stats)
{
	PgStatShared_Lock *stat_shmem = (PgStatShared_Lock *) stats;

	LWLockInitialize(&stat_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_lock_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Lock *shmem = &pgStatLocal.shmem->lock;
	dshash_seq_status hstat;
	PgStatShared_LockBackendEntry *entry;
	dshash_table *hash;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_LOCK);

	/*
	 * Hold the kind lock while resetting both the global stats and live
	 * entries. Transfers hold the same lock, so pre-reset counters cannot be
	 * moved into the global stats after it is reset.
	 */
	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);
	memset(&shmem->stats, 0, sizeof(shmem->stats));
	shmem->stats.stat_reset_timestamp = ts;

	/* Reset all per-backend entries, since they contribute to the global view */
	if (hash != NULL)
	{
		dshash_seq_init(&hstat, hash, true);
		while ((entry = dshash_seq_next(&hstat)) != NULL)
		{
			LWLockAcquire(&entry->header.lock, LW_EXCLUSIVE);
			memset(&entry->stats.stats, 0, sizeof(entry->stats.stats));
			entry->stats.stat_reset_timestamp = ts;
			LWLockRelease(&entry->header.lock);
		}
		dshash_seq_term(&hstat);
	}

	LWLockRelease(&shmem->lock);
}

/*
 * Build lock stats snapshot by aggregating global stats and all live
 * per-backend entries.
 */
void
pgstat_lock_snapshot_cb(void)
{
	PgStatShared_Lock *shmem = &pgStatLocal.shmem->lock;
	PgStat_Lock *snap = &pgStatLocal.snapshot.lock;
	dshash_table *hash;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_LOCK);

	/*
	 * Prevent entries from moving to the global stats between copying it and
	 * scanning the per-backend hash.
	 */
	LWLockAcquire(&shmem->lock, LW_SHARED);
	memcpy(snap, &shmem->stats, sizeof(PgStat_Lock));

	/* Add in all live per-backend entries */
	if (hash != NULL)
		pgstat_per_backend_snapshot(PGSTAT_KIND_LOCK, hash, snap);

	LWLockRelease(&shmem->lock);
}

/*
 * Accumulate one per-backend lock entry into a snapshot or the global stats.
 */
void
pgstat_lock_per_backend_acc_cb(void *dst, void *entry)
{
	PgStat_Lock *stats = dst;
	PgStatShared_LockBackendEntry *e = (PgStatShared_LockBackendEntry *) entry;

	for (int j = 0; j <= LOCKTAG_LAST_TYPE; j++)
	{
		stats->stats[j].waits += e->stats.stats[j].waits;
		stats->stats[j].wait_time += e->stats.stats[j].wait_time;
		stats->stats[j].fastpath_exceeded += e->stats.stats[j].fastpath_exceeded;
	}
}

/* Macro to accumulate lock counters from src into dst */
#define LOCK_ACCUMULATE_COUNTERS(dst, src) \
do { \
	for (int _i = 0; _i <= LOCKTAG_LAST_TYPE; _i++) \
	{ \
		(dst)[_i].waits += (src)[_i].waits; \
		(dst)[_i].wait_time += (src)[_i].wait_time; \
		(dst)[_i].fastpath_exceeded += (src)[_i].fastpath_exceeded; \
	} \
} while (0)

/*
 * Accumulate a backend's lock stats into the global stats, then
 * remove the entry from the dshash.
 *
 * Called at backend exit after the final flush, or when a ProcNumber is
 * being reused.
 */
void
pgstat_lock_acc_backend_cb(void)
{
	pgstat_acc_my_per_backend(PGSTAT_KIND_LOCK, &pgStatLocal.shmem->lock.lock);
}

/*
 * Accumulate all per-backend lock stats entries into the global stats and remove
 * them. Called at clean server shutdown to ensure all flushed data is preserved
 * in the stats file.
 */
void
pgstat_lock_acc_all_backends(void)
{
	pgstat_acc_all_per_backend(PGSTAT_KIND_LOCK, &pgStatLocal.shmem->lock.lock);
}

/*
 * Returns per-backend lock statistics for the given ProcNumber.
 */
PgStat_Lock *
pgstat_fetch_stat_backend_lock(ProcNumber procnum)
{
	return (PgStat_Lock *) pgstat_fetch_per_backend(PGSTAT_KIND_LOCK, procnum);
}

/*
 * Increment counter for lock not acquired with the fast-path, per lock
 * type, due to the fast-path slot limit reached.
 *
 * Note: This function should not be called in performance-sensitive paths,
 * like lock acquisitions.
 */
void
pgstat_count_lock_fastpath_exceeded(uint8 locktag_type)
{
	Assert(locktag_type <= LOCKTAG_LAST_TYPE);
	PendingLockStats.stats[locktag_type].fastpath_exceeded++;
	have_lockstats = true;
	pgstat_report_fixed = true;
}

/*
 * Increment the number of waits and wait time, per lock type.
 *
 * Note: This function should not be called in performance-sensitive paths,
 * like lock acquisitions.
 */
void
pgstat_count_lock_waits(uint8 locktag_type, PgStat_Counter usecs)
{
	Assert(locktag_type <= LOCKTAG_LAST_TYPE);
	PendingLockStats.stats[locktag_type].waits++;
	PendingLockStats.stats[locktag_type].wait_time += usecs;
	have_lockstats = true;
	pgstat_report_fixed = true;
}

/*
 * Reset a backend's lock stats. Accumulate the entry's counters into the
 * global stats, then zero the stats and set the reset timestamp.
 */
void
pgstat_lock_reset_backend_cb(ProcNumber procnum, TimestampTz ts)
{
	PgStatShared_Lock *shmem = &pgStatLocal.shmem->lock;
	dshash_table *hash;
	PgStatShared_LockBackendEntry *entry;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_LOCK);

	if (hash == NULL)
		return;

	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);

	entry = dshash_find(hash, &procnum, true);

	if (entry == NULL)
	{
		LWLockRelease(&shmem->lock);
		return;
	}

	LWLockAcquire(&entry->header.lock, LW_EXCLUSIVE);

	/* Accumulate current stats into global before zeroing */
	LOCK_ACCUMULATE_COUNTERS(shmem->stats.stats, entry->stats.stats);

	/* Zero stats and set reset timestamp */
	memset(&entry->stats.stats, 0, sizeof(entry->stats.stats));
	entry->stats.stat_reset_timestamp = ts;

	LWLockRelease(&entry->header.lock);
	dshash_release_lock(hash, entry);
	LWLockRelease(&shmem->lock);
}
