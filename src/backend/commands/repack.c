/*-------------------------------------------------------------------------
 *
 * repack.c
 *    REPACK a table; formerly known as CLUSTER.  VACUUM FULL also uses
 *    parts of this code.
 *
 * There are two somewhat different ways to rewrite a table.  In non-
 * concurrent mode, it's easy: take AccessExclusiveLock, create a new
 * transient relation, copy the tuples over to the relfilenode of the new
 * relation, swap the relfilenodes, then drop the old relation.
 *
 * In concurrent mode, we lock the table with only ShareUpdateExclusiveLock,
 * then do an initial copy as above.  However, while the tuples are being
 * copied, concurrent transactions could modify the table. To cope with those
 * changes, we rely on logical decoding to obtain them from WAL.  A bgworker
 * consumes WAL while the initial copy is ongoing (to prevent excessive WAL
 * from being reserved), and accumulates the changes in a file.  Once the
 * initial copy is complete, we read the changes from the file and re-apply
 * them on the new heap.  Then we upgrade our ShareUpdateExclusiveLock to
 * AccessExclusiveLock and swap the relfilenodes.  This way, the time we hold
 * a strong lock on the table is much reduced, and the bloat is eliminated.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/repack.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/detoast.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_inherits.h"
#include "catalog/toasting.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/repack.h"
#include "commands/repack_internal.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "replication/logicalrelation.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/wait_event_types.h"

/*
 * This struct is used to pass around the information on tables to be
 * clustered. We need this so we can make a list of them when invoked without
 * a specific table/index pair.
 */
typedef struct
{
	Oid			tableOid;
	Oid			indexOid;
} RelToCluster;

/*
 * Information needed to close and re-open relation on transaction boundary.
 */
typedef struct RelReopenInfo
{
	Oid			relid;
	Relation   *p_rel;
	bool		is_new;
} RelReopenInfo;

/*
 * Information needed to release ans re-initialize ChangeContext on
 * transaction boundary.
 */
typedef struct ChangeContexBackup
{
	/* The new relation. */
	Relation	rel;
	RelReopenInfo rel_ri;
	Oid			ident_index;

	/* Auxiliary relation. */
	Relation	rel_aux;
	RelReopenInfo rel_aux_ri;
	Oid			ident_index_aux;

	/* Common fields */
	int			file_seq_snapshot;
	int			file_seq_changes;
	Oid			clustering_index;
	TransactionId	last_snapshot_xmin;
} ChangeContextBackup;

/*
 * When REPACK (CONCURRENTLY) copies data to the new heap, a new snapshot is
 * built after processing this many pages. XXX Tune the value.
 */
int			repack_pages_per_snapshot = 1024;

/*
 * Backend-local information to control the decoding worker.
 */
typedef struct DecodingWorker
{
	/* The worker. */
	BackgroundWorkerHandle *handle;

	/* DecodingWorkerShared is in this segment. */
	dsm_segment *seg;

	/* Handle of the error queue. */
	shm_mq_handle *error_mqh;
} DecodingWorker;

/* Pointer to currently running decoding worker. */
static DecodingWorker *decoding_worker = NULL;

/*
 * In the CONCURRENTLY mode, we need multiple transactions to process a single
 * table. Information that needs to survive commit should be stored in this
 * context.
 */
static MemoryContext repack_cxt = NULL;

/*
 * Is there a message sent by a repack worker that the backend needs to
 * receive?
 */
volatile sig_atomic_t RepackMessagePending = false;

static LOCKMODE RepackLockLevel(bool concurrent);
static bool cluster_rel_recheck(RepackCommand cmd, Relation OldHeap,
								Oid indexOid, Oid userid, LOCKMODE lmode,
								int options);
static void check_concurrent_repack_requirements(Relation rel,
												 Oid *ident_idx_p);
static void check_index_requirements(Relation rel, RepackCommand cmd);
static void rebuild_relation(Relation OldHeap, Relation index, bool verbose,
							 Oid ident_idx);
static ChangeContext *make_new_heap_for_repack(Oid tablespace,
											   Oid access_method,
											   char relpersistence,
											   Oid ident_idx,
											   Relation *p_old_heap,
											   Relation *p_clustering_index,
											   IndexBuildSecurity *ibsec);
static List *find_new_heaps(Oid relid_old);
static void copy_table_data(Relation NewHeap, Relation OldHeap, Relation OldIndex,
							bool verbose,
							bool *pSwapToastByContent,
							TransactionId *pFreezeXid,
							MultiXactId *pCutoffMulti,
							double *p_num_tuples,
							ChangeContext *chgcxt);
static void copy_table_data_update_stats(Relation OldHeap, Relation NewHeap,
										 BlockNumber num_pages,
										 double num_tuples);
static void update_relation_cutoffs(Oid relid, TransactionId frozenXid,
									MultiXactId cutoffMulti);
static List *get_tables_to_repack(RepackCommand cmd, bool usingindex,
								  MemoryContext permcxt);
static List *get_tables_to_repack_partitioned(RepackStmt *stmt,
											  Relation rel,
											  MemoryContext permcxt);
static bool repack_is_permitted_for_relation(RepackCommand cmd,
											 Oid relid, Oid userid,
											 bool already_locked);

static void apply_concurrent_changes(ChangeContext *chgcxt,
									 BlockNumber range_start,
									 BlockNumber range_end);
static void apply_concurrent_insert(RepackDest *dest,
									TupleTableSlot *spill_tuple,
									TupleTableSlot *new_tuple,
									TransactionId xid);
static void apply_concurrent_update(RepackDest *dest,
									TupleTableSlot *spilled_tuple,
									TupleTableSlot *ondisk_tuple,
									TupleTableSlot *new_tuple,
									TransactionId xid);
static void apply_concurrent_delete(Relation rel, TupleTableSlot *slot,
									TransactionId xid);
static void restore_tuple(BufFile *file, Relation relation,
						  TupleTableSlot *slot, BlockNumber *block_nr_p,
						  BlockNumber *old_block_nr_p,
						  TransactionId *xid_p);
static void adjust_toast_pointers(Relation relation, TupleTableSlot *dest,
								  TupleTableSlot *src);
static bool is_block_in_range(BlockNumber blknum, BlockNumber start,
							  BlockNumber end);
static bool find_target_tuple(RepackDest *dest, TupleTableSlot *locator,
							  TupleTableSlot *retrieved);
static bool identity_key_equal(RepackDest *dest,
							   TupleTableSlot *locator,
							   TupleTableSlot *candidate);
static void initialize_change_context(ChangeContext *chgcxt,
									  Relation relation,
									  Oid ident_index_id);
static void release_change_context(ChangeContext *chgcxt);
static void initialize_change_dest(RepackDest *dest, Relation relation,
								   Oid ident_index_id);
static void release_change_dest(RepackDest *dest);
static void backup_change_context(ChangeContext *chgcxt,
								  ChangeContextBackup *backup);
static ChangeContext *reinitialize_change_context(ChangeContextBackup *backup);
static void enable_session_locks(Relation rel, LOCKMODE lmode, bool is_new);
static void disable_session_locks(Relation rel, LOCKMODE lmode, bool is_new);
static void prepare_relation_for_reopening(Relation *p_rel,
										   RelReopenInfo *backup,
										   bool is_new);
static void reopen_relation(RelReopenInfo *backup);
static void rebuild_relation_finish_concurrent(Relation NewHeap, Relation OldHeap,
											   Oid identIdx,
											   TransactionId frozenXid,
											   MultiXactId cutoffMulti,
											   double num_tuples,
											   ChangeContext *chgcxt);
static ChangeContext *process_auxiliary_table(ChangeContext *chgcxt,
											  Relation *pOldHeap,
											  Relation *pNewHeap,
											  Oid identIdx,
											  TransactionId freeze_xid,
											  MultiXactId cutoff_multi);
static List *build_new_indexes(List *OldIndexes, Relation *p_old,
							   Relation *p_new,
							   ChangeContext **p_chgcxt);
static Oid	build_new_index(Relation NewHeap, Relation OldHeap, Oid oldindex);
static ChangeContext *start_new_transaction(ChangeContext *chgcxt,
											Relation *p_old, Relation *p_new);
static void copy_index_constraints(Relation old_index, Oid new_index_id,
								   Oid new_heap_id);
static void copy_attribute_defaults(Oid old_heap_oid, Oid new_heap_oid);
static Relation process_single_relation(RepackStmt *stmt,
										LOCKMODE lockmode,
										bool isTopLevel,
										ClusterParams *params);
static Oid	determine_clustered_index(Relation rel, bool usingindex,
									  const char *indexname);

static void start_repack_decoding_worker(Oid relid);
static void wait_for_repack_decoding_worker(void);
static void stop_repack_decoding_worker(void);
static void stop_repack_decoding_worker_cb(int code, Datum arg);

static void ProcessRepackMessage(StringInfo msg);
static const char *RepackCommandAsString(RepackCommand cmd);


/*
 * The repack code allows for processing multiple tables at once. Because
 * of this, we cannot just run everything on a single transaction, or we
 * would be forced to acquire exclusive locks on all the tables being
 * clustered, simultaneously --- very likely leading to deadlock.
 *
 * To solve this we follow a similar strategy to VACUUM code, processing each
 * relation in a separate transaction. For this to work, we need to:
 *
 *	- provide a separate memory context so that we can pass information in
 *	  a way that survives across transactions
 *	- start a new transaction every time a new relation is clustered
 *	- check for validity of the information on to-be-clustered relations,
 *	  as someone might have deleted a relation behind our back, or
 *	  clustered one on a different index
 *	- end the transaction
 *
 * The single-relation case does not have any such overhead.
 *
 * We also allow a relation to be repacked following an index, but without
 * naming a specific one.  In that case, the indisclustered bit will be
 * looked up, and an ERROR will be thrown if no so-marked index is found.
 */
void
ExecRepack(ParseState *pstate, RepackStmt *stmt, bool isTopLevel)
{
	ClusterParams params = {0};
	Relation	rel = NULL;
	MemoryContext repack_context;
	LOCKMODE	lockmode;
	List	   *rtcs;
	bool		verbose = false;
	bool		analyze = false;
	bool		concurrently = false;

	/* Parse option list */
	foreach_node(DefElem, opt, stmt->params)
	{
		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "analyze") == 0 ||
				 strcmp(opt->defname, "analyse") == 0)
		{
			if (stmt->command != REPACK_COMMAND_REPACK)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("ANALYZE option not supported for %s",
							   RepackCommandAsString(stmt->command)));
			analyze = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "concurrently") == 0)
		{
			if (stmt->command != REPACK_COMMAND_REPACK)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("CONCURRENTLY option not supported for %s",
							   RepackCommandAsString(stmt->command)));
			concurrently = defGetBoolean(opt);
		}
		else
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("unrecognized %s option \"%s\"",
						   RepackCommandAsString(stmt->command),
						   opt->defname),
					parser_errposition(pstate, opt->location));
	}

	params.options |=
		(verbose ? CLUOPT_VERBOSE : 0) |
		(analyze ? CLUOPT_ANALYZE : 0) |
		(concurrently ? CLUOPT_CONCURRENT : 0);

	/* Determine the lock mode to use. */
	lockmode = RepackLockLevel((params.options & CLUOPT_CONCURRENT) != 0);

	if ((params.options & CLUOPT_CONCURRENT) != 0)
	{
		/*
		 * In concurrent mode, make sure we're not in a transaction block.
		 *
		 * The reason is that repack_setup_logical_decoding() could wait
		 * indefinitely for our XID to complete. (The deadlock detector would
		 * not recognize it because we'd be waiting for ourselves, i.e. no
		 * real lock conflict.) It would be possible to run in a transaction
		 * block if we had no XID, but this restriction is simpler for users
		 * to understand and we don't lose any functionality.
		 */
		PreventInTransactionBlock(isTopLevel, "REPACK (CONCURRENTLY)");
	}
	else if ((params.options & CLUOPT_ANALYZE) != 0)
	{
		/*
		 * With ANALYZE, process_single_relation() would commit the current
		 * transaction and start a new one, which would break our state if
		 * we're in a transaction block or PL-execution environment.  Reject
		 * the option in that case.  It may be possible to remove this
		 * restriction in the future.
		 */
		PreventInTransactionBlock(isTopLevel, "REPACK (ANALYZE)");
	}

	/*
	 * If a single relation is specified, process it and we're done ... unless
	 * the relation is a partitioned table, in which case we fall through.
	 */
	if (stmt->relation != NULL)
	{
		rel = process_single_relation(stmt, lockmode, isTopLevel, &params);
		if (rel == NULL)
			return;				/* all done */
	}

	/*
	 * Don't allow ANALYZE in the multiple-relation case for now.  Maybe we
	 * can add support for this later.
	 */
	if (params.options & CLUOPT_ANALYZE)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot execute %s on multiple tables",
					   "REPACK (ANALYZE)"));

	/*
	 * By here, we know we are in a multi-table situation.
	 *
	 * Concurrent processing is currently considered rather special (e.g. in
	 * terms of resources consumed) so it is not performed in bulk.
	 */
	if (params.options & CLUOPT_CONCURRENT)
	{
		if (rel != NULL)
		{
			Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("%s is not supported for partitioned tables",
						   "REPACK (CONCURRENTLY)"),
					errhint("Consider running the command on individual partitions."));
		}
		else
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("%s requires an explicit table name",
						   "REPACK (CONCURRENTLY)"));
	}

	/*
	 * In order to avoid holding locks for too long, we want to process each
	 * table in its own transaction.  This forces us to disallow running
	 * inside a user transaction block.
	 */
	PreventInTransactionBlock(isTopLevel, RepackCommandAsString(stmt->command));

	/* Also, we need a memory context to hold our list of relations */
	repack_context = AllocSetContextCreate(PortalContext,
										   "Repack",
										   ALLOCSET_DEFAULT_SIZES);

	/*
	 * Since we open a new transaction for each relation, we have to check
	 * that the relation still is what we think it is.
	 *
	 * In single-transaction CLUSTER, we don't need the overhead.
	 */
	params.options |= CLUOPT_RECHECK;

	/*
	 * If we don't have a relation yet, determine a relation list.  If we do,
	 * then it must be a partitioned table, and we want to process its
	 * partitions.  Note that we don't acquire any locks on these tables, so
	 * the returned list must be treated with suspicion.
	 */
	if (rel == NULL)
	{
		Assert(stmt->indexname == NULL);
		rtcs = get_tables_to_repack(stmt->command, stmt->usingindex,
									repack_context);
		params.options |= CLUOPT_RECHECK_ISCLUSTERED;
	}
	else
	{
		rtcs = get_tables_to_repack_partitioned(stmt, rel, repack_context);
		rel = NULL;				/* clobber no longer valid pointer */
	}

	/* Commit to get out of starting transaction */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Cluster the tables, each in a separate transaction */
	Assert(rel == NULL);
	foreach_ptr(RelToCluster, rtc, rtcs)
	{
		/* Start a new transaction for each relation. */
		StartTransactionCommand();

		/*
		 * Open the target table.  It may have been dropped or replaced with
		 * something different, in which case silently skip it.
		 */
		rel = try_relation_open(rtc->tableOid, lockmode);
		if (rel == NULL)
		{
			CommitTransactionCommand();
			continue;
		}
		if (rel->rd_rel->relkind != RELKIND_RELATION &&
			rel->rd_rel->relkind != RELKIND_MATVIEW)
		{
			relation_close(rel, lockmode);
			CommitTransactionCommand();
			continue;
		}

		/* functions in indexes may want a snapshot set */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* Process this table */
		cluster_rel(stmt->command, rel, rtc->indexOid, &params, isTopLevel);
		/* cluster_rel closes the relation, but keeps lock */

		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	/* Start a new transaction for the cleanup work. */
	StartTransactionCommand();

	/* Clean up working storage */
	MemoryContextDelete(repack_context);
}

/*
 * In the non-concurrent case, we obtain AccessExclusiveLock throughout the
 * operation to avoid any lock-upgrade hazards.  In the concurrent case, we
 * grab ShareUpdateExclusiveLock (just like VACUUM) for most of the
 * processing and only acquire AccessExclusiveLock at the end, to swap the
 * relation -- supposedly for a short time.
 */
static LOCKMODE
RepackLockLevel(bool concurrent)
{
	if (concurrent)
		return ShareUpdateExclusiveLock;
	else
		return AccessExclusiveLock;
}

/*
 * cluster_rel
 *
 * This clusters the table by creating a new, clustered table and
 * swapping the relfilenumbers of the new table and the old table, so
 * the OID of the original table is preserved.  Thus we do not lose
 * GRANT, inheritance nor references to this table.
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new table, it's better to create the indexes afterwards than to fill
 * them incrementally while we load the table.
 *
 * If indexOid is InvalidOid, the table will be rewritten in physical order
 * instead of index order.
 *
 * Note that, in the concurrent case, the function releases the lock at some
 * point, in order to get AccessExclusiveLock for the final steps (i.e. to
 * swap the relation files). To make things simpler, the caller should expect
 * OldHeap to be closed on return, regardless CLUOPT_CONCURRENT. (The
 * AccessExclusiveLock is kept till the end of the transaction.)
 *
 * 'cmd' indicates which command is being executed, to be used for error
 * messages.
 */
void
cluster_rel(RepackCommand cmd, Relation OldHeap, Oid indexOid,
			ClusterParams *params, bool isTopLevel)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Relation	index;
	LOCKMODE	lmode;
	bool		verbose = ((params->options & CLUOPT_VERBOSE) != 0);
	bool		recheck = ((params->options & CLUOPT_RECHECK) != 0);
	bool		concurrent = ((params->options & CLUOPT_CONCURRENT) != 0);
	Oid			ident_idx = InvalidOid;
	const int	progress_index[] = {
		PROGRESS_REPACK_COMMAND,
		PROGRESS_REPACK_INDEX_RELID
	};
	const int64 progress_values[] = {cmd, indexOid};

	/* Determine the lock mode to use. */
	lmode = RepackLockLevel(concurrent);

	/*
	 * Check some preconditions in the concurrent case.  This also obtains the
	 * replica index OID.
	 */
	if (concurrent)
		check_concurrent_repack_requirements(OldHeap, &ident_idx);

	/*
	 * Also check the state of indexes; this can abort the command for REPACK.
	 * Historically this hasn't affected CLUSTER or VACUUM FULL, so don't do
	 * it for those commands.
	 */
	if (cmd == REPACK_COMMAND_REPACK)
		check_index_requirements(OldHeap, cmd);

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	pgstat_progress_start_command(PROGRESS_COMMAND_REPACK, tableOid);
	/* Report the ordering index even when using a sequential scan and sort. */
	pgstat_progress_update_multi_param(2, progress_index, progress_values);

	/*
	 * Recheck that the relation is still what it was when we started.
	 *
	 * Note that it's critical to skip this in single-relation CLUSTER;
	 * otherwise, we would reject an attempt to cluster using a
	 * not-previously-clustered index.
	 */
	if (recheck &&
		!cluster_rel_recheck(cmd, OldHeap, indexOid, GetUserId(),
							 lmode, params->options))
		goto out;

	/*
	 * We allow repacking shared catalogs only when not using an index. It
	 * would work to use an index in most respects, but the index would only
	 * get marked as indisclustered in the current database, leading to
	 * unexpected behavior if CLUSTER were later invoked in another database.
	 */
	if (OidIsValid(indexOid) && OldHeap->rd_rel->relisshared)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: first %s is name of a SQL command, eg. REPACK */
				errmsg("cannot execute %s on a shared catalog",
					   RepackCommandAsString(cmd)));

	/*
	 * The CONCURRENTLY case should have been rejected earlier because it does
	 * not support system catalogs.
	 */
	Assert(!(OldHeap->rd_rel->relisshared && concurrent));

	/*
	 * Don't process temp tables of other backends ... their local buffer
	 * manager is not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(OldHeap))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: first %s is name of a SQL command, eg. REPACK */
				errmsg("cannot execute %s on temporary tables of other sessions",
					   RepackCommandAsString(cmd)));

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 */
	CheckTableNotInUse(OldHeap, RepackCommandAsString(cmd));

	/* Check heap and index are valid to cluster on */
	if (OidIsValid(indexOid))
	{
		/* verify the index is good and lock it */
		check_index_is_clusterable(OldHeap, indexOid, lmode);
		/* also open it */
		index = index_open(indexOid, NoLock);
	}
	else
		index = NULL;

	/*
	 * When allow_system_table_mods is turned off, we disallow repacking a
	 * catalog on a particular index unless that's already the clustered index
	 * for that catalog.
	 *
	 * XXX We don't check for this in CLUSTER, because it's historically been
	 * allowed.
	 */
	if (cmd != REPACK_COMMAND_CLUSTER &&
		!allowSystemTableMods && OidIsValid(indexOid) &&
		IsCatalogRelation(OldHeap) && !index->rd_index->indisclustered)
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("permission denied: \"%s\" is a system catalog",
					   RelationGetRelationName(OldHeap)),
				errdetail("System catalogs can only be clustered by the index they're already clustered on, if any, unless \"%s\" is enabled.",
						  "allow_system_table_mods"));

	/*
	 * Quietly ignore the request if this is a materialized view which has not
	 * been populated from its query. No harm is done because there is no data
	 * to deal with, and we don't want to throw an error if this is part of a
	 * multi-relation request -- for example, CLUSTER was run on the entire
	 * database.
	 */
	if (OldHeap->rd_rel->relkind == RELKIND_MATVIEW &&
		!RelationIsPopulated(OldHeap))
	{
		if (index)
			index_close(index, lmode);
		relation_close(OldHeap, lmode);
		goto out;
	}

	Assert(OldHeap->rd_rel->relkind == RELKIND_RELATION ||
		   OldHeap->rd_rel->relkind == RELKIND_MATVIEW ||
		   OldHeap->rd_rel->relkind == RELKIND_TOASTVALUE);

	/*
	 * All predicate locks on the tuples or pages are about to be made
	 * invalid, because we move tuples around.  Promote them to relation
	 * locks.  Predicate locks on indexes will be promoted when they are
	 * reindexed.
	 *
	 * During concurrent processing, the heap as well as its indexes stay in
	 * operation, so we postpone this step until they are locked using
	 * AccessExclusiveLock near the end of the processing.
	 */
	if (!concurrent)
		TransferPredicateLocksToHeapRelation(OldHeap);

	/*
	 * rebuild_relation does all the dirty work, and closes OldHeap and index,
	 * if valid.
	 *
	 * In concurrent mode, make sure the worker terminates; normally it does
	 * so by itself, but a PG_ENSURE_ERROR_CLEANUP callback ensures that this
	 * happens even in case this backend dies early on a FATAL exit.  Normal
	 * mode doesn't need that overhead.
	 */
	if (concurrent)
	{
		/*
		 * In the CONCURRENTLY mode, new transactions may be started. Make
		 * sure we can preserve information across transaction boundaries.
		 */
		if (repack_cxt == NULL)
			repack_cxt = AllocSetContextCreate(TopMemoryContext,
											   "Repack - Top",
											   ALLOCSET_DEFAULT_SIZES);
		else
			MemoryContextReset(repack_cxt);

		PG_ENSURE_ERROR_CLEANUP(stop_repack_decoding_worker_cb, 0);
		{
			rebuild_relation(OldHeap, index, verbose, ident_idx);
		}
		PG_END_ENSURE_ERROR_CLEANUP(stop_repack_decoding_worker_cb, 0);
		stop_repack_decoding_worker();
	}
	else
		rebuild_relation(OldHeap, index, verbose, ident_idx);

out:
	pgstat_progress_end_command();
}

/*
 * Check if the table (and its index) still meets the requirements of
 * cluster_rel().
 */
static bool
cluster_rel_recheck(RepackCommand cmd, Relation OldHeap, Oid indexOid,
					Oid userid, LOCKMODE lmode, int options)
{
	Oid			tableOid = RelationGetRelid(OldHeap);

	Assert(CheckRelationLockedByMe(OldHeap, lmode, false));

	/* Check that the user still has privileges for the relation */
	if (!repack_is_permitted_for_relation(cmd, tableOid, userid, true))
	{
		relation_close(OldHeap, lmode);
		return false;
	}

	/*
	 * Silently skip a temp table for a remote session.  Only doing this check
	 * in the "recheck" case is appropriate (which currently means somebody is
	 * executing a database-wide CLUSTER or on a partitioned table), because
	 * there is another check in cluster() which will stop any attempt to
	 * cluster remote temp tables by name.  There is another check in
	 * cluster_rel which is redundant, but we leave it for extra safety.
	 */
	if (RELATION_IS_OTHER_TEMP(OldHeap))
	{
		relation_close(OldHeap, lmode);
		return false;
	}

	if (OidIsValid(indexOid))
	{
		/*
		 * Check that the index still exists
		 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexOid)))
		{
			relation_close(OldHeap, lmode);
			return false;
		}

		/*
		 * Check that the index is still the one with indisclustered set, if
		 * needed.
		 */
		if ((options & CLUOPT_RECHECK_ISCLUSTERED) != 0 &&
			!get_index_isclustered(indexOid))
		{
			relation_close(OldHeap, lmode);
			return false;
		}
	}

	return true;
}

/*
 * Verify that the specified heap and index are valid to cluster on
 *
 * Side effect: obtains lock on the index.  The caller may
 * in some cases already have a lock of the same strength on the table, but
 * not in all cases so we can't rely on the table-level lock for
 * protection here.
 */
void
check_index_is_clusterable(Relation OldHeap, Oid indexOid, LOCKMODE lockmode)
{
	Relation	OldIndex;

	OldIndex = index_open(indexOid, lockmode);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != RelationGetRelid(OldHeap))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(OldIndex),
						RelationGetRelationName(OldHeap))));

	/* Index AM must allow clustering */
	if (!OldIndex->rd_indam->amclusterable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on index \"%s\" because access method does not support clustering",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow clustering on incomplete indexes (those that might not index
	 * every row of the relation).  We could relax this by making a separate
	 * seqscan pass over the table to copy the missing rows, but that seems
	 * expensive and tedious.
	 */
	if (!heap_attisnull(OldIndex->rd_indextuple, Anum_pg_index_indpred, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on partial index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow if index is left over from a failed CREATE INDEX CONCURRENTLY;
	 * it might well not contain entries for every heap row, or might not even
	 * be internally consistent.  (But note that we don't check indcheckxmin;
	 * the worst consequence of following broken HOT chains would be that we
	 * might put recently-dead tuples out-of-order in the new table, and there
	 * is little harm in that.)
	 */
	if (!OldIndex->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on invalid index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/* Drop relcache refcnt on OldIndex, but keep lock */
	index_close(OldIndex, NoLock);
}

/*
 * mark_index_clustered: mark the specified index as the one clustered on
 *
 * With indexOid == InvalidOid, will mark all indexes of rel not-clustered.
 */
void
mark_index_clustered(Relation rel, Oid indexOid, bool is_internal)
{
	HeapTuple	indexTuple;
	Form_pg_index indexForm;
	Relation	pg_index;
	ListCell   *index;

	Assert(rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE);

	/*
	 * If the index is already marked clustered, no need to do anything.
	 */
	if (OidIsValid(indexOid))
	{
		if (get_index_isclustered(indexOid))
			return;
	}

	/*
	 * Check each index of the relation and set/clear the bit as needed.
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Unset the bit if set.  We know it's wrong because we checked this
		 * earlier.
		 */
		if (indexForm->indisclustered)
		{
			indexForm->indisclustered = false;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}
		else if (thisIndexOid == indexOid)
		{
			/* this was checked earlier, but let's be real sure */
			if (!indexForm->indisvalid)
				elog(ERROR, "cannot cluster on invalid index %u", indexOid);
			indexForm->indisclustered = true;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);
		}

		InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
									 InvalidOid, is_internal);

		heap_freetuple(indexTuple);
	}

	table_close(pg_index, RowExclusiveLock);
}

/*
 * check_index_requirements: verify index state on relation being processed
 *
 * Throw an error if any incompletely-built indexes are found.
 *
 * Indexes that are not ready for inserts, such as ones left behind by failed
 * CREATE INDEX CONCURRENTLY, are not maintained by DML.  Indexes that aren't
 * marked valid could have been in the middle of validation when their build
 * failed, and thus it's not certain that they could be built.  In both cases,
 * attempting to rebuild may fail altogether.  Throwing an error here forces
 * the user to take action on these indexes separately from the table
 * reconstruction, which prevents perpetuating them for no reason.
 */
static void
check_index_requirements(Relation rel, RepackCommand cmd)
{
	Relation	indrel;
	SysScanDesc indscan;
	ScanKeyData skey;
	HeapTuple	htup;
	int			num_invalid_idxs = 0;
	StringInfoData dest;

	initStringInfo(&dest);

	/* Prepare to scan pg_index for entries having indrelid = this rel. */
	ScanKeyInit(&skey,
				Anum_pg_index_indrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	indrel = table_open(IndexRelationId, AccessShareLock);
	indscan = systable_beginscan(indrel, IndexIndrelidIndexId, true,
								 NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(indscan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(htup);

		if (!index->indisvalid)
		{
			if (num_invalid_idxs == 0)
				appendStringInfo(&dest, _("\"%s\""), get_rel_name(index->indexrelid));
			else
				appendStringInfo(&dest, _(", \"%s\""), get_rel_name(index->indexrelid));
			num_invalid_idxs++;
		}
	}
	systable_endscan(indscan);
	table_close(indrel, AccessShareLock);

	if (num_invalid_idxs > 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot execute %s on relation \"%s\"",
					   RepackCommandAsString(cmd), RelationGetRelationName(rel)),
				errdetail_plural("An invalid index cannot be processed correctly: %s.",
								 "Some invalid indexes cannot be processed correctly: %s.",
								 num_invalid_idxs,
								 dest.data),
				errhint("Use DROP INDEX or REINDEX."));
}

/*
 * Check if the CONCURRENTLY option is legal for the relation.
 *
 * *Ident_idx_p receives OID of the identity index.
 */
static void
check_concurrent_repack_requirements(Relation rel, Oid *ident_idx_p)
{
	char		relpersistence,
				replident;
	Oid			ident_idx;

	if (wal_level < WAL_LEVEL_REPLICA)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cannot execute %s in this configuration",
					   "REPACK (CONCURRENTLY)"),
				errdetail("This operation requires \"wal_level\" to be set to \"replica\" or higher."));

	/*
	 * A table AM that doesn't support logical decoding would cause REPACK
	 * (CONCURRENTLY) to silently lose the changes made during the rewrite.
	 * Nothing in TableAmRoutine tells us whether it does, so for now restrict
	 * to heap. Check the routine rather than the AM OID, so that an AM
	 * reusing the heap handler still works.
	 */
	if (rel->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail("This operation is only supported for the \"heap\" access method."));

	/* Data changes in system relations are not logically decoded. */
	if (IsCatalogRelation(rel))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail("This operation is not supported for system catalogs."));

	/*
	 * REPACK (CONCURRENTLY) is not MVCC-safe; it doesn't preserve visibility
	 * information, which logical decoding needs because it reads user catalog
	 * tables under a historic snapshot. Removing this check requires making
	 * it MVCC-safe and logical rewrite mappings.
	 */
	if (RelationIsUsedAsCatalogTable(rel))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail("This operation is not supported for user catalog tables."));

	/*
	 * reorderbuffer.c does not seem to handle processing of TOAST relation
	 * alone.
	 */
	if (IsToastRelation(rel))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail("This operation is not supported for TOAST tables."));

	relpersistence = rel->rd_rel->relpersistence;
	if (relpersistence != RELPERSISTENCE_PERMANENT)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail("This operation is only supported for permanent relations."));

	/* A materialized view produces no logically decoded changes. */
	if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail_relkind_not_supported(rel->rd_rel->relkind));

	/*
	 * With NOTHING, WAL does not contain the old tuple; FULL is not yet
	 * supported.
	 */
	replident = rel->rd_rel->relreplident;
	if (replident == REPLICA_IDENTITY_NOTHING ||
		replident == REPLICA_IDENTITY_FULL)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail("This operation does not support tables with %s.",
						  replident == REPLICA_IDENTITY_NOTHING ?
						  "REPLICA IDENTITY NOTHING" : "REPLICA IDENTITY FULL"));

	/*
	 * Obtain the replica identity index to use.  If there isn't one, the
	 * table cannot be repacked concurrently.  (Replica identity FULL is not
	 * supported yet.)
	 */
	ident_idx = RelationGetReplicaIndex(rel);
	if (!OidIsValid(ident_idx))
	{
		/* This special case warrants its own error message */
		if (OidIsValid(rel->rd_pkindex) && rel->rd_ispkdeferrable)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot execute %s on relation \"%s\"",
						   "REPACK (CONCURRENTLY)",
						   RelationGetRelationName(rel)),
					errdetail("This operation does not support deferrable primary keys."),
					errhint("Use ALTER TABLE ... REPLICA IDENTITY USING INDEX to designate another index as replica identity."));

		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot execute %s on relation \"%s\"",
					   "REPACK (CONCURRENTLY)", RelationGetRelationName(rel)),
				errdetail("Relation \"%s\" has no identity index.",
						RelationGetRelationName(rel)));
	}

	/*
	 * In the CONCURRENTLY mode we don't want to use the same snapshot
	 * throughout the whole processing, as it could block the progress of xmin
	 * horizon. Assert should be ok as we already disallow transaction block
	 * in the CONCURRENTLY case.
	 */
	Assert(!IsolationUsesXactSnapshot());

	*ident_idx_p = ident_idx;
}

/*
 * rebuild_relation: rebuild an existing relation in index or physical order
 *
 * OldHeap: table to rebuild.  See cluster_rel() for comments on the required
 * lock strength.
 *
 * index: index to cluster by, or NULL to rewrite in physical order.
 *
 * ident_idx: identity index, to handle replaying of concurrent data changes
 * to the new heap. InvalidOid if there's no CONCURRENTLY option.
 *
 * On entry, heap and index (if one is given) must be open, and the
 * appropriate lock held on them -- AccessExclusiveLock for exclusive
 * processing and ShareUpdateExclusiveLock for concurrent processing.
 *
 * On exit, they are closed, but still locked with AccessExclusiveLock.
 * (The function handles the lock upgrade if 'concurrent' is true.)
 */
static void
rebuild_relation(Relation OldHeap, Relation index, bool verbose,
				 Oid ident_idx)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			accessMethod = OldHeap->rd_rel->relam;
	Oid			tableSpace = OldHeap->rd_rel->reltablespace;
	Oid			OIDNewHeap = InvalidOid;
	Relation	NewHeap = NULL;
	char		relpersistence;
	bool		swap_toast_by_content;
	TransactionId frozenXid;
	MultiXactId cutoffMulti;
	bool		concurrent = OidIsValid(ident_idx);
	double		num_tuples = 0;
	IndexBuildSecurity ibsec;
	ChangeContext *chgcxt = NULL;
#if USE_ASSERT_CHECKING
	LOCKMODE	lmode;

	lmode = RepackLockLevel(concurrent);

	Assert(CheckRelationLockedByMe(OldHeap, lmode, false));
	Assert(index == NULL || CheckRelationLockedByMe(index, lmode, false));
#endif

	/*
	 * Prevent index functions from doing what they are not supposed to.
	 */
	enable_index_build_security(OldHeap->rd_rel->relowner, &ibsec);

	if (concurrent)
	{
		MemoryContext oldcxt;

		/*
		 * The worker needs to be member of the locking group we're the leader
		 * of. We ought to become the leader before the worker starts. The
		 * worker will join the group as soon as it starts.
		 *
		 * This is to make sure that the deadlock described below is
		 * detectable by deadlock.c: if the worker waits for a transaction to
		 * complete and we are waiting for the worker output, then effectively
		 * we (i.e. this backend) are waiting for that transaction.
		 */
		BecomeLockGroupLeader();

		/*
		 * Start the worker that decodes data changes applied while we're
		 * copying the table contents.
		 *
		 * Note that the worker has to wait for all transactions with XID
		 * already assigned to finish. If some of those transactions is
		 * waiting for a lock conflicting with ShareUpdateExclusiveLock on our
		 * table (e.g.  it runs CREATE INDEX), we can end up in a deadlock.
		 * Not sure this risk is worth unlocking/locking the table (and its
		 * clustering index) and checking again if it's still eligible for
		 * REPACK CONCURRENTLY.
		 *
		 * Use a transaction context that survives transaction commit(s).
		 */
		oldcxt = MemoryContextSwitchTo(repack_cxt);
		start_repack_decoding_worker(tableOid);
		MemoryContextSwitchTo(oldcxt);
	}

	/* for CLUSTER or REPACK USING INDEX, mark the index as the one to use */
	if (index != NULL)
		mark_index_clustered(OldHeap, RelationGetRelid(index), true);

	/* Remember info about rel before closing OldHeap */
	relpersistence = OldHeap->rd_rel->relpersistence;

	if (concurrent)
	{
		/*
		 * Create the transient table that will receive the re-ordered data,
		 * and possibly the auxiliary table for the ordered data.
		 */
		chgcxt = make_new_heap_for_repack(tableSpace, accessMethod,
										  relpersistence, ident_idx,
										  &OldHeap, &index, &ibsec);
		NewHeap = chgcxt->cc_dest.rel;
	}
	else
	{
		Assert(CheckRelationLockedByMe(OldHeap, AccessExclusiveLock, false));

		/*
		 * Nothing special here. No locking as the caller should already have
		 * a lock on the old relation, and the new one will be locked by
		 * make_new_heap() anyway.
		 */
		OIDNewHeap = make_new_heap(tableOid, tableSpace, accessMethod,
								   relpersistence, NoLock, false);
		/* No additional lock needed. */
		Assert(CheckRelationOidLockedByMe(OIDNewHeap, AccessExclusiveLock,
										  false));
		NewHeap = table_open(OIDNewHeap, NoLock);
	}

	/* Copy the heap data into the new table in the desired order */
	copy_table_data(NewHeap, OldHeap, index, verbose,
					&swap_toast_by_content, &frozenXid, &cutoffMulti,
					&num_tuples, chgcxt);

	if (concurrent)
	{
		Assert(!swap_toast_by_content);

		/*
		 * Close the index, but keep the lock. Both heaps will be closed by
		 * the following call.
		 */
		if (index)
			index_close(index, NoLock);

		rebuild_relation_finish_concurrent(NewHeap, OldHeap, ident_idx,
										   frozenXid, cutoffMulti, num_tuples,
										   chgcxt);

		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_FINAL_CLEANUP);

		/*
		 * REPACK (CONCURRENTLY) launches separate transaction(s) so it
		 * shouldn't rely on the current portal to pop the active snapshot.
		 */
		PopActiveSnapshot();
	}
	else
	{
		bool		is_system_catalog = IsSystemRelation(OldHeap);

		/* Close relcache entries, but keep lock until transaction commit */
		table_close(OldHeap, NoLock);
		if (index)
			index_close(index, NoLock);

		/*
		 * Close the new relation so it can be dropped as soon as the storage
		 * is swapped. The relation is not visible to others, so no need to
		 * unlock it explicitly.
		 */
		table_close(NewHeap, NoLock);

		/*
		 * Swap the physical files of the target and transient tables, then
		 * rebuild the target's indexes and throw away the transient table.
		 *
		 * If swap_toast_by_content is false, we don't need to update the
		 * cutoffs because the TOAST relation is new.
		 */
		finish_heap_swap(tableOid, OIDNewHeap, is_system_catalog,
						 swap_toast_by_content, false, true,
						 true,	/* reindex */
						 frozenXid, cutoffMulti,
						 swap_toast_by_content, /* update_toast_cutoffs */
						 relpersistence);
	}

	/* Relax the restrictions imposed above. */
	disable_index_build_security(&ibsec);
}

/*
 * Wrapper around make_new_heap() to handle specifics of REPACK
 * (CONCURRENTLY).
 *
 * 'ident_idx' is the identity index, to handle replaying of concurrent data
 * changes to the new heap.
 *
 * The function starts a new transaction. Therefore, both old heap and
 * clustering index are passed in the form of pointer so the caller has the
 * correct entries in the new transaction. New heap is returned in the
 * ChangeContext object. On return, the new heap will be locked in
 * AccessExclusiveLock mode.
 */
static ChangeContext *
make_new_heap_for_repack(Oid tablespace, Oid access_method,
						 char relpersistence, Oid ident_idx,
						 Relation *p_old_heap, Relation *p_clustering_index,
						 IndexBuildSecurity *ibsec)
{
	Relation	old_heap = *p_old_heap;
	Relation	clustering_index = *p_clustering_index;
	List	   *existing = NIL;
	Relation	new_heap;
	Oid			old_heap_oid = RelationGetRelid(old_heap);
	bool		need_aux_rel;
	Oid			new_heap_oid;
	ObjectAddress old_obj,
				new_obj;
	Oid			ident_idx_new;
	Oid			aux_oid = InvalidOid;
	Oid			aux_ident_idx = InvalidOid;
	Oid			clustering_index_oid = InvalidOid;
	Relation	aux_rel = NULL;
	ChangeContext *chgcxt;

	/*
	 * In the CONCURRENTLY case, we do the catalog changes in a separate
	 * transaction so that we do not have XID assigned while copying the data.
	 * (That XID would block the progress of the xmin horizon for VACUUM.)
	 * Therefore, the new relation will be created in a separate transaction
	 * and thus be visible to other transactions, although our lock should not
	 * let them access it. However, if previous run of REPACK ended with
	 * ERROR, such relation may still be there.
	 *
	 * Besides that, an auxiliary relation (for tuple sorting) might exist.
	 * Thus we expect a list of zero, one or two items.
	 *
	 * Since the relations should depend on the old one (see below), we use
	 * the pg_depend catalog to find it.
	 */
	existing = find_new_heaps(old_heap_oid);
	Assert(list_length(existing) <= 2);
	foreach_oid(existing_oid, existing)
	{
		/* There appears to be one, so simply drop it. */
		new_obj.classId = RelationRelationId;
		new_obj.objectId = existing_oid;
		new_obj.objectSubId = InvalidOid;

		/*
		 * User is not supposed to create any dependencies on the relation, so
		 * CASCADE.
		 */
		performDeletion(&new_obj, DROP_CASCADE, PERFORM_DELETION_INTERNAL);
	}

	/*
	 * The old heap should already be locked by the caller. As for the new
	 * one, lock is needed because the commit below will make it visible to
	 * other transactions. make_new_heap() should lock it.
	 */
	Assert(CheckRelationLockedByMe(old_heap, ShareUpdateExclusiveLock,
								   false));
	new_heap_oid = make_new_heap(old_heap_oid, tablespace, access_method,
								 relpersistence, NoLock, false);
	Assert(CheckRelationOidLockedByMe(new_heap_oid, AccessExclusiveLock,
									  false));

	new_heap = table_open(new_heap_oid, NoLock);

	/*
	 * Create a copy of the attribute defaults on the temp table, which the
	 * executor needs when replaying concurrent data changes.
	 */
	copy_attribute_defaults(old_heap_oid, new_heap_oid);

	/*
	 * Auxiliary table is needed for clustering in the CONCURRENTLY mode, see
	 * comments in ChangeContext. FIXME Non-btree indexes are allowed
	 * historically, but in general, these can hardly define any useful order.
	 * We ignore them here.
	 */
	need_aux_rel = clustering_index != NULL &&
		clustering_index->rd_rel->relam == BTREE_AM_OID;
	if (!need_aux_rel)
	{
		/*
		 * Create the identity index. We will need it during data copying so
		 * that we can apply the data changes at the appropriate time - see
		 * comments around the call of repack_process_concurrent_changes()
		 * with block range specified.
		 *
		 * XXX NewHeap is empty - should we pass INDEX_CREATE_SKIP_BUILD?
		 */
		ident_idx_new = build_new_index(new_heap, old_heap, ident_idx);
	}
	else
	{
		/*
		 * As the concurrent data changes will be applied to the auxiliary
		 * heap, the new heap does not need the identity index yet. We'll
		 * build it after having copied the data from the auxiliary heap.
		 * (Bulk insert should be more efficient.)
		 */
		ident_idx_new = InvalidOid;

		/*
		 * Create the auxiliary table - like the new heap above, but only
		 * unlogged - neither crash recovery nor replication is needed.
		 */
		aux_oid = make_new_heap(old_heap_oid, tablespace, access_method,
								RELPERSISTENCE_UNLOGGED, NoLock, true);
		Assert(CheckRelationOidLockedByMe(aux_oid, AccessExclusiveLock,
										  false));

		/*
		 * The same for identity index. (The additional
		 * ShareUpdateExclusiveLock on ident_idx is not a problem, it'll be
		 * released at the end of transaction.)
		 */
		aux_rel = table_open(aux_oid, NoLock);
		aux_ident_idx = build_new_index(aux_rel, old_heap, ident_idx);

		clustering_index_oid = RelationGetRelid(clustering_index);

		/*
		 * Copy the attribute defaults as we did for the new heap above - the
		 * concurrent changes also need to be applied to the auxiliary table.
		 */
		copy_attribute_defaults(old_heap_oid, aux_oid);
	}

	/*
	 * Create dependencies on the old heap - it ensures that dropping the old
	 * heap automatically cascades to the new ones.
	 */
	old_obj.classId = RelationRelationId;
	old_obj.objectId = old_heap_oid;
	old_obj.objectSubId = InvalidOid;
	new_obj.classId = RelationRelationId;
	new_obj.objectId = new_heap_oid;
	new_obj.objectSubId = InvalidOid;
	recordDependencyOn(&new_obj, &old_obj, DEPENDENCY_AUTO);

	if (OidIsValid(aux_oid))
	{
		new_obj.objectId = aux_oid;
		recordDependencyOn(&new_obj, &old_obj, DEPENDENCY_AUTO);
	}

	/*
	 * Commit will release the locks, so make sure no one can alter or drop
	 * the relations until we're done.
	 */
	enable_session_locks(old_heap, ShareUpdateExclusiveLock, false);
	/* NoLock for simplicity, commit will release the existing lock anyway. */
	table_close(old_heap, NoLock);

	/* Likewise, acquire session lock for the new heap. */
	enable_session_locks(new_heap, AccessExclusiveLock, true);
	table_close(new_heap, NoLock);

	/*
	 * The same for the auxiliary relation and clustering index if ordering is
	 * required.
	 */
	if (need_aux_rel)
	{
		enable_session_locks(aux_rel, AccessExclusiveLock, true);
		table_close(aux_rel, NoLock);

		/* is_new does not matter for index. */
		enable_session_locks(clustering_index, ShareUpdateExclusiveLock,
							 false);
		index_close(clustering_index, NoLock);
	}

	/*
	 * Even if the clustering index is not appropriate for sorting, we ought
	 * to close it before committing the transaction.
	 */
	else if (clustering_index)
		index_close(clustering_index, NoLock);

	/*
	 * Commit the current transaction and start a new one.
	 */
	disable_index_build_security(ibsec);
	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	enable_index_build_security(ibsec->userid, ibsec);

	/*
	 * Open the new heap in the new transaction and disable the session
	 * lock(s).
	 */
	new_heap = table_open(new_heap_oid, NoLock);
	disable_session_locks(new_heap, AccessExclusiveLock, true);

	/*
	 * Allocate and initialize memory for the output information. We shouldn't
	 * have done it in the memory context of the previous transaction.
	 */
	chgcxt = palloc0_object(ChangeContext);
	initialize_change_context(chgcxt, new_heap, ident_idx_new);
	chgcxt->cc_ind_build_sec = *ibsec;
	if (need_aux_rel)
	{
		/*
		 * Make the auxiliary relation ready for use.
		 */
		chgcxt->cc_dest_aux = palloc0_object(RepackDest);
		aux_rel = table_open(aux_oid, NoLock);
		disable_session_locks(aux_rel, AccessExclusiveLock, true);
		initialize_change_dest(chgcxt->cc_dest_aux, aux_rel,
							   aux_ident_idx);

		/*
		 * Set OID of the old relation's clustering index if it's different
		 * from the identity index. Otherwise set InvalidOid to indicate that
		 * the identity index should be used for clustering.
		 */
		if (clustering_index_oid != ident_idx)
			chgcxt->cc_clustering_index = clustering_index_oid;
		else
			chgcxt->cc_clustering_index = InvalidOid;

		/* Re-open the clustering index. */
		clustering_index = index_open(clustering_index_oid, NoLock);
		/* is_new does not matter for index. */
		disable_session_locks(clustering_index, ShareUpdateExclusiveLock,
							  false);
	}
	else
		clustering_index = NULL;

	/*
	 * Re-open the old heap.
	 */
	old_heap = table_open(old_heap_oid, NoLock);
	disable_session_locks(old_heap, ShareUpdateExclusiveLock, false);

	*p_old_heap = old_heap;
	*p_clustering_index = clustering_index;

	return chgcxt;
}

/*
 * Check if new heap, and possibly also an auxiliary heap, already exists for
 * given old heap - typically due to failed REPACK (CONCURRENTLY). Return OIDs
 * of in a list or NIL if there is none.
 */
static List *
find_new_heaps(Oid relid_old)
{
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *result = NIL;

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				RelationRelationId);
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid_old));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));

	depRel = table_open(DependRelationId, AccessShareLock);
	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform;

		/*
		 * There should be AUTO dependency from another table, which is the
		 * new or the auxiliary one. If we found any other, just ignore them.
		 */
		depform = (Form_pg_depend) GETSTRUCT(tup);
		if (depform->classid == RelationRelationId &&
			depform->deptype == DEPENDENCY_AUTO)
		{
			Relation	rel;
			Form_pg_class classForm;
			Oid			matched = InvalidOid;

			rel = relation_open(depform->objid, AccessShareLock);

			/*
			 * The new relation's relrewrite should point to the old relation.
			 */
			classForm = rel->rd_rel;
			if (classForm->relrewrite == relid_old)
				matched = depform->objid;
			table_close(rel, AccessShareLock);

			if (OidIsValid(matched))
				result = lappend_oid(result, matched);
		}
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return result;
}

/*
 * Create the transient table that will be filled with new data during
 * CLUSTER, ALTER TABLE, and similar operations.  The transient table
 * duplicates the logical structure of the OldHeap; but will have the
 * specified physical storage properties NewTableSpace, NewAccessMethod, and
 * relpersistence.
 *
 * After this, the caller should load the new heap with transferred/modified
 * data, then call finish_heap_swap to complete the operation.
 */
Oid
make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
			  char relpersistence, LOCKMODE lockmode, bool auxiliary)
{
	TupleDesc	OldHeapDesc;
	char		NewHeapName[NAMEDATALEN];
	Oid			OIDNewHeap;
	Oid			toastid;
	Relation	OldHeap;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isNull;
	Oid			namespaceid;
	const char *suffix;

	OldHeap = table_open(OIDOldHeap, lockmode);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * Note that the NewHeap will not receive any of the defaults or
	 * constraints associated with the OldHeap; we don't need 'em, and there's
	 * no reason to spend cycles inserting them into the catalogs only to
	 * delete them.
	 */

	/*
	 * But we do want to use reloptions of the old heap for new heap.
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(OIDOldHeap));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								 &isNull);
	if (isNull)
		reloptions = (Datum) 0;

	if (relpersistence == RELPERSISTENCE_TEMP)
		namespaceid = LookupCreationNamespace("pg_temp");
	else
		namespaceid = RelationGetNamespace(OldHeap);

	/*
	 * Create the new heap, using a temporary name in the same namespace as
	 * the existing table.  NOTE: there is some risk of collision with user
	 * relnames.  Working around this seems more trouble than it's worth; in
	 * particular, we can't create the new heap in a different namespace from
	 * the old, or we will have problems with the TEMP status of temp tables.
	 *
	 * Note: the new heap is not a shared relation, even if we are rebuilding
	 * a shared rel.  However, we do make the new heap mapped if the source is
	 * mapped.  This simplifies swap_relation_files, and is absolutely
	 * necessary for rebuilding pg_class, for reasons explained there.
	 */
	suffix = auxiliary ? "_aux" : "";
	snprintf(NewHeapName, sizeof(NewHeapName), "pg_temp_%u%s", OIDOldHeap,
			 suffix);

	OIDNewHeap = heap_create_with_catalog(NewHeapName,
										  namespaceid,
										  NewTableSpace,
										  InvalidOid,
										  InvalidOid,
										  InvalidOid,
										  OldHeap->rd_rel->relowner,
										  NewAccessMethod,
										  OldHeapDesc,
										  NIL,
										  RELKIND_RELATION,
										  relpersistence,
										  false,
										  RelationIsMapped(OldHeap),
										  ONCOMMIT_NOOP,
										  reloptions,
										  false,
										  true,
										  true,
										  OIDOldHeap,
										  NULL);
	Assert(OIDNewHeap != InvalidOid);

	ReleaseSysCache(tuple);

	/*
	 * Advance command counter so that the newly-created relation's catalog
	 * tuples will be visible to table_open.
	 */
	CommandCounterIncrement();

	/*
	 * If necessary, create a TOAST table for the new relation.
	 *
	 * If the relation doesn't have a TOAST table already, we can't need one
	 * for the new relation.  The other way around is possible though: if some
	 * wide columns have been dropped, NewHeapCreateToastTable can decide that
	 * no TOAST table is needed for the new table.
	 *
	 * Note that NewHeapCreateToastTable ends with CommandCounterIncrement, so
	 * that the TOAST table will be visible for insertion.
	 */
	toastid = OldHeap->rd_rel->reltoastrelid;
	if (OidIsValid(toastid))
	{
		/* keep the existing toast table's reloptions, if any */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);
		reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									 &isNull);
		if (isNull)
			reloptions = (Datum) 0;

		NewHeapCreateToastTable(OIDNewHeap, reloptions, lockmode, toastid);

		ReleaseSysCache(tuple);
	}

	table_close(OldHeap, NoLock);

	return OIDNewHeap;
}

bool
tuple_needs_reform(HeapTuple tuple, TupleDesc tupDesc)
{
	/*
	 * A short tuple might require values from attmissing val, so activate the
	 * coding unconditionally in that case.  The value might legitimally be
	 * NULL otherwise, so this is slightly wasteful, but it probably beats
	 * having to test each attribute for presence of attmissingval each time.
	 */
	if (HeapTupleHeaderGetNatts(tuple->t_data) < tupDesc->natts)
		return true;

	/* Does it have dropped attributes? */
	for (int i = 0; i < tupDesc->natts; i++)
	{
		if (TupleDescCompactAttr(tupDesc, i)->attisdropped &&
			!heap_attisnull(tuple, i + 1, tupDesc))
			return true;
	}

	return false;
}

/*
 * Subroutine for reform_and_rewrite_tuple and heap_insert_for_repack.
 *
 * Set values of dropped columns to NULL,
 */
void
clear_dropped_attributes(HeapTuple tuple, TupleTableSlot *reform)
{
	TupleDesc	tupDesc = reform->tts_tupleDescriptor;

	Assert(TTS_IS_VIRTUAL(reform) || TTS_IS_HEAPTUPLE(reform));
	ExecForceStoreHeapTuple(tuple, reform, false);

	for (int i = 0; i < tupDesc->natts; i++)
	{
		if (TupleDescCompactAttr(tupDesc, i)->attisdropped)
		{
			/*
			 * If 'reform' is virtual, all the attributes are already
			 * deformed. XXX Should we use the virtual slot at all? .
			 */
			if (TTS_IS_HEAPTUPLE(reform))
				slot_getsomeattrs(reform, i + 1);

			reform->tts_isnull[i] = true;
		}
	}
}

/*
 * Do the physical copying of table data.
 *
 * There are three output parameters:
 * *pSwapToastByContent is set true if toast tables must be swapped by content.
 * *pFreezeXid receives the TransactionId used as freeze cutoff point.
 * *pCutoffMulti receives the MultiXactId used as a cutoff point.
 * *p_num_tuples receives the number of tuples copied.
 */
static void
copy_table_data(Relation NewHeap, Relation OldHeap, Relation OldIndex,
				bool verbose, bool *pSwapToastByContent,
				TransactionId *pFreezeXid, MultiXactId *pCutoffMulti,
				double *p_num_tuples, ChangeContext *chgcxt)
{
	TupleDesc	oldTupDesc PG_USED_FOR_ASSERTS_ONLY;
	TupleDesc	newTupDesc PG_USED_FOR_ASSERTS_ONLY;
	VacuumParams params;
	struct VacuumCutoffs cutoffs;
	bool		use_sort;
	double		num_tuples = 0,
				tups_vacuumed = 0,
				tups_recently_dead = 0;
	int			elevel = verbose ? INFO : DEBUG2;
	PGRUsage	ru0;
	char	   *nspname;
	bool		concurrent = chgcxt != NULL;
	LOCKMODE	lmode;

	lmode = RepackLockLevel(concurrent);

	pg_rusage_init(&ru0);

	/* Store a copy of the namespace name for logging purposes */
	nspname = get_namespace_name(RelationGetNamespace(OldHeap));

	/*
	 * Their tuple descriptors should be exactly alike, but here we only need
	 * assume that they have the same number of columns.
	 */
	oldTupDesc = RelationGetDescr(OldHeap);
	newTupDesc = RelationGetDescr(NewHeap);
	Assert(newTupDesc->natts == oldTupDesc->natts);

	/*
	 * If the OldHeap has a toast table, get lock on the toast table to keep
	 * it from being vacuumed.  This is needed because autovacuum processes
	 * toast tables independently of their main tables, with no lock on the
	 * latter.  If an autovacuum were to start on the toast table after we
	 * compute our OldestXmin below, it would use a later OldestXmin, and then
	 * possibly remove as DEAD toast tuples belonging to main tuples we think
	 * are only RECENTLY_DEAD.  Then we'd fail while trying to copy those
	 * tuples.
	 *
	 * We don't need to open the toast relation here, just lock it.  The lock
	 * will be held till end of transaction.
	 */
	if (OldHeap->rd_rel->reltoastrelid)
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, lmode);

	/*
	 * If both tables have TOAST tables, perform toast swap by content.  It is
	 * possible that the old table has a toast table but the new one doesn't,
	 * if toastable columns have been dropped.  In that case we have to do
	 * swap by links.  This is okay because swap by content is only essential
	 * for system catalogs, and we don't support schema changes for them.
	 */
	if (OldHeap->rd_rel->reltoastrelid && NewHeap->rd_rel->reltoastrelid &&
		!concurrent)
	{
		*pSwapToastByContent = true;

		/*
		 * When doing swap by content, any toast pointers written into NewHeap
		 * must use the old toast table's OID, because that's where the toast
		 * data will eventually be found.  Set this up by setting rd_toastoid.
		 * This also tells toast_save_datum() to preserve the toast value
		 * OIDs, which we want so as not to invalidate toast pointers in
		 * system catalog caches, and to avoid making multiple copies of a
		 * single toast value.
		 *
		 * Note that we must hold NewHeap open until we are done writing data,
		 * since the relcache will not guarantee to remember this setting once
		 * the relation is closed.  Also, this technique depends on the fact
		 * that no one will try to read from the NewHeap until after we've
		 * finished writing it and swapping the rels --- otherwise they could
		 * follow the toast pointers to the wrong place.  (It would actually
		 * work for values copied over from the old toast table, but not for
		 * any values that we toast which were previously not toasted.)
		 *
		 * This would not work with CONCURRENTLY because we may need to delete
		 * TOASTed tuples from the new heap. With this hack, we'd delete them
		 * from the old heap.
		 */
		NewHeap->rd_toastoid = OldHeap->rd_rel->reltoastrelid;
	}
	else
		*pSwapToastByContent = false;

	/*
	 * Compute xids used to freeze and weed out dead tuples and multixacts.
	 * Since we're going to rewrite the whole table anyway, there's no reason
	 * not to be aggressive about this.
	 */
	memset(&params, 0, sizeof(VacuumParams));
	vacuum_get_cutoffs(OldHeap, &params, &cutoffs);

	/*
	 * FreezeXid will become the table's new relfrozenxid, and that mustn't go
	 * backwards, so take the max.
	 */
	{
		TransactionId relfrozenxid = OldHeap->rd_rel->relfrozenxid;

		if (TransactionIdIsValid(relfrozenxid) &&
			TransactionIdPrecedes(cutoffs.FreezeLimit, relfrozenxid))
			cutoffs.FreezeLimit = relfrozenxid;
	}

	/*
	 * MultiXactCutoff, similarly, shouldn't go backwards either.
	 */
	{
		MultiXactId relminmxid = OldHeap->rd_rel->relminmxid;

		if (MultiXactIdIsValid(relminmxid) &&
			MultiXactIdPrecedes(cutoffs.MultiXactCutoff, relminmxid))
			cutoffs.MultiXactCutoff = relminmxid;
	}

	if (!concurrent)
	{
		/*
		 * Decide whether to use an indexscan or seqscan-and-optional-sort to
		 * scan the OldHeap.  If the index is a btree, ask the planner to
		 * choose via normal path cost comparison.
		 *
		 * The underlying tuplesort.c code doesn't support AMs other than
		 * btree, so we must always use a normal indexscan if a non-btree
		 * index is specified -- or an unsorted seqscan if no index is given.
		 */
		if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
			use_sort = plan_cluster_use_sort(RelationGetRelid(OldHeap),
											 RelationGetRelid(OldIndex));
	}
	else
	{
		/*
		 * To use multiple snapshots, we need to read the table sequentially.
		 */
		use_sort = true;
	}

	/* Log what we're doing */
	if (OldIndex != NULL && !use_sort)
		ereport(elevel,
				errmsg("repacking \"%s.%s\" using index scan on \"%s\"",
					   nspname,
					   RelationGetRelationName(OldHeap),
					   RelationGetRelationName(OldIndex)));
	else if (use_sort)
		ereport(elevel,
				errmsg("repacking \"%s.%s\" using sequential scan and sort",
					   nspname,
					   RelationGetRelationName(OldHeap)));
	else
		ereport(elevel,
				errmsg("repacking \"%s.%s\" in physical order",
					   nspname,
					   RelationGetRelationName(OldHeap)));

	/*
	 * Hand off the actual copying to AM specific function, the generic code
	 * cannot know how to deal with visibility across AMs. Note that this
	 * routine is allowed to set FreezeXid / MultiXactCutoff to different
	 * values (e.g. because the AM doesn't use freezing).
	 */
	table_relation_copy_for_cluster(OldHeap, NewHeap, OldIndex, use_sort,
									cutoffs.OldestXmin,
									&cutoffs.FreezeLimit,
									&cutoffs.MultiXactCutoff,
									&num_tuples, &tups_vacuumed,
									&tups_recently_dead, chgcxt);

	/* return selected values to caller, get set as relfrozenxid/minmxid */
	*pFreezeXid = cutoffs.FreezeLimit;
	*pCutoffMulti = cutoffs.MultiXactCutoff;

	/*
	 * Reset rd_toastoid just to be tidy --- it shouldn't be looked at again.
	 * In the CONCURRENTLY case, we need to set it again before applying the
	 * concurrent changes.
	 */
	NewHeap->rd_toastoid = InvalidOid;

	/* Log what we did */
	ereport(elevel,
			(errmsg("\"%s.%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					nspname,
					RelationGetRelationName(OldHeap),
					tups_vacuumed, num_tuples,
					RelationGetNumberOfBlocks(OldHeap)),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
					   "%s.",
					   tups_recently_dead,
					   pg_rusage_show(&ru0))));

	/*
	 * Update pg_class fields. In the CONCURRENTLY case we do it later because
	 * 1) the catalog update triggers XID assignment, 2) the work is split
	 * into several transactions, so the catalog update should take place in
	 * the last one.
	 */
	if (!concurrent)
	{
		BlockNumber num_pages;

		num_pages = RelationGetNumberOfBlocks(NewHeap);

		copy_table_data_update_stats(OldHeap, NewHeap, num_pages, num_tuples);
	}

	*p_num_tuples = num_tuples;
}

/*
 * Sub-routine of copy_table_data(), to update pg_class.
 */
static void
copy_table_data_update_stats(Relation OldHeap, Relation NewHeap,
							 BlockNumber num_pages, double num_tuples)
{
	Relation	relRelation;
	HeapTuple	reltup;
	Form_pg_class relform;

	/* Update pg_class to reflect the correct values of pages and tuples. */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID,
								 ObjectIdGetDatum(RelationGetRelid(NewHeap)));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(NewHeap));
	relform = (Form_pg_class) GETSTRUCT(reltup);

	relform->relpages = num_pages;
	relform->reltuples = num_tuples;

	/* Don't update the stats for pg_class.  See swap_relation_files. */
	if (RelationGetRelid(OldHeap) != RelationRelationId)
		CatalogTupleUpdate(relRelation, &reltup->t_self, reltup);
	else
		CacheInvalidateRelcacheByTuple(reltup);

	/* Clean up. */
	heap_freetuple(reltup);
	table_close(relRelation, RowExclusiveLock);

	/* Make the update visible */
	CommandCounterIncrement();
}

/*
 * Swap the physical files of two given relations.
 *
 * We swap the physical identity (reltablespace, relfilenumber) while keeping
 * the same logical identities of the two relations.  relpersistence is also
 * swapped, which is critical since it determines where buffers live for each
 * relation.
 *
 * We can swap associated TOAST data in either of two ways: recursively swap
 * the physical content of the toast tables (and their indexes), or swap the
 * TOAST links in the given relations' pg_class entries.  The former is needed
 * to manage rewrites of shared catalogs (where we cannot change the pg_class
 * links) while the latter is the only way to handle cases in which a toast
 * table is added or removed altogether.
 *
 * Lastly, if r2 and its toast table and toast index (if any) are mapped,
 * their OIDs are emitted into mapped_tables[].  This is hacky but beats
 * having to look the information up again later in finish_heap_swap.
 */
static void
swap_relation_files(Oid r1, Oid r2, bool target_is_pg_class,
					bool swap_toast_by_content,
					bool is_internal,
					Oid *mapped_tables, Oid *r1_toastid)
{
	Relation	relRelation;
	HeapTuple	reltup1,
				reltup2;
	Form_pg_class relform1,
				relform2;
	RelFileNumber relfilenumber1,
				relfilenumber2;
	RelFileNumber swaptemp;
	char		swptmpchr;
	Oid			relam1,
				relam2;

	/* We need writable copies of both pg_class tuples. */
	relRelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup1 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r1));
	if (!HeapTupleIsValid(reltup1))
		elog(ERROR, "cache lookup failed for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r2));
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "cache lookup failed for relation %u", r2);
	relform2 = (Form_pg_class) GETSTRUCT(reltup2);

	relfilenumber1 = relform1->relfilenode;
	relfilenumber2 = relform2->relfilenode;
	relam1 = relform1->relam;
	relam2 = relform2->relam;

	if (r1_toastid)
		*r1_toastid = relform1->reltoastrelid;

	if (RelFileNumberIsValid(relfilenumber1) &&
		RelFileNumberIsValid(relfilenumber2))
	{
		/*
		 * Normal non-mapped relations: swap relfilenumbers, reltablespaces,
		 * relpersistence
		 */
		Assert(!target_is_pg_class);

		swaptemp = relform1->relfilenode;
		relform1->relfilenode = relform2->relfilenode;
		relform2->relfilenode = swaptemp;

		swaptemp = relform1->reltablespace;
		relform1->reltablespace = relform2->reltablespace;
		relform2->reltablespace = swaptemp;

		swaptemp = relform1->relam;
		relform1->relam = relform2->relam;
		relform2->relam = swaptemp;

		swptmpchr = relform1->relpersistence;
		relform1->relpersistence = relform2->relpersistence;
		relform2->relpersistence = swptmpchr;

		/* Also swap toast links, if we're swapping by links */
		if (!swap_toast_by_content)
		{
			swaptemp = relform1->reltoastrelid;
			relform1->reltoastrelid = relform2->reltoastrelid;
			relform2->reltoastrelid = swaptemp;
		}
	}
	else
	{
		/*
		 * Mapped-relation case.  Here we have to swap the relation mappings
		 * instead of modifying the pg_class columns.  Both must be mapped.
		 */
		if (RelFileNumberIsValid(relfilenumber1) ||
			RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "cannot swap mapped relation \"%s\" with non-mapped relation",
				 NameStr(relform1->relname));

		/*
		 * We can't change the tablespace nor persistence of a mapped rel, and
		 * we can't handle toast link swapping for one either, because we must
		 * not apply any critical changes to its pg_class row.  These cases
		 * should be prevented by upstream permissions tests, so these checks
		 * are non-user-facing emergency backstop.
		 */
		if (relform1->reltablespace != relform2->reltablespace)
			elog(ERROR, "cannot change tablespace of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relpersistence != relform2->relpersistence)
			elog(ERROR, "cannot change persistence of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relam != relform2->relam)
			elog(ERROR, "cannot change access method of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (!swap_toast_by_content &&
			(relform1->reltoastrelid || relform2->reltoastrelid))
			elog(ERROR, "cannot swap toast by links for mapped relation \"%s\"",
				 NameStr(relform1->relname));

		/*
		 * Fetch the mappings --- shouldn't fail, but be paranoid
		 */
		relfilenumber1 = RelationMapOidToFilenumber(r1, relform1->relisshared);
		if (!RelFileNumberIsValid(relfilenumber1))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform1->relname), r1);
		relfilenumber2 = RelationMapOidToFilenumber(r2, relform2->relisshared);
		if (!RelFileNumberIsValid(relfilenumber2))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform2->relname), r2);

		/*
		 * Send replacement mappings to relmapper.  Note these won't actually
		 * take effect until CommandCounterIncrement.
		 */
		RelationMapUpdateMap(r1, relfilenumber2, relform1->relisshared, false);
		RelationMapUpdateMap(r2, relfilenumber1, relform2->relisshared, false);

		/* Pass OIDs of mapped r2 tables back to caller */
		*mapped_tables++ = r2;
	}

	/*
	 * Recognize that rel1's relfilenumber (swapped from rel2) is new in this
	 * subtransaction. The rel2 storage (swapped from rel1) may or may not be
	 * new.
	 */
	{
		Relation	rel1,
					rel2;

		rel1 = relation_open(r1, NoLock);
		rel2 = relation_open(r2, NoLock);
		rel2->rd_createSubid = rel1->rd_createSubid;
		rel2->rd_newRelfilelocatorSubid = rel1->rd_newRelfilelocatorSubid;
		rel2->rd_firstRelfilelocatorSubid = rel1->rd_firstRelfilelocatorSubid;
		RelationAssumeNewRelfilelocator(rel1);
		relation_close(rel1, NoLock);
		relation_close(rel2, NoLock);
	}

	/*
	 * In the case of a shared catalog, these next few steps will only affect
	 * our own database's pg_class row; but that's okay, because they are all
	 * noncritical updates.  That's also an important fact for the case of a
	 * mapped catalog, because it's possible that we'll commit the map change
	 * and then fail to commit the pg_class update.
	 */

	/* swap size statistics too, since new rel has freshly-updated stats */
	{
		int32		swap_pages;
		float4		swap_tuples;
		int32		swap_allvisible;
		int32		swap_allfrozen;

		swap_pages = relform1->relpages;
		relform1->relpages = relform2->relpages;
		relform2->relpages = swap_pages;

		swap_tuples = relform1->reltuples;
		relform1->reltuples = relform2->reltuples;
		relform2->reltuples = swap_tuples;

		swap_allvisible = relform1->relallvisible;
		relform1->relallvisible = relform2->relallvisible;
		relform2->relallvisible = swap_allvisible;

		swap_allfrozen = relform1->relallfrozen;
		relform1->relallfrozen = relform2->relallfrozen;
		relform2->relallfrozen = swap_allfrozen;
	}

	/*
	 * Update the tuples in pg_class --- unless the target relation of the
	 * swap is pg_class itself.  In that case, there is zero point in making
	 * changes because we'd be updating the old data that we're about to throw
	 * away.  Because the real work being done here for a mapped relation is
	 * just to change the relation map settings, it's all right to not update
	 * the pg_class rows in this case. The most important changes will instead
	 * performed later, in finish_heap_swap() itself.
	 */
	if (!target_is_pg_class)
	{
		CatalogIndexState indstate;

		indstate = CatalogOpenIndexes(relRelation);
		CatalogTupleUpdateWithInfo(relRelation, &reltup1->t_self, reltup1,
								   indstate);
		CatalogTupleUpdateWithInfo(relRelation, &reltup2->t_self, reltup2,
								   indstate);
		CatalogCloseIndexes(indstate);
	}
	else
	{
		/* no update ... but we do still need relcache inval */
		CacheInvalidateRelcacheByTuple(reltup1);
		CacheInvalidateRelcacheByTuple(reltup2);
	}

	/*
	 * Now that pg_class has been updated with its relevant information for
	 * the swap, update the dependency of the relations to point to their new
	 * table AM, if it has changed.
	 */
	if (relam1 != relam2)
	{
		if (changeDependencyFor(RelationRelationId,
								r1,
								AccessMethodRelationId,
								relam1,
								relam2) != 1)
			elog(ERROR, "could not change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r1)),
				 get_rel_name(r1));
		if (changeDependencyFor(RelationRelationId,
								r2,
								AccessMethodRelationId,
								relam2,
								relam1) != 1)
			elog(ERROR, "could not change access method dependency for relation \"%s.%s\"",
				 get_namespace_name(get_rel_namespace(r2)),
				 get_rel_name(r2));
	}

	/*
	 * Post alter hook for modified relations. The change to r2 is always
	 * internal, but r1 depends on the invocation context.
	 */
	InvokeObjectPostAlterHookArg(RelationRelationId, r1, 0,
								 InvalidOid, is_internal);
	InvokeObjectPostAlterHookArg(RelationRelationId, r2, 0,
								 InvalidOid, true);

	/*
	 * If we have toast tables associated with the relations being swapped,
	 * deal with them too.
	 */
	if (relform1->reltoastrelid || relform2->reltoastrelid)
	{
		if (swap_toast_by_content)
		{
			if (relform1->reltoastrelid && relform2->reltoastrelid)
			{
				/* Recursively swap the contents of the toast tables */
				swap_relation_files(relform1->reltoastrelid,
									relform2->reltoastrelid,
									target_is_pg_class,
									swap_toast_by_content,
									is_internal,
									mapped_tables,
									NULL);
			}
			else
			{
				/* caller messed up */
				elog(ERROR, "cannot swap toast files by content when there's only one");
			}
		}
		else
		{
			/*
			 * We swapped the ownership links, so we need to change dependency
			 * data to match.
			 *
			 * NOTE: it is possible that only one table has a toast table.
			 *
			 * NOTE: at present, a TOAST table's only dependency is the one on
			 * its owning table.  If more are ever created, we'd need to use
			 * something more selective than deleteDependencyRecordsFor() to
			 * get rid of just the link we want.
			 */
			ObjectAddress baseobject,
						toastobject;
			long		count;

			/*
			 * We disallow this case for system catalogs, to avoid the
			 * possibility that the catalog we're rebuilding is one of the
			 * ones the dependency changes would change.  It's too late to be
			 * making any data changes to the target catalog.
			 */
			if (IsSystemClass(r1, relform1))
				elog(ERROR, "cannot swap toast files by links for system catalogs");

			/* Delete old dependencies */
			if (relform1->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform1->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}
			if (relform2->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform2->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}

			/* Register new dependencies */
			baseobject.classId = RelationRelationId;
			baseobject.objectSubId = 0;
			toastobject.classId = RelationRelationId;
			toastobject.objectSubId = 0;

			if (relform1->reltoastrelid)
			{
				baseobject.objectId = r1;
				toastobject.objectId = relform1->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}

			if (relform2->reltoastrelid)
			{
				baseobject.objectId = r2;
				toastobject.objectId = relform2->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}
		}
	}

	/*
	 * If we're swapping two toast tables by content, do the same for their
	 * valid index. The swap can actually be safely done only if the relations
	 * have indexes.
	 */
	if (swap_toast_by_content &&
		relform1->relkind == RELKIND_TOASTVALUE &&
		relform2->relkind == RELKIND_TOASTVALUE)
	{
		Oid			toastIndex1,
					toastIndex2;

		/* Get valid index for each relation */
		toastIndex1 = toast_get_valid_index(r1,
											AccessExclusiveLock);
		toastIndex2 = toast_get_valid_index(r2,
											AccessExclusiveLock);

		swap_relation_files(toastIndex1,
							toastIndex2,
							target_is_pg_class,
							swap_toast_by_content,
							is_internal,
							mapped_tables,
							NULL);
	}

	/* Clean up. */
	heap_freetuple(reltup1);
	heap_freetuple(reltup2);

	table_close(relRelation, RowExclusiveLock);
}

/*
 * Update relfrozenxid and relminmxid attributes of pg_class entry, specified
 * by OID.
 */
static void
update_relation_cutoffs(Oid relid, TransactionId frozenXid,
						MultiXactId cutoffMulti)
{
	Relation	relRelation;
	HeapTuple	reltup;
	Form_pg_class relform;
	CatalogIndexState indstate;

	relRelation = table_open(RelationRelationId, RowExclusiveLock);
	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relform = (Form_pg_class) GETSTRUCT(reltup);
	relform->relfrozenxid = frozenXid;
	relform->relminmxid = cutoffMulti;

	indstate = CatalogOpenIndexes(relRelation);
	CatalogTupleUpdateWithInfo(relRelation, &reltup->t_self, reltup,
							   indstate);
	CatalogCloseIndexes(indstate);

	heap_freetuple(reltup);
	table_close(relRelation, RowExclusiveLock);
}

/*
 * Remove the transient table that was built by make_new_heap, and finish
 * cleaning up (including rebuilding all indexes on the old heap).
 *
 * 'update_toast_cutoffs' tells whether relfrozenxid and relminmxid of the
 * TOAST relation should be updated too.
 */
void
finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
				 bool is_system_catalog,
				 bool swap_toast_by_content,
				 bool check_constraints,
				 bool is_internal,
				 bool reindex,
				 TransactionId frozenXid,
				 MultiXactId cutoffMulti,
				 bool update_toast_cutoffs,
				 char newrelpersistence)
{
	ObjectAddress object;
	Oid			mapped_tables[4];
	Oid			oid_old_toastid;
	int			i;

	/*
	 * In the swap-toast-by-content case, we always need to update the
	 * cutoffs. In the swap-toast-links case, we usually assume we don't need
	 * to change the toast table's relfrozenxid: the new version of the toast
	 * table should already have relfrozenxid set to RecentXmin, which is good
	 * enough. However, there's a special case - REPACK (CONCURRENTLY) - which
	 * still needs to update the cutoffs - see the related call for more info.
	 */
	Assert((swap_toast_by_content && update_toast_cutoffs) ||
		   !swap_toast_by_content);

	/* Report that we are now swapping relation files */
	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_SWAP_REL_FILES);

	/* Zero out possible results from swapped_relation_files */
	memset(mapped_tables, 0, sizeof(mapped_tables));

	/*
	 * Swap the contents of the heap relations (including any toast tables).
	 * Also set old heap's relfrozenxid to frozenXid.
	 */
	swap_relation_files(OIDOldHeap, OIDNewHeap,
						(OIDOldHeap == RelationRelationId),
						swap_toast_by_content, is_internal,
						mapped_tables, &oid_old_toastid);

	/*
	 * If it's a system catalog, queue a sinval message to flush all catcaches
	 * on the catalog when we reach CommandCounterIncrement.
	 */
	if (is_system_catalog)
		CacheInvalidateCatalog(OIDOldHeap);

	if (reindex)
	{
		int			reindex_flags;
		ReindexParams reindex_params = {0};

		/*
		 * Rebuild each index on the relation (but not the toast table, which
		 * is all-new at this point).  It is important to do this before the
		 * DROP step because if we are processing a system catalog that will
		 * be used during DROP, we want to have its indexes available.  There
		 * is no advantage to the other order anyway because this is all
		 * transactional, so no chance to reclaim disk space before commit. We
		 * do not need a final CommandCounterIncrement() because
		 * reindex_relation does it.
		 *
		 * Note: because index_build is called via reindex_relation, it will
		 * never set indcheckxmin true for the indexes.  This is OK even
		 * though in some sense we are building new indexes rather than
		 * rebuilding existing ones, because the new heap won't contain any
		 * HOT chains at all, let alone broken ones, so it can't be necessary
		 * to set indcheckxmin.
		 */
		reindex_flags = REINDEX_REL_SUPPRESS_INDEX_USE;
		if (check_constraints)
			reindex_flags |= REINDEX_REL_CHECK_CONSTRAINTS;

		/*
		 * Ensure that the indexes have the same persistence as the parent
		 * relation.
		 */
		if (newrelpersistence == RELPERSISTENCE_UNLOGGED)
			reindex_flags |= REINDEX_REL_FORCE_INDEXES_UNLOGGED;
		else if (newrelpersistence == RELPERSISTENCE_PERMANENT)
			reindex_flags |= REINDEX_REL_FORCE_INDEXES_PERMANENT;

		/* Report that we are now reindexing relations */
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_REBUILD_INDEX);

		reindex_relation(NULL, OIDOldHeap, reindex_flags, &reindex_params);
	}

	/* Report that we are now doing clean up */
	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_FINAL_CLEANUP);

	/*
	 * Update relfrozenxid and relminmxid. CCI is needed to see the most
	 * recent pg_class tuple, created by swap_relation_files(). However that
	 * CCI makes us use the new relation file. Therefore we cannot do this
	 * before indexes have been rebuilt too.
	 */
	CommandCounterIncrement();
	update_relation_cutoffs(OIDOldHeap, frozenXid, cutoffMulti);
	/* The same for TOAST, if requested. */
	if (OidIsValid(oid_old_toastid) && update_toast_cutoffs)
		update_relation_cutoffs(oid_old_toastid, frozenXid, cutoffMulti);

	/* Destroy new heap with old filenumber */
	object.classId = RelationRelationId;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	if (!reindex)
	{
		/*
		 * Make sure the changes in pg_class are visible. This is especially
		 * important if !swap_toast_by_content, so that the correct TOAST
		 * relation is dropped. (reindex_relation() above did not help in this
		 * case))
		 */
		CommandCounterIncrement();
	}

	/*
	 * The new relation is local to our transaction and we know nothing
	 * depends on it, so DROP_RESTRICT should be OK.
	 */
	performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* performDeletion does CommandCounterIncrement at end */

	/*
	 * Now we must remove any relation mapping entries that we set up for the
	 * transient table, as well as its toast table and toast index if any. If
	 * we fail to do this before commit, the relmapper will complain about new
	 * permanent map entries being added post-bootstrap.
	 */
	for (i = 0; OidIsValid(mapped_tables[i]); i++)
		RelationMapRemoveMapping(mapped_tables[i]);

	/*
	 * At this point, everything is kosher except that, if we did toast swap
	 * by links, the toast table's name corresponds to the transient table.
	 * The name is irrelevant to the backend because it's referenced by OID,
	 * but users looking at the catalogs could be confused.  Rename it to
	 * prevent this problem.
	 *
	 * Note no lock required on the relation, because we already hold an
	 * exclusive lock on it.
	 */
	if (!swap_toast_by_content)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		if (OidIsValid(newrel->rd_rel->reltoastrelid))
		{
			Oid			toastidx;
			char		NewToastName[NAMEDATALEN];

			/* Get the associated valid index to be renamed */
			toastidx = toast_get_valid_index(newrel->rd_rel->reltoastrelid,
											 AccessExclusiveLock);

			/* rename the toast table ... */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u",
					 OIDOldHeap);
			RenameRelationInternal(newrel->rd_rel->reltoastrelid,
								   NewToastName, true, false);

			/* ... and its valid index too. */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u_index",
					 OIDOldHeap);

			RenameRelationInternal(toastidx,
								   NewToastName, true, true);

			/*
			 * Reset the relrewrite for the toast. The command-counter
			 * increment is required here as we are about to update the tuple
			 * that is updated as part of RenameRelationInternal.
			 */
			CommandCounterIncrement();
			ResetRelRewrite(newrel->rd_rel->reltoastrelid);
		}
		relation_close(newrel, NoLock);
	}

	/* if it's not a catalog table, clear any missing attribute settings */
	if (!is_system_catalog)
	{
		Relation	newrel;

		newrel = table_open(OIDOldHeap, NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
	}
}

/*
 * Determine which relations to process, when REPACK/CLUSTER is called
 * without specifying a table name.  The exact process depends on whether
 * USING INDEX was given or not, and in any case we only return tables and
 * materialized views that the current user has privileges to repack/cluster.
 *
 * If USING INDEX was given, we scan pg_index to find those that have
 * indisclustered set; if it was not given, scan pg_class and return all
 * tables.
 *
 * Return it as a list of RelToCluster in the given memory context.
 */
static List *
get_tables_to_repack(RepackCommand cmd, bool usingindex, MemoryContext permcxt)
{
	Relation	catalog;
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *rtcs = NIL;

	if (usingindex)
	{
		ScanKeyData entry;

		/*
		 * For USING INDEX, scan pg_index to find those with indisclustered.
		 *
		 * Note we don't obtain lock of any kind on the index, which means the
		 * index or its owning table could be gone or change at any point.  We
		 * have to be extra careful when examining catalog state for them.
		 */
		catalog = table_open(IndexRelationId, AccessShareLock);
		ScanKeyInit(&entry,
					Anum_pg_index_indisclustered,
					BTEqualStrategyNumber, F_BOOLEQ,
					BoolGetDatum(true));
		scan = table_beginscan_catalog(catalog, 1, &entry);
		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			RelToCluster *rtc;
			Form_pg_index index;
			HeapTuple	classtup;
			Oid			relnamespace;
			char		relpersistence;
			MemoryContext oldcxt;

			index = (Form_pg_index) GETSTRUCT(tuple);

			classtup = SearchSysCache1(RELOID, ObjectIdGetDatum(index->indrelid));
			if (!HeapTupleIsValid(classtup))
				continue;
			relnamespace = ((Form_pg_class) GETSTRUCT(classtup))->relnamespace;
			relpersistence = ((Form_pg_class) GETSTRUCT(classtup))->relpersistence;
			ReleaseSysCache(classtup);

			/* Skip temp relations belonging to other sessions */
			if (relpersistence == RELPERSISTENCE_TEMP &&
				!isTempOrTempToastNamespace(relnamespace))
				continue;

			/* noisily skip rels which the user can't process */
			if (!repack_is_permitted_for_relation(cmd, index->indrelid,
												  GetUserId(), false))
				continue;

			/* Use a permanent memory context for the result list */
			oldcxt = MemoryContextSwitchTo(permcxt);
			rtc = palloc_object(RelToCluster);
			rtc->tableOid = index->indrelid;
			rtc->indexOid = index->indexrelid;
			rtcs = lappend(rtcs, rtc);
			MemoryContextSwitchTo(oldcxt);
		}
	}
	else
	{
		catalog = table_open(RelationRelationId, AccessShareLock);
		scan = table_beginscan_catalog(catalog, 0, NULL);

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			RelToCluster *rtc;
			Form_pg_class class;
			MemoryContext oldcxt;

			class = (Form_pg_class) GETSTRUCT(tuple);

			/* Can only process plain tables and matviews */
			if (class->relkind != RELKIND_RELATION &&
				class->relkind != RELKIND_MATVIEW)
				continue;

			/* Skip temp relations belonging to other sessions */
			if (class->relpersistence == RELPERSISTENCE_TEMP &&
				!isTempOrTempToastNamespace(class->relnamespace))
				continue;

			/* noisily skip rels which the user can't process */
			if (!repack_is_permitted_for_relation(cmd, class->oid,
												  GetUserId(), false))
				continue;

			/* Use a permanent memory context for the result list */
			oldcxt = MemoryContextSwitchTo(permcxt);
			rtc = palloc_object(RelToCluster);
			rtc->tableOid = class->oid;
			rtc->indexOid = InvalidOid;
			rtcs = lappend(rtcs, rtc);
			MemoryContextSwitchTo(oldcxt);
		}
	}

	table_endscan(scan);
	table_close(catalog, AccessShareLock);

	return rtcs;
}

/*
 * Determine relations to process, when REPACK/CLUSTER is called with a
 * partitioning table; that is, a list of its leaf partitions.  That table has
 * already been opened by caller and is passed as 'rel'.  It is closed and
 * unlocked here before return, so caller should clobber its pointer to avoid
 * confusion.
 *
 * Return it as a list of RelToCluster.
 *
 * XXX we don't support CONCURRENTLY for partitioned tables yet.
 */
static List *
get_tables_to_repack_partitioned(RepackStmt *stmt, Relation rel,
								 MemoryContext permcxt)
{
	Oid			relid;
	bool		rel_is_index;
	List	   *inhoids;
	List	   *rtcs = NIL;

	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	Assert(CheckRelationLockedByMe(rel, AccessExclusiveLock, false));

	/*
	 * We find the list of tables by looking for inheritors.  If USING INDEX
	 * was given, look for inheritors of that index, whose name we resolve
	 * now.
	 *
	 * Otherwise we look for inheritors of the table itself.
	 */
	if (stmt->usingindex)
	{
		/*
		 * If no index name was specified when repacking a partitioned table,
		 * punt for now.  Maybe we can improve this later.
		 */
		if (!stmt->indexname)
		{
			if (stmt->command == REPACK_COMMAND_CLUSTER)
				ereport(ERROR,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("there is no previously clustered index for table \"%s\"",
							   RelationGetRelationName(rel)));
			else
				ereport(ERROR,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				/*- translator: first %s is name of a SQL command, eg. REPACK */
						errmsg("cannot execute %s on partitioned table \"%s\" USING INDEX with no index name",
							   RepackCommandAsString(stmt->command),
							   RelationGetRelationName(rel)));
		}

		relid = determine_clustered_index(rel, stmt->usingindex,
										  stmt->indexname);
		if (!OidIsValid(relid))
			elog(ERROR, "unable to determine index to cluster on");
		check_index_is_clusterable(rel, relid, AccessExclusiveLock);

		rel_is_index = true;
	}
	else
	{
		relid = RelationGetRelid(rel);
		rel_is_index = false;
	}

	/*
	 * Do not lock the children until they're processed.  Note that we do hold
	 * a lock on the parent partitioned table.
	 */
	inhoids = find_all_inheritors(relid, NoLock, NULL);
	foreach_oid(child_oid, inhoids)
	{
		Oid			table_oid,
					index_oid;
		RelToCluster *rtc;
		MemoryContext oldcxt;

		if (rel_is_index)
		{
			/* consider only leaf indexes */
			if (get_rel_relkind(child_oid) != RELKIND_INDEX)
				continue;

			/*
			 * Although we do have a lock on some ancestor partitioned index,
			 * we may not have one on the immediate parent, so this lookup may
			 * still return invalid.
			 */
			table_oid = IndexGetRelation(child_oid, true);
			if (!OidIsValid(table_oid))
				continue;
			index_oid = child_oid;
		}
		else
		{
			/* consider only leaf relations */
			if (get_rel_relkind(child_oid) != RELKIND_RELATION)
				continue;

			table_oid = child_oid;
			index_oid = InvalidOid;
		}

		/*
		 * It's possible that the user does not have privileges to CLUSTER the
		 * leaf partition despite having them on the partitioned table.  Skip
		 * if so.
		 */
		if (!repack_is_permitted_for_relation(stmt->command, table_oid,
											  GetUserId(), false))
			continue;

		/* Use a permanent memory context for the result list */
		oldcxt = MemoryContextSwitchTo(permcxt);
		rtc = palloc_object(RelToCluster);
		rtc->tableOid = table_oid;
		rtc->indexOid = index_oid;
		rtcs = lappend(rtcs, rtc);
		MemoryContextSwitchTo(oldcxt);
	}

	/* close parent relation, releasing lock on it */
	table_close(rel, AccessExclusiveLock);

	return rtcs;
}


/*
 * Return whether userid has privileges to execute REPACK/CLUSTER on relid.
 *
 * The relation may already be locked by caller, in which case it cannot
 * possibly go missing; otherwise it may have been removed recently.  If
 * it's been removed, silently return false.  If the relation exists but
 * the user doesn't have the required privs, emit a WARNING and return false.
 *
 * Otherwise the relation exists and user has required perms, so return true.
 */
static bool
repack_is_permitted_for_relation(RepackCommand cmd, Oid relid, Oid userid,
								 bool already_locked)
{
	bool		is_missing = false;
	AclResult	result;
	char	   *relname;

	Assert(cmd == REPACK_COMMAND_CLUSTER || cmd == REPACK_COMMAND_REPACK);
	Assert(!already_locked ||
		   CheckRelationOidLockedByMe(relid, AccessShareLock, true));

	result = pg_class_aclcheck_ext(relid, userid, ACL_MAINTAIN, &is_missing);

	/*
	 * If the relation was concurrently dropped, nothing to do.  This is only
	 * reachable when the caller doesn't already have a lock on the relation.
	 */
	if (is_missing)
	{
		Assert(!already_locked);
		return false;
	}

	if (result == ACLCHECK_OK)
		return true;

	/*
	 * The relation can also be dropped after we tested its ACL and before we
	 * read its relname, so be careful here.
	 */
	relname = get_rel_name(relid);
	if (relname != NULL)
	{
		ereport(WARNING,
				errmsg("permission denied to execute %s on \"%s\", skipping it",
					   RepackCommandAsString(cmd), relname));
		pfree(relname);
	}

	return false;
}


/*
 * Given a RepackStmt with an indicated relation name, resolve the relation
 * name, obtain lock on it, then determine what to do based on the relation
 * type: if it's table and not partitioned, repack it as indicated (using an
 * existing clustered index, or following the given one), and return NULL.
 *
 * On the other hand, if the table is partitioned, do nothing further and
 * instead return the opened and locked relcache entry, so that caller can
 * process the partitions using the multiple-table handling code.  In this
 * case, if an index name is given, it's up to the caller to resolve it.
 *
 * A new transaction is started in either case.
 */
static Relation
process_single_relation(RepackStmt *stmt, LOCKMODE lockmode, bool isTopLevel,
						ClusterParams *params)
{
	Relation	rel;
	Oid			tableOid;

	Assert(stmt->relation != NULL);
	Assert(stmt->command == REPACK_COMMAND_CLUSTER ||
		   stmt->command == REPACK_COMMAND_REPACK);

	/*
	 * Make sure ANALYZE is specified if a column list is present.
	 */
	if ((params->options & CLUOPT_ANALYZE) == 0 && stmt->relation->va_cols != NIL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("ANALYZE option must be specified when a column list is provided"));

	/* Find, lock, and check permissions on the table. */
	tableOid = RangeVarGetRelidExtended(stmt->relation->relation,
										lockmode,
										0,
										RangeVarCallbackMaintainsTable,
										NULL);
	rel = table_open(tableOid, NoLock);

	/*
	 * Reject clustering a remote temp table ... their local buffer manager is
	 * not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: first %s is name of a SQL command, eg. REPACK */
				errmsg("cannot execute %s on temporary tables of other sessions",
					   RepackCommandAsString(stmt->command)));

	/*
	 * For partitioned tables, let caller handle this.  Otherwise, process it
	 * here and we're done.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return rel;
	else
	{
		Oid			indexOid = InvalidOid;

		indexOid = determine_clustered_index(rel, stmt->usingindex,
											 stmt->indexname);
		if (OidIsValid(indexOid))
			check_index_is_clusterable(rel, indexOid, lockmode);

		cluster_rel(stmt->command, rel, indexOid, params, isTopLevel);

		/*
		 * Do an analyze, if requested.  We close the transaction and start a
		 * new one, so that we don't hold the stronger lock for longer than
		 * needed.
		 */
		if (params->options & CLUOPT_ANALYZE)
		{
			VacuumParams vac_params = {0};

			PopActiveSnapshot();
			CommitTransactionCommand();

			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());

			vac_params.options |= VACOPT_ANALYZE;
			if (params->options & CLUOPT_VERBOSE)
				vac_params.options |= VACOPT_VERBOSE;
			analyze_rel(tableOid, NULL, &vac_params,
						stmt->relation->va_cols, true, NULL);
			PopActiveSnapshot();
			CommandCounterIncrement();
		}

		return NULL;
	}
}

/*
 * Given a relation and the usingindex/indexname options in a
 * REPACK USING INDEX or CLUSTER command, return the OID of the
 * index to use for clustering the table.
 *
 * Caller must hold lock on the relation so that the set of indexes
 * doesn't change, and must call check_index_is_clusterable.
 */
static Oid
determine_clustered_index(Relation rel, bool usingindex, const char *indexname)
{
	Oid			indexOid;

	if (indexname == NULL && usingindex)
	{
		/*
		 * If USING INDEX with no name is given, find a clustered index, or
		 * error out if none.
		 */
		indexOid = InvalidOid;
		foreach_oid(idxoid, RelationGetIndexList(rel))
		{
			if (get_index_isclustered(idxoid))
			{
				indexOid = idxoid;
				break;
			}
		}

		if (!OidIsValid(indexOid))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("there is no previously clustered index for table \"%s\"",
						   RelationGetRelationName(rel)));
	}
	else if (indexname != NULL)
	{
		/* An index was specified; obtain its OID. */
		indexOid = get_relname_relid(indexname, rel->rd_rel->relnamespace);
		if (!OidIsValid(indexOid))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("index \"%s\" for table \"%s\" does not exist",
						   indexname, RelationGetRelationName(rel)));
	}
	else
		indexOid = InvalidOid;

	return indexOid;
}

static const char *
RepackCommandAsString(RepackCommand cmd)
{
	switch (cmd)
	{
		case REPACK_COMMAND_REPACK:
			return "REPACK";
		case REPACK_COMMAND_VACUUMFULL:
			return "VACUUM";
		case REPACK_COMMAND_CLUSTER:
			return "CLUSTER";
	}
	return "???";				/* keep compiler quiet */
}

/*
 * Apply data changes that affect pages in given range.
 */
static void
apply_concurrent_changes(ChangeContext *chgcxt, BlockNumber range_start,
						 BlockNumber range_end)
{
	ConcurrentChangeKind kind = '\0';
	RepackDest *dest;
	Relation	rel;
	TupleTableSlot *spilled_tuple;
	TupleTableSlot *old_update_tuple;
	TupleTableSlot *ondisk_tuple;
	TupleTableSlot *new_tuple;
	bool		have_old_tuple = false;
	bool		check_range;
	MemoryContext oldcxt;
	DecodingWorkerShared *shared;
	char		fname[MAXPGPATH];
	BufFile    *file;
	SnapshotData SnapshotNewHeap;

	/*
	 * Use the auxiliary table if one exists, otherwise the "final"
	 * destination table.
	 */
	dest = chgcxt->cc_dest_aux ? chgcxt->cc_dest_aux : &chgcxt->cc_dest;
	rel = dest->rel;

	/*
	 * Range needs to be checked if the bounds are specified. Expect either
	 * both or none.
	 */
	Assert(BlockNumberIsValid(range_start) == BlockNumberIsValid(range_end));
	check_range = BlockNumberIsValid(range_start);

	shared = (DecodingWorkerShared *) dsm_segment_address(decoding_worker->seg);

	/* Open the file containing the changes. */
	DecodingWorkerFileName(fname, shared->relid, chgcxt->cc_file_seq_changes,
						   false);
	file = BufFileOpenFileSet(&shared->sfs.fs, fname, O_RDONLY, false);

	spilled_tuple = MakeSingleTupleTableSlot(RelationGetDescr(rel),
											 &TTSOpsVirtual);
	ondisk_tuple = MakeSingleTupleTableSlot(RelationGetDescr(rel),
											table_slot_callbacks(rel));
	old_update_tuple = MakeSingleTupleTableSlot(RelationGetDescr(rel),
												&TTSOpsVirtual);
	new_tuple = MakeSingleTupleTableSlot(RelationGetDescr(rel),
										 &TTSOpsHeapTuple);

	oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(dest->estate));

	/*
	 * Finding tuples to UPDATE / DELETE is exactly the purpose of
	 * SNAPSHOT_NEW_HEAP.
	 */
	InitNewHeapSnapshot(SnapshotNewHeap);
	PushActiveSnapshot(&SnapshotNewHeap);

	while (true)
	{
		size_t		nread;
		BlockNumber block,
					old_block;
		BlockNumber *old_block_p;
		TransactionId xid;

		CHECK_FOR_INTERRUPTS();

		nread = BufFileReadMaybeEOF(file, &kind, 1, true);
		if (nread == 0)			/* done with the file? */
			break;

		/*
		 * If this is the old tuple for an update, read it into the tuple slot
		 * and go to the next one.  The update itself will be executed on the
		 * next iteration, when we receive the NEW tuple.
		 */
		if (kind == CHANGE_UPDATE_OLD)
		{
			restore_tuple(file, rel, old_update_tuple, NULL, NULL, NULL);
			have_old_tuple = true;
			continue;
		}

		/*
		 * Now restore the tuple into the slot and execute the change.
		 *
		 * old_block is only stored with UPDATE_NEW.
		 */
		old_block_p = kind == CHANGE_UPDATE_NEW ? &old_block : NULL;
		restore_tuple(file, rel, spilled_tuple, &block, old_block_p, &xid);

		if (kind == CHANGE_INSERT)
		{
			/*
			 * Only insert the tuple if it fits into the current range (or if
			 * range does not matter).
			 */
			if (!check_range ||
				is_block_in_range(block, range_start, range_end))
				apply_concurrent_insert(dest, spilled_tuple, new_tuple, xid);
		}
		else if (kind == CHANGE_DELETE)
		{
			/*
			 * Only delete the tuple if it fits into the current range (or if
			 * range does not matter).
			 */
			if (!check_range ||
				is_block_in_range(block, range_start, range_end))
			{
				bool		found;

				/* Find the tuple to be deleted */
				found = find_target_tuple(dest, spilled_tuple, ondisk_tuple);
				if (!found)
					elog(ERROR, "could not find target tuple");
				apply_concurrent_delete(rel, ondisk_tuple, xid);
			}
		}
		else if (kind == CHANGE_UPDATE_NEW)
		{
			TupleTableSlot *key;
			bool		found;

			if (have_old_tuple)
				key = old_update_tuple;
			else
				key = spilled_tuple;

			/*
			 * Perform normal update if both old and new version are in the
			 * current range.
			 */
			if (!check_range ||
				(is_block_in_range(old_block, range_start, range_end) &&
				 is_block_in_range(block, range_start, range_end)))
			{
				/* Find the tuple to be updated or deleted. */
				found = find_target_tuple(dest, key, ondisk_tuple);
				if (!found)
					elog(ERROR, "could not find target tuple");

				/*
				 * If 'spilled_tuple' contains TOAST pointers, they point to
				 * the old relation's toast. Copy the corresponding TOAST
				 * pointers for the new relation from the existing tuple. (The
				 * fact that we received a TOAST pointer here implies that the
				 * attribute hasn't changed.)
				 */
				adjust_toast_pointers(rel, spilled_tuple, ondisk_tuple);

				apply_concurrent_update(dest, spilled_tuple, ondisk_tuple,
										new_tuple, xid);
			}
			else
			{
				Assert(check_range);

				if (is_block_in_range(block, range_start, range_end))
				{
					/*
					 * The old key is in another range, so only insert the new
					 * one into the current range. The old version should not
					 * be visible to the snapshot that we'll use to copy the
					 * other range.
					 *
					 * Unlike UPDATE, there's no old tuple to copy the TOAST
					 * pointers from. Therefore pass NULL for the source
					 * tuple, to enforce detoasting of the TOAST pointers in
					 * 'spilled_tuple'.
					 */
					adjust_toast_pointers(rel, spilled_tuple, NULL);

					apply_concurrent_insert(dest, spilled_tuple, new_tuple,
											xid);
				}
				else if (is_block_in_range(old_block, range_start, range_end))
				{
					found = find_target_tuple(dest, key, ondisk_tuple);
					if (!found)
						elog(ERROR, "could not find target tuple");

					/*
					 * The new key is in another range, so only delete the old
					 * one from the current range. The new version should be
					 * visible to the snapshot that we'll use to copy the
					 * other range.
					 */
					apply_concurrent_delete(rel, ondisk_tuple, xid);
				}

				/*
				 * Otherwise, both tuple versions belong to another range, so
				 * there's nothing to do here.
				 */
			}

			ExecClearTuple(old_update_tuple);
			have_old_tuple = false;
		}
		else
			elog(ERROR, "unrecognized kind of change: %d", kind);

		ResetPerTupleExprContext(dest->estate);
	}
	PopActiveSnapshot();

	/* Cleanup. */
	ExecDropSingleTupleTableSlot(spilled_tuple);
	ExecDropSingleTupleTableSlot(ondisk_tuple);
	ExecDropSingleTupleTableSlot(old_update_tuple);
	ExecDropSingleTupleTableSlot(new_tuple);

	MemoryContextSwitchTo(oldcxt);

	BufFileClose(file);
}

/*
 * Apply an insert from the spill of concurrent changes to the new copy of the
 * table. 'new_tuple' is the source for table AM.
 */
static void
apply_concurrent_insert(RepackDest *dest, TupleTableSlot *spill_tuple,
						TupleTableSlot *new_tuple, TransactionId xid)
{
	HeapTuple	tup;
	bool		shouldFree;

	/* Copy the contents to a slot that preserves the header fields. */
	Assert(TTS_IS_HEAPTUPLE(new_tuple));
	ExecCopySlot(new_tuple, spill_tuple);

	/* Get pointer to the contained tuple (not a copy). */
	tup = ExecFetchSlotHeapTuple(new_tuple, false, &shouldFree);
	Assert(!shouldFree);

	/* Set the XID. */
	HeapTupleHeaderSetXmin(tup->t_data, xid);

	/*
	 * Put the tuple in the table, but make sure it won't be decoded. At the
	 * same time, request that the XID we set above is used, instead of
	 * generating a new one.
	 *
	 * FirstCommandId is ok in the new table because the transaction that
	 * inserted the tuple has already committed, and no other transaction
	 * should ever need the CID.
	 */
	table_tuple_insert(dest->rel, new_tuple, FirstCommandId,
					   TABLE_INSERT_NO_LOGICAL | TABLE_REUSE_XID,
					   NULL);

	/* Update indexes with this new tuple. */
	ExecInsertIndexTuples(dest->rri,
						  dest->estate,
						  0,
						  new_tuple,
						  NIL, NULL);
	pgstat_progress_incr_param(PROGRESS_REPACK_HEAP_TUPLES_INSERTED, 1);
}

/*
 * Apply an update from the spill of concurrent changes to the new copy of the
 * table. 'new_tuple' is the source for table AM.
 */
static void
apply_concurrent_update(RepackDest *dest, TupleTableSlot *spilled_tuple,
						TupleTableSlot *ondisk_tuple,
						TupleTableSlot *new_tuple, TransactionId xid)
{
	HeapTuple	tup;
	bool		shouldFree;
	Relation	rel = dest->rel;
	LockTupleMode lockmode;
	TM_FailureData tmfd;
	TU_UpdateIndexes update_indexes;
	TM_Result	res;

	/* Copy the contents to a slot that preserves the header fields. */
	Assert(TTS_IS_HEAPTUPLE(new_tuple));
	ExecCopySlot(new_tuple, spilled_tuple);

	/* Get pointer to the contained tuple (not a copy). */
	tup = ExecFetchSlotHeapTuple(new_tuple, false, &shouldFree);
	Assert(!shouldFree);

	/* Set the XID. */
	HeapTupleHeaderSetXmin(tup->t_data, xid);

	/*
	 * Carry out the update, skipping logical decoding for it.
	 *
	 * See comments in apply_concurrent_insert() to understand why
	 * FirstCommandId is ok in the new table.
	 */
	res = table_tuple_update(rel, &(ondisk_tuple->tts_tid), new_tuple,
							 FirstCommandId,
							 TABLE_UPDATE_NO_LOGICAL | TABLE_REUSE_XID,
							 InvalidSnapshot,
							 InvalidSnapshot,
							 false,
							 &tmfd, &lockmode, &update_indexes);
	if (res != TM_Ok)
		ereport(ERROR,
				errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				errmsg("could not apply concurrent %s on relation \"%s\"",
					   "UPDATE", RelationGetRelationName(rel)));

	if (update_indexes != TU_None)
	{
		uint32		flags = EIIT_IS_UPDATE;

		if (update_indexes == TU_Summarizing)
			flags |= EIIT_ONLY_SUMMARIZING;
		ExecInsertIndexTuples(dest->rri,
							  dest->estate,
							  flags,
							  new_tuple,
							  NIL, NULL);
	}

	pgstat_progress_incr_param(PROGRESS_REPACK_HEAP_TUPLES_UPDATED, 1);
}

static void
apply_concurrent_delete(Relation rel, TupleTableSlot *slot, TransactionId xid)
{
	TM_Result	res;
	TM_FailureData tmfd;

	/*
	 * Delete tuple from the new heap, skipping logical decoding for it.
	 *
	 * See comments in heap_insert_for_repack() to understand why
	 * FirstCommandId is ok in the new table.
	 *
	 * See comments in apply_concurrent_insert() to understand why
	 * FirstCommandId is ok in the new table.
	 */
	res = table_tuple_delete(rel, &(slot->tts_tid),
							 xid, FirstCommandId,
							 TABLE_DELETE_NO_LOGICAL,
							 InvalidSnapshot, InvalidSnapshot,
							 false,
							 &tmfd);

	if (res != TM_Ok)
		ereport(ERROR,
				errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				errmsg("could not apply concurrent %s on relation \"%s\"",
					   "DELETE", RelationGetRelationName(rel)));

	pgstat_progress_incr_param(PROGRESS_REPACK_HEAP_TUPLES_DELETED, 1);
}

/*
 * Read tuple from file and put it in the input slot.  All memory is allocated
 * in the current memory context; caller is responsible for freeing it as
 * appropriate.
 *
 * External attributes are stored in separate memory chunks, in order to avoid
 * exceeding MaxAllocSize - that could happen if the individual attributes are
 * smaller than MaxAllocSize but the whole tuple is bigger.
 */
static void
restore_tuple(BufFile *file, Relation relation, TupleTableSlot *slot,
			  BlockNumber *block_nr_p, BlockNumber *old_block_nr_p,
			  TransactionId *xid_p)
{
	uint32		t_len;
	HeapTuple	tup;
	int			natt_ext;

	/* Read the tuple. */
	BufFileReadExact(file, &t_len, sizeof(t_len));
	tup = (HeapTuple) palloc(HEAPTUPLESIZE + t_len);
	tup->t_data = (HeapTupleHeader) ((char *) tup + HEAPTUPLESIZE);
	BufFileReadExact(file, tup->t_data, t_len);
	tup->t_len = t_len;
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * Put the tuple we read in a slot. This deforms it, so that we can hack
	 * the external attributes in place.
	 */
	ExecForceStoreHeapTuple(tup, slot, false);

	/* Handle TID separate because not all tuple slots care about it. */
	if (block_nr_p)
		*block_nr_p = ItemPointerGetBlockNumber(&tup->t_data->t_ctid);
	if (xid_p)
		*xid_p = HeapTupleHeaderGetXmin(tup->t_data);
	if (old_block_nr_p)
		BufFileReadExact(file, old_block_nr_p, sizeof(BlockNumber));

	/*
	 * Next, read any attributes we stored separately into the tts_values
	 * array elements expecting them, if any.  This matches
	 * repack_store_change.
	 */
	BufFileReadExact(file, &natt_ext, sizeof(natt_ext));
	if (natt_ext > 0)
	{
		TupleDesc	desc = slot->tts_tupleDescriptor;

		for (int i = 0; i < desc->natts; i++)
		{
			CompactAttribute *attr = TupleDescCompactAttr(desc, i);
			varlena    *varlen;
			uint64		chunk_header;
			void	   *value;
			Size		varlensz;

			if (attr->attisdropped || attr->attlen != -1)
				continue;
			if (slot_attisnull(slot, i + 1))
				continue;
			varlen = (varlena *) DatumGetPointer(slot->tts_values[i]);
			if (!VARATT_IS_EXTERNAL_INDIRECT(varlen))
				continue;
			slot_getsomeattrs(slot, i + 1);

			BufFileReadExact(file, &chunk_header, VARHDRSZ);
			varlensz = VARSIZE_ANY(&chunk_header);

			value = palloc(varlensz);
			memcpy(value, &chunk_header, VARHDRSZ);
			BufFileReadExact(file, (char *) value + VARHDRSZ, varlensz - VARHDRSZ);

			slot->tts_values[i] = PointerGetDatum(value);
			natt_ext--;
			if (natt_ext < 0)
				elog(ERROR, "insufficient number of attributes stored separately");
		}

		if (natt_ext != 0)
			elog(ERROR,
				 "unexpected number of attributes stored separately (%d remaining)",
				 natt_ext);
	}
}

/*
 * Adjust 'dest' replacing any EXTERNAL_ONDISK toast pointers with the
 * corresponding ones from 'src'. If 'src' is NULL, replace the toast pointer
 * with the actual value.
 */
static void
adjust_toast_pointers(Relation relation, TupleTableSlot *dest,
					  TupleTableSlot *src)
{
	TupleDesc	desc = dest->tts_tupleDescriptor;

	for (int i = 0; i < desc->natts; i++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(desc, i);
		varlena    *varlena_dst;

		if (attr->attisdropped)
			continue;
		if (attr->attlen != -1)
			continue;
		if (slot_attisnull(dest, i + 1))
			continue;

		slot_getsomeattrs(dest, i + 1);

		varlena_dst = (varlena *) DatumGetPointer(dest->tts_values[i]);
		if (!VARATT_IS_EXTERNAL_ONDISK(varlena_dst))
			continue;

		/*
		 * Ideally we just copy the value, but if there is no source tuple, we
		 * need to detoast the value.
		 */
		if (src)
		{
			slot_getsomeattrs(src, i + 1);
			dest->tts_values[i] = src->tts_values[i];
		}
		else
		{
			varlena    *detoasted;

			detoasted = detoast_external_attr(varlena_dst);
			dest->tts_values[i] = PointerGetDatum(detoasted);
		}
	}
}

/*
 * Check if tuple originates from given range of blocks that have already been
 * copied.
 */
static bool
is_block_in_range(BlockNumber blknum, BlockNumber start, BlockNumber end)
{
	Assert(BlockNumberIsValid(start) && BlockNumberIsValid(end));
	Assert(BlockNumberIsValid(blknum));

	if (start < end)
		return blknum >= start && blknum < end;
	else
	{
		/* Has the scan position wrapped around? */
		Assert(start > end);

		return blknum >= start || blknum < end;
	}
}

/*
 * Find the tuple to be updated or deleted by the given data change, whose
 * tuple has already been loaded into locator.
 *
 * If the tuple is found, put it in retrieved and return true.  If the tuple is
 * not found, return false.
 */
static bool
find_target_tuple(RepackDest *dest, TupleTableSlot *locator,
				  TupleTableSlot *retrieved)
{
	Relation	rel = dest->rel;
	Form_pg_index idx = dest->ident_index->rd_index;
	IndexScanDesc scan;
	bool		retval = false;

	/*
	 * Scan key is passed by caller, so it does not have to be constructed
	 * multiple times. Key entries have all fields initialized, except for
	 * sk_argument.
	 *
	 * Use the incoming tuple to finalize the scan key.
	 */
	for (int i = 0; i < dest->ident_key_nentries; i++)
	{
		ScanKey		entry = &dest->ident_key[i];
		AttrNumber	attno = idx->indkey.values[i];

		entry->sk_argument = locator->tts_values[attno - 1];
		Assert(!locator->tts_isnull[attno - 1]);
	}

	/* XXX no instrumentation for now */
	scan = index_beginscan(rel, dest->ident_index, false, GetActiveSnapshot(),
						   NULL, dest->ident_key_nentries, 0, 0);
	index_rescan(scan, dest->ident_key, dest->ident_key_nentries, NULL, 0);
	while (table_index_getnext_slot(scan, ForwardScanDirection, retrieved))
	{
		/* Be wary of temporal constraints */
		if (scan->xs_recheck && !identity_key_equal(dest, locator, retrieved))
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		retval = true;
		break;
	}
	index_endscan(scan);

	return retval;
}

/*
 * Check whether the candidate tuple matches the locator tuple on all replica
 * identity key columns, using the same equality operators as the identity
 * index scan.  The locator tuple has already been loaded into cc_ident_key.
 *
 * This is needed to filter lossy index matches, such as GiST multirange scans
 * used for temporal constraints.
 */
static bool
identity_key_equal(RepackDest *dest, TupleTableSlot *locator,
				   TupleTableSlot *candidate)
{
	slot_getsomeattrs(locator, dest->last_key_attno);
	slot_getsomeattrs(candidate, dest->last_key_attno);

	for (int i = 0; i < dest->ident_key_nentries; i++)
	{
		ScanKey		entry = &dest->ident_key[i];
		AttrNumber	attno = dest->ident_index->rd_index->indkey.values[i];

		Assert(attno > 0);

		if (locator->tts_isnull[attno - 1] != candidate->tts_isnull[attno - 1])
			return false;

		if (locator->tts_isnull[attno - 1])
			continue;

		if (!DatumGetBool(FunctionCall2Coll(&entry->sk_func,
											entry->sk_collation,
											candidate->tts_values[attno - 1],
											entry->sk_argument)))
			return false;
	}

	return true;
}

/*
 * Initialize the ChangeContext struct for the given relation.
 */
static void
initialize_change_context(ChangeContext *chgcxt, Relation relation,
						  Oid ident_index_id)
{
	initialize_change_dest(&chgcxt->cc_dest, relation, ident_index_id);

	chgcxt->cc_file_seq_snapshot = 0;
	chgcxt->cc_file_seq_changes = 0;

	chgcxt->cc_dest_aux = NULL;
	chgcxt->cc_clustering_index = InvalidOid;
	chgcxt->cc_last_snapshot_xmin = InvalidTransactionId;
}

/*
 * Free up resources taken by a ChangeContext.
 */
static void
release_change_context(ChangeContext *chgcxt)
{
	release_change_dest(&chgcxt->cc_dest);
	if (chgcxt->cc_dest_aux)
		release_change_dest(chgcxt->cc_dest_aux);
}

/*
 * Initialize the RepackDest struct for the given relation, with the given
 * index as identity index. InvalidOid can be specified to only make the
 * relation ready for insertions.
 */
static void
initialize_change_dest(RepackDest *dest, Relation relation,
					   Oid ident_index_id)
{
	dest->rel = relation;
	dest->bistate = GetBulkInsertState();

	/* If there's no identity index yet, there should be no indexes at all. */
	if (!OidIsValid(ident_index_id))
		return;

	/* Only initialize fields needed by ExecInsertIndexTuples(). */
	dest->estate = CreateExecutorState();

	/*
	 * Set up a range table for the executor, containing our repacked table as
	 * its only member.
	 */
	{
		RangeTblEntry *rte;
		TupleDesc	desc = RelationGetDescr(relation);
		List	   *perminfos = NIL;
		Bitmapset  *updatedCols = NULL;
		RTEPermissionInfo *perminfo;

		/*
		 * For our use, the RTE only needs to have perminfoindex initialized,
		 * but there's no reason to not set the fields whose values we have at
		 * hand.
		 */
		rte = makeNode(RangeTblEntry);
		rte->rtekind = RTE_RELATION;
		rte->relid = RelationGetRelid(relation);
		rte->relkind = RelationGetForm(relation)->relkind;
		/* Create the RTEPermissionInfo instance (and set ->perminfoindex). */
		addRTEPermissionInfo(&perminfos, rte);

		/*
		 * Initialize updatedCols to show that all columns are updated.  This
		 * is of course not necessarily true, and we cannot know this early;
		 * but this is only used by ExecInsertIndexTuples to flag index
		 * updates with no logical value changes, so if it's wrong, nothing
		 * terribly bad happens. We may want to improve this someday though.
		 *
		 * Don't claim that dropped columns are changed though.
		 */
		for (int i = 0; i < desc->natts; i++)
		{
			CompactAttribute *attr = TupleDescCompactAttr(desc, i);

			if (attr->attisdropped)
				continue;
			updatedCols = bms_add_member(updatedCols,
										 i + 1 - FirstLowInvalidHeapAttributeNumber);
		}

		/* install updatedCols in the right place */
		perminfo = getRTEPermissionInfo(perminfos, rte);
		perminfo->updatedCols = updatedCols;

		/* finally we can initialize the range table proper */
		ExecInitRangeTable(dest->estate, list_make1(rte), perminfos,
						   bms_make_singleton(1));
	}

	/* Set up our ResultRelInfo to use for index updates */
	dest->rri = makeNode(ResultRelInfo);
	InitResultRelInfo(dest->rri, relation, 1, NULL, 0);
	ExecOpenIndices(dest->rri, false);

	/*
	 * The table's relcache entry already has the relcache entry for the
	 * identity index; find that.
	 */
	dest->ident_index = NULL;
	for (int i = 0; i < dest->rri->ri_NumIndices; i++)
	{
		Relation	ind_rel;

		ind_rel = dest->rri->ri_IndexRelationDescs[i];
		if (ind_rel->rd_id == ident_index_id)
		{
			dest->ident_index = ind_rel;
			break;
		}
	}
	if (dest->ident_index == NULL)
		elog(ERROR, "could not find identity index");

	/* Set up for scanning said identity index */
	{
		Form_pg_index indexForm;

		indexForm = dest->ident_index->rd_index;
		dest->ident_key_nentries = indexForm->indnkeyatts;
		dest->ident_key = (ScanKey) palloc_array(ScanKeyData, indexForm->indnkeyatts);
		for (int i = 0; i < indexForm->indnkeyatts; i++)
		{
			ScanKey		entry;
			Oid			opfamily,
						opcintype,
						opno,
						opcode;
			StrategyNumber eq_strategy;

			entry = &dest->ident_key[i];

			opfamily = dest->ident_index->rd_opfamily[i];
			opcintype = dest->ident_index->rd_opcintype[i];
			eq_strategy = IndexAmTranslateCompareType(COMPARE_EQ,
													  dest->ident_index->rd_rel->relam,
													  opfamily, false);
			if (eq_strategy == InvalidStrategy)
				elog(ERROR, "could not find equality strategy for index operator family %u for type %u",
					 opfamily, opcintype);
			opno = get_opfamily_member(opfamily, opcintype, opcintype,
									   eq_strategy);
			if (!OidIsValid(opno))
				elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
					 eq_strategy, opcintype, opcintype, opfamily);
			opcode = get_opcode(opno);
			if (!OidIsValid(opcode))
				elog(ERROR, "missing oprcode for operator %u", opno);

			/* Initialize everything but argument. */
			ScanKeyInit(entry,
						i + 1,
						eq_strategy, opcode,
						(Datum) 0);
			entry->sk_collation = dest->ident_index->rd_indcollation[i];
		}
	}

	/* Determine the last column we must deform to read the identity */
	dest->last_key_attno = InvalidAttrNumber;
	for (int i = 0; i < dest->ident_key_nentries; i++)
	{
		AttrNumber	attno = dest->ident_index->rd_index->indkey.values[i];

		Assert(attno > 0);
		dest->last_key_attno = Max(dest->last_key_attno, attno);
	}
}

static void
release_change_dest(RepackDest *dest)
{
	FreeBulkInsertState(dest->bistate);

	/* It's possible that no indexes were opened during initialization. */
	if (dest->rri == NULL)
		return;

	ExecCloseIndices(dest->rri);
	FreeExecutorState(dest->estate);
	/* XXX are these pfrees necessary? */
	pfree(dest->rri);
	pfree(dest->ident_key);
}

/*
 * Copy information needed to re-initialize 'chgcxt' to 'backup'.
 */
static void
backup_change_context(ChangeContext *chgcxt, ChangeContextBackup *backup)
{
	RepackDest *dest;

	memset(backup, 0, sizeof(ChangeContextBackup));

	/* Store information on the new relation. */
	dest = &chgcxt->cc_dest;
	backup->rel = dest->rel;
	prepare_relation_for_reopening(&backup->rel, &backup->rel_ri, true);
	backup->ident_index = dest->ident_index ?
		RelationGetRelid(dest->ident_index) : InvalidOid;

	/* Store information on the auxiliary relation if one exists. */
	if (chgcxt->cc_dest_aux)
	{
		dest = chgcxt->cc_dest_aux;
		backup->rel_aux = dest->rel;
		prepare_relation_for_reopening(&backup->rel_aux, &backup->rel_aux_ri,
									   true);
		backup->ident_index_aux = dest->ident_index ?
			RelationGetRelid(dest->ident_index) : InvalidOid;
	}
	else
	{
		/* There should be no reopening. */
		Assert(backup->rel_aux_ri.relid == InvalidOid);
	}

	/* Backup the common fields. */
	backup->file_seq_snapshot = chgcxt->cc_file_seq_snapshot;
	backup->file_seq_changes = chgcxt->cc_file_seq_changes;
	backup->clustering_index = chgcxt->cc_clustering_index;
	backup->last_snapshot_xmin = chgcxt->cc_last_snapshot_xmin;
}

/*
 * Create and initialize an instance of ChangeContext, based on previously
 * existing instance. It includes re-opening of relations and indexes.
 */
static ChangeContext *
reinitialize_change_context(ChangeContextBackup *backup)
{
	ChangeContext *chgcxt;
	RelReopenInfo *rri;

	chgcxt = palloc0_object(ChangeContext);

	/* Re-initialize the new relation part. */
	rri = &backup->rel_ri;
	reopen_relation(rri);
	initialize_change_context(chgcxt, backup->rel, backup->ident_index);

	/* The same for the auxiliary relation, if it exists. */
	rri = &backup->rel_aux_ri;
	if (OidIsValid(rri->relid))
	{
		reopen_relation(rri);
		chgcxt->cc_dest_aux = palloc0_object(RepackDest);
		initialize_change_dest(chgcxt->cc_dest_aux, backup->rel_aux,
							   backup->ident_index_aux);
	}

	/* Restore the common fields. */
	chgcxt->cc_file_seq_snapshot = backup->file_seq_snapshot;
	chgcxt->cc_file_seq_changes = backup->file_seq_changes;
	chgcxt->cc_clustering_index = backup->clustering_index;
	chgcxt->cc_last_snapshot_xmin = backup->last_snapshot_xmin;

	return chgcxt;
}

/*
 * Lock the relation and its TOAST relation using a session lock.
 */
static void
enable_session_locks(Relation rel, LOCKMODE lmode, bool is_new)
{
	LockRelId	lock;
	Oid			toastid;

	Assert(CheckRelationLockedByMe(rel, lmode, false));
	lock = rel->rd_lockInfo.lockRelId;
	LockRelationIdForSession(&lock, lmode);

	/*
	 * The same for TOAST relation. XXX Is it ok that we do not lock TOAST
	 * relation of the old relation until we start swapping the files? (The
	 * new relation's TOAST is locked on creation, so we keep it that way.)
	 */
	toastid = rel->rd_rel->reltoastrelid;
	if (is_new && OidIsValid(toastid))
	{
		Assert(CheckRelationOidLockedByMe(toastid, lmode, false));
		lock.relId = toastid;
		LockRelationIdForSession(&lock, lmode);
	}
}

/*
 * Disable session locks acquired earlier by enable_session_locks(), but first
 * acquire normal (transactional) lock(s).
 */
static void
disable_session_locks(Relation rel, LOCKMODE lmode, bool is_new)
{
	LockRelId	lock;
	Oid			toastid;

	LockRelation(rel, lmode);
	lock = rel->rd_lockInfo.lockRelId;
	UnlockRelationIdForSession(&lock, lmode);
	Assert(CheckRelationLockedByMe(rel, lmode, false));

	/* The same for TOAST relation. See enable_session_locks(). */
	toastid = rel->rd_rel->reltoastrelid;
	if (is_new && OidIsValid(toastid))
	{
		lock.relId = toastid;
		LockRelationId(&lock, lmode);
		UnlockRelationIdForSession(&lock, lmode);
		Assert(CheckRelationOidLockedByMe(toastid, lmode, false));
	}
}

/*
 * Remember OID of a relation, lock it using a session lock and close it. and a
 *
 * The lock mode is always ShareUpdateExclusiveLock, as this is related to
 * REPACK (CONCURRENTLY).
 */
static void
prepare_relation_for_reopening(Relation *p_rel, RelReopenInfo *backup,
							   bool is_new)
{
	Relation	rel = *p_rel;
	LOCKMODE	lmode;

	lmode = is_new ? AccessExclusiveLock : ShareUpdateExclusiveLock;

	/* Remember what we need to be able to re-open the relation later. */
	backup->relid = RelationGetRelid(rel);
	backup->p_rel = p_rel;
	backup->is_new = is_new;

	/*
	 * Close it, but make sure the lock is not lost at transaction boundary.
	 */
	enable_session_locks(rel, lmode, is_new);
	relation_close(rel, NoLock);
}

/*
 * Open the relation, acquire transactional lock on it and release the session
 * lock acquired by prepare_relation_for_reopening().
 */
static void
reopen_relation(RelReopenInfo *backup)
{
	Relation	rel;
	LOCKMODE	lmode;

	lmode = backup->is_new ? AccessExclusiveLock : ShareUpdateExclusiveLock;
	rel = relation_open(backup->relid, lmode);
	disable_session_locks(rel, lmode, backup->is_new);
	*backup->p_rel = rel;
}

/*
 * The final steps of rebuild_relation() for concurrent processing.
 *
 * On entry, NewHeap is locked in AccessExclusiveLock mode. OldHeap and its
 * clustering index (if one is passed) are still locked in a mode that allows
 * concurrent data changes. On exit, both tables and their indexes are closed,
 * but locked in AccessExclusiveLock mode.
 */
static void
rebuild_relation_finish_concurrent(Relation NewHeap, Relation OldHeap,
								   Oid identIdx, TransactionId frozenXid,
								   MultiXactId cutoffMulti,
								   double num_tuples,
								   ChangeContext *chgcxt)
{
	List	   *ind_oids_new;
	Oid			old_table_oid = RelationGetRelid(OldHeap);
	Oid			new_table_oid = RelationGetRelid(NewHeap);
	List	   *ind_oids_old;
	ListCell   *lc,
			   *lc2;
	char		relpersistence;
	bool		is_system_catalog;
	XLogRecPtr	end_of_wal;
	MemoryContext oldcxt;
	List	   *indexrels;
	List	   *inds_tmp = NIL;
	BlockNumber num_pages;

	Assert(CheckRelationLockedByMe(OldHeap, ShareUpdateExclusiveLock, false));
	Assert(CheckRelationLockedByMe(NewHeap, AccessExclusiveLock, false));

	/*
	 * If we have the auxiliary table, this is the moment we should use it.
	 * Note that the function can start new transaction(s). In that case it
	 * re-opens the new and old heap, so pass pointers to get the relation
	 * cache entries updated.
	 */
	if (chgcxt->cc_dest_aux)
		chgcxt = process_auxiliary_table(chgcxt, &OldHeap, &NewHeap, identIdx,
										 frozenXid, cutoffMulti);

	/*
	 * Unlike the exclusive case, we build new indexes for the new relation
	 * rather than swapping the storage and reindexing the old relation. The
	 * point is that the index build can take some time, so we do it before we
	 * get AccessExclusiveLock on the old heap and therefore we cannot swap
	 * the heap storage yet.
	 *
	 * index_create() will lock the new indexes using AccessExclusiveLock - no
	 * need to change that. At the same time, we use ShareUpdateExclusiveLock
	 * to lock the existing indexes - that should be enough to prevent others
	 * from changing them while we're repacking the relation. The lock on
	 * table should prevent others from changing the index column list, but
	 * might not be enough for commands like ALTER INDEX ... SET ... (Those
	 * are not necessarily dangerous, but can make user confused if the
	 * changes they do get lost due to REPACK.)
	 *
	 * As the identity index already had to be built, skip it here. XXX
	 * Consider if the retail inserts during data copying (in the case w/o
	 * auxiliary table) can be a problem in terms of index layout. Shouldn't
	 * we drop the identity index and build it using bulk insert too?
	 */
	oldcxt = MemoryContextSwitchTo(repack_cxt);
	ind_oids_old = RelationGetIndexList(OldHeap);
	foreach_oid(ind_oid, ind_oids_old)
	{
		if (ind_oid != identIdx)
			inds_tmp = lappend_oid(inds_tmp, ind_oid);
	}
	MemoryContextSwitchTo(oldcxt);
	ind_oids_old = inds_tmp;
	ind_oids_new = build_new_indexes(ind_oids_old, &OldHeap, &NewHeap,
									 &chgcxt);

	/*
	 * The identity index will be involved in the following processing.
	 */
	ind_oids_old = lappend_oid(ind_oids_old, identIdx);
	ind_oids_new = lappend_oid(ind_oids_new,
							   RelationGetRelid(chgcxt->cc_dest.ident_index));

	/*
	 * Since we haven't copied "recently dead" tuples into the new heap, we
	 * must not finish the processing until they are considered dead by all
	 * backends.
	 *
	 * In particular, the VACUUM xmin horizon for the table must be at least
	 * xmin of the last snapshot that we used to copy the data.  That means
	 * even the least recently deleted tuples we omitted from the copying
	 * (because we considered them dead) must be considered dead by anyone.
	 *
	 * Note: Although some time should have elapsed since the data copying
	 * stage (at least the time to build the indexes), we might get stuck here
	 * due to another backend running REPACK because its snapshot does not
	 * allow the xmin horizon to advance for some time.
	 *
	 * TODO Consider this when determining the value of
	 * repack_pages_per_snapshot (currently GUC, in the future preferably a
	 * constant). Is this worth an additional phase in progress reporting?
	 */
	Assert(TransactionIdIsValid(chgcxt->cc_last_snapshot_xmin));
	while (true)
	{
		TransactionId oldest_xmin;

		oldest_xmin = GetOldestNonRemovableTransactionId(OldHeap);
		if (TransactionIdFollowsOrEquals(oldest_xmin,
										 chgcxt->cc_last_snapshot_xmin))
			break;

		/* Wait before the next check. */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000L,
						 WAIT_EVENT_REPACK_MVCC_SAFETY);
		ResetLatch(MyLatch);
	}

	/*
	 * During testing, wait for another backend to perform concurrent data
	 * changes which we will process below.
	 */
	INJECTION_POINT("repack-concurrently-before-lock", NULL);

	/*
	 * Flush all WAL records inserted so far (possibly except for the last
	 * incomplete page; see GetInsertRecPtr), to minimize the amount of data
	 * we need to flush while holding exclusive lock on the source table.
	 */
	XLogFlush(GetXLogInsertEndRecPtr());
	end_of_wal = GetFlushRecPtr(NULL);

	/*
	 * Decode and apply concurrent changes again, to minimize the time we need
	 * to hold AccessExclusiveLock. (Quite some amount of WAL could have been
	 * written during the data copying and index creation.)
	 */
	repack_process_concurrent_changes(chgcxt, end_of_wal,
									  InvalidBlockNumber, InvalidBlockNumber,
									  false, false);

	/*
	 * Acquire AccessExclusiveLock on the table, its TOAST relation (if there
	 * is one), all its indexes, so that we can swap the files.
	 */
	LockRelationOid(old_table_oid, AccessExclusiveLock);

	/*
	 * Lock all indexes now, not only the clustering one: all indexes need to
	 * have their files swapped. While doing that, store their relation
	 * references in a zero-terminated array, to handle predicate locks below.
	 */
	indexrels = NIL;
	foreach_oid(ind_oid, ind_oids_old)
	{
		Relation	index;

		index = index_open(ind_oid, AccessExclusiveLock);

		/*
		 * Some things about the index may have changed before we locked the
		 * index, such as ALTER INDEX RENAME.  We don't need to do anything
		 * here to absorb those changes in the new index.
		 */
		indexrels = lappend(indexrels, index);
	}

	/*
	 * Lock the OldHeap's TOAST relation exclusively - again, the lock is
	 * needed to swap the files.
	 */
	if (OidIsValid(OldHeap->rd_rel->reltoastrelid))
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, AccessExclusiveLock);

	/*
	 * Tuples and pages of the old heap will be gone, but the heap will stay.
	 */
	TransferPredicateLocksToHeapRelation(OldHeap);
	foreach_ptr(RelationData, index, indexrels)
	{
		TransferPredicateLocksToHeapRelation(index);
		index_close(index, NoLock);
	}
	list_free(indexrels);

	/*
	 * Flush WAL again, to make sure that all changes committed while we were
	 * waiting for the exclusive lock are available for decoding.
	 */
	XLogFlush(GetXLogInsertEndRecPtr());
	end_of_wal = GetFlushRecPtr(NULL);

	/*
	 * Decode and apply the concurrent changes again. Indicate that the
	 * decoding worker won't be needed anymore.
	 */
	repack_process_concurrent_changes(chgcxt, end_of_wal,
									  InvalidBlockNumber, InvalidBlockNumber,
									  false, true);

	/* Remember info about rel before closing OldHeap */
	relpersistence = OldHeap->rd_rel->relpersistence;
	is_system_catalog = IsSystemRelation(OldHeap);

	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_SWAP_REL_FILES);

	/*
	 * Even ShareUpdateExclusiveLock should have prevented others from
	 * creating / dropping indexes (even using the CONCURRENTLY option), so we
	 * do not need to check whether the lists match.
	 */
	forboth(lc, ind_oids_old, lc2, ind_oids_new)
	{
		Oid			ind_old = lfirst_oid(lc);
		Oid			ind_new = lfirst_oid(lc2);
		Oid			mapped_tables[4] = {0};

		swap_relation_files(ind_old, ind_new,
							(old_table_oid == RelationRelationId),
							false,	/* swap_toast_by_content */
							true,
							mapped_tables,
							NULL);

#ifdef USE_ASSERT_CHECKING

		/*
		 * Concurrent processing is not supported for system relations, so
		 * there should be no mapped tables.
		 */
		for (int i = 0; i < 4; i++)
			Assert(!OidIsValid(mapped_tables[i]));
#endif
	}

	/* The new indexes must be visible for deletion. */
	CommandCounterIncrement();

	/* Update the numbers of pages and tuples in pg_class. */
	num_pages = RelationGetNumberOfBlocks(NewHeap);
	copy_table_data_update_stats(OldHeap, NewHeap, num_pages, num_tuples);

	/* Close the old heap but keep lock until transaction commit. */
	table_close(OldHeap, NoLock);
	/* Close the new heap. (We didn't have to open its indexes). */
	table_close(NewHeap, NoLock);

	/* Cleanup what we don't need anymore. (And close the identity index.) */
	release_change_context(chgcxt);

	/*
	 * Swap the relations and their TOAST relations and TOAST indexes. This
	 * also drops the new relation and its indexes.
	 *
	 * update_toast_cutoffs is true because REPACK (CONCURRENTLY) does not
	 * freeze tuples decoded from WAL, and because RecentXmin is not affected
	 * by logical decoding. Thus if we accepted relfrozenxid of the new TOAST
	 * relation (derived from RecentXmin), it could incorrectly tell that we
	 * froze more recent XID's than we actually did.
	 *
	 * (System catalogs are currently not supported.)
	 */
	Assert(!is_system_catalog);
	finish_heap_swap(old_table_oid, new_table_oid,
					 is_system_catalog,
					 false,		/* swap_toast_by_content */
					 false,
					 true,
					 false,		/* reindex */
					 frozenXid, cutoffMulti,
					 true,		/* update_toast_cutoffs */
					 relpersistence);
}

/*
 * Copy the contents of the auxiliary table to the new table in the desired
 * order, then drop the auxiliary table.
 *
 * All the opened relations may need to be closed and re-opened because the
 * function may start a new transaction. That's why a pointer to old and new
 * heap is passed. 'chgcxt' is re-created in that case, so user should use the
 * returned value.
 */
static ChangeContext *
process_auxiliary_table(ChangeContext *chgcxt, Relation *pOldHeap,
						Relation *pNewHeap, Oid identIdx,
						TransactionId freeze_xid, MultiXactId cutoff_multi)
{
	RepackDest *dest = chgcxt->cc_dest_aux;
	Oid			ident_idx_new;
	Relation	clustering_index;
	IndexScanDesc scan;
	TupleTableSlot *slot;
	Oid			aux_oid;
	ObjectAddress object;
	Relation	rel;
	SnapshotData SnapshotNewHeap;
	RewriteState rwstate;

	/*
	 * First, make sure the clustering index exists.
	 */
	if (OidIsValid(chgcxt->cc_clustering_index))
	{
		Oid			cl_ind_oid;

		/*
		 * Create it according to the clustering index on the old relation.
		 */
		cl_ind_oid = build_new_index(dest->rel, *pOldHeap,
									 chgcxt->cc_clustering_index);

		/*
		 * The index build generated a new XID, so commit the transaction and
		 * start a new one.
		 */
		chgcxt = start_new_transaction(chgcxt, pOldHeap, pNewHeap);
		dest = chgcxt->cc_dest_aux;

		clustering_index = index_open(cl_ind_oid, NoLock);
	}
	else
	{
		/* The identity index is also the clustering index. */
		clustering_index = dest->ident_index;
	}

	/* Now do the copying. */
	slot = table_slot_create(dest->rel, NULL);

	/*
	 * No point in specifying the auxiliary relation as the old one: we
	 * haven't frozen tuples when inserting them (one freezing is enough, see
	 * below), so the tuples do not satisfy the relfrozenxid / relminmxid
	 * limits, and thus the following freezing would fail.
	 */
	rwstate = begin_heap_rewrite(NULL, chgcxt->cc_dest.rel,
	/* oldest_xmin only needed for rewriting */
								 InvalidTransactionId,
								 freeze_xid, cutoff_multi,
								 true);

	/*
	 * Scan of the auxiliary table can take long time, but the SnapshotNewHeap
	 * snapshot can be used here (because there should be no aborted
	 * insertions in the table), so the scan should not affect the xmin
	 * horizons.
	 */
	PopActiveSnapshot();
	InitNewHeapSnapshot(SnapshotNewHeap);
	PushActiveSnapshot(&SnapshotNewHeap);
	scan = index_beginscan(dest->rel, clustering_index, false,
						   GetActiveSnapshot(), NULL, 0, 0, SO_NONE);
	index_rescan(scan, NULL, 0, NULL, 0);
	for (;;)
	{
		HeapTuple	tuple;
		bool		shouldFree;

		CHECK_FOR_INTERRUPTS();

		if (!table_index_getnext_slot(scan, ForwardScanDirection, slot))
			break;

		/* Make sure we have a writable copy of the tuple. */
		tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

		/*
		 * This kind of slot maintains the tuple header. We don't need to copy
		 * the contents into a slot of other kind because reforming was
		 * performed when populating the auxiliary table.
		 */
		Assert(TTS_IS_BUFFERTUPLE(slot));
		Assert(TransactionIdIsValid(HeapTupleHeaderGetXmin(tuple->t_data)));

		/*
		 * Insert the tuple into the new relation, and freeze it while doing
		 * so.
		 *
		 * Since our copy is already writable, the tuple can be passed for
		 * both old and new tuple.
		 */
		rewrite_heap_tuple_no_chains(rwstate, tuple, tuple, true);

		if (shouldFree)
			pfree(tuple);
	}
	index_endscan(scan);
	PopActiveSnapshot();
	InvalidateCatalogSnapshot();

	/*
	 * We should not be restricting the progress of xmin horizons at the
	 * moment.
	 */
	Assert(!TransactionIdIsValid(MyProc->xmin));
	Assert(!TransactionIdIsValid(MyProc->xid));
	Assert(!HaveRegisteredOrActiveSnapshot());

	PushActiveSnapshot(GetTransactionSnapshot());
	ExecDropSingleTupleTableSlot(slot);
	end_heap_rewrite(rwstate);

	/*
	 * Close the relation, its identity index and clustering index if we had
	 * to open it above. Lock will be released on commit.
	 */
	aux_oid = RelationGetRelid(dest->rel);
	table_close(dest->rel, NoLock);
	if (OidIsValid(chgcxt->cc_clustering_index))
		index_close(clustering_index, NoLock);
	/* Here we close the other indexes. */
	release_change_dest(dest);
	chgcxt->cc_dest_aux = NULL;

	/* Drop the auxiliary table. */
	object.classId = RelationRelationId;
	object.objectId = aux_oid;
	object.objectSubId = 0;
	performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* Build the identity index on the new relation. */
	ident_idx_new = build_new_index(chgcxt->cc_dest.rel, *pOldHeap, identIdx);

	/*
	 * The index build generated a new XID, so commit the transaction and
	 * start a new one.
	 */
	chgcxt = start_new_transaction(chgcxt, pOldHeap, pNewHeap);

	/*
	 * Make the new heap ready to use the index for future replaying of
	 * concurrent changes.
	 */
	rel = chgcxt->cc_dest.rel;
	release_change_dest(&chgcxt->cc_dest);
	initialize_change_dest(&chgcxt->cc_dest, rel, ident_idx_new);

	return chgcxt;
}

/*
 * Build indexes on NewHeap according to those on OldHeap.
 *
 * OldIndexes is the list of index OIDs on OldHeap. The contained indexes end
 * up locked using ShareUpdateExclusiveLock.
 *
 * A list of OIDs of the corresponding indexes created on NewHeap is
 * returned. The order of items does match, so we can use these arrays to swap
 * index storage.
 *
 * A separate transaction is used for each index. Therefore caller needs to
 * pass pointers to the relcache entries so that we can provide him with new
 * entries after reopening the relations. Likewise, pointer to ChangeContext
 * is used to return the re-created instance.
 */
static List *
build_new_indexes(List *OldIndexes, Relation *p_old, Relation *p_new,
				  ChangeContext **p_chgcxt)
{
	ChangeContext *chgcxt = *p_chgcxt;
	List	   *result = NIL;

	foreach_oid(oldindex, OldIndexes)
	{
		Relation	rel_old = *p_old;
		Relation	rel_new = *p_new;
		Oid			newindex;
		MemoryContext oldcxt;

		newindex = build_new_index(rel_new, rel_old, oldindex);

		/*
		 * Allocate the list in repack_cxt so it survives the current
		 * transaction.
		 */
		oldcxt = MemoryContextSwitchTo(repack_cxt);
		result = lappend_oid(result, newindex);
		MemoryContextSwitchTo(oldcxt);

		/*
		 * Use one transaction per index, to limit the impact on xmin
		 * horizons. XXX Does it make sense to even first commit the catalog
		 * changes and then do the build in a new transaction (which should
		 * not set MyProc->xmin until the build is complete)? Not sure.
		 */
		chgcxt = start_new_transaction(chgcxt, p_old, p_new);
	}

	*p_chgcxt = chgcxt;

	return result;
}

/*
 * Subroutine of build_new_indexes().
 */
static Oid
build_new_index(Relation NewHeap, Relation OldHeap, Oid oldindex)
{
	Oid			newindex;
	char	   *newName;
	Relation	ind;

	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_REBUILD_INDEX);

	ind = index_open(oldindex, ShareUpdateExclusiveLock);

	newName = ChooseRelationName(get_rel_name(oldindex),
								 NULL,
								 "repacknew",
								 get_rel_namespace(ind->rd_index->indrelid),
								 false);

	/* Functions in indexes may want a snapshot set. */
	Assert(ActiveSnapshotSet());
	newindex = index_create_copy(NewHeap, INDEX_CREATE_SUPPRESS_PROGRESS,
								 oldindex, ind->rd_rel->reltablespace,
								 newName);

	copy_index_constraints(ind, newindex, RelationGetRelid(NewHeap));
	index_close(ind, NoLock);

	pgstat_progress_incr_param(PROGRESS_REPACK_INDEX_REBUILD_COUNT, 1);

	return newindex;
}

/*
 * Start a new transaction, but make sure that the caller still has valid
 * relcache entries as well as valid ChangeContext. The problem is that
 * relcache references should be closed before transaction commit, so we need
 * them re-opened in the new transaction. Also, commit releases all locks
 * acquired in the transaction, however we don't want to lose our locks. We
 * work it around by using session locks temporarily.
 */
static ChangeContext *
start_new_transaction(ChangeContext *chgcxt, Relation *p_old, Relation *p_new)
{
	ChangeContextBackup chgcxt_backup;
	RelReopenInfo rri_old;
	MemoryContext oldcxt;
	IndexBuildSecurity ibsec = chgcxt->cc_ind_build_sec;

	/* This also closes the new relation. */
	backup_change_context(chgcxt, &chgcxt_backup);
	release_change_context(chgcxt);
	prepare_relation_for_reopening(p_old, &rri_old, false);

	disable_index_build_security(&ibsec);
	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	enable_index_build_security(ibsec.userid, &ibsec);

	/* Create 'chgcxt' using re-opened relations. */
	oldcxt = MemoryContextSwitchTo(repack_cxt);
	/* This also re-opens the new relation. */
	chgcxt = reinitialize_change_context(&chgcxt_backup);
	chgcxt->cc_ind_build_sec = ibsec;
	MemoryContextSwitchTo(oldcxt);

	reopen_relation(&rri_old);

	/* Update the callers relcache entry for the new relation. */
	*p_new = chgcxt->cc_dest.rel;

	return chgcxt;
}

/*
 * Create a transient copy of a constraint -- supported by a transient
 * copy of the index that supports the original constraint.
 *
 * When repacking a table that contains exclusion constraints, the executor
 * relies on these constraints being properly catalogued.  These copies are
 * to support that.
 *
 * We don't need the constraints for anything else (the original constraints
 * will be there once repack completes), so we add pg_depend entries so that
 * they are dropped when the transient table is dropped.
 */
static void
copy_index_constraints(Relation old_index, Oid new_index_id, Oid new_heap_id)
{
	ScanKeyData skey;
	Relation	rel;
	TupleDesc	desc;
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress objrel;

	rel = table_open(ConstraintRelationId, RowExclusiveLock);
	ObjectAddressSet(objrel, RelationRelationId, new_heap_id);

	/*
	 * Retrieve the constraints supported by the old index and create an
	 * identical one that points to the new index.
	 */
	ScanKeyInit(&skey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(old_index->rd_index->indrelid));
	scan = systable_beginscan(rel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, &skey);
	desc = RelationGetDescr(rel);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(tup);
		Oid			oid;
		Datum		values[Natts_pg_constraint] = {0};
		bool		nulls[Natts_pg_constraint] = {0};
		bool		replaces[Natts_pg_constraint] = {0};
		HeapTuple	new_tup;
		ObjectAddress objcon;

		if (conform->conindid != RelationGetRelid(old_index))
			continue;

		oid = GetNewOidWithIndex(rel, ConstraintOidIndexId,
								 Anum_pg_constraint_oid);
		values[Anum_pg_constraint_oid - 1] = ObjectIdGetDatum(oid);
		replaces[Anum_pg_constraint_oid - 1] = true;
		values[Anum_pg_constraint_conrelid - 1] = ObjectIdGetDatum(new_heap_id);
		replaces[Anum_pg_constraint_conrelid - 1] = true;
		values[Anum_pg_constraint_conindid - 1] = ObjectIdGetDatum(new_index_id);
		replaces[Anum_pg_constraint_conindid - 1] = true;

		new_tup = heap_modify_tuple(tup, desc, values, nulls, replaces);

		/* Insert it into the catalog. */
		CatalogTupleInsert(rel, new_tup);

		/* Create a dependency so it's removed when we drop the new heap. */
		ObjectAddressSet(objcon, ConstraintRelationId, oid);
		recordDependencyOn(&objcon, &objrel, DEPENDENCY_AUTO);
	}
	systable_endscan(scan);

	table_close(rel, RowExclusiveLock);

	CommandCounterIncrement();
}

/*
 * Create a transient copy of attribute defaults.
 *
 * When repacking a table that has stored generated columns, the executor
 * relies on these entries to generate the values for them during apply of
 * concurrent operations.  These copies are there to support that.
 *
 * We don't need the defaults for anything else, so we add pg_depend entries
 * so that they are dropped when the transient table is dropped.
 */
static void
copy_attribute_defaults(Oid old_heap_oid, Oid new_heap_oid)
{
	ScanKeyData skey;
	Relation	rel;
	Relation	att_rel;
	SysScanDesc scan;
	HeapTuple	def_tup;
	ObjectAddress objrel;

	rel = table_open(AttrDefaultRelationId, RowExclusiveLock);
	att_rel = table_open(AttributeRelationId, RowExclusiveLock);

	ObjectAddressSet(objrel, RelationRelationId, new_heap_oid);

	ScanKeyInit(&skey,
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(old_heap_oid));
	scan = systable_beginscan(rel, AttrDefaultIndexId, true,
							  NULL, 1, &skey);
	while (HeapTupleIsValid(def_tup = systable_getnext(scan)))
	{
		Form_pg_attrdef adform;
		Oid			oid;
		Datum		def_values[Natts_pg_attrdef];
		bool		def_nulls[Natts_pg_attrdef];
		bool		def_replaces[Natts_pg_attrdef] = {0};
		Datum		att_values[Natts_pg_attribute];
		bool		att_nulls[Natts_pg_attribute];
		bool		att_replaces[Natts_pg_attribute] = {0};
		HeapTuple	new_def_tup,
					att_tup,
					new_att_tup;
		ObjectAddress objad;

		adform = (Form_pg_attrdef) GETSTRUCT(def_tup);
		Assert(adform->adrelid == old_heap_oid);

		/*
		 * Insert a new tuple that's identical to the existing one, other than
		 * its OID and the relation it refers to.
		 */
		oid = GetNewOidWithIndex(rel, AttrDefaultOidIndexId,
								 Anum_pg_attrdef_oid);
		def_values[Anum_pg_attrdef_oid - 1] = ObjectIdGetDatum(oid);
		def_nulls[Anum_pg_attrdef_oid - 1] = false;
		def_replaces[Anum_pg_attrdef_oid - 1] = true;
		def_values[Anum_pg_attrdef_adrelid - 1] = ObjectIdGetDatum(new_heap_oid);
		def_nulls[Anum_pg_attrdef_adrelid - 1] = false;
		def_replaces[Anum_pg_attrdef_adrelid - 1] = true;
		new_def_tup = heap_modify_tuple(def_tup, RelationGetDescr(rel),
										def_values, def_nulls, def_replaces);
		CatalogTupleInsert(rel, new_def_tup);

		/* Set atthasdef for this attribute in the transient table */
		att_tup = SearchSysCache2(ATTNUM,
								  ObjectIdGetDatum(new_heap_oid),
								  ObjectIdGetDatum(adform->adnum));
		if (!HeapTupleIsValid(att_tup))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 adform->adnum, new_heap_oid);
		att_values[Anum_pg_attribute_atthasdef - 1] = BoolGetDatum(true);
		att_nulls[Anum_pg_attribute_atthasdef - 1] = false;
		att_replaces[Anum_pg_attribute_atthasdef - 1] = true;
		new_att_tup = heap_modify_tuple(att_tup, RelationGetDescr(att_rel),
										att_values, att_nulls, att_replaces);
		CatalogTupleUpdate(att_rel, &new_att_tup->t_self, new_att_tup);
		ReleaseSysCache(att_tup);

		/* Add a pg_depend record so it's removed with the transient table */
		ObjectAddressSet(objad, AttrDefaultRelationId, oid);
		recordDependencyOn(&objad, &objrel, DEPENDENCY_AUTO);
	}
	systable_endscan(scan);

	table_close(rel, RowExclusiveLock);
	table_close(att_rel, RowExclusiveLock);

	CommandCounterIncrement();
}

/*
 * Try to start a background worker to perform logical decoding of data
 * changes applied to relation while REPACK CONCURRENTLY is copying its
 * contents to a new table.
 */
static void
start_repack_decoding_worker(Oid relid)
{
	Size		size;
	DecodingWorkerShared *shared;
	shm_mq	   *mq;
	BackgroundWorker bgw;

	decoding_worker = palloc0_object(DecodingWorker);

	/* Setup shared memory. */
	size = BUFFERALIGN(offsetof(DecodingWorkerShared, error_queue)) +
		BUFFERALIGN(REPACK_ERROR_QUEUE_SIZE);
	decoding_worker->seg = dsm_create(size, 0);
	/* The segment should not be detached at transaction boundary. */
	dsm_pin_mapping(decoding_worker->seg);

	shared = (DecodingWorkerShared *) dsm_segment_address(decoding_worker->seg);
	shared->initialized = false;
	/* Snapshot is the first thing we need from the worker. */
	shared->snapshot_requested = true;
	shared->lsn_upto = InvalidXLogRecPtr;
	shared->done = false;
	SharedFileSetInit(&shared->sfs, decoding_worker->seg);
	shared->last_exported_snapshot = -1;
	shared->last_exported_changes = -1;
	SpinLockInit(&shared->mutex);
	shared->dbid = MyDatabaseId;

	/*
	 * This is the UserId set in cluster_rel(). Security context shouldn't be
	 * needed for decoding worker.
	 */
	shared->roleid = GetUserId();
	shared->relid = relid;
	ConditionVariableInit(&shared->cv);
	shared->backend_pid = MyProcPid;
	shared->backend_proc_number = MyProcNumber;

	/* Transmit our timeouts to the worker too */
	shared->lock_timeout = LockTimeout;
	shared->transaction_timeout = TransactionTimeout;

	mq = shm_mq_create((char *) BUFFERALIGN(shared->error_queue),
					   REPACK_ERROR_QUEUE_SIZE);
	shm_mq_set_receiver(mq, MyProc);

	decoding_worker->error_mqh = shm_mq_attach(mq, decoding_worker->seg, NULL);

	memset(&bgw, 0, sizeof(bgw));
	snprintf(bgw.bgw_name, BGW_MAXLEN,
			 "REPACK decoding worker for relation \"%s\"",
			 get_rel_name(relid));
	snprintf(bgw.bgw_type, BGW_MAXLEN, "REPACK decoding worker");
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "RepackWorkerMain");
	bgw.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(decoding_worker->seg));
	bgw.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&bgw, &decoding_worker->handle))
		ereport(ERROR,
				errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				errmsg("out of background worker slots"),
				errhint("You might need to increase \"%s\".", "max_worker_processes"));

	/*
	 * Now that the worker is registered, connect the error message queue to
	 * it.
	 */
	shm_mq_set_handle(decoding_worker->error_mqh, decoding_worker->handle);

	/*
	 * Make sure the worker has started before we wait for it to initialize
	 * decoding below, so that the failure-to-start case does not hang
	 * forever.
	 */
	wait_for_repack_decoding_worker();

	/*
	 * The decoding setup must be done before the caller can have XID assigned
	 * for any reason, otherwise the worker might end up in a deadlock,
	 * waiting for the caller's transaction to end. Therefore wait here until
	 * the worker indicates that it has the logical decoding initialized.
	 */
	ConditionVariablePrepareToSleep(&shared->cv);
	for (;;)
	{
		bool		initialized;

		SpinLockAcquire(&shared->mutex);
		initialized = shared->initialized;
		SpinLockRelease(&shared->mutex);

		if (initialized)
			break;

		ConditionVariableSleep(&shared->cv, WAIT_EVENT_REPACK_WORKER_EXPORT);
	}
	ConditionVariableCancelSleep();
}

/*
 * Wait for the decoding worker to start up, and throw an error if it fails
 * to do so.
 *
 * This is similar to WaitForParallelWorkersToAttach(). The only reliable way
 * to tell a worker that failed to start (fork failure, or an exit before it
 * attached) from one that is merely slow is to check whether it became the
 * sender on the error message queue. If it stopped without attaching, nothing
 * was queued and we report the generic failure ourselves. If it attached, any
 * error it reported is in the queue and is thrown when we process pending
 * messages, either here or later while we wait for it to initialize decoding.
 */
static void
wait_for_repack_decoding_worker(void)
{
	for (;;)
	{
		BgwHandleStatus status;
		shm_mq	   *mq;
		int			rc;
		pid_t		pid;

		/*
		 * This will process any repack messages that are pending and it may
		 * also throw an error propagated from a worker.
		 */
		CHECK_FOR_INTERRUPTS();

		/* If error_mqh is NULL, the worker has exited cleanly */
		if (decoding_worker->error_mqh == NULL)
			break;

		status = GetBackgroundWorkerPid(decoding_worker->handle, &pid);
		if (status == BGWH_STARTED)
		{
			/* Has the worker attached to the error message queue? */
			mq = shm_mq_get_queue(decoding_worker->error_mqh);
			if (shm_mq_get_sender(mq) != NULL)
				break;
		}
		else if (status == BGWH_STOPPED)
		{
			/*
			 * If the worker stopped without attaching to the error message
			 * queue, throw an error. Otherwise, assume it attached and
			 * reported an error before exiting, so mark it attached and let
			 * the next attempt to process pending messages, here or later
			 * while the initial snapshot is set up, throw that error.
			 */
			mq = shm_mq_get_queue(decoding_worker->error_mqh);
			if (shm_mq_get_sender(mq) == NULL)
				ereport(ERROR,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("REPACK decoding worker failed to start"),
						errhint("More details may be available in the server log."));
			break;
		}

		/* Worker neither started or stopped yet, so wait. */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
					   -1, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
	}
}

/*
 * Stop the decoding worker and cleanup the related resources.
 *
 * The worker stops on its own when it knows there is no more work to do, but
 * we need to stop it explicitly at least on ERROR in the launching backend.
 */
static void
stop_repack_decoding_worker(void)
{
	/* Nothing to do if no worker was set up. */
	if (decoding_worker == NULL)
		return;

	/* Terminate the decoding worker, if one is running */
	if (decoding_worker->handle != NULL)
		TerminateBackgroundWorker(decoding_worker->handle);

	/*
	 * The error queue should have already been nulled out during worker
	 * shutdown, but if that didn't happen, do it now.
	 */
	if (decoding_worker->error_mqh != NULL)
	{
		shm_mq_detach(decoding_worker->error_mqh);
		decoding_worker->error_mqh = NULL;
	}

	/*
	 * Cancel any sleep on the condition variable before detaching the shared
	 * memory segment, because the CV lives in that segment.  Otherwise later
	 * cleanup would touch freed memory.
	 */
	ConditionVariableCancelSleep();

	/*
	 * We can't finish the REPACK command until the worker has exited.  This
	 * means, in particular, that we can't respond to interrupts at this
	 * stage.
	 */
	if (decoding_worker->handle != NULL)
	{
		BgwHandleStatus status;

		HOLD_INTERRUPTS();
		status = WaitForBackgroundWorkerShutdown(decoding_worker->handle);
		RESUME_INTERRUPTS();

		if (status == BGWH_POSTMASTER_DIED)
			ereport(FATAL,
					errcode(ERRCODE_ADMIN_SHUTDOWN),
					errmsg("postmaster exited during REPACK command"));
	}

	/*
	 * Detach from the shared memory segment only now that the worker is gone.
	 * The worker attaches to the shared file set after it maps the segment,
	 * so detaching any earlier can destroy the file set under a worker that
	 * is still starting up.
	 */
	if (decoding_worker->seg != NULL)
	{
		dsm_detach(decoding_worker->seg);
		decoding_worker->seg = NULL;
	}

	pfree(decoding_worker);
	decoding_worker = NULL;
}

/* stop_repack_decoding_worker, wrapped as a before_shmem_exit callback */
static void
stop_repack_decoding_worker_cb(int code, Datum arg)
{
	stop_repack_decoding_worker();
}

/*
 * Get snapshot from the decoding worker.
 */
Snapshot
repack_get_snapshot(ChangeContext *chgcxt)
{
	DecodingWorkerShared *shared;
	char		fname[MAXPGPATH];
	BufFile    *file;
	Size		snap_size;
	char	   *snap_space;
	Snapshot	snapshot;

	shared = (DecodingWorkerShared *) dsm_segment_address(decoding_worker->seg);

	/*
	 * For the first snapshot request, the worker needs to initialize the
	 * logical decoding, which usually takes some time. Therefore it makes
	 * sense to prepare for the sleep first. Does it make sense to skip the
	 * preparation on the next requests?
	 */
	ConditionVariablePrepareToSleep(&shared->cv);
	for (;;)
	{
		int			last_exported;

		SpinLockAcquire(&shared->mutex);
		last_exported = shared->last_exported_snapshot;
		SpinLockRelease(&shared->mutex);

		/*
		 * Has the worker exported the file we are waiting for?
		 */
		if (last_exported == chgcxt->cc_file_seq_snapshot)
			break;

		ConditionVariableSleep(&shared->cv, WAIT_EVENT_REPACK_WORKER_EXPORT);
	}
	ConditionVariableCancelSleep();

	/* Read the snapshot from a file. */
	DecodingWorkerFileName(fname, shared->relid, chgcxt->cc_file_seq_snapshot,
						   true);
	file = BufFileOpenFileSet(&shared->sfs.fs, fname, O_RDONLY, false);
	BufFileReadExact(file, &snap_size, sizeof(snap_size));
	snap_space = (char *) palloc(snap_size);
	BufFileReadExact(file, snap_space, snap_size);
	BufFileClose(file);

#ifdef USE_ASSERT_CHECKING
	SpinLockAcquire(&shared->mutex);
	Assert(!shared->snapshot_requested);
	shared->snapshot_requested = false;
	SpinLockRelease(&shared->mutex);
#endif

	/* Restore it. */
	snapshot = RestoreSnapshot(snap_space);
	pfree(snap_space);

	/* Get ready for the next snapshot. */
	chgcxt->cc_file_seq_snapshot++;

	return snapshot;
}

/*
 * Get concurrent changes, up to (and including) the record whose LSN is
 * 'end_of_wal', from the decoding worker, and apply them to the new table. If
 * block range is specified, only apply changes related to that range.
 *
 * If 'request_snapshot' is true, the snapshot built at LSN following the last
 * data change needs to be exported too.
 */
void
repack_process_concurrent_changes(ChangeContext *chgcxt,
								  XLogRecPtr end_of_wal,
								  BlockNumber range_start,
								  BlockNumber range_end,
								  bool request_snapshot, bool done)
{
	DecodingWorkerShared *shared;

	pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
								 PROGRESS_REPACK_PHASE_CATCH_UP);

	/* Ask the worker for the file. */
	shared = (DecodingWorkerShared *) dsm_segment_address(decoding_worker->seg);
	SpinLockAcquire(&shared->mutex);
	shared->lsn_upto = end_of_wal;
	Assert(!shared->snapshot_requested);
	shared->snapshot_requested = request_snapshot;
	shared->done = done;
	SpinLockRelease(&shared->mutex);

	/*
	 * The worker needs to finish processing of the current WAL record. Even
	 * if it's idle, it'll need to close the output file. Thus we're likely to
	 * wait, so prepare for sleep.
	 */
	ConditionVariablePrepareToSleep(&shared->cv);
	for (;;)
	{
		int			last_exported;

		SpinLockAcquire(&shared->mutex);
		last_exported = shared->last_exported_changes;
		SpinLockRelease(&shared->mutex);

		/*
		 * Has the worker exported the file we are waiting for?
		 */
		if (last_exported == chgcxt->cc_file_seq_changes)
			break;

		ConditionVariableSleep(&shared->cv, WAIT_EVENT_REPACK_WORKER_EXPORT);
	}
	ConditionVariableCancelSleep();

#ifdef USE_ASSERT_CHECKING
	/* No file is exported until the worker exports the next one. */
	SpinLockAcquire(&shared->mutex);
	Assert(XLogRecPtrIsInvalid(shared->lsn_upto));
	SpinLockRelease(&shared->mutex);
#endif

	/* Apply the changes to the new table. */
	apply_concurrent_changes(chgcxt, range_start, range_end);

	/* Get ready for the next set of changes. */
	chgcxt->cc_file_seq_changes++;
}

/*
 * Generate worker's file name into 'fname', which must be of size MAXPGPATH.
 * If relations of the same 'relid' happen to be processed at the same time,
 * they must be from different databases and therefore different backends must
 * be involved.
 */
void
DecodingWorkerFileName(char *fname, Oid relid, uint32 seq, bool snapshot)
{
	/* The PID is already present in the fileset name, so we needn't add it */
	if (!snapshot)
		snprintf(fname, MAXPGPATH, "%u-%u", relid, seq);
	else
		snprintf(fname, MAXPGPATH, "%u-%u-snapshot", relid, seq);
}

/*
 * Handle receipt of an interrupt indicating a repack worker message.
 *
 * Note: this is called within a signal handler!  All we can do is set
 * a flag that will cause the next CHECK_FOR_INTERRUPTS() to invoke
 * ProcessRepackMessages().
 */
void
HandleRepackMessageInterrupt(void)
{
	InterruptPending = true;
	RepackMessagePending = true;
	SetLatch(MyLatch);
}

/*
 * Process any queued protocol messages received from the repack worker.
 */
void
ProcessRepackMessages(void)
{
	MemoryContext oldcontext;
	static MemoryContext hpm_context = NULL;

	/*
	 * Nothing to do if we haven't launched the worker yet or have already
	 * terminated it. Stopping the worker detaches the error message queue
	 * before clearing decoding_worker, so also bail out once error_mqh is
	 * gone.
	 */
	if (decoding_worker == NULL || decoding_worker->error_mqh == NULL)
		return;

	/*
	 * This is invoked from ProcessInterrupts(), and since some of the
	 * functions it calls contain CHECK_FOR_INTERRUPTS(), there is a potential
	 * for recursive calls if more signals are received while this runs.  It's
	 * unclear that recursive entry would be safe, and it doesn't seem useful
	 * even if it is safe, so let's block interrupts until done.
	 */
	HOLD_INTERRUPTS();

	/*
	 * Moreover, CurrentMemoryContext might be pointing almost anywhere.  We
	 * don't want to risk leaking data into long-lived contexts, so let's do
	 * our work here in a private context that we can reset on each use.
	 */
	if (hpm_context == NULL)	/* first time through? */
		hpm_context = AllocSetContextCreate(TopMemoryContext,
											"ProcessRepackMessages",
											ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(hpm_context);

	oldcontext = MemoryContextSwitchTo(hpm_context);

	/* OK to process messages.  Reset the flag saying there are more to do. */
	RepackMessagePending = false;

	/*
	 * Read messages from the worker, but stop if the error queue disappears,
	 * which happens when a PqRepackMsg_Terminate is received; or as soon as
	 * no more messages can be read without blocking.  Messages are
	 * infrequent, so no point optimizing stringinfo allocation.
	 */
	while (decoding_worker->error_mqh != NULL)
	{
		shm_mq_result res;
		Size		nbytes;
		void	   *data;
		StringInfoData msg;

		res = shm_mq_receive(decoding_worker->error_mqh, &nbytes,
							 &data, true);
		switch (res)
		{
			case SHM_MQ_SUCCESS:
				initStringInfo(&msg);
				appendBinaryStringInfo(&msg, data, nbytes);
				ProcessRepackMessage(&msg);
				pfree(msg.data);
				break;

			case SHM_MQ_WOULD_BLOCK:
				/* no more messages to process for now */
				goto out;

			case SHM_MQ_DETACHED:

				/*
				 * Normal worker stop sends a Terminate message, causing
				 * ProcessRepackMessage to set error_mqh to NULL, thus exiting
				 * this loop; so this case should never happen.  If it does,
				 * the worker terminated abnormally, so report that.
				 */
				ereport(ERROR,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("lost connection to REPACK decoding worker"),
						errhint("More details may be available in the server log."));
		}
	}

out:
	MemoryContextSwitchTo(oldcontext);

	/* Might as well clear the context on our way out */
	MemoryContextReset(hpm_context);

	RESUME_INTERRUPTS();
}

/*
 * Process a single protocol message received from a repack worker.
 */
static void
ProcessRepackMessage(StringInfo msg)
{
	char		msgtype;

	msgtype = pq_getmsgbyte(msg);

	switch (msgtype)
	{
		case PqMsg_ErrorResponse:
		case PqMsg_NoticeResponse:
			{
				ErrorData	edata;

				/* Parse ErrorResponse or NoticeResponse. */
				pq_parse_errornotice(msg, &edata);

				/* Death of a worker isn't enough justification for suicide. */
				edata.elevel = Min(edata.elevel, ERROR);

				/*
				 * Add a context line to show that this is a message
				 * propagated from the worker.  Otherwise, it can sometimes be
				 * confusing to understand what actually happened.
				 */
				if (edata.context)
					edata.context = psprintf("%s\n%s", edata.context,
											 _("REPACK decoding worker"));
				else
					edata.context = pstrdup(_("REPACK decoding worker"));

				/* Rethrow error or print notice. */
				ThrowErrorData(&edata);

				break;
			}

		case PqRepackMsg_Terminate:

			/*
			 * The worker has completed its work; stop watching its message
			 * queue now for orderly shutdown.
			 */
			shm_mq_detach(decoding_worker->error_mqh);
			decoding_worker->error_mqh = NULL;
			break;

		default:
			elog(ERROR, "unrecognized message type received from decoding worker: %c (message length %d bytes)",
				 msgtype, msg->len);
	}
}
