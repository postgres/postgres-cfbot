/*-------------------------------------------------------------------------
 *
 * pg_temp_statistic_ext_data.h
 *	  definition of the "temporary extended statistics data" system catalog
 *	  (pg_temp_statistic_ext_data)
 *
 * This is a global temporary system catalog table storing the statistical
 * data for extended statistics objects on temporary tables.  Currently, it
 * is only used for global temporary tables.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/catalog/pg_temp_statistic_ext_data.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TEMP_STATISTIC_EXT_DATA_H
#define PG_TEMP_STATISTIC_EXT_DATA_H

#include "catalog/genbki.h"
#include "catalog/pg_temp_statistic_ext_data_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_temp_statistic_ext_data definition.  cpp turns this into
 *		typedef struct FormData_pg_temp_statistic_ext_data
 * ----------------
 */
BEGIN_CATALOG_STRUCT

/*
 * NB: The fields here must exactly match those in pg_statistic_ext_data.
 */
CATALOG(pg_temp_statistic_ext_data,8088,TempStatisticExtDataRelationId) BKI_TEMP_RELATION
{
	Oid			stxoid BKI_LOOKUP(pg_statistic_ext);	/* statistics object
														 * this data is for */
	bool		stxdinherit;	/* true if inheritance children are included */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */

	pg_ndistinct stxdndistinct; /* ndistinct coefficients (serialized) */
	pg_dependencies stxddependencies;	/* dependencies (serialized) */
	pg_mcv_list stxdmcv;		/* MCV (serialized) */
	pg_statistic stxdexpr[1];	/* stats for expressions */

#endif

} FormData_pg_temp_statistic_ext_data;

END_CATALOG_STRUCT

DECLARE_TOAST(pg_temp_statistic_ext_data, 8089, 8090);

DECLARE_UNIQUE_INDEX_PKEY(pg_temp_statistic_ext_data_stxoid_inh_index, 8091, TempStatisticExtDataStxoidInhIndexId, pg_temp_statistic_ext_data, btree(stxoid oid_ops, stxdinherit bool_ops));

MAKE_SYSCACHE(TEMPSTATEXTDATASTXOID, pg_temp_statistic_ext_data_stxoid_inh_index, 4);

#endif							/* PG_TEMP_STATISTIC_EXT_DATA_H */
