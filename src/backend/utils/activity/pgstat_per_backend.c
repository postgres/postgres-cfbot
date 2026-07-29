/* -------------------------------------------------------------------------
 *
 * pgstat_per_backend.c
 *	  Generic infrastructure for per-backend statistics.
 *
 * This file manages the per-kind arrays used for per-backend statistics,
 * including entry creation, fetching, snapshots, transfer to global
 * statistics, and invalidation.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_per_backend.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/proc.h"
#include "utils/pgstat_internal.h"

/*
 * Return one per-backend entry, whether or not it is currently active.
 *
 * Callers that inspect or modify the entry must hold its content lock.
 */
void *
pgstat_get_per_backend_entry(PgStat_Kind kind, ProcNumber procnum)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	char	   *array;

	if (kind_info == NULL || kind_info->per_backend_entry_size == 0)
		return NULL;

	Assert(pgstat_is_kind_builtin(kind));

	if (procnum < 0 || procnum >= NumPerBackendStatSlots)
		return NULL;

	array = pgStatLocal.shmem->per_backend_arrays[kind];
	Assert(array != NULL);

	return array + procnum * kind_info->per_backend_entry_size;
}

/*
 * Initialize this process's entry for one per-backend statistics kind.
 *
 * A blocking flush during early startup may already have initialized the
 * entry. In that case, preserve the statistics it contains.
 */
static PgStatShared_PerBackendEntry *
pgstat_create_my_per_backend_entry(PgStat_Kind kind)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStatShared_PerBackendEntry *entry;

	entry = pgstat_get_per_backend_entry(kind, MyProcNumber);

	LWLockAcquire(&entry->lock, LW_EXCLUSIVE);

	if (entry->backend_type == B_INVALID)
	{
		entry->backend_type = MyBackendType;
		memset((char *) entry + kind_info->per_backend_data_off, 0,
			   kind_info->per_backend_data_len);
	}

	LWLockRelease(&entry->lock);

	return entry;
}

/*
 * Initialize entries for all kinds that use per-backend arrays.
 *
 * This runs after any stale entries for the reused ProcNumber have been
 * transferred to global statistics.
 */
void
pgstat_create_my_per_backend_entries(void)
{
	for (PgStat_Kind kind = PGSTAT_KIND_BUILTIN_MIN;
		 kind <= PGSTAT_KIND_BUILTIN_MAX; kind++)
	{
		const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

		if (kind_info == NULL || kind_info->per_backend_entry_size == 0)
			continue;

		(void) pgstat_create_my_per_backend_entry(kind);
	}
}

/*
 * Lock this process's entry for a per-backend statistics kind.
 *
 * The blocking path initializes an inactive entry, which allows forced
 * flushes during early process startup. The routine nonblocking path does not
 * initialize entries.
 *
 * NB: On success, the entry is returned with its content lock held. A
 * nonblocking call returns NULL if the entry cannot be locked or is inactive.
 */
void *
pgstat_lock_my_per_backend_entry(PgStat_Kind kind, bool nowait)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStatShared_PerBackendEntry *entry;

	entry = pgstat_get_per_backend_entry(kind, MyProcNumber);

	if (nowait)
	{
		if (!LWLockConditionalAcquire(&entry->lock, LW_EXCLUSIVE))
			return NULL;

		if (entry->backend_type == B_INVALID)
		{
			LWLockRelease(&entry->lock);
			return NULL;
		}
	}
	else
	{
		LWLockAcquire(&entry->lock, LW_EXCLUSIVE);

		if (entry->backend_type == B_INVALID)
		{
			entry->backend_type = MyBackendType;
			memset((char *) entry + kind_info->per_backend_data_off, 0,
				   kind_info->per_backend_data_len);
		}
	}

	return entry;
}

/*
 * Accumulate all active per-backend entries into a fixed-kind snapshot.
 *
 * SNAPSHOT mode also copies every active entry into the backend-local
 * snapshot hash, fixing both the values and the set of visible processes.
 * The caller must hold the kind lock to prevent entries from moving into the
 * global statistics while the snapshot is assembled.
 */
void
pgstat_per_backend_snapshot(PgStat_Kind kind, void *snap)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStat_HashKey key = {0};

	key.kind = kind;
	key.dboid = InvalidOid;

	for (ProcNumber procnum = 0; procnum < NumPerBackendStatSlots; procnum++)
	{
		PgStatShared_PerBackendEntry *entry;

		entry = pgstat_get_per_backend_entry(kind, procnum);

		LWLockAcquire(&entry->lock, LW_SHARED);

		if (entry->backend_type != B_INVALID)
		{
			kind_info->per_backend_acc_cb(snap, entry);

			if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
			{
				key.objid = procnum;
				pgstat_cache_snapshot_entry(&key,
											(char *) entry +
											kind_info->per_backend_data_off,
											kind_info->per_backend_data_len);
			}
		}

		LWLockRelease(&entry->lock);
	}
}

/*
 * Fetch one process's per-backend statistics.
 *
 * NONE mode returns a copy allocated in the current memory context. CACHE and
 * SNAPSHOT modes return data owned by the backend-local statistics snapshot.
 * Return NULL if the ProcNumber is invalid or no process was active at the
 * consistency mode's observation point.
 */
void *
pgstat_fetch_per_backend(PgStat_Kind kind, ProcNumber procnum)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStatShared_PerBackendEntry *entry;
	PgStat_HashKey key = {0};
	void	   *stats_data;
	bool		found;

	pgstat_prep_snapshot();

	entry = pgstat_get_per_backend_entry(kind, procnum);

	if (entry == NULL)
		return NULL;

	/* Read directly without caching in NONE mode */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_NONE)
	{
		LWLockAcquire(&entry->lock, LW_SHARED);

		if (entry->backend_type == B_INVALID)
		{
			LWLockRelease(&entry->lock);
			return NULL;
		}

		stats_data = palloc(kind_info->per_backend_data_len);
		memcpy(stats_data, (char *) entry + kind_info->per_backend_data_off,
			   kind_info->per_backend_data_len);
		LWLockRelease(&entry->lock);

		return stats_data;
	}

	Assert(pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_CACHE ||
		   pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT);

	/*
	 * A full fixed-kind snapshot pre-caches all active entries. CACHE mode
	 * reads only the requested array entry on a cache miss.
	 */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		pgstat_snapshot_fixed(kind);
	else
		pgStatLocal.snapshot.mode = PGSTAT_FETCH_CONSISTENCY_CACHE;

	key.kind = kind;
	key.dboid = InvalidOid;
	key.objid = procnum;

	stats_data = pgstat_lookup_snapshot_entry(&key, &found);

	if (found)
		return stats_data;

	/*
	 * A miss after building a full snapshot means that the process was not
	 * active at the snapshot point.
	 */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		return NULL;

	LWLockAcquire(&entry->lock, LW_SHARED);

	if (entry->backend_type == B_INVALID)
		stats_data = pgstat_cache_snapshot_entry(&key, NULL, 0);
	else
		stats_data = pgstat_cache_snapshot_entry(&key,
												 (char *) entry +
												 kind_info->per_backend_data_off,
												 kind_info->per_backend_data_len);

	LWLockRelease(&entry->lock);

	return stats_data;
}

/*
 * Transfer this process's entry into global statistics and invalidate it.
 *
 * The kind lock serializes this transfer with global snapshots and resets.
 * The entry lock serializes it with routine flushes and per-backend reads.
 */
void
pgstat_acc_my_per_backend(PgStat_Kind kind, LWLock *lock)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStatShared_PerBackendEntry *entry;
	char	   *shared_struct;
	void	   *dst;

	entry = pgstat_get_per_backend_entry(kind, MyProcNumber);

	if (entry == NULL)
		return;

	shared_struct = (char *) pgStatLocal.shmem + kind_info->shared_ctl_off;
	dst = shared_struct + kind_info->shared_data_off;

	LWLockAcquire(lock, LW_EXCLUSIVE);
	LWLockAcquire(&entry->lock, LW_EXCLUSIVE);

	if (entry->backend_type != B_INVALID)
	{
		kind_info->per_backend_acc_cb(dst, entry);
		entry->backend_type = B_INVALID;
	}

	LWLockRelease(&entry->lock);
	LWLockRelease(lock);
}

/*
 * Transfer every active entry into global statistics and invalidate it.
 *
 * This is called during clean server shutdown before the statistics file is
 * written.
 */
void
pgstat_acc_all_per_backend(PgStat_Kind kind, LWLock *lock)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	char	   *shared_struct;
	void	   *dst;

	shared_struct = (char *) pgStatLocal.shmem + kind_info->shared_ctl_off;
	dst = shared_struct + kind_info->shared_data_off;

	LWLockAcquire(lock, LW_EXCLUSIVE);

	for (ProcNumber procnum = 0; procnum < NumPerBackendStatSlots; procnum++)
	{
		PgStatShared_PerBackendEntry *entry;

		entry = pgstat_get_per_backend_entry(kind, procnum);

		LWLockAcquire(&entry->lock, LW_EXCLUSIVE);

		if (entry->backend_type != B_INVALID)
		{
			kind_info->per_backend_acc_cb(dst, entry);
			entry->backend_type = B_INVALID;
		}

		LWLockRelease(&entry->lock);
	}

	LWLockRelease(lock);
}
