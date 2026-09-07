/*-------------------------------------------------------------------------
 *
 * pg_temp_index.h
 *	  definition of the "temporary index" system catalog (pg_temp_index)
 *
 * This is a global temporary system catalog table storing session-specific
 * information about temporary indexes.  Currently, it is only used for global
 * temporary indexes.  The attributes (currently just indisvalid) are a subset
 * of those from pg_index, and their values take precedence over the values
 * from pg_index.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/catalog/pg_index.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TEMP_INDEX_H
#define PG_TEMP_INDEX_H

#include "access/htup.h"
#include "catalog/genbki.h"
#include "catalog/pg_index.h"
#include "catalog/pg_temp_index_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_temp_index definition.  cpp turns this into
 *		typedef struct FormData_pg_temp_index.
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_temp_index,8092,TempIndexRelationId) BKI_TEMP_RELATION
{
	Oid			indexrelid BKI_LOOKUP(pg_class);	/* OID of the index */
	bool		indisvalid;		/* is this index valid for use by queries? */
} FormData_pg_temp_index;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_temp_index corresponds to a pointer to a tuple with
 *		the format of pg_temp_index relation.
 * ----------------
 */
typedef FormData_pg_temp_index *Form_pg_temp_index;

DECLARE_UNIQUE_INDEX_PKEY(pg_temp_index_indexrelid_index, 8093, TempIndexRelidIndexId, pg_temp_index, btree(indexrelid oid_ops));

MAKE_SYSCACHE(TEMPINDEXRELID, pg_temp_index_indexrelid_index, 64);

/*
 * Get the effective value of indisvalid from pg_index and pg_temp_index tuple
 * data.  The value from pg_temp_index (if present) takes precedence.
 */
static inline bool
GetEffective_indisvalid(Form_pg_index f, Form_pg_temp_index tf)
{
	return tf != NULL ? tf->indisvalid : f->indisvalid;
}

extern bool PgTempIndexTupleExists(Oid relid);
extern HeapTuple GetPgTempIndexTuple(Oid indexrelid);
extern void InsertPgTempIndexTuple(Oid indexrelid, bool indisvalid);
extern void UpdatePgTempIndexTuple(Oid indexrelid, HeapTuple newtuple);
extern void DeletePgTempIndexTuple(Oid indexrelid);
extern HeapTuple GetPgIndexAndPgTempIndexTuples(Oid indexrelid,
												HeapTuple *temp_tuple,
												bool check_temp);
extern HeapTuple GetEffectivePgIndexTuple(Oid indexrelid);

#endif							/* PG_TEMP_INDEX_H */
