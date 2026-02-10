/*-------------------------------------------------------------------------
 *
 * vci_aggref.c
 *    Routine to inline transition functions for speeding up aggregate functions
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_aggref.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdlib.h>

#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "commands/explain.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"

#include "vci.h"
#include "vci_executor.h"
#include "vci_aggref.h"

#define VCI_USE_CMP_FUNC
#include "postgresql_copy.h"
#undef VCI_USE_CMP_FUNC

#define VCI_TRANS_INPUTS_0           (0)
#define VCI_TRANS_INPUTS_1_SIMPLEVAR (1)
#define VCI_TRANS_INPUTS_1_EVALEXPR  (2)

/*
 * Default pattern
 */
#define VCI_ADVANCE_AGGREF_FUNC     aggref_0input_default
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_0
#define VCI_TRANFN_OID              0
#define VCI_TRANS_FN_STRICT         peraggstate->transfn.fn_strict
#define VCI_TRANS_TYPE_BYVAL        -1
#define VCI_TRANS_USE_CURPERAGG
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_USE_CURPERAGG
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_default
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              0
#define VCI_TRANS_FN_STRICT         peraggstate->transfn.fn_strict
#define VCI_TRANS_TYPE_BYVAL        -1
#define VCI_TRANS_USE_CURPERAGG
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_USE_CURPERAGG
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_default
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              0
#define VCI_TRANS_FN_STRICT         peraggstate->transfn.fn_strict
#define VCI_TRANS_TYPE_BYVAL        -1
#define VCI_TRANS_USE_CURPERAGG
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_USE_CURPERAGG
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

/*
 * Individual advance transition routine
 */
#define VCI_ADVANCE_AGGREF_FUNC     aggref_0input_int8inc
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_0
#define VCI_TRANFN_OID              F_INT8INC
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int8inc_any
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT8INC_ANY
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float4_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT4_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float4pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT4PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float4larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT4LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float4smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT4SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float8pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT8PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int4larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT4LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int4smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT4SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_cash_pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_CASH_PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_cashlarger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_CASHLARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_cashsmaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_CASHSMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_date_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_DATE_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_date_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_DATE_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_interval_pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INTERVAL_PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_timestamp_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_TIMESTAMP_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_timestamp_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_TIMESTAMP_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_interval_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INTERVAL_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_interval_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INTERVAL_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_time_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_TIME_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_time_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_TIME_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_timetz_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_TIMETZ_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_timetz_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_TIMETZ_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int2_sum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT2_SUM
#define VCI_TRANS_FN_STRICT         0
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int4_sum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT4_SUM
#define VCI_TRANS_FN_STRICT         0
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int4and
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT4AND
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int4or
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT4OR
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int4_avg_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT4_AVG_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_booland_statefunc
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_BOOLAND_STATEFUNC
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_boolor_statefunc
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_BOOLOR_STATEFUNC
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int2and
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT2AND
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int2or
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT2OR
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int2_avg_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT2_AVG_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int2larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT2LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int2smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT2SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int8and
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT8AND
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int8or
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT8OR
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int8larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT8LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_int8smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_INT8SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float8larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT8LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float8smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT8SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_simple_var_float8_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_SIMPLEVAR
#define VCI_TRANFN_OID              F_FLOAT8_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

/* eval expr */

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int8inc_any
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT8INC_ANY
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float4_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT4_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float4pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT4PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float4larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT4LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float4smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT4SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float8pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT8PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int4larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT4LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int4smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT4SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_cash_pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_CASH_PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_cashlarger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_CASHLARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_cashsmaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_CASHSMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_date_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_DATE_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_date_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_DATE_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_interval_pl
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INTERVAL_PL
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_timestamp_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_TIMESTAMP_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_timestamp_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_TIMESTAMP_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_interval_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INTERVAL_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_interval_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INTERVAL_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_time_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_TIME_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_time_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_TIME_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_timetz_larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_TIMETZ_LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_timetz_smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_TIMETZ_SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int2_sum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT2_SUM
#define VCI_TRANS_FN_STRICT         0
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int4_sum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT4_SUM
#define VCI_TRANS_FN_STRICT         0
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int4and
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT4AND
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int4or
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT4OR
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int4_avg_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT4_AVG_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_booland_statefunc
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_BOOLAND_STATEFUNC
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_boolor_statefunc
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_BOOLOR_STATEFUNC
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int2and
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT2AND
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int2or
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT2OR
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int2_avg_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT2_AVG_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int2larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT2LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int2smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT2SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        1
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int8and
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT8AND
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int8or
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT8OR
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int8larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT8LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_int8smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_INT8SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float8larger
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT8LARGER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float8smaller
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT8SMALLER
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        USE_FLOAT8_BYVAL
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

#define VCI_ADVANCE_AGGREF_FUNC     aggref_eval_expr_float8_accum
#define VCI_TRANS_INPUTS_ARG        VCI_TRANS_INPUTS_1_EVALEXPR
#define VCI_TRANFN_OID              F_FLOAT8_ACCUM
#define VCI_TRANS_FN_STRICT         1
#define VCI_TRANS_TYPE_BYVAL        0
#include "vci_aggref_impl.inc"
#undef  VCI_TRANS_TYPE_BYVAL
#undef  VCI_TRANS_FN_STRICT
#undef  VCI_TRANFN_OID
#undef  VCI_TRANS_INPUTS_ARG
#undef  VCI_ADVANCE_AGGREF_FUNC

typedef struct
{
	Oid			fn_oid;
	short		fn_nargs;
	bool		fn_strict;
	bool		transtypeByVal;
	bool		consumeMemory;
	bool		useCurPerAgg;
	VciAdvanceAggref_Func simple_var_func;
	VciAdvanceAggref_Func eval_expr_func;
} AggrefTransInfo;

#define VCI_F_TIMESTAMP_SMALLER 2035
#define VCI_F_TIMESTAMP_LARGER  2036

static int	compare_aggref_trans_info(const void *p1, const void *p2);
static AggrefTransInfo *search_aggref_trans_info(Oid oid);

/**
 * Show the inline expansion routine for each transition function
 *
 *
 * @note Array should be ordered in ascending fn_oid order
 */

#ifdef USE_FLOAT8_BYVAL
#define VCI_FLOAT8_TRANSTYPEBYVAL true
#else
#define VCI_FLOAT8_TRANSTYPEBYVAL false
#endif

static AggrefTransInfo function_table[] = {
	{F_FLOAT4PL, 2, true, true, false, false, aggref_simple_var_float4pl, aggref_eval_expr_float4pl},	/* 204 */
	{F_FLOAT4_ACCUM, 2, true, false, false, false, aggref_simple_var_float4_accum, aggref_eval_expr_float4_accum},	/* 208 */
	{F_FLOAT4LARGER, 2, true, true, false, false, aggref_simple_var_float4larger, aggref_eval_expr_float4larger},	/* 209 */
	{F_FLOAT4SMALLER, 2, true, true, false, false, aggref_simple_var_float4smaller, aggref_eval_expr_float4smaller},	/* 211 */
	{F_FLOAT8PL, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_float8pl, aggref_eval_expr_float8pl},	/* 218 */
	{F_FLOAT8_ACCUM, 2, true, false, false, false, aggref_simple_var_float8_accum, aggref_eval_expr_float8_accum},	/* 222 */
	{F_FLOAT8LARGER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_float8larger, aggref_eval_expr_float8larger},	/* 223 */
	{F_FLOAT8SMALLER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_float8smaller, aggref_eval_expr_float8smaller},	/* 224 */
	{F_INT4LARGER, 2, true, true, false, false, aggref_simple_var_int4larger, aggref_eval_expr_int4larger}, /* 768 */
	{F_INT4SMALLER, 2, true, true, false, false, aggref_simple_var_int4smaller, aggref_eval_expr_int4smaller},	/* 769 */
	{F_INT2LARGER, 2, true, true, false, false, aggref_simple_var_int2larger, aggref_eval_expr_int2larger}, /* 770 */
	{F_INT2SMALLER, 2, true, true, false, false, aggref_simple_var_int2smaller, aggref_eval_expr_int2smaller},	/* 771 */
	{F_CASH_PL, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_cash_pl, aggref_eval_expr_cash_pl}, /* 894 */
	{F_CASHLARGER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_cashlarger, aggref_eval_expr_cashlarger},	/* 898 */
	{F_CASHSMALLER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_cashsmaller, aggref_eval_expr_cashsmaller}, /* 899 */
	{F_DATE_LARGER, 2, true, true, false, false, aggref_simple_var_date_larger, aggref_eval_expr_date_larger},	/* 1138 */
	{F_DATE_SMALLER, 2, true, true, false, false, aggref_simple_var_date_smaller, aggref_eval_expr_date_smaller},	/* 1139 */
	{F_INTERVAL_PL, 2, true, false, true, false, aggref_simple_var_interval_pl, aggref_eval_expr_interval_pl},	/* 1169 */
	{F_TIMESTAMP_SMALLER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_timestamp_smaller, aggref_eval_expr_timestamp_smaller},	/* 1195 */
	{F_TIMESTAMP_LARGER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_timestamp_larger, aggref_eval_expr_timestamp_larger},	/* 1196 */
	{F_INTERVAL_SMALLER, 2, true, false, false, false, aggref_simple_var_interval_smaller, aggref_eval_expr_interval_smaller},	/* 1197 */
	{F_INTERVAL_LARGER, 2, true, false, false, false, aggref_simple_var_interval_larger, aggref_eval_expr_interval_larger}, /* 1198 */
	{F_INT8INC, 1, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_0input_int8inc, NULL}, /* 1219 */
	{F_INT8LARGER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_int8larger, aggref_eval_expr_int8larger},	/* 1236 */
	{F_INT8SMALLER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_int8smaller, aggref_eval_expr_int8smaller}, /* 1237 */
	{F_TIME_LARGER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_time_larger, aggref_eval_expr_time_larger}, /* 1377 */
	{F_TIME_SMALLER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_time_smaller, aggref_eval_expr_time_smaller},	/* 1378 */
	{F_TIMETZ_LARGER, 2, true, false, false, false, aggref_simple_var_timetz_larger, aggref_eval_expr_timetz_larger},	/* 1379 */
	{F_TIMETZ_SMALLER, 2, true, false, false, false, aggref_simple_var_timetz_smaller, aggref_eval_expr_timetz_smaller},	/* 1380 */
	{F_INT2_SUM, 2, false, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_int2_sum, aggref_eval_expr_int2_sum}, /* 1840 */
	{F_INT4_SUM, 2, false, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_int4_sum, aggref_eval_expr_int4_sum}, /* 1841 */
	{F_INT2AND, 2, true, true, false, false, aggref_simple_var_int2and, aggref_eval_expr_int2and},	/* 1892 */
	{F_INT2OR, 2, true, true, false, false, aggref_simple_var_int2or, aggref_eval_expr_int2or}, /* 1893 */
	{F_INT4AND, 2, true, true, false, false, aggref_simple_var_int4and, aggref_eval_expr_int4and},	/* 1898 */
	{F_INT4OR, 2, true, true, false, false, aggref_simple_var_int4or, aggref_eval_expr_int4or}, /* 1899 */
	{F_INT8AND, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_int8and, aggref_eval_expr_int8and}, /* 1904 */
	{F_INT8OR, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_int8or, aggref_eval_expr_int8or},	/* 1905 */
	{F_INT2_AVG_ACCUM, 2, true, false, false, false, aggref_simple_var_int2_avg_accum, aggref_eval_expr_int2_avg_accum},	/* 1962 */
	{F_INT4_AVG_ACCUM, 2, true, false, false, false, aggref_simple_var_int4_avg_accum, aggref_eval_expr_int4_avg_accum},	/* 1963 */
	{VCI_F_TIMESTAMP_SMALLER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_timestamp_smaller, aggref_eval_expr_timestamp_smaller},	/* 2035 */
	{VCI_F_TIMESTAMP_LARGER, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_timestamp_larger, aggref_eval_expr_timestamp_larger},	/* 2036 */
	{F_BOOLAND_STATEFUNC, 2, true, true, false, false, aggref_simple_var_booland_statefunc, aggref_eval_expr_booland_statefunc},	/* 2515 */
	{F_BOOLOR_STATEFUNC, 2, true, true, false, false, aggref_simple_var_boolor_statefunc, aggref_eval_expr_boolor_statefunc},	/* 2516 */
	{F_INT8INC_ANY, 2, true, VCI_FLOAT8_TRANSTYPEBYVAL, false, false, aggref_simple_var_int8inc_any, aggref_eval_expr_int8inc_any}, /* 2804 */
};

/**
 * Returns routine that individually inlines transition function for aggregate function
 *
 * When PostgreSQL performs avg aggregation on float4, the transition function that adds each input data (of float4 type)
 * is float4_accum(). This function uses information from VciAggStatePerAgg to identify the transition function and
 * returns a pointer to a fast routine that inlines the transition function, if any.
 *
 * @param[in]     peraggstate Pointer to AggrefState information
 * @return Returns pointer to transition routine for the aggregate function. If not supported, returns NULL
 */
VciAdvanceAggref_Func
VciGetSpecialAdvanceAggrefFunc(VciAggStatePerAgg peraggstate)
{
	VciProjectionInfo *projInfo = peraggstate->evalproj;

	if (peraggstate->aggref->aggfilter != NULL || peraggstate->numSortCols > 0)
		return NULL;

	if ((peraggstate->numTransInputs == 0) ||
		(peraggstate->numTransInputs == 1 &&
		 projInfo->pi_numSimpleVars == 1 && projInfo->pi_directMap && projInfo->pi_tle_array_len == 0))
	{
		AggrefTransInfo *trans_info_p = search_aggref_trans_info(peraggstate->transfn_oid);

		if (trans_info_p)
		{
			if (trans_info_p->simple_var_func)
			{
				if (peraggstate->transfn.fn_nargs != trans_info_p->fn_nargs)
					elog(ERROR, "Oid %d fn_nargs = %d, trans_info.fn_nargs = %d",
						 peraggstate->transfn_oid, peraggstate->transfn.fn_nargs, trans_info_p->fn_nargs);

				if (peraggstate->transfn.fn_strict != trans_info_p->fn_strict)
					elog(ERROR, "Oid %d peraggstate fn_strict = %d, trans_info.fn_strict = %d",
						 peraggstate->transfn_oid, peraggstate->transfn.fn_strict, trans_info_p->fn_strict);

				if (peraggstate->transtypeByVal != trans_info_p->transtypeByVal)
					elog(ERROR, "Oid %d transtypeByVal peraggstate = %d, trans_info = %d",
						 peraggstate->transfn_oid, peraggstate->transtypeByVal, trans_info_p->transtypeByVal);

				return trans_info_p->simple_var_func;
			}
		}

		if (peraggstate->numTransInputs == 0)
			return aggref_0input_default;
		else
			return aggref_simple_var_default;
	}
	else if (peraggstate->numTransInputs == 1 &&
			 projInfo->pi_numSimpleVars == 0 && projInfo->pi_tle_array_len == 1)
	{
		AggrefTransInfo *trans_info_p = search_aggref_trans_info(peraggstate->transfn_oid);

		if (trans_info_p)
		{
			if (trans_info_p->eval_expr_func)
			{
				if (peraggstate->transfn.fn_nargs != trans_info_p->fn_nargs)
					elog(ERROR, "Oid %d fn_nargs = %d, trans_info.fn_nargs = %d",
						 peraggstate->transfn_oid, peraggstate->transfn.fn_nargs, trans_info_p->fn_nargs);

				if (peraggstate->transfn.fn_strict != trans_info_p->fn_strict)
					elog(ERROR, "Oid %d peraggstate fn_strict = %d, trans_info.fn_strict = %d",
						 peraggstate->transfn_oid, peraggstate->transfn.fn_strict, trans_info_p->fn_strict);

				if (peraggstate->transtypeByVal != trans_info_p->transtypeByVal)
					elog(ERROR, "Oid %d transtypeByVal peraggstate = %d, trans_info = %d",
						 peraggstate->transfn_oid, peraggstate->transtypeByVal, trans_info_p->transtypeByVal);

				return trans_info_p->eval_expr_func;
			}
		}

		return aggref_eval_expr_default;
	}

	return NULL;
}

static AggrefTransInfo *
search_aggref_trans_info(Oid oid)
{
	AggrefTransInfo key = {0};
	AggrefTransInfo *res;

	key.fn_oid = oid;

	res = (AggrefTransInfo *) bsearch(&key, function_table,
									  lengthof(function_table), sizeof(function_table[0]),
									  compare_aggref_trans_info);

	return res;
}

static int
compare_aggref_trans_info(const void *p1, const void *p2)
{
	const AggrefTransInfo *info1 = (const AggrefTransInfo *) p1;
	const AggrefTransInfo *info2 = (const AggrefTransInfo *) p2;

	if (info1->fn_oid > info2->fn_oid)
		return +1;
	else if (info1->fn_oid < info2->fn_oid)
		return -1;
	else
		return 0;
}
