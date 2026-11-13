/*-------------------------------------------------------------------------
 *
 * vci_aggref_impl.h
 *	  Templates for specialized advance_aggref functions
 *
 *    This file is included by vci_aggref.c. This template can be used like:
 *
 *        #define VCI_ADVANCE_AGGREF_FUNC     aggref_0input_default
 *        #include "executor/vci_aggref_impl.h"
 *        #undef  VCI_ADVANCE_AGGREF_FUNC *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_aggref.impl.h
 *
 *-------------------------------------------------------------------------
 */

#include "utils/float.h"
#include "datatype/timestamp.h"

#include "vci_executor.h"

static void
VCI_ADVANCE_AGGREF_FUNC(VciAggState *aggstate,
						int aggno,
						VciAggStatePerGroup *entries,
						int max_slots)
{
	MemoryContext oldContext;
	int			slot_index;
	VciScanState *scanstate;
	uint16	   *skip_list;

#if VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_0

	/*
	 * aggref_0input_int8inc does not use these variable, skip the
	 * declaration.
	 */
#if VCI_TRANFN_OID != F_INT8INC
	Datum	   *inputValues = NULL;
	bool	   *inputIsNulls = NULL;
	VciAggStatePerAgg peraggstate = &aggstate->peragg[aggno];
#endif							/* VCI_TRANFN_OID */

#elif VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_1_SIMPLEVAR
	Datum	   *inputValues = NULL;
	bool	   *inputIsNulls = NULL;
	VciAggStatePerAgg peraggstate = &aggstate->peragg[aggno];
	VciProjectionInfo *projInfo;
	int			attno;

#elif VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_1_EVALEXPR
	Datum	   *inputValues = NULL;
	bool	   *inputIsNulls = NULL;
	VciAggStatePerAgg peraggstate = &aggstate->peragg[aggno];
	VciProjectionInfo *projInfo;
	ExprContext *econtext;
	VciVPContext *vpcontext;
#endif							/* SELECT VCI_TRANS_INPUTS_ARG */

	scanstate = (VciScanState *) outerPlanState(aggstate);
	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);
	skip_list = vci_CSGetSkipFromVirtualTuples(scanstate->vector_set);

#if VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_0

#elif VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_1_SIMPLEVAR
	projInfo = peraggstate->evalproj;

	attno = projInfo->pi_varNumbers[0];

	inputValues = scanstate->result_values[attno - 1];
	inputIsNulls = scanstate->result_isnull[attno - 1];

#elif VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_1_EVALEXPR

	projInfo = peraggstate->evalproj;
	econtext = projInfo->pi_exprContext;

	vpcontext = projInfo->pi_vp_tle_array[0];

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	VciExecEvalVectorProcessing(vpcontext, econtext, max_slots);
	MemoryContextSwitchTo(oldContext);

	inputValues = vpcontext->resultValue;
	inputIsNulls = vpcontext->resultIsNull;

#endif							/* SELECT VCI_TRANS_INPUTS_ARG */

	for (slot_index = skip_list[0]; slot_index < max_slots; slot_index += skip_list[slot_index + 1] + 1)
	{
		VciAggStatePerGroup pergroupstate;
		Datum		newVal;
		bool		newIsNull;

		pergroupstate = &(entries[slot_index])[aggno];

		if (VCI_TRANS_FN_STRICT)	/* peraggstate->transfn.fn_strict or 1 or
									 * 0 */
		{
			/*
			 * For a strict transfn, nothing happens when there's a NULL
			 * input; we just keep the prior transValue.
			 */
#if (VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_1_SIMPLEVAR) || (VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_1_EVALEXPR)
			if (inputIsNulls[slot_index])
				continue;
#endif

			if (pergroupstate->noTransValue)
			{
				/*
				 * transValue has not been initialized. This is the first
				 * non-NULL input value. We use it as the initial value for
				 * transValue. (We already checked that the agg's input type
				 * is binary-compatible with its transtype, so straight copy
				 * here is OK.)
				 *
				 * We must copy the datum into aggcontext if it is
				 * pass-by-ref. We do not need to pfree the old transValue,
				 * since it's NULL.
				 */
				oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
#if VCI_TRANS_INPUTS_ARG == VCI_TRANS_INPUTS_0
				pergroupstate->transValue = 0;
#elif VCI_TRANS_TYPE_BYVAL <= 0
				pergroupstate->transValue = datumCopy(inputValues[slot_index],
													  peraggstate->transtypeByVal,
													  peraggstate->transtypeLen);
#else
				pergroupstate->transValue = inputValues[slot_index];
#endif
				pergroupstate->transValueIsNull = false;
				pergroupstate->noTransValue = false;
				MemoryContextSwitchTo(oldContext);
				continue;
			}
			if (pergroupstate->transValueIsNull)
			{
				/*
				 * Don't call a strict function with NULL inputs.  Note it is
				 * possible to get here despite the above tests, if the
				 * transfn is strict *and* returned a NULL on a prior cycle.
				 * If that happens we will propagate the NULL all the way to
				 * the end.
				 */
				continue;
			}
		}

#if VCI_TRANS_TYPE_BYVAL <= 0
		/* We run the transition functions in per-input-tuple memory context */
		oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
#endif

#ifdef VCI_TRANS_USE_CURPERAGG
		/* set up aggstate->curperagg for AggGetAggref() */
		aggstate->pseudo_aggstate->curperagg = (AggStatePerAgg) peraggstate;	/* @remark */
#endif

#if VCI_TRANFN_OID == F_FLOAT4_ACCUM	/* 208 */
		/* float4_accum */
		{
			ArrayType  *transarray = DatumGetArrayTypeP(pergroupstate->transValue);

			float8		newval = DatumGetFloat4(inputValues[slot_index]);
			float8	   *transvalues;
			float8		N,
						Sx,
						Sxx,
						tmp;

			transvalues = check_float8_array(transarray, "float4_accum", 3);
			N = transvalues[0];
			Sx = transvalues[1];
			Sxx = transvalues[2];

			/*
			 * Use the Youngs-Cramer algorithm to incorporate the new value
			 * into the transition values.
			 */

			N += 1.0;
			Sx += newval;
			if (transvalues[0] > 0.0)
			{
				tmp = newval * N - Sx;
				Sxx += tmp * tmp / (N * transvalues[0]);

				/*
				 * Overflow check.  We only report an overflow error when
				 * finite inputs lead to infinite results.  Note also that Sxx
				 * should be NaN if any of the inputs are infinite, so we
				 * intentionally prevent Sxx from becoming infinite.
				 */
				if (isinf(Sx) || isinf(Sxx))
				{
					if (!isinf(transvalues[1]) && !isinf(newval))
						ereport(ERROR,
								(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
								 errmsg("value out of range: overflow")));

					Sxx = get_float8_nan();
				}
			}

			transvalues[0] = N;
			transvalues[1] = Sx;
			transvalues[2] = Sxx;

			newVal = pergroupstate->transValue;
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_FLOAT4PL	/* 204 */
		/* float4pl */
		{
			float4		arg1 = DatumGetFloat4(pergroupstate->transValue);
			float4		arg2 = DatumGetFloat4(inputValues[slot_index]);
			float4		result;

			result = arg1 + arg2;

			CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
			newVal = Float4GetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_FLOAT4LARGER	/* 209 */
		/* float4larger */
		{
			float4		arg1 = DatumGetFloat4(pergroupstate->transValue);
			float4		arg2 = DatumGetFloat4(inputValues[slot_index]);
			float4		result;

			if (float4_gt(arg1, arg2))
				result = arg1;
			else
				result = arg2;
			newVal = Float4GetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_FLOAT4SMALLER /* 211 */
		/* float4smaller */
		{
			float4		arg1 = DatumGetFloat4(pergroupstate->transValue);
			float4		arg2 = DatumGetFloat4(inputValues[slot_index]);
			float4		result;

			if (float4_lt(arg1, arg2))
				result = arg1;
			else
				result = arg2;
			newVal = Float4GetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_FLOAT8PL	/* 218 */
		/* float8pl */
		{
			float8		arg1 = DatumGetFloat8(pergroupstate->transValue);
			float8		arg2 = DatumGetFloat8(inputValues[slot_index]);
			float8		result;

			result = arg1 + arg2;

			CHECKFLOATVAL(result, isinf(arg1) || isinf(arg2), true);
			newVal = Float8GetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT4LARGER	/* 768 */
		/* int4larger */
		{
			int32		arg1 = DatumGetInt32(pergroupstate->transValue);
			int32		arg2 = DatumGetInt32(inputValues[slot_index]);

			newVal = Int32GetDatum((arg1 > arg2) ? arg1 : arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT4SMALLER	/* 769 */
		/* int4smaller */
		{
			int32		arg1 = DatumGetInt32(pergroupstate->transValue);
			int32		arg2 = DatumGetInt32(inputValues[slot_index]);

			newVal = Int32GetDatum((arg1 < arg2) ? arg1 : arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_CASH_PL	/* 894 */
		/* cash_pl */
		{
			Cash		c1 = DatumGetCash(pergroupstate->transValue);
			Cash		c2 = DatumGetCash(inputValues[slot_index]);
			Cash		result;

			result = c1 + c2;

			newVal = CashGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_CASHLARGER	/* 898 */
		/* cashlarger */
		{
			Cash		c1 = DatumGetCash(pergroupstate->transValue);
			Cash		c2 = DatumGetCash(inputValues[slot_index]);
			Cash		result;

			result = (c1 > c2) ? c1 : c2;

			newVal = CashGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_CASHSMALLER	/* 899 */
		/* cashsmaller */
		{
			Cash		c1 = DatumGetCash(pergroupstate->transValue);
			Cash		c2 = DatumGetCash(inputValues[slot_index]);
			Cash		result;

			result = (c1 < c2) ? c1 : c2;

			newVal = CashGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_DATE_LARGER	/* 1138 */
		/* date_larger */
		{
			DateADT		dateVal1 = DatumGetDateADT(pergroupstate->transValue);
			DateADT		dateVal2 = DatumGetDateADT(inputValues[slot_index]);

			newVal = DateADTGetDatum((dateVal1 > dateVal2) ? dateVal1 : dateVal2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_DATE_SMALLER	/* 1139 */
		/* date_smaller */
		{
			DateADT		dateVal1 = DatumGetDateADT(pergroupstate->transValue);
			DateADT		dateVal2 = DatumGetDateADT(inputValues[slot_index]);

			newVal = DateADTGetDatum((dateVal1 < dateVal2) ? dateVal1 : dateVal2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INTERVAL_PL	/* 1169 */
		/* interval_pl */
		{
			Interval   *span1 = DatumGetIntervalP(pergroupstate->transValue);
			Interval   *span2 = DatumGetIntervalP(inputValues[slot_index]);
			Interval   *result;

			result = palloc_object(Interval);

			/*
			 * Handle infinities.
			 *
			 * We treat anything that amounts to "infinity - infinity" as an
			 * error, since the interval type has nothing equivalent to NaN.
			 */
			if (INTERVAL_IS_NOBEGIN(span1))
			{
				if (INTERVAL_IS_NOEND(span2))
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("interval out of range")));
				else
					INTERVAL_NOBEGIN(result);
			}
			else if (INTERVAL_IS_NOEND(span1))
			{
				if (INTERVAL_IS_NOBEGIN(span2))
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("interval out of range")));
				else
					INTERVAL_NOEND(result);
			}
			else if (INTERVAL_NOT_FINITE(span2))
				memcpy(result, span2, sizeof(Interval));
			else
				finite_interval_pl(span1, span2, result);

			newVal = IntervalPGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_TIMESTAMP_SMALLER /* 1195 */
		/* timestamp_smaller */
		{
			Timestamp	dt1 = DatumGetTimestamp(pergroupstate->transValue);
			Timestamp	dt2 = DatumGetTimestamp(inputValues[slot_index]);
			Timestamp	result;

			/*
			 * use timestamp_cmp_internal to be sure this agrees with
			 * comparisons
			 */
			if (timestamp_cmp_internal(dt1, dt2) < 0)
				result = dt1;
			else
				result = dt2;
			newVal = TimestampGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_TIMESTAMP_LARGER	/* 1196 */
		/* timestamp_larger */
		{
			Timestamp	dt1 = DatumGetTimestamp(pergroupstate->transValue);
			Timestamp	dt2 = DatumGetTimestamp(inputValues[slot_index]);
			Timestamp	result;

			if (timestamp_cmp_internal(dt1, dt2) > 0)
				result = dt1;
			else
				result = dt2;
			newVal = TimestampGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INTERVAL_SMALLER	/* 1197 */
		/* interval_smaller */
		{
			Interval   *interval1 = DatumGetIntervalP(pergroupstate->transValue);
			Interval   *interval2 = DatumGetIntervalP(inputValues[slot_index]);
			Interval   *result;

			/*
			 * use interval_cmp_internal to be sure this agrees with
			 * comparisons
			 */
			if (interval_cmp_internal(interval1, interval2) < 0)
				result = interval1;
			else
				result = interval2;
			newVal = IntervalPGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INTERVAL_LARGER	/* 1198 */
		/* interval_larger */
		{
			Interval   *interval1 = DatumGetIntervalP(pergroupstate->transValue);
			Interval   *interval2 = DatumGetIntervalP(inputValues[slot_index]);
			Interval   *result;

			if (interval_cmp_internal(interval1, interval2) > 0)
				result = interval1;
			else
				result = interval2;
			newVal = IntervalPGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT8INC	/* 1219 */
		{
			newVal = Int64GetDatum(DatumGetInt64(pergroupstate->transValue) + 1);

			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT8INC_ANY	/* 2804 */
		/* Mostly same as F_INT8INC, but NULL-check for arguments is done */
		{
			newVal = Int64GetDatum(DatumGetInt64(pergroupstate->transValue) + 1);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_TIME_LARGER	/* 1377 */
		/* time_larger */
		{
			TimeADT		time1 = DatumGetTimeADT(pergroupstate->transValue);
			TimeADT		time2 = DatumGetTimeADT(inputValues[slot_index]);

			newVal = TimeADTGetDatum((time1 > time2) ? time1 : time2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_TIME_SMALLER	/* 1378 */
		/* time_smaller */
		{
			TimeADT		time1 = DatumGetTimeADT(pergroupstate->transValue);
			TimeADT		time2 = DatumGetTimeADT(inputValues[slot_index]);

			newVal = TimeADTGetDatum((time1 < time2) ? time1 : time2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_TIMETZ_LARGER /* 1379 */
		/* timetz_larger */
		{
			TimeTzADT  *time1 = DatumGetTimeTzADTP(pergroupstate->transValue);
			TimeTzADT  *time2 = DatumGetTimeTzADTP(inputValues[slot_index]);
			TimeTzADT  *result;

			if (timetz_cmp_internal(time1, time2) > 0)
				result = time1;
			else
				result = time2;
			newVal = TimeTzADTPGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_TIMETZ_SMALLER	/* 1380 */
		/* timetz_smaller */
		{
			TimeTzADT  *time1 = DatumGetTimeTzADTP(pergroupstate->transValue);
			TimeTzADT  *time2 = DatumGetTimeTzADTP(inputValues[slot_index]);
			TimeTzADT  *result;

			if (timetz_cmp_internal(time1, time2) < 0)
				result = time1;
			else
				result = time2;
			newVal = TimeTzADTPGetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT2_SUM	/* 1840 */
		/* int2_sum */
		{
			int64		newval;

			newIsNull = false;
			if (pergroupstate->transValueIsNull)
			{
				if (inputIsNulls[slot_index])
				{
					newval = 0;
					newIsNull = true;
				}
				else
					newval = (int64) DatumGetInt16(inputValues[slot_index]);
			}
			else
			{
				int64		oldsum = DatumGetInt64(pergroupstate->transValue);

				if (inputIsNulls[slot_index])
					newval = oldsum;
				else
					newval = oldsum + (int64) DatumGetInt16(inputValues[slot_index]);
			}
			newVal = Int64GetDatum(newval);
		}

#elif VCI_TRANFN_OID == F_INT4_SUM	/* 1841 */
		/* int4_sum */
		{
			int64		newval;

			newIsNull = false;
			if (pergroupstate->transValueIsNull)
			{
				if (inputIsNulls[slot_index])
				{
					newval = 0;
					newIsNull = true;
				}
				else
					newval = (int64) DatumGetInt32(inputValues[slot_index]);
			}
			else
			{
				int64		oldsum = DatumGetInt64(pergroupstate->transValue);

				if (inputIsNulls[slot_index])
					newval = oldsum;
				else
					newval = oldsum + (int64) DatumGetInt32(inputValues[slot_index]);
			}
			newVal = Int64GetDatum(newval);
		}

#elif VCI_TRANFN_OID == F_INT4AND	/* 1898 */
		/* int4and */
		{
			int32		arg1 = DatumGetInt32(pergroupstate->transValue);
			int32		arg2 = DatumGetInt32(inputValues[slot_index]);

			newVal = Int32GetDatum(arg1 & arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT4OR	/* 1899 */
		/* int4or */
		{
			int32		arg1 = DatumGetInt32(pergroupstate->transValue);
			int32		arg2 = DatumGetInt32(inputValues[slot_index]);

			newVal = Int32GetDatum(arg1 | arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT4_AVG_ACCUM	/* 1963 */
		/* int4_avg_accum */
		{
			ArrayType  *transarray = DatumGetArrayTypeP(pergroupstate->transValue);
			int32		newval = DatumGetInt32(inputValues[slot_index]);
			Int8TransTypeData *transdata;

			if (ARR_HASNULL(transarray) ||
				ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
				elog(ERROR, "expected 2-element int8 array");

			transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);
			transdata->count++;
			transdata->sum += newval;

			newVal = pergroupstate->transValue;
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_BOOLAND_STATEFUNC /* 2515 */
		/* booland_statefunc */
		{
			newVal = BoolGetDatum(
								  DatumGetBool(pergroupstate->transValue) && DatumGetBool(inputValues[slot_index]));
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_BOOLOR_STATEFUNC	/* 2516 */
		/* boolor_statefunc */
		{
			newVal = BoolGetDatum(
								  DatumGetBool(pergroupstate->transValue) || DatumGetBool(inputValues[slot_index]));
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT2LARGER	/* 770 */
		/* int2larger */
		{
			int16		arg1 = DatumGetInt16(pergroupstate->transValue);
			int16		arg2 = DatumGetInt16(inputValues[slot_index]);

			newVal = Int16GetDatum((arg1 > arg2) ? arg1 : arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT2SMALLER	/* 771 */
		/* int2smaller */
		{
			int16		arg1 = DatumGetInt16(pergroupstate->transValue);
			int16		arg2 = DatumGetInt16(inputValues[slot_index]);

			newVal = Int16GetDatum((arg1 < arg2) ? arg1 : arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT2AND	/* 1892 */
		/* int2and */
		{
			int16		arg1 = DatumGetInt16(pergroupstate->transValue);
			int16		arg2 = DatumGetInt16(inputValues[slot_index]);

			newVal = Int16GetDatum(arg1 & arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT2OR	/* 1893 */
		/* int2or */
		{
			int16		arg1 = DatumGetInt16(pergroupstate->transValue);
			int16		arg2 = DatumGetInt16(inputValues[slot_index]);

			newVal = Int16GetDatum(arg1 | arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT2_AVG_ACCUM	/* 1962 */
		/* int2_avg_accum */
		{
			ArrayType  *transarray = DatumGetArrayTypeP(pergroupstate->transValue);
			int16		newval = DatumGetInt16(inputValues[slot_index]);
			Int8TransTypeData *transdata;

			if (ARR_HASNULL(transarray) ||
				ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
				elog(ERROR, "expected 2-element int8 array");

			transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);
			transdata->count++;
			transdata->sum += newval;

			newVal = pergroupstate->transValue;
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT8LARGER	/* 1236 */
		/* int8larger */
		{
			int64		arg1 = DatumGetInt64(pergroupstate->transValue);
			int64		arg2 = DatumGetInt64(inputValues[slot_index]);

			newVal = Int64GetDatum((arg1 > arg2) ? arg1 : arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT8SMALLER	/* 1237 */
		/* int8smaller */
		{
			int64		arg1 = DatumGetInt64(pergroupstate->transValue);
			int64		arg2 = DatumGetInt64(inputValues[slot_index]);

			newVal = Int64GetDatum((arg1 < arg2) ? arg1 : arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT8AND	/* 1904 */
		/* int8and */
		{
			int64		arg1 = DatumGetInt64(pergroupstate->transValue);
			int64		arg2 = DatumGetInt64(inputValues[slot_index]);

			newVal = Int64GetDatum(arg1 & arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_INT8OR	/* 1905 */
		/* int8or */
		{
			int64		arg1 = DatumGetInt64(pergroupstate->transValue);
			int64		arg2 = DatumGetInt64(inputValues[slot_index]);

			newVal = Int64GetDatum(arg1 | arg2);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_FLOAT8_ACCUM	/* 222 */
		/* float8_accum */
		{
			ArrayType  *transarray = DatumGetArrayTypeP(pergroupstate->transValue);

			float8		newval = DatumGetFloat8(inputValues[slot_index]);
			float8	   *transvalues;
			float8		N,
						Sx,
						Sxx,
						tmp;

			transvalues = check_float8_array(transarray, "float8_accum", 3);
			N = transvalues[0];
			Sx = transvalues[1];
			Sxx = transvalues[2];

			N += 1.0;
			Sx += newval;
			if (transvalues[0] > 0.0)
			{
				tmp = newval * N - Sx;
				Sxx += tmp * tmp / (N * transvalues[0]);

				/*
				 * Overflow check.  We only report an overflow error when
				 * finite inputs lead to infinite results.  Note also that Sxx
				 * should be NaN if any of the inputs are infinite, so we
				 * intentionally prevent Sxx from becoming infinite.
				 */
				if (isinf(Sx) || isinf(Sxx))
				{
					if (!isinf(transvalues[1]) && !isinf(newval))
						ereport(ERROR,
								(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
								 errmsg("value out of range: overflow")));

					Sxx = get_float8_nan();
				}
			}

			transvalues[0] = N;
			transvalues[1] = Sx;
			transvalues[2] = Sxx;

			newVal = pergroupstate->transValue;
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_FLOAT8LARGER	/* 223 */
		/* float8larger */
		{
			float8		arg1 = DatumGetFloat8(pergroupstate->transValue);
			float8		arg2 = DatumGetFloat8(inputValues[slot_index]);
			float8		result;

			if (float8_cmp_internal(arg1, arg2) > 0)
				result = arg1;
			else
				result = arg2;
			newVal = Float8GetDatum(result);
			newIsNull = false;
		}

#elif VCI_TRANFN_OID == F_FLOAT8SMALLER /* 224 */
		/* float8smaller */
		{
			float8		arg1 = DatumGetFloat8(pergroupstate->transValue);
			float8		arg2 = DatumGetFloat8(inputValues[slot_index]);
			float8		result;

			if (float8_cmp_internal(arg1, arg2) < 0)
				result = arg1;
			else
				result = arg2;
			newVal = Float8GetDatum(result);
			newIsNull = false;
		}

#else							/* default */
		{
			FunctionCallInfo fcinfo = peraggstate->transfn_fcinfo;

			fcinfo->args[0].value = pergroupstate->transValue;
			fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
			fcinfo->args[1].value = inputValues[slot_index];
			fcinfo->args[1].isnull = inputIsNulls[slot_index];
			fcinfo->isnull = false;
			newVal = FunctionCallInvoke(fcinfo);
			newIsNull = fcinfo->isnull;
		}
#endif

#ifdef VCI_TRANS_USE_CURPERAGG
		aggstate->pseudo_aggstate->curperagg = NULL;
#endif

#if VCI_TRANS_TYPE_BYVAL == -1
		if (!peraggstate->transtypeByVal &&
			DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
		{
			if (!newIsNull)
			{
				MemoryContextSwitchTo(aggstate->aggcontext);
				newVal = datumCopy(newVal,
								   peraggstate->transtypeByVal,
								   peraggstate->transtypeLen);
			}
			if (!pergroupstate->transValueIsNull)
				pfree(DatumGetPointer(pergroupstate->transValue));
		}
#elif VCI_TRANS_TYPE_BYVAL == 0
		if (DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
		{
			if (!newIsNull)
			{
				MemoryContextSwitchTo(aggstate->aggcontext);
				newVal = datumCopy(newVal,
								   peraggstate->transtypeByVal,
								   peraggstate->transtypeLen);
			}
			if (!pergroupstate->transValueIsNull)
				pfree(DatumGetPointer(pergroupstate->transValue));
		}
#endif

		pergroupstate->transValue = newVal;
		pergroupstate->transValueIsNull = newIsNull;

#if VCI_TRANS_TYPE_BYVAL <= 0
		MemoryContextSwitchTo(oldContext);
#endif
	}
}
