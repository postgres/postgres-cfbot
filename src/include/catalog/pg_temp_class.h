/*-------------------------------------------------------------------------
 *
 * pg_temp_class.h
 *	  definition of the "temporary relation" system catalog (pg_temp_class)
 *
 * This is a global temporary system catalog table storing session-specific
 * information about temporary relations.  Currently, it is only used for
 * global temporary relations.  The attributes are a subset of those from
 * pg_class, and their values take precedence over the values from pg_class.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/catalog/pg_temp_class.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TEMP_CLASS_H
#define PG_TEMP_CLASS_H

#include "access/htup.h"
#include "catalog/genbki.h"
#include "catalog/pg_class.h"
#include "catalog/pg_temp_class_d.h"	/* IWYU pragma: export */
#include "utils/rel.h"

/* ----------------
 *		pg_temp_class definition.  cpp turns this into
 *		typedef struct FormData_pg_temp_class
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_temp_class,8082,TempRelationRelationId) BKI_TEMP_RELATION
{
	/* oid */
	Oid			oid BKI_LOOKUP(pg_class);

	/* identifier of physical storage file */
	/* relfilenode == 0 means it is a "mapped" relation, see relmapper.c */
	Oid			relfilenode BKI_DEFAULT(0);

	/* identifier of table space for relation (0 means default for database) */
	Oid			reltablespace BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_tablespace);

	/* # of blocks (not always up-to-date) */
	int32		relpages BKI_DEFAULT(0);

	/* # of tuples (not always up-to-date; -1 means "unknown") */
	float4		reltuples BKI_DEFAULT(-1);

	/* # of all-visible blocks (not always up-to-date) */
	int32		relallvisible BKI_DEFAULT(0);

	/* # of all-frozen blocks (not always up-to-date) */
	int32		relallfrozen BKI_DEFAULT(0);

	/* all Xids < this are frozen in this rel */
	TransactionId relfrozenxid BKI_DEFAULT(3);	/* FirstNormalTransactionId */

	/* all multixacts in this rel are >= this; it is really a MultiXactId */
	TransactionId relminmxid BKI_DEFAULT(1);	/* FirstMultiXactId */
} FormData_pg_temp_class;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_temp_class corresponds to a pointer to a tuple with
 *		the format of pg_temp_class relation.
 * ----------------
 */
typedef FormData_pg_temp_class *Form_pg_temp_class;

DECLARE_UNIQUE_INDEX_PKEY(pg_temp_class_oid_index, 8083, TempClassOidIndexId, pg_temp_class, btree(oid oid_ops));

MAKE_SYSCACHE(TEMPRELOID, pg_temp_class_oid_index, 128);

/*
 * Copy all pg_temp_class attributes from "source" to "target", where the
 * source and target may be of type Form_pg_class or Form_pg_temp_class.
 *
 * Beware of multiple evaluations of arguments!
 */
#define COPY_PG_TEMP_CLASS_ATTRS(source, target) \
	do { \
		(target)->oid = (source)->oid; \
		(target)->relfilenode = (source)->relfilenode; \
		(target)->reltablespace = (source)->reltablespace; \
		(target)->relpages = (source)->relpages; \
		(target)->reltuples = (source)->reltuples; \
		(target)->relallvisible = (source)->relallvisible; \
		(target)->relallfrozen = (source)->relallfrozen; \
		(target)->relfrozenxid = (source)->relfrozenxid; \
		(target)->relminmxid = (source)->relminmxid; \
	} while (0)

/*
 * Get the effective value of relfilenode from pg_class and pg_temp_class
 * tuple data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline Oid
GetEffective_relfilenode(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->relfilenode : cf->relfilenode;
}

/*
 * Get the effective value of reltablespace from pg_class and pg_temp_class
 * tuple data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline Oid
GetEffective_reltablespace(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->reltablespace : cf->reltablespace;
}

/*
 * Get the effective value of relpages from pg_class and pg_temp_class tuple
 * data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline int32
GetEffective_relpages(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->relpages : cf->relpages;
}

/*
 * Get the effective value of reltuples from pg_class and pg_temp_class tuple
 * data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline float4
GetEffective_reltuples(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->reltuples : cf->reltuples;
}

/*
 * Get the effective value of relallvisible from pg_class and pg_temp_class
 * tuple data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline int32
GetEffective_relallvisible(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->relallvisible : cf->relallvisible;
}

/*
 * Get the effective value of relallfrozen from pg_class and pg_temp_class
 * tuple data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline int32
GetEffective_relallfrozen(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->relallfrozen : cf->relallfrozen;
}

/*
 * Get the effective value of relfrozenxid from pg_class and pg_temp_class
 * tuple data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline TransactionId
GetEffective_relfrozenxid(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->relfrozenxid : cf->relfrozenxid;
}

/*
 * Get the effective value of relminmxid from pg_class and pg_temp_class tuple
 * data.  The value from pg_temp_class (if present) takes precedence.
 */
static inline MultiXactId
GetEffective_relminmxid(Form_pg_class cf, Form_pg_temp_class tf)
{
	return tf != NULL ? tf->relminmxid : cf->relminmxid;
}

/*
 * Set the effective value of relfilenode in tuple form data from pg_class or
 * pg_temp_class.  The value is set in pg_temp_class instead of pg_class, if
 * the pg_temp_class tuple form data is non-NULL.
 */
static inline void
SetEffective_relfilenode(Form_pg_class cf, Form_pg_temp_class tf, Oid val)
{
	if (tf != NULL)
		tf->relfilenode = val;
	else
		cf->relfilenode = val;
}

/*
 * Set the effective value of reltablespace in tuple form data from pg_class
 * and pg_temp_class.  The value is set in pg_temp_class as well as pg_class,
 * if the pg_temp_class tuple form data is non-NULL.
 */
static inline void
SetEffective_reltablespace(Form_pg_class cf, Form_pg_temp_class tf, Oid val)
{
	/* NB: Value is set *both* locally and globally */
	cf->reltablespace = val;
	if (tf != NULL)
		tf->reltablespace = val;
}

/*
 * Set the effective value of relpages in tuple form data from pg_class or
 * pg_temp_class.  The value is set in pg_temp_class instead of pg_class, if
 * the pg_temp_class tuple form data is non-NULL.  If non-NULL, the cdirty or
 * tdirty flag is updated, if the value actually changes.
 */
static inline void
SetEffective_relpages(Form_pg_class cf, Form_pg_temp_class tf, int32 val,
					  bool *cdirty, bool *tdirty)
{
	if (tf != NULL)
	{
		if (val != tf->relpages)
		{
			tf->relpages = val;
			if (tdirty != NULL)
				*tdirty = true;
		}
	}
	else if (val != cf->relpages)
	{
		cf->relpages = val;
		if (cdirty != NULL)
			*cdirty = true;
	}
}

/*
 * Set the effective value of reltuples in tuple form data from pg_class or
 * pg_temp_class.  The value is set in pg_temp_class instead of pg_class, if
 * the pg_temp_class tuple form data is non-NULL.  If non-NULL, the cdirty or
 * tdirty flag is updated, if the value actually changes.
 */
static inline void
SetEffective_reltuples(Form_pg_class cf, Form_pg_temp_class tf, float4 val,
					   bool *cdirty, bool *tdirty)
{
	if (tf != NULL)
	{
		if (val != tf->reltuples)
		{
			tf->reltuples = val;
			if (tdirty != NULL)
				*tdirty = true;
		}
	}
	else if (val != cf->reltuples)
	{
		cf->reltuples = val;
		if (cdirty != NULL)
			*cdirty = true;
	}
}

/*
 * Set the effective value of relallvisible in tuple form data from pg_class
 * or pg_temp_class.  The value is set in pg_temp_class instead of pg_class,
 * if the pg_temp_class tuple form data is non-NULL.  If non-NULL, the cdirty
 * or tdirty flag is updated, if the value actually changes.
 */
static inline void
SetEffective_relallvisible(Form_pg_class cf, Form_pg_temp_class tf, int32 val,
						   bool *cdirty, bool *tdirty)
{
	if (tf != NULL)
	{
		if (val != tf->relallvisible)
		{
			tf->relallvisible = val;
			if (tdirty != NULL)
				*tdirty = true;
		}
	}
	else if (val != cf->relallvisible)
	{
		cf->relallvisible = val;
		if (cdirty != NULL)
			*cdirty = true;
	}
}

/*
 * Set the effective value of relallfrozen in tuple form data from pg_class or
 * pg_temp_class.  The value is set in pg_temp_class instead of pg_class, if
 * the pg_temp_class tuple form data is non-NULL.  If non-NULL, the cdirty or
 * tdirty flag is updated, if the value actually changes.
 */
static inline void
SetEffective_relallfrozen(Form_pg_class cf, Form_pg_temp_class tf, int32 val,
						  bool *cdirty, bool *tdirty)
{
	if (tf != NULL)
	{
		if (val != tf->relallfrozen)
		{
			tf->relallfrozen = val;
			if (tdirty != NULL)
				*tdirty = true;
		}
	}
	else if (val != cf->relallfrozen)
	{
		cf->relallfrozen = val;
		if (cdirty != NULL)
			*cdirty = true;
	}
}

/*
 * Set the effective value of relfrozenxid in tuple form data from pg_class or
 * pg_temp_class.  The value is set in pg_temp_class instead of pg_class, if
 * the pg_temp_class tuple form data is non-NULL.  If non-NULL, the cdirty or
 * tdirty flag is updated, if the value actually changes.
 */
static inline void
SetEffective_relfrozenxid(Form_pg_class cf, Form_pg_temp_class tf,
						  TransactionId val, bool *cdirty, bool *tdirty)
{
	if (tf != NULL)
	{
		if (val != tf->relfrozenxid)
		{
			tf->relfrozenxid = val;
			if (tdirty != NULL)
				*tdirty = true;
		}
	}
	else if (val != cf->relfrozenxid)
	{
		cf->relfrozenxid = val;
		if (cdirty != NULL)
			*cdirty = true;
	}
}

/*
 * Set the effective value of relminmxid in tuple form data from pg_class or
 * pg_temp_class.  The value is set in pg_temp_class instead of pg_class, if
 * the pg_temp_class tuple form data is non-NULL.  If non-NULL, the cdirty or
 * tdirty flag is updated, if the value actually changes.
 */
static inline void
SetEffective_relminmxid(Form_pg_class cf, Form_pg_temp_class tf,
						MultiXactId val, bool *cdirty, bool *tdirty)
{
	if (tf != NULL)
	{
		if (val != tf->relminmxid)
		{
			tf->relminmxid = val;
			if (tdirty != NULL)
				*tdirty = true;
		}
	}
	else if (val != cf->relminmxid)
	{
		cf->relminmxid = val;
		if (cdirty != NULL)
			*cdirty = true;
	}
}

extern bool PgTempClassTupleExists(Oid relid);
extern HeapTuple GetPgTempClassTuple(Oid relid);
extern void InsertPgTempClassTuple(Relation rel);
extern void UpdatePgTempClassTuple(Oid relid, HeapTuple newtuple);
extern void UpdatePgTempClassTupleInPlace(Oid relid, HeapTuple newtuple);
extern void DeletePgTempClassTuple(Oid relid);
extern HeapTuple GetPgClassAndPgTempClassTuples(Oid relid, bool lock_tuple,
												HeapTuple *temp_tuple,
												bool check_temp);
extern HeapTuple GetEffectivePgClassTuple(Oid relid);

#endif							/* PG_TEMP_CLASS_H */
