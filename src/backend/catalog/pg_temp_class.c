/*-------------------------------------------------------------------------
 *
 * pg_temp_class.c
 *	  routines to support manipulation of the pg_temp_class relation
 *
 * The pg_temp_class system catalog table is a global temporary table that
 * stores local overrides to various fields from the pg_class table for the
 * duration of the current session.  Currently, this is only used for
 * global temporary relations, though in the future, it might also be used
 * for local temporary relations.
 *
 * Tuples are first added to pg_temp_class when global temporary relations
 * (including pg_temp_class itself) are created or opened for the first
 * time in a session.  This "first time" might be repeated if the effects
 * of a previous "first time" are rolled back.
 *
 * All pg_temp_class tuples are held in a cache, managed by gtcatcache.c,
 * and all updates to pg_temp_class by backend code should go through the
 * routines defined here.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_temp_class.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_temp_class.h"
#include "utils/gtcatcache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/* Cached copy of the pg_temp_class tuple descriptor */
static TupleDesc pg_temp_class_tupdesc = NULL;

/*
 * get_pg_temp_class_tupdesc
 *
 *	Returns the tuple descriptor for pg_temp_class.
 */
static TupleDesc
get_pg_temp_class_tupdesc(void)
{
	/* Build the tuple descriptor the first time through */
	if (pg_temp_class_tupdesc == NULL)
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		tupdesc = CreateTemplateTupleDesc(Natts_pg_temp_class);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_oid,
						   "oid", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_relfilenode,
						   "relfilenode", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_reltablespace,
						   "reltablespace", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_relpages,
						   "relpages", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_reltuples,
						   "reltuples", FLOAT4OID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_relallvisible,
						   "relallvisible", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_relallfrozen,
						   "relallfrozen", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_relfrozenxid,
						   "relfrozenxid", XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) Anum_pg_temp_class_relminmxid,
						   "relminmxid", XIDOID, -1, 0);
		TupleDescFinalize(tupdesc);

		MemoryContextSwitchTo(oldcontext);

		/* Cache it for all future use */
		pg_temp_class_tupdesc = tupdesc;
	}
	return pg_temp_class_tupdesc;
}

/*
 * PgTempClassTupleExists
 *
 *	Test if a pg_temp_class tuple for a global temporary relation exists.
 */
bool
PgTempClassTupleExists(Oid relid)
{
	return GTCatCacheTupleExists(PG_TEMP_CLASS, relid);
}

/*
 * GetPgTempClassTuple
 *
 *	Get the pg_temp_class tuple for a global temporary relation.
 *
 *	Returns NULL if the tuple could not be found.  Otherwise, the tuple
 *	returned should be freed with heap_freetuple().
 */
HeapTuple
GetPgTempClassTuple(Oid relid)
{
	return GTCatCacheSearch(PG_TEMP_CLASS, relid);
}

/*
 * InsertPgTempClassTuple
 *
 *	Insert a new pg_temp_class tuple for a global temporary relation.
 *
 *	This is called when a global temporary relation is created or accessed for
 *	the first time in a session.  All tuple data is taken from rel->rd_rel.
 *
 *	Note: The new tuple is not written to the database unless and until
 *	CommandCounterIncrement() is called for a non-read-only command, or the
 *	(sub)transaction is committed.  However, the new tuple *is* visible to all
 *	the functions defined here.
 */
void
InsertPgTempClassTuple(Relation rel)
{
	Form_pg_class form = rel->rd_rel;
	Datum		values[Natts_pg_temp_class];
	bool		nulls[Natts_pg_temp_class] = {0};

	values[Anum_pg_temp_class_oid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_temp_class_relfilenode - 1] = ObjectIdGetDatum(form->relfilenode);
	values[Anum_pg_temp_class_reltablespace - 1] = ObjectIdGetDatum(form->reltablespace);
	values[Anum_pg_temp_class_relpages - 1] = Int32GetDatum(form->relpages);
	values[Anum_pg_temp_class_reltuples - 1] = Float4GetDatum(form->reltuples);
	values[Anum_pg_temp_class_relallvisible - 1] = Int32GetDatum(form->relallvisible);
	values[Anum_pg_temp_class_relallfrozen - 1] = Int32GetDatum(form->relallfrozen);
	values[Anum_pg_temp_class_relfrozenxid - 1] = TransactionIdGetDatum(form->relfrozenxid);
	values[Anum_pg_temp_class_relminmxid - 1] = MultiXactIdGetDatum(form->relminmxid);

	GTCatCacheTupleInsert(PG_TEMP_CLASS,
						  RelationGetRelid(rel),
						  rel->rd_rel->relkind,
						  get_pg_temp_class_tupdesc(),
						  values, nulls);
}

/*
 * UpdatePgTempClassTuple
 *
 *	Update the pg_temp_class tuple for a global temporary relation.
 */
void
UpdatePgTempClassTuple(Oid relid, HeapTuple newtuple)
{
	GTCatCacheTupleUpdate(PG_TEMP_CLASS, relid, newtuple);
}

/*
 * UpdatePgTempClassTupleInPlace
 *
 *	Do an in-place update of the pg_temp_class tuple for a global temporary
 *	relation.
 */
void
UpdatePgTempClassTupleInPlace(Oid relid, HeapTuple newtuple)
{
	GTCatCacheTupleUpdateInPlace(PG_TEMP_CLASS, relid, newtuple);
}

/*
 * DeletePgTempClassTuple
 *
 *	Delete the pg_temp_class tuple for a global temporary relation.
 */
void
DeletePgTempClassTuple(Oid relid)
{
	GTCatCacheTupleDelete(PG_TEMP_CLASS, relid);
}

/*
 * GetPgClassAndPgTempClassTuples
 *
 *	Get the pg_class tuple for a relation, and if it's a global temporary
 *	relation, also get the corresponding pg_temp_class tuple.
 *
 *	If lock_tuple is true, the pg_class tuple will be locked, but not the
 *	pg_temp_class tuple.
 *
 *	If check_temp is true, an error will be raised if a global temporary
 *	relation's pg_temp_class tuple is not found.  After a global temporary
 *	relation has been opened, its pg_temp_class tuple should always exist.
 *
 *	Returns NULL if the pg_class tuple could not be found.  Otherwise, the
 *	tuple(s) returned should be freed with heap_freetuple().
 */
HeapTuple
GetPgClassAndPgTempClassTuples(Oid relid, bool lock_tuple,
							   HeapTuple *temp_tuple, bool check_temp)
{
	HeapTuple	tuple;

	/* Get a copy of the pg_class tuple */
	if (lock_tuple)
		tuple = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relid));
	else
		tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (HeapTupleIsValid(tuple) &&
		((Form_pg_class) GETSTRUCT(tuple))->relpersistence == RELPERSISTENCE_GLOBAL_TEMP)
	{
		/* Get the pg_temp_class tuple, and check it exists, if requested */
		*temp_tuple = GetPgTempClassTuple(relid);
		if (check_temp && !HeapTupleIsValid(*temp_tuple))
			elog(ERROR, "cache lookup failed for global temp relation %u", relid);
	}
	else
		*temp_tuple = NULL;

	return tuple;
}

/*
 * GetEffectivePgClassTuple
 *
 *	Get the effective pg_class tuple for a relation.
 *
 *	This will fetch the pg_class tuple for the relation and then, if it's a
 *	global temporary relation, fetch the corresponding pg_temp_class tuple and
 *	use the values in it to override the corresponding values in the pg_class
 *	tuple.  Thus, the result represents the effective state of the relation in
 *	this session.
 *
 *	For a global temporary relation that has not yet been opened in this
 *	session, there will be no pg_temp_class tuple, and the pg_class tuple will
 *	be returned unchanged.
 *
 *	Returns NULL if the pg_class tuple could not be found.  Otherwise, the
 *	tuple returned should be freed with heap_freetuple().
 */
HeapTuple
GetEffectivePgClassTuple(Oid relid)
{
	HeapTuple	tuple;
	HeapTuple	temp_tuple;
	Form_pg_class classform;
	Form_pg_temp_class temp_classform;

	/*
	 * Get the pg_class and pg_temp_class tuples.  If we have the latter, use
	 * it to update the former.
	 */
	tuple = GetPgClassAndPgTempClassTuples(relid, false, &temp_tuple, false);

	if (HeapTupleIsValid(tuple) && HeapTupleIsValid(temp_tuple))
	{
		classform = (Form_pg_class) GETSTRUCT(tuple);
		temp_classform = (Form_pg_temp_class) GETSTRUCT(temp_tuple);
		COPY_PG_TEMP_CLASS_ATTRS(temp_classform, classform);
	}
	return tuple;
}
