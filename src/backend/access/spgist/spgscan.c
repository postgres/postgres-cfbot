/*-------------------------------------------------------------------------
 *
 * spgscan.c
 *	  routines for scanning SP-GiST indexes
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/spgist_private.h"
#include "executor/instrument_node.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static Buffer spgReadItemPage(IndexScanDesc scan, SpGistSearchItem *item,
							  Buffer buffer);
static void spgProcessInnerPage(IndexScanDesc scan, SpGistSearchItem *item,
								Page page);
static void spgProcessLeafPage(IndexScanDesc scan, SpGistSearchItem *item,
							   Page page, IndexScanBatch batch);

/*
 * Pairing heap comparison function for the SpGistSearchItem queue.
 * KNN-searches currently only support NULLS LAST.  So, preserve this logic
 * here.
 */
static int
pairingheap_SpGistSearchItem_cmp(const pairingheap_node *a,
								 const pairingheap_node *b, void *arg)
{
	const SpGistSearchItem *sa = (const SpGistSearchItem *) a;
	const SpGistSearchItem *sb = (const SpGistSearchItem *) b;
	SpGistScanOpaque so = (SpGistScanOpaque) arg;
	int			i;

	if (sa->isNull)
	{
		if (!sb->isNull)
			return -1;
	}
	else if (sb->isNull)
	{
		return 1;
	}
	else
	{
		/* Order according to distance comparison */
		for (i = 0; i < so->numberOfNonNullOrderBys; i++)
		{
			if (isnan(sa->distances[i]) && isnan(sb->distances[i]))
				continue;		/* NaN == NaN */
			if (isnan(sa->distances[i]))
				return -1;		/* NaN > number */
			if (isnan(sb->distances[i]))
				return 1;		/* number < NaN */
			if (sa->distances[i] != sb->distances[i])
				return (sa->distances[i] < sb->distances[i]) ? 1 : -1;
		}
	}

	/* Leaf items go before inner pages, to ensure a depth-first search */
	if (sa->isLeaf && !sb->isLeaf)
		return 1;
	if (!sa->isLeaf && sb->isLeaf)
		return -1;

	return 0;
}

static void
spgFreeSearchItem(SpGistScanOpaque so, SpGistSearchItem *item)
{
	/* value is of type attType if isLeaf, else of type attLeafType */
	/* (no, that is not backwards; yes, it's confusing) */
	if (!(item->isLeaf ? so->state.attType.attbyval :
		  so->state.attLeafType.attbyval) &&
		DatumGetPointer(item->value) != NULL)
		pfree(DatumGetPointer(item->value));

	if (item->leafTuple)
		pfree(item->leafTuple);

	if (item->traversalValue)
		pfree(item->traversalValue);

	pfree(item);
}

/*
 * Add SpGistSearchItem to queue
 *
 * Called in queue context
 */
static void
spgAddSearchItemToQueue(SpGistScanOpaque so, SpGistSearchItem *item)
{
	pairingheap_add(so->scanQueue, &item->phNode);
}

static SpGistSearchItem *
spgAllocSearchItem(SpGistScanOpaque so, bool isnull, double *distances)
{
	/* allocate distance array only for non-NULL items */
	SpGistSearchItem *item =
		palloc(SizeOfSpGistSearchItem(isnull ? 0 : so->numberOfNonNullOrderBys));

	item->isNull = isnull;

	if (!isnull && so->numberOfNonNullOrderBys > 0)
		memcpy(item->distances, distances,
			   sizeof(item->distances[0]) * so->numberOfNonNullOrderBys);

	return item;
}

static void
spgAddStartItem(SpGistScanOpaque so, bool isnull)
{
	SpGistSearchItem *startEntry =
		spgAllocSearchItem(so, isnull, so->zeroDistances);

	ItemPointerSet(&startEntry->heapPtr,
				   isnull ? SPGIST_NULL_BLKNO : SPGIST_ROOT_BLKNO,
				   FirstOffsetNumber);
	startEntry->isLeaf = false;
	startEntry->level = 0;
	startEntry->value = (Datum) 0;
	startEntry->leafTuple = NULL;
	startEntry->traversalValue = NULL;
	startEntry->recheck = false;
	startEntry->recheckDistances = false;

	spgAddSearchItemToQueue(so, startEntry);
}

/*
 * Initialize queue to search the root page, resetting
 * any previously active scan
 */
static void
resetSpGistScanOpaque(SpGistScanOpaque so)
{
	MemoryContext oldCtx;

	MemoryContextReset(so->traversalCxt);

	oldCtx = MemoryContextSwitchTo(so->traversalCxt);

	/* initialize queue only for distance-ordered scans */
	so->scanQueue = pairingheap_allocate(pairingheap_SpGistSearchItem_cmp, so);

	if (so->searchNulls)
		/* Add a work item to scan the null index entries */
		spgAddStartItem(so, true);

	if (so->searchNonNulls)
		/* Add a work item to scan the non-null index entries */
		spgAddStartItem(so, false);

	MemoryContextSwitchTo(oldCtx);
}

/*
 * Prepare scan keys in SpGistScanOpaque from caller-given scan keys
 *
 * Sets searchNulls, searchNonNulls, numberOfKeys, keyData fields of *so.
 *
 * The point here is to eliminate null-related considerations from what the
 * opclass consistent functions need to deal with.  We assume all SPGiST-
 * indexable operators are strict, so any null RHS value makes the scan
 * condition unsatisfiable.  We also pull out any IS NULL/IS NOT NULL
 * conditions; their effect is reflected into searchNulls/searchNonNulls.
 */
static void
spgPrepareScanKeys(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	bool		qual_ok;
	bool		haveIsNull;
	bool		haveNotNull;
	int			nkeys;
	int			i;

	so->numberOfOrderBys = scan->numberOfOrderBys;
	so->orderByData = scan->orderByData;

	if (so->numberOfOrderBys <= 0)
		so->numberOfNonNullOrderBys = 0;
	else
	{
		int			j = 0;

		/*
		 * Remove all NULL keys, but remember their offsets in the original
		 * array.
		 */
		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = &so->orderByData[i];

			if (skey->sk_flags & SK_ISNULL)
				so->nonNullOrderByOffsets[i] = -1;
			else
			{
				if (i != j)
					so->orderByData[j] = *skey;

				so->nonNullOrderByOffsets[i] = j++;
			}
		}

		so->numberOfNonNullOrderBys = j;
	}

	if (scan->numberOfKeys <= 0)
	{
		/* If no quals, whole-index scan is required */
		so->searchNulls = true;
		so->searchNonNulls = true;
		so->numberOfKeys = 0;
		return;
	}

	/* Examine the given quals */
	qual_ok = true;
	haveIsNull = haveNotNull = false;
	nkeys = 0;
	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		skey = &scan->keyData[i];

		if (skey->sk_flags & SK_SEARCHNULL)
			haveIsNull = true;
		else if (skey->sk_flags & SK_SEARCHNOTNULL)
			haveNotNull = true;
		else if (skey->sk_flags & SK_ISNULL)
		{
			/* ordinary qual with null argument - unsatisfiable */
			qual_ok = false;
			break;
		}
		else
		{
			/* ordinary qual, propagate into so->keyData */
			so->keyData[nkeys++] = *skey;
			/* this effectively creates a not-null requirement */
			haveNotNull = true;
		}
	}

	/* IS NULL in combination with something else is unsatisfiable */
	if (haveIsNull && haveNotNull)
		qual_ok = false;

	/* Emit results */
	if (qual_ok)
	{
		so->searchNulls = haveIsNull;
		so->searchNonNulls = haveNotNull;
		so->numberOfKeys = nkeys;
	}
	else
	{
		so->searchNulls = false;
		so->searchNonNulls = false;
		so->numberOfKeys = 0;
	}
}

IndexScanDesc
spgbeginscan(Relation rel, int keysz, int orderbysz)
{
	IndexScanDesc scan;
	SpGistScanOpaque so;
	int			i;

	scan = RelationGetIndexScan(rel, keysz, orderbysz);

	so = palloc0_object(SpGistScanOpaqueData);
	if (keysz > 0)
		so->keyData = palloc_array(ScanKeyData, keysz);
	else
		so->keyData = NULL;
	initSpGistState(&so->state, scan->indexRelation);

	so->tempCxt = AllocSetContextCreate(CurrentMemoryContext,
										"SP-GiST search temporary context",
										ALLOCSET_DEFAULT_SIZES);
	so->traversalCxt = AllocSetContextCreate(CurrentMemoryContext,
											 "SP-GiST traversal-value context",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Set up reconTupDesc and xs_hitupdesc in case it's an index-only scan,
	 * making sure that the key column is shown as being of type attType.
	 * (It's rather annoying to do this work when it might be wasted, but for
	 * most opclasses we can re-use the index reldesc instead of making one.)
	 */
	so->reconTupDesc = scan->xs_hitupdesc =
		getSpGistTupleDesc(rel, &so->state.attType);
	so->reconCxt = AllocSetContextCreate(CurrentMemoryContext,
										 "SP-GiST reconstruction context",
										 ALLOCSET_SMALL_SIZES);

	/* Allocate various arrays needed for order-by scans */
	if (scan->numberOfOrderBys > 0)
	{
		/* This will be filled in spgrescan, but allocate the space here */
		so->orderByTypes = palloc_array(Oid, scan->numberOfOrderBys);
		so->nonNullOrderByOffsets = palloc_array(int, scan->numberOfOrderBys);

		/* These arrays have constant contents, so we can fill them now */
		so->zeroDistances = palloc_array(double, scan->numberOfOrderBys);
		so->infDistances = palloc_array(double, scan->numberOfOrderBys);

		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			so->zeroDistances[i] = 0.0;
			so->infDistances[i] = get_float8_infinity();
		}

		scan->xs_orderbyvals = palloc0_array(Datum, scan->numberOfOrderBys);
		scan->xs_orderbynulls = palloc_array(bool, scan->numberOfOrderBys);
		memset(scan->xs_orderbynulls, true,
			   sizeof(bool) * scan->numberOfOrderBys);

		/*
		 * Ordered scans fill a "virtual" batch by draining the
		 * distance-ordered queue, so the batch size is a tuning knob with no
		 * natural value. Testing has shown that a very small size will
		 * increase per-batch overhead (and likely instruction-cache misses),
		 * while a large size (such as MaxIndexTuplesPerPage) risks producing
		 * many tuples that a LIMIT node never consumes.  This maxitemsbatch
		 * is a compromise.
		 */
		scan->maxitemsbatch = MaxIndexTuplesPerPage / 32;
	}
	else
	{
		/*
		 * A non-ordered batch holds all of the matches from a single leaf
		 * page, so one page's worth of items is the natural cap.  Using
		 * MaxIndexTuplesPerPage is a bit hokey since SpGistLeafTuples aren't
		 * exactly IndexTuples; however, they are larger, so this is safe.
		 */
		scan->maxitemsbatch = MaxIndexTuplesPerPage;
	}

	fmgr_info_copy(&so->innerConsistentFn,
				   index_getprocinfo(rel, 1, SPGIST_INNER_CONSISTENT_PROC),
				   CurrentMemoryContext);

	fmgr_info_copy(&so->leafConsistentFn,
				   index_getprocinfo(rel, 1, SPGIST_LEAF_CONSISTENT_PROC),
				   CurrentMemoryContext);

	so->indexCollation = rel->rd_indcollation[0];

	scan->batch_index_opaque_static = MAXALIGN(sizeof(SpGistBatchData));
	scan->batch_tuples_workspace = BLCKSZ;

	scan->opaque = so;

	return scan;
}

void
spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	if (scan->numberOfOrderBys > 0 && !scan->batchImmediateUnguard)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SP-GiST ordered index scans require a heap-fetching scan with an MVCC-compliant snapshot")));

	/* copy scankeys into local storage */
	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));

	/* initialize order-by data if needed */
	if (orderbys && scan->numberOfOrderBys > 0)
	{
		int			i;

		memcpy(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));

		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			ScanKey		skey = &scan->orderByData[i];

			/*
			 * Look up the datatype returned by the original ordering
			 * operator. SP-GiST always uses a float8 for the distance
			 * function, but the ordering operator could be anything else.
			 *
			 * XXX: The distance function is only allowed to be lossy if the
			 * ordering operator's result type is float4 or float8.  Otherwise
			 * we don't know how to return the distance to the executor.  But
			 * we cannot check that here, as we won't know if the distance
			 * function is lossy until it returns *recheck = true for the
			 * first time.
			 */
			so->orderByTypes[i] = get_func_rettype(skey->sk_func.fn_oid);
		}
	}

	/* preprocess scankeys, set up the representation in *so */
	spgPrepareScanKeys(scan);

	/*
	 * Size the dynamic opaque area now that xs_want_itup is known.  Ordered
	 * (virtual) batches need a full SpGistBatchItem array (each item's ORDER
	 * BY distances included); non-ordered batches need only a recheck flag
	 * per item.  Index-only scans (always non-ordered) need extra room after
	 * the array for the reconstruction prefix (see SpGistBatchGetReconArea
	 * and spgcanreturn).
	 *
	 * We do this here rather than in spgbeginscan because xs_want_itup is set
	 * by index_beginscan only after ambeginscan returns.
	 */
	if (scan->numberOfOrderBys > 0)
		scan->batch_index_opaque_dyn = SpGistBatchItemArraySize(scan);
	else
		scan->batch_index_opaque_dyn = SpGistBatchRecheckArraySize(scan);
	if (scan->xs_want_itup)
		scan->batch_index_opaque_dyn += SpGistBatchReconAreaSize;

	/* set up starting queue entries */
	resetSpGistScanOpaque(so);

	/* discard any index-only tuple reconstructed by a previous scan */
	MemoryContextReset(so->reconCxt);
	scan->xs_hitup = NULL;

	/* count an indexscan for stats */
	pgstat_count_index_scan(scan->indexRelation);
	if (scan->instrument)
		scan->instrument->nsearches++;
}

void
spgendscan(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	MemoryContextDelete(so->tempCxt);
	MemoryContextDelete(so->traversalCxt);
	MemoryContextDelete(so->reconCxt);

	if (so->keyData)
		pfree(so->keyData);

	if (so->state.leafTupDesc &&
		so->state.leafTupDesc != RelationGetDescr(so->state.index))
		FreeTupleDesc(so->state.leafTupDesc);

	if (so->state.deadTupleStorage)
		pfree(so->state.deadTupleStorage);

	if (scan->numberOfOrderBys > 0)
	{
		pfree(so->orderByTypes);
		pfree(so->nonNullOrderByOffsets);
		pfree(so->zeroDistances);
		pfree(so->infDistances);
		pfree(scan->xs_orderbyvals);
		pfree(scan->xs_orderbynulls);
	}

	pfree(so);
}

/*
 * Leaf SpGistSearchItem constructor, called in queue context
 */
static SpGistSearchItem *
spgNewHeapItem(IndexScanDesc scan, int level, SpGistLeafTuple leafTuple,
			   Datum leafValue, bool recheck, bool recheckDistances,
			   bool isnull, double *distances)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	SpGistSearchItem *item = spgAllocSearchItem(so, isnull, distances);

	item->level = level;
	item->heapPtr = leafTuple->heapPtr;

	/*
	 * If we need the reconstructed value, copy it to queue cxt out of tmp
	 * cxt.  Caution: the leaf_consistent method may not have supplied a value
	 * if we didn't ask it to, and mildly-broken methods might supply one of
	 * the wrong type.  The correct leafValue type is attType not leafType.
	 */
	if (scan->xs_want_itup)
	{
		item->value = isnull ? (Datum) 0 :
			datumCopy(leafValue, so->state.attType.attbyval,
					  so->state.attType.attlen);

		/*
		 * If we're going to need to reconstruct INCLUDE attributes, store the
		 * whole leaf tuple so we can get the INCLUDE attributes out of it.
		 */
		if (so->state.leafTupDesc->natts > 1)
		{
			item->leafTuple = palloc(leafTuple->size);
			memcpy(item->leafTuple, leafTuple, leafTuple->size);
		}
		else
			item->leafTuple = NULL;
	}
	else
	{
		item->value = (Datum) 0;
		item->leafTuple = NULL;
	}
	item->traversalValue = NULL;
	item->isLeaf = true;
	item->recheck = recheck;
	item->recheckDistances = recheckDistances;

	return item;
}

/*
 * Test whether a leaf tuple satisfies all the scan keys.
 *
 * When a match is found, an ordered scan queues the heap tuple for later
 * distance-ordered draining.  A non-ordered scan appends it to batch.
 *
 * 'batch' arg is NULL for ordered scans.
 */
static bool
spgLeafTest(IndexScanDesc scan, SpGistSearchItem *item,
			SpGistLeafTuple leafTuple, bool isnull, IndexScanBatch batch)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	Datum		leafValue;
	double	   *distances;
	bool		result;
	bool		recheck;
	bool		recheckDistances;

	if (isnull)
	{
		/* Should not have arrived on a nulls page unless nulls are wanted */
		Assert(so->searchNulls);
		leafValue = (Datum) 0;
		distances = NULL;
		recheck = false;
		recheckDistances = false;
		result = true;
	}
	else
	{
		spgLeafConsistentIn in;
		spgLeafConsistentOut out;

		/* use temp context for calling leaf_consistent */
		MemoryContext oldCxt = MemoryContextSwitchTo(so->tempCxt);

		in.scankeys = so->keyData;
		in.nkeys = so->numberOfKeys;
		in.orderbys = so->orderByData;
		in.norderbys = so->numberOfNonNullOrderBys;
		Assert(!item->isLeaf);	/* else reconstructedValue would be wrong type */
		in.reconstructedValue = item->value;
		in.traversalValue = item->traversalValue;
		in.level = item->level;
		in.returnData = scan->xs_want_itup;
		in.leafDatum = SGLTDATUM(leafTuple, &so->state);

		out.leafValue = (Datum) 0;
		out.recheck = false;
		out.distances = NULL;
		out.recheckDistances = false;

		result = DatumGetBool(FunctionCall2Coll(&so->leafConsistentFn,
												so->indexCollation,
												PointerGetDatum(&in),
												PointerGetDatum(&out)));
		recheck = out.recheck;
		recheckDistances = out.recheckDistances;
		leafValue = out.leafValue;
		distances = out.distances;

		MemoryContextSwitchTo(oldCxt);
	}

	if (result)
	{
		/* item passes the scankeys */
		if (scan->numberOfOrderBys > 0)
		{
			/* The scan is ordered; add the item to the queue */
			MemoryContext oldCxt;
			SpGistSearchItem *heapItem;

			Assert(scan->batchImmediateUnguard);

			oldCxt = MemoryContextSwitchTo(so->traversalCxt);
			heapItem = spgNewHeapItem(scan, item->level, leafTuple, leafValue,
									  recheck, recheckDistances, isnull,
									  distances);

			spgAddSearchItemToQueue(so, heapItem);

			MemoryContextSwitchTo(oldCxt);
		}
		else
		{
			/*
			 * The scan is non-ordered; add the item to caller's batch
			 * directly.
			 */
			int			i = ++batch->lastItem;

			Assert(!recheckDistances);
			Assert(i < scan->maxitemsbatch);

			batch->items[i].tableTid = leafTuple->heapPtr;
			batch->items[i].indexOffset = InvalidOffsetNumber;	/* meaningless */
			batch->items[i].tupleOffset = 0;

			SpGistBatchGetRecheck(scan, batch)[i] = recheck;

			if (scan->xs_want_itup)
			{
				Size		sz = leafTuple->size;
				int			off = 0;

				if (i > batch->firstItem)
				{
					int			prev = batch->items[i - 1].tupleOffset;

					/*
					 * Copy tuple to point immediately after most recently
					 * appended tuple
					 */
					off = prev + ((SpGistLeafTuple) (batch->currTuples + prev))->size;
				}

				batch->items[i].tupleOffset = off;
				Assert(off + sz <= scan->batch_tuples_workspace);
				memcpy(batch->currTuples + off, leafTuple, sz);
			}
		}
	}

	return result;
}

/* A bundle initializer for inner_consistent methods */
static void
spgInitInnerConsistentIn(spgInnerConsistentIn *in,
						 IndexScanDesc scan,
						 SpGistSearchItem *item,
						 SpGistInnerTuple innerTuple)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	in->scankeys = so->keyData;
	in->orderbys = so->orderByData;
	in->nkeys = so->numberOfKeys;
	in->norderbys = so->numberOfNonNullOrderBys;
	Assert(!item->isLeaf);		/* else reconstructedValue would be wrong type */
	in->reconstructedValue = item->value;
	in->traversalMemoryContext = so->traversalCxt;
	in->traversalValue = item->traversalValue;
	in->level = item->level;
	in->returnData = scan->xs_want_itup;
	in->allTheSame = innerTuple->allTheSame;
	in->hasPrefix = (innerTuple->prefixSize > 0);
	in->prefixDatum = SGITDATUM(innerTuple, &so->state);
	in->nNodes = innerTuple->nNodes;
	in->nodeLabels = spgExtractNodeLabels(&so->state, innerTuple);
}

static SpGistSearchItem *
spgMakeInnerItem(SpGistScanOpaque so,
				 SpGistSearchItem *parentItem,
				 SpGistNodeTuple tuple,
				 spgInnerConsistentOut *out, int i, bool isnull,
				 double *distances)
{
	SpGistSearchItem *item = spgAllocSearchItem(so, isnull, distances);

	item->heapPtr = tuple->t_tid;
	item->level = out->levelAdds ? parentItem->level + out->levelAdds[i]
		: parentItem->level;

	/* Must copy value out of temp context */
	/* (recall that reconstructed values are of type leafType) */
	item->value = out->reconstructedValues
		? datumCopy(out->reconstructedValues[i],
					so->state.attLeafType.attbyval,
					so->state.attLeafType.attlen)
		: (Datum) 0;

	item->leafTuple = NULL;

	/*
	 * Elements of out.traversalValues should be allocated in
	 * in.traversalMemoryContext, which is actually a long lived context of
	 * index scan.
	 */
	item->traversalValue =
		out->traversalValues ? out->traversalValues[i] : NULL;

	item->isLeaf = false;
	item->recheck = false;
	item->recheckDistances = false;

	return item;
}

static void
spgInnerTest(IndexScanDesc scan, SpGistSearchItem *item,
			 SpGistInnerTuple innerTuple, bool isnull)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	MemoryContext oldCxt = MemoryContextSwitchTo(so->tempCxt);
	spgInnerConsistentOut out;
	int			nNodes = innerTuple->nNodes;
	int			i;

	memset(&out, 0, sizeof(out));

	if (!isnull)
	{
		spgInnerConsistentIn in;

		spgInitInnerConsistentIn(&in, scan, item, innerTuple);

		/* use user-defined inner consistent method */
		FunctionCall2Coll(&so->innerConsistentFn,
						  so->indexCollation,
						  PointerGetDatum(&in),
						  PointerGetDatum(&out));
	}
	else
	{
		/* force all children to be visited */
		out.nNodes = nNodes;
		out.nodeNumbers = palloc_array(int, nNodes);
		for (i = 0; i < nNodes; i++)
			out.nodeNumbers[i] = i;
	}

	/* If allTheSame, they should all or none of them match */
	if (innerTuple->allTheSame && out.nNodes != 0 && out.nNodes != nNodes)
		elog(ERROR, "inconsistent inner_consistent results for allTheSame inner tuple");

	if (out.nNodes)
	{
		/* collect node pointers */
		SpGistNodeTuple node;
		SpGistNodeTuple *nodes = palloc_array(SpGistNodeTuple, nNodes);

		SGITITERATE(innerTuple, i, node)
		{
			nodes[i] = node;
		}

		MemoryContextSwitchTo(so->traversalCxt);

		for (i = 0; i < out.nNodes; i++)
		{
			int			nodeN = out.nodeNumbers[i];
			SpGistSearchItem *innerItem;
			double	   *distances;

			Assert(nodeN >= 0 && nodeN < nNodes);

			node = nodes[nodeN];

			if (!ItemPointerIsValid(&node->t_tid))
				continue;

			/*
			 * Use infinity distances if innerConsistentFn() failed to return
			 * them or if is a NULL item (their distances are really unused).
			 */
			distances = out.distances ? out.distances[i] : so->infDistances;

			innerItem = spgMakeInnerItem(so, item, node, &out, i, isnull,
										 distances);

			spgAddSearchItemToQueue(so, innerItem);
		}
	}

	MemoryContextSwitchTo(oldCxt);
}

/* Returns a next item in an (ordered) scan or null if the index is exhausted */
static SpGistSearchItem *
spgGetNextQueueItem(SpGistScanOpaque so)
{
	if (pairingheap_is_empty(so->scanQueue))
		return NULL;			/* Done when both heaps are empty */

	/* Return item; caller is responsible to pfree it */
	return (SpGistSearchItem *) pairingheap_remove_first(so->scanQueue);
}

enum SpGistSpecialOffsetNumbers
{
	SpGistBreakOffsetNumber = InvalidOffsetNumber,
	SpGistRedirectOffsetNumber = MaxOffsetNumber + 1,
	SpGistErrorOffsetNumber = MaxOffsetNumber + 2,
};

static OffsetNumber
spgTestLeafTuple(IndexScanDesc scan,
				 SpGistSearchItem *item,
				 Page page, OffsetNumber offset,
				 bool isnull, bool isroot,
				 IndexScanBatch batch)
{
	SpGistLeafTuple leafTuple = (SpGistLeafTuple)
		PageGetItem(page, PageGetItemId(page, offset));

	if (leafTuple->tupstate != SPGIST_LIVE)
	{
		if (!isroot)			/* all tuples on root should be live */
		{
			if (leafTuple->tupstate == SPGIST_REDIRECT)
			{
				/* redirection tuple should be first in chain */
				Assert(offset == ItemPointerGetOffsetNumber(&item->heapPtr));
				/* transfer attention to redirect point */
				item->heapPtr = ((SpGistDeadTuple) leafTuple)->pointer;
				Assert(ItemPointerGetBlockNumber(&item->heapPtr) != SPGIST_METAPAGE_BLKNO);
				return SpGistRedirectOffsetNumber;
			}

			if (leafTuple->tupstate == SPGIST_DEAD)
			{
				/* dead tuple should be first in chain */
				Assert(offset == ItemPointerGetOffsetNumber(&item->heapPtr));
				/* No live entries on this page */
				Assert(SGLT_GET_NEXTOFFSET(leafTuple) == InvalidOffsetNumber);
				return SpGistBreakOffsetNumber;
			}
		}

		/* We should not arrive at a placeholder */
		elog(ERROR, "unexpected SPGiST tuple state: %d", leafTuple->tupstate);
		return SpGistErrorOffsetNumber;
	}

	Assert(ItemPointerIsValid(&leafTuple->heapPtr));

	spgLeafTest(scan, item, leafTuple, isnull, batch);

	return SGLT_GET_NEXTOFFSET(leafTuple);
}

/*
 * Walk the tree and return the next batch of matching tuples.
 *
 * Main driver of spggetbitmap and non-ordered spggetbatch scans.
 */
static IndexScanBatch
spgWalk(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	IndexScanBatch batch;
	Buffer		buffer = InvalidBuffer;

	batch = indexam_util_alloc_batch(scan);

	/* SP-GiST only ever scans forward; set the batch's direction up front */
	batch->dir = ForwardScanDirection;

	/* Walk until a leaf page yields matches, or the index is exhausted */
	while (batch->firstItem > batch->lastItem)
	{
		SpGistSearchItem *item = spgGetNextQueueItem(so);
		Page		page;

		if (item == NULL)
			break;				/* No more items in queue -> done */

		/* Heap items only occur in ordered scans (see spgWalkOrdered) */
		Assert(!item->isLeaf);

		/*
		 * Navigate to the item's live page, then process its contents.
		 *
		 * Note: spgReadItemPage calls CHECK_FOR_INTERRUPTS().
		 */
		buffer = spgReadItemPage(scan, item, buffer);
		page = BufferGetPage(buffer);

		if (SpGistPageIsLeaf(page))
			spgProcessLeafPage(scan, item, page, batch);
		else
			spgProcessInnerPage(scan, item, page);

		if (batch->firstItem <= batch->lastItem)
		{
			/* batch has matching items to return */
			SpGistBatchData *sbatch = SpGistBatchGetData(scan, batch);

			Assert(BufferIsValid(buffer));
			Assert(SpGistPageIsLeaf(BufferGetPage(buffer)));

			sbatch->buf = buffer;
			sbatch->blkno = BufferGetBlockNumber(buffer);

			if (scan->xs_want_itup)
			{
				/*
				 * Stash the shared reconstruction prefix for spggettransform,
				 * which runs after item is freed.  The prefix (item->value)
				 * is the same for every match in the batch.  It can be NULL
				 * when the opclass reconstructs entirely from the leaf datum
				 * (e.g. quad/kd-tree) or at the root level.
				 */
				sbatch->level = item->level;
				sbatch->isNull = item->isNull;

				if (so->state.attLeafType.attbyval || item->isNull ||
					DatumGetPointer(item->value) == NULL)
				{
					sbatch->reconValue = item->value;
				}
				else
				{
					/* pass-by-reference prefix: copy it into the recon area */
					Size		sz = datumGetSize(item->value, false,
												  so->state.attLeafType.attlen);
					char	   *dest = SpGistBatchGetReconArea(scan, batch);

					if (sz > SpGistBatchReconAreaSize)
						ereport(ERROR,
								(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								 errmsg("SP-GiST reconstructed value size %zu exceeds maximum %d",
										sz, SpGistBatchReconAreaSize)));

					memcpy(dest, DatumGetPointer(item->value), sz);
					sbatch->reconValue = PointerGetDatum(dest);
				}
			}
		}

		/* done with this scan item */
		spgFreeSearchItem(so, item);
		/* clear temp context before proceeding to the next one */
		MemoryContextReset(so->tempCxt);
	}

	if (batch->firstItem > batch->lastItem)
	{
		/* queue exhausted without finding any matches: end of scan */
		if (buffer != InvalidBuffer)
			UnlockReleaseBuffer(buffer);
		indexam_util_release_batch(scan, batch);
		return NULL;
	}

	indexam_util_unlock_batch(scan, batch, buffer);

	return batch;
}

/*
 * Convert an ordered heap item's flattened distances into the batch item's
 * IndexOrderByDistance array, honoring nonNullOrderByOffsets.
 */
static void
spgFillBatchItemDistances(SpGistScanOpaque so, SpGistBatchItem *bitem,
						  SpGistSearchItem *item)
{
	if (item->isNull || so->numberOfNonNullOrderBys <= 0)
	{
		for (int i = 0; i < so->numberOfOrderBys; i++)
		{
			bitem->distances[i].value = 0.0;
			bitem->distances[i].isnull = true;
		}
		return;
	}

	for (int i = 0; i < so->numberOfOrderBys; i++)
	{
		int			offset = so->nonNullOrderByOffsets[i];

		if (offset >= 0)
		{
			bitem->distances[i].value = item->distances[offset];
			bitem->distances[i].isnull = false;
		}
		else
		{
			bitem->distances[i].value = 0.0;
			bitem->distances[i].isnull = true;
		}
	}
}

/*
 * spgWalkOrdered() -- drain the distance queue into one virtual batch
 *
 * Pop items from so->scanQueue in (lower-bound) distance order: index pages are
 * scanned (pushing children and matching heap tuples back onto the queue), heap
 * tuples are appended to the batch, until the batch fills or the queue empties.
 * The result is a "virtual" batch spanning many leaf pages, holding no pin (and
 * never index-only, which the planner forbids for ordered scans).
 */
static IndexScanBatch
spgWalkOrdered(IndexScanDesc scan)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	IndexScanBatch batch = indexam_util_alloc_batch(scan);
	SpGistBatchData *sbatch;
	Buffer		buffer = InvalidBuffer;
	int			nitems = 0;

	/* SP-GiST only ever scans forward; set the batch's direction up front */
	batch->dir = ForwardScanDirection;

	for (;;)
	{
		SpGistSearchItem *item = spgGetNextQueueItem(so);

		if (item == NULL)
			break;				/* queue exhausted (end of scan) */

		if (item->isLeaf)
		{
			/* matching heap tuple: append to the batch in distance order */
			SpGistBatchItem *bitem = SpGistBatchGetItem(scan, batch, nitems);

			batch->items[nitems].tableTid = item->heapPtr;
			batch->items[nitems].indexOffset = InvalidOffsetNumber;
			batch->items[nitems].tupleOffset = 0;

			bitem->recheck = item->recheck;
			bitem->recheckDistances = item->recheckDistances;
			spgFillBatchItemDistances(so, bitem, item);

			spgFreeSearchItem(so, item);
			MemoryContextReset(so->tempCxt);

			if (++nitems == scan->maxitemsbatch)
				break;			/* batch full; remaining items stay queued */
		}
		else
		{
			Page		page;

			/*
			 * Index page: scan it, pushing children/heap items onto the
			 * queue.
			 *
			 * Note: spgReadItemPage calls CHECK_FOR_INTERRUPTS().
			 */
			buffer = spgReadItemPage(scan, item, buffer);
			page = BufferGetPage(buffer);

			if (SpGistPageIsLeaf(page))
			{
				/* leaf page: queue matching heap items (batch unused) */
				spgProcessLeafPage(scan, item, page, NULL);
			}
			else
				spgProcessInnerPage(scan, item, page);

			spgFreeSearchItem(so, item);
			MemoryContextReset(so->tempCxt);
		}
	}

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);

	if (nitems == 0)
	{
		/* no matching items remain: the scan is exhausted */
		indexam_util_release_batch(scan, batch);
		return NULL;
	}

	/* an ordered batch is "virtual" and holds no interlock pin */
	sbatch = SpGistBatchGetData(scan, batch);
	sbatch->buf = InvalidBuffer;
	sbatch->blkno = InvalidBlockNumber;

	batch->firstItem = 0;
	batch->lastItem = nitems - 1;

	Assert(!batch->isGuarded);

	return batch;
}

/*
 * Navigate to the live page that 'item' points at, following inner-tuple and
 * leaf-head REDIRECTs.
 *
 * 'buffer' is the lock the caller is already holding (or InvalidBuffer).  We
 * keep that lock while the item stays on the same block, releasing and
 * re-acquiring only when the block changes.
 *
 * The returned buffer is pinned and share-locked, holding either a live inner
 * tuple or a leaf page (whose chain head is not a redirect) at item->heapPtr.
 */
static Buffer
spgReadItemPage(IndexScanDesc scan, SpGistSearchItem *item, Buffer buffer)
{
	Relation	index = scan->indexRelation;

	Assert(!item->isLeaf);		/* heap items are handled by the caller */

	for (;;)
	{
		BlockNumber blkno = ItemPointerGetBlockNumber(&item->heapPtr);
		OffsetNumber offset = ItemPointerGetOffsetNumber(&item->heapPtr);
		Page		page;

		/* Release the page we hold if the item moved to a different block */
		if (buffer != InvalidBuffer && blkno != BufferGetBlockNumber(buffer))
		{
			UnlockReleaseBuffer(buffer);
			buffer = InvalidBuffer;
		}

		/* Acquire the page if we're not already holding it */
		if (buffer == InvalidBuffer)
		{
			CHECK_FOR_INTERRUPTS();
			buffer = ReadBuffer(index, blkno);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
		}

		page = BufferGetPage(buffer);

		if (SpGistPageIsLeaf(page))
		{
			ItemId		iid;
			SpGistLeafTuple head;

			/* When root is a leaf, all its tuples are live: no redirect */
			if (SpGistBlockIsRoot(blkno))
				return buffer;

			/*
			 * A leaf REDIRECT is always the head of its chain; follow it to
			 * the live tuples' page before the caller reports any match.  A
			 * live or dead head is left for spgProcessLeafPage to deal with.
			 */
			iid = PageGetItemId(page, offset);
			head = (SpGistLeafTuple) PageGetItem(page, iid);

			if (head->tupstate == SPGIST_REDIRECT)
			{
				item->heapPtr = ((SpGistDeadTuple) head)->pointer;
				Assert(ItemPointerGetBlockNumber(&item->heapPtr) !=
					   SPGIST_METAPAGE_BLKNO);
				continue;
			}

			return buffer;
		}
		else					/* page is inner */
		{
			ItemId		iid;
			SpGistInnerTuple innerTuple;

			iid = PageGetItemId(page, offset);
			innerTuple = (SpGistInnerTuple) PageGetItem(page, iid);

			if (innerTuple->tupstate != SPGIST_LIVE)
			{
				if (innerTuple->tupstate == SPGIST_REDIRECT)
				{
					/* transfer attention to redirect point */
					item->heapPtr = ((SpGistDeadTuple) innerTuple)->pointer;
					Assert(ItemPointerGetBlockNumber(&item->heapPtr) !=
						   SPGIST_METAPAGE_BLKNO);
					continue;
				}
				elog(ERROR, "unexpected SPGiST tuple state: %d",
					 innerTuple->tupstate);
			}

			return buffer;
		}
	}
}

/*
 * Descend a live inner tuple reached by spgReadItemPage: run inner_consistent
 * and push the matching child nodes onto the scan queue.
 *
 * When we're called, buffer containing 'page' is share-locked.  The tuple at
 * item->heapPtr must be live.
 */
static void
spgProcessInnerPage(IndexScanDesc scan, SpGistSearchItem *item, Page page)
{
	OffsetNumber offset = ItemPointerGetOffsetNumber(&item->heapPtr);
	ItemId		iid;
	SpGistInnerTuple innerTuple;

	Assert(!SpGistPageIsLeaf(page));

	iid = PageGetItemId(page, offset);
	innerTuple = (SpGistInnerTuple) PageGetItem(page, iid);
	Assert(innerTuple->tupstate == SPGIST_LIVE);

	spgInnerTest(scan, item, innerTuple, SpGistPageStoresNulls(page));
}

/*
 * Examine a leaf page reached by spgReadItemPage, acting on matching tuples:
 * a non-ordered scan appends them to batch; an ordered scan queues them, with
 * batch NULL.
 *
 * When we're called, buffer containing 'page' is share-locked.
 * spgReadItemPage must have already followed any leaf-head redirect, so the
 * chain examined here contains no redirect.
 */
static void
spgProcessLeafPage(IndexScanDesc scan, SpGistSearchItem *item, Page page,
				   IndexScanBatch batch)
{
	BlockNumber blkno = ItemPointerGetBlockNumber(&item->heapPtr);
	OffsetNumber offset = ItemPointerGetOffsetNumber(&item->heapPtr);
	bool		isnull = SpGistPageStoresNulls(page);
	OffsetNumber max = PageGetMaxOffsetNumber(page);

	Assert(SpGistPageIsLeaf(page));

	if (SpGistBlockIsRoot(blkno))
	{
		/* When root is a leaf, examine all its tuples */
		for (offset = FirstOffsetNumber; offset <= max; offset++)
			(void) spgTestLeafTuple(scan, item, page, offset,
									isnull, true, batch);
	}
	else
	{
		/* Normal case: just examine the chain we arrived at */
		while (offset != InvalidOffsetNumber)
		{
			Assert(offset >= FirstOffsetNumber && offset <= max);
			offset = spgTestLeafTuple(scan, item, page, offset,
									  isnull, false, batch);
			/* spgReadItemPage already resolved any leaf-head redirect */
			Assert(offset != SpGistRedirectOffsetNumber);
		}
	}
}

int64
spggetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	int64		ntids = 0;
	IndexScanBatch batch;

	/*
	 * Drive spgWalk one leaf page at a time, draining each batch into the
	 * bitmap and releasing it before fetching the next, so only one batch is
	 * ever live (cf. hashgetbitmap).
	 */
	while ((batch = spgWalk(scan)) != NULL)
	{
		bool	   *recheck = SpGistBatchGetRecheck(scan, batch);

		for (int i = batch->firstItem; i <= batch->lastItem; i++)
		{
			tbm_add_tuples(tbm, &batch->items[i].tableTid, 1, recheck[i]);
			ntids++;
		}

		/*
		 * Return the batch to the single-slot bitmap cache, to be reused by
		 * the next spgWalk
		 */
		indexam_util_release_batch(scan, batch);
	}

	return ntids;
}

IndexScanBatch
spggetbatch(IndexScanDesc scan, IndexScanBatch priorbatch, ScanDirection dir)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	IndexScanBatch batch;

	/*
	 * Note: Persistent traversal state lives in so->scanQueue, so we have no
	 * use for priorbatch here
	 */
	if (dir != ForwardScanDirection)
		elog(ERROR, "SP-GiST only supports forward scan direction");

	if (scan->numberOfOrderBys > 0)
		batch = spgWalkOrdered(scan);
	else
		batch = spgWalk(scan);

	/*
	 * When the scan queue was left empty, the scan already ended on the
	 * returned batch; mark the batch accordingly
	 */
	if (batch && pairingheap_is_empty(so->scanQueue))
		batch->knownEndForward = true;

	return batch;
}

/*
 * spgunguardbatch() -- Drop a batch's TID recycling interlock (buffer pin)
 */
void
spgunguardbatch(IndexScanDesc scan, IndexScanBatch batch)
{
	SpGistBatchData *sbatch = SpGistBatchGetData(scan, batch);

	/* Should be called exactly once iff !batchImmediateUnguard */
	Assert(!scan->batchImmediateUnguard);
	Assert(batch->isGuarded);

	ReleaseBuffer(sbatch->buf);
}

/*
 * spggettransform() -- Set up the scan's per-tuple output for one batch item
 *
 * Applies the item's recheck flag, and either reconstructs the index-only heap
 * tuple (xs_hitup) or reports the item's ORDER BY distances.
 */
void
spggettransform(IndexScanDesc scan, IndexScanBatch batch, int item)
{
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	Assert(item >= batch->firstItem && item <= batch->lastItem);

	/* Ordered (virtual) batch: recheck flag and distances live in the item */
	if (scan->numberOfOrderBys > 0)
	{
		SpGistBatchItem *bitem = SpGistBatchGetItem(scan, batch, item);

		Assert(!scan->xs_want_itup);
		Assert(SpGistBatchGetData(scan, batch)->blkno == InvalidBlockNumber);

		scan->xs_recheck = bitem->recheck;
		index_store_float8_orderby_distances(scan, so->orderByTypes,
											 bitem->distances,
											 bitem->recheckDistances);
		return;
	}

	/* Non-ordered batch: recheck flags live in a bool array */
	scan->xs_recheck = SpGistBatchGetRecheck(scan, batch)[item];

	if (scan->xs_want_itup)
	{
		/* Index-only scan */
		SpGistBatchData *sbatch = SpGistBatchGetData(scan, batch);
		SpGistLeafTuple leafTuple;
		Datum		leafDatums[INDEX_MAX_KEYS];
		bool		leafIsnulls[INDEX_MAX_KEYS];
		Datum		leafValue = (Datum) 0;
		MemoryContext oldcxt;

		Assert(scan->numberOfOrderBys == 0);
		Assert(sbatch->blkno != InvalidBlockNumber);

		/* Reconstruct the key value via leaf_consistent */
		leafTuple = (SpGistLeafTuple) (batch->currTuples +
									   batch->items[item].tupleOffset);
		if (!sbatch->isNull)
		{
			spgLeafConsistentIn in;
			spgLeafConsistentOut out;

			oldcxt = MemoryContextSwitchTo(so->tempCxt);

			in.scankeys = so->keyData;
			in.orderbys = NULL;
			in.nkeys = so->numberOfKeys;
			in.norderbys = 0;
			in.reconstructedValue = sbatch->reconValue;
			in.traversalValue = NULL;
			in.level = sbatch->level;
			in.returnData = true;
			in.leafDatum = SGLTDATUM(leafTuple, &so->state);

			out.leafValue = (Datum) 0;
			out.recheck = false;
			out.distances = NULL;
			out.recheckDistances = false;

			(void) FunctionCall2Coll(&so->leafConsistentFn, so->indexCollation,
									 PointerGetDatum(&in), PointerGetDatum(&out));
			leafValue = out.leafValue;

			MemoryContextSwitchTo(oldcxt);
		}

		/* free the previously returned reconstructed tuple, if any */
		if (scan->xs_hitup)
		{
			pfree(scan->xs_hitup);
			scan->xs_hitup = NULL;
		}

		/* build the returnable heap tuple in the scan-lifetime context */
		oldcxt = MemoryContextSwitchTo(so->reconCxt);

		/* Only deform the leaf tuple if it has INCLUDE attributes */
		if (so->state.leafTupDesc->natts > 1)
			spgDeformLeafTuple(leafTuple, so->state.leafTupDesc,
							   leafDatums, leafIsnulls, sbatch->isNull);

		leafDatums[spgKeyColumn] = leafValue;
		leafIsnulls[spgKeyColumn] = sbatch->isNull;

		scan->xs_hitup = heap_form_tuple(so->reconTupDesc,
										 leafDatums, leafIsnulls);

		MemoryContextSwitchTo(oldcxt);

		/* clean up after the leaf_consistent call */
		MemoryContextReset(so->tempCxt);
	}
}

bool
spgcanreturn(Relation index, int attno)
{
	SpGistCache *cache = spgGetCache(index);

	/*
	 * Forbid index-only scans for "long values" opclasses (e.g. text radix):
	 * the key is reconstructed from the prefix accumulated during the descent
	 * (see spggettransform) and is bounded only by the field-size limit, so
	 * it won't fit the fixed per-batch reconstruction workspace
	 * (spgbeginscan).
	 */
	if (cache->config.longValuesOK)
		return false;

	/*
	 * else INCLUDE attributes can always be fetched for index-only scans.
	 *
	 * Note: We deliberately give up on INCLUDE-only index-only scans too,
	 * even though an INCLUDE column comes straight from the bounded leaf
	 * tuple and needs no key reconstruction: spggettransform reconstructs the
	 * key unconditionally, and recheck of a key-column qual would need the
	 * key value regardless.
	 */
	if (attno > 1)
		return true;

	/* We can do it if the opclass config function says so */
	return cache->config.canReturnData;
}
