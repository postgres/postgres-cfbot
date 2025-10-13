/*
 * ginfuncs.c
 *		Functions to investigate the content of GIN indexes
 *
 * Copyright (c) 2014-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pageinspect/ginfuncs.c
 */
#include "postgres.h"

#include "access/gin_private.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"


PG_FUNCTION_INFO_V1(gin_metapage_info);
PG_FUNCTION_INFO_V1(gin_page_opaque_info);
PG_FUNCTION_INFO_V1(gin_entrypage_items);
PG_FUNCTION_INFO_V1(gin_leafpage_items);
PG_FUNCTION_INFO_V1(gin_datapage_items);

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_GIN(r) ((r)->rd_rel->relam == GIN_AM_OID)

Datum
gin_metapage_info(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	TupleDesc	tupdesc;
	Page		page;
	GinPageOpaque opaq;
	GinMetaPageData *metadata;
	HeapTuple	resultTuple;
	Datum		values[10];
	bool		nulls[10];

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	page = get_page_from_raw(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("input page is not a valid GIN metapage"),
				errdetail("Expected special size %d, got %d.",
						  (int) MAXALIGN(sizeof(GinPageOpaqueData)),
						  (int) PageGetSpecialSize(page)));

	opaq = GinPageGetOpaque(page);

	if (opaq->flags != GIN_META)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("input page is not a GIN metapage"),
				errdetail("Flags %04X, expected %04X",
						  opaq->flags, GIN_META));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	metadata = GinPageGetMeta(page);

	memset(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(metadata->head);
	values[1] = Int64GetDatum(metadata->tail);
	values[2] = Int32GetDatum(metadata->tailFreeSize);
	values[3] = Int64GetDatum(metadata->nPendingPages);
	values[4] = Int64GetDatum(metadata->nPendingHeapTuples);

	/* statistics, updated by VACUUM */
	values[5] = Int64GetDatum(metadata->nTotalPages);
	values[6] = Int64GetDatum(metadata->nEntryPages);
	values[7] = Int64GetDatum(metadata->nDataPages);
	values[8] = Int64GetDatum(metadata->nEntries);

	values[9] = Int32GetDatum(metadata->ginVersion);

	/* Build and return the result tuple. */
	resultTuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(resultTuple);
}


Datum
gin_page_opaque_info(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	TupleDesc	tupdesc;
	Page		page;
	GinPageOpaque opaq;
	HeapTuple	resultTuple;
	Datum		values[3];
	bool		nulls[3];
	Datum		flags[16];
	int			nflags = 0;
	uint16		flagbits;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	page = get_page_from_raw(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("input page is not a valid GIN data leaf page"),
				errdetail("Expected special size %d, got %d.",
						  (int) MAXALIGN(sizeof(GinPageOpaqueData)),
						  (int) PageGetSpecialSize(page)));

	opaq = GinPageGetOpaque(page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Convert the flags bitmask to an array of human-readable names */
	flagbits = opaq->flags;
	if (flagbits & GIN_DATA)
		flags[nflags++] = CStringGetTextDatum("data");
	if (flagbits & GIN_LEAF)
		flags[nflags++] = CStringGetTextDatum("leaf");
	if (flagbits & GIN_DELETED)
		flags[nflags++] = CStringGetTextDatum("deleted");
	if (flagbits & GIN_META)
		flags[nflags++] = CStringGetTextDatum("meta");
	if (flagbits & GIN_LIST)
		flags[nflags++] = CStringGetTextDatum("list");
	if (flagbits & GIN_LIST_FULLROW)
		flags[nflags++] = CStringGetTextDatum("list_fullrow");
	if (flagbits & GIN_INCOMPLETE_SPLIT)
		flags[nflags++] = CStringGetTextDatum("incomplete_split");
	if (flagbits & GIN_COMPRESSED)
		flags[nflags++] = CStringGetTextDatum("compressed");
	flagbits &= ~(GIN_DATA | GIN_LEAF | GIN_DELETED | GIN_META | GIN_LIST |
				  GIN_LIST_FULLROW | GIN_INCOMPLETE_SPLIT | GIN_COMPRESSED);
	if (flagbits)
	{
		/* any flags we don't recognize are printed in hex */
		flags[nflags++] = DirectFunctionCall1(to_hex32, Int32GetDatum(flagbits));
	}

	memset(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum(opaq->rightlink);
	values[1] = Int32GetDatum(opaq->maxoff);
	values[2] = PointerGetDatum(construct_array_builtin(flags, nflags, TEXTOID));

	/* Build and return the result tuple. */
	resultTuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(resultTuple);
}

typedef struct gin_leafpage_items_state
{
	TupleDesc	tupd;
	GinPostingList *seg;
	GinPostingList *lastseg;
} gin_leafpage_items_state;

/*
 * gin_entrypage_items
 *
 * Allows inspection of contents of an entry tree page.
 */
Datum
gin_entrypage_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Oid			indexRelid = PG_GETARG_OID(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	indexRel;
	OffsetNumber maxoff;
	TupleDesc	tupdesc;
	Page		page;
	GinPageOpaque opaq;
	StringInfoData buf;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	InitMaterializedSRF(fcinfo, 0);

	/* Open the index relation */
	indexRel = index_open(indexRelid, AccessShareLock);

	if (!IS_INDEX(indexRel) || !IS_GIN(indexRel))
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("\"%s\" is not a %s index",
					   RelationGetRelationName(indexRel), "GIN"));

	page = get_page_from_raw(raw_page);

	if (PageIsNew(page))
	{
		index_close(indexRel, AccessShareLock);
		PG_RETURN_NULL();
	}

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("input page is not a valid GIN entry tree page"),
				errdetail("Expected special size %d, got %d.",
						  (int) MAXALIGN(sizeof(GinPageOpaqueData)),
						  (int) PageGetSpecialSize(page)));

	opaq = GinPageGetOpaque(page);

	/* we only support entry tree in this function, check that */
	if (opaq->flags & GIN_META)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("gin_entrypage_items does not support metapages"));

	if (opaq->flags & (GIN_LIST | GIN_LIST_FULLROW))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("gin_entrypage_items does not support fast list pages"));

	if (opaq->flags & GIN_DATA)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("gin_entrypage_items does not support posting tree pages"),
				errhint("This appears to be a GIN posting tree page. Please use gin_datapage_items."));

	initStringInfo(&buf);
	maxoff = PageGetMaxOffsetNumber(page);

	tupdesc = RelationGetDescr(indexRel);

	for (OffsetNumber offset = FirstOffsetNumber;
		 offset <= maxoff;
		 offset = OffsetNumberNext(offset))
	{
		OffsetNumber indAtt;
		Datum		values[4];
		bool		nulls[4] = {0};
		Datum		attrVal;
		bool		isnull;
		IndexTuple	idxtuple;
		ItemId		iid = PageGetItemId(page, offset);

		if (!ItemIdIsValid(iid))
			ereport(ERROR, errcode(ERRCODE_INDEX_CORRUPTED), errmsg("invalid ItemId at offset %u", offset));

		idxtuple = (IndexTuple) PageGetItem(page, iid);

		values[0] = UInt16GetDatum(offset);

		if (tupdesc->natts == 1)
		{
			indAtt = FirstOffsetNumber;

			/* Here we can safely reuse any tuple descriptor. */
			attrVal = index_getattr(idxtuple, FirstOffsetNumber, tupdesc, &isnull);
		}
		else
		{
			TupleDesc	tmpTupdesc;
			Datum		res;
			Form_pg_attribute attr;

			/*
			 * Multi-column GIN indexes store 2-attribute tuple on each page
			 * item. First attribute is which heap attribute is stored as the
			 * second value in pair. To display value with proper output
			 * function we need to recreate tuple descriptor on each offset.
			 * NB: It is safe to reuse the original index tuple. See also
			 * gintuple_get_attrnum.
			 */

			res = index_getattr(idxtuple, FirstOffsetNumber, tupdesc, &isnull);

			/*
			 * we do not expect null for first attr in multi-column GIN
			 */
			if (isnull)
				ereport(ERROR,
						errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("invalid gin entry page tuple at offset %u", offset));

			indAtt = DatumGetUInt16(res);

			attr = TupleDescAttr(tupdesc, indAtt - 1);

			tmpTupdesc = CreateTemplateTupleDesc(2);

			TupleDescInitEntry(tmpTupdesc, (AttrNumber) 1, NULL,
							   INT2OID, -1, 0);
			TupleDescInitEntry(tmpTupdesc, (AttrNumber) 2, NULL,
							   attr->atttypid,
							   attr->atttypmod,
							   attr->attndims);
			TupleDescInitEntryCollation(tmpTupdesc, (AttrNumber) 2,
										attr->attcollation);

			TupleDescFinalize(tmpTupdesc);

			attrVal = index_getattr(idxtuple, OffsetNumberNext(FirstOffsetNumber),
									tmpTupdesc,
									&isnull);

			FreeTupleDesc(tmpTupdesc);
		}

		appendStringInfo(&buf, "%s=", quote_identifier(TupleDescAttr(tupdesc, indAtt - 1)->attname.data));

		if (!isnull)
		{
			Oid			foutoid;
			bool		typisvarlena;
			Oid			typoid;
			char	   *value;
			bool		nq;

			/*
			 * The following value output and quoting logic is copied from
			 * record_out().
			 */
			typoid = TupleDescAttr(tupdesc, indAtt - 1)->atttypid;
			getTypeOutputInfo(typoid, &foutoid, &typisvarlena);
			value = OidOutputFunctionCall(foutoid, attrVal);

			/* Check whether we need double quotes for this value */
			nq = (value[0] == '\0');	/* force quotes for empty string */
			for (const char *tmp = value; *tmp; tmp++)
			{
				char		ch = *tmp;

				if (ch == '"' || ch == '\\' ||
					ch == '(' || ch == ')' || ch == ',' ||
					isspace((unsigned char) ch))
				{
					nq = true;
					break;
				}
			}

			/* And emit the string */
			if (nq)
				appendStringInfoCharMacro(&buf, '"');
			for (const char *tmp = value; *tmp; tmp++)
			{
				char		ch = *tmp;

				if (ch == '"' || ch == '\\')
					appendStringInfoCharMacro(&buf, ch);
				appendStringInfoCharMacro(&buf, ch);
			}
			if (nq)
				appendStringInfoCharMacro(&buf, '"');
		}
		else
		{
			appendStringInfo(&buf, "NULL");
		}

		values[3] = CStringGetTextDatum(buf.data);
		resetStringInfo(&buf);

		if (GinIsPostingTree(idxtuple))
		{
			values[1] = ItemPointerGetDatum(&idxtuple->t_tid);
			nulls[2] = true;
		}
		else
		{
			int			ndecoded;
			Datum	   *tids_datum;
			ItemPointer items_orig;
			bool		free_items_orig;

			values[1] = ItemPointerGetDatum(&idxtuple->t_tid);
			/* Get list of item pointers from the tuple. */
			if (GinItupIsCompressed(idxtuple))
			{
				items_orig = ginPostingListDecode((GinPostingList *) GinGetPosting(idxtuple), &ndecoded);
				free_items_orig = true;
			}
			else
			{
				items_orig = (ItemPointer) GinGetPosting(idxtuple);
				ndecoded = GinGetNPosting(idxtuple);
				free_items_orig = false;
			}

			tids_datum = palloc_array(Datum, ndecoded);
			for (int i = 0; i < ndecoded; i++)
				tids_datum[i] = ItemPointerGetDatum(&items_orig[i]);
			values[2] = PointerGetDatum(construct_array_builtin(tids_datum, ndecoded, TIDOID));

			pfree(tids_datum);

			if (free_items_orig)
				pfree(items_orig);
		}

		/* Build and return the result tuple. */
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	index_close(indexRel, AccessShareLock);

	return (Datum) 0;
}

/*
 * gin_datapage_items
 *
 * Allows inspection of contents of an posting tree non-leaf page.
 */
Datum
gin_datapage_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	OffsetNumber maxoff;
	Page		page;
	GinPageOpaque opaq;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	InitMaterializedSRF(fcinfo, 0);
	page = get_page_from_raw(raw_page);

	if (PageIsNew(page))
	{
		PG_RETURN_NULL();
	}

	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("input page is not a valid GIN data page"),
				errdetail("Expected special size %d, got %d.",
						  (int) MAXALIGN(sizeof(GinPageOpaqueData)),
						  (int) PageGetSpecialSize(page)));

	opaq = GinPageGetOpaque(page);

	/*
	 * Reject non-posting-tree-internal GIN pages, which are the metapage, fast
	 * list pages, entry tree pages and posting tree leaf pages.
	 */
	if (opaq->flags & GIN_META)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("gin_datapage_items does not support metapages"));

	if (opaq->flags & (GIN_LIST | GIN_LIST_FULLROW))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("gin_datapage_items does not support fast list pages"));

	if (!(opaq->flags & GIN_DATA))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("input page is not a GIN data tree page"));

	if (opaq->flags & GIN_LEAF)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("gin_datapage_items does not support posting tree leaf pages"),
				errhint("This appears to be a GIN posting tree leaf page. Please use gin_leafpage_items."));

	maxoff = opaq->maxoff;

	for (OffsetNumber offset = FirstOffsetNumber;
		 offset <= maxoff;
		 offset = OffsetNumberNext(offset))
	{
		Datum		values[3];
		bool		nulls[3];
		PostingItem *item = GinDataPageGetPostingItem(page, offset);

		memset(nulls, 0, sizeof(nulls));

		values[0] = UInt16GetDatum(offset);

		values[1] = UInt32GetDatum(BlockIdGetBlockNumber(&item->child_blkno));
		values[2] = ItemPointerGetDatum(&item->key);

		/* Build and return the result tuple. */
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

Datum
gin_leafpage_items(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	FuncCallContext *fctx;
	gin_leafpage_items_state *inter_call_data;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to use raw page functions"));

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext mctx;
		Page		page;
		GinPageOpaque opaq;

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		page = get_page_from_raw(raw_page);

		if (PageIsNew(page))
		{
			MemoryContextSwitchTo(mctx);
			PG_RETURN_NULL();
		}

		if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("input page is not a valid GIN data leaf page"),
					errdetail("Expected special size %d, got %d.",
							  (int) MAXALIGN(sizeof(GinPageOpaqueData)),
							  (int) PageGetSpecialSize(page)));

		opaq = GinPageGetOpaque(page);
		if (opaq->flags != (GIN_DATA | GIN_LEAF | GIN_COMPRESSED))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("input page is not a compressed GIN data leaf page"),
					errdetail("Flags %04X, expected %04X",
							  opaq->flags,
							  (GIN_DATA | GIN_LEAF | GIN_COMPRESSED)));

		inter_call_data = palloc_object(gin_leafpage_items_state);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		inter_call_data->tupd = tupdesc;

		inter_call_data->seg = GinDataLeafPageGetPostingList(page);
		inter_call_data->lastseg = (GinPostingList *)
			(((char *) inter_call_data->seg) +
			 GinDataLeafPageGetPostingListSize(page));

		fctx->user_fctx = inter_call_data;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	inter_call_data = fctx->user_fctx;

	if (inter_call_data->seg != inter_call_data->lastseg)
	{
		GinPostingList *cur = inter_call_data->seg;
		HeapTuple	resultTuple;
		Datum		result;
		Datum		values[3];
		bool		nulls[3];
		int			ndecoded,
					i;
		ItemPointer tids;
		Datum	   *tids_datum;

		memset(nulls, 0, sizeof(nulls));

		values[0] = ItemPointerGetDatum(&cur->first);
		values[1] = Int16GetDatum(cur->nbytes);

		/* build an array of decoded item pointers */
		tids = ginPostingListDecode(cur, &ndecoded);
		tids_datum = palloc_array(Datum, ndecoded);
		for (i = 0; i < ndecoded; i++)
			tids_datum[i] = ItemPointerGetDatum(&tids[i]);
		values[2] = PointerGetDatum(construct_array_builtin(tids_datum, ndecoded, TIDOID));
		pfree(tids_datum);
		pfree(tids);

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(inter_call_data->tupd, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		inter_call_data->seg = GinNextPostingListSegment(cur);

		SRF_RETURN_NEXT(fctx, result);
	}

	SRF_RETURN_DONE(fctx);
}
