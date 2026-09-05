/*-------------------------------------------------------------------------
 *
 * pg_temp_index.c
 *   routines to support manipulation of the pg_temp_index relation
 *
 * The pg_temp_index system catalog table is a global temporary table that
 * stores local overrides to the indisvalid field from the pg_index table
 * for the duration of the current session.  Currently, this is only used
 * for global temporary relations, though in the future, it might also be
 * used for local temporary relations.
 *
 * Much of the code here mirrors similar code in pg_temp_class.c --- see
 * the comments there for more detail.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *   src/backend/catalog/pg_temp_index.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_temp_index.h"
#include "utils/gtcatcache.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/* Cached copy of the pg_temp_index tuple descriptor */
static TupleDesc pg_temp_index_tupdesc = NULL;

/*
 * get_pg_temp_index_tupdesc
 *
 *	Returns the tuple descriptor for pg_temp_index.
 */
static TupleDesc
get_pg_temp_index_tupdesc(void)
{
	/* Build the tuple descriptor the first time through */
	if (pg_temp_index_tupdesc == NULL)
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		tupdesc = CreateTemplateTupleDesc(Natts_pg_temp_index);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_index_indexrelid,
						   "indexrelid", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_index_indisvalid,
						   "indisvalid", BOOLOID, -1, 0);
		TupleDescFinalize(tupdesc);

		MemoryContextSwitchTo(oldcontext);

		/* Cache it for all future use */
		pg_temp_index_tupdesc = tupdesc;
	}
	return pg_temp_index_tupdesc;
}

/*
 * PgTempIndexTupleExists
 *
 *	Test if a pg_temp_index tuple for a global temporary index exists.
 */
bool
PgTempIndexTupleExists(Oid indexrelid)
{
	return GTCatCacheTupleExists(PG_TEMP_INDEX, indexrelid);
}

/*
 * GetPgTempIndexTuple
 *
 *	Get the pg_temp_index tuple for a global temporary index relation.
 *
 *	Returns NULL if the tuple could not be found.  Otherwise, the tuple
 *	returned should be freed with heap_freetuple().
 */
HeapTuple
GetPgTempIndexTuple(Oid indexrelid)
{
	return GTCatCacheSearch(PG_TEMP_INDEX, indexrelid);
}

/*
 * InsertPgTempIndexTuple
 *
 *	Insert a new pg_temp_index tuple for a global temporary index relation.
 *
 *	This is called when a global temporary index relation is created or
 *	accessed for the first time in a session.
 *
 *	Note: The new tuple is not written to the database unless and until
 *	CommandCounterIncrement() is called for a non-read-only command, or the
 *	(sub)transaction is committed.  However, the new tuple *is* visible to all
 *	the functions defined here.
 */
void
InsertPgTempIndexTuple(Oid indexrelid, bool indisvalid)
{
	Datum		values[Natts_pg_temp_index];
	bool		nulls[Natts_pg_temp_index] = {0};

	values[Anum_pg_temp_index_indexrelid - 1] = ObjectIdGetDatum(indexrelid);
	values[Anum_pg_temp_index_indisvalid - 1] = BoolGetDatum(indisvalid);

	GTCatCacheTupleInsert(PG_TEMP_INDEX, indexrelid, RELKIND_INDEX,
						  get_pg_temp_index_tupdesc(), values, nulls);
}

/*
 * UpdatePgTempIndexTuple
 *
 *	Update the pg_temp_index tuple for a global temporary index relation.
 */
void
UpdatePgTempIndexTuple(Oid indexrelid, HeapTuple newtuple)
{
	GTCatCacheTupleUpdate(PG_TEMP_INDEX, indexrelid, newtuple);
}

/*
 * DeletePgTempIndexTuple
 *
 *	Delete the pg_temp_index tuple for a global temporary index relation.
 */
void
DeletePgTempIndexTuple(Oid indexrelid)
{
	GTCatCacheTupleDelete(PG_TEMP_INDEX, indexrelid);
}

/*
 * GetPgIndexAndPgTempIndexTuples
 *
 *	Get the pg_index tuple for an index relation, and if it's a global
 *	temporary index relation, also get the corresponding pg_temp_index tuple,
 *	if present.
 *
 *	Returns NULL if the pg_index tuple could not be found.  Otherwise, the
 *	tuple(s) returned should be freed with heap_freetuple().
 */
HeapTuple
GetPgIndexAndPgTempIndexTuples(Oid indexrelid, HeapTuple *temp_tuple,
							   bool check_temp)
{
	HeapTuple	tuple;

	/* Get a copy of the pg_index tuple */
	tuple = SearchSysCacheCopy1(INDEXRELID, ObjectIdGetDatum(indexrelid));

	if (HeapTupleIsValid(tuple) &&
		rel_is_global_temp(((Form_pg_index) GETSTRUCT(tuple))->indexrelid))
	{
		/* Get the pg_temp_index tuple, and check it exists, if requested */
		*temp_tuple = GetPgTempIndexTuple(indexrelid);
		if (check_temp && !HeapTupleIsValid(*temp_tuple))
			elog(ERROR, "cache lookup failed for global temp index %u", indexrelid);
	}
	else
		*temp_tuple = NULL;

	return tuple;
}

/*
 * GetEffectivePgIndexTuple
 *
 *	Get the effective pg_index tuple for an index relation.
 *
 *	This will fetch the pg_index tuple for the relation and then, if it's a
 *	global temporary relation, fetch the corresponding pg_temp_index tuple and
 *	use the values in it to override the corresponding values in the pg_index
 *	tuple (currently just indisvalid).  Thus, the result represents the
 *	effective state of the index relation in this session.
 *
 *	For a global temporary index relation that has not yet been opened in this
 *	session, there will be no pg_temp_index tuple, and the pg_index tuple will
 *	be returned unchanged.
 *
 *	Returns NULL if the pg_index tuple could not be found.  Otherwise, the
 *	tuple returned should be freed with heap_freetuple().
 */
HeapTuple
GetEffectivePgIndexTuple(Oid indexrelid)
{
	HeapTuple	tuple;
	HeapTuple	temp_tuple;
	Form_pg_index indexform;
	Form_pg_temp_index temp_indexform;

	/*
	 * Get the pg_index and pg_temp_index tuples.  If we have the latter, use
	 * it to update the former.
	 */
	tuple = GetPgIndexAndPgTempIndexTuples(indexrelid, &temp_tuple, false);

	if (HeapTupleIsValid(tuple) && HeapTupleIsValid(temp_tuple))
	{
		indexform = (Form_pg_index) GETSTRUCT(tuple);
		temp_indexform = (Form_pg_temp_index) GETSTRUCT(temp_tuple);
		indexform->indisvalid = temp_indexform->indisvalid;
	}
	return tuple;
}
