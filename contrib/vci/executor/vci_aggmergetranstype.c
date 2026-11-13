/*-------------------------------------------------------------------------
 *
 * vci_aggmergetranstype.c
 *	  Parallel merge utility routines to merge between aggregate function's
 *    internal transition (state) data.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_aggmergetranstype.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "vci.h"

#include "vci_executor.h"

#include "postgresql_copy.h"

/**
 * Determine if the given aggregation function is a type that can be supported by VCI
 *
 * @param[in] aggref Pointer to Aggref that holds the aggregate function to be determined
 * @return true if supportable, false if not
 */
bool
vci_is_supported_aggregation(Aggref *aggref)
{
	int			numInputs;
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	AclResult	aclresult;
	Oid			transfn_oid;
	Oid			rettype;
	Oid		   *argtypes;
	int			nargs;
	bool		ret = false;

	/* not UDF */
	if (FirstNormalObjectId <= aggref->aggfnoid)
	{
		elog(DEBUG1, "Aggref contains user-defined aggregation");
		return false;
	}

	/* 0 or 1 input function */
	numInputs = list_length(aggref->args);
	if (1 < numInputs)
	{
		elog(DEBUG1, "Aggref contains an aggregation with 2 or more arguments");
		return false;
	}

	/* Fetch the pg_aggregate row */
	aggTuple = SearchSysCache1(AGGFNOID,
							   ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u",
			 aggref->aggfnoid);

	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

	aclresult = object_aclcheck(ProcedureRelationId, aggref->aggfnoid, GetUserId(),
								ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_AGGREGATE,
					   get_func_name(aggref->aggfnoid));

	transfn_oid = aggform->aggtransfn;

	/* Check that aggregate owner has permission to call component fns */

	rettype = get_func_signature(transfn_oid, &argtypes, &nargs);

	if ((rettype != INTERNALOID) &&
		(nargs == 2) && (rettype == argtypes[0]) && (rettype == argtypes[1]))
	{
		ret = true;
	}
	else
	{
		switch (transfn_oid)
		{
			case F_FLOAT4_ACCUM:
			case F_FLOAT8_ACCUM:
			case F_INT8INC:
			case F_NUMERIC_ACCUM:
			case F_INT2_ACCUM:
			case F_INT4_ACCUM:
			case F_INT8_ACCUM:
			case F_INT2_SUM:
			case F_INT4_SUM:
			case F_INT2_AVG_ACCUM:
			case F_INT4_AVG_ACCUM:
			case F_INT8_AVG_ACCUM:
			case F_INT8INC_ANY:
			case F_NUMERIC_AVG_ACCUM:
			case F_INTERVAL_AVG_COMBINE:
				ret = true;
				break;
			default:
				break;
		}
	}

	if (!ret)
		elog(DEBUG1, "Aggref contains unsupported aggregation function");

	ReleaseSysCache(aggTuple);

	return ret;
}
