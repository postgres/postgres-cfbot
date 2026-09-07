/*-------------------------------------------------------------------------
 *
 * global_temp.c
 *	  Global temporary relation management.
 *
 * This tracks all global temporary relations in use across all backends,
 * as well as any local storage created for global temporary relations used
 * in this backend.
 *
 * When a global temporary relation is created or first opened, it is
 * initialized for use, which (for most relkinds) includes creating local
 * storage for it.  All global temporary relations in use and all local
 * storage created is tracked, taking into account (sub)transaction
 * rollback --- a rollback can undo the effects of creating or opening a
 * global temporary relation, including the creation of local storage.  If
 * a global temporary relation is first opened in a (sub)transaction which
 * is then rolled back, it is reinitialized the next time it is opened.
 * When the backend exits, all locally created storage is deleted.
 *
 * Relcache invalidation messages are passed on to code here so that it can
 * deal with other backends dropping global temporary relations.  If a
 * global temporary relation in use by this backend is dropped by another
 * backend, all local storage created for the relation in this backend is
 * deleted.
 *
 * A shared hash table is used to track all global temporary relations in
 * use across all databases and all backends.  A "usage count" is kept for
 * each relation, which is a count of the number of backends using it.
 * This is used to prevent operations like ALTER TABLE from altering a
 * global temporary table in a way that would require rewriting its
 * contents, if it's in use by other backends, which cannot be allowed,
 * since there is no way to rewrite the local storage of other backends.
 *
 * A global temporary relation is regarded as "in use" by a backend from
 * the time it is created or first opened until the time it is dropped or
 * the backend exits (or a rollback undoes the creation or opening of the
 * relation).  This means that a backend that executes CREATE GLOBAL TEMP
 * TABLE is counted as using it, even if it never opens it.
 *
 * When a global temporary relation is not in use by any backend, it has no
 * physical storage.  Thus a global temporary relation must have a shared
 * dependency on its tablespace to prevent the tablespace from being
 * dropped while the relation is not being used.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/catalog/global_temp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/global_temp.h"
#include "catalog/indexing.h"
#include "catalog/pg_temp_class.h"
#include "catalog/pg_temp_index.h"
#include "catalog/storage.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "utils/fmgroids.h"
#include "utils/gtcatcache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

/*
 * gtr_local_storage
 *
 *	Hash table to track local storage created by this backend for global
 *	temporary relations.
 */
typedef struct GtrStorageEntry
{
	RelFileLocator rlocator;	/* lookup key: relfilelocator of storage */
	Oid			relid;			/* OID of relation owning the storage */
	SubTransactionId created_subid; /* storage was created in current xact */
	SubTransactionId dropped_subid; /* dropped with another subid set */
} GtrStorageEntry;

static HTAB *gtr_local_storage;

/*
 * eoxact_storage_list[]
 *
 *	List of relfilelocators of storage that (might) need AtEOXact cleanup
 *	work.  As with the relcache's eoxact_list[], this list intentionally has
 *	limited size, and we switch to a full hash table traversal if the list
 *	overflows.
 */
#define MAX_EOXACT_STORAGE_LIST 32
static RelFileLocator eoxact_storage_list[MAX_EOXACT_STORAGE_LIST];
static int	eoxact_storage_list_len = 0;
static bool eoxact_storage_list_overflowed = false;

#define EOXactStorageListAdd(rlocator) \
	do { \
		if (eoxact_storage_list_len < MAX_EOXACT_STORAGE_LIST) \
			eoxact_storage_list[eoxact_storage_list_len++] = (rlocator); \
		else \
			eoxact_storage_list_overflowed = true; \
	} while (0)

/*
 * gtr_local_usage
 *
 *	Hash table to track global temporary relations in use in this backend.
 */
typedef struct GtrUsageEntry
{
	Oid			relid;			/* lookup key: OID of relation in use */
	char		relkind;		/* relkind of the relation */
	SubTransactionId started_subid; /* usage started in current xact */
	SubTransactionId stopped_subid; /* usage ended with another subid set */
} GtrUsageEntry;

static HTAB *gtr_local_usage;

/*
 * eoxact_usage_list[]
 *
 *	List of OIDs of global temporary relation usage entries that (might) need
 *	AtEOXact cleanup work.  Cf. eoxact_storage_list[].
 */
#define MAX_EOXACT_USAGE_LIST 32
static Oid	eoxact_usage_list[MAX_EOXACT_USAGE_LIST];
static int	eoxact_usage_list_len = 0;
static bool eoxact_usage_list_overflowed = false;

#define EOXactUsageListAdd(relid) \
	do { \
		if (eoxact_usage_list_len < MAX_EOXACT_USAGE_LIST) \
			eoxact_usage_list[eoxact_usage_list_len++] = (relid); \
		else \
			eoxact_usage_list_overflowed = true; \
	} while (0)

/*
 * Invalidation message handling:
 *
 *	gtrs_invalidated
 *		OIDs of global temporary relations that we are using, for which we
 *		have received an invalidation message.
 *
 *	gtrs_dropped
 *		OIDs of global temporary relations that we were using, which have been
 *		dropped by another backend (excludes locally dropped relations).
 *
 *	processing_invalidated_gtrs
 *		True while processing invalidated global temporary relations (used to
 *		prevent infinite recursion).
 *
 *	processed_dropped_subid
 *		Subtransaction ID in which we processed global temporary relations
 *		dropped by other backends.
 */
static List *gtrs_invalidated = NIL;
static List *gtrs_dropped = NIL;
static bool processing_invalidated_gtrs = false;
static SubTransactionId processed_dropped_subid = InvalidSubTransactionId;

/*
 * update_tempfrozenxids
 *
 *	Flag indicating that this backend's tempfrozenxid and tempminmxid need to
 *	be updated on commit.  See comments in UpdateTempFrozenXids().
 */
static bool update_tempfrozenxids = false;

/*
 * Subtransaction ID in which we executed DISCARD GLOBAL TEMP.
 */
static SubTransactionId discard_subid = InvalidSubTransactionId;

/*
 * gtr_shared_usage
 *
 *	Shared hash table recording all global temporary relation usage across all
 *	databases and backends.  For each relation, "usage_count" records the
 *	number of backends (including us) using the relation.  Entries are
 *	removed when the usage count hits zero.
 */
typedef struct GtrSharedUsageKey
{
	Oid			dbid;			/* DB containing global temporary relation */
	Oid			relid;			/* OID of global temporary relation */
} GtrSharedUsageKey;

typedef struct GtrSharedUsageEntry
{
	GtrSharedUsageKey key;		/* lookup key: (dbid, relid) of relation */
	int			usage_count;	/* number of backends accessing relation */
} GtrSharedUsageEntry;

static const dshash_parameters gtr_shared_usage_params = {
	sizeof(GtrSharedUsageKey),
	sizeof(GtrSharedUsageEntry),
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	LWTRANCHE_GLOBAL_TEMP_REL_HASH
};

static dsa_area *gtr_shared_usage_dsa;
static dshash_table *gtr_shared_usage;

/*
 * gtr_shmem_control
 *
 *	Shared memory state for the global temporary relation shared usage table.
 */
typedef struct GlobalTempRelShmemControl
{
	dsa_handle dsa_handle;		/* usage table's DSA handle */
	dshash_table_handle dshash_handle;	/* usage table's dshash handle */
} GlobalTempRelShmemControl;

static GlobalTempRelShmemControl *gtr_shmem_control;

/*
 * GlobalTempRelShmemCallbacks
 *
 *	Callbacks to register our shared memory requirements and initialize it.
 */
static void
gtr_shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "Global Temporary Relation Usage Table",
					   .size = sizeof(GlobalTempRelShmemControl),
					   .ptr = (void **) &gtr_shmem_control,
		);
}

static void
gtr_shmem_init(void *arg)
{
	gtr_shmem_control->dsa_handle = DSA_HANDLE_INVALID;
	gtr_shmem_control->dshash_handle = DSHASH_HANDLE_INVALID;
}

const ShmemCallbacks GlobalTempRelShmemCallbacks = {
	.request_fn = gtr_shmem_request,
	.init_fn = gtr_shmem_init,
};

/*
 * gtr_delete_all_storage_on_exit
 *
 *	Backend exit callback to delete all local storage created for global
 *	temporary relations in this backend.
 *
 *	NOTE: Storage is deleted non-transactionally, and cannot be rolled back.
 *	This is fine for an exit callback, but not for any other purposes.
 */
static void
gtr_delete_all_storage_on_exit(int code, Datum arg)
{
	ProcNumber	backend;
	HASH_SEQ_STATUS status;
	GtrStorageEntry *entry;

	/* Loop over all storage created and delete it */
	backend = ProcNumberForTempRelations();
	hash_seq_init(&status, gtr_local_storage);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		SMgrRelation srel;

		srel = smgropen(entry->rlocator, backend);
		smgrdounlinkall(&srel, 1, false);
		smgrclose(srel);
	}
}

/*
 * gtr_init_storage_table
 *
 *	Initialize the hash table recording local storage created for global
 *	temporary relations, if not already done.
 */
static void
gtr_init_storage_table(void)
{
	if (gtr_local_storage == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(GtrStorageEntry);

		gtr_local_storage =
			hash_create("Global temporary relation storage table",
						128, &ctl, HASH_ELEM | HASH_BLOBS);

		/* Register callback to delete all local storage on exit */
		before_shmem_exit(gtr_delete_all_storage_on_exit, 0);
	}
}

/*
 * gtr_storage_dropped
 *
 *	Invalidate a global temporary relation whose storage has been dropped.
 *
 *	This is called as part of AtEO(Sub)Xact cleanup if storage creation is
 *	rolled back, or storage deletion is committed.  This can happen several
 *	different ways:
 *
 *	- The relation was initialized in a transaction which was then rolled
 *	  back, causing the local storage created during initialization to be
 *	  dropped.
 *
 *	- An operation like REPACK or TRUNCATE was committed and the old storage
 *	  was dropped.
 *
 *	- An operation like REPACK or TRUNCATE was rolled back and the new storage
 *	  was dropped.  The old storage may or may not still exist, depending on
 *	  when it was created.
 *
 *	- The table itself was dropped.
 *
 *	Here, we have no way to distinguish between these cases, so we just mark
 *	the relation's relcache entry as invalid (if it still has one), forcing it
 *	to be reloaded and reinitialized when it is next opened.  New storage for
 *	the relation will then be created, if needed.
 */
static void
gtr_storage_dropped(Oid relid, RelFileLocator rlocator)
{
	/* If the relation is still in the relcache, mark it as invalid */
	RelationMarkInvalid(relid);

	/*
	 * Remove the hash entry for the dropped storage, forcing the relation to
	 * create new storage if its relfilenode points to this storage after it
	 * is reloaded.
	 */
	(void) hash_search(gtr_local_storage, &rlocator, HASH_REMOVE, NULL);
}

/*
 * AtEOXact_StorageCleanup
 *
 *	Clean up the storage record for a single global temporary relation at
 *	main-transaction commit or abort.
 *
 *	NB: this processing must be idempotent, because EOXactStorageListAdd()
 *	doesn't bother to prevent duplicate entries in eoxact_storage_list[].
 */
static void
AtEOXact_StorageCleanup(GtrStorageEntry *entry, bool isCommit)
{
	/*
	 * If the storage no longer exists after this transaction ends, the global
	 * temporary relation that was using it may no longer have any storage.
	 * Mark the relation as invalid and remove the storage hash entry, forcing
	 * the relation to be reinitialized and have new storage created, if
	 * necessary, when it is next loaded.  Otherwise, reset the hash entry's
	 * subids to InvalidSubTransactionId.
	 */
	if ((isCommit && entry->dropped_subid != InvalidSubTransactionId) ||
		(!isCommit && entry->created_subid != InvalidSubTransactionId))
	{
		gtr_storage_dropped(entry->relid, entry->rlocator);
	}
	else
	{
		entry->created_subid = InvalidSubTransactionId;
		entry->dropped_subid = InvalidSubTransactionId;
	}
}

/*
 * AtEOSubXact_StorageCleanup
 *
 *	Clean up the storage record for a single global temporary relation at
 *	subtransaction commit or abort.
 *
 *	NB: this processing must be idempotent, because EOXactStorageListAdd()
 *	doesn't bother to prevent duplicate entries in eoxact_storage_list[].
 */
static void
AtEOSubXact_StorageCleanup(GtrStorageEntry *entry, bool isCommit,
						   SubTransactionId mySubid,
						   SubTransactionId parentSubid)
{
	/*
	 * Is it storage created in the current subtransaction?
	 *
	 * During subcommit, mark it as belonging to the parent, instead, as long
	 * as it has not been deleted.  Otherwise, the global temporary relation
	 * that was using this storage may no longer have any storage; mark the
	 * relation as invalid and remove the storage hash entry, forcing the
	 * relation to be reinitialized and have new storage created, if
	 * necessary.
	 */
	if (entry->created_subid == mySubid)
	{
		Assert(entry->dropped_subid == mySubid ||
			   entry->dropped_subid == InvalidSubTransactionId);

		if (isCommit && entry->dropped_subid == InvalidSubTransactionId)
			entry->created_subid = parentSubid;
		else
		{
			gtr_storage_dropped(entry->relid, entry->rlocator);
			return;
		}
	}

	/* Update the storage dropped subid */
	if (entry->dropped_subid == mySubid)
	{
		if (isCommit)
			entry->dropped_subid = parentSubid;
		else
			entry->dropped_subid = InvalidSubTransactionId;
	}
}

/*
 * gtr_remove_all_usage_on_exit
 *
 *	Backend exit callback to remove all records of this backend's use of
 *	global temporary relations from the shared usage hash table.
 */
static void
gtr_remove_all_usage_on_exit(int code, Datum arg)
{
	HASH_SEQ_STATUS status;
	GtrUsageEntry *local_entry;

	/* Loop over all the global temporary relations we were using */
	hash_seq_init(&status, gtr_local_usage);
	while ((local_entry = hash_seq_search(&status)) != NULL)
	{
		GtrSharedUsageKey key;
		GtrSharedUsageEntry *shared_entry;

		/*
		 * Remove the local usage entry.  This might seem unnecessary on exit,
		 * but it is possible for gtr_remove_usage() to run after this, so the
		 * local and shared usage entries do need to be kept in sync.
		 */
		(void) hash_search(gtr_local_usage,
						   &local_entry->relid, HASH_REMOVE, NULL);

		/* Find the shared usage entry */
		key.dbid = MyDatabaseId;
		key.relid = local_entry->relid;
		shared_entry = dshash_find(gtr_shared_usage, &key, true);
		if (shared_entry == NULL)
			continue;			/* should be impossible, but tolerate it */

		if (shared_entry->usage_count > 1)
		{
			/* Other backends are still using the relation */
			shared_entry->usage_count--;
			dshash_release_lock(gtr_shared_usage, shared_entry);
		}
		else
		{
			/* No more backends using it */
			dshash_delete_entry(gtr_shared_usage, shared_entry);
		}
	}
}

/*
 * gtr_init_usage_tables
 *
 *	Initialize the local and shared usage hash tables for global temporary
 *	relations, if not already done.
 */
static void
gtr_init_usage_tables(void)
{
	/* Local usage table */
	if (gtr_local_usage == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(GtrUsageEntry);

		gtr_local_usage = hash_create("Global temporary relations in use locally",
									  128, &ctl, HASH_ELEM | HASH_BLOBS);
	}

	/* Shared usage table */
	if (gtr_shared_usage == NULL)
	{
		MemoryContext oldcontext;

		/* Use a lock to ensure only one process creates the table */
		LWLockAcquire(GlobalTempRelControlLock, LW_EXCLUSIVE);

		/* Be sure any local memory allocated by DSA routines is persistent */
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		if (gtr_shmem_control->dshash_handle == DSA_HANDLE_INVALID)
		{
			/* Initialize dynamic shared hash table to track shared usage */
			gtr_shared_usage_dsa = dsa_create(LWTRANCHE_GLOBAL_TEMP_REL_DSA);
			dsa_pin(gtr_shared_usage_dsa);
			dsa_pin_mapping(gtr_shared_usage_dsa);

			gtr_shared_usage = dshash_create(gtr_shared_usage_dsa,
											 &gtr_shared_usage_params, NULL);

			/* Store handles in shared memory for other backends to use */
			gtr_shmem_control->dsa_handle = dsa_get_handle(gtr_shared_usage_dsa);
			gtr_shmem_control->dshash_handle =
				dshash_get_hash_table_handle(gtr_shared_usage);
		}
		else
		{
			/* Attach to existing dynamic shared hash table */
			gtr_shared_usage_dsa = dsa_attach(gtr_shmem_control->dsa_handle);
			dsa_pin_mapping(gtr_shared_usage_dsa);

			gtr_shared_usage = dshash_attach(gtr_shared_usage_dsa,
											 &gtr_shared_usage_params,
											 gtr_shmem_control->dshash_handle,
											 NULL);
		}

		MemoryContextSwitchTo(oldcontext);
		LWLockRelease(GlobalTempRelControlLock);

		/* Register callback to remove all our usage records on exit */
		before_shmem_exit(gtr_remove_all_usage_on_exit, 0);
	}
}

/*
 * gtr_record_usage
 *
 *	Record the fact that we're using a global temporary relation by adding
 *	entries to the local and shared usage hash tables.
 *
 *	Note: This is intentionally idempotent --- it does nothing if we already
 *	have usage records for this relation.
 */
static void
gtr_record_usage(Oid relid, char relkind)
{
	GtrUsageEntry *local_entry;
	GtrSharedUsageKey key;
	GtrSharedUsageEntry *shared_entry;
	bool		found;

	/* Initialize the usage tables, if necessary */
	gtr_init_usage_tables();

	/* Add local usage entry, if not already there */
	local_entry = hash_search(gtr_local_usage, &relid, HASH_ENTER, &found);
	if (found)
		return;					/* already recorded, nothing to do */

	/*
	 * For a sequence, the storage is created non-transactionally, and isn't
	 * deleted on (sub)rollback, and so the sequence is not invalidated and
	 * reinitialized after (sub)rollback.  Do the same for the usage record,
	 * so that we always regard a sequence with storage as in use.  Otherwise,
	 * for any other relkind, record the usage as starting in the current
	 * subtransaction, and flag it for eoxact cleanup.
	 */
	if (relkind == RELKIND_SEQUENCE)
		local_entry->started_subid = InvalidSubTransactionId;
	else
	{
		local_entry->started_subid = GetCurrentSubTransactionId();
		EOXactUsageListAdd(relid);
	}
	local_entry->stopped_subid = InvalidSubTransactionId;

	/* Remember the relation's relkind */
	local_entry->relkind = relkind;

	/* Add/update shared usage entry */
	key.dbid = MyDatabaseId;
	key.relid = relid;
	shared_entry = dshash_find_or_insert_extended(gtr_shared_usage,
												  &key, &found,
												  DSHASH_INSERT_NO_OOM);
	if (shared_entry == NULL)
	{
		/* Remove the local usage entry, so the hash tables remain in sync */
		hash_search(gtr_local_usage, &relid, HASH_REMOVE, NULL);
		ereport(ERROR,
				errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("out of memory"),
				errdetail("Could not insert global temporary table usage entry into shared hash table."));
	}

	if (found)
		shared_entry->usage_count++;
	else
		shared_entry->usage_count = 1;

	dshash_release_lock(gtr_shared_usage, shared_entry);
}

/*
 * gtr_remove_usage
 *
 *	Remove our usage records for a global temporary relation that we're no
 *	longer using.
 *
 *	Note: This is intentionally idempotent --- it does nothing if we have
 *	already removed the relation's usage records.
 */
static void
gtr_remove_usage(Oid relid)
{
	GtrSharedUsageKey key;
	GtrSharedUsageEntry *shared_entry;

	/* Initialize the usage tables, if necessary */
	gtr_init_usage_tables();

	/* Remove local usage entry */
	if (!hash_search(gtr_local_usage, &relid, HASH_REMOVE, NULL))
		return;					/* nothing to do */

	/* Update/delete shared usage entry */
	key.dbid = MyDatabaseId;
	key.relid = relid;
	shared_entry = dshash_find(gtr_shared_usage, &key, true);
	if (shared_entry == NULL)
		return;					/* should be impossible, but tolerate it */

	if (shared_entry->usage_count > 1)
	{
		/* Other backends are still using the relation */
		shared_entry->usage_count--;
		dshash_release_lock(gtr_shared_usage, shared_entry);
	}
	else
	{
		/* No more backends using it */
		dshash_delete_entry(gtr_shared_usage, shared_entry);
	}
}

/*
 * gtr_finalize_discard
 *
 *	Final stage of DISCARD GLOBAL TEMP/TEMPORARY.  This is invoked on commit,
 *	if no additional user global temporary relations were created or reopened
 *	after the DISCARD --- see DiscardGlobalTempRelations().
 */
static void
gtr_finalize_discard(void)
{
	HASH_SEQ_STATUS status;
	GtrUsageEntry *usage_entry;

	/*
	 * By the time we get here, all storage for user-defined global temporary
	 * relations should have been deleted.  Delete all remaining storage (for
	 * global temporary system catalog relations).
	 */
	if (gtr_local_storage)
	{
		ProcNumber	backend;
		GtrStorageEntry *storage_entry;

		backend = ProcNumberForTempRelations();

		hash_seq_init(&status, gtr_local_storage);
		while ((storage_entry = hash_seq_search(&status)) != NULL)
		{
			SMgrRelation srel;

			Assert(IsCatalogRelationOid(storage_entry->relid));

			srel = smgropen(storage_entry->rlocator, backend);
			smgrdounlinkall(&srel, 1, false);
			smgrclose(srel);

			(void) hash_search(gtr_local_storage, &storage_entry->rlocator,
							   HASH_REMOVE, NULL);

			RelationMarkInvalid(storage_entry->relid);
		}
	}

	/* Remove all usage records */
	hash_seq_init(&status, gtr_local_usage);
	while ((usage_entry = hash_seq_search(&status)) != NULL)
	{
		gtr_remove_usage(usage_entry->relid);
		RelationMarkInvalid(usage_entry->relid);
	}

	/* Discard all cached pg_temp_class and pg_temp_index tuples */
	GTCatCacheDiscard();

	/* Reset tempfrozenxid and tempminmxid for this backend */
	MyProc->tempfrozenxid = InvalidTransactionId;
	MyProc->tempminmxid = InvalidMultiXactId;
}

/*
 * AtEOXact_UsageCleanup
 *
 *	Clean up the usage records for a single global temporary relation at
 *	main-transaction commit or abort.
 *
 *	NB: this processing must be idempotent, because EOXactUsageListAdd()
 *	doesn't bother to prevent duplicate entries in eoxact_usage_list[].
 */
static void
AtEOXact_UsageCleanup(GtrUsageEntry *entry, bool isCommit)
{
	/*
	 * If the relation is no longer in use after this transaction ends, remove
	 * the usage hash table entries for it.  Otherwise, reset the hash entry's
	 * subids to InvalidSubTransactionId.
	 */
	if ((isCommit && entry->stopped_subid != InvalidSubTransactionId) ||
		(!isCommit && entry->started_subid != InvalidSubTransactionId))
	{
		gtr_remove_usage(entry->relid);
	}
	else
	{
		entry->started_subid = InvalidSubTransactionId;
		entry->stopped_subid = InvalidSubTransactionId;
	}
}

/*
 * AtEOSubXact_UsageCleanup
 *
 *	Clean up the usage records for a single global temporary relation at
 *	subtransaction commit or abort.
 *
 *	NB: this processing must be idempotent, because EOXactUsageListAdd()
 *	doesn't bother to prevent duplicate entries in eoxact_usage_list[].
 */
static void
AtEOSubXact_UsageCleanup(GtrUsageEntry *entry, bool isCommit,
						 SubTransactionId mySubid,
						 SubTransactionId parentSubid)
{
	/*
	 * Did usage start in the current subtransaction?
	 *
	 * During subcommit, mark it as starting in the parent, instead, as long
	 * as it has not been stopped.  Otherwise, the global temporary relation
	 * is no longer in use.
	 */
	if (entry->started_subid == mySubid)
	{
		Assert(entry->stopped_subid == mySubid ||
			   entry->stopped_subid == InvalidSubTransactionId);

		if (isCommit && entry->stopped_subid == InvalidSubTransactionId)
			entry->started_subid = parentSubid;
		else
		{
			gtr_remove_usage(entry->relid);
			return;
		}
	}

	/* Update the usage stopped subid */
	if (entry->stopped_subid == mySubid)
	{
		if (isCommit)
			entry->stopped_subid = parentSubid;
		else
			entry->stopped_subid = InvalidSubTransactionId;
	}
}

/*
 * TrackGlobalTempRelationStorage
 *
 *	Track about-to-be-created or scheduled-to-be-deleted storage for a global
 *	temporary relation, and arrange for all storage created to be deleted on
 *	backend exit.
 *
 *	For about-to-be-created storage, if register_delete is true (the normal
 *	case), the storage creation is transactional, and it will be deleted on
 *	rollback.  If register_delete is false, the storage will not be deleted on
 *	rollback (used for sequences).
 *
 *	This is called for global temporary relations whenever storage is created
 *	using RelationCreateStorage() or deleted using RelationDropStorage().
 */
void
TrackGlobalTempRelationStorage(Oid relid, RelFileLocator rlocator,
							   ProcNumber backend, bool create,
							   bool register_delete)
{
	GtrStorageEntry *entry;

	if (create)
	{
		bool		found;
		SMgrRelation srel;

		/* Initialize the storage table, if necessary */
		gtr_init_storage_table();

		/* Insert an entry to track the storage */
		entry = hash_search(gtr_local_storage, &rlocator, HASH_ENTER, &found);
		if (found)
			elog(ERROR, "Storage already exists for relation %u", relid);

		/*
		 * We're about to create storage for a global temporary relation.
		 * First, check if storage already exists and if so, delete it --- can
		 * happen if a previous backend with the same ProcNumber crashed, and
		 * RemovePgTempFiles() didn't delete it.  The old storage is deleted
		 * non-transactionally, so this is never rolled back.
		 */
		srel = smgropen(rlocator, backend);
		if (smgrexists(srel, MAIN_FORKNUM))
			smgrdounlinkall(&srel, 1, false);
		smgrclose(srel);

		/*
		 * If register_delete is true, mark the storage as created in the
		 * current subtransaction, so that it is deleted on rollback, and flag
		 * it for eoxact cleanup.
		 */
		entry->relid = relid;
		if (register_delete)
		{
			entry->created_subid = GetCurrentSubTransactionId();
			EOXactStorageListAdd(rlocator);
		}
		else
			entry->created_subid = InvalidSubTransactionId;

		entry->dropped_subid = InvalidSubTransactionId;
	}
	else
	{
		/* Mark the storage as deleted in the current subtransaction */
		entry = gtr_local_storage == NULL ? NULL :
			hash_search(gtr_local_storage, &rlocator, HASH_FIND, NULL);
		if (entry == NULL)
			elog(ERROR, "Storage not found for relation %u", relid);

		entry->dropped_subid = GetCurrentSubTransactionId();

		/* Flag the storage for eoxact cleanup */
		EOXactStorageListAdd(rlocator);
	}
}

/*
 * ReassignGlobalTempRelationStorage
 *
 *	Reassign global temporary relation storage to a different relation.  This
 *	is needed for operations such as ALTER TABLE and REPACK, that rewrite a
 *	relation's contents by building a transient relation and then swapping its
 *	storage with the original relation.  We must mark the new storage as
 *	belonging to the original relation here, otherwise it would be deleted
 *	when the transient relation is dropped.
 *
 *	Note: we have no way of undoing this reassignment in case of rollback, so
 *	we do not assign the original storage to the transient relation, since
 *	that would leave it in an invalid state after rollback.  This isn't a
 *	problem for the new storage, since that is dropped on rollback.  Thus,
 *	this operates in the same way as TRUNCATE, in that both the old and new
 *	storage are temporarily marked as belonging to the same relation.  On
 *	commit, the old storage is dropped and the relation is left pointing to
 *	the new storage, and on rollback the new storage is dropped and the
 *	relcache entry is reloaded and made to point to the old storage.
 */
void
ReassignGlobalTempRelationStorage(RelFileLocator rlocator,
								  Oid newRelid)
{
	GtrStorageEntry *entry;

	/* Must already be tracking the storage */
	entry = hash_search(gtr_local_storage, &rlocator, HASH_FIND, NULL);
	if (entry == NULL)
		elog(ERROR, "could not find global temp relation storage {spcOid: %u, dbOid: %u, relNumber: %u}",
			 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber);

	/* Reassign it */
	entry->relid = newRelid;
}

/*
 * InitGlobalTempRelation
 *
 *	Initialize a global temporary relation for use in this backend, if we
 *	haven't already done so.
 *
 *	NB: this processing must be idempotent, because it is called both when
 *	opening a global temporary relation for the first time, and after a
 *	relcache invalidation.  The relation may have been created in this
 *	backend, or in some other backend.  Thus, it may or may not already have
 *	storage and/or usage records (the existence of one does not imply the
 *	other).
 */
void
InitGlobalTempRelation(Relation relation)
{
	/*
	 * Cannot create storage during parallel operation.  Checks in the planner
	 * should prevent this happening directly from core code during query
	 * execution, but some SQL-callable functions, such as pg_table_size(),
	 * may open global temporary relations from parallel workers.  In that
	 * case, if the relation hasn't already been initialized, we leave it
	 * uninitialized, with no storage or usage records, and error out if such
	 * a function actually attempts to read from the relation.
	 */
	if (IsInParallelMode() || IsParallelWorker())
		return;

	/*
	 * Create storage for the relation, if it has none.  Relations created in
	 * this backend will already have storage, but relations created in other
	 * backends won't, when we see them for the first time.
	 */
	if (RELKIND_HAS_STORAGE(relation->rd_rel->relkind) &&
		(gtr_local_storage == NULL ||
		 hash_search(gtr_local_storage,
					 &relation->rd_locator, HASH_FIND, NULL) == NULL))
	{
		/*
		 * Create (and track) storage for the relation.  For a sequence, the
		 * storage is created non-transactionally, so that the initialization
		 * survives rollback and, as for a permanent sequence, rollback
		 * doesn't cause a sequence restart.  Otherwise, for other relkinds,
		 * the storage is created transactionally.
		 */
		if (RELKIND_HAS_TABLE_AM(relation->rd_rel->relkind))
			table_relation_set_new_filelocator(relation,
											   &relation->rd_locator,
											   relation->rd_rel->relpersistence,
											   &relation->rd_rel->relfrozenxid,
											   &relation->rd_rel->relminmxid);
		else
			RelationCreateStorage(relation->rd_id,
								  relation->rd_locator,
								  relation->rd_rel->relpersistence,
								  relation->rd_rel->relkind != RELKIND_SEQUENCE);

		/*
		 * Register the relation's ON COMMIT action, if it's DELETE ROWS (may
		 * be NONE, PRESERVE ROWS, or DELETE ROWS, but mustn't be DROP).
		 */
		Assert(relation->rd_rel->reloncommit == RELONCOMMIT_NONE ||
			   relation->rd_rel->reloncommit == RELONCOMMIT_PRESERVE_ROWS ||
			   relation->rd_rel->reloncommit == RELONCOMMIT_DELETE_ROWS);

		if (relation->rd_rel->reloncommit == RELONCOMMIT_DELETE_ROWS)
			register_on_commit_action(relation->rd_id, ONCOMMIT_DELETE_ROWS);

		/*
		 * If it's an index, build an empty index in the main fork.
		 *
		 * If the table is not empty (can happen if another session added the
		 * index after we populated the table), then mark it as invalid.  The
		 * user will need to do a REINDEX to build it.
		 *
		 * Note that this check for a non-empty table, using the block count,
		 * might give a false positive, if the table contains deleted tuples,
		 * which would force an unnecessary reindex, but it doesn't seem worth
		 * the effort to do a more thorough check.
		 */
		if (relation->rd_rel->relkind == RELKIND_INDEX)
		{
			Relation	heapRelation;
			BlockNumber nblocks;

			relation->rd_indam->ambuildempty(relation, MAIN_FORKNUM);

			heapRelation = table_open(relation->rd_index->indrelid, AccessShareLock);
			nblocks = RelationGetNumberOfBlocks(heapRelation);
			table_close(heapRelation, AccessShareLock);

			if (nblocks > 0)
				relation->rd_index->indisvalid = false;
		}

		/* If it's a sequence, initialize it */
		if (relation->rd_rel->relkind == RELKIND_SEQUENCE)
			InitGlobalTempSequence(relation);
	}

	/* Track our use of the relation, if we haven't already done so */
	TrackGlobalTempRelation(relation);
}

/*
 * TrackGlobalTempRelation
 *
 *	Track our use of a global temporary relation, if we haven't already done
 *	so.
 *
 *	NB: this processing must be idempotent, because it is called both when a
 *	global temporary relation is created in this session, and when one that
 *	was created by some other backend is opened for the first time, as well as
 *	after a relcache invalidation.
 */
void
TrackGlobalTempRelation(Relation relation)
{
	/*
	 * Record our use of the relation and insert a pg_temp_class tuple for it.
	 * We arrange things so that the presence of a usage record implies the
	 * presence of a pg_temp_class tuple and vice versa, so it's sufficient to
	 * do just one hash table lookup.
	 */
	if (gtr_local_usage == NULL ||
		hash_search(gtr_local_usage,
					&relation->rd_id, HASH_FIND, NULL) == NULL)
	{
		gtr_record_usage(relation->rd_id, relation->rd_rel->relkind);
		InsertPgTempClassTuple(relation);

		/* For an index, also insert a pg_temp_index tuple */
		if (relation->rd_rel->relkind == RELKIND_INDEX ||
			relation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		{
			/*
			 * When creating a new index locally, relation->rd_index will be
			 * NULL here.  Mark it as valid for now --- UpdateIndexRelation()
			 * will update it later, if it's not actually valid (e.g., CREATE
			 * INDEX ... ON ONLY ...).  Otherwise, for an index created in
			 * another session, relation->rd_index->indisvalid will accurately
			 * reflect whether or not the index needs to be marked invalid
			 * locally (if our instance of the index's table is not empty).
			 */
			InsertPgTempIndexTuple(relation->rd_id,
								   relation->rd_index == NULL ||
								   relation->rd_index->indisvalid);
		}

		/*
		 * If this backend's tempfrozenxid and tempminmxid haven't been set
		 * yet (this is the first global temporary relation accessed in this
		 * session), then update them to account for this relation.  If they
		 * have been set, it must have been from an earlier transaction, so
		 * this relation will not affect them.
		 */
		if (!TransactionIdIsValid(MyProc->tempfrozenxid) ||
			!MultiXactIdIsValid(MyProc->tempminmxid))
			UpdateTempFrozenXids();
	}
	Assert(PgTempClassTupleExists(relation->rd_id));
}

/*
 * ForgetGlobalTempRelation
 *
 *	Forget our use of a global temporary relation that we have dropped.
 */
void
ForgetGlobalTempRelation(Oid relid)
{
	GtrUsageEntry *entry;

	Assert(gtr_local_usage != NULL);

	/*
	 * Mark the relation's usage as ending in the current subtransaction, and
	 * flag it for eoxact cleanup.
	 */
	entry = hash_search(gtr_local_usage, &relid, HASH_FIND, NULL);
	Assert(entry != NULL && entry->stopped_subid == InvalidSubTransactionId);

	entry->stopped_subid = GetCurrentSubTransactionId();
	EOXactUsageListAdd(relid);

	/* Delete its pg_temp_class and pg_temp_index tuples */
	DeletePgTempClassTuple(relid);

	if (entry->relkind == RELKIND_INDEX ||
		entry->relkind == RELKIND_PARTITIONED_INDEX)
		DeletePgTempIndexTuple(relid);

	/* Update this backend's tempfrozenxid and tempminmxid */
	UpdateTempFrozenXids();
}

/*
 * InvalidateGlobalTempRelation
 *
 *	Accept an invalidation message for a relation.
 *
 *	We are only interested in global temporary relations that we are currently
 *	using, but the relcache will call this for all invalidated relations, not
 *	just global temporary relations, since it has no way of knowing the
 *	difference for relations no longer in its cache.  We filter out the ones
 *	we're not interested in, and process them later in
 *	ProcessInvalidatedGlobalTempRelations().
 *
 *	For a whole-relcache invalidation, RelationCacheInvalidate() will invoke
 *	this with relid = InvalidOid.
 */
void
InvalidateGlobalTempRelation(Oid relid)
{
	MemoryContext oldcontext;

	/* Quick exit if we haven't used any global temporary relations */
	if (gtr_local_usage == NULL)
		return;

	/* Be sure any memory allocated for gtrs_invalidated is persistent */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * We can't do any DB access here, so just make a record of the
	 * invalidations that might be of interest to us (those for in-use global
	 * temporary relations).  We don't care about global temporary relations
	 * that we haven't touched, or any other types of relations.
	 */
	if (OidIsValid(relid))
	{
		/* Invalidate rel if it's a locally in-use global temp relation */
		if (hash_search(gtr_local_usage, &relid, HASH_FIND, NULL))
		{
			gtrs_invalidated = list_append_unique_oid(gtrs_invalidated, relid);
		}
	}
	else
	{
		HASH_SEQ_STATUS status;
		GtrUsageEntry *entry;

		/* Invalidate all global temporary relations in use locally */
		hash_seq_init(&status, gtr_local_usage);
		while ((entry = hash_seq_search(&status)) != NULL)
		{
			gtrs_invalidated = list_append_unique_oid(gtrs_invalidated,
													  entry->relid);
		}
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * ProcessInvalidatedGlobalTempRelations
 *
 *	Process any invalidated global temporary relations, dealing with any that
 *	have been dropped by other backends.  Global temporary relations dropped by
 *	this backend need no additional processing, and are ignored.
 */
void
ProcessInvalidatedGlobalTempRelations(void)
{
	/* Prevent infinite recursion */
	if (processing_invalidated_gtrs)
		return;

	processing_invalidated_gtrs = true;

	/*
	 * Scan the list of invalidated global temporary relations for any more
	 * relations dropped by other backends (may already have found some in a
	 * prior invocation).
	 *
	 * As we scan gtrs_invalidated, more invalidation messages may arrive and
	 * be added to the end of the list, so we need to be prepared for the list
	 * growing as we traverse it.
	 */
	if (gtrs_invalidated)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		for (int i = 0; i < list_length(gtrs_invalidated); i++)
		{
			Oid			relid = list_nth_oid(gtrs_invalidated, i);
			GtrUsageEntry *entry;

			/* Ignore relations we've already found */
			if (list_member_oid(gtrs_dropped, relid))
				continue;

			/* Ignore relations we've already forgotten (dropped by us) */
			entry = hash_search(gtr_local_usage, &relid, HASH_FIND, NULL);
			if (entry == NULL || entry->stopped_subid != InvalidSubTransactionId)
				continue;

			/* Ignore relations that still exist */
			if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
				continue;

			/* Relation dropped by another backend; add it to the list */
			gtrs_dropped = lappend_oid(gtrs_dropped, relid);

			/*
			 * Clear processed_dropped_subid; we have no longer processed all
			 * the dropped relations.
			 */
			processed_dropped_subid = InvalidSubTransactionId;
		}

		/* All invalidation messages processed; clear the list */
		list_free(gtrs_invalidated);
		gtrs_invalidated = NIL;

		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Process any dropped relations, if we haven't done so already.  If the
	 * (sub)transaction is rolled back, this needs to be repeated, so we don't
	 * clear gtrs_dropped here (it is only cleared upon successful commit),
	 * but we do set processed_dropped_subid, so that we don't needlessly
	 * repeat this later in the same transaction.
	 */
	if (gtrs_dropped && processed_dropped_subid == InvalidSubTransactionId)
	{
		bool		tuples_deleted = false;
		Relation	statrel;
		SysScanDesc scan;
		HeapTuple	tuple;

		/*
		 * Delete and forget locally-created storage for dropped relations.
		 * This is done non-transactionally, since gtrs_dropped contains only
		 * relations dropped by other backends in committed transactions, so
		 * this is never rolled back.
		 */
		if (gtr_local_storage != NULL)
		{
			ProcNumber	backend;
			HASH_SEQ_STATUS status;
			GtrStorageEntry *entry;

			backend = ProcNumberForTempRelations();
			hash_seq_init(&status, gtr_local_storage);
			while ((entry = hash_seq_search(&status)) != NULL)
			{
				if (list_member_oid(gtrs_dropped, entry->relid))
				{
					SMgrRelation srel;

					srel = smgropen(entry->rlocator, backend);
					smgrdounlinkall(&srel, 1, false);
					smgrclose(srel);

					(void) hash_search(gtr_local_storage, &entry->rlocator,
									   HASH_REMOVE, NULL);
				}
			}
		}

		/*
		 * Remove all usage records, forget any ON COMMIT actions, and delete
		 * any temporary catalog entries for the dropped relations.  The usage
		 * record removal is non-transactional, but the rest may be undone by
		 * (sub)rollback.
		 */
		statrel = table_open(TempStatisticRelationId, RowExclusiveLock);

		foreach_oid(relid, gtrs_dropped)
		{
			ScanKeyData key[1];

			gtr_remove_usage(relid);
			remove_on_commit_action(relid);

			/* Delete the relation's pg_temp_class tuple, if it has one */
			if (PgTempClassTupleExists(relid))
			{
				DeletePgTempClassTuple(relid);
				tuples_deleted = true;
			}

			/* For an index, delete its pg_temp_index tuple, if it has one */
			if (PgTempIndexTupleExists(relid))
			{
				DeletePgTempIndexTuple(relid);
				tuples_deleted = true;
			}

			/* Delete any per-column statistics from pg_temp_statistic */
			ScanKeyInit(&key[0],
						Anum_pg_temp_statistic_starelid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(relid));

			scan = systable_beginscan(statrel,
									  TempStatisticRelidAttnumInhIndexId,
									  true, NULL, 1, key);

			while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			{
				CatalogTupleDelete(statrel, &tuple->t_self);
				tuples_deleted = true;
			}

			systable_endscan(scan);
		}

		table_close(statrel, RowExclusiveLock);

		/*
		 * Delete any orphaned extended stats data from
		 * pg_temp_statistic_ext_data.  This requires a full table scan, since
		 * there is no stxrelid column referring back to the relation.  That
		 * also means that it's not strictly limited to gtrs_dropped, but
		 * that's probably no bad thing --- it will tidy up *any* orphaned
		 * global temporary extended stats data, though in theory, that should
		 * be limited to gtrs_dropped.
		 */
		statrel = table_open(TempStatisticExtDataRelationId, RowExclusiveLock);

		scan = systable_beginscan(statrel, InvalidOid, false, NULL, 0, NULL);

		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Oid			stxoid;

			stxoid = SysCacheGetAttrNotNull(TEMPSTATEXTDATASTXOID, tuple,
											Anum_pg_temp_statistic_ext_data_stxoid);

			if (!SearchSysCacheExists1(STATEXTOID, ObjectIdGetDatum(stxoid)))
			{
				CatalogTupleDelete(statrel, &tuple->t_self);
				tuples_deleted = true;
			}
		}

		systable_endscan(scan);

		table_close(statrel, RowExclusiveLock);

		/* If we deleted anything, make the changes visible */
		if (tuples_deleted)
			CommandCounterIncrement();

		/* Update this backend's tempfrozenxid and tempminmxid */
		UpdateTempFrozenXids();

		/* All dropped relations have been processed, as of this subxact */
		processed_dropped_subid = GetCurrentSubTransactionId();
	}

	/* Done processing */
	processing_invalidated_gtrs = false;
}

/*
 * UpdateTempFrozenXids
 *
 *	Update this backend's tempfrozenxid and tempminmxid values, setting them
 *	to the minimum relfrozenxid and relminmxid values from pg_temp_class.
 *
 *	Note: the updates are deferred until main transaction commit.  This is
 *	necessary, in case some or all of the changes made in this transaction are
 *	rolled back (e.g., a DROP that makes it seem as though tempfrozenxid
 *	and/or tempminmxid can be advanced, only to be rolled back).  Other
 *	backends must only see the final state that we commit.
 */
void
UpdateTempFrozenXids(void)
{
	/* Flag tempfrozenxid and tempminmxid as to-be-updated on commit */
	update_tempfrozenxids = true;
}

/*
 * AtEOXact_GlobalTempRelation
 *
 *	Clean up storage and usage records at main-transaction commit or abort.
 */
void
AtEOXact_GlobalTempRelation(bool isCommit)
{
	HASH_SEQ_STATUS status;
	GtrStorageEntry *storage_entry;
	GtrUsageEntry *usage_entry;

	/*
	 * Unless the eoxact_storage_list[] overflowed, we only need to examine
	 * the storage listed in it.  Otherwise fall back on a hash_seq_search
	 * scan --- see similar code in AtEOXact_RelationCache().
	 */
	if (eoxact_storage_list_overflowed)
	{
		hash_seq_init(&status, gtr_local_storage);
		while ((storage_entry = hash_seq_search(&status)) != NULL)
		{
			AtEOXact_StorageCleanup(storage_entry, isCommit);
		}
	}
	else
	{
		for (int i = 0; i < eoxact_storage_list_len; i++)
		{
			storage_entry = hash_search(gtr_local_storage,
										&eoxact_storage_list[i],
										HASH_FIND, NULL);
			if (storage_entry)
				AtEOXact_StorageCleanup(storage_entry, isCommit);
		}
	}

	/* Similarly, cleanup usage records */
	if (eoxact_usage_list_overflowed)
	{
		hash_seq_init(&status, gtr_local_usage);
		while ((usage_entry = hash_seq_search(&status)) != NULL)
		{
			AtEOXact_UsageCleanup(usage_entry, isCommit);
		}
	}
	else
	{
		for (int i = 0; i < eoxact_usage_list_len; i++)
		{
			usage_entry = hash_search(gtr_local_usage, &eoxact_usage_list[i],
									  HASH_FIND, NULL);
			if (usage_entry)
				AtEOXact_UsageCleanup(usage_entry, isCommit);
		}
	}

	/* Now we're out of the transaction and can clear the lists */
	eoxact_storage_list_len = 0;
	eoxact_storage_list_overflowed = false;
	eoxact_usage_list_len = 0;
	eoxact_usage_list_overflowed = false;

	/*
	 * On commit, clear gtrs_dropped.  Otherwise keep it, so that dropped
	 * relations are processed in the next transaction.
	 */
	if (gtrs_dropped && isCommit)
	{
		list_free(gtrs_dropped);
		gtrs_dropped = NIL;
	}
	processing_invalidated_gtrs = false;
	processed_dropped_subid = InvalidSubTransactionId;

	/*
	 * Are we committing a DISCARD GLOBAL TEMP?
	 *
	 * DiscardGlobalTempRelations() scheduled all storage for user-defined
	 * relations to be deleted, and by this point, all hash table entries for
	 * that storage will have been removed.  However, we must check if there
	 * is any new storage for any user-defined relations, in case any global
	 * temporary relations were created or reopened after the DISCARD.
	 */
	if (discard_subid != InvalidSubTransactionId && isCommit)
	{
		bool		discard_ok = true;

		if (gtr_local_storage)
		{
			hash_seq_init(&status, gtr_local_storage);
			while ((storage_entry = hash_seq_search(&status)) != NULL)
			{
				if (!IsCatalogRelationOid(storage_entry->relid))
				{
					discard_ok = false;
					hash_seq_term(&status);
					break;
				}
			}
		}

		if (discard_ok)
			gtr_finalize_discard();
	}
	discard_subid = InvalidSubTransactionId;

	/* Clean up global temporary catalog caches */
	AtEOXact_GTCatCache(isCommit);

	/*
	 * Finally, on commit, update tempfrozenxid and tempminmxid, if requested.
	 * This must be done after AtEOXact_GTCatCache(), so that it sees the
	 * final state of pg_temp_class.
	 */
	if (update_tempfrozenxids && isCommit)
	{
		TransactionId min_relfrozenxid;
		MultiXactId min_relminmxid;

		GTCatCacheGetMinFrozenXids(&min_relfrozenxid, &min_relminmxid);

		MyProc->tempfrozenxid = min_relfrozenxid;
		MyProc->tempminmxid = min_relminmxid;
	}
	update_tempfrozenxids = false;
}

/*
 * AtEOSubXact_GlobalTempRelation
 *
 *	Clean up storage and usage records at sub-transaction commit or abort.
 */
void
AtEOSubXact_GlobalTempRelation(bool isCommit, SubTransactionId mySubid,
							   SubTransactionId parentSubid)
{
	HASH_SEQ_STATUS status;
	GtrStorageEntry *storage_entry;
	GtrUsageEntry *usage_entry;

	/*
	 * Unless the eoxact_storage_list[] overflowed, we only need to examine
	 * the storage listed in it.  Otherwise fall back on a hash_seq_search
	 * scan.  Same logic as in AtEOXact_GlobalTempRelation().
	 */
	if (eoxact_storage_list_overflowed)
	{
		hash_seq_init(&status, gtr_local_storage);
		while ((storage_entry = hash_seq_search(&status)) != NULL)
		{
			AtEOSubXact_StorageCleanup(storage_entry, isCommit, mySubid,
									   parentSubid);
		}
	}
	else
	{
		for (int i = 0; i < eoxact_storage_list_len; i++)
		{
			storage_entry = hash_search(gtr_local_storage,
										&eoxact_storage_list[i],
										HASH_FIND, NULL);
			if (storage_entry)
				AtEOSubXact_StorageCleanup(storage_entry, isCommit, mySubid,
										   parentSubid);
		}
	}

	/* Similarly, cleanup usage records */
	if (eoxact_usage_list_overflowed)
	{
		hash_seq_init(&status, gtr_local_usage);
		while ((usage_entry = hash_seq_search(&status)) != NULL)
		{
			AtEOSubXact_UsageCleanup(usage_entry, isCommit, mySubid,
									 parentSubid);
		}
	}
	else
	{
		for (int i = 0; i < eoxact_usage_list_len; i++)
		{
			usage_entry = hash_search(gtr_local_usage, &eoxact_usage_list[i],
									  HASH_FIND, NULL);
			if (usage_entry)
				AtEOSubXact_UsageCleanup(usage_entry, isCommit, mySubid,
										 parentSubid);
		}
	}

	/* Update processed_dropped_subid */
	if (processed_dropped_subid == mySubid)
	{
		if (isCommit)
			processed_dropped_subid = parentSubid;
		else
			processed_dropped_subid = InvalidSubTransactionId;
	}

	/* Update discard_subid */
	if (discard_subid == mySubid)
	{
		if (isCommit)
			discard_subid = parentSubid;
		else
			discard_subid = InvalidSubTransactionId;
	}

	/* Clean up global temporary catalog caches */
	AtEOSubXact_GTCatCache(isCommit, mySubid, parentSubid);

	/* Don't reset the lists; we still need more cleanup later */
}

/*
 * IsGlobalTempRelationInUse
 *
 *	Test if the specified global temporary relation is being used by this
 *	backend.
 */
bool
IsGlobalTempRelationInUse(Oid relid)
{
	return gtr_local_usage != NULL &&
		hash_search(gtr_local_usage, &relid, HASH_FIND, NULL) != NULL;
}

/*
 * IsOtherUsingGlobalTempRelation
 *
 *	Test if any other backend is using the specified global temporary
 *	relation.  The caller should have an exclusive lock on the relation, or
 *	else the result could be quickly out-dated.
 */
bool
IsOtherUsingGlobalTempRelation(Oid relid)
{
	bool		used_locally;
	GtrSharedUsageKey key;
	GtrSharedUsageEntry *entry;
	int			usage_count;

	gtr_init_usage_tables();

	/* Are we using the relation? (expect true) */
	(void) hash_search(gtr_local_usage, &relid, HASH_FIND, &used_locally);

	/* Total usage count (including us) */
	key.dbid = MyDatabaseId;
	key.relid = relid;
	entry = dshash_find(gtr_shared_usage, &key, false);

	if (entry)
	{
		usage_count = entry->usage_count;
		Assert(usage_count > 0);
		dshash_release_lock(gtr_shared_usage, entry);
	}
	else
		usage_count = 0;

	return used_locally ? (usage_count > 1) : (usage_count > 0);
}

/*
 * GetAllGlobalTempRelationsInUse
 *
 *	Returns a list of OIDs of all global temporary relations in use (by any
 *	backend, including us) in the specified database (or all databases, if
 *	dbid is InvalidOid).
 *
 *	Note: The result may be almost immediately out-dated.
 */
List *
GetAllGlobalTempRelationsInUse(Oid dbid)
{
	List	   *rels_in_use = NIL;
	dshash_seq_status status;
	GtrSharedUsageEntry *entry;

	gtr_init_usage_tables();

	dshash_seq_init(&status, gtr_shared_usage, false);
	while ((entry = dshash_seq_next(&status)) != NULL)
	{
		Assert(entry->usage_count > 0);
		if (!OidIsValid(dbid) || entry->key.dbid == dbid)
			rels_in_use = lappend_oid(rels_in_use, entry->key.relid);
	}
	dshash_seq_term(&status);

	return rels_in_use;
}

/*
 * DiscardGlobalTempRelations
 *
 *	DISCARD GLOBAL TEMP/TEMPORARY --- delete all storage created for global
 *	temporary relations and remove all usage records, restoring the session to
 *	the state it had before any global temporary relations were opened.
 */
void
DiscardGlobalTempRelations(void)
{
	/*
	 * This is a two stage process.  In the first stage (here), we remove all
	 * storage associated with user-defined global temporary relations, but we
	 * keep their usage records and global temporary system catalog entries.
	 * In the second stage (on main transaction commit), if the DISCARD has
	 * survived without (sub)transaction rollback, and all the user-defined
	 * relations still have no storage (none were created or reopened), we
	 * remove all remaining storage (the storage used by global temporary
	 * system catalog relations) and delete all storage and usage records.
	 */
	if (gtr_local_usage != NULL)
	{
		List	   *relids = NIL;
		HASH_SEQ_STATUS status;
		GtrUsageEntry *entry;

		hash_seq_init(&status, gtr_local_usage);
		while ((entry = hash_seq_search(&status)) != NULL)
		{
			Oid			relid = entry->relid;
			Relation	rel;
			RelFileNumber newrelfilenumber;
			HeapTuple	tuple;
			Form_pg_temp_class form;

			/* Skip system catalogs */
			if (IsCatalogRelationOid(relid))
				continue;

			/* Skip dropped relations */
			if (entry->stopped_subid != InvalidSubTransactionId)
				continue;

			/* Skip relations that don't have storage */
			if (!RELKIND_HAS_STORAGE(entry->relkind))
				continue;

			/*
			 * Schedule the relation's current storage for deletion and
			 * allocate a new relfilenumber, but don't actually create new
			 * storage.  The new storage will be created if it is reopened.
			 */
			rel = relation_open(relid, AccessExclusiveLock);

			newrelfilenumber = GetNewRelFileNumber(rel->rd_rel->reltablespace,
												   NULL,
												   rel->rd_rel->relpersistence);
			RelationDropStorage(rel);

			RelationAssumeNewRelfilelocator(rel);

			/* Forget its ON COMMIT action */
			remove_on_commit_action(relid);

			/* Update its pg_temp_class entry */
			tuple = GetPgTempClassTuple(relid);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "could not find tuple for relation %u", relid);

			form = (Form_pg_temp_class) GETSTRUCT(tuple);
			form->relfilenode = newrelfilenumber;
			form->relpages = 0;
			form->reltuples = 0;
			form->relallvisible = 0;
			form->relallfrozen = 0;

			UpdatePgTempClassTuple(relid, tuple);

			heap_freetuple(tuple);

			relation_close(rel, NoLock);

			relids = lappend_oid(relids, relid);
		}

		/*
		 * Make the pg_temp_class changes visible.  This will cause the
		 * relcache entries to get updated, too.
		 */
		CommandCounterIncrement();

		/*
		 * Mark all the relcache entries for the relations whose storage we
		 * deleted as invalid.  This will force a reload and reinitialize with
		 * new storage the next time they are opened.
		 */
		foreach_oid(relid, relids)
		{
			RelationMarkInvalid(relid);
		}

		/*
		 * Make note of the subtransaction ID in which we did this, so we can
		 * track whether it survives to the end of the main transaction.
		 */
		if (discard_subid == InvalidSubTransactionId)
			discard_subid = GetCurrentSubTransactionId();
	}
}
