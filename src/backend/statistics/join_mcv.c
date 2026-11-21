/*-------------------------------------------------------------------------
 *
 * join_mcv.c
 *	  POSTGRES join MCV lists
 *
 * Join MCV statistics are MCV lists over the rows of a join.  The first
 * table of the definition, the anchor, owns the statistics object, and
 * ANALYZE of the anchor samples the join by looking up the rows of the
 * other table through an index on its join column.
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
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/joininfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "rewrite/rewriteManip.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/sampling.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * One filter clause that join statistics can estimate.
 */
typedef struct JoinMCVFilter
{
	RestrictInfo *rinfo;		/* the filter clause */
	RelOptInfo *rel;			/* the relation it filters */
	Expr	   *stat_clause;	/* the clause with the stat's varnos */
} JoinMCVFilter;

/*
 * One relation of a join statistics object, as the query joins it.
 */
typedef struct JoinStatRel
{
	RelOptInfo *rel;			/* the relation in the query */
	Index		stat_varno;		/* its varno in the stat's definition */
	AttrNumber	joinkey;		/* its column in the stat's join condition */
} JoinStatRel;

/*
 * Where sample_index() takes a column of the join sample from.
 */
typedef struct JoinSampleColumn
{
	bool		anchor;			/* is it a column of the anchor? */
	AttrNumber	attnum;			/* its attnum in its table */
	int16		typlen;
	bool		typbyval;
} JoinSampleColumn;

/*
 * Entries in the simplehash hash table used by sample_index() to group the
 * anchor sample tuples by join value.
 */
typedef struct JoinValueGroup
{
	Datum		value;			/* the join value */
	List	   *rows;			/* indexes of the sample tuples having it */
	uint32		hash;			/* hash code for the Datum */
	char		status;			/* status code used by simplehash.h */
} JoinValueGroup;

/* forward reference */
typedef struct JoinValueTable_hash JoinValueTable_hash;

static uint32 hash_join_value(JoinValueTable_hash *tab, Datum key);
static bool join_values_equal(JoinValueTable_hash *tab, Datum key0,
							  Datum key1);

/* Define support routines for the join value hash table */
#define SH_PREFIX				JoinValueTable
#define SH_ELEMENT_TYPE			JoinValueGroup
#define SH_KEY_TYPE				Datum
#define SH_KEY					value
#define SH_HASH_KEY(tab,key)	hash_join_value(tab, key)
#define SH_EQUAL(tab,key0,key1)	join_values_equal(tab, key0, key1)
#define SH_SCOPE				static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tab,ent)	(ent)->hash
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"


/*
 * find_index_for_operator
 *		Find an index that supports lookup on the given column using
 *		the specified operator.
 *
 * Returns InvalidOid if there is none.  A unique index is preferred, then
 * one with fewer key columns.
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
		 * A partial index would bias the sample, as it has only the rows
		 * matching its predicate.  The lookup needs the column to lead the
		 * index, and an AM with amgettuple, which BRIN lacks.
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
 * join_stats_cond_vars
 *		Get the two columns compared by a join statistics condition.
 *
 * The parser wraps a column in a RelabelType when it compares it as a
 * binary-compatible type, such as a varchar column compared as text.  Look
 * through any such relabeling, as the planner does when it examines a
 * variable.
 *
 * Returns false if cond is not an operator with two column operands.
 */
bool
join_stats_cond_vars(Node *cond, Var **lvar, Var **rvar)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;

	if (!IsA(cond, OpExpr))
		return false;
	op = (OpExpr *) cond;
	if (list_length(op->args) != 2)
		return false;

	left = linitial(op->args);
	right = lsecond(op->args);
	while (IsA(left, RelabelType))
		left = (Node *) ((RelabelType *) left)->arg;
	while (IsA(right, RelabelType))
		right = (Node *) ((RelabelType *) right)->arg;
	if (!IsA(left, Var) || !IsA(right, Var))
		return false;

	*lvar = (Var *) left;
	*rvar = (Var *) right;
	return true;
}

/*
 * join_stats_cond_sides
 *		Get the columns of a stored join statistics condition: the one of the
 *		table sampled first, and the one of the table probed for its rows.
 *
 * The tables are numbered so that the probed table of a condition has the
 * higher varno.  *commuted is set if the probed column is the left operand.
 */
void
join_stats_cond_sides(OpExpr *cond, Var **sampled, Var **probed,
					  bool *commuted)
{
	Var		   *lvar;
	Var		   *rvar;

	if (!join_stats_cond_vars((Node *) cond, &lvar, &rvar))
		elog(ERROR, "unexpected join statistics condition");
	Assert(lvar->varno != rvar->varno);

	*commuted = lvar->varno > rvar->varno;
	*sampled = *commuted ? rvar : lvar;
	*probed = *commuted ? lvar : rvar;
}

/*
 * Support functions for the JoinValueTable hash table, which groups join
 * values by their bytes.  private_data is the join column's CompactAttribute.
 */
static uint32
hash_join_value(JoinValueTable_hash *tab, Datum key)
{
	CompactAttribute *attr = (CompactAttribute *) tab->private_data;

	return datum_image_hash(key, attr->attbyval, attr->attlen);
}

static bool
join_values_equal(JoinValueTable_hash *tab, Datum key0, Datum key1)
{
	CompactAttribute *attr = (CompactAttribute *) tab->private_data;

	return datum_image_eq(key0, key1, attr->attbyval, attr->attlen);
}

/*
 * store_join_row
 *		Store a pair of the join sample built by sample_index() as its row k.
 *
 * The pair is the anchor sample tuple anchor_tuple and the probed row in
 * slot.  Values already stored in row k are freed first, as the reservoir
 * replaces rows.
 */
static void
store_join_row(StatsBuildData *data, int k, JoinSampleColumn *cols,
			   HeapTuple anchor_tuple, TupleDesc anchor_desc,
			   TupleTableSlot *slot)
{
	int			f;

	for (f = 0; f < data->nattnums; f++)
	{
		Datum		value;
		bool		isnull;

		if (k < data->numrows && !data->nulls[f][k] && !cols[f].typbyval)
			pfree(DatumGetPointer(data->values[f][k]));

		if (cols[f].anchor)
			value = heap_getattr(anchor_tuple, cols[f].attnum, anchor_desc,
								 &isnull);
		else
			value = slot_getattr(slot, cols[f].attnum, &isnull);

		if (!isnull && !cols[f].typbyval)
			value = datumCopy(value, false, cols[f].typlen);

		data->values[f][k] = value;
		data->nulls[f][k] = isnull;
	}
}

/*
 * sample_index -- Build a random sample of the join result using an index.
 *
 * Given sample tuples S from the anchor table and an index I on the other
 * table's join column, this produces a sample of the join of S with the other
 * table, as Algorithm 1 from Leis et al., CIDR 2017, Section 3.1 does: if the
 * join has N rows and the caller asks for at most n, it is a random sample of
 * min(N, n) of them, drawn without replacement.
 *
 * Many tuples of S may have the same join value, and they all join the same
 * rows of the other table.  So rather than probing I once per tuple of S, we
 * group the tuples of S by join value and probe I once per group, reading
 * each matching row only once.  Tuples whose join value is NULL join nothing
 * and are skipped.
 *
 * The tuples are grouped by the bytes of their join value (datum_image_eq),
 * not by an equality operator, so grouping can never disagree with the
 * index's own equality.  Values that are equal but stored differently, such
 * as numeric 1.0 and 1.00, form separate groups, which costs an extra probe
 * but does not change the sample.
 *
 * The join is sampled in one pass with reservoir sampling, as
 * acquire_sample_rows() samples a table.  A row read from the other table
 * pairs with every tuple of its group, and a skip drawn from Vitter's
 * Algorithm Z passes over all those pairs in one step.  So the work grows
 * with the rows read from the other table, not with the size of the join.
 *
 * Returns the sample, with the ncols columns cols in order, or NULL if no
 * tuple of S joins.  *result_joinrows is set to the number of join rows the
 * tuples of S make, of which the sample is drawn.
 */
static StatsBuildData *
sample_index(HeapTuple *sample_tuples, int nsample, TupleDesc sample_desc,
			 AttrNumber joinkey_attnum, Oid joinkey_type, Oid join_op,
			 Relation heap_rel, Relation idx_rel, int n,
			 JoinSampleColumn *cols, int ncols, double *result_joinrows)
{
	IndexScanDesc iscan;
	ScanKeyData skey[1];
	TupleTableSlot *slot;
	int			idx_strategy;
	int			i;

	/* the tuples of S grouped by join value */
	JoinValueTable_hash *groups;
	JoinValueTable_iterator iter;
	JoinValueGroup *group;

	/* reservoir sampling of the join rows */
	ReservoirStateData rstate;
	double		samplerows = 0; /* join rows seen so far */
	double		rowstoskip = -1;	/* -1 means not set yet */

	StatsBuildData *result_data;

	Oid			scan_eq_op;
	RegProcedure eq_proc;
	CompactAttribute *joinkey_attr;

	/*
	 * The scan key compares the index column with the anchor's join value as
	 * the type the join condition compares it as, joinkey_type, which differs
	 * from the column's own type under a binary-compatible relabeling.
	 */
	idx_strategy = get_op_opfamily_strategy(join_op, idx_rel->rd_opfamily[0]);
	scan_eq_op = get_opfamily_member(idx_rel->rd_opfamily[0],
									 idx_rel->rd_opcintype[0],
									 joinkey_type,
									 idx_strategy);
	if (!OidIsValid(scan_eq_op))
		elog(ERROR, "could not find equality operator for types %u and %u in opfamily %u",
			 idx_rel->rd_opcintype[0], joinkey_type, idx_rel->rd_opfamily[0]);
	eq_proc = get_opcode(scan_eq_op);

	joinkey_attr = TupleDescCompactAttr(sample_desc, joinkey_attnum - 1);
	groups = JoinValueTable_create(CurrentMemoryContext, nsample,
								   joinkey_attr);

	for (i = 0; i < nsample; i++)
	{
		Datum		joinkey_value;
		bool		joinkey_isnull;
		bool		found;

		joinkey_value = heap_getattr(sample_tuples[i], joinkey_attnum, sample_desc, &joinkey_isnull);
		if (joinkey_isnull)
			continue;

		group = JoinValueTable_insert(groups, joinkey_value, &found);
		if (!found)
		{
			/*
			 * Detoast once here rather than in every index comparison, as
			 * ExecIndexEvalRuntimeKeys() does.  The bytes, and so the group,
			 * stay the same.
			 */
			if (joinkey_attr->attlen == -1)
				group->value = PointerGetDatum(PG_DETOAST_DATUM(joinkey_value));
			group->rows = NIL;
		}
		group->rows = lappend_int(group->rows, i);
	}

	/* Done if every join value is NULL */
	if (groups->members == 0)
		return NULL;

	/* ANALYZE runs with an active snapshot, which the index scan uses */
	Assert(ActiveSnapshotSet());

	slot = MakeSingleTupleTableSlot(RelationGetDescr(heap_rel),
									&TTSOpsBufferHeapTuple);
	iscan = index_beginscan(heap_rel, idx_rel, false, GetActiveSnapshot(),
							NULL, 1, 0, 0);

	/* I.lookup(t): only the argument of the scan key changes per group */
	ScanKeyEntryInitialize(&skey[0], 0, 1, idx_strategy, joinkey_type,
						   idx_rel->rd_indcollation[0],
						   eq_proc, (Datum) 0);

	/* The reservoir: at most n join rows */
	result_data = palloc(sizeof(StatsBuildData));
	result_data->nattnums = ncols;
	result_data->numrows = 0;
	result_data->attnums = palloc_array(AttrNumber, ncols);
	result_data->stats = palloc0_array(VacAttrStats *, ncols);
	result_data->values = palloc_array(Datum *, ncols);
	result_data->nulls = palloc_array(bool *, ncols);

	for (i = 0; i < ncols; i++)
	{
		/*
		 * The columns come from two tables, so their attnums can collide.
		 * Number them by their position instead.
		 */
		result_data->attnums[i] = i + 1;
		result_data->values[i] = palloc_array(Datum, n);
		result_data->nulls[i] = palloc_array(bool, n);
	}

	reservoir_init_selection_state(&rstate, n);

	JoinValueTable_start_iterate(groups, &iter);
	while ((group = JoinValueTable_iterate(groups, &iter)) != NULL)
	{
		int			nrows = list_length(group->rows);

		/* I.lookup(t), for every tuple t of the group */
		skey[0].sk_argument = group->value;
		index_rescan(iscan, skey, 1, NULL, 0);

		while (table_index_getnext_slot(iscan, ForwardScanDirection, slot))
		{
			int			j;

			vacuum_delay_point(true);

			/* a lossy index, such as hash, returns rows to recheck */
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

			/*
			 * The row pairs with each of the nrows tuples of the group.  The
			 * pairs fill the reservoir first.  After that, each pair the
			 * sampler selects replaces a random one in the reservoir, and the
			 * pairs in between are skipped without being looked at.
			 */
			j = 0;
			while (j < nrows)
			{
				int			k;

				if (result_data->numrows < n)
				{
					store_join_row(result_data, result_data->numrows, cols,
								   sample_tuples[list_nth_int(group->rows, j)],
								   sample_desc, slot);
					result_data->numrows++;
				}
				else
				{
					/*
					 * t in Vitter's paper is the number of records already
					 * processed, here samplerows, all the pairs before the
					 * j-th pair of this row.
					 */
					if (rowstoskip < 0)
						rowstoskip = reservoir_get_next_S(&rstate, samplerows, n);

					/* Skip the rest of this row's pairs? */
					if (rowstoskip >= nrows - j)
					{
						rowstoskip -= nrows - j;
						samplerows += nrows - j;
						break;
					}

					j += (int) rowstoskip;
					samplerows += rowstoskip;
					rowstoskip = -1;

					k = (int) (n * sampler_random_fract(&rstate.randstate));
					Assert(k >= 0 && k < n);
					store_join_row(result_data, k, cols,
								   sample_tuples[list_nth_int(group->rows, j)],
								   sample_desc, slot);
				}

				j++;
				samplerows += 1;
			}
		}

		list_free(group->rows);
	}

	JoinValueTable_destroy(groups);
	index_endscan(iscan);
	ExecDropSingleTupleTableSlot(slot);

	if (result_data->numrows == 0)
		return NULL;

	*result_joinrows = samplerows;
	return result_data;
}

/*
 * statext_join_mcv_build
 *		Build join MCV statistics using index-based join sampling.
 *
 * Samples the join of the anchor's ANALYZE sample rows with the other table,
 * looking rows up through index_oid, and builds an MCV list from the sample.
 *
 * joinrel_oids and joinconds are the stat's tables, the anchor first, and
 * its join conditions.  stxkeys are the stat's columns, keyrefs their
 * varnos.
 */
MCVList *
statext_join_mcv_build(Relation anchor_rel,
					   Oid *joinrel_oids,
					   int njoinrels,
					   List *joinconds,
					   Oid index_oid,
					   int16 *stxkeys,
					   int16 *keyrefs,
					   int nkeys,
					   int stattarget,
					   int numrows, HeapTuple *rows,
					   double totalrows,
					   VacAttrStats ***result_stats)
{
	OpExpr	   *cond;
	Var		   *sampled_var;
	Var		   *probed_var;
	bool		commuted;
	TupleDesc	anchor_desc = RelationGetDescr(anchor_rel);
	Relation	other_rel;
	TupleDesc	other_desc;
	Relation	idx_rel;
	JoinSampleColumn *cols;
	StatsBuildData *data;
	double		joinrows;
	MCVList    *mcv;
	int			i;

	/* the caller skips the shapes that cannot be built yet */
	Assert(njoinrels == 2 && list_length(joinconds) == 1);

	cond = linitial_node(OpExpr, joinconds);
	join_stats_cond_sides(cond, &sampled_var, &probed_var, &commuted);

	other_rel = table_open(joinrel_oids[1], AccessShareLock);
	other_desc = RelationGetDescr(other_rel);
	idx_rel = index_open(index_oid, AccessShareLock);

	/* keyrefs[i] is 1 for a column of the anchor, 2 for the other table */
	cols = palloc_array(JoinSampleColumn, nkeys);
	for (i = 0; i < nkeys; i++)
	{
		Form_pg_attribute attr;

		cols[i].anchor = (keyrefs[i] == 1);
		cols[i].attnum = stxkeys[i];
		attr = TupleDescAttr(cols[i].anchor ? anchor_desc : other_desc,
							 stxkeys[i] - 1);
		cols[i].typlen = attr->attlen;
		cols[i].typbyval = attr->attbyval;
	}

	/*
	 * XXX ANALYZE samples 300 rows per unit of statistics target for
	 * single-table statistics; whether the join sample should match is open.
	 */
	data = sample_index(rows, numrows, anchor_desc,
						sampled_var->varattno,
						exprType(commuted ? lsecond(cond->args) :
								 linitial(cond->args)),
						cond->opno, other_rel, idx_rel, stattarget * 100,
						cols, nkeys, &joinrows);

	index_close(idx_rel, AccessShareLock);

	if (data == NULL)
	{
		table_close(other_rel, AccessShareLock);
		return NULL;
	}

	for (i = 0; i < nkeys; i++)
	{
		TupleDesc	desc = cols[i].anchor ? anchor_desc : other_desc;
		Form_pg_attribute attr = TupleDescAttr(desc, cols[i].attnum - 1);
		VacAttrStats *s;
		HeapTuple	typtuple;

		s = palloc0(sizeof(VacAttrStats));
		s->tupDesc = desc;
		s->tupattnum = i + 1;
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
		data->stats[i] = s;
	}

	/*
	 * statext_mcv_build() needs the number of rows the sample was drawn from,
	 * the size of the join.  Estimate it as acquire_sample_rows() estimates a
	 * table's size, scaling up from the sampled anchor rows.
	 */
	mcv = statext_mcv_build(data, joinrows * totalrows / numrows,
							stattarget);

	table_close(other_rel, AccessShareLock);

	*result_stats = data->stats;
	return mcv;
}

/*
 * join_mcv_base_selectivity -
 *	  Sum of the base frequencies of the matching MCV items.
 *
 * The base frequency of an item is the share of the join that would hold
 * its values if its columns were unrelated to the join: the product, over
 * the dimensions, of the standard estimate of "column = value" on the
 * column's table.  The single-table code reads base_frequency from the MCV
 * list, the product of the per-column frequencies in the same sample.  Here
 * the sample is the join, while the shares the standard estimate assumes are
 * those in the tables.
 *
 * rels are the nrels relations of the stat, as the query joins them, and
 * matched[] marks the items that match the filters.
 */
static Selectivity
join_mcv_base_selectivity(PlannerInfo *root,
						  StatisticExtInfo *stat,
						  JoinStatRel *rels,
						  int nrels,
						  MCVList *mcvlist,
						  bool *matched)
{
	Selectivity basesel = 0.0;
	VariableStatData *vardatas;
	Var		  **vars;
	Oid		   *eqops;
	ListCell   *lc;
	int			dim;
	int			i;

	vardatas = palloc_array(VariableStatData, mcvlist->ndimensions);
	vars = palloc_array(Var *, mcvlist->ndimensions);
	eqops = palloc_array(Oid, mcvlist->ndimensions);

	/* The dimensions of the MCV list are the stat's columns, in order */
	dim = 0;
	foreach(lc, stat->exprs)
	{
		Var		   *var;
		TypeCacheEntry *typentry;

		/* join statistics have no expressions */
		if (!bms_is_member(foreach_current_index(lc), stat->columns))
			continue;

		/* the stat's columns carry its varnos, and rels are in varno order */
		var = copyObject(lfirst_node(Var, lc));
		Assert(var->varno >= 1 && var->varno <= nrels &&
			   rels[var->varno - 1].stat_varno == var->varno);
		ChangeVarNodes((Node *) var, var->varno,
					   rels[var->varno - 1].rel->relid, 0);

		examine_variable(root, (Node *) var, 0, &vardatas[dim]);
		vars[dim] = var;

		/* the MCV list is built with the type's btree opclass, which has = */
		typentry = lookup_type_cache(mcvlist->types[dim], TYPECACHE_EQ_OPR);
		Assert(OidIsValid(typentry->eq_opr));
		eqops[dim] = typentry->eq_opr;

		dim++;
	}
	Assert(dim == mcvlist->ndimensions);

	for (i = 0; i < mcvlist->nitems; i++)
	{
		MCVItem    *item = &mcvlist->items[i];
		Selectivity itemsel = 1.0;

		if (!matched[i])
			continue;

		for (dim = 0; dim < mcvlist->ndimensions; dim++)
		{
			if (item->isnull[dim])
				itemsel *= nulltestsel(root, IS_NULL, (Node *) vars[dim], 0,
									   JOIN_INNER, NULL);
			else
				itemsel *= var_eq_const(&vardatas[dim], eqops[dim],
										vars[dim]->varcollid,
										item->values[dim], false, true, false);
		}

		basesel += itemsel;
	}

	for (dim = 0; dim < mcvlist->ndimensions; dim++)
		ReleaseVariableStats(vardatas[dim]);
	pfree(vardatas);
	pfree(vars);
	pfree(eqops);

	return basesel;
}

/*
 * join_mcv_clauselist_selectivity -
 *	  Estimate the share of the join that satisfies the filters.
 *
 * Returns the sum of the frequencies of the items of the stat's MCV list that
 * match every filter in filters, a List of JoinMCVFilter.  An item's
 * frequency is a fraction of the sampled join, not of a table's rows.
 * Dimensions no filter covers are left unconstrained, so with statistics on
 * (category, color) a filter on color alone matches every item storing that
 * color.
 *
 * rels are the nrels relations of the stat, as the query joins them.
 * *basesel and *totalsel are set as mcv_clauselist_selectivity() sets them
 * for the single-table code, for mcv_combine_selectivities().
 */
static Selectivity
join_mcv_clauselist_selectivity(PlannerInfo *root,
								StatisticExtInfo *stat,
								JoinStatRel *rels,
								int nrels,
								List *filters,
								Selectivity *basesel,
								Selectivity *totalsel)
{
	MCVList    *mcvlist;
	Selectivity sel = 0.0;
	List	   *clauses = NIL;
	bool	   *matched;
	int			i;

	mcvlist = statext_mcv_load(stat->statOid, false);

	*totalsel = 0.0;
	for (i = 0; i < mcvlist->nitems; i++)
		*totalsel += mcvlist->items[i].frequency;

	/* the filters carry the stat's varnos, as the MCV list's columns do */
	foreach_ptr(JoinMCVFilter, filter, filters)
		clauses = lappend(clauses, filter->stat_clause);

	matched = mcv_get_match_bitmap(root, clauses, stat->exprs, mcvlist, false);

	for (i = 0; i < mcvlist->nitems; i++)
		if (matched[i])
			sel += mcvlist->items[i].frequency;

	*basesel = join_mcv_base_selectivity(root, stat, rels, nrels, mcvlist,
										 matched);

	return sel;
}

/*
 * filter_rows_selectable -
 *	  Could the user read the rows that a filter's join MCV values come from?
 *
 * Those values come from the rows of the relations that join, so the user
 * must be able to read every relation's join key and filter_attnums, the
 * plain attnums of the filtered columns of filter_rel, with no RLS or
 * security barrier quals on any of the relations.  This is the join
 * counterpart of the all_rows_selectable() check in
 * statext_is_compatible_clause().
 */
static bool
filter_rows_selectable(PlannerInfo *root, JoinStatRel *rels, int nrels,
					   JoinStatRel *filter_rel, Bitmapset *filter_attnums)
{
	int			i;

	for (i = 0; i < nrels; i++)
	{
		Bitmapset  *attnums;
		bool		selectable;

		attnums = bms_make_singleton(rels[i].joinkey -
									 FirstLowInvalidHeapAttributeNumber);
		if (&rels[i] == filter_rel)
			attnums = bms_join(attnums,
							   bms_offset_members(bms_copy(filter_attnums),
												  0 - FirstLowInvalidHeapAttributeNumber));

		selectable = all_rows_selectable(root, rels[i].rel->relid, attnums);
		bms_free(attnums);
		if (!selectable)
			return false;
	}

	return true;
}

/*
 * stat_covers_column -
 *	  Is column attnum of the stat's relation stat_varno a column of the stat?
 */
static bool
stat_covers_column(StatisticExtInfo *stat, Index stat_varno, AttrNumber attnum)
{
	ListCell   *lc;

	foreach(lc, stat->exprs)
	{
		Var		   *var = (Var *) lfirst(lc);

		/* join statistics have no expressions */
		if (!bms_is_member(foreach_current_index(lc), stat->columns))
			continue;

		if (var->varno == stat_varno && var->varattno == attnum)
			return true;
	}

	return false;
}

/*
 * match_filters_for_rel -
 *	  Find the filters on one relation of the join that the stat can estimate.
 *
 * Appends to *filters a JoinMCVFilter for each restriction clause of
 * statrel, one of the nrels relations in rels, that is on columns of the stat
 * and that the single-table MCV code could estimate.  Estimation calls the
 * clause's operator on the stored values, so a leaky operator is used only if
 * the user could read the rows those values come from.
 */
static void
match_filters_for_rel(PlannerInfo *root,
					  StatisticExtInfo *stat,
					  JoinStatRel *rels,
					  int nrels,
					  JoinStatRel *statrel,
					  List **filters)
{
	RelOptInfo *rel = statrel->rel;
	Index		stat_varno = statrel->stat_varno;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
		JoinMCVFilter *filter;
		Bitmapset  *attnums = NULL;
		List	   *exprs = NIL;
		bool		leakproof = true;
		bool		covered = true;
		int			attnum = -1;

		if (!statext_is_compatible_clause_internal(root, (Node *) ri->clause,
												   rel->relid, &attnums,
												   &exprs, &leakproof))
			continue;

		/* join statistics have no expressions */
		if (exprs != NIL || bms_is_empty(attnums))
			continue;

		while ((attnum = bms_next_member(attnums, attnum)) >= 0)
		{
			if (!stat_covers_column(stat, stat_varno, attnum))
			{
				covered = false;
				break;
			}
		}
		if (!covered)
			continue;

		/* Skip a leaky operator unless the user could read the rows anyway */
		if (!leakproof &&
			!filter_rows_selectable(root, rels, nrels, statrel, attnums))
			continue;

		filter = palloc_object(JoinMCVFilter);
		filter->rinfo = ri;
		filter->rel = rel;

		/* the stat's columns carry its varnos, not the query's */
		filter->stat_clause = copyObject(ri->clause);
		ChangeVarNodes((Node *) filter->stat_clause, rel->relid,
					   stat_varno, 0);

		*filters = lappend(*filters, filter);
	}
}

/*
 * clause_is_join_condition -
 *	  Does a clause of the query equate the columns of a stat's condition?
 *
 * The clause must be "Var op Var" between anchor_relids and other_relids on
 * the condition's columns, with the condition's operator or its commutator.
 * Nulling bits on the Vars are ignored, as for a foreign key column in
 * match_foreign_keys_to_quals().
 */
static bool
clause_is_join_condition(Expr *clause,
						 Relids anchor_relids, Relids other_relids,
						 AttrNumber anchor_key, AttrNumber other_key,
						 Oid opno)
{
	Var		   *lvar;
	Var		   *rvar;
	Oid			clause_opno;

	if (!join_stats_cond_vars((Node *) clause, &lvar, &rvar))
		return false;
	clause_opno = ((OpExpr *) clause)->opno;

	/* Orient the clause as (anchor, other) */
	if (bms_is_member(lvar->varno, other_relids))
	{
		Var		   *tmp = lvar;

		lvar = rvar;
		rvar = tmp;
		clause_opno = get_commutator(clause_opno);
	}

	return bms_is_member(lvar->varno, anchor_relids) &&
		bms_is_member(rvar->varno, other_relids) &&
		lvar->varattno == anchor_key &&
		rvar->varattno == other_key &&
		clause_opno == opno;
}

/*
 * ec_has_join_condition -
 *	  Does an EquivalenceClass equate the two columns of a stat's condition?
 *
 * Both columns must be members of the class, as plain Vars matched by varno
 * and attno (nulling bits are ignored), and the class's operator families
 * must be those of the condition's operator, as
 * match_eclasses_to_foreign_key_col() matches a foreign key column.
 */
static bool
ec_has_join_condition(EquivalenceClass *ec,
					  RelOptInfo *anchor_rel, RelOptInfo *other_rel,
					  AttrNumber anchor_key, AttrNumber other_key,
					  Oid opno)
{
	EquivalenceMemberIterator it;
	EquivalenceMember *em;
	Relids		join_relids;
	bool		anchor_found = false;
	bool		other_found = false;

	join_relids = bms_union(anchor_rel->relids, other_rel->relids);
	setup_eclass_member_iterator(&it, ec, join_relids);
	while ((em = eclass_member_iterator_next(&it)) != NULL)
	{
		Var		   *var = (Var *) em->em_expr;

		while (var && IsA(var, RelabelType))
			var = (Var *) ((RelabelType *) var)->arg;
		if (!(var && IsA(var, Var)))
			continue;

		if (bms_is_member(var->varno, anchor_rel->relids) &&
			var->varattno == anchor_key)
			anchor_found = true;
		else if (bms_is_member(var->varno, other_rel->relids) &&
				 var->varattno == other_key)
			other_found = true;
	}
	bms_free(join_relids);

	if (!anchor_found || !other_found)
		return false;

	return equal(get_mergejoin_opfamilies(opno), ec->ec_opfamilies);
}

/*
 * clauses_have_join_condition -
 *	  Do the join clauses estimated at a join step enforce a stat's join
 *	  condition between anchor_rel and other_rel?
 *
 * A clause enforces the condition if it is the condition itself, or comes
 * from an EquivalenceClass equating the two columns.  For a join the planner
 * generates one clause per class, between whichever two members it picks, so
 * the class rather than the clause shows the condition holds at this step.
 */
static bool
clauses_have_join_condition(List *clauses,
							RelOptInfo *anchor_rel, RelOptInfo *other_rel,
							AttrNumber anchor_key, AttrNumber other_key,
							Oid opno)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo;
		EquivalenceClass *ec;

		if (!IsA(lfirst(lc), RestrictInfo))
			continue;
		rinfo = (RestrictInfo *) lfirst(lc);

		if (clause_is_join_condition(rinfo->clause,
									 anchor_rel->relids, other_rel->relids,
									 anchor_key, other_key, opno))
			return true;

		/*
		 * A generated clause records its class in parent_ec; a source clause
		 * of a class, reused as it is, has the class as both left_ec and
		 * right_ec.
		 */
		ec = rinfo->parent_ec;
		if (ec == NULL && rinfo->left_ec != NULL &&
			rinfo->left_ec == rinfo->right_ec)
			ec = rinfo->left_ec;
		if (ec != NULL &&
			ec_has_join_condition(ec, anchor_rel, other_rel,
								  anchor_key, other_key, opno))
			return true;
	}

	return false;
}

/*
 * stat_describes_join -
 *	  Does a stat describe the join of anchor_rel and other_rel being
 *	  estimated?
 *
 * The stat's condition must be among clauses, the join clauses estimated at
 * this step, and the query must have no other join condition between the two
 * relations.  Those are counted from the planner's data rather than from
 * clauses, since a caller may estimate only some of them, such as the
 * clauses of a foreign key.  Requiring the condition among the step's
 * clauses is what applies a stat exactly once: a clause pushed down into a
 * parameterized path is estimated there, and is absent from the join above
 * it.
 *
 * Sets *anchor_key and *other_key to the columns of the stat's condition.
 *
 * XXX Several conditions would need each of the stat's conditions matched to
 * a distinct join condition of the query, and composite key lookups in
 * ANALYZE.
 */
static bool
stat_describes_join(PlannerInfo *root,
					StatisticExtInfo *stat,
					RelOptInfo *anchor_rel,
					RelOptInfo *other_rel,
					List *clauses,
					AttrNumber *anchor_key,
					AttrNumber *other_key)
{
	OpExpr	   *cond;
	Var		   *anchor_var;
	Var		   *other_var;
	bool		commuted;
	Oid			opno;

	if (list_length(stat->joinconds) != 1)
		return false;

	cond = linitial_node(OpExpr, stat->joinconds);
	join_stats_cond_sides(cond, &anchor_var, &other_var, &commuted);
	Assert(anchor_var->varno == 1 && other_var->varno == 2);

	/* the operator for the (anchor, other) order */
	opno = commuted ? get_commutator(cond->opno) : cond->opno;
	if (!OidIsValid(opno))
		return false;

	*anchor_key = anchor_var->varattno;
	*other_key = other_var->varattno;

	if (count_relevant_joinclauses(root, anchor_rel, other_rel) != 1)
		return false;

	return clauses_have_join_condition(clauses, anchor_rel, other_rel,
									   *anchor_key, *other_key, opno);
}

/*
 * join_mcv_combine_selectivities
 *		Combine a join clause's selectivity with the join MCV selectivity
 *		estimates of the filters.
 *
 * join_sel is an estimate of the join clause alone: the standard one for an
 * ordinary join, or the foreign key's in get_foreign_key_join_selectivity().
 * The join MCV statistics do not replace it: their frequencies are fractions
 * of the joined rows, so what they estimate is how often the covered filters
 * hold among the rows that join.
 *
 * covered_join_sel is that estimate, the selectivity of the covered filters
 * over the joined rows, P(covered_filters | join).  covered_rels_sel is the
 * product, over the joined relations, of the selectivity of each relation's
 * covered filters over that relation.  The planner computes:
 *
 *     nrows = rows(rel_1) * rows(rel_2) * ... * sel(clause)
 *
 * where each relation's rows already reflect its base restrictions, the
 * covered filters among them.  Of the rows of the join without filters,
 * covered_join_sel satisfy the covered filters.  Dividing out the share of
 * the relations' rows the covered filters account for gives:
 *
 *     sel = join_sel * covered_join_sel / covered_rels_sel
 *
 * The quotient is 1 if the filters are unrelated to the join.  The result is
 * not clamped, as the caller applies the corrections of all its pairs and
 * clamps once.
 */
static Selectivity
join_mcv_combine_selectivities(Selectivity join_sel,
							   Selectivity covered_join_sel,
							   Selectivity covered_rels_sel)
{
	Assert(covered_rels_sel > 0);

	return join_sel * covered_join_sel / covered_rels_sel;
}

/*
 * join_mcv_pair_selectivities
 *		Estimate the selectivity of the filters covered by join MCV
 *		statistics, within the join and in the tables.
 *
 * Searches the statlists of a pair of base relations for a join stat that
 * describes their join, given the join clauses being estimated.  *joinsel is
 * set to the share of the join that satisfies the filters the stat covers,
 * and *relssel to the product, over the two relations, of the selectivity of
 * each relation's covered filters.  The caller combines the two with its
 * estimate of the join clause using join_mcv_combine_selectivities().
 * Uncovered filters need nothing here, as they are already reflected in the
 * relations' rows.
 *
 * Returns false if no join stat applies.  *joinsel may be 0, when a stat
 * storing every combination in the join has none matching the filters.
 */
static bool
join_mcv_pair_selectivities(PlannerInfo *root,
							RelOptInfo *baserel1,
							RelOptInfo *baserel2,
							List *clauses,
							Selectivity *joinsel,
							Selectivity *relssel)
{
	RelOptInfo *rels[2];
	int			r;
	Selectivity best_join_sel = 0.0;
	Selectivity best_rels_sel = 0.0;
	int			best_ncovered = 0;

	if (baserel1->rtekind != RTE_RELATION || baserel2->rtekind != RTE_RELATION)
		return false;

	/* The floors on the covered selectivities below divide by tuples */
	if (baserel1->tuples <= 0 || baserel2->tuples <= 0)
		return false;

	rels[0] = baserel1;
	rels[1] = baserel2;

	/*
	 * A join stat is in the statlist of its anchor, so try each relation as
	 * the anchor.  A stat covering all the filters is used at once, and
	 * otherwise the one covering the most.
	 */
	for (r = 0; r < 2; r++)
	{
		RelOptInfo *anchor_rel = rels[r];
		RelOptInfo *other_rel = rels[1 - r];
		Oid			other_reloid = root->simple_rte_array[other_rel->relid]->relid;
		ListCell   *lc;
		int			nfilters;	/* total baserestrictinfo clauses on both rels */

		nfilters = list_length(other_rel->baserestrictinfo) +
			list_length(anchor_rel->baserestrictinfo);

		foreach(lc, anchor_rel->statlist)
		{
			StatisticExtInfo *stat = (StatisticExtInfo *) lfirst(lc);
			JoinStatRel statrels[2];
			List	   *filters;
			Selectivity covered_join_sel;
			Selectivity covered_rels_sel;
			Selectivity mcv_basesel;
			Selectivity mcv_totalsel;
			AttrNumber	anchor_key;
			AttrNumber	other_key;
			int			ncovered;	/* number of filters the stat covers */
			int			i;

			/* Skip non-join and non-MCV stats */
			if (stat->joinrels == NIL || stat->kind != STATS_EXT_MCV)
				continue;

			/* Check that this stat joins to the other relation */
			if (!list_member_oid(stat->joinrels, other_reloid))
				continue;

			if (!stat_describes_join(root, stat, anchor_rel, other_rel, clauses,
									 &anchor_key, &other_key))
				continue;

			/* the stat's definition numbers the anchor 1, the other table 2 */
			statrels[0].rel = anchor_rel;
			statrels[0].stat_varno = 1;
			statrels[0].joinkey = anchor_key;
			statrels[1].rel = other_rel;
			statrels[1].stat_varno = 2;
			statrels[1].joinkey = other_key;

			/*
			 * Which filters does it cover?  Without at least one, the stat
			 * cannot improve on the standard join selectivity estimate.
			 */
			filters = NIL;
			for (i = 0; i < lengthof(statrels); i++)
				match_filters_for_rel(root, stat, statrels, lengthof(statrels),
									  &statrels[i], &filters);
			if (filters == NIL)
				continue;
			ncovered = list_length(filters);

			/*
			 * The selectivity of each relation's covered filters, through
			 * clauselist_selectivity() so that single-table extended
			 * statistics are used, as they were for the relation's rows.  It
			 * is at least one row, as set_baserel_size_estimates() clamps the
			 * rows to at least one.  When the covered filters are all of the
			 * relation's restrictions, this is that clamp exactly.
			 */
			covered_rels_sel = 1.0;
			for (i = 0; i < lengthof(statrels); i++)
			{
				List	   *rinfos = NIL;
				Selectivity sel = 1.0;

				foreach_ptr(JoinMCVFilter, filter, filters)
				{
					if (filter->rel == statrels[i].rel)
						rinfos = lappend(rinfos, filter->rinfo);
				}
				if (rinfos != NIL)
					sel = clauselist_selectivity(root, rinfos, 0, JOIN_INNER,
												 NULL);
				covered_rels_sel *= Max(sel, 1.0 / statrels[i].rel->tuples);
			}

			/*
			 * Combine the share of the join the matching items measure with
			 * the standard estimate for the rest of the join, as
			 * mcv_combine_selectivities() does for the single-table code. The
			 * standard estimate here is covered_rels_sel, the share of the
			 * join the filters select if they are unrelated to it.
			 */
			covered_join_sel = join_mcv_clauselist_selectivity(root, stat,
															   statrels,
															   lengthof(statrels),
															   filters,
															   &mcv_basesel,
															   &mcv_totalsel);
			covered_join_sel = mcv_combine_selectivities(covered_rels_sel,
														 covered_join_sel,
														 mcv_basesel,
														 mcv_totalsel);

			/* Full coverage: return immediately */
			if (ncovered >= nfilters)
			{
				*joinsel = covered_join_sel;
				*relssel = covered_rels_sel;
				return true;
			}

			/* Partial coverage: save if better than previous best */
			if (ncovered > best_ncovered)
			{
				/*
				 * With a relation's rows at the one-row floor, the uncovered
				 * filters are seen only through that clamped value, so a
				 * result that raises the estimate would divide out the
				 * covered filters alone and inflate it.  A result that lowers
				 * the estimate is kept: adding the uncovered filters cannot
				 * make the covered ones more common in the join.
				 */
				if (covered_join_sel > covered_rels_sel &&
					(other_rel->rows <= 1.0 || anchor_rel->rows <= 1.0))
					continue;

				best_join_sel = covered_join_sel;
				best_rels_sel = covered_rels_sel;
				best_ncovered = ncovered;
			}
		}
	}

	if (best_ncovered > 0)
	{
		*joinsel = best_join_sel;
		*relssel = best_rels_sel;
		return true;
	}

	return false;
}

/*
 * join_mcv_join_selectivity
 *		Correct the selectivity of a join step with join MCV statistics.
 *
 * outer_relids and inner_relids are the relations joined at this step,
 * clauses the join clauses estimated at it (all of them, including any a
 * foreign key accounts for), and join_sel the planner's estimate of those
 * clauses: the product of their standard selectivities and of any foreign
 * key's, or their estimate as restrictions for a parameterized scan.  For
 * each pair of base relations across the two sides whose join a stat
 * describes, join_sel is corrected for the filters the stat covers, with
 * join_mcv_combine_selectivities().  Returns the corrected selectivity.
 *
 * A stat is applied at the step whose clauses enforce its join condition,
 * so its correction enters the join's estimate once, whatever the join order
 * and however the condition appears: as a clause of the step, removed by a
 * foreign key, generated by an EquivalenceClass, or pushed down into a
 * parameterized scan, which is then corrected instead of the join above it.
 * The corrections of the pairs are multiplied and the result clamped once.
 */
Selectivity
join_mcv_join_selectivity(PlannerInfo *root,
						  Relids outer_relids,
						  Relids inner_relids,
						  List *clauses,
						  Selectivity join_sel)
{
	int			outer_relid = -1;

	/* Nothing to search for if no relation of the query has join stats */
	if (!root->hasJoinStats)
		return join_sel;

	while ((outer_relid = bms_next_member(outer_relids, outer_relid)) >= 0)
	{
		RelOptInfo *outer_rel = find_base_rel_ignore_join(root, outer_relid);
		int			inner_relid = -1;

		if (outer_rel == NULL)
			continue;

		while ((inner_relid = bms_next_member(inner_relids, inner_relid)) >= 0)
		{
			RelOptInfo *inner_rel = find_base_rel_ignore_join(root, inner_relid);
			Selectivity covered_join_sel;
			Selectivity covered_rels_sel;

			if (inner_rel == NULL)
				continue;

			if (join_mcv_pair_selectivities(root, outer_rel, inner_rel,
											clauses,
											&covered_join_sel,
											&covered_rels_sel))
				join_sel = join_mcv_combine_selectivities(join_sel,
														  covered_join_sel,
														  covered_rels_sel);
		}
	}
	CLAMP_PROBABILITY(join_sel);

	return join_sel;
}

/*
 * join_mcv_restriction_selectivity
 *		Correct the selectivity of a join clause estimated as a restriction.
 *
 * A join clause is estimated as a restriction on varRelid when a scan of
 * that relation applies it, parameterized by the other relations the clause
 * mentions: get_parameterized_baserel_size() estimates the scan's rows that
 * way, and the index cost estimators the quals of its indexes.  The scan
 * enforces the clause, so its join statistics apply here, to sel.  The join
 * above the scan does not see the clause and does not correct for it again.
 *
 * Not when the relation is the left-hand side of a semi join whose
 * right-hand side supplies the parameters.  That side is made unique before
 * the join, so the scan's outer rows are not joined rows, of which the
 * statistics' frequencies are fractions.
 */
Selectivity
join_mcv_restriction_selectivity(PlannerInfo *root,
								 RestrictInfo *rinfo,
								 int varRelid,
								 Selectivity sel)
{
	RelOptInfo *rel;
	Relids		outer_relids;
	ListCell   *lc;

	if (!root->hasJoinStats)
		return sel;

	rel = find_base_rel_ignore_join(root, varRelid);
	if (rel == NULL)
		return sel;

	outer_relids = bms_del_member(bms_copy(rinfo->clause_relids), varRelid);

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_SEMI &&
			bms_is_member(varRelid, sjinfo->syn_lefthand) &&
			bms_overlap(outer_relids, sjinfo->syn_righthand))
			return sel;
	}

	return join_mcv_join_selectivity(root, outer_relids, rel->relids,
									 list_make1(rinfo), sel);
}
