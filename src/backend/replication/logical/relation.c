/*-------------------------------------------------------------------------
 * relation.c
 *	   PostgreSQL logical replication relation mapping cache
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/relation.c
 *
 * NOTES
 *	  Routines in this file mainly have to do with mapping the properties
 *	  of local replication target relations to the properties of their
 *	  remote counterpart.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "libpq/pqformat.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "replication/logicalrelation.h"
#include "replication/worker_internal.h"
#include "rewrite/rewriteHandler.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static MemoryContext LogicalRepRelMapContext = NULL;

static HTAB *LogicalRepRelMap = NULL;

/*
 * Partition map (LogicalRepPartMap)
 *
 * When a partitioned table is used as replication target, replicated
 * operations are actually performed on its leaf partitions, which requires
 * the partitions to also be mapped to the remote relation.  Parent's entry
 * (LogicalRepRelMapEntry) cannot be used as-is for all partitions, because
 * individual partitions may have different attribute numbers, which means
 * attribute mappings to remote relation's attributes must be maintained
 * separately for each partition.
 */
static MemoryContext LogicalRepPartMapContext = NULL;
static HTAB *LogicalRepPartMap = NULL;
typedef struct LogicalRepPartMapEntry
{
	Oid			partoid;		/* LogicalRepPartMap's key */
	LogicalRepRelMapEntry relmapentry;
} LogicalRepPartMapEntry;

static Oid	FindLogicalRepLocalIndex(Relation localrel, LogicalRepRelation *remoterel,
									 AttrMap *attrMap);

/*
 * Write all the remote relation information from the LogicalRepRelMapEntry to
 * the output stream. The number of relations is stored in the num_rels.
 */
void
logicalrep_write_all_internal_rels(StringInfo out, int *num_rels)
{
	LogicalRepRelMapEntry *entry;
	HASH_SEQ_STATUS status;

	if (LogicalRepRelMap)
		*num_rels = hash_get_num_entries(LogicalRepRelMap);
	else
		*num_rels = 0;

	if (*num_rels == 0)
		return;

	pq_sendbyte(out, LOGICAL_REP_MSG_INTERNAL_MESSAGE);
	pq_sendbyte(out, PA_MSG_RELMAP);
	pq_sendint(out, *num_rels, 4);

	hash_seq_init(&status, LogicalRepRelMap);

	while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
		logicalrep_write_internal_rel(out, &entry->remoterel);
}

/*
 * Similar to logicalrep_write_all_internal_rels but writes only the given
 * relation.
 */
void
logicalrep_write_one_internal_rel(StringInfo out, LogicalRepRelation *rel)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_INTERNAL_MESSAGE);
	pq_sendbyte(out, PA_MSG_RELMAP);
	pq_sendint(out, 1, 4);

	logicalrep_write_internal_rel(out, rel);
}

/*
 * Relcache invalidation callback for our relation map cache.
 */
static void
logicalrep_relmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepRelMapEntry *entry;

	/* Just to be sure. */
	if (LogicalRepRelMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		/* TODO, use inverse lookup hashtable? */
		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->localreloid == reloid)
			{
				entry->localrelvalid = false;
				entry->parallel_safety_valid = false;
				entry->local_unique_indexes_valid = false;
				hash_seq_term(&status);
				break;
			}
		}
	}
	else
	{
		/* invalidate all cache entries */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
		{
			entry->localrelvalid = false;
			entry->parallel_safety_valid = false;
			entry->local_unique_indexes_valid = false;
		}
	}
}

/*
 * Initialize the relation map cache.
 */
static void
logicalrep_relmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepRelMapContext)
		LogicalRepRelMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepRelMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* Initialize the relation hash table. */
	ctl.keysize = sizeof(LogicalRepRelId);
	ctl.entrysize = sizeof(LogicalRepRelMapEntry);
	ctl.hcxt = LogicalRepRelMapContext;

	LogicalRepRelMap = hash_create("logicalrep relation map cache", 128, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Watch for invalidation events. */
	CacheRegisterRelcacheCallback(logicalrep_relmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * Release local index list
 */
static void
free_local_unique_indexes(LogicalRepRelMapEntry *entry)
{
	Assert(am_leader_apply_worker());

	foreach_ptr(LogicalRepSubUniqueIndex, idxinfo, entry->local_unique_indexes)
		bms_free(idxinfo->indexkeys);

	list_free_deep(entry->local_unique_indexes);
	entry->local_unique_indexes = NIL;
}

/*
 * Free the entry of a relation map cache.
 */
static void
logicalrep_relmap_free_entry(LogicalRepRelMapEntry *entry)
{
	LogicalRepRelation *remoterel;

	remoterel = &entry->remoterel;

	pfree(remoterel->nspname);
	pfree(remoterel->relname);

	if (remoterel->natts > 0)
	{
		int			i;

		for (i = 0; i < remoterel->natts; i++)
			pfree(remoterel->attnames[i]);

		pfree(remoterel->attnames);
		pfree(remoterel->atttyps);
	}
	bms_free(remoterel->attkeys);

	if (entry->attrmap)
		free_attrmap(entry->attrmap);

	if (entry->local_unique_indexes != NIL)
		free_local_unique_indexes(entry);
}

/*
 * Add new entry or update existing entry in the relation map cache.
 *
 * Called when new relation mapping is sent by the publisher to update
 * our expected view of incoming data from said publisher.
 */
void
logicalrep_relmap_update(LogicalRepRelation *remoterel)
{
	MemoryContext oldctx;
	LogicalRepRelMapEntry *entry;
	bool		found;
	int			i;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/*
	 * HASH_ENTER returns the existing entry if present or creates a new one.
	 */
	entry = hash_search(LogicalRepRelMap, &remoterel->remoteid,
						HASH_ENTER, &found);

	if (found)
		logicalrep_relmap_free_entry(entry);

	/*
	 * Clear all fields except last_depended_xid, which tracks historical
	 * dependency info that remains valid across schema changes.
	 */
	memset(entry, 0, found ? offsetof(LogicalRepRelMapEntry, last_depended_xid)
		   : sizeof(LogicalRepRelMapEntry));

	/* Make cached copy of the data */
	oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
	entry->remoterel.remoteid = remoterel->remoteid;
	entry->remoterel.nspname = pstrdup(remoterel->nspname);
	entry->remoterel.relname = pstrdup(remoterel->relname);
	entry->remoterel.natts = remoterel->natts;
	entry->remoterel.attnames = palloc_array(char *, remoterel->natts);
	entry->remoterel.atttyps = palloc_array(Oid, remoterel->natts);
	for (i = 0; i < remoterel->natts; i++)
	{
		entry->remoterel.attnames[i] = pstrdup(remoterel->attnames[i]);
		entry->remoterel.atttyps[i] = remoterel->atttyps[i];
	}
	entry->remoterel.replident = remoterel->replident;

	/*
	 * XXX The walsender currently does not transmit the relkind of the remote
	 * relation when replicating changes. Since we support replicating only
	 * table changes at present, we default to initializing relkind as
	 * RELKIND_RELATION. This is needed in CheckSubscriptionRelkind() to check
	 * if the publisher and subscriber relation kinds are compatible.
	 */
	entry->remoterel.relkind =
		(remoterel->relkind == 0) ? RELKIND_RELATION : remoterel->relkind;

	entry->remoterel.attkeys = bms_copy(remoterel->attkeys);

	/*
	 * Rebuild the key info using the latest replica identity, which may have
	 * changed.
	 */
	entry->local_unique_indexes_valid = false;

	MemoryContextSwitchTo(oldctx);
}

/*
 * Find attribute index in TupleDesc struct by attribute name.
 *
 * Returns -1 if not found.
 */
static int
logicalrep_rel_att_by_name(LogicalRepRelation *remoterel, const char *attname)
{
	int			i;

	for (i = 0; i < remoterel->natts; i++)
	{
		if (strcmp(remoterel->attnames[i], attname) == 0)
			return i;
	}

	return -1;
}

/*
 * Returns a comma-separated string of attribute names based on the provided
 * relation and bitmap indicating which attributes to include.
 */
static char *
logicalrep_get_attrs_str(LogicalRepRelation *remoterel, Bitmapset *atts)
{
	StringInfoData attsbuf;
	bool		first = true;
	int			i = -1;

	Assert(!bms_is_empty(atts));

	initStringInfo(&attsbuf);

	while ((i = bms_next_member(atts, i)) >= 0)
	{
		if (first)
			appendStringInfo(&attsbuf, _("\"%s\""), remoterel->attnames[i]);
		else
			appendStringInfo(&attsbuf, _(", \"%s\""), remoterel->attnames[i]);
		first = false;
	}

	return attsbuf.data;
}

/*
 * If attempting to replicate missing or generated columns, report an error.
 * Prioritize 'missing' errors if both occur though the prioritization is
 * arbitrary.
 */
static void
logicalrep_report_missing_or_gen_attrs(LogicalRepRelation *remoterel,
									   Bitmapset *missingatts,
									   Bitmapset *generatedatts)
{
	if (!bms_is_empty(missingatts))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg_plural("logical replication target relation \"%s.%s\" is missing replicated column: %s",
							  "logical replication target relation \"%s.%s\" is missing replicated columns: %s",
							  bms_num_members(missingatts),
							  remoterel->nspname,
							  remoterel->relname,
							  logicalrep_get_attrs_str(remoterel,
													   missingatts)));

	if (!bms_is_empty(generatedatts))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg_plural("logical replication target relation \"%s.%s\" has incompatible generated column: %s",
							  "logical replication target relation \"%s.%s\" has incompatible generated columns: %s",
							  bms_num_members(generatedatts),
							  remoterel->nspname,
							  remoterel->relname,
							  logicalrep_get_attrs_str(remoterel,
													   generatedatts)));
}

/*
 * Check if replica identity matches and mark the updatable flag.
 *
 * We allow for stricter replica identity (fewer columns) on subscriber as
 * that will not stop us from finding unique tuple. IE, if publisher has
 * identity (id,timestamp) and subscriber just (id) this will not be a
 * problem, but in the opposite scenario it will.
 *
 * We just mark the relation entry as not updatable here if the local
 * replica identity is found to be insufficient for applying
 * updates/deletes (inserts don't care!) and leave it to
 * check_relation_updatable() to throw the actual error if needed.
 */
static void
logicalrep_rel_mark_updatable(LogicalRepRelMapEntry *entry)
{
	Bitmapset  *idkey;
	LogicalRepRelation *remoterel = &entry->remoterel;
	int			i;

	entry->updatable = true;

	idkey = RelationGetIndexAttrBitmap(entry->localrel,
									   INDEX_ATTR_BITMAP_IDENTITY_KEY);
	/* fallback to PK if no replica identity */
	if (idkey == NULL)
	{
		idkey = RelationGetIndexAttrBitmap(entry->localrel,
										   INDEX_ATTR_BITMAP_PRIMARY_KEY);

		/*
		 * If no replica identity index and no PK, the published table must
		 * have replica identity FULL.
		 */
		if (idkey == NULL && remoterel->replident != REPLICA_IDENTITY_FULL)
			entry->updatable = false;
	}

	i = -1;
	while ((i = bms_next_member(idkey, i)) >= 0)
	{
		int			attnum = i + FirstLowInvalidHeapAttributeNumber;

		if (!AttrNumberIsForUserDefinedAttr(attnum))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" uses "
							"system columns in REPLICA IDENTITY index",
							remoterel->nspname, remoterel->relname)));

		attnum = AttrNumberGetAttrOffset(attnum);

		if (entry->attrmap->attnums[attnum] < 0 ||
			!bms_is_member(entry->attrmap->attnums[attnum], remoterel->attkeys))
		{
			entry->updatable = false;
			break;
		}
	}
}

/*
 * Open the local relation associated with the remote one.
 *
 * Rebuilds the Relcache mapping if it was invalidated by local DDL.
 */
LogicalRepRelMapEntry *
logicalrep_rel_open(LogicalRepRelId remoteid, LOCKMODE lockmode)
{
	LogicalRepRelMapEntry *entry;
	bool		found;
	LogicalRepRelation *remoterel;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/* Search for existing entry. */
	entry = hash_search(LogicalRepRelMap, &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(ERROR, "no relation map entry for remote relation ID %u",
			 remoteid);

	remoterel = &entry->remoterel;

	/* Ensure we don't leak a relcache refcount. */
	if (entry->localrel)
		elog(ERROR, "remote relation ID %u is already open", remoteid);

	/*
	 * When opening and locking a relation, pending invalidation messages are
	 * processed which can invalidate the relation.  Hence, if the entry is
	 * currently considered valid, try to open the local relation by OID and
	 * see if invalidation ensues.
	 */
	if (entry->localrelvalid)
	{
		entry->localrel = try_table_open(entry->localreloid, lockmode);
		if (!entry->localrel)
		{
			/* Table was renamed or dropped. */
			entry->localrelvalid = false;
			entry->parallel_safety_valid = false;
			entry->local_unique_indexes_valid = false;
		}
		else if (!entry->localrelvalid)
		{
			/* Note we release the no-longer-useful lock here. */
			table_close(entry->localrel, lockmode);
			entry->localrel = NULL;
		}
	}

	/*
	 * If the entry has been marked invalid since we last had lock on it,
	 * re-open the local relation by name and rebuild all derived data.
	 */
	if (!entry->localrelvalid)
	{
		Oid			relid;
		TupleDesc	desc;
		MemoryContext oldctx;
		int			i;
		Bitmapset  *missingatts;
		Bitmapset  *generatedattrs = NULL;

		/* Release the no-longer-useful attrmap, if any. */
		if (entry->attrmap)
		{
			free_attrmap(entry->attrmap);
			entry->attrmap = NULL;
		}

		/* Try to find and lock the relation by name. */
		relid = RangeVarGetRelid(makeRangeVar(remoterel->nspname,
											  remoterel->relname, -1),
								 lockmode, true);
		if (!OidIsValid(relid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" does not exist",
							remoterel->nspname, remoterel->relname)));
		entry->localrel = table_open(relid, NoLock);
		entry->localreloid = relid;

		/* Check for supported relkind. */
		CheckSubscriptionRelkind(entry->localrel->rd_rel->relkind,
								 remoterel->relkind,
								 remoterel->nspname, remoterel->relname);

		/*
		 * Build the mapping of local attribute numbers to remote attribute
		 * numbers and validate that we don't miss any replicated columns as
		 * that would result in potentially unwanted data loss.
		 */
		desc = RelationGetDescr(entry->localrel);
		oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
		entry->attrmap = make_attrmap(desc->natts);
		MemoryContextSwitchTo(oldctx);

		/* check and report missing attrs, if any */
		missingatts = bms_add_range(NULL, 0, remoterel->natts - 1);
		for (i = 0; i < desc->natts; i++)
		{
			int			attnum;
			Form_pg_attribute attr = TupleDescAttr(desc, i);

			if (attr->attisdropped)
			{
				entry->attrmap->attnums[i] = -1;
				continue;
			}

			attnum = logicalrep_rel_att_by_name(remoterel,
												NameStr(attr->attname));

			entry->attrmap->attnums[i] = attnum;
			if (attnum >= 0)
			{
				/* Remember which subscriber columns are generated. */
				if (attr->attgenerated)
					generatedattrs = bms_add_member(generatedattrs, attnum);

				missingatts = bms_del_member(missingatts, attnum);
			}
		}

		logicalrep_report_missing_or_gen_attrs(remoterel, missingatts,
											   generatedattrs);

		/* be tidy */
		bms_free(generatedattrs);
		bms_free(missingatts);

		/*
		 * Set if the table's replica identity is enough to apply
		 * update/delete.
		 */
		logicalrep_rel_mark_updatable(entry);

		/*
		 * Finding a usable index is an infrequent task. It occurs when an
		 * operation is first performed on the relation, or after invalidation
		 * of the relation cache entry (such as ANALYZE or CREATE/DROP index
		 * on the relation).
		 */
		entry->localindexoid = FindLogicalRepLocalIndex(entry->localrel, remoterel,
														entry->attrmap);

		entry->localrelvalid = true;
	}

	if (entry->state != SUBREL_STATE_READY)
		entry->state = GetSubscriptionRelState(MySubscription->oid,
											   entry->localreloid,
											   &entry->statelsn);

	return entry;
}

/*
 * Close the previously opened logical relation.
 */
void
logicalrep_rel_close(LogicalRepRelMapEntry *rel, LOCKMODE lockmode)
{
	table_close(rel->localrel, lockmode);
	rel->localrel = NULL;
}

/*
 * Check whether changes on this relation can be applied in parallel.
 *
 * Parallel apply is unsafe if the table has any user-defined functions that may
 * be executed during change application (e.g., in triggers, defaults, stored
 * generated expressions, or constraint expressions). In theory, we should check
 * the volatility of all such functions.
 *
 * Note that we do not check user-defined CHECK constraints here, as PostgreSQL
 * already assumes they are immutable; we follow that same rule.
 *
 * If any mutable function is found, the relation is marked as globally unsafe,
 * and the result is cached.
 */
void
logicalrep_rel_check_parallel_safety(LogicalRepRelMapEntry *entry)
{
	Relation	localrel = entry->localrel;
	TriggerDesc *trigdesc = localrel->trigdesc;
	int			ntriggers = trigdesc ? trigdesc->numtriggers : 0;
	TupleDesc	desc = RelationGetDescr(localrel);

	if (entry->parallel_safety_valid)
		return;

	memset(entry->parallel_global_unsafe, 0,
		   sizeof(entry->parallel_global_unsafe));

	/* Seek triggers one by one to see the volatility */
	for (int i = 0; i < ntriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		Assert(OidIsValid(trigger->tgfoid));

		/* Skip if the trigger is internal (e.g., fkey triggers) */
		if (trigger->tgisinternal)
			continue;

		/* Skip if the trigger is not enabled for logical replication */
		if (trigger->tgenabled == TRIGGER_DISABLED ||
			trigger->tgenabled == TRIGGER_FIRES_ON_ORIGIN)
			continue;

		/*
		 * Check the volatility of the trigger. Exit if it is not immutable.
		 * A multi-event trigger (e.g., BEFORE INSERT OR UPDATE) must mark
		 * every action it fires for, so these checks are independent ifs
		 * rather than an else-if chain.
		 */
		if (func_volatile(trigger->tgfoid) != PROVOLATILE_IMMUTABLE)
		{
			if (TRIGGER_FOR_INSERT(trigger->tgtype))
				entry->parallel_global_unsafe[LRPA_INSERT] = true;
			if (TRIGGER_FOR_UPDATE(trigger->tgtype))
				entry->parallel_global_unsafe[LRPA_UPDATE] = true;
			if (TRIGGER_FOR_DELETE(trigger->tgtype))
				entry->parallel_global_unsafe[LRPA_DELETE] = true;
			if (TRIGGER_FOR_TRUNCATE(trigger->tgtype))
				entry->parallel_global_unsafe[LRPA_TRUNCATE] = true;
		}
	}

	/*
	 * Check column defaults and stored generated expressions for mutable
	 * functions.
	 */
	for (int attnum = 0; attnum < desc->natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum);
		Expr	   *defexpr;

		if (att->attisdropped)
			continue;

		if (!att->atthasdef && att->attgenerated != ATTRIBUTE_GENERATED_STORED)
			continue;

		/* Skip columns whose values come from remote change */
		if (entry->attrmap->attnums[attnum] >= 0)
			continue;

		defexpr = (Expr *) build_column_default(entry->localrel, attnum + 1);

		if (contain_mutable_functions_after_planning(defexpr))
		{
			/* Only INSERT operations are affected by column defaults */
			entry->parallel_global_unsafe[LRPA_INSERT] = true;
			break;
		}
	}

	entry->parallel_safety_valid = true;
}

/*
 * Append a new unique index to the list if it is not a duplicate of, or a
 * superset of, an existing index. If it is a subset of an existing index,
 * replace the existing one with the new one.
 */
static void
maybe_append_unique_index(Oid indexoid, Bitmapset *indexkeys,
						  bool nulls_distinct, List **unique_indexes)
{
	LogicalRepSubUniqueIndex *newinfo;
	MemoryContext oldctx;

	foreach_ptr(LogicalRepSubUniqueIndex, oldinfo, *unique_indexes)
	{
		BMS_Comparison cmp;

		if (oldinfo->nulls_distinct != nulls_distinct)
			continue;

		cmp = bms_subset_compare(oldinfo->indexkeys, indexkeys);

		/* Duplicate index, no need to add */
		if (cmp == BMS_EQUAL || cmp == BMS_SUBSET2)
			return;

		/* New index is a subset of an existing one, replace the old one */
		if (cmp == BMS_SUBSET1)
		{
			oldinfo->indexoid = indexoid;
			oldinfo->indexkeys = indexkeys;
			oldinfo->nulls_distinct = nulls_distinct;
			return;
		}
	}

	oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);

	newinfo = palloc(sizeof(LogicalRepSubUniqueIndex));
	newinfo->indexoid = indexoid;
	newinfo->indexkeys = bms_copy(indexkeys);
	newinfo->nulls_distinct = nulls_distinct;
	*unique_indexes = lappend(*unique_indexes, newinfo);

	MemoryContextSwitchTo(oldctx);
}

/*
 * Collect all local unique indexes that can be used for dependency tracking
 *
 * This function collects all types of unique indexes, including those with
 * index expressions and partial indexes. However, to avoid the overhead and
 * complexity of executing expressions, we do not evaluate them during
 * dependency tracking.
 *
 * For indexes with expressions, only the non-expression columns are recorded in
 * the bitmap. The dependency tracking function will use only these columns,
 * which may lead to false dependency detection. For example, consider a unique
 * index defined as UNIQUE (a, func(b)), where b is an expression column. Rows
 * (1, 2) and (1, 3) will be treated as dependent even though they are not. This
 * is acceptable, as it is still better than disabling parallelism for all
 * relations that have expression indexes.
 *
 * Similarly, partial indexes may also cause false dependencies due to predicate
 * expressions. For the same reason, we consider this acceptable as well.
 *
 * To avoid redundant dependency tracking, indexes whose key columns are the
 * same as, or a superset of, the replica identity key or an already collected
 * unique index are skipped, as their scope is already covered.
 *
 * Columns not in the replica identity key are excluded from the unique column
 * set. Since the old tuple of an UPDATE or DELETE contains only replica
 * identity key columns, any other columns would be missing and thus unavailable
 * for dependency tracking.
 */
static void
get_unique_indexes(LogicalRepRelMapEntry *entry, List **local_unique_indexes)
{
	List	   *idxlist;

	idxlist = RelationGetIndexList(entry->localrel);

	/* Iterate indexes to list all usable indexes */
	foreach_oid(idxoid, idxlist)
	{
		Relation	idxrel;
		AttrMap    *attrmap;
		Bitmapset  *indexkeys = NULL;
		BMS_Comparison cmp;
		int			indnkeys;
		bool		nulls_distinct;

		idxrel = index_open(idxoid, AccessShareLock);

		/*
		 * Only unique indexes are considered. Indexes backing a deferrable
		 * unique constraint are skipped: their uniqueness is checked at
		 * commit time and commit order is preserved, so they cannot cause
		 * an apply-time conflict.
		 */
		if (!idxrel->rd_index->indisunique ||
			!idxrel->rd_index->indimmediate)
		{
			index_close(idxrel, AccessShareLock);
			continue;
		}

		indnkeys = idxrel->rd_index->indnkeyatts;
		nulls_distinct = !idxrel->rd_index->indnullsnotdistinct;
		attrmap = entry->attrmap;

		Assert(indnkeys);

		/* Seek each attributes and add to a Bitmap */
		for (int i = 0; i < indnkeys; i++)
		{
			AttrNumber	localcol = idxrel->rd_index->indkey.values[i];
			AttrNumber	remotecol;

			/* Skip expression */
			if (!AttributeNumberIsValid(localcol))
				continue;

			/*
			 * Skip generated columns. The subscriber recomputes generated
			 * values using its own expression, so the value sent by the
			 * publisher (if published at all) does not necessarily match
			 * the local one and cannot be used for dependency tracking.
			 * Dropping the column may degenerate the index to table-level
			 * serialization, which is safe.
			 */
			if (TupleDescCompactAttr(RelationGetDescr(entry->localrel),
									 AttrNumberGetAttrOffset(localcol))->attgenerated)
				continue;

			remotecol = attrmap->attnums[AttrNumberGetAttrOffset(localcol)];

			/* Skip if the column does not exist on publisher node */
			if (remotecol < 0)
				continue;

			/* Skip columns that are not part of the replica identity key */
			if (!bms_is_member(remotecol, entry->remoterel.attkeys))
				continue;

			/* Checks are passed, remember the attribute */
			indexkeys = bms_add_member(indexkeys, remotecol);
		}

		index_close(idxrel, AccessShareLock);

		/*
		 * Skip indexes whose key columns are a superset of the replica identity
		 * key.
		 */
		cmp = bms_subset_compare(entry->remoterel.attkeys, indexkeys);
		if (cmp == BMS_EQUAL || cmp == BMS_SUBSET1)
			continue;

		maybe_append_unique_index(idxoid, indexkeys, nulls_distinct,
								  local_unique_indexes);
	}

	list_free(idxlist);
}

/*
 * Collect all local unique indexes that can be used for dependency tracking.
 * For partitioned tables, this also collects unique indexes from all leaf
 * partitions.
 *
 * See get_unique_indexes() for details on how unique indexes are collected.
 */
void
logicalrep_build_dependent_unique_indexes(LogicalRepRelMapEntry *entry)
{
	List	   *partitions;

	if (entry->local_unique_indexes_valid)
		return;

	free_local_unique_indexes(entry);

	/* For non-partitioned tables, simply get the unique indexes and return */
	if (entry->localrel->rd_rel->relkind == RELKIND_RELATION)
	{
		get_unique_indexes(entry, &entry->local_unique_indexes);
		entry->local_unique_indexes_valid = true;
		return;
	}

	/*
	 * Collect unique indexes from all leaf partitions of this partitioned
	 * table.
	 */
	partitions = find_all_inheritors(entry->localreloid, AccessShareLock,
									 NULL);

	foreach_oid(relid, partitions)
	{
		LogicalRepRelMapEntry *part_entry;
		Relation	partrel;
		AttrMap    *root_to_part_attrmap;

		/* Only check leaf partitions */
		if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
			continue;

		partrel = table_open(relid, AccessShareLock);

		root_to_part_attrmap =
			build_attrmap_by_name_if_req(RelationGetDescr(entry->localrel),
										 RelationGetDescr(partrel), false);

		part_entry = logicalrep_partition_open(entry, partrel,
											   root_to_part_attrmap);

		if (root_to_part_attrmap)
			free_attrmap(root_to_part_attrmap);

		get_unique_indexes(part_entry, &entry->local_unique_indexes);

		table_close(partrel, AccessShareLock);
	}

	entry->local_unique_indexes_valid = true;
}

/*
 * Partition cache: look up partition LogicalRepRelMapEntry's
 *
 * Unlike relation map cache, this is keyed by partition OID, not remote
 * relation OID, because we only have to use this cache in the case where
 * partitions are not directly mapped to any remote relation, such as when
 * replication is occurring with one of their ancestors as target.
 */

/*
 * Relcache invalidation callback
 */
static void
logicalrep_partmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepPartMapEntry *entry;

	/* Just to be sure. */
	if (LogicalRepPartMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		/*
		 * LogicalRepPartMap is keyed by partition OID, matching with
		 * entry->relmapentry.localreloid (see logicalrep_partition_open), so
		 * we can invalidate via a direct hash lookup.
		 */
		entry = hash_search(LogicalRepPartMap, &reloid, HASH_FIND, NULL);
		if (entry != NULL)
		{
			LogicalRepRelMapEntry  *parent;

			entry->relmapentry.localrelvalid = false;
			entry->relmapentry.parallel_safety_valid = false;

			/*
			 * Invalidating a leaf partition makes the parent table's collected
			 * all partition's index information stale.
			 */
			parent = hash_search(LogicalRepRelMap,
								 &entry->relmapentry.remoterel.remoteid,
								 HASH_FIND, NULL);

			if (parent)
				parent->local_unique_indexes_valid = false;
		}
	}
	else
	{
		/* invalidate all cache entries */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepPartMap);

		while ((entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
		{
			entry->relmapentry.localrelvalid = false;
			entry->relmapentry.parallel_safety_valid = false;
		}
	}
}

/*
 * Reset the entries in the partition map that refer to remoterel.
 *
 * Called when new relation mapping is sent by the publisher to update our
 * expected view of incoming data from said publisher.
 *
 * Note that we don't update the remoterel information in the entry here,
 * we will update the information in logicalrep_partition_open to avoid
 * unnecessary work.
 */
void
logicalrep_partmap_reset_relmap(LogicalRepRelation *remoterel)
{
	HASH_SEQ_STATUS status;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelMapEntry *entry;

	if (LogicalRepPartMap == NULL)
		return;

	hash_seq_init(&status, LogicalRepPartMap);
	while ((part_entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
	{
		entry = &part_entry->relmapentry;

		if (entry->remoterel.remoteid != remoterel->remoteid)
			continue;

		logicalrep_relmap_free_entry(entry);

		memset(entry, 0, sizeof(LogicalRepRelMapEntry));
	}
}

/*
 * Initialize the partition map cache.
 */
static void
logicalrep_partmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepPartMapContext)
		LogicalRepPartMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepPartMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* Initialize the relation hash table. */
	ctl.keysize = sizeof(Oid);	/* partition OID */
	ctl.entrysize = sizeof(LogicalRepPartMapEntry);
	ctl.hcxt = LogicalRepPartMapContext;

	LogicalRepPartMap = hash_create("logicalrep partition map cache", 64, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Watch for invalidation events. */
	CacheRegisterRelcacheCallback(logicalrep_partmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * logicalrep_partition_open
 *
 * Returned entry reuses most of the values of the root table's entry, save
 * the attribute map, which can be different for the partition.  However,
 * we must physically copy all the data, in case the root table's entry
 * gets freed/rebuilt.
 *
 * Note there's no logicalrep_partition_close, because the caller closes the
 * component relation.
 */
LogicalRepRelMapEntry *
logicalrep_partition_open(LogicalRepRelMapEntry *root,
						  Relation partrel, AttrMap *map)
{
	LogicalRepRelMapEntry *entry;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelation *remoterel = &root->remoterel;
	Oid			partOid = RelationGetRelid(partrel);
	AttrMap    *attrmap = root->attrmap;
	bool		found;
	MemoryContext oldctx;

	if (LogicalRepPartMap == NULL)
		logicalrep_partmap_init();

	/* Search for existing entry. */
	part_entry = (LogicalRepPartMapEntry *) hash_search(LogicalRepPartMap,
														&partOid,
														HASH_ENTER, &found);

	entry = &part_entry->relmapentry;

	/*
	 * We must always overwrite entry->localrel with the latest partition
	 * Relation pointer, because the Relation pointed to by the old value may
	 * have been cleared after the caller would have closed the partition
	 * relation after the last use of this entry.  Note that localrelvalid is
	 * only updated by the relcache invalidation callback, so it may still be
	 * true irrespective of whether the Relation pointed to by localrel has
	 * been cleared or not.
	 */
	if (found && entry->localrelvalid)
	{
		Assert(entry->localreloid == partOid);
		entry->localrel = partrel;
		return entry;
	}

	/* Switch to longer-lived context. */
	oldctx = MemoryContextSwitchTo(LogicalRepPartMapContext);

	if (!found)
	{
		memset(part_entry, 0, sizeof(LogicalRepPartMapEntry));
		part_entry->partoid = partOid;
	}

	/* Release the no-longer-useful attrmap, if any. */
	if (entry->attrmap)
	{
		free_attrmap(entry->attrmap);
		entry->attrmap = NULL;
	}

	if (!entry->remoterel.remoteid)
	{
		int			i;

		/* Remote relation is copied as-is from the root entry. */
		entry->remoterel.remoteid = remoterel->remoteid;
		entry->remoterel.nspname = pstrdup(remoterel->nspname);
		entry->remoterel.relname = pstrdup(remoterel->relname);
		entry->remoterel.natts = remoterel->natts;
		entry->remoterel.attnames = palloc_array(char *, remoterel->natts);
		entry->remoterel.atttyps = palloc_array(Oid, remoterel->natts);
		for (i = 0; i < remoterel->natts; i++)
		{
			entry->remoterel.attnames[i] = pstrdup(remoterel->attnames[i]);
			entry->remoterel.atttyps[i] = remoterel->atttyps[i];
		}
		entry->remoterel.replident = remoterel->replident;
		entry->remoterel.attkeys = bms_copy(remoterel->attkeys);
	}

	entry->localrel = partrel;
	entry->localreloid = partOid;

	/*
	 * If the partition's attributes don't match the root relation's, we'll
	 * need to make a new attrmap which maps partition attribute numbers to
	 * remoterel's, instead of the original which maps root relation's
	 * attribute numbers to remoterel's.
	 *
	 * Note that 'map' which comes from the tuple routing data structure
	 * contains 1-based attribute numbers (of the parent relation).  However,
	 * the map in 'entry', a logical replication data structure, contains
	 * 0-based attribute numbers (of the remote relation).
	 */
	if (map)
	{
		AttrNumber	attno;

		entry->attrmap = make_attrmap(map->maplen);
		for (attno = 0; attno < entry->attrmap->maplen; attno++)
		{
			AttrNumber	root_attno = map->attnums[attno];

			/* 0 means it's a dropped attribute.  See comments atop AttrMap. */
			if (root_attno == 0)
				entry->attrmap->attnums[attno] = -1;
			else
				entry->attrmap->attnums[attno] = attrmap->attnums[root_attno - 1];
		}
	}
	else
	{
		/* Lacking copy_attmap, do this the hard way. */
		entry->attrmap = make_attrmap(attrmap->maplen);
		memcpy(entry->attrmap->attnums, attrmap->attnums,
			   attrmap->maplen * sizeof(AttrNumber));
	}

	/* Set if the table's replica identity is enough to apply update/delete. */
	logicalrep_rel_mark_updatable(entry);

	/* state and statelsn are left set to 0. */
	MemoryContextSwitchTo(oldctx);

	/*
	 * Finding a usable index is an infrequent task. It occurs when an
	 * operation is first performed on the relation, or after invalidation of
	 * the relation cache entry (such as ANALYZE or CREATE/DROP index on the
	 * relation).
	 *
	 * We also prefer to run this code on the oldctx so that we do not leak
	 * anything in the LogicalRepPartMapContext (hence CacheMemoryContext).
	 */
	entry->localindexoid = FindLogicalRepLocalIndex(partrel, remoterel,
													entry->attrmap);

	entry->localrelvalid = true;

	return entry;
}

/*
 * Returns the oid of an index that can be used by the apply worker to scan
 * the relation.
 *
 * We expect to call this function when REPLICA IDENTITY FULL is defined for
 * the remote relation.
 *
 * If no suitable index is found, returns InvalidOid.
 */
static Oid
FindUsableIndexForReplicaIdentityFull(Relation localrel, AttrMap *attrmap)
{
	List	   *idxlist = RelationGetIndexList(localrel);

	foreach_oid(idxoid, idxlist)
	{
		bool		isUsableIdx;
		Relation	idxRel;

		idxRel = index_open(idxoid, AccessShareLock);
		isUsableIdx = IsIndexUsableForReplicaIdentityFull(idxRel, attrmap);
		index_close(idxRel, AccessShareLock);

		/* Return the first eligible index found */
		if (isUsableIdx)
			return idxoid;
	}

	return InvalidOid;
}

/*
 * Returns true if the index is usable for replica identity full.
 *
 * The index must have an equal strategy for each key column, be non-partial,
 * and the leftmost field must be a column (not an expression) that references
 * the remote relation column. These limitations help to keep the index scan
 * similar to PK/RI index scans.
 *
 * attrmap is a map of local attributes to remote ones. We can consult this
 * map to check whether the local index attribute has a corresponding remote
 * attribute.
 *
 * Note that the limitations of index scans for replica identity full only
 * adheres to a subset of the limitations of PK/RI. For example, we support
 * columns that are marked as [NULL] or we are not interested in the [NOT
 * DEFERRABLE] aspect of constraints here. It works for us because we always
 * compare the tuples for non-PK/RI index scans. See
 * RelationFindReplTupleByIndex().
 *
 * XXX: To support partial indexes, the required changes are likely to be larger.
 * If none of the tuples satisfy the expression for the index scan, we fall-back
 * to sequential execution, which might not be a good idea in some cases.
 */
bool
IsIndexUsableForReplicaIdentityFull(Relation idxrel, AttrMap *attrmap)
{
	AttrNumber	keycol;
	oidvector  *indclass;

	/* The index must not be a partial index */
	if (!heap_attisnull(idxrel->rd_indextuple, Anum_pg_index_indpred, NULL))
		return false;

	Assert(idxrel->rd_index->indnatts >= 1);

	indclass = (oidvector *) DatumGetPointer(SysCacheGetAttrNotNull(INDEXRELID,
																	idxrel->rd_indextuple,
																	Anum_pg_index_indclass));

	/* Ensure that the index has a valid equal strategy for each key column */
	for (int i = 0; i < idxrel->rd_index->indnkeyatts; i++)
	{
		Oid			opfamily;

		opfamily = get_opclass_family(indclass->values[i]);
		if (IndexAmTranslateCompareType(COMPARE_EQ, idxrel->rd_rel->relam, opfamily, true) == InvalidStrategy)
			return false;
	}

	/*
	 * For indexes other than PK and REPLICA IDENTITY, we need to match the
	 * local and remote tuples.  The equality routine tuples_equal() cannot
	 * accept a data type where the type cache cannot provide an equality
	 * operator.
	 */
	for (int i = 0; i < idxrel->rd_att->natts; i++)
	{
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(TupleDescAttr(idxrel->rd_att, i)->atttypid, TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			return false;
	}

	/* The leftmost index field must not be an expression */
	keycol = idxrel->rd_index->indkey.values[0];
	if (!AttributeNumberIsValid(keycol))
		return false;

	/*
	 * And the leftmost index field must reference the remote relation column.
	 * This is because if it doesn't, the sequential scan is favorable over
	 * index scan in most cases.
	 */
	if (attrmap->maplen <= AttrNumberGetAttrOffset(keycol) ||
		attrmap->attnums[AttrNumberGetAttrOffset(keycol)] < 0)
		return false;

	/*
	 * The given index access method must implement "amgettuple", which will
	 * be used later to fetch the tuples.  See RelationFindReplTupleByIndex().
	 */
	if (GetIndexAmRoutineByAmId(idxrel->rd_rel->relam, false)->amgettuple == NULL)
		return false;

	return true;
}

/*
 * Return the OID of the replica identity index if one is defined;
 * the OID of the PK if one exists and is not deferrable;
 * otherwise, InvalidOid.
 */
Oid
GetRelationIdentityOrPK(Relation rel)
{
	Oid			idxoid;

	idxoid = RelationGetReplicaIndex(rel);

	if (!OidIsValid(idxoid))
		idxoid = RelationGetPrimaryKeyIndex(rel, false);

	return idxoid;
}

/*
 * Returns the index oid if we can use an index for subscriber. Otherwise,
 * returns InvalidOid.
 */
static Oid
FindLogicalRepLocalIndex(Relation localrel, LogicalRepRelation *remoterel,
						 AttrMap *attrMap)
{
	Oid			idxoid;

	/*
	 * We never need index oid for partitioned tables, always rely on leaf
	 * partition's index.
	 */
	if (localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return InvalidOid;

	/*
	 * Simple case, we already have a primary key or a replica identity index.
	 */
	idxoid = GetRelationIdentityOrPK(localrel);
	if (OidIsValid(idxoid))
		return idxoid;

	if (remoterel->replident == REPLICA_IDENTITY_FULL)
	{
		/*
		 * We are looking for one more opportunity for using an index. If
		 * there are any indexes defined on the local relation, try to pick a
		 * suitable index.
		 *
		 * The index selection safely assumes that all the columns are going
		 * to be available for the index scan given that remote relation has
		 * replica identity full.
		 *
		 * Note that we are not using the planner to find the cheapest method
		 * to scan the relation as that would require us to either use lower
		 * level planner functions which would be a maintenance burden in the
		 * long run or use the full-fledged planner which could cause
		 * overhead.
		 */
		return FindUsableIndexForReplicaIdentityFull(localrel, attrMap);
	}

	return InvalidOid;
}

/*
 * Get the LogicalRepRelMapEntry corresponding to the given relid without
 * opening the local relation.
 */
LogicalRepRelMapEntry *
logicalrep_get_relentry(LogicalRepRelId remoteid)
{
	LogicalRepRelMapEntry *entry;
	bool		found;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/* Search for existing entry. */
	entry = hash_search(LogicalRepRelMap, (void *) &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(DEBUG1, "no relation map entry for remote relation ID %u",
			 remoteid);

	return entry;
}
