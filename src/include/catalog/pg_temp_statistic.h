/*-------------------------------------------------------------------------
 *
 * pg_temp_statistic.h
 *	  definition of the "temporary statistics" system catalog
 *	  (pg_temp_statistic)
 *
 * This is a global temporary system catalog table storing session-specific
 * statistics for temporary relations.  Currently, it is only used for
 * global temporary relations.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/catalog/pg_temp_statistic.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TEMP_STATISTIC_H
#define PG_TEMP_STATISTIC_H

#include "catalog/genbki.h"
#include "catalog/pg_temp_statistic_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_temp_statistic definition.  cpp turns this into
 *		typedef struct FormData_pg_temp_statistic
 * ----------------
 */
BEGIN_CATALOG_STRUCT

/*
 * NB: The fields here must exactly match those in pg_statistic.
 */
CATALOG(pg_temp_statistic,8084,TempStatisticRelationId) BKI_TEMP_RELATION
{
	/* These fields form the unique key for the entry: */
	Oid			starelid BKI_LOOKUP(pg_class);	/* relation containing
												 * attribute */
	int16		staattnum;		/* attribute (column) stats are for */
	bool		stainherit;		/* true if inheritance children are included */

	/* the fraction of the column's entries that are NULL: */
	float4		stanullfrac;

	/*
	 * stawidth is the average width in bytes of non-null entries.  For
	 * fixed-width datatypes this is of course the same as the typlen, but for
	 * var-width types it is more useful.  Note that this is the average width
	 * of the data as actually stored, post-TOASTing (eg, for a
	 * moved-out-of-line value, only the size of the pointer object is
	 * counted).  This is the appropriate definition for the primary use of
	 * the statistic, which is to estimate sizes of in-memory hash tables of
	 * tuples.
	 */
	int32		stawidth;

	/* ----------------
	 * stadistinct indicates the (approximate) number of distinct non-null
	 * data values in the column.  The interpretation is:
	 *		0		unknown or not computed
	 *		> 0		actual number of distinct values
	 *		< 0		negative of multiplier for number of rows
	 * The special negative case allows us to cope with columns that are
	 * unique (stadistinct = -1) or nearly so (for example, a column in which
	 * non-null values appear about twice on the average could be represented
	 * by stadistinct = -0.5 if there are no nulls, or -0.4 if 20% of the
	 * column is nulls).  Because the number-of-rows statistic in pg_class may
	 * be updated more frequently than pg_statistic is, it's important to be
	 * able to describe such situations as a multiple of the number of rows,
	 * rather than a fixed number of distinct values.  But in other cases a
	 * fixed number is correct (eg, a boolean column).
	 * ----------------
	 */
	float4		stadistinct;

	/* ----------------
	 * To allow keeping statistics on different kinds of datatypes,
	 * we do not hard-wire any particular meaning for the remaining
	 * statistical fields.  Instead, we provide several "slots" in which
	 * statistical data can be placed.  Each slot includes:
	 *		kind			integer code identifying kind of data (see below)
	 *		op				OID of associated operator, if needed
	 *		coll			OID of relevant collation, or 0 if none
	 *		numbers			float4 array (for statistical values)
	 *		values			anyarray (for representations of data values)
	 * The ID, operator, and collation fields are never NULL; they are zeroes
	 * in an unused slot.  The numbers and values fields are NULL in an
	 * unused slot, and might also be NULL in a used slot if the slot kind
	 * has no need for one or the other.
	 * ----------------
	 */

	int16		stakind1;
	int16		stakind2;
	int16		stakind3;
	int16		stakind4;
	int16		stakind5;

	Oid			staop1 BKI_LOOKUP_OPT(pg_operator);
	Oid			staop2 BKI_LOOKUP_OPT(pg_operator);
	Oid			staop3 BKI_LOOKUP_OPT(pg_operator);
	Oid			staop4 BKI_LOOKUP_OPT(pg_operator);
	Oid			staop5 BKI_LOOKUP_OPT(pg_operator);

	Oid			stacoll1 BKI_LOOKUP_OPT(pg_collation);
	Oid			stacoll2 BKI_LOOKUP_OPT(pg_collation);
	Oid			stacoll3 BKI_LOOKUP_OPT(pg_collation);
	Oid			stacoll4 BKI_LOOKUP_OPT(pg_collation);
	Oid			stacoll5 BKI_LOOKUP_OPT(pg_collation);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	float4		stanumbers1[1];
	float4		stanumbers2[1];
	float4		stanumbers3[1];
	float4		stanumbers4[1];
	float4		stanumbers5[1];

	/*
	 * Values in these arrays are values of the column's data type, or of some
	 * related type such as an array element type.  We presently have to cheat
	 * quite a bit to allow polymorphic arrays of this kind, but perhaps
	 * someday it'll be a less bogus facility.
	 */
	anyarray	stavalues1;
	anyarray	stavalues2;
	anyarray	stavalues3;
	anyarray	stavalues4;
	anyarray	stavalues5;
#endif
} FormData_pg_temp_statistic;

END_CATALOG_STRUCT

DECLARE_TOAST(pg_temp_statistic, 8085, 8086);

DECLARE_UNIQUE_INDEX_PKEY(pg_temp_statistic_relid_att_inh_index, 8087, TempStatisticRelidAttnumInhIndexId, pg_temp_statistic, btree(starelid oid_ops, staattnum int2_ops, stainherit bool_ops));

MAKE_SYSCACHE(TEMPSTATRELATTINH, pg_temp_statistic_relid_att_inh_index, 128);

/* Is the specified tuple from pg_temp_statistic? */
#define IsTempStatisticTuple(tuple) \
	((tuple)->t_tableOid == TempStatisticRelationId)

#endif							/* PG_TEMP_STATISTIC_H */
