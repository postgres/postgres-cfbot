/*-------------------------------------------------------------------------
 *
 * join_mcv.c
 *	  POSTGRES join MCV lists
 *
 * Join MCV statistics capture correlations between columns across an
 * equijoin.  The "anchor" relation owns the statistics object (stxrelid)
 * and is sampled during ANALYZE; the "other" relation (stxjoinrels) is
 * probed via index lookup to build a weighted sample of the join result.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/join_mcv.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_index.h"
#include "catalog/pg_statistic_ext.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/selfuncs.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"



/*
 * find_index_for_operator
 *		Find an index that supports lookup on the given column using
 *		the specified operator.
 *
 * Checks that the operator belongs to the index's opfamily for the
 * target column.  Works for btree, hash, and any other AM that supports
 * amgettuple and registers the operator in its opfamily.  Partial indexes
 * are excluded.
 *
 * Returns the index OID, or InvalidOid if no suitable index found.
 * Prefers unique indexes, then fewer columns.
 */
Oid
find_index_for_operator(Relation rel, AttrNumber attnum, Oid eq_op)
{
	List	   *indexlist;
	ListCell   *lc;
	Oid			best_idx = InvalidOid;
	bool		best_is_unique = false;
	int			best_nattnums = INT_MAX;

	indexlist = RelationGetIndexList(rel);

	foreach(lc, indexlist)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	idxrel;
		Form_pg_index idxform;
		Oid			opfamily;

		idxrel = index_open(indexoid, AccessShareLock);
		idxform = idxrel->rd_index;

		/*
		 * The index must be valid, ready, live, and non-partial (partial
		 * indexes only contain rows matching their predicate, which would
		 * bias the sample).  The target column must be the leading key column
		 * so the AM can look up by it (a multi-column index is fine).  The AM
		 * must support amgettuple for single-tuple scans (this excludes BRIN,
		 * which only supports bitmap scans).  The index's opfamily must
		 * contain our equality operator.
		 */
		if (idxform->indisvalid && idxform->indisready &&
			idxform->indislive &&
			heap_attisnull(idxrel->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			idxform->indnkeyatts >= 1 &&
			idxform->indkey.values[0] == attnum &&
			idxrel->rd_indam->amgettuple != NULL)
		{
			opfamily = idxrel->rd_opfamily[0];
			if (op_in_opfamily(eq_op, opfamily))
			{
				if (!OidIsValid(best_idx) ||
					(!best_is_unique && idxform->indisunique) ||
					(idxform->indisunique == best_is_unique &&
					 idxform->indnkeyatts < best_nattnums))
				{
					best_idx = indexoid;
					best_is_unique = idxform->indisunique;
					best_nattnums = idxform->indnkeyatts;
				}
			}
		}

		index_close(idxrel, AccessShareLock);
	}

	list_free(indexlist);
	return best_idx;
}

/*
 * sample_index -- Build a weighted sample of the join result using an index.
 *
 * Implements Algorithm 1 from Leis et al., CIDR 2017, Section 3.1.  Given
 * sample tuples S from the anchor table and an index I on the other table's
 * join column, this produces a representative sample of the join result.
 *
 * For each tuple in S, we probe the index to count matching tuples, producing
 * a "count per tuple" (cpt) array and the total match count (sum).
 *
 * We then sample min(sum, n) positions from [0, sum), where n is the maximum
 * sample size requested by the caller.  The cpt counts define contiguous
 * intervals in [0, sum); each sampled position falls into exactly one
 * interval, identifying both the cpt entry and the offset within it.
 *
 * For each sampled position, we re-probe the index for the corresponding
 * sample tuple and advance to the target offset to fetch the matching row.
 * The requested columns are extracted from each fetched tuple.
 *
 * When sum <= n every match is collected without sampling.
 *
 * Returns a StatsBuildData with extracted column values from the matched
 * side, or NULL if no matches found.  *result_sample_indices is set to an
 * array mapping each output row to its input tuple index in sample_tuples,
 * so the caller can extract columns from the anchor side as well.
 */
static StatsBuildData *
sample_index(HeapTuple *sample_tuples, int nsample, TupleDesc sample_desc,
			 AttrNumber joinkey_attnum,
			 Relation heap_rel, Relation idx_rel,
			 int idx_strategy,
			 int n,
			 AttrNumber *extract_attnums, int16 *extract_typlens,
			 bool *extract_typbyvals, int nextract,
			 int **result_sample_indices)
{
	IndexScanDesc iscan;
	ScanKeyData skey[1];
	TupleTableSlot *slot;
	Snapshot	snapshot;
	bool		pushed_snapshot = false;
	int			i;
	int			sid_idx;
	int64		prefix_sum;
	int			nresults;
	int		   *sample_indices; /* maps each output row to its input index */

	/* cpt ("count per tuple") = sequence of (tuple_index, count) pairs */
	int64	   *cpt_count;
	int		   *cpt_tupleidx;
	int			cpt_len;
	int64		sum;

	/* sid = sampled positions (Algorithm S variables) */
	int64	   *sid;
	int			sid_len;
	int64		t;				/* population elements scanned */
	int			m;				/* elements selected so far */

	/* result_data = output data */
	StatsBuildData *result_data;

	/* scan key derivation */
	Oid			anchor_type;
	Oid			scan_eq_op;
	RegProcedure eq_proc;

	/* Derive the index-perspective equality operator and comparison proc. */
	anchor_type = TupleDescAttr(sample_desc, joinkey_attnum - 1)->atttypid;
	scan_eq_op = get_opfamily_member(idx_rel->rd_opfamily[0],
									 idx_rel->rd_opcintype[0],
									 anchor_type,
									 idx_strategy);
	if (!OidIsValid(scan_eq_op))
		elog(ERROR, "could not find equality operator for types %u and %u in opfamily %u",
			 idx_rel->rd_opcintype[0], anchor_type, idx_rel->rd_opfamily[0]);
	eq_proc = get_opcode(scan_eq_op);

	/*
	 * The index scan needs an active snapshot for tuple visibility.  The
	 * caller (vacuum.c) always pushes one before we get here, but be
	 * defensive in case this is ever called from another path.
	 */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_snapshot = true;
	}
	snapshot = GetActiveSnapshot();

	slot = MakeSingleTupleTableSlot(RelationGetDescr(heap_rel),
									&TTSOpsBufferHeapTuple);
	iscan = index_beginscan(heap_rel, idx_rel, snapshot, NULL, 1, 0, 0);

	/* Count matching tuples per sample tuple to build the cpt array. */
	cpt_count = palloc_array(int64, nsample);
	cpt_tupleidx = palloc_array(int, nsample);
	sum = 0;
	cpt_len = 0;

	for (i = 0; i < nsample; i++)
	{
		Datum		joinkey_value;
		bool		joinkey_isnull;
		int64		count;

		joinkey_value = heap_getattr(sample_tuples[i], joinkey_attnum, sample_desc, &joinkey_isnull);
		if (joinkey_isnull)
			continue;

		/* I.lookup(t).count */
		ScanKeyEntryInitialize(&skey[0], 0, 1, idx_strategy, anchor_type,
							   idx_rel->rd_indcollation[0],
							   eq_proc, joinkey_value);
		index_rescan(iscan, skey, 1, NULL, 0);

		count = 0;
		while (index_getnext_slot(iscan, ForwardScanDirection, slot))
		{
			/* skip if AM requires recheck and tuple doesn't actually match */
			if (iscan->xs_recheck)
			{
				Datum		val;
				bool		isnull;

				val = slot_getattr(slot,
								   idx_rel->rd_index->indkey.values[0],
								   &isnull);
				if (isnull ||
					!DatumGetBool(FunctionCall2Coll(&skey[0].sk_func,
													skey[0].sk_collation,
													val,
													skey[0].sk_argument)))
					continue;
			}
			count++;
		}

		cpt_count[cpt_len] = count;
		cpt_tupleidx[cpt_len] = i;
		sum += count;
		cpt_len++;
	}

	if (sum == 0)
	{
		index_endscan(iscan);
		ExecDropSingleTupleTableSlot(slot);
		if (pushed_snapshot)
			PopActiveSnapshot();
		pfree(cpt_count);
		pfree(cpt_tupleidx);
		return NULL;
	}

	/*
	 * "sid = sample non-negative integers < sum, |sid| = min{sum, n}"
	 *
	 * When sum <= n, sid = {0, 1, ..., sum-1} -- every position is selected.
	 */
	sid_len = Min(sum, n);

	if (sum <= n)
	{
		sid = palloc_array(int64, sid_len);
		for (i = 0; i < sid_len; i++)
			sid[i] = i;
	}
	else
	{
		/*
		 * Draw positions without replacement using Knuth's Algorithm S (Knuth
		 * 3.4.2).  This is the same algorithm used by BlockSampler for
		 * table-block sampling.  The output is naturally sorted, so no
		 * separate qsort step is needed.
		 */
		sid = palloc_array(int64, sid_len);
		t = 0;
		m = 0;

		while (m < sid_len)
		{
			int64		K = sum - t;	/* remaining positions */
			int			k = sid_len - m;	/* samples still needed */
			double		p;		/* probability to skip position */
			double		V;		/* random */

			Assert(sum > t && sid_len > m); /* hence K > 0 and k > 0 */

			if (k >= K)
			{
				/* need all the rest */
				sid[m++] = t++;
			}
			else
			{
				V = sampler_random_fract(&pg_global_prng_state);
				p = 1.0 - (double) k / (double) K;

				while (V < p)
				{
					/* skip */
					t++;
					K--;		/* keep K == sum - t */

					/* adjust p to be new cutoff point in reduced range */
					p *= 1.0 - (double) k / (double) K;
				}

				/* select */
				sid[m++] = t++;
			}
		}
	}

	sample_indices = palloc_array(int, sid_len);
	result_data = palloc(sizeof(StatsBuildData));
	result_data->nattnums = nextract;
	result_data->numrows = 0;
	result_data->attnums = palloc_array(AttrNumber, nextract);
	result_data->stats = palloc0_array(VacAttrStats *, nextract);
	result_data->values = palloc_array(Datum *, nextract);
	result_data->nulls = palloc_array(bool *, nextract);

	for (i = 0; i < nextract; i++)
	{
		result_data->attnums[i] = extract_attnums[i];
		result_data->values[i] = palloc_array(Datum, sid_len);
		result_data->nulls[i] = palloc_array(bool, sid_len);
	}

	/*
	 * Walk cpt and sid in parallel (both sorted by position) using a running
	 * prefix sum to identify the cpt entry and offset for each sampled
	 * position, then fetch the matching tuple from the index.
	 */
	nresults = 0;
	sid_idx = 0;
	prefix_sum = 0;

	for (i = 0; i < cpt_len && sid_idx < sid_len; i++)
	{
		/* this cpt entry owns positions [prefix_sum, next_prefix) */
		int64		next_prefix = prefix_sum + cpt_count[i];
		int64		offset;
		Datum		joinkey_value;
		bool		joinkey_isnull;
		int64		j;
		int			f;
		bool		scan_started;
		bool		scan_exhausted;

		scan_started = false;
		scan_exhausted = false;
		j = -1;

		while (sid_idx < sid_len && sid[sid_idx] < next_prefix)
		{
			/* offset within this cpt entry's interval */
			offset = sid[sid_idx] - prefix_sum;

			/*
			 * If a previous advance within this cpt entry ran out of tuples,
			 * all later offsets (which are larger) will too.
			 */
			if (scan_exhausted)
			{
				sid_idx++;
				continue;
			}

			/*
			 * Start the index scan on the first sid for this cpt entry;
			 * subsequent sids reuse the open scan.
			 */
			if (!scan_started)
			{
				joinkey_value = heap_getattr(sample_tuples[cpt_tupleidx[i]],
											 joinkey_attnum, sample_desc,
											 &joinkey_isnull);
				Assert(!joinkey_isnull);

				ScanKeyEntryInitialize(&skey[0], 0, 1, idx_strategy,
									   anchor_type,
									   idx_rel->rd_indcollation[0],
									   eq_proc, joinkey_value);
				index_rescan(iscan, skey, 1, NULL, 0);
				scan_started = true;
			}

			/* Advance to the target offset */
			while (index_getnext_slot(iscan, ForwardScanDirection, slot))
			{
				/* skip if AM requires recheck and tuple doesn't match */
				if (iscan->xs_recheck)
				{
					Datum		val;
					bool		valisnull;

					val = slot_getattr(slot,
									   idx_rel->rd_index->indkey.values[0],
									   &valisnull);
					if (valisnull ||
						!DatumGetBool(FunctionCall2Coll(&skey[0].sk_func,
														skey[0].sk_collation,
														val,
														skey[0].sk_argument)))
						continue;
				}
				j++;
				if (j >= offset)
					break;
			}

			if (j >= offset)
			{
				/* Extract requested columns from tA */
				for (f = 0; f < nextract; f++)
				{
					result_data->values[f][nresults] =
						slot_getattr(slot, extract_attnums[f],
									 &result_data->nulls[f][nresults]);

					if (!result_data->nulls[f][nresults] && !extract_typbyvals[f])
						result_data->values[f][nresults] =
							datumCopy(result_data->values[f][nresults],
									  extract_typbyvals[f],
									  extract_typlens[f]);
				}

				/* Record which input tuple this output row came from */
				sample_indices[nresults] = cpt_tupleidx[i];
				nresults++;
			}
			else
			{
				/* Scan exhausted; skip remaining sids for this entry */
				scan_exhausted = true;
			}

			sid_idx++;
		}

		prefix_sum = next_prefix;
	}

	result_data->numrows = nresults;

	pfree(sid);
	pfree(cpt_count);
	pfree(cpt_tupleidx);
	index_endscan(iscan);
	ExecDropSingleTupleTableSlot(slot);

	if (pushed_snapshot)
		PopActiveSnapshot();

	if (nresults == 0)
	{
		pfree(sample_indices);
		return NULL;
	}

	*result_sample_indices = sample_indices;
	return result_data;
}

/*
 * statext_join_mcv_build
 *		Build join MCV statistics using index-based join sampling.
 *
 * Samples the join between the anchor table and the other table by scanning
 * anchor rows and probing via index lookup, then feeds the merged sample
 * into statext_mcv_build() to produce a standard MCVList.
 *
 * joinrel_oids is anchor-first (joinrel_oids[0] == anchor_rel, already open);
 * joinrel_oids[1..] are the relations to probe.  njoinrels counts all
 * participating relations, including the anchor.
 *
 * Currently only 2-way joins are supported; returns NULL for n-way
 * (njoinrels > 2).
 */
MCVList *
statext_join_mcv_build(Relation anchor_rel,
					   Oid *joinrel_oids,
					   int njoinrels,
					   int16 *joinleft,
					   int16 *joinright,
					   Oid *joinops,
					   int njoinquals,
					   int16 *stxkeys,
					   int16 *keyrefs,
					   int nkeys,
					   int stattarget,
					   int numrows, HeapTuple *rows,
					   double totalrows,
					   VacAttrStats ***result_stats)
{
	Relation	other_rel;
	Oid			idx_oid;
	Relation	idx_rel;
	TupleDesc	other_desc;
	TupleDesc	anchor_desc;
	int			nfilters;
	AttrNumber *other_attnums;
	int16	   *other_typlens;
	bool	   *other_typbyvals;
	int			nother;
	StatsBuildData *data;
	StatsBuildData *merged;
	int			other_col;
	MCVList    *mcv;
	int			i;
	int			n;
	int		   *sample_indices;

	/* Currently only 2-way joins are supported for collection */
	if (njoinrels != 2)
		return NULL;

	if (njoinquals < 1)
		return NULL;

	/* Open the other table (joinrel_oids[0] is the anchor; [1] is probed) */
	other_rel = table_open(joinrel_oids[1], AccessShareLock);
	other_desc = RelationGetDescr(other_rel);

	/* Find an index I that supports equality lookup with our operator */
	idx_oid = find_index_for_operator(other_rel, joinright[0], joinops[0]);
	if (!OidIsValid(idx_oid))
	{
		ereport(WARNING,
				(errmsg("no suitable index on \"%s\" column \"%s\" for join statistics",
						RelationGetRelationName(other_rel),
						NameStr(TupleDescAttr(other_desc, joinright[0] - 1)->attname))));
		table_close(other_rel, AccessShareLock);
		return NULL;
	}

	idx_rel = index_open(idx_oid, AccessShareLock);

	/*
	 * Split stxkey columns into anchor-side and other-side.  keyrefs[i]
	 * identifies which relation stxkeys[i] belongs to: 1 = anchor (stxrelid),
	 * 2 = first joinrel (stxjoinrels[0]), etc.  sample_index can only extract
	 * from the probed table; anchor-side columns are extracted afterwards
	 * using sample_indices.
	 */
	nfilters = nkeys;
	anchor_desc = RelationGetDescr(anchor_rel);
	nother = 0;

	other_attnums = palloc_array(AttrNumber, nfilters);
	other_typlens = palloc_array(int16, nfilters);
	other_typbyvals = palloc_array(bool, nfilters);

	for (i = 0; i < nkeys; i++)
	{
		AttrNumber	attnum = stxkeys[i];

		if (keyrefs[i] == 1)
			continue;			/* anchor-side; extracted later */

		/* Other-side column */
		other_attnums[nother] = attnum;
		other_typlens[nother] = TupleDescAttr(other_desc, attnum - 1)->attlen;
		other_typbyvals[nother] = TupleDescAttr(other_desc, attnum - 1)->attbyval;
		nother++;
	}

	/*
	 * Run Algorithm 1: sample the join, extracting other-side columns.
	 * Request stattarget * 100 rows to give statext_mcv_build() enough data
	 * to identify the most common values reliably.
	 */
	n = stattarget * 100;
	data = sample_index(rows, numrows, anchor_desc,
						joinleft[0],
						other_rel, idx_rel,
						get_op_opfamily_strategy(joinops[0],
												 idx_rel->rd_opfamily[0]),
						n,
						other_attnums, other_typlens,
						other_typbyvals, nother,
						&sample_indices);

	index_close(idx_rel, AccessShareLock);

	if (data == NULL)
	{
		table_close(other_rel, AccessShareLock);
		return NULL;
	}

	/*
	 * Rebuild StatsBuildData in stxkeys order with virtual column numbers.
	 *
	 * The join result is a virtual relation.  Each stxkeys position gets a
	 * virtual column number (1, 2, 3, ...).  sample_index produced data for
	 * other-side columns; we extract anchor-side columns from the ANALYZE
	 * sample using sample_indices.  The final data is ordered by stxkeys
	 * position, ensuring unique attnums regardless of base-table attnum
	 * collisions.
	 */
	other_col = 0;
	merged = palloc(sizeof(StatsBuildData));
	merged->nattnums = nkeys;
	merged->numrows = data->numrows;
	merged->attnums = palloc_array(AttrNumber, nkeys);
	merged->stats = palloc0_array(VacAttrStats *, nkeys);
	merged->values = palloc_array(Datum *, nkeys);
	merged->nulls = palloc_array(bool *, nkeys);

	for (i = 0; i < nkeys; i++)
	{
		AttrNumber	base_attnum = stxkeys[i];
		VacAttrStats *s;
		Form_pg_attribute attr;
		TupleDesc	col_desc;
		HeapTuple	typtuple;
		int			r;

		/* Virtual column number: 1-based position in stxkeys */
		merged->attnums[i] = i + 1;

		if (keyrefs[i] == 1)
		{
			/* Anchor-side: extract from rows[] via sample_indices */
			col_desc = anchor_desc;
			attr = TupleDescAttr(anchor_desc, base_attnum - 1);

			merged->values[i] = palloc_array(Datum, data->numrows);
			merged->nulls[i] = palloc_array(bool, data->numrows);

			for (r = 0; r < data->numrows; r++)
			{
				merged->values[i][r] =
					heap_getattr(rows[sample_indices[r]],
								 base_attnum,
								 anchor_desc,
								 &merged->nulls[i][r]);

				if (!merged->nulls[i][r] && !attr->attbyval)
					merged->values[i][r] =
						datumCopy(merged->values[i][r],
								  attr->attbyval, attr->attlen);
			}
		}
		else
		{
			/* Other-side: already extracted by sample_index */
			col_desc = other_desc;
			attr = TupleDescAttr(other_desc, base_attnum - 1);

			merged->values[i] = data->values[other_col];
			merged->nulls[i] = data->nulls[other_col];
			other_col++;
		}

		/* Build VacAttrStats with virtual attnum */
		s = palloc0(sizeof(VacAttrStats));
		s->tupDesc = col_desc;
		s->tupattnum = i + 1;	/* virtual column number */
		s->attrtypid = attr->atttypid;
		s->attrtypmod = attr->atttypmod;
		s->attrcollid = attr->attcollation;

		typtuple = SearchSysCache1(TYPEOID,
								   ObjectIdGetDatum(attr->atttypid));
		if (!HeapTupleIsValid(typtuple))
			elog(ERROR, "cache lookup failed for type %u", attr->atttypid);
		s->attrtype = (Form_pg_type) palloc(sizeof(FormData_pg_type));
		memcpy(s->attrtype, GETSTRUCT(typtuple),
			   sizeof(FormData_pg_type));
		ReleaseSysCache(typtuple);
		merged->stats[i] = s;
	}

	table_close(other_rel, AccessShareLock);

	if (sample_indices)
		pfree(sample_indices);
	pfree(other_attnums);
	pfree(other_typlens);
	pfree(other_typbyvals);

	if (result_stats)
		*result_stats = merged->stats;

	mcv = statext_mcv_build(merged, totalrows, stattarget);

	return mcv;
}

/*
 * join_mcv_clauselist_selectivity -
 *	  Apply join MCV statistics to estimate selectivity.
 *
 * Given a join MCVList and filter values, compute the selectivity by summing
 * the frequencies of MCV items that satisfy the filters. stxkey_indexes
 * contains 0-based stxkey positions, as produced by match_join_stat().
 *
 * For single-column queries on multi-column stats, we compute the
 * marginal distribution by summing frequencies across non-queried
 * dimensions.
 *
 * For multi-column queries, an MCV item matches when every queried dimension
 * satisfies its filter's operator.
 *
 * Unlike the single-table MCV code, where an item's frequency is a fraction of
 * a single table's rows, here it is a fraction of the sampled join result.
 *
 * all_equality is true when every covered filter is an equality (or IN); the
 * non-MCV selectivity below is added only in that case.
 */
static Selectivity
join_mcv_clauselist_selectivity(MCVList *mcvlist,
								List *filter_values,
								List *stxkey_indexes,
								List *filter_collations,
								List *filter_opnos,
								List *filter_expronlefts,
								bool all_equality,
								double ndistinct)
{
	int			item_idx;
	Selectivity total_sel;
	Selectivity mcv_totalsel;
	int			nkeys;
	int			i;
	FmgrInfo   *op_funcs;
	FunctionCallInfo *fcinfo_arr;
	int		   *var_argnos;
	int		   *const_argnos;
	bool	   *matched;

	if (!mcvlist || mcvlist->nitems == 0 || filter_values == NIL || stxkey_indexes == NIL)
		return 0.0;

	nkeys = list_length(stxkey_indexes);

	if (nkeys > STATS_MAX_DIMENSIONS)
		return 0.0;

	op_funcs = palloc_array(FmgrInfo, nkeys);
	fcinfo_arr = palloc_array(FunctionCallInfo, nkeys);
	var_argnos = palloc_array(int, nkeys);
	const_argnos = palloc_array(int, nkeys);

	/*
	 * Resolve each filter's operator and record its argument order: the
	 * stored MCV value is the operator's "expression" argument, so it goes on
	 * the left for "var OP const" (expronleft) and on the right otherwise.
	 */
	for (i = 0; i < nkeys; i++)
	{
		Oid			opno = list_nth_oid(filter_opnos, i);
		Oid			coll = list_nth_oid(filter_collations, i);
		Oid			opcode = get_opcode(opno);
		bool		expronleft = list_nth_int(filter_expronlefts, i) != 0;

		if (!OidIsValid(opcode))
			return 0.0;

		fmgr_info(opcode, &op_funcs[i]);

		fcinfo_arr[i] = palloc(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*fcinfo_arr[i], &op_funcs[i],
								 2, coll, NULL, NULL);

		var_argnos[i] = expronleft ? 0 : 1;
		const_argnos[i] = expronleft ? 1 : 0;
	}

	total_sel = 0.0;

	mcv_totalsel = 0.0;
	for (item_idx = 0; item_idx < mcvlist->nitems; item_idx++)
		mcv_totalsel += mcvlist->items[item_idx].frequency;

	if (list_length(filter_values) != nkeys)
		return 0.0;

	/*
	 * Match the MCV items.  An item matches when every covered filter holds:
	 * its value in that filter's dimension must satisfy the operator against
	 * at least one of the filter's values (so IN is an OR within a filter),
	 * and every filter must hold (AND across filters).  Dimensions not
	 * covered by a filter are left unconstrained, which marginalizes over
	 * them.  Like the single-table mcv_get_match_bitmap(), an ordering
	 * operator can match many items, so we scan the whole list.
	 */
	matched = palloc_array(bool, mcvlist->nitems);
	for (item_idx = 0; item_idx < mcvlist->nitems; item_idx++)
		matched[item_idx] = true;

	for (i = 0; i < nkeys; i++)
	{
		int			key_idx = list_nth_int(stxkey_indexes, i);
		List	   *vals = (List *) list_nth(filter_values, i);
		int			var_argno = var_argnos[i];
		int			const_argno = const_argnos[i];
		bool	   *filter_matched = palloc0_array(bool, mcvlist->nitems);
		ListCell   *lc_val;

		foreach(lc_val, vals)
		{
			fcinfo_arr[i]->args[const_argno].value = PointerGetDatum(lfirst(lc_val));
			fcinfo_arr[i]->args[const_argno].isnull = false;

			for (item_idx = 0; item_idx < mcvlist->nitems; item_idx++)
			{
				MCVItem    *item = &mcvlist->items[item_idx];
				Datum		fresult;

				if (item->isnull[key_idx])
					continue;

				fcinfo_arr[i]->args[var_argno].value = item->values[key_idx];
				fcinfo_arr[i]->args[var_argno].isnull = false;
				fcinfo_arr[i]->isnull = false;

				fresult = FunctionCallInvoke(fcinfo_arr[i]);
				if (!fcinfo_arr[i]->isnull && DatumGetBool(fresult))
					filter_matched[item_idx] = true;
			}
		}

		/* AND this filter's matches into the running match set. */
		for (item_idx = 0; item_idx < mcvlist->nitems; item_idx++)
			matched[item_idx] = matched[item_idx] && filter_matched[item_idx];

		pfree(filter_matched);
	}

	for (item_idx = 0; item_idx < mcvlist->nitems; item_idx++)
		if (matched[item_idx])
			total_sel += mcvlist->items[item_idx].frequency;

	/*
	 * Non-MCV estimation: the MCV keeps only the top-K most frequent
	 * combinations, so when a covered equality/IN matches nothing stored, add
	 * the average frequency of one non-stored combination, (1 - mcv_totalsel)
	 * / (number of non-stored combinations).  This is added only for equality
	 * and IN (all_equality); a range's non-MCV selectivity needs a per-column
	 * distribution of the join result (a join histogram, left as a
	 * follow-up), so ranges rely on the matched MCV frequencies alone,
	 * floored at the standard estimate by the caller's cross-check (see
	 * join_mcv_clause_selectivity).
	 *
	 * A single-column query on a multi-column MCV uses the combination count
	 * as a coarse proxy for the number of stored values.  The caller's
	 * cross-check discards this whenever the ordinary per-column estimate is
	 * at least as good, which for a single column it always is, so the
	 * imprecision has no observable effect.
	 */
	if (all_equality && total_sel <= 0.0 && ndistinct > mcvlist->nitems)
	{
		double		other_freq;

		other_freq = (1.0 - mcv_totalsel) / (ndistinct - mcvlist->nitems);
		CLAMP_PROBABILITY(other_freq);

		total_sel = other_freq;
		CLAMP_PROBABILITY(total_sel);
	}

	return total_sel;
}

/*
 * filter_op_is_supported
 *		Can opno be matched against the stored MCV values, and is it equality?
 *
 * Join-MCV filters are evaluated against each stored MCV value using the
 * clause's own operator (like the single-table MCV code in mcv.c).  We
 * classify the operator exactly as statext_is_compatible_clause_internal()
 * does, by its restriction-selectivity estimator: equality, not-equal, and the
 * ordering operators are supported; anything else is rejected so it falls back
 * to standard estimation instead of being misinterpreted.
 *
 * On a supported operator, *is_equality is set to whether it estimates as
 * equality (F_EQSEL); the caller adds the non-MCV selectivity only when every
 * filter is an equality.
 */
static bool
filter_op_is_supported(Oid opno, bool *is_equality)
{
	*is_equality = false;

	switch (get_oprrest(opno))
	{
		case F_EQSEL:
			*is_equality = true;
			return true;
		case F_NEQSEL:
		case F_SCALARLTSEL:
		case F_SCALARLESEL:
		case F_SCALARGTSEL:
		case F_SCALARGESEL:
			return true;
		default:
			return false;
	}
}

/*
 * extract_filter_info
 *		Extract filter column and constant value(s) from a filter clause
 *
 * Handles two types of filter clauses:
 * 1. OpExpr: col OP constant (equality, <, <=, >, >=, <>)
 * 2. ScalarArrayOpExpr: col OP ANY (const1, const2, ...), e.g. IN
 *
 * Returns true if a valid filter pattern is found, false otherwise.
 * On success, sets *filter_var, *filter_values (list of Datums), *collation,
 * *filter_opno (the clause's comparison operator), *filter_expronleft (true if
 * the Var was on the left of the operator, i.e. "var OP const") and
 * *filter_is_equality (whether the operator is equality/IN).
 */
static bool
extract_filter_info(Node *clause,
					Index expected_relid,
					Var **filter_var,
					List **filter_values,
					Oid *collation,
					Oid *filter_opno,
					bool *filter_expronleft,
					bool *filter_is_equality)
{
	*filter_var = NULL;
	*filter_values = NIL;

	/* Case 1: OpExpr - a comparison against a constant (col OP const) */
	if (IsA(clause, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) clause;
		Node	   *var_node = NULL;
		Const	   *const_node = NULL;
		bool		expronleft;

		if (list_length(opexpr->args) != 2)
			return false;

		/* Reject operators we cannot match against the MCV. */
		if (!filter_op_is_supported(opexpr->opno, filter_is_equality))
			return false;

		if (!examine_opclause_args(opexpr->args, &var_node, &const_node,
								   &expronleft))
			return false;

		if (!var_node || !const_node || !IsA(var_node, Var))
			return false;

		if (const_node->constisnull)
			return false;

		*filter_var = (Var *) var_node;

		/* Verify the Var is from the expected relation */
		if ((*filter_var)->varno != expected_relid)
			return false;

		/* Create single-element list - store Datum as pointer */
		*filter_values = list_make1(DatumGetPointer(const_node->constvalue));

		/*
		 * Compare using the column's collation, which the MCV was built under
		 * (matching mcv_get_match_bitmap), rather than the clause's
		 * inputcollid, which an explicit COLLATE could override.
		 */
		*collation = (*filter_var)->varcollid;
		*filter_opno = opexpr->opno;
		*filter_expronleft = expronleft;

		return true;
	}

	/* Case 2: ScalarArrayOpExpr - IN clause (col IN (...)) */
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *scalar_node;
		Node	   *array_node;
		Const	   *array_const;
		ArrayType  *arr;
		int			nitems;
		Datum	   *items;
		bool	   *nulls;
		int			i;
		Oid			elmtype;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;

		/* Only support ANY (IN), not ALL */
		if (!saop->useOr)
			return false;

		/* Reject unsupported element operators (see the OpExpr case). */
		if (!filter_op_is_supported(saop->opno, filter_is_equality))
			return false;

		if (list_length(saop->args) != 2)
			return false;

		scalar_node = (Node *) linitial(saop->args);
		array_node = (Node *) lsecond(saop->args);

		/* Strip RelabelType */
		while (IsA(scalar_node, RelabelType))
			scalar_node = (Node *) ((RelabelType *) scalar_node)->arg;

		/* Scalar must be a Var after stripping coercions */
		if (!IsA(scalar_node, Var))
			return false;

		*filter_var = (Var *) scalar_node;

		/* Verify the Var is from the expected relation */
		if ((*filter_var)->varno != expected_relid)
			return false;

		/* Array must be a Const for us to extract values */
		if (!IsA(array_node, Const))
			return false;

		array_const = (Const *) array_node;

		/* Can't handle NULL arrays */
		if (array_const->constisnull)
			return false;

		/* Deconstruct the array */
		arr = DatumGetArrayTypeP(array_const->constvalue);
		elmtype = ARR_ELEMTYPE(arr);

		/* Get type info for deconstruction */
		get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

		deconstruct_array(arr, elmtype, elmlen, elmbyval, elmalign,
						  &items, &nulls, &nitems);

		/* Build list of non-NULL Datums */
		*filter_values = NIL;
		for (i = 0; i < nitems; i++)
		{
			if (!nulls[i])
			{
				/*
				 * Store Datum as pointer - safe since Datum and pointer are
				 * same size
				 */
				*filter_values = lappend(*filter_values, DatumGetPointer(items[i]));
			}
		}

		/* If all values were NULL, we can't use this */
		if (*filter_values == NIL)
			return false;

		/* Use the column's collation; see the OpExpr case above. */
		*collation = (*filter_var)->varcollid;

		/*
		 * A ScalarArrayOpExpr is always "scalar OP ANY (array)", i.e. the Var
		 * is on the left of the element operator.
		 */
		*filter_opno = saop->opno;
		*filter_expronleft = true;

		return true;
	}

	return false;
}

/*
 * match_filters_for_rel -
 *	  Match a rel's base restrictions against a stat's columns.
 *
 * For each restriction clause on rel that extract_filter_info can parse,
 * check whether the filtered column appears among the stat's columns on the
 * matching side.  Matched filters are appended to the output lists.
 *
 * is_anchor selects which side's columns to match: the anchor's columns have
 * varno == 1, the other rel's columns have varno != 1.
 *
 * A filter whose operator is not leakproof is skipped unless the user may read
 * all rows of the filtered column, mirroring the single-table MCV check in
 * statext_is_compatible_clause(): the estimator applies the operator to the
 * stored MCV values, so a leaky operator could otherwise reveal values the
 * user is not allowed to see.
 */
static void
match_filters_for_rel(PlannerInfo *root,
					  RelOptInfo *rel,
					  StatisticExtInfo *stat,
					  bool is_anchor,
					  List **stxkey_indexes,
					  List **filter_values,
					  List **covered_rinfos,
					  List **filter_collations,
					  List **filter_opnos,
					  List **filter_expronlefts,
					  bool *all_equality)
{
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
		Var		   *filter_var;
		List	   *values;
		Oid			filter_collation;
		Oid			filter_opno;
		bool		filter_expronleft;
		bool		filter_is_equality;
		int			key_idx = -1;
		int			pos = 0;
		ListCell   *lc_key;

		if (!extract_filter_info((Node *) ri->clause,
								 rel->relid,
								 &filter_var, &values,
								 &filter_collation,
								 &filter_opno, &filter_expronleft,
								 &filter_is_equality))
			continue;

		foreach(lc_key, stat->exprs)
		{
			Node	   *node = (Node *) lfirst(lc_key);
			Var		   *keyvar;
			bool		ref_matches;

			/* only plain columns can match a filter's column */
			if (!statext_is_column(node))
				continue;

			keyvar = (Var *) node;
			ref_matches = is_anchor
				? (keyvar->varno == 1)
				: (keyvar->varno != 1);

			if (keyvar->varattno == filter_var->varattno && ref_matches)
			{
				key_idx = pos;
				break;
			}
			pos++;
		}

		if (key_idx < 0)
			continue;

		/*
		 * Apply the leakproof check (see this function's header).  The
		 * operator touches only its own column's stored values, so checking
		 * that one column on this rel suffices.
		 */
		if (!get_func_leakproof(get_opcode(filter_opno)))
		{
			Bitmapset  *filter_attnums = NULL;

			filter_attnums = bms_add_member(filter_attnums,
											filter_var->varattno - FirstLowInvalidHeapAttributeNumber);

			if (!all_rows_selectable(root, rel->relid, filter_attnums))
			{
				bms_free(filter_attnums);
				continue;
			}
			bms_free(filter_attnums);
		}

		*stxkey_indexes = lappend_int(*stxkey_indexes, key_idx);
		*filter_values = lappend(*filter_values, values);
		*covered_rinfos = lappend(*covered_rinfos, ri);
		*filter_collations = lappend_oid(*filter_collations, filter_collation);
		*filter_opnos = lappend_oid(*filter_opnos, filter_opno);
		*filter_expronlefts = lappend_int(*filter_expronlefts,
										  filter_expronleft ? 1 : 0);
		if (!filter_is_equality)
			*all_equality = false;
	}
}

/*
 * match_join_stat -
 *	  Check if a stat matches the join clause and find covered filters.
 *
 * Checks whether the stat's join condition matches join_rinfo, then walks
 * other_rel's baserestrictinfo to find filter clauses covered by the stat's
 * stxkeys.  Uncovered filters are skipped; they are already reflected in
 * other_rel->rows.
 *
 * Returns the number of covered filters (0 means no match -- either the join
 * clause didn't match or no filters were covered).  On success, populates
 * output parameters with the covered filter details needed by
 * compute_join_mcv_selec().
 */
static int
match_join_stat(PlannerInfo *root,
				StatisticExtInfo *stat,
				RelOptInfo *anchor_rel,
				RelOptInfo *other_rel,
				RestrictInfo *join_rinfo,
				List **covered_rinfos,
				List **stxkey_indexes,
				List **filter_values,
				List **filter_collations,
				List **filter_opnos,
				List **filter_expronlefts,
				bool *all_equality)
{
	OpExpr	   *stat_op;
	Var		   *stat_left_var;
	Var		   *stat_right_var;
	AttrNumber	anchor_joinkey;
	AttrNumber	other_joinkey;
	Oid			stat_join_opno; /* operator normalized to (anchor, other) */
	bool		join_matched = false;

	*covered_rinfos = NIL;
	*stxkey_indexes = NIL;
	*filter_values = NIL;
	*filter_collations = NIL;
	*filter_opnos = NIL;
	*filter_expronlefts = NIL;
	*all_equality = true;

	/*
	 * Check if the join clause matches this stat's join condition.
	 *
	 * XXX Currently only single-condition joins are supported.  To handle
	 * multiple join conditions on the same baserel pair (e.g., a.x = b.x AND
	 * a.y = b.y), the caller would need to group clauses by baserel pair and
	 * present the full group here, and this function would need to match all
	 * conditions against the stat's joinconds.  The sampling code in ANALYZE
	 * would also need to support composite join key lookups.
	 */
	if (list_length(stat->joinconds) != 1)
		return 0;

	stat_op = (OpExpr *) linitial(stat->joinconds);
	Assert(IsA(stat_op, OpExpr) && list_length(stat_op->args) == 2);
	stat_left_var = (Var *) linitial(stat_op->args);
	stat_right_var = (Var *) lsecond(stat_op->args);
	Assert(IsA(stat_left_var, Var) && IsA(stat_right_var, Var));

	/*
	 * Identify anchor and other join key attnums from the stat's joincond,
	 * and normalize the operator to (anchor, other) argument order.
	 *
	 * TODO: this assumes a single join condition between the anchor (varno 1)
	 * and the one other table (varno 2), i.e. 2-way joins only.  N-way
	 * support will need this reworked to match multiple conditions among
	 * arbitrary relation pairs.
	 */
	if (stat_left_var->varno == 1 && stat_right_var->varno == 2)
	{
		anchor_joinkey = stat_left_var->varattno;
		other_joinkey = stat_right_var->varattno;
		stat_join_opno = stat_op->opno;
	}
	else if (stat_left_var->varno == 2 && stat_right_var->varno == 1)
	{
		anchor_joinkey = stat_right_var->varattno;
		other_joinkey = stat_left_var->varattno;
		stat_join_opno = get_commutator(stat_op->opno);
		if (!OidIsValid(stat_join_opno))
			return 0;
	}
	else
		return 0;

	if (IsA(join_rinfo->clause, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) join_rinfo->clause;

		if (list_length(opexpr->args) == 2)
		{
			Node	   *left_node = linitial(opexpr->args);
			Node	   *right_node = lsecond(opexpr->args);

			while (IsA(left_node, RelabelType))
				left_node = (Node *) ((RelabelType *) left_node)->arg;
			while (IsA(right_node, RelabelType))
				right_node = (Node *) ((RelabelType *) right_node)->arg;

			if (IsA(left_node, Var) && IsA(right_node, Var))
			{
				Var		   *left_var = (Var *) left_node;
				Var		   *right_var = (Var *) right_node;

				if (bms_is_member(left_var->varno, anchor_rel->relids) &&
					bms_is_member(right_var->varno, other_rel->relids) &&
					left_var->varattno == anchor_joinkey &&
					right_var->varattno == other_joinkey &&
					opexpr->opno == stat_join_opno)
					join_matched = true;

				if (bms_is_member(right_var->varno, anchor_rel->relids) &&
					bms_is_member(left_var->varno, other_rel->relids) &&
					right_var->varattno == anchor_joinkey &&
					left_var->varattno == other_joinkey &&
					opexpr->opno == get_commutator(stat_join_opno))
					join_matched = true;
			}
		}
	}

	if (!join_matched)
		return 0;

	/*
	 * Find filter clauses on other_rel and anchor_rel that are covered by
	 * this stat's columns.  Without at least one covered filter, the stat
	 * cannot improve on the standard join selectivity estimate, so bail out.
	 *
	 * For other_rel filters, match columns with varno != 1 (joined relation
	 * columns).  For anchor_rel filters, match columns with varno == 1
	 * (anchor relation columns).
	 */
	match_filters_for_rel(root, other_rel, stat, false,
						  stxkey_indexes, filter_values,
						  covered_rinfos, filter_collations,
						  filter_opnos, filter_expronlefts, all_equality);
	match_filters_for_rel(root, anchor_rel, stat, true,
						  stxkey_indexes, filter_values,
						  covered_rinfos, filter_collations,
						  filter_opnos, filter_expronlefts, all_equality);

	return list_length(*covered_rinfos);
}

/*
 * compute_join_mcv_selec -
 *	  Compute join MCV selectivity from a matched stat.
 *
 * Given a stat and the filter matching results from match_join_stat(),
 * loads the MCVList and computes the raw join+filter selectivity.
 * stxkey_indexes contains 0-based stxkey positions produced by
 * match_join_stat().
 *
 * ndistinct is the number of distinct values for the filter column,
 * used for non-MCV estimation.  Pass 0 to disable estimation.
 *
 * Returns selectivity >= 0.
 */
static Selectivity
compute_join_mcv_selec(StatisticExtInfo *stat,
					   List *stxkey_indexes,
					   List *filter_values,
					   List *filter_collations,
					   List *filter_opnos,
					   List *filter_expronlefts,
					   bool all_equality,
					   double ndistinct)
{
	MCVList    *mcvlist;
	Selectivity selec;

	/* Load the MCVList */
	mcvlist = statext_mcv_load(stat->statOid, false);
	if (!mcvlist)
		return 0.0;

	/*
	 * filter_values is one value list per covered filter (a single-element
	 * list for "col = const", or several for "col IN (...)").
	 */
	selec = join_mcv_clauselist_selectivity(mcvlist,
											filter_values,
											stxkey_indexes, filter_collations,
											filter_opnos, filter_expronlefts,
											all_equality, ndistinct);

	return selec;
}

/*
 * join_mcv_clause_selectivity
 *		Estimate a join clause's selectivity using join MCV statistics.
 *
 * Given a join clause (e.g., a.id = b.id), extracts the baserel pair from
 * the Vars and searches their statlists for a join stat covering that pair.
 * This per baserel pair lookup produces the same result regardless of which
 * component rel pair is used to form the joinrel first.
 *
 * The stat may cover only a subset of the filter clauses on either rel.
 * The raw MCV selectivity is P(join AND covered_filters) relative to
 * anchor_totalrows.  Since the planner's join size formula is:
 *
 *     nrows = anchor_rows * other_rows * sel(clause)
 *
 * and both anchor_rows and other_rows already reflect their respective base
 * restrictions (including uncovered filters), we convert via:
 *
 *     adjusted_sel = raw_sel
 *         / (covered_anchor_sel * other_tuples * covered_other_sel)
 *
 * where covered_anchor_sel and covered_other_sel are computed separately
 * through clauselist_selectivity so that single-table extended stats on
 * each rel are used, ensuring consistency with how each rel's rows was
 * computed.  Uncovered filters are accounted for through the respective
 * rel's rows in the planner's formula.
 *
 * On success, returns the adjusted selectivity (> 0).
 * Returns 0 if no applicable join stat is found.
 */
Selectivity
join_mcv_clause_selectivity(PlannerInfo *root,
							RestrictInfo *rinfo)
{
	OpExpr	   *opexpr;
	Node	   *left_node;
	Node	   *right_node;
	Var		   *left_var;
	Var		   *right_var;
	int			varno1;
	int			varno2;
	RelOptInfo *baserel1;
	RelOptInfo *baserel2;
	RelOptInfo *rels[2];
	int			r;
	Selectivity best_sel = 0.0;
	int			best_ncovered = 0;
	bool		found_full_coverage = false;
	SpecialJoinInfo sjinfo_data;

	/* Must be a simple Var = Var equality */
	if (!IsA(rinfo->clause, OpExpr))
		return 0.0;

	opexpr = (OpExpr *) rinfo->clause;
	if (list_length(opexpr->args) != 2)
		return 0.0;

	/* Strip RelabelType wrappers */
	left_node = linitial(opexpr->args);
	right_node = lsecond(opexpr->args);
	while (IsA(left_node, RelabelType))
		left_node = (Node *) ((RelabelType *) left_node)->arg;
	while (IsA(right_node, RelabelType))
		right_node = (Node *) ((RelabelType *) right_node)->arg;
	if (!IsA(left_node, Var) || !IsA(right_node, Var))
		return 0.0;

	left_var = (Var *) left_node;
	right_var = (Var *) right_node;

	varno1 = left_var->varno;
	varno2 = right_var->varno;
	if (varno1 == varno2)
		return 0.0;

	baserel1 = find_base_rel(root, varno1);
	baserel2 = find_base_rel(root, varno2);
	rels[0] = baserel1;
	rels[1] = baserel2;

	/*
	 * Build a dummy SpecialJoinInfo so that clause_selectivity_ext treats the
	 * join clause as a join (dispatching to eqjoinsel) rather than as a
	 * restriction (dispatching to eqsel).
	 */
	init_dummy_sjinfo(&sjinfo_data, baserel1->relids, baserel2->relids);

	/*
	 * Try each base rel as the potential stat anchor.  A join stat is stored
	 * on its anchor table's statlist (stxrelid = anchor).  We prefer stats
	 * covering more filter columns; a full-coverage stat is returned
	 * immediately, while partial-coverage results are saved and used only if
	 * no better stat is found.
	 */
	for (r = 0; r < 2; r++)
	{
		RelOptInfo *anchor_rel = rels[r];
		RelOptInfo *other_rel = rels[1 - r];
		Oid			other_reloid;
		RangeTblEntry *other_rte;
		ListCell   *lc;
		int			nfilters;	/* total baserestrictinfo clauses on both rels */

		other_rte = root->simple_rte_array[other_rel->relid];
		other_reloid = other_rte->relid;
		nfilters = list_length(other_rel->baserestrictinfo) +
			list_length(anchor_rel->baserestrictinfo);

		foreach(lc, anchor_rel->statlist)
		{
			StatisticExtInfo *stat = (StatisticExtInfo *) lfirst(lc);
			List	   *covered_rinfos = NIL;
			List	   *anchor_covered_rinfos = NIL;
			List	   *other_covered_rinfos = NIL;
			List	   *stxkey_indexes = NIL;
			List	   *filter_values = NIL;
			List	   *filter_collations = NIL;
			List	   *filter_opnos = NIL;
			List	   *filter_expronlefts = NIL;
			bool		all_equality;
			Selectivity raw_sel;
			Selectivity covered_anchor_sel = 1.0;
			Selectivity covered_other_sel = 1.0;
			int			ncovered;	/* number of filters the stat covers */
			ListCell   *lc2;
			double		ndistinct = 0;
			Selectivity standard_sel;

			/* Skip non-join and non-MCV stats */
			if (stat->joinrels == NIL || stat->kind != STATS_EXT_MCV)
				continue;

			/* Check that this stat joins to the other relation */
			if (!list_member_oid(stat->joinrels, other_reloid))
				continue;

			/* Does this stat match our join clause and cover any filters? */
			ncovered = match_join_stat(root, stat, anchor_rel, other_rel, rinfo,
									   &covered_rinfos, &stxkey_indexes,
									   &filter_values, &filter_collations,
									   &filter_opnos, &filter_expronlefts,
									   &all_equality);
			if (ncovered == 0)
				continue;

			/* Split covered filters by relation */
			foreach(lc2, covered_rinfos)
			{
				RestrictInfo *ri = lfirst_node(RestrictInfo, lc2);

				if (bms_is_member(anchor_rel->relid, ri->clause_relids))
					anchor_covered_rinfos = lappend(anchor_covered_rinfos, ri);
				else
					other_covered_rinfos = lappend(other_covered_rinfos, ri);
			}

			/*
			 * Look up ndistinct for each covered filter column, used for
			 * non-MCV estimation.  For single-key, this is the per-column
			 * ndistinct.  For multi-key, it is the product of per-column
			 * ndistinct values (independence assumption). Pass 0 to disable
			 * correction when stats are unavailable.
			 */
			if (covered_rinfos != NIL)
			{
				ndistinct = 1.0;

				foreach(lc2, covered_rinfos)
				{
					RestrictInfo *filter_ri = lfirst_node(RestrictInfo,
														  lc2);
					Var		   *filter_var = NULL;
					List	   *dummy_values;
					Oid			dummy_collation;
					Oid			dummy_opno;
					bool		dummy_expronleft;
					bool		dummy_is_equality;
					RelOptInfo *filter_rel;
					VariableStatData vardata;
					bool		isdefault;
					double		col_ndistinct;

					if (bms_is_member(other_rel->relid,
									  filter_ri->clause_relids))
						filter_rel = other_rel;
					else
						filter_rel = anchor_rel;

					if (!extract_filter_info((Node *) filter_ri->clause,
											 filter_rel->relid,
											 &filter_var, &dummy_values,
											 &dummy_collation,
											 &dummy_opno,
											 &dummy_expronleft,
											 &dummy_is_equality) ||
						filter_var == NULL)
					{
						ndistinct = 0;
						break;
					}

					examine_variable(root, (Node *) filter_var, 0,
									 &vardata);
					col_ndistinct = get_variable_numdistinct(&vardata,
															 &isdefault);
					ReleaseVariableStats(vardata);

					if (isdefault)
					{
						ndistinct = 0;
						break;
					}

					ndistinct *= col_ndistinct;
				}
			}

			/* Compute raw MCV selectivity (join AND covered filters) */
			raw_sel = compute_join_mcv_selec(stat, stxkey_indexes,
											 filter_values,
											 filter_collations,
											 filter_opnos,
											 filter_expronlefts,
											 all_equality,
											 ndistinct);

			/*
			 * Full coverage with zero MCV match means the stat saw the filter
			 * combination and found no matches.  This is authoritative -- do
			 * not let a partial-coverage stat override it with a positive
			 * estimate.
			 */
			if (ncovered >= nfilters && raw_sel <= 0)
			{
				found_full_coverage = true;
				continue;
			}

			if (raw_sel <= 0)
				continue;

			/*
			 * Convert the join-result-fraction selectivity into a join
			 * selectivity.
			 *
			 * raw_sel is P(join AND covered_filters) / anchor_totalrows,
			 * computed from the MCV frequencies.  The planner computes:
			 *
			 * nrows = anchor_rows * other_rows * join_sel
			 *
			 * where anchor_rows and other_rows each already reflect their
			 * base restrictions.  If we return raw_sel directly, covered
			 * filters would be double-counted: anchor-side covered filters
			 * appear in both raw_sel and anchor_rows, and other-side covered
			 * filters appear in both raw_sel and other_rows.  To cancel both:
			 *
			 * adjusted_sel = raw_sel / (covered_anchor_sel * other_tuples *
			 * covered_other_sel)
			 *
			 * The other_tuples * covered_other_sel denominator is clamped to
			 * at least 1.0 to match clamp_row_est() in
			 * set_baserel_size_estimates.  Selectivities are computed via
			 * clauselist_selectivity so that single-table extended stats are
			 * used, ensuring consistency with how each rel's rows was
			 * computed.
			 */
			if (anchor_covered_rinfos != NIL)
				covered_anchor_sel = clauselist_selectivity(root,
															anchor_covered_rinfos,
															0,
															JOIN_INNER,
															NULL);
			if (other_covered_rinfos != NIL)
				covered_other_sel = clauselist_selectivity(root,
														   other_covered_rinfos,
														   0,
														   JOIN_INNER,
														   NULL);
			if (covered_anchor_sel > 0 && other_rel->tuples > 0)
			{
				double		other_denom = Max(other_rel->tuples * covered_other_sel,
											  1.0);

				raw_sel /= covered_anchor_sel * other_denom;
				CLAMP_PROBABILITY(raw_sel);
			}
			else
				continue;

			/*
			 * Cross-check: the MCV-based estimate shouldn't be lower than
			 * what the standard join estimator would produce (eqjoinsel on
			 * per-column stats only, no extended stats).  If our estimate is
			 * lower, skip this stat and fall back to the standard estimate.
			 */
			standard_sel = clause_selectivity_ext(root,
												  (Node *) rinfo->clause,
												  0,
												  JOIN_INNER,
												  &sjinfo_data,
												  false);
			if (raw_sel < standard_sel)
				continue;

			/* Full coverage: return immediately */
			if (ncovered >= nfilters)
				return raw_sel;

			/* Partial coverage: save if better than previous best */
			if (ncovered > best_ncovered)
			{
				/*
				 * Skip partial-coverage results when other_rel->rows is at
				 * the clamp_row_est floor.  This means the single-table stats
				 * predict ~0 matching rows for the full filter set.  The
				 * partial-coverage formula would inflate the estimate because
				 * it divides by only the covered filter selectivity, while
				 * the uncovered filters (which drive rows toward zero) are
				 * accounted for only through the clamped rows value.
				 */
				if (other_rel->rows <= 1.0 || anchor_rel->rows <= 1.0)
					continue;

				best_sel = raw_sel;
				best_ncovered = ncovered;
			}
		}
	}

	/*
	 * Do not use a partial-coverage result if a full-coverage stat already
	 * reported zero MCV match -- the full stat's answer is more
	 * authoritative.
	 */
	if (best_sel > 0 && !found_full_coverage)
		return best_sel;

	return 0.0;
}
