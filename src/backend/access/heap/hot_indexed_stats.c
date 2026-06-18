/*-------------------------------------------------------------------------
 *
 * hot_indexed_stats.c
 *	  SQL-callable diagnostic that walks every page of a heap relation and
 *	  reports hot-indexed-related structural statistics.
 *
 * These numbers complement the running pgstat counters
 * (n_tup_hot_indexed_upd in pg_stat_all_tables): they answer "what is on disk
 * right now?" rather than "how often did hot-indexed fire during the stats
 * window?".
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/hot_indexed_stats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/hot_indexed.h"
#include "access/htup_details.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"

/*
 * pg_relation_hot_indexed_stats(regclass) -> record
 *
 * Walks every block of the relation's main fork and counts:
 *   n_hot_indexed   is live HOT-indexed versions (HEAP_INDEXED_UPDATED, natts>0,
 *                      carrying an inline-trailing modified-attrs bitmap)
 *   n_chains        is LP_REDIRECT items, i.e. HOT chain roots.  Matches
 *                      the number of distinct HOT chains that have survived
 *                      the most recent prune.  Root-not-redirect chains
 *                      (length 1) are not counted here because they are
 *                      indistinguishable from a non-chain tuple.
 *   avg_chain_len   is mean length across chains rooted at an LP_REDIRECT,
 *                      derived by walking each redirect target to the end
 *                      of its HEAP_HOT_UPDATED chain.
 *   max_chain_len   is the longest chain observed.
 *
 * The caller must hold SELECT privilege on the relation, like other
 * relation-inspection functions; it takes only AccessShareLock and short-term
 * buffer share locks while scanning.
 */
Datum
pg_relation_hot_indexed_stats(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	AclResult	aclresult;
	BlockNumber nblocks;
	BlockNumber blk;
	int64		n_hot_indexed = 0;
	int64		n_chains = 0;
	int64		sum_chain_len = 0;
	int64		max_chain_len = 0;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};
	HeapTuple	resulttup;

	rel = relation_open(relid, AccessShareLock);
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, materialized view, or TOAST table",
						RelationGetRelationName(rel))));

	/* Caller must be able to read the relation. */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	nblocks = RelationGetNumberOfBlocks(rel);

	for (blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		OffsetNumber maxoff;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageIsEmpty(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			ItemId		lp = PageGetItemId(page, off);

			if (!ItemIdIsUsed(lp))
				continue;

			if (ItemIdIsRedirected(lp))
			{
				/* Walk the chain starting at the redirect target. */
				OffsetNumber cur = ItemIdGetRedirect(lp);
				int64		len = 0;

				/*
				 * Walk the same-page HOT chain.  Bound the loop by the page's
				 * item count so a corrupt cyclic t_ctid cannot spin forever
				 * under the buffer lock, and check for interrupts each step.
				 */
				while (cur >= FirstOffsetNumber && cur <= maxoff && len < maxoff)
				{
					ItemId		chain_lp = PageGetItemId(page, cur);
					HeapTupleHeader thdr;

					CHECK_FOR_INTERRUPTS();

					if (!ItemIdIsNormal(chain_lp))
						break;
					thdr = (HeapTupleHeader) PageGetItem(page, chain_lp);
					len++;
					if (!(thdr->t_infomask2 & HEAP_HOT_UPDATED))
						break;
					/* HOT chains stay on one page; stop if the link leaves it. */
					if (ItemPointerGetBlockNumber(&thdr->t_ctid) != blk)
						break;
					cur = ItemPointerGetOffsetNumber(&thdr->t_ctid);
				}
				if (len > 0)
				{
					n_chains++;
					sum_chain_len += len;
					if (len > max_chain_len)
						max_chain_len = len;
				}
			}
			else if (ItemIdIsNormal(lp))
			{
				HeapTupleHeader thdr = (HeapTupleHeader) PageGetItem(page, lp);

				if (!HotIndexedHeaderIsStub(thdr) &&
					(thdr->t_infomask2 & HEAP_INDEXED_UPDATED) != 0)
					n_hot_indexed++;
			}
		}

		UnlockReleaseBuffer(buf);
	}

	relation_close(rel, AccessShareLock);

	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "n_hot_indexed", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "n_chains", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "avg_chain_len", FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "max_chain_len", INT8OID, -1, 0);
	TupleDescFinalize(tupdesc);
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum(n_hot_indexed);
	values[1] = Int64GetDatum(n_chains);
	if (n_chains > 0)
		values[2] = Float8GetDatum(((double) sum_chain_len) / (double) n_chains);
	else
		values[2] = Float8GetDatum(0.0);
	values[3] = Int64GetDatum(max_chain_len);

	resulttup = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(resulttup));
}
