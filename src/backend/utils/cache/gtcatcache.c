/*-------------------------------------------------------------------------
 *
 * gtcatcache.c
 *	  Global temporary catalog cache.
 *
 * This caches of the contents of selected global temporary catalog tables,
 * holding details about all global temporary relations in use.
 *
 * Since global temporary relations are reset on backend exit, the contents
 * of these catalogs are themselves temporary.  Additionally, all data is
 * local to this session, and is never invalidated by another session
 * (except if another session drops a global temporary relation, which is
 * handled by ProcessInvalidatedGlobalTempRelations() in global_temp.c).
 * Therefore, tuples added to these caches are kept until the end of the
 * session, unless explicitly deleted.
 *
 * In addition, the contents of these caches are regarded as the master
 * copies of the data --- tuples added to the caches are not written to the
 * database immediately, but instead, are only written out when necessary.
 * Tuples in the database are updated from the contents of these caches,
 * not the other way round.  This requires all reads and writes to these
 * global temporary system catalogs by backend code to go through this API.
 *
 * One reason for this design is that on a hot standby, or when operating
 * in parallel mode, we cannot write directly to the catalog tables, but we
 * may still open global temporary relations, so we must rely solely on the
 * in-cache tuples.
 *
 * Another reason is that tuples for global temporary sequences must be
 * inserted non-transactionally, but any tuple written to the database
 * might be removed by rollback, so we may need to re-insert a database
 * tuple after rollback of initialization of a global temporary sequence.
 *
 * In addition, this design delays the point at which we have to actually
 * open the underlying catalog tables, which solves the "chicken and egg"
 * bootstrapping problem when opening pg_temp_class for the first time.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/gtcatcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_temp_class.h"
#include "utils/fmgroids.h"
#include "utils/gtcatcache.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/*
 * GTCatCacheEntry
 *
 *	A cache entry holding a single global temporary catalog table tuple.  All
 *	cache entries are keyed by relation OID (these caches are only used for
 *	global temporary catalog tables whose primary key is the global temporary
 *	relation's OID).
 *
 *	If a cache entry is edited in a transaction or subtransaction, a linked
 *	list of previous versions of the entry is built, allowing it to be
 *	restored on rollback or subrollback.
 */
typedef struct GTCatCacheEntry
{
	Oid			relid;			/* lookup key: OID the tuple is for */
	HeapTuple	tuple;			/* cached copy of the tuple */
	bool		written;		/* has tuple been written to the database? */
	bool		deleted;		/* has tuple been deleted? */
	SubTransactionId subid;		/* subxact ID of insert/update/delete/flush */
	struct GTCatCacheEntry *prev;	/* previous version, for (sub)rollback */
} GTCatCacheEntry;

/*
 * A cache entry needs to be flushed if it has been written to the database
 * and subsequently deleted, or it it has not been written to the database and
 * not deleted.  We don't need to worry about updates, because all updates are
 * written to the database immediately.
 */
#define CACHE_ENTRY_NEEDS_FLUSH(entry) ((entry)->written == (entry)->deleted)

/*
 * GTCatCache
 *
 *	A single global temporary catalog cache.
 */
typedef struct GTCatCache
{
	char	   *name;			/* cache name, for debugging purposes */
	Oid			catalog_relid;	/* OID of underlying catalog table */
	Oid			index_relid;	/* OID of catalog table's OID index */
	AttrNumber	key_attno;		/* attno of catalog's key (OID) column */
	SysCacheIdentifier cacheid; /* catalog's syscache ID */
	HTAB	   *hashtable;		/* hash table for catalog tuples */

	/*
	 * List of cache entries that (might) need AtEOXact cleanup work.  As with
	 * the relcache's eoxact_list[], this list intentionally has limited size,
	 * and we switch to a full hash table traversal if the list overflows.
	 */
#define MAX_EOXACT_LIST 32
	Oid			eoxact_list[MAX_EOXACT_LIST];
	int			eoxact_list_len;
	bool		eoxact_list_overflowed;
} GTCatCache;

#define EOXactListAdd(cache, relid) \
	do { \
		if ((cache)->eoxact_list_len < MAX_EOXACT_LIST) \
			(cache)->eoxact_list[(cache)->eoxact_list_len++] = (relid); \
		else \
			(cache)->eoxact_list_overflowed = true; \
	} while (0)

/* Do we have any entries that need to be flushed to the database? */
static bool have_entries_to_flush;

/* Are we currently flushing entries (used to prevent infinite recursion) */
static bool flushing_entries;

/* Memory context for all cached tuples */
static MemoryContext gt_cat_cache_tupctx;

/* The actual caches (the hash tables are lazily built) */
static GTCatCache gt_cat_cache[NUM_GT_CAT_CACHES] = {
	/* PG_TEMP_CLASS */
	{
		.name = "pg_temp_class cache",
		.catalog_relid = TempRelationRelationId,
		.index_relid = TempClassOidIndexId,
		.key_attno = Anum_pg_temp_class_oid,
		.cacheid = TEMPRELOID,
		.hashtable = NULL,
		.eoxact_list_len = 0,
		.eoxact_list_overflowed = false,
	},
};

/*
 * can_flush_catalogs
 *
 *	Returns true if we can flush catalog entries to the database; false if the
 *	database should be considered read-only.
 */
static inline bool
can_flush_catalogs(void)
{
	/* Prevent infinite recursion */
	if (flushing_entries)
		return false;

	/*
	 * A hot standby may open global temporary relations, creating global
	 * temporary catalog entries, but it can never write them out.
	 */
	if (RecoveryInProgress())
		return false;

	/* Similarly, while in parallel mode, the database is read-only */
	if (IsInParallelMode() || IsParallelWorker())
		return false;

	return true;
}

/*
 * initialize_cache
 *
 *	Lazily initialize the specified global temporary catalog cache.
 */
static void
initialize_cache(GTCatCache *cache)
{
	/* Create the cache's hash table, if we haven't done so already */
	if (cache->hashtable == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(GTCatCacheEntry);

		cache->hashtable = hash_create(cache->name, 128, &ctl,
									   HASH_ELEM | HASH_BLOBS);
	}

	/* Create the tuple memory context, if we haven't done so already */
	if (gt_cat_cache_tupctx == NULL)
	{
		gt_cat_cache_tupctx =
			AllocSetContextCreate(TopMemoryContext,
								  "Global temporary catalog cache tuples",
								  ALLOCSET_DEFAULT_SIZES);
	}
}

/*
 * find_and_update_cache_entry
 *
 *	Find and update the cache entry tuple for the specified relation.
 */
static GTCatCacheEntry *
find_and_update_cache_entry(GTCatCache *cache, Oid relid, HeapTuple newtuple)
{
	SubTransactionId mySubid = GetCurrentSubTransactionId();
	GTCatCacheEntry *entry;
	MemoryContext oldcontext;

	/* Find the cache entry; must exist */
	if (cache->hashtable == NULL ||
		(entry = hash_search(cache->hashtable, &relid, HASH_FIND, NULL)) == NULL)
		elog(ERROR, "cache lookup failed for global temp relation %u", relid);

	/* Should not have been deleted */
	if (entry->deleted)
		elog(ERROR, "cache entry for global temp relation %u was deleted", relid);

	Assert(HeapTupleIsValid(entry->tuple));

	/* Update the cache entry, saving a copy for rollback, if necessary */
	oldcontext = MemoryContextSwitchTo(gt_cat_cache_tupctx);

	if (entry->subid != mySubid)
	{
		GTCatCacheEntry *save_entry;

		save_entry = palloc_object(GTCatCacheEntry);
		save_entry->relid = entry->relid;
		save_entry->tuple = entry->tuple;
		save_entry->written = entry->written;
		save_entry->deleted = entry->deleted;
		save_entry->subid = entry->subid;
		save_entry->prev = entry->prev;

		entry->subid = mySubid;
		entry->prev = save_entry;

		/* Flag the entry as needing eoxact cleanup */
		EOXactListAdd(cache, relid);
	}
	else
		heap_freetuple(entry->tuple);

	entry->tuple = heap_copytuple(newtuple);

	MemoryContextSwitchTo(oldcontext);

	return entry;
}

/*
 * flush_cache_entries
 *
 *	Flush all cache entries to their respective database catalogs, inserting
 *	new entries, and removing deleted entries.  We needn't worry about updated
 *	entries, because all updates are written to the database immediately.
 */
static void
flush_cache_entries(void)
{
	SubTransactionId mySubid = GetCurrentSubTransactionId();
	Relation	rel[NUM_GT_CAT_CACHES];
	CatalogIndexState indstate[NUM_GT_CAT_CACHES];
	bool		db_updated = false;

	/* Prevent infinite recursion while flushing */
	Assert(!flushing_entries);
	flushing_entries = true;

	/*
	 * Check whether we actually have any cache entries to flush.  This is
	 * worth doing, because have_entries_to_flush may be a false positive
	 * after (sub)rollback, and we don't want to create global temporary
	 * catalog entries unless we actually need to.
	 */
	have_entries_to_flush = false;

	for (int cacheId = 0; cacheId < NUM_GT_CAT_CACHES; cacheId++)
	{
		GTCatCache *cache = &gt_cat_cache[cacheId];

		if (cache->hashtable != NULL)
		{
			HASH_SEQ_STATUS status;
			GTCatCacheEntry *entry;

			hash_seq_init(&status, cache->hashtable);
			while ((entry = hash_seq_search(&status)) != NULL)
			{
				if (CACHE_ENTRY_NEEDS_FLUSH(entry))
				{
					have_entries_to_flush = true;
					hash_seq_term(&status);
					break;
				}
			}
			if (have_entries_to_flush)
				break;
		}
	}

	if (!have_entries_to_flush)
	{
		flushing_entries = false;
		return;
	}

	/*
	 * Open the catalog tables and their indexes for all the caches.  We do
	 * this before anything else, because doing so might lead to additional
	 * cache entries being inserted, which we would like to write out too.
	 */
	for (int cacheId = 0; cacheId < NUM_GT_CAT_CACHES; cacheId++)
	{
		GTCatCache *cache = &gt_cat_cache[cacheId];

		rel[cacheId] = table_open(cache->catalog_relid, RowExclusiveLock);
		indstate[cacheId] = CatalogOpenIndexes(rel[cacheId]);
	}

	/*
	 * For each cache, write out all entries not already written, and delete
	 * any database tuples for entries written and marked as deleted.
	 */
	for (int cacheId = 0; cacheId < NUM_GT_CAT_CACHES; cacheId++)
	{
		GTCatCache *cache = &gt_cat_cache[cacheId];

		if (cache->hashtable != NULL)
		{
			HASH_SEQ_STATUS status;
			GTCatCacheEntry *entry;

			hash_seq_init(&status, cache->hashtable);
			while ((entry = hash_seq_search(&status)) != NULL)
			{
				/* Ignore entries that don't need flushing */
				if (!CACHE_ENTRY_NEEDS_FLUSH(entry))
					continue;

				/* Delete or insert the tuple, as necessary */
				if (entry->deleted)
				{
					HeapTuple	tuple;

					tuple = SearchSysCache1(cache->cacheid,
											ObjectIdGetDatum(entry->relid));
					if (HeapTupleIsValid(tuple))
					{
						CatalogTupleDelete(rel[cacheId], &tuple->t_self);
						ReleaseSysCache(tuple);
					}
				}
				else
					CatalogTupleInsertWithInfo(rel[cacheId], entry->tuple,
											   indstate[cacheId]);

				/*
				 * Update the entry's written status, saving a copy for
				 * rollback, if necessary.
				 */
				if (entry->subid != mySubid)
				{
					MemoryContext oldcontext;
					GTCatCacheEntry *save_entry;

					oldcontext = MemoryContextSwitchTo(gt_cat_cache_tupctx);

					save_entry = palloc_object(GTCatCacheEntry);
					save_entry->relid = entry->relid;
					save_entry->tuple = heap_copytuple(entry->tuple);
					save_entry->written = entry->written;
					save_entry->deleted = entry->deleted;
					save_entry->subid = entry->subid;
					save_entry->prev = entry->prev;

					entry->subid = mySubid;
					entry->prev = save_entry;

					/* Flag the entry as needing eoxact cleanup */
					EOXactListAdd(cache, entry->relid);

					MemoryContextSwitchTo(oldcontext);
				}

				entry->written = !entry->deleted;
				db_updated = true;
			}
		}
	}

	/* If we made any changes, make them visible */
	if (db_updated)
		CommandCounterIncrement();

	/* Tidy up */
	for (int cacheId = 0; cacheId < NUM_GT_CAT_CACHES; cacheId++)
	{
		CatalogCloseIndexes(indstate[cacheId]);
		table_close(rel[cacheId], RowExclusiveLock);
	}
	have_entries_to_flush = false;
	flushing_entries = false;
}

/*
 * AtEOXact_GTCatCacheEntryCleanup
 *
 *	Clean up a single cache entry at main-transaction commit or abort.
 *
 *	NB: this processing must be idempotent, because EOXactListAdd() doesn't
 *	bother to prevent duplicate entries in eoxact_list[].
 */
static void
AtEOXact_GTCatCacheEntryCleanup(GTCatCache *cache, GTCatCacheEntry *entry,
								bool isCommit)
{
	/*
	 * Was the entry inserted, updated, deleted, or flushed in this
	 * transaction?
	 *
	 * On commit, reset the subid, marking it as no longer belonging to a
	 * transaction, and discard any previous copy of the entry that was saved
	 * in case of rollback.  If the tuple has been deleted in both the cache
	 * and the database, the cache entry is no longer needed, and is removed.
	 *
	 * On rollback of an update, delete, or flush, restore the saved copy
	 * reflecting the state of the entry prior to the transaction.
	 *
	 * Otherwise (rollback of an insert), the tuple no longer exists, and has
	 * been removed from the database, so remove the cache entry.
	 */
	if (entry->subid != InvalidSubTransactionId)
	{
		GTCatCacheEntry *prev = entry->prev;

		/*
		 * If there's a saved copy, it should be the version that existed
		 * prior to this transaction.
		 *
		 * Note: the saved copy might be marked as deleted, and have no tuple
		 * (the change made in this transaction might have been to flush that
		 * delete to the database).
		 */
		Assert(prev == NULL ||
			   (prev->relid == entry->relid &&
				prev->subid == InvalidSubTransactionId &&
				prev->prev == NULL));

		if (isCommit)
		{
			/* Commit of an insert, update, delete, or flush */
			entry->subid = InvalidSubTransactionId;
			entry->prev = NULL;
			if (prev != NULL)
				heap_freetuple(prev->tuple);
			if (entry->deleted && !entry->written)
				hash_search(cache->hashtable, &entry->relid, HASH_REMOVE, NULL);
		}
		else if (prev != NULL)
		{
			/* Rollback of an update, delete, or flush */
			if (HeapTupleIsValid(entry->tuple))
				heap_freetuple(entry->tuple);
			entry->tuple = prev->tuple;
			entry->written = prev->written;
			entry->deleted = prev->deleted;
			entry->subid = prev->subid;
			entry->prev = NULL;
			if (CACHE_ENTRY_NEEDS_FLUSH(entry))
				have_entries_to_flush = true;
		}
		else
		{
			/* Rollback of an insert */
			if (HeapTupleIsValid(entry->tuple))
				heap_freetuple(entry->tuple);
			if (prev != NULL)
				heap_freetuple(prev->tuple);
			hash_search(cache->hashtable, &entry->relid, HASH_REMOVE, NULL);
		}

		/* Free previous saved copy */
		if (prev)
			pfree(prev);
	}
}

/*
 * AtEOSubXact_GTCatCacheEntryCleanup
 *
 *	Clean up a single cache entry at subtransaction commit or abort.
 *
 *	NB: this processing must be idempotent, because EOXactListAdd() doesn't
 *	bother to prevent duplicate entries in eoxact_list[].
 */
static void
AtEOSubXact_GTCatCacheEntryCleanup(GTCatCache *cache, GTCatCacheEntry *entry,
								   bool isCommit, SubTransactionId mySubid,
								   SubTransactionId parentSubid)
{
	/*
	 * Was the entry inserted, updated, deleted, or flushed in the current
	 * subtransaction?
	 *
	 * On subcommit, mark it as inserted, updated, deleted, or flushed in the
	 * parent, instead, and discard any previous copy of the entry that was
	 * saved in case of subrollback, if it was for the parent subtransaction.
	 *
	 * On subrollback of an update, delete, or flush, restore the saved copy
	 * from the parent subtransaction (or possibly a lower level).
	 *
	 * Otherwise (subrollback of an insert), just remove the cache entry.
	 */
	if (entry->subid == mySubid)
	{
		GTCatCacheEntry *prev = entry->prev;

		/*
		 * If there's a saved copy, it should be a version from the parent
		 * subtransaction, or a lower level.
		 *
		 * Note: the saved copy might be marked as deleted, and have no tuple
		 * (the change made in this subtransaction might have been to flush
		 * that delete to the database).
		 */
		Assert(prev == NULL ||
			   (prev->relid == entry->relid && prev->subid <= parentSubid));

		if (isCommit)
		{
			/* Subcommit of an insert, update, delete, or flush */
			entry->subid = parentSubid;
			if (prev != NULL && prev->subid == parentSubid)
			{
				if (HeapTupleIsValid(prev->tuple))
					heap_freetuple(prev->tuple);
				entry->prev = prev->prev;
				pfree(prev);
			}
		}
		else if (prev != NULL)
		{
			/* Subrollback of an update, delete, or flush */
			if (HeapTupleIsValid(entry->tuple))
				heap_freetuple(entry->tuple);
			entry->tuple = prev->tuple;
			entry->written = prev->written;
			entry->deleted = prev->deleted;
			entry->subid = prev->subid;
			entry->prev = prev->prev;
			pfree(prev);
			if (CACHE_ENTRY_NEEDS_FLUSH(entry))
				have_entries_to_flush = true;
		}
		else
		{
			/* Subrollback of an insert */
			if (HeapTupleIsValid(entry->tuple))
				heap_freetuple(entry->tuple);
			hash_search(cache->hashtable, &entry->relid, HASH_REMOVE, NULL);
		}
	}
}

/*
 * GTCatCacheTupleExists
 *
 *	Test if a catalog tuple for the specified relation exists.
 */
bool
GTCatCacheTupleExists(GTCatCacheIdentifier cacheId, Oid relid)
{
	GTCatCache *cache = &gt_cat_cache[cacheId];
	GTCatCacheEntry *entry;

	if (cache->hashtable == NULL)
		return false;

	entry = hash_search(cache->hashtable, &relid, HASH_FIND, NULL);

	return entry != NULL && !entry->deleted;
}

/*
 * GTCatCacheSearch
 *
 *	Search for the catalog tuple for the specified relation.  Returns NULL if
 *	not found.  Otherwise the tuple should be freed with heap_freetuple().
 */
HeapTuple
GTCatCacheSearch(GTCatCacheIdentifier cacheId, Oid relid)
{
	GTCatCache *cache = &gt_cat_cache[cacheId];
	GTCatCacheEntry *entry;

	if (cache->hashtable == NULL)
		return NULL;

	entry = hash_search(cache->hashtable, &relid, HASH_FIND, NULL);
	if (entry == NULL || entry->deleted)
		return NULL;

	return heap_copytuple(entry->tuple);
}

/*
 * GTCatCacheTupleInsert
 *
 *	Insert a new catalog tuple, constructed from the specified values, for the
 *	specified relation.
 *
 *	Note: The new tuple is not written to the database until GTCatCacheFlush()
 *	is called.
 */
void
GTCatCacheTupleInsert(GTCatCacheIdentifier cacheId,
					  Oid relid, char relkind, TupleDesc tupdesc,
					  const Datum *values, const bool *nulls)
{
	GTCatCache *cache = &gt_cat_cache[cacheId];
	GTCatCacheEntry *entry;
	bool		found;
	MemoryContext oldcontext;

	initialize_cache(cache);

	/* Insert a new cache entry for the tuple */
	entry = hash_search(cache->hashtable, &relid, HASH_ENTER, &found);
	if (found && !entry->deleted)
		/* Should never try to re-insert a tuple for the same relid */
		elog(ERROR, "tuple for global temporary relation %u already exists", relid);

	/* Fill in entry; copy tuple to long-term tuple memory context */
	oldcontext = MemoryContextSwitchTo(gt_cat_cache_tupctx);

	entry->tuple = heap_form_tuple(tupdesc, values, nulls);
	entry->written = false;
	entry->deleted = false;
	entry->prev = NULL;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * For a sequence, the tuple is inserted non-transactionally, and isn't
	 * deleted on (sub)rollback.  Otherwise, for any other relkind, mark the
	 * entry as created in the current subtransaction, and flag it for eoxact
	 * cleanup.
	 */
	if (relkind == RELKIND_SEQUENCE)
		entry->subid = InvalidSubTransactionId;
	else
	{
		entry->subid = GetCurrentSubTransactionId();
		EOXactListAdd(cache, relid);
	}

	/* Ensure that it is written out when requested */
	have_entries_to_flush = true;
}

/*
 * GTCatCacheTupleUpdate
 *
 *	Update a catalog tuple for the specified relation.
 *
 *	Note: This updates both the cache entry, and the tuple in the database
 *	(inserting it, if it hasn't already been written out).  This should not be
 *	called while the database is read-only.
 */
void
GTCatCacheTupleUpdate(GTCatCacheIdentifier cacheId, Oid relid,
					  HeapTuple newtuple)
{
	GTCatCache *cache = &gt_cat_cache[cacheId];
	GTCatCacheEntry *entry;
	Relation	rel;

	/* Find and update the cache entry for this relation */
	entry = find_and_update_cache_entry(cache, relid, newtuple);

	/* Update the tuple in the database to match */
	rel = table_open(cache->catalog_relid, RowExclusiveLock);

	if (entry->written)
	{
		HeapTuple	oldtuple;

		oldtuple = SearchSysCache1(cache->cacheid, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(oldtuple))
			elog(ERROR, "cache lookup failed for global temp relation %u", relid);

		CatalogTupleUpdate(rel, &oldtuple->t_self, newtuple);

		ReleaseSysCache(oldtuple);
	}
	else
	{
		CatalogTupleInsert(rel, newtuple);
		entry->written = true;
	}

	table_close(rel, RowExclusiveLock);
}

/*
 * GTCatCacheTupleDelete
 *
 *	Delete the catalog tuple for the specified relation.
 *
 *	Note: If the database is currently read-only, the tuple will only be
 *	marked as deleted in the cache; it won't actually be deleted from the
 *	database until GTCatCacheFlush() is called.
 */
void
GTCatCacheTupleDelete(GTCatCacheIdentifier cacheId, Oid relid)
{
	GTCatCache *cache = &gt_cat_cache[cacheId];
	GTCatCacheEntry *entry;

	/*
	 * Find and update the cache entry for this relation, setting its tuple to
	 * NULL, and marking it as deleted.
	 */
	entry = find_and_update_cache_entry(cache, relid, NULL);
	entry->deleted = true;

	/*
	 * If it was written to the database, delete the tuple there too, unless
	 * the database is currently read-only.
	 */
	if (entry->written && can_flush_catalogs())
	{
		Relation	rel;

		rel = table_open(cache->catalog_relid, RowExclusiveLock);

		/* Re-check entry->written, in case an intervening flush deleted it */
		if (entry->written)
		{
			HeapTuple	oldtuple;

			oldtuple = SearchSysCache1(cache->cacheid, ObjectIdGetDatum(relid));
			if (!HeapTupleIsValid(oldtuple))
				elog(ERROR, "cache lookup failed for global temp relation %u", relid);

			CatalogTupleDelete(rel, &oldtuple->t_self);
			ReleaseSysCache(oldtuple);
			entry->written = false;
		}

		table_close(rel, RowExclusiveLock);
	}
}

/*
 * GTCatCacheFlush
 *
 *	Write out any new cache entries to the database, so that the database is
 *	in sync with the contents of the cache (unless the database is currently
 *	read-only).
 */
void
GTCatCacheFlush(void)
{
	if (have_entries_to_flush && can_flush_catalogs())
		flush_cache_entries();
}

/*
 * AtEOXact_GTCatCache
 *
 *	Clean up global temporary catalog caches at main-transaction commit or
 *	abort.
 */
void
AtEOXact_GTCatCache(bool isCommit)
{
	/* Clean up each cache */
	for (int cacheId = 0; cacheId < NUM_GT_CAT_CACHES; cacheId++)
	{
		GTCatCache *cache = &gt_cat_cache[cacheId];
		GTCatCacheEntry *entry;

		/*
		 * Unless the eoxact_list[] overflowed, we only need to examine the
		 * entries listed in it.  Otherwise fall back on a hash_seq_search
		 * scan --- see similar code in AtEOXact_RelationCache().
		 */
		if (cache->eoxact_list_overflowed)
		{
			HASH_SEQ_STATUS status;

			hash_seq_init(&status, cache->hashtable);
			while ((entry = hash_seq_search(&status)) != NULL)
			{
				AtEOXact_GTCatCacheEntryCleanup(cache, entry, isCommit);
			}
		}
		else
		{
			for (int i = 0; i < cache->eoxact_list_len; i++)
			{
				entry = hash_search(cache->hashtable, &cache->eoxact_list[i],
									HASH_FIND, NULL);
				if (entry)
					AtEOXact_GTCatCacheEntryCleanup(cache, entry, isCommit);
			}
		}

		/* Now we're out of the transaction and can clear eoxact_list */
		cache->eoxact_list_len = 0;
		cache->eoxact_list_overflowed = false;
	}
	flushing_entries = false;
}

/*
 * AtEOSubXact_GTCatCache
 *
 *	Clean up global temporary catalog caches at sub-transaction commit or
 *	abort.
 */
void
AtEOSubXact_GTCatCache(bool isCommit, SubTransactionId mySubid,
					   SubTransactionId parentSubid)
{
	/* Clean up each cache */
	for (int cacheId = 0; cacheId < NUM_GT_CAT_CACHES; cacheId++)
	{
		GTCatCache *cache = &gt_cat_cache[cacheId];
		GTCatCacheEntry *entry;

		/*
		 * Unless the eoxact_list[] overflowed, we only need to examine the
		 * entries listed in it.  Otherwise fall back on a hash_seq_search
		 * scan.  Same logic as in AtEOXact_GTCatCache().
		 */
		if (cache->eoxact_list_overflowed)
		{
			HASH_SEQ_STATUS status;

			hash_seq_init(&status, cache->hashtable);
			while ((entry = hash_seq_search(&status)) != NULL)
			{
				AtEOSubXact_GTCatCacheEntryCleanup(cache, entry, isCommit,
												   mySubid, parentSubid);
			}
		}
		else
		{
			for (int i = 0; i < cache->eoxact_list_len; i++)
			{
				entry = hash_search(cache->hashtable, &cache->eoxact_list[i],
									HASH_FIND, NULL);
				if (entry)
					AtEOSubXact_GTCatCacheEntryCleanup(cache, entry,
													   isCommit, mySubid,
													   parentSubid);
			}
		}

		/* Don't reset eoxact_list; we still need more cleanup later */
	}
}
