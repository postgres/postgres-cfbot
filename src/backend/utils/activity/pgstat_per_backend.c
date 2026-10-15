/* -------------------------------------------------------------------------
 *
 * pgstat_per_backend.c
 *	  Generic infrastructure for per-backend statistics.
 *
 * This file manages the dedicated per-kind dshashes used for per-backend
 * statistics, including entry creation, fetching, snapshots, transfer to
 * global statistics, and removal.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_per_backend.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/dshash.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"

/* hash table for per-backend stats snapshot entries */
typedef struct PgStat_PerBackendSnapshotKey
{
	PgStat_Kind kind;
	ProcNumber	procnum;
} PgStat_PerBackendSnapshotKey;

typedef struct PgStat_PerBackendSnapshotEntry
{
	PgStat_PerBackendSnapshotKey key;
	char		status;			/* for simplehash use */
	void	   *data;			/* the stats data itself */
} PgStat_PerBackendSnapshotEntry;

#define SH_PREFIX pgstat_per_backend_snapshot
#define SH_ELEMENT_TYPE PgStat_PerBackendSnapshotEntry
#define SH_KEY_TYPE PgStat_PerBackendSnapshotKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) \
	fasthash32((const char *) &key, sizeof(PgStat_PerBackendSnapshotKey), 0)
#define SH_EQUAL(tb, a, b) \
	(memcmp(&a, &b, sizeof(PgStat_PerBackendSnapshotKey)) == 0)
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

/* Per-kind, backend-local state for the per-backend dshashes. */
typedef struct PgStat_PerBackendLocalState
{
	dshash_table *hash;
	PgStatShared_PerBackendEntry *my_entry;
} PgStat_PerBackendLocalState;

static PgStat_PerBackendLocalState per_backend_states[PGSTAT_KIND_BUILTIN_SIZE];

/*
 * Look up a per-backend stats entry in the backend snapshot hash.
 *
 * The returned entry may be empty when no matching statistics were found on
 * first access.
 */
static PgStat_PerBackendSnapshotEntry *
pgstat_lookup_per_backend_entry(PgStat_Kind kind, ProcNumber procnum)
{
	PgStat_PerBackendSnapshotKey key;

	if (pgStatLocal.snapshot.per_backend_stats == NULL)
		return NULL;

	key.kind = kind;
	key.procnum = procnum;

	return pgstat_per_backend_snapshot_lookup(pgStatLocal.snapshot.per_backend_stats,
											  key);
}

/*
 * Cache a per-backend stats entry in the backend snapshot hash.
 *
 * If data is NULL, cache an empty entry to record that no matching statistics
 * were found on first access.
 */
static void *
pgstat_cache_per_backend_entry(PgStat_Kind kind, ProcNumber procnum,
							   const void *data)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStat_PerBackendSnapshotKey key;
	PgStat_PerBackendSnapshotEntry *entry;
	bool		found;

	Assert(pgstat_fetch_consistency > PGSTAT_FETCH_CONSISTENCY_NONE);
	Assert(kind_info != NULL);
	Assert(kind_info->per_backend_data_len > 0);

	/* Ensure snapshot context exists */
	if (!pgStatLocal.snapshot.context)
		pgStatLocal.snapshot.context = AllocSetContextCreate(TopMemoryContext,
															 "PgStat Snapshot",
															 ALLOCSET_SMALL_SIZES);

	/* Create per-backend hash on first use */
	if (pgStatLocal.snapshot.per_backend_stats == NULL)
		pgStatLocal.snapshot.per_backend_stats =
			pgstat_per_backend_snapshot_create(pgStatLocal.snapshot.context, 64, NULL);

	key.kind = kind;
	key.procnum = procnum;

	/* If already cached, return cached data */
	entry = pgstat_per_backend_snapshot_lookup(pgStatLocal.snapshot.per_backend_stats, key);

	if (entry)
		return entry->data;

	/* Insert new entry into the hash */
	entry = pgstat_per_backend_snapshot_insert(pgStatLocal.snapshot.per_backend_stats,
											   key, &found);
	Assert(!found);

	if (data != NULL)
	{
		entry->data = MemoryContextAlloc(pgStatLocal.snapshot.context,
										 kind_info->per_backend_data_len);
		memcpy(entry->data, data, kind_info->per_backend_data_len);
	}
	else
		entry->data = NULL;

	return entry->data;
}

static inline PgStat_PerBackendLocalState *
pgstat_get_per_backend_local_state(PgStat_Kind kind)
{
	const PgStat_KindInfo *kind_info PG_USED_FOR_ASSERTS_ONLY = pgstat_get_kind_info(kind);

	Assert(kind_info != NULL);
	Assert(kind_info->fixed_amount);

	if (!pgstat_is_kind_builtin(kind))
		elog(ERROR, "invalid statistics kind: %u", kind);

	return &per_backend_states[kind];
}

/*
 * Create and cache this process's entry for one per-backend statistics kind.
 */
static PgStatShared_PerBackendEntry *
pgstat_create_my_per_backend_entry(PgStat_Kind kind)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStat_PerBackendLocalState *state = pgstat_get_per_backend_local_state(kind);
	dshash_table *hash = pgstat_per_backend_attach(kind);
	PgStatShared_PerBackendEntry *entry;
	bool		found;

	Assert(state->my_entry == NULL);

	if (hash == NULL)
		return NULL;

	Assert(kind_info != NULL);
	Assert(kind_info->per_backend_data_len > 0);

	entry = dshash_find_or_insert(hash, &MyProcNumber, &found);

	/*
	 * A forced flush during early backend startup may already have created
	 * the entry. Preserve any statistics it contains.
	 */
	if (!found)
	{
		entry->backend_type = MyBackendType;
		LWLockInitialize(&entry->lock, LWTRANCHE_PGSTATS_DATA);
		memset((char *) entry + kind_info->per_backend_data_off, 0,
			   kind_info->per_backend_data_len);
	}

	state->my_entry = entry;
	dshash_release_lock(hash, entry);

	return entry;
}

/*
 * Create entries for all the kinds that use per-backend dshashes.
 *
 * Allocations and dshash resizes are deliberately done during backend
 * initialization so routine stats flushes only need to conditionally acquire
 * the cached entry's content lock.
 */
void
pgstat_create_my_per_backend_entries(void)
{
	for (PgStat_Kind kind = PGSTAT_KIND_BUILTIN_MIN;
		 kind <= PGSTAT_KIND_BUILTIN_MAX; kind++)
	{
		const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

		if (kind_info == NULL || kind_info->per_backend_data_len == 0)
			continue;

		(void) pgstat_create_my_per_backend_entry(kind);
	}
}

/*
 * Attach to the per-backend dshash for the given kind.
 * Returns NULL if the hash is not available (e.g. during bootstrap or
 * if this kind doesn't have per-backend tracking).
 */
dshash_table *
pgstat_per_backend_attach(PgStat_Kind kind)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStat_PerBackendLocalState *state = pgstat_get_per_backend_local_state(kind);
	char	   *shared_struct;
	dshash_table_handle *handle_ptr;
	MemoryContext oldcontext;
	dshash_parameters params;

	if (state->hash != NULL)
		return state->hash;

	if (!kind_info || !kind_info->per_backend_data_len)
		return NULL;

	/* Get the shared struct for this kind */
	shared_struct = (char *) pgStatLocal.shmem + kind_info->shared_ctl_off;
	handle_ptr = (dshash_table_handle *) (shared_struct + kind_info->per_backend_hash_handle_off);

	if (*handle_ptr == DSHASH_HANDLE_INVALID)
		return NULL;

	/*
	 * Build dshash parameters from kind info. All per-backend hashes use
	 * ProcNumber keys and dshash_memcmp/dshash_memhash.
	 */
	params.key_size = sizeof(ProcNumber);
	params.entry_size = kind_info->per_backend_data_off + kind_info->per_backend_data_len;
	params.compare_function = dshash_memcmp;
	params.hash_function = dshash_memhash;
	params.copy_function = dshash_memcpy;
	params.tranche_id = LWTRANCHE_PGSTATS_HASH;

	/* Attach in TopMemoryContext */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	state->hash = dshash_attach(pgStatLocal.dsa, &params, *handle_ptr, NULL);
	MemoryContextSwitchTo(oldcontext);

	return state->hash;
}

/*
 * Lock this process's cached entry for a per-backend statistics kind.
 *
 * A missing entry is only created in the blocking path. The routine nowait
 * path must not perform dshash lookups, allocations, or DSA address
 * resolution.
 */
void *
pgstat_lock_my_per_backend_entry(PgStat_Kind kind, bool nowait)
{
	const PgStat_KindInfo *kind_info PG_USED_FOR_ASSERTS_ONLY = pgstat_get_kind_info(kind);
	PgStat_PerBackendLocalState *state = pgstat_get_per_backend_local_state(kind);
	PgStatShared_PerBackendEntry *entry = state->my_entry;

	Assert(kind_info != NULL);
	Assert(kind_info->per_backend_data_len > 0);

	if (entry == NULL)
	{
		if (nowait)
			return NULL;

		entry = pgstat_create_my_per_backend_entry(kind);
		if (entry == NULL)
			return NULL;
	}

	if (nowait)
	{
		if (!LWLockConditionalAcquire(&entry->lock, LW_EXCLUSIVE))
			return NULL;
	}
	else
		LWLockAcquire(&entry->lock, LW_EXCLUSIVE);

	return entry;
}

/*
 * Accumulate all live per-backend entries into the kind's data in
 * pgStatLocal.snapshot, optionally caching each entry for SNAPSHOT mode.
 */
void
pgstat_per_backend_snapshot(PgStat_Kind kind, dshash_table *hash, void *snap)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	dshash_seq_status hstat;
	PgStatShared_PerBackendEntry *entry;

	dshash_seq_init(&hstat, hash, false);

	while ((entry = dshash_seq_next(&hstat)) != NULL)
	{
		LWLockAcquire(&entry->lock, LW_SHARED);

		/* Kind-specific accumulation into global snapshot */
		kind_info->per_backend_acc_cb(snap, entry);

		/*
		 * In SNAPSHOT mode, cache each per-backend entry so that
		 * pgstat_fetch_per_backend() can return a consistent point-in-time
		 * view without re-reading from shared memory.
		 */
		if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		{
			pgstat_cache_per_backend_entry(kind, entry->key,
										   (char *) entry + kind_info->per_backend_data_off);
		}

		LWLockRelease(&entry->lock);
	}

	dshash_seq_term(&hstat);
}

/*
 * Fetch per-backend stats for the given kind and ProcNumber.
 * Returns NULL if no entry exists. In NONE mode, returns a copy allocated in
 * the current memory context. In CACHE and SNAPSHOT modes, returns a pointer
 * owned by the statistics snapshot cache.
 */
void *
pgstat_fetch_per_backend(PgStat_Kind kind, ProcNumber procnum)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	dshash_table *hash;
	PgStatShared_PerBackendEntry *entry;
	void	   *stats_data;
	PgStat_PerBackendSnapshotEntry *snapshot_entry;

	pgstat_maybe_clear_snapshot();

	hash = pgstat_per_backend_attach(kind);

	if (hash == NULL)
		return NULL;

	/* In NONE mode, read directly and don't cache */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_NONE)
	{
		entry = dshash_find(hash, &procnum, false);
		if (entry == NULL)
			return NULL;

		LWLockAcquire(&entry->lock, LW_SHARED);
		stats_data = palloc(kind_info->per_backend_data_len);
		memcpy(stats_data, (char *) entry + kind_info->per_backend_data_off,
			   kind_info->per_backend_data_len);
		LWLockRelease(&entry->lock);
		dshash_release_lock(hash, entry);

		return stats_data;
	}

	Assert(pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_CACHE ||
		   pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT);

	/*
	 * Building a full snapshot pre-caches all existing per-backend entries.
	 * CACHE mode only needs the requested entry, so it must not build the
	 * aggregate fixed-kind snapshot and scan every live backend.
	 */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		pgstat_snapshot_fixed(kind);

	snapshot_entry = pgstat_lookup_per_backend_entry(kind, procnum);

	if (snapshot_entry != NULL)
		return snapshot_entry->data;

	/*
	 * Once a full snapshot has been built, a cache miss means the entry did
	 * not exist at the snapshot point. Do not admit an entry created later.
	 */
	if (pgstat_fetch_consistency == PGSTAT_FETCH_CONSISTENCY_SNAPSHOT)
		return NULL;

	/* CACHE miss: copy the live entry directly into the snapshot context. */
	entry = dshash_find(hash, &procnum, false);

	if (entry == NULL)
		return pgstat_cache_per_backend_entry(kind, procnum, NULL);

	LWLockAcquire(&entry->lock, LW_SHARED);

	stats_data = pgstat_cache_per_backend_entry(kind, procnum,
												(char *) entry + kind_info->per_backend_data_off);

	LWLockRelease(&entry->lock);
	dshash_release_lock(hash, entry);

	return stats_data;
}

/*
 * Accumulate this process's per-backend stats into the global stats, then
 * remove the entry from the dshash.
 * Acquire the kind lock before the dshash partition lock. Snapshots and resets
 * hold the kind lock while accessing both the global stats and live entries,
 * so an entry cannot move between them during either operation.
 *
 * NB: The entry may belong to an earlier process that used the same ProcNumber.
 * It must still be accumulated before removal.
 */
void
pgstat_acc_my_per_backend(PgStat_Kind kind, LWLock *lock)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStat_PerBackendLocalState *state = pgstat_get_per_backend_local_state(kind);
	char	   *shared_struct;
	dshash_table *hash;
	void	   *dst;
	PgStatShared_PerBackendEntry *entry;

	hash = pgstat_per_backend_attach(kind);

	if (hash == NULL)
		return;

	shared_struct = (char *) pgStatLocal.shmem + kind_info->shared_ctl_off;
	dst = shared_struct + kind_info->shared_data_off;

	LWLockAcquire(lock, LW_EXCLUSIVE);

	entry = dshash_find(hash, &MyProcNumber, true);

	if (entry == NULL)
	{
		state->my_entry = NULL;
		LWLockRelease(lock);
		return;
	}

	LWLockAcquire(&entry->lock, LW_EXCLUSIVE);

	if (state->my_entry == entry)
		state->my_entry = NULL;

	/* Kind-specific accumulation */
	kind_info->per_backend_acc_cb(dst, entry);

	LWLockRelease(&entry->lock);

	/* Remove the entry */
	dshash_delete_entry(hash, entry);

	LWLockRelease(lock);
}

/*
 * Accumulate all per-backend entries into global stats and delete them.
 * Called at clean server shutdown before writing the stats file. Acquire the
 * kind's global lock before starting the dshash scan.
 */
void
pgstat_acc_all_per_backend(PgStat_Kind kind, LWLock *lock)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
	PgStat_PerBackendLocalState *state = pgstat_get_per_backend_local_state(kind);
	char	   *shared_struct;
	dshash_table *hash;
	void	   *dst;
	dshash_seq_status hstat;
	PgStatShared_PerBackendEntry *entry;

	hash = pgstat_per_backend_attach(kind);

	if (hash == NULL)
		return;

	shared_struct = (char *) pgStatLocal.shmem + kind_info->shared_ctl_off;
	dst = shared_struct + kind_info->shared_data_off;

	LWLockAcquire(lock, LW_EXCLUSIVE);

	dshash_seq_init(&hstat, hash, true);

	while ((entry = dshash_seq_next(&hstat)) != NULL)
	{
		LWLockAcquire(&entry->lock, LW_EXCLUSIVE);

		if (state->my_entry == entry)
			state->my_entry = NULL;

		/* Kind-specific accumulation */
		kind_info->per_backend_acc_cb(dst, entry);

		LWLockRelease(&entry->lock);
		dshash_delete_current(&hstat);
	}

	dshash_seq_term(&hstat);

	LWLockRelease(lock);
}
