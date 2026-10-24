/*-------------------------------------------------------------------------
 *
 * pg_toast_dictionary.h
 *	  definition of the "TOAST compression dictionary" system catalog
 *	  (pg_toast_dictionary)
 *
 * Each row is one trained compression dictionary.  A dictionary is owned by
 * exactly one column of one table, just as a pg_statistic row is: it is
 * deleted with that column or table, never shared, and needs no pg_depend
 * entries.  Values compressed with a dictionary are only ever stored in the
 * column that owns it; see toast_dictionary.c for how that is enforced.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_toast_dictionary.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TOAST_DICTIONARY_H
#define PG_TOAST_DICTIONARY_H

#include "catalog/genbki.h"
#include "catalog/pg_toast_dictionary_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_toast_dictionary definition.  cpp turns this into
 *		typedef struct FormData_pg_toast_dictionary
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_toast_dictionary,9260,ToastDictionaryRelationId)
{
	Oid			oid;			/* dictionary ID, as stored in each datum */
	Oid			tdrelid BKI_LOOKUP(pg_class);	/* owning relation */
	int16		tdattnum;		/* owning column */
	bool		tdcurrent;		/* used to compress new values? */
	int64		tdtrainrows;	/* reltuples estimate when trained, for
								 * the auto-evolution growth trigger */
	int64		tdtraintime;	/* training time as a TimestampTz, for the
								 * auto-evolution cooldown */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	bytea		tddict BKI_FORCE_NOT_NULL;	/* trained dictionary content */
#endif
} FormData_pg_toast_dictionary;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_toast_dictionary corresponds to a pointer to a tuple with
 *		the format of pg_toast_dictionary relation.
 * ----------------
 */
typedef FormData_pg_toast_dictionary *Form_pg_toast_dictionary;

DECLARE_TOAST(pg_toast_dictionary, 9261, 9262);

DECLARE_UNIQUE_INDEX_PKEY(pg_toast_dictionary_oid_index, 9263, ToastDictionaryOidIndexId, pg_toast_dictionary, btree(oid oid_ops));
DECLARE_INDEX(pg_toast_dictionary_relid_attnum_index, 9264, ToastDictionaryRelidAttnumIndexId, pg_toast_dictionary, btree(tdrelid oid_ops, tdattnum int2_ops));

MAKE_SYSCACHE(TOASTDICTOID, pg_toast_dictionary_oid_index, 16);

#endif							/* PG_TOAST_DICTIONARY_H */
