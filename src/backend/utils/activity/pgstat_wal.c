/* -------------------------------------------------------------------------
 *
 * pgstat_wal.c
 *	  Implementation of WAL statistics.
 *
 * This file contains the implementation of WAL statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * WAL statistics use a per-backend dshash to avoid double-counting. Each
 * backend flushes WAL usage to its own entry in the dshash (keyed by
 * ProcNumber). The global pg_stat_wal view aggregates the global stats
 * (which holds stats from exited backends) plus all live per-backend entries.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_wal.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/instrument.h"
#include "utils/pgstat_internal.h"


/*
 * WAL usage counters saved from pgWalUsage at the previous call to
 * pgstat_report_wal(). This is used to calculate how much WAL usage
 * happens between pgstat_report_wal() calls, by subtracting
 * the previous counters from the current ones.
 */
static WalUsage prevWalUsage;


/*
 * Calculate how much WAL usage counters have increased and update
 * shared WAL and IO statistics.
 *
 * Must be called by processes that generate WAL, that do not call
 * pgstat_report_stat(), like walwriter.
 *
 * "force" set to true ensures that the statistics are flushed; note that
 * this needs to acquire the pgstat shmem LWLock, waiting on it.  When
 * set to false, the statistics may not be flushed if the lock could not
 * be acquired.
 */
void
pgstat_report_wal(bool force)
{
	bool		nowait;

	/* like in pgstat.c, don't wait for lock acquisition when !force */
	nowait = !force;

	/* flush wal stats */
	(void) pgstat_wal_flush_cb(nowait);

	/* flush IO stats */
	pgstat_flush_io(nowait);
	(void) pgstat_flush_backend(nowait, PGSTAT_BACKEND_FLUSH_IO);
}

/*
 * Support function for the SQL-callable pgstat* functions. Returns
 * a pointer to the WAL statistics struct.
 */
PgStat_WalStats *
pgstat_fetch_stat_wal(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_WAL);

	return &pgStatLocal.snapshot.wal;
}

/*
 * To determine whether WAL usage happened.
 */
static inline bool
pgstat_wal_have_pending(void)
{
	return pgWalUsage.wal_records != prevWalUsage.wal_records;
}

/*
 * Calculate how much WAL usage counters have increased by subtracting the
 * previous counters from the current ones.
 *
 * Flush WAL usage counters to the per-backend dshash entry.
 *
 * If nowait is true, this function returns true if the lock could not be
 * acquired. Otherwise return false.
 */
bool
pgstat_wal_flush_cb(bool nowait)
{
	PgStatShared_WalBackendEntry *entry;
	WalUsage	wal_usage_diff = {0};

	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);
	Assert(pgStatLocal.shmem != NULL &&
		   !pgStatLocal.shmem->is_shutdown);

	/*
	 * This function can be called even if nothing at all has happened. Avoid
	 * taking lock for nothing in that case.
	 */
	if (!pgstat_wal_have_pending())
		return false;

	/*
	 * Calculate how much WAL usage counters were increased by subtracting the
	 * previous counters from the current ones.
	 */
	WalUsageAccumDiff(&wal_usage_diff, &pgWalUsage, &prevWalUsage);

	entry = pgstat_lock_my_per_backend_entry(PGSTAT_KIND_WAL, nowait);

	if (entry == NULL)
		return nowait;

#define WALSTAT_ACC(fld, var_to_add) \
	(entry->stats.wal_counters.fld += var_to_add.fld)
	WALSTAT_ACC(wal_records, wal_usage_diff);
	WALSTAT_ACC(wal_fpi, wal_usage_diff);
	WALSTAT_ACC(wal_bytes, wal_usage_diff);
	WALSTAT_ACC(wal_fpi_bytes, wal_usage_diff);
	WALSTAT_ACC(wal_buffers_full, wal_usage_diff);
#undef WALSTAT_ACC

	LWLockRelease(&entry->header.lock);

	/*
	 * Save the current counters for the subsequent calculation of WAL usage.
	 */
	prevWalUsage = pgWalUsage;

	return false;
}

void
pgstat_wal_init_backend_cb(void)
{
	/*
	 * Initialize prevWalUsage with pgWalUsage so that pgstat_wal_flush_cb()
	 * can calculate how much pgWalUsage counters are increased by subtracting
	 * prevWalUsage from pgWalUsage.
	 */
	prevWalUsage = pgWalUsage;
}

void
pgstat_wal_init_shmem_cb(void *stats)
{
	PgStatShared_Wal *stats_shmem = (PgStatShared_Wal *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_wal_reset_all_cb(TimestampTz ts)
{
	PgStatShared_Wal *shmem = &pgStatLocal.shmem->wal;
	dshash_seq_status hstat;
	PgStatShared_WalBackendEntry *entry;
	dshash_table *hash;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_WAL);

	/*
	 * Hold the kind lock while resetting both the global stats and live
	 * entries. Transfers hold the same lock, so pre-reset counters cannot be
	 * moved into the global stats after it is reset.
	 */
	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);
	memset(&shmem->stats, 0, sizeof(shmem->stats));
	shmem->stats.stat_reset_timestamp = ts;

	/* Reset all per-backend entries */
	if (hash != NULL)
	{
		dshash_seq_init(&hstat, hash, true);
		while ((entry = dshash_seq_next(&hstat)) != NULL)
		{
			LWLockAcquire(&entry->header.lock, LW_EXCLUSIVE);
			memset(&entry->stats.wal_counters, 0, sizeof(PgStat_WalCounters));
			entry->stats.stat_reset_timestamp = ts;
			LWLockRelease(&entry->header.lock);
		}
		dshash_seq_term(&hstat);
	}

	LWLockRelease(&shmem->lock);
}

/*
 * Build WAL stats snapshot by aggregating global stats and all live
 * per-backend entries.
 */
void
pgstat_wal_snapshot_cb(void)
{
	PgStatShared_Wal *shmem = &pgStatLocal.shmem->wal;
	PgStat_WalStats *snap = &pgStatLocal.snapshot.wal;
	dshash_table *hash;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_WAL);

	/*
	 * Prevent entries from moving to the global stats between copying it and
	 * scanning the per-backend hash.
	 */
	LWLockAcquire(&shmem->lock, LW_SHARED);
	memcpy(snap, &shmem->stats, sizeof(PgStat_WalStats));

	/* Add in all live per-backend entries */
	if (hash != NULL)
		pgstat_per_backend_snapshot(PGSTAT_KIND_WAL, hash, snap);

	LWLockRelease(&shmem->lock);
}

/* Macro to accumulate WAL counters from src into dst */
#define WAL_ACCUMULATE_COUNTERS(dst, src) \
do { \
	(dst).wal_records += (src).wal_records; \
	(dst).wal_fpi += (src).wal_fpi; \
	(dst).wal_bytes += (src).wal_bytes; \
	(dst).wal_fpi_bytes += (src).wal_fpi_bytes; \
	(dst).wal_buffers_full += (src).wal_buffers_full; \
} while (0)

/*
 * Accumulate one per-backend WAL entry into a snapshot or the global stats.
 */
void
pgstat_wal_per_backend_acc_cb(void *dst, void *entry)
{
	PgStat_WalStats *stats = dst;
	PgStatShared_WalBackendEntry *e = (PgStatShared_WalBackendEntry *) entry;

	WAL_ACCUMULATE_COUNTERS(stats->wal_counters, e->stats.wal_counters);
}

/*
 * Accumulate a backend's WAL stats into the global stats, then
 * remove the entry from the dshash.
 *
 * Called at backend exit after the final flush, or when a ProcNumber is
 * being reused.
 */
void
pgstat_wal_acc_backend_cb(void)
{
	pgstat_acc_my_per_backend(PGSTAT_KIND_WAL, &pgStatLocal.shmem->wal.lock);
}

/*
 * Returns per-backend WAL statistics for the given ProcNumber.
 */
PgStat_WalStats *
pgstat_fetch_stat_backend_wal(ProcNumber procnum)
{
	return (PgStat_WalStats *) pgstat_fetch_per_backend(PGSTAT_KIND_WAL, procnum);
}

/*
 * Reset a backend's WAL stats. Accumulate the entry's counters into the
 * global stats, then zero the stats and set the reset timestamp.
 */
void
pgstat_wal_reset_backend_cb(ProcNumber procnum, TimestampTz ts)
{
	PgStatShared_Wal *shmem = &pgStatLocal.shmem->wal;
	dshash_table *hash;
	PgStatShared_WalBackendEntry *entry;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_WAL);

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
	WAL_ACCUMULATE_COUNTERS(shmem->stats.wal_counters, entry->stats.wal_counters);

	/* Zero stats and set reset timestamp */
	memset(&entry->stats.wal_counters, 0, sizeof(PgStat_WalCounters));
	entry->stats.stat_reset_timestamp = ts;

	LWLockRelease(&entry->header.lock);
	dshash_release_lock(hash, entry);
	LWLockRelease(&shmem->lock);
}

/*
 * Accumulate all per-backend WAL stats entries into the global stats and remove
 * them. Called at clean server shutdown to ensure all flushed data is preserved
 * in the stats file.
 */
void
pgstat_wal_acc_all_backends(void)
{
	pgstat_acc_all_per_backend(PGSTAT_KIND_WAL, &pgStatLocal.shmem->wal.lock);
}
