/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  materialized view support
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "commands/matview.h"
#include "commands/repack.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Oid			transientoid;	/* OID of new heap into which to store */
	/* These fields are filled by transientrel_startup: */
	Relation	transientrel;	/* relation to write to */
	CommandId	output_cid;		/* cmin to insert in output tuples */
	uint32		ti_options;		/* table_tuple_insert performance options */
	BulkInsertState bistate;	/* bulk insert state */
} DR_transientrel;

/*
 * Session-level cache for Partial Refresh plans.
 * We cache the prepared SPI plans for both the row-locking and refresh steps
 * avoiding expensive calls (pg_get_viewdef) and parsing on every execution.
 */
typedef struct MatViewPartialRefreshCache
{
	Oid			matviewOid;		/* Hash Key */

	/* Validation fields */
	Oid			uniqueIndexOid; /* The unique index used for conflict
								 * resolution */
	char	   *whereClauseStr; /* The WHERE clause string used to build the
								 * plans */

	/* The cached plans */
	SPIPlanPtr	lockPlan;		/* SELECT FOR UPDATE */
	SPIPlanPtr	refreshPlan;	/* Fused CTE: Evaluate -> Upsert -> Delete */
}			MatViewPartialRefreshCache;

static HTAB *MatViewRefreshCache = NULL;

static int	matview_maintenance_depth = 0;

static void transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);
static uint64 refresh_matview_datafill(DestReceiver *dest, Query *query,
									   const char *queryString, bool is_create);
static void refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
								   int save_sec_context, char *whereClauseStr,
								   ParamListInfo params);
static uint64 refresh_by_direct_modification(Oid matviewOid, Oid relowner,
											 int save_sec_context, char *whereClauseStr,
											 ParamListInfo params);
static void refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence);
static bool is_usable_unique_index(Relation indexRel);
static void OpenMatViewIncrementalMaintenance(void);
static void CloseMatViewIncrementalMaintenance(void);
static int	matview_execute_spi(const char *command, ParamListInfo params, bool read_only);
static int	matview_execute_spi_plan(SPIPlanPtr plan, ParamListInfo params, bool read_only);
static char *get_matview_view_query(Oid matviewOid);
static void InitMatViewCache(void);

/*
 * SetMatViewPopulatedState
 *		Mark a materialized view as populated, or not.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 */
void
SetMatViewPopulatedState(Relation relation, bool newstate)
{
	Relation	pgrel;
	HeapTuple	tuple;
	Form_pg_class classForm;

	Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * If the state matches, do nothing. This prevents cache invalidation
	 * storms when doing frequent partial refreshes via triggers.
	 */
	if (relation->rd_rel->relispopulated == newstate)
		return;

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 */
	pgrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(relation));

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	if (classForm->relispopulated != newstate)
	{
		classForm->relispopulated = newstate;
		CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);
	}

	heap_freetuple(tuple);
	table_close(pgrel, RowExclusiveLock);

	/*
	 * Advance command counter to make the updated pg_class row locally
	 * visible.
	 */
	CommandCounterIncrement();
}

/*
 * Hook to allow parameters (e.g. $1) in the WHERE clause.
 */
static Node *
refresh_paramref_hook(ParseState *pstate, ParamRef *pref)
{
	ParamListInfo params = (ParamListInfo) pstate->p_ref_hook_state;
	Param	   *param;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = pref->number;
	param->paramtype = UNKNOWNOID;
	param->paramtypmod = -1;
	param->paramcollid = InvalidOid;
	param->location = pref->location;

	if (params && pref->number > 0 && pref->number <= params->numParams)
	{
		Oid			ptype = params->params[pref->number - 1].ptype;

		if (OidIsValid(ptype))
			param->paramtype = ptype;
	}

	return (Node *) param;
}

/*
 * Transform the WHERE clause for REFRESH MATERIALIZED VIEW.
 */
static Node *
transformRefreshWhereClause(Oid relid, Node *whereClause, ParamListInfo params)
{
	ParseState *pstate = make_parsestate(NULL);
	Relation	rel = table_open(relid, NoLock);
	ParseNamespaceItem *nsitem;
	Node	   *result;

	pstate->p_paramref_hook = refresh_paramref_hook;
	pstate->p_ref_hook_state = (void *) params;

	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock, NULL, false, true);
	addNSItemToQuery(pstate, nsitem, false, true, true);

	result = transformExpr(pstate, whereClause, EXPR_KIND_WHERE);
	result = coerce_to_boolean(pstate, result, "WHERE");

	if (contain_volatile_functions(result))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("WHERE clause in REFRESH MATERIALIZED VIEW cannot contain volatile functions")));

	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("WHERE clause in REFRESH MATERIALIZED VIEW cannot contain aggregates")));

	table_close(rel, NoLock);
	free_parsestate(pstate);

	return result;
}

static char *
deparseRefreshWhereClause(Oid relid, Node *whereClause)
{
	return TextDatumGetCString(DirectFunctionCall2(pg_get_expr,
												   CStringGetTextDatum(nodeToString(whereClause)),
												   ObjectIdGetDatum(relid)));
}

/*
 * Helper to execute SPI commands with optional parameters.
 */
static int
matview_execute_spi(const char *command, ParamListInfo params, bool read_only)
{
	if (params && params->numParams > 0)
	{
		Oid		   *argtypes;
		Datum	   *argvalues;
		char	   *nulls;
		int			i;
		int			res;

		argtypes = (Oid *) palloc(params->numParams * sizeof(Oid));
		argvalues = (Datum *) palloc(params->numParams * sizeof(Datum));
		nulls = (char *) palloc(params->numParams * sizeof(char));

		for (i = 0; i < params->numParams; i++)
		{
			ParamExternData *prm = &params->params[i];

			argtypes[i] = prm->ptype;
			argvalues[i] = prm->value;
			nulls[i] = prm->isnull ? 'n' : ' ';
		}

		res = SPI_execute_with_args(command, params->numParams, argtypes,
									argvalues, nulls, read_only, 0);

		pfree(argtypes);
		pfree(argvalues);
		pfree(nulls);

		return res;
	}
	else
	{
		return SPI_exec(command, 0);
	}
}

/*
 * Helper to execute Prepared SPI Plans with optional parameters.
 */
static int
matview_execute_spi_plan(SPIPlanPtr plan, ParamListInfo params, bool read_only)
{
	if (params && params->numParams > 0)
	{
		Datum	   *argvalues;
		char	   *nulls;
		int			i;
		int			res;

		argvalues = (Datum *) palloc(params->numParams * sizeof(Datum));
		nulls = (char *) palloc(params->numParams * sizeof(char));

		for (i = 0; i < params->numParams; i++)
		{
			ParamExternData *prm = &params->params[i];

			argvalues[i] = prm->value;
			nulls[i] = prm->isnull ? 'n' : ' ';
		}

		res = SPI_execute_plan(plan, argvalues, nulls, read_only, 0);

		pfree(argvalues);
		pfree(nulls);

		return res;
	}
	else
	{
		return SPI_execute_plan(plan, NULL, NULL, read_only, 0);
	}
}

/*
 * ExecRefreshMatView -- execute a REFRESH MATERIALIZED VIEW command
 *
 * This is the entry point for REFRESH MATERIALIZED VIEW.  It handles:
 *
 * - WITH NO DATA: effectively like a TRUNCATE.
 * - CONCURRENTLY: diff-based refresh allowing concurrent reads.
 * - WHERE clause: partial refresh of a subset of rows.
 * - Default: full rebuild via heap swap.
 *
 * The statement node's skipData field shows whether WITH NO DATA was used.
 */
ObjectAddress
ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				   ParamListInfo params, QueryCompletion *qc)
{
	Oid			matviewOid;
	LOCKMODE	lockmode;

	/* Determine strength of lock needed. */
	if (stmt->concurrent)
		lockmode = ExclusiveLock;
	else if (stmt->whereClause)
		lockmode = RowExclusiveLock;
	else
		lockmode = AccessExclusiveLock;

	/*
	 * Get a lock until end of transaction.
	 */
	matviewOid = RangeVarGetRelidExtended(stmt->relation,
										  lockmode, 0,
										  RangeVarCallbackMaintainsTable,
										  NULL);

	return RefreshMatViewByOid(matviewOid, false, stmt->skipData,
							   stmt->concurrent, stmt->whereClause,
							   queryString, params, qc);
}

/*
 * RefreshMatViewByOid -- refresh materialized view by OID
 *
 * This refreshes a materialized view using one of three strategies:
 *
 * 1. Partial non-concurrent (WHERE clause, no CONCURRENTLY):
 * Directly modifies the matview in-place using a two-step approach
 * (SELECT FOR UPDATE followed by a CTE upsert/delete).
 * Uses RowExclusiveLock, allowing concurrent reads and concurrent writes
 * to non-overlapping rows. Overlapping writes are serialized by row locks.
 *
 * 2. Concurrent (CONCURRENTLY, with or without WHERE clause):
 * Creates a temporary table with new data, computes a diff against
 * the existing matview, and applies changes. Uses ExclusiveLock,
 * allowing concurrent reads throughout the operation but blocking all
 * concurrent writes.
 *
 * 3. Full rebuild (default, no WHERE, no CONCURRENTLY):
 * Creates a new heap, populates it, and swaps relfilenumbers.
 * Uses AccessExclusiveLock, blocking all concurrent access.
 * The OID of the original materialized view is preserved, so we
 * do not lose GRANT nor references to this materialized view.
 *
 * If skipData is true, this is effectively like a TRUNCATE; otherwise it is
 * like a TRUNCATE followed by an INSERT using the SELECT statement associated
 * with the materialized view.
 *
 * For full rebuild, indexes are rebuilt too, via REINDEX.  Since we are
 * effectively bulk-loading the new heap, it's better to create the indexes
 * afterwards than to fill them incrementally while we load.
 *
 * The matview's "populated" state is changed based on whether the contents
 * reflect the result set of the materialized view's query.
 *
 * This is also used to populate the materialized view created by CREATE
 * MATERIALIZED VIEW command.
 */
ObjectAddress
RefreshMatViewByOid(Oid matviewOid, bool is_create, bool skipData,
					bool concurrent, Node *whereClause,
					const char *queryString, ParamListInfo params,
					QueryCompletion *qc)
{
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;
	Oid			relowner;
	uint64		processed = 0;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	ObjectAddress address;
	Node	   *qual = NULL;
	char	   *qual_str = NULL;

	matviewRel = table_open(matviewOid, NoLock);
	relowner = matviewRel->rd_rel->relowner;

	/*
	 * Switch to the owner's userid, so that any functions are run as that
	 * user.  Also lock down security-restricted operations and arrange to
	 * make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/* Make sure it is a materialized view. */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/* Check that CONCURRENTLY is not specified if not populated. */
	if (concurrent && !RelationIsPopulated(matviewRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CONCURRENTLY cannot be used when the materialized view is not populated")));

	if (whereClause && !RelationIsPopulated(matviewRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("WHERE clause cannot be used when the materialized view is not populated")));

	if (concurrent && skipData)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s options %s and %s cannot be used together",
						"REFRESH", "CONCURRENTLY", "WITH NO DATA")));

	/*
	 * Check that everything is correct for a refresh. Problems at this point
	 * are internal errors, so elog is sufficient.
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	if (whereClause)
	{
		qual = transformRefreshWhereClause(matviewOid, whereClause, params);
		qual_str = deparseRefreshWhereClause(matviewOid, qual);
	}

	/*
	 * Check that there is a unique index with no WHERE clause on one or more
	 * columns of the materialized view if CONCURRENTLY is specified.
	 */
	if (concurrent)
	{
		List	   *indexoidlist = RelationGetIndexList(matviewRel);
		ListCell   *indexoidscan;
		bool		hasUniqueIndex = false;

		Assert(!is_create);

		foreach(indexoidscan, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(indexoidscan);
			Relation	indexRel;

			indexRel = index_open(indexoid, AccessShareLock);
			hasUniqueIndex = is_usable_unique_index(indexRel);
			index_close(indexRel, AccessShareLock);
			if (hasUniqueIndex)
				break;
		}

		list_free(indexoidlist);

		if (!hasUniqueIndex)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot refresh materialized view \"%s\" concurrently",
							quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
													   RelationGetRelationName(matviewRel))),
					 errhint("Create a unique index with no WHERE clause on one or more columns of the materialized view.")));
	}

	/*
	 * The stored query was rewritten at the time of the MV definition, but
	 * has not been scribbled on by the planner.
	 */
	dataQuery = linitial_node(Query, actions);

	/*
	 * Check for active uses of the relation in the current transaction, such
	 * as open scans.
	 *
	 * NB: We count on this to protect us against problems with refreshing the
	 * data using TABLE_INSERT_FROZEN.
	 */
	CheckTableNotInUse(matviewRel,
					   is_create ? "CREATE MATERIALIZED VIEW" :
					   "REFRESH MATERIALIZED VIEW");

	/*
	 * Tentatively mark the matview as populated or not, if its state is
	 * changing (this will roll back if we fail later).
	 */
	if (RelationIsPopulated(matviewRel) != !skipData)
		SetMatViewPopulatedState(matviewRel, !skipData);

	/*
	 * STRATEGY 1: PARTIAL NON-CONCURRENT
	 */
	if (qual && !concurrent && !skipData)
	{
		processed = refresh_by_direct_modification(matviewOid, relowner,
												   save_sec_context, qual_str,
												   params);
	}

	/*
	 * STRATEGY 2: CONCURRENT (PARTIAL or FULL)
	 */
	else if (concurrent)
	{
		Oid			tableSpace;
		char		relpersistence;
		Oid			OIDNewHeap;
		int			old_depth = matview_maintenance_depth;

		tableSpace = GetDefaultTablespace(RELPERSISTENCE_TEMP, false);
		relpersistence = RELPERSISTENCE_TEMP;

		/*
		 * Create the transient table that will receive the regenerated data.
		 * Lock it against access by any other process until commit (by which
		 * time it will be gone).
		 */
		OIDNewHeap = make_new_heap(matviewOid, tableSpace,
								   matviewRel->rd_rel->relam,
								   relpersistence, ExclusiveLock);
		Assert(CheckRelationOidLockedByMe(OIDNewHeap, AccessExclusiveLock, false));

		/* Generate the data, if wanted. */
		if (!skipData)
		{
			if (qual_str)
			{
				StringInfoData buf;
				char	   *view_sql = get_matview_view_query(matviewOid);
				char	   *transient_name;
				Relation	transientRel = table_open(OIDNewHeap, NoLock);

				transient_name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(transientRel)),
															RelationGetRelationName(transientRel));
				table_close(transientRel, NoLock);

				/*
				 * Init buffer before SPI connection to avoid double free
				 * issues on context destroy
				 */
				initStringInfo(&buf);
				appendStringInfo(&buf, "INSERT INTO %s SELECT * FROM (%s) _mv_q WHERE %s",
								 transient_name, view_sql, qual_str);

				SPI_connect();
				if (matview_execute_spi(buf.data, params, false) != SPI_OK_INSERT)
					elog(ERROR, "SPI_exec failed: %s", buf.data);
				processed = SPI_processed;
				SPI_finish();
				pfree(view_sql);
				pfree(transient_name);
				pfree(buf.data);
			}
			else
			{
				DestReceiver *dest;

				dest = CreateTransientRelDestReceiver(OIDNewHeap);
				processed = refresh_matview_datafill(dest, dataQuery, queryString, is_create);
			}
		}

		PG_TRY();
		{
			refresh_by_match_merge(matviewOid, OIDNewHeap, relowner,
								   save_sec_context, qual_str, params);
		}
		PG_CATCH();
		{
			matview_maintenance_depth = old_depth;
			PG_RE_THROW();
		}
		PG_END_TRY();

		Assert(matview_maintenance_depth == old_depth);
	}

	/*
	 * STRATEGY 3: FULL REBUILD
	 */
	else
	{
		Oid			tableSpace;
		char		relpersistence;
		Oid			OIDNewHeap;

		tableSpace = matviewRel->rd_rel->reltablespace;
		relpersistence = matviewRel->rd_rel->relpersistence;

		OIDNewHeap = make_new_heap(matviewOid, tableSpace,
								   matviewRel->rd_rel->relam,
								   relpersistence, AccessExclusiveLock);

		if (!skipData)
		{
			DestReceiver *dest;

			dest = CreateTransientRelDestReceiver(OIDNewHeap);
			processed = refresh_matview_datafill(dest, dataQuery, queryString, is_create);
		}

		refresh_by_heap_swap(matviewOid, OIDNewHeap, relpersistence);

		/*
		 * Inform cumulative stats system about our activity: basically, we
		 * truncated the matview and inserted some new data.  (The concurrent
		 * code path above doesn't need to worry about this because the
		 * inserts and deletes it issues get counted by lower-level code.)
		 */
		pgstat_count_truncate(matviewRel);
		if (!skipData)
			pgstat_count_heap_insert(matviewRel, processed);
	}

	table_close(matviewRel, NoLock);

	/* Roll back any GUC changes */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	ObjectAddressSet(address, RelationRelationId, matviewOid);

	/*
	 * Save the rowcount so that pg_stat_statements can track the total number
	 * of rows processed by REFRESH MATERIALIZED VIEW command. Note that we
	 * still don't display the rowcount in the command completion tag output,
	 * i.e., the display_rowcount flag of CMDTAG_REFRESH_MATERIALIZED_VIEW
	 * command tag is left false in cmdtaglist.h. Otherwise, the change of
	 * completion tag output might break applications using it.
	 *
	 * When called from CREATE MATERIALIZED VIEW command, the rowcount is
	 * displayed with the command tag CMDTAG_SELECT.
	 */
	if (qc)
		SetQueryCompletion(qc,
						   is_create ? CMDTAG_SELECT : CMDTAG_REFRESH_MATERIALIZED_VIEW,
						   processed);

	return address;
}

/*
 * refresh_matview_datafill
 *
 * Execute the given query, sending result rows to "dest" (which will
 * insert them into the target matview).
 *
 * Returns number of rows inserted.
 */
static uint64
refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString, bool is_create)
{
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	Query	   *copied_query;
	uint64		processed;

	/* Lock and rewrite, using a copy to preserve the original query. */
	copied_query = copyObject(query);
	AcquireRewriteLocks(copied_query, true, false);
	rewritten = QueryRewrite(copied_query);

	/* SELECT should never rewrite to more or less than one SELECT query */
	if (list_length(rewritten) != 1)
		elog(ERROR, "unexpected rewrite result for %s",
			 is_create ? "CREATE MATERIALIZED VIEW " : "REFRESH MATERIALIZED VIEW");
	query = (Query *) linitial(rewritten);

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/* Plan the query which will generate data for the refresh. */
	plan = pg_plan_query(query, queryString, CURSOR_OPT_PARALLEL_OK, NULL, NULL);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.  (This could only matter if
	 * the planner executed an allegedly-stable function that changed the
	 * database contents, but let's do it anyway to be safe.)
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* Create a QueryDesc, redirecting output to our tuple receiver */
	queryDesc = CreateQueryDesc(plan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, NULL, NULL, 0);

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, 0);

	/* run the plan */
	ExecutorRun(queryDesc, ForwardScanDirection, 0);

	processed = queryDesc->estate->es_processed;

	/* and clean up */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	return processed;
}

DestReceiver *
CreateTransientRelDestReceiver(Oid transientoid)
{
	DR_transientrel *self = palloc0_object(DR_transientrel);

	self->pub.receiveSlot = transientrel_receive;
	self->pub.rStartup = transientrel_startup;
	self->pub.rShutdown = transientrel_shutdown;
	self->pub.rDestroy = transientrel_destroy;
	self->pub.mydest = DestTransientRel;
	self->transientoid = transientoid;

	return (DestReceiver *) self;
}

/*
 * transientrel_startup --- executor startup
 */
static void
transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Relation	transientrel;

	transientrel = table_open(myState->transientoid, NoLock);

	/*
	 * Fill private fields of myState for use by later routines
	 */
	myState->transientrel = transientrel;
	myState->output_cid = GetCurrentCommandId(true);
	myState->ti_options = TABLE_INSERT_SKIP_FSM | TABLE_INSERT_FROZEN;
	myState->bistate = GetBulkInsertState();

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 */
	Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- receive one tuple
 */
static bool
transientrel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	/*
	 * Note that the input slot might not be of the type of the target
	 * relation. That's supported by table_tuple_insert(), but slightly less
	 * efficient than inserting with the right slot - but the alternative
	 * would be to copy into a slot of the right type, which would not be
	 * cheap either. This also doesn't allow accessing per-AM data (say a
	 * tuple's xmin), but since we don't do that here...
	 */

	table_tuple_insert(myState->transientrel,
					   slot,
					   myState->output_cid,
					   myState->ti_options,
					   myState->bistate);

	/* We know this is a newly created relation, so there are no indexes */

	return true;
}

/*
 * transientrel_shutdown --- executor end
 */
static void
transientrel_shutdown(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	FreeBulkInsertState(myState->bistate);

	table_finish_bulk_insert(myState->transientrel, myState->ti_options);

	/* close transientrel, but keep lock until commit */
	table_close(myState->transientrel, NoLock);
	myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- release DestReceiver object
 */
static void
transientrel_destroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * get_matview_view_query
 *
 * Retrieve the SQL definition of a materialized view's underlying query.
 * Returns the query text with trailing semicolons and whitespace removed.
 */
static char *
get_matview_view_query(Oid matviewOid)
{
	char	   *view_sql;

	view_sql = TextDatumGetCString(DirectFunctionCall2(pg_get_viewdef,
													   ObjectIdGetDatum(matviewOid),
													   BoolGetDatum(false)));
	if (view_sql)
	{
		int			len = strlen(view_sql);

		while (len > 0 && (view_sql[len - 1] == ';' || isspace((unsigned char) view_sql[len - 1])))
			view_sql[--len] = '\0';
	}
	return view_sql;
}

static void
InitMatViewCache(void)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(MatViewPartialRefreshCache);
	ctl.hcxt = CacheMemoryContext;

	MatViewRefreshCache = hash_create("MatView Partial Refresh Cache",
									  16,
									  &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * refresh_by_direct_modification
 *
 * This modifies the materialized view in-place without creating a temporary
 * heap or swapping relfilenumbers.  It requires a usable unique index on the
 * matview for conflict resolution.
 *
 * Concurrency is handled in two steps, each executed as a separate SPI
 * statement:
 *
 * 1. Lock existing rows matching the WHERE clause via SELECT FOR UPDATE.
 *    This serializes concurrent partial refreshes that touch overlapping
 *    rows while allowing non-overlapping refreshes to proceed in parallel.
 *
 * 2. Execute a single CTE that evaluates the underlying query, upserts
 *    the results into the matview, and deletes rows that no longer match
 *    the predicate (via anti-join against the fresh query output).
 *
 * To avoid rebuilding the SQL and re-preparing the SPI plans on every call,
 * we cache both plans in a session-level hash table keyed by matview OID.
 *
 * Returns the number of rows processed by the refresh CTE.
 */
static uint64
refresh_by_direct_modification(Oid matviewOid, Oid relowner,
							   int save_sec_context, char *whereClauseStr,
							   ParamListInfo params)
{
	Relation	matviewRel;
	Oid			uniqueIndexOid = InvalidOid;
	List	   *indexoidlist;
	ListCell   *lc;
	MatViewPartialRefreshCache *cacheEntry;
	bool		found;
	uint64		result_processed = 0;

	matviewRel = table_open(matviewOid, NoLock);

	/* Find a usable unique index, preferring the primary key. */
	indexoidlist = RelationGetIndexList(matviewRel);
	foreach(lc, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	indexRel;
		bool		usable;
		bool		is_pk;

		indexRel = index_open(indexoid, AccessShareLock);
		usable = is_usable_unique_index(indexRel);
		is_pk = indexRel->rd_index->indisprimary;
		index_close(indexRel, AccessShareLock);

		if (usable)
		{
			if (is_pk)
			{
				uniqueIndexOid = indexoid;
				break;
			}
			if (!OidIsValid(uniqueIndexOid))
				uniqueIndexOid = indexoid;
		}
	}
	list_free(indexoidlist);

	if (!OidIsValid(uniqueIndexOid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot perform partial refresh on materialized view \"%s\"",
						RelationGetRelationName(matviewRel)),
				 errdetail("Partial refresh requires a usable unique index to perform an UPSERT operation.")));

	/* Look up or create a plan cache entry for this matview. */
	if (!MatViewRefreshCache)
		InitMatViewCache();

	cacheEntry = (MatViewPartialRefreshCache *) hash_search(MatViewRefreshCache,
															&matviewOid,
															HASH_ENTER,
															&found);

	/*
	 * We have a cache hit ONLY if the entry exists, the unique index matches,
	 * and the WHERE clause string perfectly matches.  We also ensure
	 * whereClauseStr is not NULL to prevent a strcmp segfault if a previous
	 * compilation failed midway.
	 */
	if (found &&
		cacheEntry->uniqueIndexOid == uniqueIndexOid &&
		cacheEntry->whereClauseStr != NULL &&
		whereClauseStr != NULL &&
		strcmp(cacheEntry->whereClauseStr, whereClauseStr) == 0)
	{
		/* Cache is valid.  Do nothing. */
	}
	else
	{
		if (found)
		{
			/* Index or WHERE clause changed; discard stale plans. */
			if (cacheEntry->lockPlan)
				SPI_freeplan(cacheEntry->lockPlan);
			if (cacheEntry->refreshPlan)
				SPI_freeplan(cacheEntry->refreshPlan);
			if (cacheEntry->whereClauseStr)
				pfree(cacheEntry->whereClauseStr);
		}

		cacheEntry->lockPlan = NULL;
		cacheEntry->refreshPlan = NULL;
		cacheEntry->whereClauseStr = NULL;
	}

	OpenMatViewIncrementalMaintenance();

	SPI_connect();

	/* Prepare plans if we don't have valid cached ones. */
	if (cacheEntry->lockPlan == NULL || cacheEntry->refreshPlan == NULL)
	{
		StringInfoData buf;
		char	   *view_sql;
		char	   *matview_name;
		const char *matview_alias;
		Oid		   *argtypes = NULL;
		int			nargs = 0;
		Relation	indexRel;
		Form_pg_index indexStruct;
		TupleDesc	tupdesc = matviewRel->rd_att;
		StringInfoData conflict_cols;
		StringInfoData set_clause;
		StringInfoData join_clause;
		bool		first;
		bool		has_non_key_cols = false;
		int			i;
		MemoryContext oldcxt;

		matview_name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
												  RelationGetRelationName(matviewRel));
		matview_alias = quote_identifier(RelationGetRelationName(matviewRel));
		view_sql = get_matview_view_query(matviewOid);

		if (params && params->numParams > 0)
		{
			nargs = params->numParams;
			argtypes = (Oid *) palloc(nargs * sizeof(Oid));
			for (i = 0; i < nargs; i++)
				argtypes[i] = params->params[i].ptype;
		}

		/*
		 * Prepare the row-locking statement.  This acquires FOR UPDATE locks
		 * on matview rows matching the WHERE clause to serialize concurrent
		 * partial refreshes on overlapping rows.
		 */
		initStringInfo(&buf);
		appendStringInfo(&buf, "SELECT 1 FROM %s mv WHERE (%s) FOR UPDATE",
						 matview_name, whereClauseStr);

		cacheEntry->lockPlan = SPI_prepare(buf.data, nargs, argtypes);
		if (cacheEntry->lockPlan == NULL)
			elog(ERROR, "SPI_prepare failed for lock acquisition: %s", buf.data);
		SPI_keepplan(cacheEntry->lockPlan);

		/*
		 * Build the refresh CTE: evaluate the underlying query with the WHERE
		 * predicate, upsert the results, and delete matview rows that no
		 * longer appear in the query output.
		 */
		indexRel = index_open(uniqueIndexOid, AccessShareLock);
		indexStruct = indexRel->rd_index;

		initStringInfo(&conflict_cols);
		initStringInfo(&set_clause);
		initStringInfo(&join_clause);

		/* Build the ON CONFLICT column list and anti-join condition. */
		first = true;
		for (i = 0; i < indexStruct->indnkeyatts; i++)
		{
			int			attnum = indexStruct->indkey.values[i];
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
			const char *quoted;

			quoted = quote_identifier(NameStr(attr->attname));

			if (!first)
			{
				appendStringInfoString(&conflict_cols, ", ");
				appendStringInfoString(&join_clause, " AND ");
			}
			first = false;

			appendStringInfoString(&conflict_cols, quoted);
			appendStringInfo(&join_clause,
							 "nd.%s IS NOT DISTINCT FROM mv.%s",
							 quoted, quoted);
		}

		/* Build the DO UPDATE SET clause for non-key columns. */
		first = true;
		for (i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
			const char *quoted;
			bool		is_key = false;
			int			j;

			if (attr->attisdropped)
				continue;

			for (j = 0; j < indexStruct->indnkeyatts; j++)
			{
				if (indexStruct->indkey.values[j] == (i + 1))
				{
					is_key = true;
					break;
				}
			}

			if (is_key)
				continue;

			if (!first)
				appendStringInfoString(&set_clause, ", ");
			first = false;
			has_non_key_cols = true;

			quoted = quote_identifier(NameStr(attr->attname));
			appendStringInfo(&set_clause, "%s = EXCLUDED.%s", quoted, quoted);
		}

		index_close(indexRel, NoLock);

		resetStringInfo(&buf);

		if (has_non_key_cols)
		{
			appendStringInfo(&buf,
							 "WITH new_data AS MATERIALIZED ( "
							 "  SELECT * FROM (%s) %s WHERE (%s) "
							 "), "
							 "upsert AS ( "
							 "  INSERT INTO %s SELECT * FROM new_data "
							 "  ON CONFLICT (%s) DO UPDATE SET %s "
							 ") "
							 "DELETE FROM %s mv WHERE (%s) AND NOT EXISTS ( "
							 "  SELECT 1 FROM new_data nd WHERE %s"
							 ")",
							 view_sql, matview_alias, whereClauseStr,
							 matview_name, conflict_cols.data, set_clause.data,
							 matview_name, whereClauseStr, join_clause.data);
		}
		else
		{
			appendStringInfo(&buf,
							 "WITH new_data AS MATERIALIZED ( "
							 "  SELECT * FROM (%s) %s WHERE (%s) "
							 "), "
							 "upsert AS ( "
							 "  INSERT INTO %s SELECT * FROM new_data "
							 "  ON CONFLICT (%s) DO NOTHING "
							 ") "
							 "DELETE FROM %s mv WHERE (%s) AND NOT EXISTS ( "
							 "  SELECT 1 FROM new_data nd WHERE %s"
							 ")",
							 view_sql, matview_alias, whereClauseStr,
							 matview_name, conflict_cols.data,
							 matview_name, whereClauseStr, join_clause.data);
		}

		cacheEntry->refreshPlan = SPI_prepare(buf.data, nargs, argtypes);
		if (cacheEntry->refreshPlan == NULL)
			elog(ERROR, "SPI_prepare failed for refresh CTE: %s", buf.data);
		SPI_keepplan(cacheEntry->refreshPlan);

		/* Save cache metadata in a long-lived context. */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		cacheEntry->uniqueIndexOid = uniqueIndexOid;
		cacheEntry->whereClauseStr = pstrdup(whereClauseStr);
		MemoryContextSwitchTo(oldcxt);

		pfree(matview_name);
		pfree(view_sql);
		pfree(buf.data);
		pfree(conflict_cols.data);
		pfree(set_clause.data);
		pfree(join_clause.data);
		if (argtypes != NULL)
			pfree(argtypes);
	}


	/* Execute: lock matching rows, then run the refresh CTE. */
	if (matview_execute_spi_plan(cacheEntry->lockPlan, params, false) < 0)
		elog(ERROR, "SPI_execute_plan failed during lock acquisition");

	if (matview_execute_spi_plan(cacheEntry->refreshPlan, params, false) < 0)
		elog(ERROR, "SPI_execute_plan failed during refresh");

	result_processed = SPI_processed;

	SPI_finish();
	CloseMatViewIncrementalMaintenance();
	table_close(matviewRel, NoLock);

	return result_processed;
}

/*
 * refresh_by_match_merge
 *
 * Refresh a materialized view with transactional semantics, while allowing
 * concurrent reads.
 *
 * This is called after a new version of the data has been created in a
 * temporary table.  It performs a full outer join against the old version of
 * the data, producing "diff" results.  This join cannot work if there are any
 * duplicated rows in either the old or new versions, in the sense that every
 * column would compare as equal between the two rows.  It does work correctly
 * in the face of rows which have at least one NULL value, with all non-NULL
 * columns equal.  The behavior of NULLs on equality tests and on UNIQUE
 * indexes turns out to be quite convenient here; the tests we need to make
 * are consistent with default behavior.  If there is at least one UNIQUE
 * index on the materialized view, we have exactly the guarantee we need.
 *
 * If whereClauseStr is provided, only rows matching the WHERE condition
 * in the existing matview are considered for the diff operation, enabling
 * partial concurrent refresh.
 *
 * The temporary table used to hold the diff results contains just the TID of
 * the old record (if matched) and the ROW from the new table as a single
 * column of complex record type (if matched).
 *
 * Once we have the diff table, we perform set-based DELETE and INSERT
 * operations against the materialized view, and discard both temporary
 * tables.
 *
 * Everything from the generation of the new data to applying the differences
 * takes place under cover of an ExclusiveLock, since it seems as though we
 * would want to prohibit not only concurrent REFRESH operations, but also
 * incremental maintenance.  It also doesn't seem reasonable or safe to allow
 * SELECT FOR UPDATE or SELECT FOR SHARE on rows being updated or deleted by
 * this command.
 */
static void
refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
					   int save_sec_context, char *whereClauseStr,
					   ParamListInfo params)
{
	StringInfoData querybuf;
	Relation	matviewRel;
	Relation	tempRel;
	char	   *matviewname;
	char	   *tempname;
	char	   *diffname;
	char	   *temprelname;
	char	   *diffrelname;
	char	   *nsp;
	TupleDesc	tupdesc;
	bool		foundUniqueIndex;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int16		relnatts;
	Oid		   *opUsedForQual;

	initStringInfo(&querybuf);
	matviewRel = table_open(matviewOid, NoLock);
	matviewname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
											 RelationGetRelationName(matviewRel));
	tempRel = table_open(tempOid, NoLock);

	/*
	 * Build qualified names of the temporary table and the diff table.  The
	 * only difference between them is the "_2" suffix on the diff table name.
	 */
	nsp = get_namespace_name(RelationGetNamespace(tempRel));
	temprelname = RelationGetRelationName(tempRel);
	diffrelname = psprintf("%s_2", temprelname);

	tempname = quote_qualified_identifier(nsp, temprelname);
	diffname = quote_qualified_identifier(nsp, diffrelname);

	relnatts = RelationGetNumberOfAttributes(matviewRel);

	/* Open SPI context. */
	SPI_connect();

	/* Analyze the temp table with the new contents. */
	appendStringInfo(&querybuf, "ANALYZE %s", tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * We need to ensure that there are not duplicate rows without NULLs in
	 * the new data set before we can count on the "diff" results.  Check for
	 * that in a way that allows showing the first duplicated row found.  Even
	 * after we pass this test, a unique index on the materialized view may
	 * find a duplicate key problem.
	 *
	 * Note: here and below, we use "tablename.*::tablerowtype" as a hack to
	 * keep ".*" from being expanded into multiple columns in a SELECT list.
	 * Compare ruleutils.c's get_variable().
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "SELECT newdata.*::%s FROM %s newdata "
					 "WHERE newdata.* IS NOT NULL AND EXISTS "
					 "(SELECT 1 FROM %s newdata2 WHERE newdata2.* IS NOT NULL "
					 "AND newdata2.* OPERATOR(pg_catalog.*=) newdata.* "
					 "AND newdata2.ctid OPERATOR(pg_catalog.<>) "
					 "newdata.ctid)",
					 tempname, tempname, tempname);
	if (SPI_execute(querybuf.data, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	if (SPI_processed > 0)
	{
		/*
		 * Note that this ereport() is returning data to the user.  Generally,
		 * we would want to make sure that the user has been granted access to
		 * this data.  However, REFRESH MAT VIEW is only able to be run by the
		 * owner of the mat view (or a superuser) and therefore there is no
		 * need to check for access to data in the mat view.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("new data for materialized view \"%s\" contains duplicate rows without any null columns",
						RelationGetRelationName(matviewRel)),
				 errdetail("Row: %s",
						   SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1))));
	}

	/*
	 * Create the temporary "diff" table.
	 *
	 * Temporarily switch out of the SECURITY_RESTRICTED_OPERATION context,
	 * because you cannot create temp tables in SRO context.  For extra
	 * paranoia, add the composite type column only after switching back to
	 * SRO context.
	 */
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "CREATE TEMP TABLE %s (tid pg_catalog.tid)",
					 diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "ALTER TABLE %s ADD COLUMN newdata %s",
					 diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Start building the query for populating the diff table. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s "
					 "SELECT mv.ctid AS tid, newdata.*::%s AS newdata "
					 "FROM ",
					 diffname, tempname);

	if (whereClauseStr)
		appendStringInfo(&querybuf, "(SELECT ctid, * FROM %s WHERE %s) mv", matviewname, whereClauseStr);
	else
		appendStringInfo(&querybuf, "%s mv", matviewname);

	appendStringInfo(&querybuf, " FULL JOIN %s newdata ON (", tempname);

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache.  We will test for equality on all
	 * columns present in all unique indexes which only reference columns and
	 * include all rows.
	 */
	tupdesc = matviewRel->rd_att;
	opUsedForQual = palloc0_array(Oid, relnatts);
	foundUniqueIndex = false;

	indexoidlist = RelationGetIndexList(matviewRel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;

		indexRel = index_open(indexoid, RowExclusiveLock);
		if (is_usable_unique_index(indexRel))
		{
			Form_pg_index indexStruct = indexRel->rd_index;
			int			indnkeyatts = indexStruct->indnkeyatts;
			oidvector  *indclass;
			Datum		indclassDatum;
			int			i;

			/* Must get indclass the hard way. */
			indclassDatum = SysCacheGetAttrNotNull(INDEXRELID,
												   indexRel->rd_indextuple,
												   Anum_pg_index_indclass);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/* Add quals for all columns from this index. */
			for (i = 0; i < indnkeyatts; i++)
			{
				int			attnum = indexStruct->indkey.values[i];
				Oid			opclass = indclass->values[i];
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
				Oid			attrtype = attr->atttypid;
				HeapTuple	cla_ht;
				Form_pg_opclass cla_tup;
				Oid			opfamily;
				Oid			opcintype;
				Oid			op;
				const char *leftop;
				const char *rightop;

				/*
				 * Identify the equality operator associated with this index
				 * column.  First we need to look up the column's opclass.
				 */
				cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
				if (!HeapTupleIsValid(cla_ht))
					elog(ERROR, "cache lookup failed for opclass %u", opclass);
				cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
				opfamily = cla_tup->opcfamily;
				opcintype = cla_tup->opcintype;
				ReleaseSysCache(cla_ht);

				op = get_opfamily_member_for_cmptype(opfamily, opcintype, opcintype, COMPARE_EQ);
				if (!OidIsValid(op))
					elog(ERROR, "missing equality operator for (%u,%u) in opfamily %u",
						 opcintype, opcintype, opfamily);

				/*
				 * If we find the same column with the same equality semantics
				 * in more than one index, we only need to emit the equality
				 * clause once.
				 *
				 * Since we only remember the last equality operator, this
				 * code could be fooled into emitting duplicate clauses given
				 * multiple indexes with several different opclasses ... but
				 * that's so unlikely it doesn't seem worth spending extra
				 * code to avoid.
				 */
				if (opUsedForQual[attnum - 1] == op)
					continue;
				opUsedForQual[attnum - 1] = op;

				/*
				 * Actually add the qual, ANDed with any others.
				 */
				if (foundUniqueIndex)
					appendStringInfoString(&querybuf, " AND ");

				leftop = quote_qualified_identifier("newdata",
													NameStr(attr->attname));
				rightop = quote_qualified_identifier("mv",
													 NameStr(attr->attname));

				generate_operator_clause(&querybuf,
										 leftop, attrtype,
										 op,
										 rightop, attrtype);

				foundUniqueIndex = true;
			}
		}

		/* Keep the locks, since we're about to run DML which needs them. */
		index_close(indexRel, NoLock);
	}

	list_free(indexoidlist);

	/*
	 * There must be at least one usable unique index on the matview.
	 *
	 * ExecRefreshMatView() checks that after taking the exclusive lock on the
	 * matview. So at least one unique index is guaranteed to exist here
	 * because the lock is still being held.  (One known exception is if a
	 * function called as part of refreshing the matview drops the index.
	 * That's a pretty silly thing to do.)
	 */
	if (!foundUniqueIndex)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("could not find suitable unique index on materialized view \"%s\"",
					   RelationGetRelationName(matviewRel)));

	if (whereClauseStr)
	{
		StringInfoData cols;
		int			i;
		bool		first = true;

		initStringInfo(&cols);
		for (i = 0; i < relnatts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				continue;
			if (!first)
				appendStringInfoString(&cols, ", ");
			first = false;
			appendStringInfo(&cols, "mv.%s", quote_qualified_identifier(NULL, NameStr(attr->attname)));
		}

		appendStringInfo(&querybuf,
						 " AND newdata.* OPERATOR(pg_catalog.*=) ROW(%s)) "
						 "WHERE newdata.* IS NULL OR mv.ctid IS NULL "
						 "ORDER BY tid",
						 cols.data);
		pfree(cols.data);
	}
	else
	{
		appendStringInfoString(&querybuf,
							   " AND newdata.* OPERATOR(pg_catalog.*=) mv.*) "
							   "WHERE newdata.* IS NULL OR mv.* IS NULL "
							   "ORDER BY tid");
	}

	/* Populate the temporary "diff" table. */
	if (matview_execute_spi(querybuf.data, params, false) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * We have no further use for data from the "full-data" temp table, but we
	 * must keep it around because its type is referenced from the diff table.
	 */

	/* Analyze the diff table. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "ANALYZE %s", diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	OpenMatViewIncrementalMaintenance();

	/* Deletes must come before inserts; do them first. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "DELETE FROM %s mv WHERE ctid OPERATOR(pg_catalog.=) ANY "
					 "(SELECT diff.tid FROM %s diff "
					 "WHERE diff.tid IS NOT NULL "
					 "AND diff.newdata IS NULL)",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_DELETE)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Inserts go last. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s SELECT (diff.newdata).* "
					 "FROM %s diff WHERE tid IS NULL",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* We're done maintaining the materialized view. */
	CloseMatViewIncrementalMaintenance();
	table_close(tempRel, NoLock);
	table_close(matviewRel, NoLock);

	/* Clean up temp tables. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "DROP TABLE %s, %s", diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Close SPI context. */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}

/*
 * Swap the physical files of the target and transient tables, then rebuild
 * the target's indexes and throw away the transient table.  Security context
 * swapping is handled by the called function, so it is not needed here.
 */
static void
refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence)
{
	finish_heap_swap(matviewOid, OIDNewHeap, false, false, true, true,
					 true,		/* reindex */
					 RecentXmin, ReadNextMultiXactId(), relpersistence);
}

/*
 * Check whether the specified index is usable for refresh_by_match_merge
 *  or refresh_by_direct_modification.
 */
static bool
is_usable_unique_index(Relation indexRel)
{
	Form_pg_index indexStruct = indexRel->rd_index;

	/*
	 * Must be unique, valid, immediate, non-partial, and be defined over
	 * plain user columns (not expressions).
	 */
	if (indexStruct->indisunique &&
		indexStruct->indimmediate &&
		indexStruct->indisvalid &&
		RelationGetIndexPredicate(indexRel) == NIL &&
		indexStruct->indnatts > 0)
	{
		/*
		 * The point of groveling through the index columns individually is to
		 * reject both index expressions and system columns.  Currently,
		 * matviews couldn't have OID columns so there's no way to create an
		 * index on a system column; but maybe someday that wouldn't be true,
		 * so let's be safe.
		 */
		int			numatts = indexStruct->indnatts;
		int			i;

		for (i = 0; i < numatts; i++)
		{
			int			attnum = indexStruct->indkey.values[i];

			if (attnum <= 0)
				return false;
		}
		return true;
	}
	return false;
}


/*
 * This should be used to test whether the backend is in a context where it is
 * OK to allow DML statements to modify materialized views.  We only want to
 * allow that for internal code driven by the materialized view definition,
 * not for arbitrary user-supplied code.
 *
 * While the function names reflect the fact that their main intended use is
 * incremental maintenance of materialized views (in response to changes to
 * the data in referenced relations), they are currently used to allow:
 *
 * - REFRESH CONCURRENTLY without blocking concurrent reads.
 * - REFRESH ... WHERE ... which modifies the matview in-place.
 */
bool
MatViewIncrementalMaintenanceIsEnabled(void)
{
	return matview_maintenance_depth > 0;
}

static void
OpenMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth++;
}

static void
CloseMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth--;
	Assert(matview_maintenance_depth >= 0);
}
