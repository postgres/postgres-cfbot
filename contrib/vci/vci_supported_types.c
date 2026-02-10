/*-------------------------------------------------------------------------
 *
 * vci_supported_types.c
 *	  Types supported by VCI
 *
 * vci_supported_type_table[] is created with following SQL and then examined individually.
 *
 *   SELECT oid, typname FROM pg_type WHERE typnamespace = 11 AND typrelid = 0 AND typelem = 0 ORDER BY oid;
 *
 * - 'typnamespace = 11' is to exclude types not related to table structure
 * - 'typelem = 0' is to exclude array type
 * - 'typrelid = 0' is to exclude composite type
 *
 * Portions Copyright (c) 202666666tgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/vci_supported_types.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "c.h"
#include "catalog/pg_type.h"	/* for TypeRelationId, Form_pg_type */
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "vci.h"
#include "vci_supported_oid.h"

/**
 * Smallest OID among types supported by VCI
 */
#define VCI_SUPPORTED_TYPE_MIN (16)

/**
 * Biggest OID among types supported by VCI
 */
#define VCI_SUPPORTED_TYPE_MAX (2950)

/**
 * Array of information about types supported by VCI
 */
static const struct
{
	Oid			oid;
	const char *name;
	bool		is_support;
}			vci_supported_type_table[] = {
	{16, "bool", true},			/* BOOLOID */
	{17, "bytea", true},		/* BYTEAOID */
	{18, "char", true},			/* CHAROID */
	{20, "int8", true},			/* INT8OID */
	{21, "int2", true},			/* INT2OID */
	{23, "int4", true},			/* INT4OID */
	{24, "regproc", false},		/* REGPROCOID */
	{25, "text", true},			/* TEXTOID */
	{26, "oid", false},			/* OIDOID */
	{27, "tid", false},			/* TIDOID */
	{28, "xid", false},			/* XIDOID */
	{29, "cid", false},			/* CIDOID */
	{32, "pg_ddl_command", false},	/* PG_DDL_COMMANDOID */
	{114, "json", false},		/* JSONOID */
	{142, "xml", false},		/* XMLOID */
	{194, "pg_node_tree", false},	/* PG_NODE_TREEOID */
	{269, "table_am_handler", false},	/* TABLE_AM_HANDLEROID */
	{325, "index_am_handler", false},	/* INDEX_AM_HANDLEROID */
	{602, "path", false},		/* PATHOID */
	{604, "polygon", false},	/* POLYGONOID */
	{650, "cidr", false},		/* CIDROID */
	{700, "float4", true},		/* FLOAT4OID */
	{701, "float8", true},		/* FLOAT8OID */
	{705, "unknown", false},	/* UNKNOWNOID */
	{718, "circle", false},		/* CIRCLEOID */
	{774, "macaddr8", false},	/* MACADDR8OID */
	{790, "money", true},		/* MONEYOID */
	{829, "macaddr", false},	/* MACADDROID */
	{869, "inet", false},		/* INETOID */
	{1033, "aclitem", false},	/* ACLITEMOID */
	{1042, "bpchar", true},		/* BPCHAROID */
	{1043, "varchar", true},	/* VARCHAROID */
	{1082, "date", true},		/* DATEOID */
	{1083, "time", true},		/* TIMEOID */
	{1114, "timestamp", true},	/* TIMESTAMPOID */
	{1184, "timestamptz", true},	/* TIMESTAMPTZOID */
	{1186, "interval", true},	/* INTERVALOID */
	{1266, "timetz", true},		/* TIMETZOID */
	{1560, "bit", true},		/* BITOID */
	{1562, "varbit", true},		/* VARBITOID */
	{1700, "numeric", true},	/* NUMERICOID */
	{1790, "refcursor", false}, /* REFCURSOROID */
	{2202, "regprocedure", false},	/* REGPROCEDUREOID */
	{2203, "regoper", false},	/* REGOPEROID */
	{2204, "regoperator", false},	/* REGOPERATOROID */
	{2205, "regclass", false},	/* REGCLASSOID */
	{2206, "regtype", false},	/* REGTYPEOID */
	{2249, "record", false},	/* RECORDOID */
	{2275, "cstring", false},	/* CSTRINGOID */
	{2276, "any", false},		/* ANYOID */
	{2277, "anyarray", false},	/* ANYARRAYOID */
	{2278, "void", false},		/* VOIDOID */
	{2279, "trigger", false},	/* TRIGGEROID */
	{2280, "language_handler", false},	/* LANGUAGE_HANDLEROID */
	{2281, "internal", false},	/* INTERNALOID */
	{2283, "anyelement", false},	/* ANYELEMENTOID */
	{2776, "anynonarray", false},	/* ANYNONARRAYOID */
	{2950, "uuid", true},		/* UUIDOID */
	{2970, "txid_snapshot", false},
	{3115, "fdw_handler", false},	/* FDW_HANDLEROID */
	{3220, "pg_lsn", false},	/* PG_LSNOID */
	{3310, "tsm_handler", false},	/* TSM_HANDLEROID */
	{3361, "pg_ndistinct", false},	/* PG_NDISTINCTOID */
	{3402, "pg_dependencies", false},	/* PG_DEPENDENCIESOID */
	{3500, "anyenum", false},	/* ANYENUMOID */
	{3614, "tsvector", false},	/* TSVECTOROID */
	{3615, "tsquery", false},	/* TSQUERYOID */
	{3642, "gtsvector", false}, /* GTSVECTOROID */
	{3734, "regconfig", false}, /* REGCONFIGOID */
	{3769, "regdictionary", false}, /* REGDICTIONARYOID */
	{3802, "jsonb", false},		/* JSONBOID */
	{3831, "anyrange", false},	/* ANYRANGEOID */
	{3838, "event_trigger", false}, /* EVENT_TRIGGEROID */
	{3904, "int4range", false}, /* INT4RANGEOID */
	{3906, "numrange", false},
	{3908, "tsrange", false},
	{3910, "tstzrange", false},
	{3912, "daterange", false},
	{3926, "int8range", false},
	{4072, "jsonpath", false},	/* JSONPATHOID */
	{4089, "regnamespace", false},	/* REGNAMESPACEOID */
	{4096, "regrole", false},	/* REGROLEOID */
	{4191, "regcollation", false},	/* REGCOLLATIONOID */
	{4451, "int4multirange", false},
	{4532, "nummultirange", false},
	{4533, "tsmultirange", false},
	{4534, "tstzmultirange", false},
	{4535, "datemultirange", false},
	{4536, "int8multirange", false},
	{4537, "anymultirange", false},
	{4538, "anycompatiblemultirange", false},
	{4600, "pg_brin_bloom_summary", false}, /* PG_BRIN_BLOOM_SUMMARYOID */
	{4601, "pg_brin_minmax_multi_summary", false},	/* PG_BRIN_MINMAX_MULTI_SUMMARYOID */
	{5017, "pg_mcv_list", false},	/* PG_MCV_LISTOID */
	{5038, "pg_snapshot", false},	/* PG_SNAPSHOTOID */
	{5069, "xid8", false},		/* XID8OID */
	{5077, "anycompatible", false}, /* ANYCOMPATIBLEOID */
	{5078, "anycompatiblearray", false},	/* ANYCOMPATIBLEARRAYOID */
	{5079, "anycompatiblenonarray", false}, /* ANYCOMPATIBLENONARRAYOID */
	{5080, "anycompatiblerange", false},	/* ANYCOMPATIBLERANGEOID */
};

/**
 * Determine if the given oid is a type that can be supported by VCI
 *
 * @param[in] oid OID (pg_proc.oid) indicating the type to be determined
 * @return true if supported, false otherwise
 */
bool
vci_is_supported_type(Oid oid)
{
	int			min,
				max,
				pivot;

	if ((oid < VCI_SUPPORTED_TYPE_MIN) || (VCI_SUPPORTED_TYPE_MAX < oid))
		return false;

	/* 2 minute search */

	min = 0;
	max = lengthof(vci_supported_type_table);	/* exclusive */

	while (max - min > 1)
	{
		Oid			comp;

		pivot = (min + max) / 2;

		comp = vci_supported_type_table[pivot].oid;

		if (comp == oid)
			return vci_supported_type_table[pivot].is_support;
		else if (oid < comp)
			max = pivot;
		else					/* comp < oid */
			min = pivot;
	}

	if (max - min == 1)
		if (oid == vci_supported_type_table[min].oid)
			return vci_supported_type_table[min].is_support;

	return false;
}

/*==========================================================================*/
/* Implementation of PG function to check supported types at CREATE EXTENSION */
/*==========================================================================*/

PG_FUNCTION_INFO_V1(vci_check_supported_types);

Datum
vci_check_supported_types(PG_FUNCTION_ARGS)
{
	Relation	rel;

	rel = table_open(TypeRelationId, AccessShareLock);

	for (int i = 0; i < lengthof(vci_supported_type_table); i++)
	{
		HeapTuple	tuple;
		Form_pg_type typeform;

		if (!vci_supported_type_table[i].is_support)
			continue;

		tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(vci_supported_type_table[i].oid));
		if (!HeapTupleIsValid(tuple))
			goto error;

		typeform = (Form_pg_type) GETSTRUCT(tuple);

		if (strcmp(vci_supported_type_table[i].name, NameStr(typeform->typname)) != 0)
			goto error;

		ReleaseSysCache(tuple);
	}

	table_close(rel, AccessShareLock);

	PG_RETURN_VOID();

error:
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("extension \"%s\" cannot be installed under this version of PostgreSQL", VCI_STRING)));

	PG_RETURN_VOID();
}
