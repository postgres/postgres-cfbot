/* -------------------------------------------------------------------------
 *
 * pgstat_vfdcache.c
 *	  Implementation of VFD cache statistics.
 *
 * VFD events are first counted in backend-local pending storage and then
 * flushed into shared-memory cumulative stats, following the same model as
 * other fixed stats kinds.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_vfdcache.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"

/*
 * Backend-local VFD counters waiting to be flushed.
 */
PgStat_VfdCacheStats PendingVfdCacheStats = {0};

/*
 * Count a VFD cache access as either a hit or miss.
 */
void
pgstat_count_vfd_access(bool hit)
{
	if (hit)
		PendingVfdCacheStats.vfd_hits++;
	else
		PendingVfdCacheStats.vfd_misses++;
	pgstat_report_fixed = true;
}

/*
 * Flush out backend-local pending VFD cache stats.
 */
bool
pgstat_vfdcache_flush_cb(bool nowait)
{
	PgStatShared_VfdCache *stats_shmem = &pgStatLocal.shmem->vfdcache;

	if (pg_memory_is_all_zeros(&PendingVfdCacheStats,
							   sizeof(struct PgStat_VfdCacheStats)))
		return false;

	if (!nowait)
		LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&stats_shmem->lock, LW_EXCLUSIVE))
		return true;

	stats_shmem->stats.vfd_hits += PendingVfdCacheStats.vfd_hits;
	stats_shmem->stats.vfd_misses += PendingVfdCacheStats.vfd_misses;

	LWLockRelease(&stats_shmem->lock);

	MemSet(&PendingVfdCacheStats, 0, sizeof(PendingVfdCacheStats));

	return false;
}

/*
 * Support function for SQL-callable pg_stat_get_vfd_* functions.
 */
PgStat_VfdCacheStats *
pgstat_fetch_stat_vfdcache(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_VFDCACHE);

	return &pgStatLocal.snapshot.vfdcache;
}

void
pgstat_vfdcache_init_shmem_cb(void *stats)
{
	PgStatShared_VfdCache *stats_shmem = (PgStatShared_VfdCache *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_vfdcache_reset_all_cb(TimestampTz ts)
{
	PgStatShared_VfdCache *stats_shmem = &pgStatLocal.shmem->vfdcache;

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	MemSet(&stats_shmem->stats, 0, sizeof(stats_shmem->stats));
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_vfdcache_snapshot_cb(void)
{
	PgStatShared_VfdCache *stats_shmem = &pgStatLocal.shmem->vfdcache;

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&pgStatLocal.snapshot.vfdcache, &stats_shmem->stats,
		   sizeof(pgStatLocal.snapshot.vfdcache));
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_reset_vfdcache(void)
{
	pgstat_reset_of_kind(PGSTAT_KIND_VFDCACHE);
}
