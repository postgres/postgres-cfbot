/*-------------------------------------------------------------------------
 *
 * vci_fetch.c
 *        Column fetch store
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_fetch.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdlib.h>

#include "access/heapam_xlog.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"	/* for TransactionIdIsInProgress() */
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "vci.h"
#include "vci_ros.h"
#include "vci_columns.h"
#include "vci_columns_data.h"

#include "vci_fetch.h"
#include "vci_ros.h"
#include "vci_utils.h"
#include "vci_wos.h"
#include "vci_tidcrid.h"
#include "vci_xact.h"

static void ChangeLockModeInQueryContext(vci_CSQueryContext queryContext, LOCKMODE new_lockmode);

static int
CompAttrNumber(const void *a, const void *b)
{
	AttrNumber	aA = *(const AttrNumber *) a;
	AttrNumber	aB = *(const AttrNumber *) b;

	return aA - aB;
}

static bool
NeedDatumPointer(vci_MainRelHeaderInfo *info, int16 columnId)
{
	TupleDesc	tupDesc;

	if (columnId < VCI_FIRST_NORMALCOLUMN_ID)
		return false;
	tupDesc = vci_GetTupleDescr(info);

	return vci_PassByRefForFixed(TupleDescAttr(tupDesc, columnId));
}

/**
 * @brief Create query context.
 *
 * @param[in] mainRelationOid Oid of VCI main relation.
 * @param[in] numReadColumns The number of read columns in the part of query.
 * @param[in] attrNum The attribute numbers in the original heap relation,
 * not those of the VCI main relation.
 * @param[in] sharedMemCtx The shared memory context to keep elements of
 * query context, fetch context, local ROS.
 * @param[in] lockmode lockmode set after local ROS is generated.
 * @return The pointer to the allocated vci_CSQueryContext.
 */
vci_CSQueryContext
vci_CSCreateQueryContextWLockMode(Oid mainRelationOid,
								  int numReadColumns,
								  AttrNumber *attrNum,
 /* attribute number in original relation */
								  MemoryContext sharedMemCtx,
								  LOCKMODE lockmode)
{
	TransactionId curRosVer;
	TransactionId lastRosVer;
	vci_CSQueryContext result;
	Relation	rel;

	result = MemoryContextAllocZero(sharedMemCtx, sizeof(vci_CSQueryContextData));
	result->shared_memory_context = sharedMemCtx;

	result->lockmode = lockmode;

	result->main_relation_oid = mainRelationOid;

	result->heap_rel = relation_open(IndexGetRelation(mainRelationOid, false),
									 AccessShareLock);

	result->num_columns = numReadColumns;
	result->attr_num = MemoryContextAllocZero(sharedMemCtx,
											  sizeof(AttrNumber) * numReadColumns);
	memcpy(result->attr_num, attrNum, sizeof(AttrNumber) * numReadColumns);
	qsort(result->attr_num, numReadColumns, sizeof(AttrNumber), CompAttrNumber);
	result->column_id = MemoryContextAllocZero(sharedMemCtx,
											   sizeof(int16) * numReadColumns);

	result->num_local_ros_extents = 0;
	result->local_ros = NULL;

	result->num_delete = 0;
	result->delete_list = NULL;

	result->info = MemoryContextAllocZero(sharedMemCtx,
										  sizeof(vci_MainRelHeaderInfo));
	rel = relation_open(mainRelationOid, result->lockmode);
	vci_InitMainRelHeaderInfo(result->info, rel, vci_rc_query);
	vci_KeepMainRelHeader(result->info);
	result->num_nullable_columns = vci_GetMainRelVar(result->info,
													 vcimrv_num_nullable_columns,
													 0);
	result->null_width_in_byte = vci_GetMainRelVar(result->info,
												   vcimrv_null_width_in_byte,
												   0);
	result->num_ros_extents = vci_GetMainRelVar(result->info,
												vcimrv_num_extents,
												0);

	curRosVer = vci_GetMainRelVar(result->info, vcimrv_current_ros_version, 0);
	lastRosVer = vci_GetMainRelVar(result->info, vcimrv_last_ros_version, 0);

	switch (vci_transaction_get_type(curRosVer))
	{
		case VCI_XACT_DID_COMMIT:
		case VCI_XACT_SELF:
			result->ros_version = curRosVer;
			result->inclusive_xid = curRosVer;
			result->exclusive_xid = InvalidTransactionId;
			result->tid_crid_diff_sel = vci_GetMainRelVar(result->info, vcimrv_tid_crid_diff_sel, 0);
			break;

		case VCI_XACT_IN_PROGRESS:
		case VCI_XACT_DID_CRASH:
		case VCI_XACT_DID_ABORT:
			result->ros_version = lastRosVer;
			result->inclusive_xid = InvalidTransactionId;
			result->exclusive_xid = curRosVer;
			result->tid_crid_diff_sel = vci_GetMainRelVar(result->info, vcimrv_tid_crid_diff_sel_old, 0);
			break;

		case VCI_XACT_INVALID:
			elog(ERROR, "current ROS version is invalid");	/* @todo */
			break;
	}

	Assert(TransactionIdIsValid(result->ros_version));

	{
		int32		indexNumColumns = vci_GetMainRelVar(result->info,
														vcimrv_num_columns, 0);
		AttrNumber *indexAttNums = palloc_array(AttrNumber, indexNumColumns);
		TupleDesc	descHeap = RelationGetDescr(result->heap_rel);

		for (int aId = 0; aId < indexNumColumns; ++aId)
		{
			vcis_m_column_t *mColumn = vci_GetMColumn(result->info, aId);
			LOCKMODE	lockmode_asl = AccessShareLock;
			Relation	mcol_rel = table_open(mColumn->meta_oid, lockmode_asl);
			Buffer		buffer;
			vcis_column_meta_t *metaHeader = vci_GetColumnMeta(&buffer, mcol_rel);

			indexAttNums[aId] = metaHeader->pgsql_attnum;
			ReleaseBuffer(buffer);
			table_close(mcol_rel, lockmode_asl);
		}

		for (int aId = 0; aId < numReadColumns; ++aId)
		{
			AttrNumber	attNum = TupleDescAttr(descHeap, result->attr_num[aId] - 1)
				->attnum;

			/* AttrNumber is 1 origin.  We use 0 origin value. */
			result->column_id[aId] = FindInt16(indexAttNums, indexNumColumns,
											   attNum);
			Assert(0 <= result->column_id[aId]);
		}

		pfree(indexAttNums);
	}

	result->num_data_wos_entries =
		vci_EstimateNumEntriesInHeapRelation(vci_GetMainRelVar(result->info, vcimrv_data_wos_oid, 0));

	result->num_whiteout_wos_entries =
		vci_EstimateNumEntriesInHeapRelation(vci_GetMainRelVar(result->info, vcimrv_whiteout_wos_oid, 0));

	return result;
}

/**
 * @brief Destroy query context.
 *
 * @param[in] queryContext Pointer to the target context to be destroy.
 */
void
vci_CSDestroyQueryContext(vci_CSQueryContext queryContext)
{
	vci_MainRelHeaderInfo *info;
	Relation	heapRel;
	Relation	indexRel;

	Assert(queryContext != NULL);
	Assert(queryContext->info != NULL);

	info = queryContext->info;

	heapRel = queryContext->heap_rel;
	indexRel = info->rel;

	vci_ReleaseMainRelHeader(info);

	if (RelationIsValid(indexRel))
		table_close(indexRel, queryContext->lockmode);

	if (RelationIsValid(heapRel))
		table_close(heapRel, AccessShareLock);

	if (queryContext->column_id)
		pfree(queryContext->column_id);

	if (queryContext->attr_num)
		pfree(queryContext->attr_num);

	pfree(info);
	pfree(queryContext);
}

static void
ChangeLockModeInQueryContext(vci_CSQueryContext queryContext, LOCKMODE new_lockmode)
{
	Assert(queryContext);

	if (queryContext->lockmode != new_lockmode)
	{
		Assert(queryContext->info);
		Assert(queryContext->info->rel);
		LockRelation(queryContext->info->rel, new_lockmode);
		UnlockRelation(queryContext->info->rel, queryContext->lockmode);
		queryContext->lockmode = new_lockmode;
	}
}

/**
 * @brief Collect information of the specified column and the maximum size of
 * tuples.
 *
 * @param info[in] Pointer to the VCI master information.
 * @param fetchContext[in] Pointer to the fetch context.
 * @param columnId[in] column ID in VCI main relation.
 * @param datumSize[in,out] If not NULL, the datum size is written.
 * @param maxElemSize[in,out] If not NULL, the maximum size of an element is written.
 * @param maxDictSize[in,out] If not NULL, the maximum size of dictionary is written.
 * @param nullBitId[in,out] If not NULL, the null bit Id is written.
 * @param compType[in,out] If not NULL, the compression type is written.
 * @param atttypid[in,out] If not NULL, the atttypid in PostgreSQL is written.
 * @param strictDatumType[in,out] If not NULL, the flag is returned, indicating
 * true as Datum has only the pointer to the real value, or false that
 * Datum has the value itself.
 * @return the maximum size of elements, the same value in maxElemSize.
 */
static int
SizeOfElementAndPointer(vci_MainRelHeaderInfo *info,
						vci_CSFetchContext fetchContext,
						int16 columnId, /* the column ID in VCI main relation */
						uint32 *datumSize,
						uint32 *maxElemSize,
						uint32 *maxDictSize,
						int32 *nullBitId,
						vcis_compression_type_t *compType,
						Oid *atttypid,
						bool *strictDatumType)
{
	int			maxElementSize;

	if (nullBitId)
		*nullBitId = -1;		/* default is not nullable */

	if (compType)
		*compType = vcis_compression_type_fixed_raw;

	if (atttypid)
		*atttypid = InvalidOid;

	if (strictDatumType)
		*strictDatumType = false;

	if (maxDictSize)
		*maxDictSize = 0;

	if (VCI_FIRST_NORMALCOLUMN_ID <= columnId)
	{
		TupleDesc	desc = vci_GetTupleDescr(info);
		vcis_m_column_t *mColumn = vci_GetMColumn(info, columnId);

		Assert((VCI_FIRST_NORMALCOLUMN_ID <= columnId) && (columnId < desc->natts));

		maxElementSize = mColumn->max_columns_size;
		if (maxElemSize)
			*maxElemSize = maxElementSize;

		if (compType)
			*compType = mColumn->comp_type;

		if (nullBitId)
			*nullBitId = vci_GetBitIdInNullBits(desc, columnId);

		if (atttypid)
			*atttypid = TupleDescAttr(desc, columnId)->atttypid;

		switch (mColumn->comp_type)
		{
			case vcis_compression_type_fixed_raw:
				{
					if (vci_PassByRefForFixed(TupleDescAttr(desc, columnId)))
					{
						if (strictDatumType)
							*strictDatumType = true;
						if (datumSize)
							*datumSize = sizeof(Datum);
					}
					else
					{
						if (strictDatumType)
							*strictDatumType = false;
						if (datumSize)
							*datumSize = maxElementSize;
					}
				}
				break;
			case vcis_compression_type_variable_raw:
				if (strictDatumType)
					*strictDatumType = true;
				if (datumSize)
					*datumSize = sizeof(Datum);
				break;
				/* for compressions */
			default:
				ereport(ERROR, (errmsg("internal error: unsupported compression type"), errhint("Disable VCI by 'SELECT vci_disable();'")));
		}

		/*
		 * we put large data in some area, and Datum have the pointer
		 */
		if (NeedDatumPointer(info, columnId))
			maxElementSize = MAXALIGN(maxElementSize) + sizeof(Datum);

		return maxElementSize;
	}

	switch (columnId)
	{
		case VCI_COLUMN_ID_TID:
			maxElementSize = sizeof(int64);
			break;

		case VCI_COLUMN_ID_NULL:
			maxElementSize = sizeof(bool) * fetchContext->num_columns;
			break;

		case VCI_COLUMN_ID_DELETE:
			maxElementSize = sizeof(uint16);
			break;

		case VCI_COLUMN_ID_CRID:
			maxElementSize = sizeof(uint64);
			break;

		default:
			abort();
	}

	if (datumSize)
		*datumSize = maxElementSize;

	if (maxElemSize)
		*maxElemSize = maxElementSize;

	return maxElementSize;
}

static vci_MainRelHeaderInfo *
GetMainRelHeaderInfoFromFetchContext(vci_CSFetchContext fetchContext)
{
	return (fetchContext->info) ? fetchContext->info
		: fetchContext->query_context->info;
}

/**
 * @brief Obtain tuple size where each attribute is aligned by MAXALIGN.
 */
static void
GetWorstCaseTupleSize(vci_CSFetchContext fetchContext,
					  Size *sumWorstCaseDictionarySize_,
					  Size *sumWorstCaseValueSize_,
					  Size *sumWorstCaseFlagSize_,
					  Size *sumWorstCaseAreaSize_,
					  int16 numReadColumns,
 /* attribute number in original relation */
					  AttrNumber *attrNum,
					  bool returnTid,
					  bool returnCrid)
{
	vci_CSQueryContext queryContext = fetchContext->query_context;
	vci_MainRelHeaderInfo *info = GetMainRelHeaderInfoFromFetchContext(fetchContext);
	uint32		datumSize;
	uint32		maxElemSize;
	uint32		maxDictSize;
	bool		strictDatumType;
	Size		sumWorstCaseDictionarySize = 0;
	Size		sumWorstCaseValueSize = 0;
	Size		sumWorstCaseFlagSize = 0;
	Size		sumWorstCaseAreaSize = 0;

	for (int aId = 0; aId < numReadColumns; ++aId)
	{
		int16		colId = fetchContext->column_link[aId];

		SizeOfElementAndPointer(info, fetchContext,
								queryContext->column_id[colId],
								&datumSize, &maxElemSize, &maxDictSize,
								NULL, NULL, NULL, &strictDatumType);
		sumWorstCaseValueSize += TYPEALIGN(sizeof(Datum), datumSize);
		sumWorstCaseDictionarySize += MAXALIGN(maxDictSize);
		if (strictDatumType)
			sumWorstCaseAreaSize += MAXALIGN(maxElemSize);
	}

	SizeOfElementAndPointer(info, fetchContext, VCI_COLUMN_ID_NULL,
							&datumSize, &maxElemSize, &maxDictSize,
							NULL, NULL, NULL, &strictDatumType);
	sumWorstCaseFlagSize += MAXALIGN(maxElemSize);
	sumWorstCaseDictionarySize += MAXALIGN(maxDictSize);

	SizeOfElementAndPointer(info, fetchContext, VCI_COLUMN_ID_DELETE,
							&datumSize, &maxElemSize, &maxDictSize,
							NULL, NULL, NULL, &strictDatumType);
	sumWorstCaseFlagSize += MAXALIGN(maxElemSize);
	sumWorstCaseDictionarySize += MAXALIGN(maxDictSize);

	if (fetchContext->need_tid)
	{
		SizeOfElementAndPointer(info, fetchContext, VCI_COLUMN_ID_TID,
								&datumSize, &maxElemSize, &maxDictSize,
								NULL, NULL, NULL, &strictDatumType);
		sumWorstCaseFlagSize += MAXALIGN(maxElemSize);
		sumWorstCaseDictionarySize += MAXALIGN(maxDictSize);
	}

	if (fetchContext->need_crid)
	{
		SizeOfElementAndPointer(info, fetchContext, VCI_COLUMN_ID_CRID,
								&datumSize, &maxElemSize, &maxDictSize,
								NULL, NULL, NULL, &strictDatumType);
		sumWorstCaseFlagSize += MAXALIGN(maxElemSize);
		sumWorstCaseDictionarySize += MAXALIGN(maxDictSize);
	}

	sumWorstCaseFlagSize += sizeof((((vci_virtual_tuples_t *) NULL)->skip)[0]) +
		sizeof((((vci_virtual_tuples_t *) NULL)->local_skip)[0]);

	if (sumWorstCaseDictionarySize_)
		*sumWorstCaseDictionarySize_ = sumWorstCaseDictionarySize;
	if (sumWorstCaseValueSize_)
		*sumWorstCaseValueSize_ = sumWorstCaseValueSize;
	if (sumWorstCaseFlagSize_)
		*sumWorstCaseFlagSize_ = sumWorstCaseFlagSize;
	if (sumWorstCaseAreaSize_)
		*sumWorstCaseAreaSize_ = sumWorstCaseAreaSize;
}

/**
 * @brief The base function of creating an instance of \c vci_CSFetchContext.
 *
 * This function is normally called via vci_CSCreateFetchContext().
 *
 * @param[in] queryContext The query context.
 * @param[in] numRowsReadAtOnce The number of rows which read at once and
 * stored in the virtual tuples.
 * @param[in] numReadColumns The number of columns to be read.
 * @param[in] attrNum The pointer to the array which has the attribute numbers
 * of the original heap relation, not VCI main relation.
 * @param[in] useColumnStore True for column-wise store.  False for row-wise.
 * @param[in] returnTid True to get TID in virtual tuples.
 * @param[in] returnCrid True to get CRID in virtual tuples.
 * @param[in] useCompression True to use compression.
 * @return The pointer to the created fetch context.
 * NULL if some parameters are invald resulting no fetch context is created.
 * @note This function registers CurrentMemoryContext as local_memory_context
 * in \c vci_CSFetchContext.
 */
vci_CSFetchContext
vci_CSCreateFetchContextBase(vci_CSQueryContext queryContext,
							 uint32 numRowsReadAtOnce,
							 int16 numReadColumns,
 /* attribute number in original relation */
							 AttrNumber *attrNum,
							 bool useColumnStore,
							 bool returnTid,
							 bool returnCrid,
							 bool useCompression)
{
	Size		size = sizeof(vci_CSFetchContextData) +
		((numReadColumns - 1) * sizeof(int16));
	vci_CSFetchContext result;

	Assert(useCompression == false);	/* Compression code has been extracted
										 * from the contrib/vci module */

	result = MemoryContextAllocZero(queryContext->shared_memory_context, size);

	result->query_context = queryContext;
	result->size = size;

	/*
	 * The master copy does not have own vci_MainRelHeaderInfo. The localized
	 * copies have their own vci_MainRelHeaderInfo, which will be created in
	 * vci_CSLocalizeFetchContext().
	 */
	result->info = NULL;

	result->num_columns = numReadColumns;
	result->num_rows_read_at_once = TYPEALIGN(VCI_COMPACTION_UNIT_ROW,
											  numRowsReadAtOnce);
	result->use_column_store = useColumnStore;
	result->need_crid = returnCrid;
	result->need_tid = returnTid;
	result->buffer = MemoryContextAllocZero(queryContext->shared_memory_context,
											sizeof(vci_seq_scan_buffer_t));
	result->local_memory_context = CurrentMemoryContext;

	result->extent_id = VCI_INVALID_EXTENT_ID;
	result->num_rows = 0;

	{
		Size		sumWorstCaseAreaSize;
		Size		valueSizePerTuple;
		Size		flagSizePerTuple;
		Size		flagSizeBaseline;

		for (int aId = 0; aId < numReadColumns; ++aId)
		{
			int16		colId;

			Assert(0 < attrNum[aId]);
			colId = FindInt16(queryContext->attr_num,
							  queryContext->num_columns,
							  attrNum[aId]);
			Assert(VCI_FIRST_NORMALCOLUMN_ID <= colId);
			result->column_link[aId] = colId;
		}

		/* Should we use faster way? */
		GetWorstCaseTupleSize(result,
							  &(result->size_dictionary_area),
							  &valueSizePerTuple,
							  &flagSizePerTuple,
							  &sumWorstCaseAreaSize,
							  numReadColumns,
							  attrNum,
							  returnTid,
							  returnCrid);

		result->size_dictionary_area = useCompression
			? result->size_dictionary_area : 0;
		result->size_decompression_area = (result->size_dictionary_area)
			? MAXALIGN(VCI_MAX_PAGE_SPACE * VCI_COMPACTION_UNIT_ROW) : 0;

		if (!(result->use_column_store))
			flagSizePerTuple += result->num_columns * (sizeof(Datum) + sizeof(bool));

		flagSizeBaseline = sizeof((((vci_virtual_tuples_t *) NULL)->skip)[0]) +
			sizeof((((vci_virtual_tuples_t *) NULL)->local_skip)[0]) +
			result->size_dictionary_area +
			result->size_decompression_area;

recalculation:
		/* The skip information has additional one elements at the tail. */
		result->size_flags = flagSizeBaseline + flagSizePerTuple * result->num_rows_read_at_once;
		result->size_values = valueSizePerTuple * result->num_rows_read_at_once;

		/* add padding space for each MemoryContextAlloc() */
		result->size_vector_memory_context = result->size_values +
			result->size_flags + sumWorstCaseAreaSize +
			((2 * result->num_columns) * MAXIMUM_ALIGNOF);

		if (MaxAllocSize < result->size_vector_memory_context)
		{
			uint32		new_num_rows_read_at_once;

			new_num_rows_read_at_once =
				(MaxAllocSize - (flagSizeBaseline + sumWorstCaseAreaSize + 2 * result->num_columns * MAXIMUM_ALIGNOF))
				/ (flagSizePerTuple + valueSizePerTuple);

			if (new_num_rows_read_at_once > VCI_COMPACTION_UNIT_ROW)
				new_num_rows_read_at_once = TYPEALIGN(VCI_COMPACTION_UNIT_ROW,
													  new_num_rows_read_at_once - VCI_COMPACTION_UNIT_ROW + 1);

			if (new_num_rows_read_at_once == 0)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));

			result->num_rows_read_at_once = new_num_rows_read_at_once;

			goto recalculation;
		}
	}

	/*
	 * The following fields are filled by vci_CSLocalizeFetchContext()
	 */
	result->rel_column = NULL;
	vci_Initvci_ColumnRelations(&(result->rel_delete));
	vci_Initvci_ColumnRelations(&(result->rel_null));
	vci_Initvci_ColumnRelations(&(result->rel_tid));

	return result;
}

/**
 * @brief Destroy the given instance of \c vci_CSFetchContext.
 *
 * The opened relations registerd in the target are all closed and
 * related \c vci_ReleaseMainRelHeader is released.
 *
 * @param[in] fetchContext target to destroy.
 */
void
vci_CSDestroyFetchContext(vci_CSFetchContext fetchContext)
{
	LOCKMODE	lockmode = AccessShareLock;

	vci_CloseColumnRelations(&(fetchContext->rel_delete), lockmode);
	vci_CloseColumnRelations(&(fetchContext->rel_null), lockmode);
	vci_CloseColumnRelations(&(fetchContext->rel_tid), lockmode);
	if (fetchContext->rel_column)
	{
		for (int cId = 0; cId < fetchContext->num_columns; ++cId)
			vci_CloseColumnRelations(&(fetchContext->rel_column[cId]), lockmode);
		pfree(fetchContext->rel_column);
	}

	if (fetchContext->info)
	{
		Relation	rel = fetchContext->info->rel;

		vci_ReleaseMainRelHeader(fetchContext->info);

		table_close(rel, lockmode);
	}

	pfree(fetchContext->buffer);
	pfree(fetchContext);
}

/* call this function using local memory context */
/**
 * @brief Make a local copy of \c fetchContext.
 *
 * Because file handles are unable to be shared among processes,
 * and \c fetchContext has many file handles, we need a copy of
 * \c fetchContext made by each process.
 *
 * @param[in] fetchContext source of copy.
 * @param[in] memoryContext the memory context where the copy is written.
 * @return copy of given \c fetchContext.
 */
vci_CSFetchContext
vci_CSLocalizeFetchContext(vci_CSFetchContext fetchContext,
						   MemoryContext memoryContext)
{
	LOCKMODE	lockmode = AccessShareLock;
	MemoryContext oldMemCtx = MemoryContextSwitchTo(memoryContext);
	Relation	rel = relation_open(fetchContext->query_context->main_relation_oid,
									AccessShareLock);
	vci_CSFetchContext result = palloc0(fetchContext->size);

	memcpy(result, fetchContext, fetchContext->size);
	result->local_memory_context = memoryContext;
	result->buffer = MemoryContextAllocZero(result->local_memory_context,
											sizeof(vci_seq_scan_buffer_t));

	Assert(NULL == result->info);
	result->info = MemoryContextAllocZero(result->local_memory_context,
										  sizeof(vci_MainRelHeaderInfo));
	vci_InitMainRelHeaderInfo(result->info, rel,
							  fetchContext->query_context->info->command);
	vci_KeepMainRelHeader(result->info);

	result->rel_column = MemoryContextAllocZero(result->local_memory_context,
												sizeof(vci_ColumnRelations) * result->num_columns);

	vci_OpenColumnRelations(&(result->rel_delete), result->info, VCI_COLUMN_ID_DELETE, lockmode);
	vci_OpenColumnRelations(&(result->rel_null), result->info, VCI_COLUMN_ID_NULL, lockmode);
	vci_OpenColumnRelations(&(result->rel_tid), result->info, VCI_COLUMN_ID_TID, lockmode);

	for (int columnId = VCI_FIRST_NORMALCOLUMN_ID; columnId < result->num_columns; ++columnId)
	{
		int16		cId = vci_GetColumnIdFromFetchContext(fetchContext, columnId);

		vci_OpenColumnRelations(&(result->rel_column[columnId]),
								result->info, cId, lockmode);
	}

	MemoryContextSwitchTo(oldMemCtx);

	return result;
}

/**
 * @brief Create an instance of \c vci_extent_status_t where the status
 * of extents are written.
 *
 * @param[in] fetchContext the \c fetchContext of the target VCI relation
 * in the process, i.e. localized one.
 * @return the pointer of newly created instance of \c vci_extent_status_t.
 */
vci_extent_status_t *
vci_CSCreateCheckExtent(vci_CSFetchContext fetchContext)
{
	uint32		size = sizeof(vci_extent_status_t) +
		((fetchContext->num_columns - 1) * sizeof(vci_minmax_t));
	vci_extent_status_t *result;

	Assert(fetchContext->local_memory_context);
	result = MemoryContextAllocZero(fetchContext->local_memory_context, size);
	result->size = size;
	result->num_rows = 0;
	result->existence = false;
	result->visible = false;

	return result;
}

/**
 * @brief Destroy given instance of \c vci_extent_status_t.
 *
 * @param[in] fetchContext target to destory.
 */
void
vci_CSDestroyCheckExtent(vci_extent_status_t *status)
{
	pfree(status);
}

static void
SetMinMaxInvalid(vci_extent_status_t *status, vci_CSFetchContext fetchContext)
{
	for (int aId = 0; aId < fetchContext->num_columns; ++aId)
		status->minmax[aId].valid = false;
}

/**
 * @brief Get the status of an extent.
 *
 * @details Check if the extent is visible with the relation among current ROS
 * version and \c Xgen, \c Xdel.
 *
 * The current ROS version is obtained from
 * \c fetchContext->query_context->ros_version.
 *
 * @param[in,out] status the status of an extent is written in \c * \c status.
 * @param[in] fetchContext the \c fetchContext of the target VCI relation
 * in the process, i.e. localized one.
 * @param[in] extentId the extent ID to probe.
 * @param[in] readMinMax obtain min-max information if true.
 */
void
vci_CSCheckExtent(vci_extent_status_t *status,
				  vci_CSFetchContext fetchContext,
				  int32 extentId,
				  bool readMinMax)
{
	vci_MainRelHeaderInfo *info = GetMainRelHeaderInfoFromFetchContext(
																	   fetchContext);

	if (extentId < VCI_FIRST_NORMAL_EXTENT_ID)
	{
		bool		existence;
		vci_local_ros_t *localRos;
		vci_virtual_tuples_t *extent;

		Assert(fetchContext->query_context);
		existence = -(fetchContext->query_context->num_local_ros_extents) <= extentId;
		status->existence = existence;
		status->visible = existence;

		localRos = fetchContext->query_context->local_ros;
		Assert(localRos->num_local_extents == fetchContext->query_context->num_local_ros_extents);
		extent = localRos->extent[-1 - extentId];
		status->num_rows = extent->num_rows;

		SetMinMaxInvalid(status, fetchContext);

		return;
	}

	/* check if the VCI index has the extent. */
	if (!vci_ExtentInfoExists(info, extentId))
	{
		status->num_rows = 0;
		status->existence = false;
		status->visible = false;
		SetMinMaxInvalid(status, fetchContext);

		return;
	}

	{
		/* check information of extent in main relation */
		Buffer		buffer = InvalidBuffer;
		vcis_m_extent_t *mExtent;

		mExtent = vci_GetMExtent(&buffer, info, extentId);

		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		status->existence = TransactionIdIsValid(mExtent->xgen) ||
			TransactionIdIsValid(mExtent->xdel);

		status->visible = vci_ExtentIsVisible(mExtent,
											  fetchContext->query_context->ros_version);

		status->num_rows = mExtent->num_rows;

		UnlockReleaseBuffer(buffer);
	}

	if (!status->visible)
	{
		SetMinMaxInvalid(status, fetchContext);

		return;
	}
}

/**
 * @brief Entry point to generate local ROS.
 *
 * @param[in] queryContext Query context the local ROS is generated for.
 * @return The pointer to the local ROS information.
 */
vci_local_ros_t *
vci_CSGenerateLocalRos(vci_CSQueryContextData *queryContext)
{
	int64		numDataWosRows;
	int64		numWhiteoutWosRows;
	vci_local_ros_t *result;

	numDataWosRows = queryContext->num_data_wos_entries;
	numWhiteoutWosRows = queryContext->num_whiteout_wos_entries;

	numDataWosRows = Max(numDataWosRows, 1);
	numWhiteoutWosRows = Max(numWhiteoutWosRows, 1);

	result = vci_GenerateLocalRos(queryContext,
								  VciGuc.max_local_ros_size * INT64CONST(1024),
								  numDataWosRows,
								  numWhiteoutWosRows);

	ChangeLockModeInQueryContext(queryContext, AccessShareLock);

	return result;
}

/**
 * @brief Estimate the size of local ROS.
 *
 * @details We have some ways to estimate the number of rows in data WOS.
 * In the system catalog, we have two values.
 * One is pg_class.reltuples, which is updated on VACUUM.
 * The other is pg_stat_user_tables.n_live_tup, which is
 * updated on COMMIT.
 * Both cannot count those rows INSERTed in the same
 * transaction.
 * To obtain the actual count, it seems necessary to count
 * rows in data WOS.
 * And we have the count itself in memory object,
 * vci_memory_entry_t.tid_tree->number_of_nodes_in_wos.
 *
 * For local delete list, we estimate the number of entries, i.e. deleted
 * rows, from the number of DB pages in the whiteout WOS.
 *
 * We also use memory for chunk storage in building Local ROS,
 * but which can be counted in WOS -> ROS conversion memory.
 *
 * @param[in] queryContext estimate the size of local ROS for the given
 * query context.
 * @return estimated size of local ROS.
 */
Size
vci_CSEstimateLocalRosSize(vci_CSQueryContext queryContext)
{
	Size		result = 0;
	int			numRowsInExtent;
	int64		numDataWosRows;
	int64		numWhiteoutWosRows;
	int64		numLocalDeleteListRows;
	int			numRowsAtOneFetch;
	Size		oneFetchMemorySize;

	Assert(queryContext);
	Assert(queryContext->info);

	numRowsInExtent = vci_GetNumRowsInLocalRosExtent(queryContext->num_columns);

	numDataWosRows = queryContext->num_data_wos_entries =
		vci_EstimateNumEntriesInHeapRelation(
											 vci_GetMainRelVar(queryContext->info, vcimrv_data_wos_oid, 0));

	numWhiteoutWosRows = queryContext->num_whiteout_wos_entries =
		vci_EstimateNumEntriesInHeapRelation(
											 vci_GetMainRelVar(queryContext->info, vcimrv_whiteout_wos_oid, 0));

	numLocalDeleteListRows = numDataWosRows + numWhiteoutWosRows;

	if (VCI_NUM_ROWS_IN_EXTENT * VCI_MAX_NUMBER_UNCONVERTED_ROS < numDataWosRows)
		return (Size) -1;

	/*
	 * Calculate the size of memory area to store multiple
	 * vci_virtual_tuples_t. We assume to use column store for local ROS.
	 */
	{
		vci_CSFetchContext fetchContext;

		/*
		 * We are estimating the data size of local ROS, for which we do not
		 * use data compression.
		 */
		fetchContext = vci_CSCreateFetchContextBase(queryContext,
													Min(numRowsInExtent, numDataWosRows),
													queryContext->num_columns,
													queryContext->attr_num,
													true,	/* useColumnStore */
													true,
													true,
													false); /* no compression */

		numRowsAtOneFetch = fetchContext->num_rows_read_at_once;

		oneFetchMemorySize = fetchContext->size_vector_memory_context;

		vci_CSDestroyFetchContext(fetchContext);
	}

	if (numDataWosRows <= numRowsAtOneFetch)
	{
		result = oneFetchMemorySize;
	}
	else
	{
		int			numFullFetches;
		int			numRemainedRows;

		numFullFetches = numDataWosRows / numRowsAtOneFetch;
		numRemainedRows = numDataWosRows - (numFullFetches * numRowsAtOneFetch);

		result = numFullFetches * oneFetchMemorySize;

		if (0 < numRemainedRows)
		{
			vci_CSFetchContext fetchContext;

			fetchContext = vci_CSCreateFetchContextBase(queryContext,
														numRemainedRows,
														queryContext->num_columns,
														queryContext->attr_num,
														true,	/* useColumnStore */
														true,
														true,
														false); /* no compression */

			result += fetchContext->size_vector_memory_context;

			vci_CSDestroyFetchContext(fetchContext);
		}
	}

	/*
	 * Calculate the size of local delete list
	 */
	result += numLocalDeleteListRows * sizeof(queryContext->delete_list[0]);

	return result;
}

static void
RefillPointersOfVirtualTuples(vci_virtual_tuples_t *vTuples, bool keepStatus)
{
	vci_CSFetchContext fetchContext = vTuples->fetch_context;
	uint32		numRows = vTuples->num_rows_read_at_once;
	vci_MainRelHeaderInfo *info;
	char	   *ptr;

	info = GetMainRelHeaderInfoFromFetchContext(fetchContext);

	vTuples->buffer_capacity = numRows;

	vTuples->values = (Datum *) MAXALIGN(vTuples->al_values);
	vTuples->flags = (char *) MAXALIGN(vTuples->al_flags);

	ptr = vTuples->flags;

	vTuples->crid = NULL;
	if (fetchContext->need_crid)
	{
		vTuples->crid = (int64 *) ptr;
		ptr = (char *) &(vTuples->crid[numRows]);
		Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);
	}

	vTuples->tid = NULL;
	if (fetchContext->need_tid)
	{
		vTuples->tid = (int64 *) ptr;
		ptr = (char *) &(vTuples->tid[numRows]);
		Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);
	}

	vTuples->skip = (uint16 *) ptr;
	ptr = (char *) &(vTuples->skip[TYPEALIGN(8, numRows) + 1]);
	Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);

	vTuples->local_skip = (uint16 *) ptr;
	ptr = (char *) &(vTuples->local_skip[numRows + 1]);
	Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);

	vTuples->isnull = (bool *) ptr;
	ptr = (char *) &(vTuples->isnull[numRows * vTuples->num_columns]);
	Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);

	vTuples->row_wise_local_ros = NULL;
	if (!(vTuples->use_column_store))
	{
		vTuples->row_wise_local_ros = (char *) MAXALIGN(ptr);
		ptr += numRows * vTuples->num_columns * (sizeof(Datum) + sizeof(bool));
		Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);
	}

	vTuples->work_decompression = NULL;
	if (0 < vTuples->size_decompression_area)
	{
		vTuples->work_decompression = ptr;
		ptr += vTuples->size_decompression_area;
		Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);
	}

	for (int columnId = VCI_FIRST_NORMALCOLUMN_ID; columnId < vTuples->num_columns; ++columnId)
	{
		uint32		datumSize;
		uint32		maxDictSize;
		int16		cId = vci_GetColumnIdFromFetchContext(fetchContext, columnId);
		vci_virtual_tuples_column_info_t *colInfo;

		colInfo = &(vTuples->column_info[columnId]);
		SizeOfElementAndPointer(info,
								fetchContext,
								cId,
								&datumSize,
								&(colInfo->max_column_size),
								&maxDictSize,
								&(colInfo->null_bit_id),
								&(colInfo->comp_type),
								&(colInfo->atttypid),
								&(colInfo->strict_datum_type));
		colInfo->dict_info = NULL;
		if (0 < maxDictSize)
		{
			colInfo->dict_info = (vci_DictInfo *) MAXALIGN(ptr);
			ptr += sizeof(vci_DictInfo);
			Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);
			if (!keepStatus)
			{
				vci_InitializeDictInfo(colInfo->dict_info);
				colInfo->dict_info->dictionary_storage = (unsigned char *) ptr;
				colInfo->dict_info->storage_size = maxDictSize;
			}
			ptr += maxDictSize;
			Assert((uintptr_t) ptr - (uintptr_t) (vTuples->flags) <= vTuples->size_flags);
		}

		if (vTuples->use_column_store)
		{
			colInfo->isnull = &(vTuples->isnull[numRows * columnId]);
			colInfo->values = &(vTuples->values[numRows * columnId]);
		}
		else
		{
			colInfo->isnull = NULL;
			colInfo->values = NULL;
		}

		colInfo->area = (char *) MAXALIGN(colInfo->al_area);
	}
}

/**
 * @brief Create an instance of \c vci_virtual_tuples_t where the read
 * ROS is stored.
 *
 * @details The function \c vci_CSFetchVirtualTuples() reads and stores
 * multiple rows at once from the specified columns and rows.
 * Users need to have enough area to store the maximum number of rows.
 * To prepare the area, the maximum number of rows should be passed via
 * the parameter \c numRows of this function.
 *
 * @param[in] fetchContext the fetch context.
 * @param[in] numRows required number of rows read at once.
 * @return the pointer to the created \c vci_virtual_tuples_t.
 */
vci_virtual_tuples_t *
vci_CSCreateVirtualTuplesWithNumRows(vci_CSFetchContext fetchContext,
									 uint32 numRows)
{
	MemoryContext mctx;
	vci_virtual_tuples_t *result;
	int32		size;
	vci_MainRelHeaderInfo *info;

	Assert(fetchContext);
	mctx = fetchContext->local_memory_context;
	Assert(mctx);
	info = GetMainRelHeaderInfoFromFetchContext(fetchContext);

	size = sizeof(vci_virtual_tuples_t) + ((fetchContext->num_columns - 1) *
										   sizeof(vci_virtual_tuples_column_info_t));

	result = MemoryContextAllocZero(mctx, size);
	result->size = size;
	result->num_columns = fetchContext->num_columns;
	result->extent_id = VCI_INVALID_EXTENT_ID;
	result->num_rows_in_extent = 0;
	result->row_id_in_extent = -1;
	result->num_rows = 0;
	result->buffer_capacity = 0;
	result->offset_of_first_tuple_of_vector = 0;
	result->num_rows_read_at_once = numRows;
	result->fetch_context = fetchContext;
	result->use_column_store = fetchContext->use_column_store;
	result->status = vcirvs_out_of_range;

	result->size_vector_memory_context = fetchContext->size_vector_memory_context;
	Assert(result->num_rows_read_at_once <= fetchContext->num_rows_read_at_once);
	result->size_values = fetchContext->size_values;
	result->size_flags = fetchContext->size_flags;
	result->size_dictionary_area = fetchContext->size_dictionary_area;
	result->size_decompression_area = fetchContext->size_decompression_area;

	result->al_values = MemoryContextAlloc(mctx,
										   result->size_values + MAXIMUM_ALIGNOF);

	result->al_flags = MemoryContextAlloc(mctx,
										  result->size_flags + MAXIMUM_ALIGNOF);

	{
		for (int columnId = VCI_FIRST_NORMALCOLUMN_ID; columnId < result->num_columns; ++columnId)
		{
			int16		cId = vci_GetColumnIdFromFetchContext(fetchContext, columnId);

			result->column_info[columnId].al_area = NULL;
			if (NeedDatumPointer(info, cId))
			{
				uint32		maxElemSize;

				SizeOfElementAndPointer(info, fetchContext, cId, NULL,
										&maxElemSize, NULL, NULL, NULL, NULL, NULL);
				result->column_info[columnId].al_area =
					MemoryContextAlloc(mctx,
									   MAXIMUM_ALIGNOF +
									   (MAXALIGN(maxElemSize) * numRows));
			}
		}
	}

	RefillPointersOfVirtualTuples(result, false);

	return result;
}

/**
 * @brief Destroy given instance of \c vci_virtual_tuples_t.
 *
 * @param[in] vTuples target to be destory.
 */
void
vci_CSDestroyVirtualTuples(vci_virtual_tuples_t *vTuples)
{
	for (int columnId = VCI_FIRST_NORMALCOLUMN_ID; columnId < vTuples->num_columns; ++columnId)
	{
		if (vTuples->column_info[columnId].al_area)
			pfree(vTuples->column_info[columnId].al_area);
	}

	pfree(vTuples->al_values);
	pfree(vTuples->al_flags);
	pfree(vTuples);
}

/**
 * @brief Fill CRIDs in the given \c vci_virtual_tuples_t.
 *
 * @param[in] vTuples target virtual tuples.
 */
void
vci_FillCridInVirtualTuples(vci_virtual_tuples_t *vTuples)
{
	int			aId;
	int			aIdRem = (vTuples->num_rows) & 7;
	int64	   *dst = vTuples->crid;
	int64		crid = vci_CalcCrid64(vTuples->extent_id,
									  vTuples->row_id_in_extent);

	for (aId = 0; aId < aIdRem; ++aId, ++crid)
		dst[aId] = crid;

	for (; aId < vTuples->num_rows; aId += 8, crid += 8)
	{
		dst[aId + 0] = crid + 0;
		dst[aId + 1] = crid + 1;
		dst[aId + 2] = crid + 2;
		dst[aId + 3] = crid + 3;
		dst[aId + 4] = crid + 4;
		dst[aId + 5] = crid + 5;
		dst[aId + 6] = crid + 6;
		dst[aId + 7] = crid + 7;
	}
}

static void
FillSkipLoadFillBitImage(uint64 *dst, uint8 data)
{
	/*
	 * This function expands values like below, for little endian case,
	 * ((int16*) dst)[0] = (data >> 0) & 1; ((int16*) dst)[1] = (data >> 1) &
	 * 1; ((int16*) dst)[2] = (data >> 2) & 1; ((int16*) dst)[3] = (data >> 3)
	 * & 1; ((int16*) dst)[4] = (data >> 4) & 1; ((int16*) dst)[5] = (data >>
	 * 5) & 1; ((int16*) dst)[6] = (data >> 6) & 1; ((int16*) dst)[7] = (data
	 * >> 7) & 1;
	 */
#ifdef WORDS_BIGENDIAN
	uint64		value = UINT64CONST(0x0001000080004000) * data;

	value += (UINT64CONST(0x2000) * data) >> 16;
	dst[0] = value & UINT64CONST(0x0001000100010001);
	dst[1] = (value >> 4) & UINT64CONST(0x0001000100010001);
#else
	uint64		value = UINT64CONST(0x0000200040008001) * data;

	dst[0] = value & UINT64CONST(0x0001000100010001);
	dst[1] = (value >> 4) & UINT64CONST(0x0001000100010001);
#endif							/* #ifdef WORDS_BIGENDIAN */
}

static void
FillSkipLoadBody(uint16 **dst_,
				 int *startOf,
				 Page page,
				 OffsetNumber oNum,
				 int numRows)
{
	ItemId		itemId = PageGetItemId(page, oNum);
	HeapTupleHeader hTup = (HeapTupleHeader) PageGetItem(page, itemId);
	uint8	   *data = &(((uint8 *) hTup)[hTup->t_hoff + *startOf]);
	int			aIdMax = numRows / 8;
	uint64	   *dst = (uint64 *) *dst_;

	*dst_ += numRows;
	*startOf = 0;
	for (int aId = 0; aId < aIdMax; aId += 4)
	{
		FillSkipLoadFillBitImage(dst + 0, data[0]);
		FillSkipLoadFillBitImage(dst + 2, data[1]);
		FillSkipLoadFillBitImage(dst + 4, data[2]);
		FillSkipLoadFillBitImage(dst + 6, data[3]);
		dst += 8;
		data += 4;
	}
}

static void
FillSkipLoad(vci_virtual_tuples_t *vTuples)
{
	int64		cridStart = vci_CalcCrid64(vTuples->extent_id,
										   vTuples->row_id_in_extent);
	int64		cridEnd = cridStart + vTuples->num_rows - 1;
	vci_ColumnRelations rel = vTuples->fetch_context->rel_delete;
	int			initCorr = vTuples->row_id_in_extent % VCI_NUM_ROWS_IN_ONE_ITEM_FOR_DELETE;

	BlockNumber startBN = vci_CalcBlockNumberFromCrid64ForDelete(cridStart);
	OffsetNumber startON = vci_CalcOffsetNumberFromCrid64ForDelete(cridStart);
	int			startOf = vci_CalcByteFromCrid64ForDelete(cridStart);

	BlockNumber endBN = vci_CalcBlockNumberFromCrid64ForDelete(cridEnd);
	OffsetNumber endON = vci_CalcOffsetNumberFromCrid64ForDelete(cridEnd);
	int			endOf = vci_CalcByteFromCrid64ForDelete(cridEnd);

	BlockNumber bNum;
	uint16	   *dst = vTuples->skip;
#ifdef USE_ASSERT_CHECKING
	uint16	   *dstSave = dst;
#endif							/* #ifdef USE_ASSERT_CHECKING */

	/*
	 * We always expand eight bits in a byte.  So the first row ID should be a
	 * multiple of eight.
	 */
	Assert(0 == (vTuples->row_id_in_extent & 7));

	for (bNum = startBN; bNum < endBN; ++bNum)
	{
		Buffer		buffer = ReadBuffer(rel.data, bNum);
		Page		page = BufferGetPage(buffer);

		for (OffsetNumber oNum = startON;
			 oNum < (VCI_ITEMS_IN_PAGE_FOR_DELETE + FirstOffsetNumber);
			 ++oNum)
		{
			FillSkipLoadBody(&dst,
							 &startOf,
							 page,
							 oNum,
							 VCI_NUM_ROWS_IN_ONE_ITEM_FOR_DELETE - initCorr);
			initCorr = 0;
			Assert((uintptr_t) dst <= (uintptr_t) &(dstSave[TYPEALIGN(8, vTuples->num_rows)]));
		}

		ReleaseBuffer(buffer);
		startON = FirstOffsetNumber;
	}

	{
		Buffer		buffer = ReadBuffer(rel.data, bNum);
		Page		page = BufferGetPage(buffer);
		OffsetNumber oNum;

		for (oNum = startON; oNum < endON; ++oNum)
		{
			FillSkipLoadBody(&dst,
							 &startOf,
							 page,
							 oNum,
							 VCI_NUM_ROWS_IN_ONE_ITEM_FOR_DELETE - initCorr);
			initCorr = 0;
			Assert((uintptr_t) dst <= (uintptr_t) &(dstSave[TYPEALIGN(8, vTuples->num_rows)]));
		}

		FillSkipLoadBody(&dst,
						 &startOf,
						 page,
						 oNum,
						 (endOf - startOf + 1) * 8);
		Assert((uintptr_t) dst <= (uintptr_t) &(dstSave[TYPEALIGN(8, vTuples->num_rows)]));

		ReleaseBuffer(buffer);
	}
}

static int64
GetPrevIdInDeleteList(vci_CSQueryContext queryContext, uint64 crid)
{
	if (queryContext->num_delete < 16)
	{
		for (int64 result = queryContext->num_delete; result--;)
		{
			if (queryContext->delete_list[result] <= crid)
				return result;
		}

		return -1;
	}
	else
	{
		uint64		result = 0;
		int			shiftBit = vci_GetHighestBit(queryContext->num_delete);

		Assert(0 <= shiftBit);
		for (uint64 tgtBit = UINT64CONST(1) << shiftBit; tgtBit; tgtBit >>= 1)
		{
			uint64		cand = result + tgtBit;

			if ((cand < queryContext->num_delete) &&
				(queryContext->delete_list[cand] <= crid))
				result = cand;
		}
		if (0 == result)
			return (queryContext->delete_list[0] <= crid) ? 0 : -1;

		return result;
	}
}

static void
MergeLocalDeleteListToSkip(vci_virtual_tuples_t *vTuples)
{
	vci_CSFetchContext fetchContext = vTuples->fetch_context;
	vci_CSQueryContext queryContext = fetchContext->query_context;
	int64		cridStart = vci_CalcCrid64(vTuples->extent_id,
										   vTuples->row_id_in_extent);
	int			numRows = vTuples->num_rows;
	int			startId;
	int			endId;

	if (queryContext->num_delete < 1)
		return;

	startId = Max(0, GetPrevIdInDeleteList(queryContext, cridStart));
	if (queryContext->delete_list[startId] < cridStart)
		++startId;

	endId = GetPrevIdInDeleteList(queryContext, cridStart + numRows - 1);

	for (int aid = startId; aid <= endId; ++aid)
	{
		uint64		crid = queryContext->delete_list[aid];
		uint64		offset = crid - cridStart;

		vTuples->skip[offset] = 1;
	}
}

static void
FillSkipCountUp(vci_virtual_tuples_t *vTuples)
{
	uint16	   *dst = vTuples->skip;
	uint16		count = 0;

	for (int aId = vTuples->num_rows; aId--;)
		dst[aId] = count += dst[aId] + ((dst[aId] - 1) * count);
}

static void
FillSkip(vci_virtual_tuples_t *vTuples)
{
	FillSkipLoad(vTuples);
	MergeLocalDeleteListToSkip(vTuples);

	vTuples->skip[vTuples->num_rows] = 0;

	FillSkipCountUp(vTuples);
}

static char *
FillFixedWidthCopyBody1(char *dstData,
						BlockNumber startBN,
						uint32 startOf,
						int stepDstData,
						int dataWidth,
						Relation rel,
						int numRows)
{
	BlockNumber bNumCur = startBN;
	int64		offset = startOf;
	Buffer		buffer = InvalidBuffer;
	Page		page = NULL;
	const Datum zero = 0;
#ifdef WORDS_BIGENDIAN
	const int	offsetCont = MAXALIGN(dataWidth) - dataWidth;
#else							/* #ifdef WORDS_BIGENDIAN */
	const int	offsetCont = MAXALIGN(dataWidth) - sizeof(Datum);
#endif							/* #ifdef WORDS_BIGENDIAN */

	if (0 < numRows)
	{
		buffer = ReadBuffer(rel, bNumCur);
		page = BufferGetPage(buffer);
	}

	for (int aId = 0; aId < numRows;)
	{
		int64		rest = VCI_MAX_PAGE_SPACE - offset;
		int			numElem = rest / dataWidth;
		int			maxBId = Min(aId + numElem, numRows);

		Assert(0 <= rest);
		for (int bId = aId; bId < maxBId; ++bId)
		{
#ifdef WORDS_BIGENDIAN
			*(Datum *) dstData = zero;
			memcpy(&(dstData[offsetCont]), &(page[VCI_MIN_PAGE_HEADER + offset]), dataWidth);
#else							/* #ifdef WORDS_BIGENDIAN */
			*(Datum *) &(dstData[offsetCont]) = zero;
			memcpy(dstData, &(page[VCI_MIN_PAGE_HEADER + offset]), dataWidth);
#endif							/* #ifdef WORDS_BIGENDIAN */
			dstData += stepDstData;
			offset += dataWidth;
		}

		aId = maxBId;

		if (numRows <= aId)
			break;

		if (offset < VCI_MAX_PAGE_SPACE)
		{
			int			size = VCI_MAX_PAGE_SPACE - offset;
			int			nextOffset = dataWidth - size;

			Assert(VCI_MAX_PAGE_SPACE < (offset + dataWidth - 1));
#ifdef WORDS_BIGENDIAN
			*(Datum *) dstData = zero;
			memcpy(&(dstData[offsetCont]), &(page[VCI_MIN_PAGE_HEADER + offset]), size);
#else							/* #ifdef WORDS_BIGENDIAN */
			*(Datum *) &(dstData[offsetCont]) = zero;
			memcpy(dstData, &(page[VCI_MIN_PAGE_HEADER + offset]), size);
#endif							/* #ifdef WORDS_BIGENDIAN */
			ReleaseBuffer(buffer);
			buffer = ReadBuffer(rel, ++bNumCur);
			page = BufferGetPage(buffer);
#ifdef WORDS_BIGENDIAN
			memcpy(&(dstData[offsetCont + size]), &(page[VCI_MIN_PAGE_HEADER]), nextOffset);
#else							/* #ifdef WORDS_BIGENDIAN */
			memcpy(dstData + size, &(page[VCI_MIN_PAGE_HEADER]), nextOffset);
#endif							/* #ifdef WORDS_BIGENDIAN */
			dstData += stepDstData;
			offset = nextOffset;
			++aId;
		}
		else
		{
			Assert(offset == VCI_MAX_PAGE_SPACE);
			offset = 0;
			ReleaseBuffer(buffer);
			buffer = ReadBuffer(rel, ++bNumCur);
			page = BufferGetPage(buffer);
		}
	}
	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);

	return dstData;
}

static void
FillFixedWidth(vci_virtual_tuples_t *vTuples,
			   int16 columnId,
			   vci_ColumnRelations *rel)
{
	vci_MainRelHeaderInfo *info = GetMainRelHeaderInfoFromFetchContext(vTuples->fetch_context);
	Datum	   *dstPtr = NULL;
	char	   *dstData = NULL;

	int			dataWidth = 0;

	int16		colId = columnId;

	bool		passByRef = false;

	char	   *checkPtr PG_USED_FOR_ASSERTS_ONLY;

	BlockNumber startBN;
	uint32		startOf;

	int			facRow = vTuples->num_columns;
	int			stepDstData = sizeof(Datum) * facRow;

	if (VCI_FIRST_NORMALCOLUMN_ID <= columnId)
	{
		int			facCol = 1;

		Assert(columnId < vTuples->num_columns);

		dataWidth = vTuples->column_info[columnId].max_column_size;
		if (vTuples->use_column_store)
		{
			facRow = 1;
			stepDstData = sizeof(Datum) * facRow;
			facCol = vTuples->num_rows_read_at_once;
		}

		dstData = (char *) &(vTuples->values[facCol * columnId]);

		if ((passByRef = vTuples->column_info[columnId].strict_datum_type)) /* pgr0011 */
		{
			dstPtr = (Datum *) dstData;
			dstData = vTuples->column_info[columnId].area;
			stepDstData = MAXALIGN(dataWidth);
			Assert(dstData);
		}
		else
			Assert(NULL == vTuples->column_info[columnId].area);
		colId = vci_GetColumnIdFromFetchContext(vTuples->fetch_context,
												columnId);
	}
	else
	{
		Assert(VCI_COLUMN_ID_TID == columnId);
		dstData = (char *) (vTuples->tid);
		stepDstData = sizeof(vTuples->tid[0]);
		dataWidth = sizeof(ItemPointerData);
	}

	vci_GetPositionForFixedColumn(&startBN,
								  &startOf,
								  info,
								  colId,
								  vTuples->extent_id,
								  vTuples->row_id_in_extent,
								  false);

	checkPtr = FillFixedWidthCopyBody1(dstData,
									   startBN,
									   startOf,
									   stepDstData,
									   dataWidth,
									   rel->data,
									   vTuples->num_rows);
	if (passByRef)
	{
		Assert(VCI_FIRST_NORMALCOLUMN_ID <= columnId);
		for (int rId = 0; rId < vTuples->num_rows; ++rId)
			dstPtr[facRow * rId] = PointerGetDatum(&(dstData[stepDstData * rId]));
		Assert((uintptr_t) (vTuples->column_info[columnId].area) <= (uintptr_t) checkPtr);
		Assert((uintptr_t) checkPtr <= (uintptr_t) &(vTuples->column_info[columnId].area[vTuples->column_info[columnId].max_column_size * vTuples->num_rows_read_at_once]));
		if (vTuples->use_column_store)
			Assert(vTuples->column_info[columnId].values == dstPtr);
		else
			Assert(&(vTuples->values[columnId]) == dstPtr);
	}
	else
	{
		if (VCI_FIRST_NORMALCOLUMN_ID <= columnId)
		{
			if (vTuples->use_column_store)
			{
				Assert((uintptr_t) (vTuples->column_info[columnId].values) <= (uintptr_t) checkPtr);
				Assert((uintptr_t) checkPtr <= (uintptr_t) &(vTuples->column_info[columnId].values[vTuples->num_rows_read_at_once]));
			}
			else
			{
				Assert((uintptr_t) (&(vTuples->values[columnId])) <= (uintptr_t) checkPtr);
				Assert((uintptr_t) checkPtr <= (uintptr_t) &(vTuples->values[columnId + (vTuples->num_rows_read_at_once * vTuples->num_columns)]));
			}
		}
		else
		{
			Assert((uintptr_t) (vTuples->tid) <= (uintptr_t) checkPtr);
			Assert((uintptr_t) checkPtr <= (uintptr_t) &(vTuples->tid[vTuples->num_rows_read_at_once]));
		}
	}
}

static void
Copy3(char *dst, char *src, int len)
{
	if (2 & len)
	{
#if defined(__i386__) || defined(__x86_64__) || defined(__powerpc64__)	/* Little Endian */
		*(uint16 *) dst = *(uint16 *) src;
		dst += 2;
		src += 2;
#else							/* #if defined(__i386__) ||
								 * defined(__x86_64__) */
		*(dst++) = *(src++);
		*(dst++) = *(src++);
#endif							/* #if defined(__i386__) ||
								 * defined(__x86_64__) */
	}
	if (1 & len)
		*dst = *src;
}

static uint32
GetVarlenAHeader(Datum *header_,
				 Buffer *buffer,
				 BlockNumber *currentBlockNumber,
				 uint32 offsetInPage,
				 Relation rel)
{
	char	   *header = (char *) header_;
	Page		page;
	char	   *curPtr;
	int			len = VCI_MAX_PAGE_SPACE - offsetInPage;

	if (len <= 0)
	{
		Assert(BlockNumberIsValid(*currentBlockNumber));
		if (MaxBlockNumber == *currentBlockNumber)
			ereport(ERROR, (errmsg("relation full"), errhint("Disable VCI by 'SELECT vci_disable();'")));
		if (BufferIsValid(*buffer))
			ReleaseBuffer(*buffer);
		*buffer = ReadBuffer(rel, ++*currentBlockNumber);
		offsetInPage -= VCI_MAX_PAGE_SPACE;
		len = VCI_MAX_PAGE_SPACE - offsetInPage;
	}

	page = BufferGetPage(*buffer);
	curPtr = &(page[VCI_MIN_PAGE_HEADER + offsetInPage]);

	Assert(0 < len);

	if (VARATT_IS_1B_E(curPtr)) /* VARHDRSZ_EXTERNAL */
	{
		Assert(2 == VARHDRSZ_EXTERNAL);
		if (VARHDRSZ_EXTERNAL <= len)
		{
			*(header++) = *(curPtr++);
			*(header++) = *(curPtr++);

			return offsetInPage + VARHDRSZ_EXTERNAL;
		}

		Assert(1 == len);
		*(header++) = *(curPtr++);
		ReleaseBuffer(*buffer);
		++*currentBlockNumber;
		*buffer = ReadBuffer(rel, *currentBlockNumber);
		page = BufferGetPage(*buffer);
		*header = page[VCI_MIN_PAGE_HEADER];

		return 1;
	}

	if (VARATT_IS_1B(curPtr))	/* VARHDRSZ_SHORT */
	{
		Assert(1 == VARHDRSZ_SHORT);
		Assert(VARHDRSZ_SHORT <= len);
		*header = *curPtr;

		return offsetInPage + VARHDRSZ_SHORT;
	}

	/* VARHDRSZ */
	Assert(4 == VARHDRSZ);

	if (VARHDRSZ <= len)
	{
		*(uint32 *) header = *(uint32 *) curPtr;

		return offsetInPage + VARHDRSZ;
	}

	Assert((0 <= len) && (len <= 3));
	Copy3(header, curPtr, len);
	header += len;
	curPtr += len;

	ReleaseBuffer(*buffer);
	++*currentBlockNumber;
	*buffer = ReadBuffer(rel, *currentBlockNumber);
	page = BufferGetPage(*buffer);
	len = VARHDRSZ - len;
	curPtr = &(page[VCI_MIN_PAGE_HEADER]);

	Assert((0 <= len) && (len <= 3));
	Copy3(header, curPtr, len);

	return len;
}

static void
FillVariableWidth(vci_virtual_tuples_t *vTuples,
				  int16 columnId,
				  vci_ColumnRelations *rel)
{
	vci_MainRelHeaderInfo *info = GetMainRelHeaderInfoFromFetchContext(vTuples->fetch_context);
	char	   *dstData = vTuples->column_info[columnId].area;
	Datum	   *dstPtr = &(vTuples->values[columnId]);
	int			ptrStep = vTuples->num_columns;

	BlockNumber startBN;
	uint32		startOf;

	if (vTuples->use_column_store)
	{
		dstPtr = &(vTuples->values[vTuples->num_rows_read_at_once * columnId]);
		ptrStep = 1;
	}

	/* This function must be called only for ROS, not local ROS. */
	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= vTuples->extent_id);

	{
		uint32		offset;
		uint32		dataOffset;
		BlockNumber blockNumberBase;
		TupleDesc	desc = vci_GetTupleDescr(info);
		int16		cId = vci_GetColumnIdFromFetchContext(vTuples->fetch_context,
														  columnId);

		vci_GetElementPosition(&offset,
							   &blockNumberBase,
							   &dataOffset,
							   rel,
							   vTuples->extent_id,
							   vTuples->row_id_in_extent,
							   TupleDescAttr(desc, cId));
		vci_GetBlockNumberAndOffsetInPage(&startBN,
										  &startOf,
										  offset + dataOffset);
		startBN += blockNumberBase;
	}

	{
		BlockNumber bNum = startBN;
		Size		offsetInPage = startOf;
		Buffer		buffer = InvalidBuffer;
		Page		page;
		int			numWrite = 0;
		int			numRows = vTuples->num_rows;

		if (0 < numRows)
		{
			buffer = ReadBuffer(rel->data, bNum);
			page = BufferGetPage(buffer);
		}

		for (int aId = 0; aId < numRows; ++aId)
		{
			offsetInPage = GetVarlenAHeader((Datum *) dstData,
											&buffer,
											&bNum,
											offsetInPage,
											rel->data);

			{
				int32		copySize;
				uint32		dataSize = VARSIZE_ANY_EXHDR(dstData);
				uint32		headerSize = vci_VARHDSZ_ANY(dstData);

				dstPtr[ptrStep * (numWrite++)] = PointerGetDatum(dstData);

				if (VCI_MAX_PAGE_SPACE <= offsetInPage)
				{
					offsetInPage -= VCI_MAX_PAGE_SPACE;
					Assert(offsetInPage < VCI_MAX_PAGE_SPACE);
					if (0 == offsetInPage)
					{
						ReleaseBuffer(buffer);
						buffer = ReadBuffer(rel->data, ++bNum);
					}
				}
				page = BufferGetPage(buffer);

				copySize = Min(dataSize, VCI_MAX_PAGE_SPACE - offsetInPage);
				memcpy(&(dstData[headerSize]),
					   &(page[VCI_MIN_PAGE_HEADER + offsetInPage]),
					   copySize);
				offsetInPage += copySize;

				if (copySize < dataSize)
				{
					ReleaseBuffer(buffer);
					buffer = ReadBuffer(rel->data, ++bNum);
					page = BufferGetPage(buffer);
					memcpy(&(dstData[copySize + headerSize]),
						   &(page[VCI_MIN_PAGE_HEADER]),
						   dataSize - copySize);
					offsetInPage = dataSize - copySize; /* pgr0063 */
				}
				dstData += MAXALIGN(dataSize + headerSize);
			}

			if (VCI_MAX_PAGE_SPACE <= offsetInPage)
			{
				ReleaseBuffer(buffer);
				buffer = ReadBuffer(rel->data, ++bNum);
				page = BufferGetPage(buffer);
				offsetInPage -= VCI_MAX_PAGE_SPACE;
				Assert(offsetInPage < VCI_MAX_PAGE_SPACE);
			}
		}

		if (BufferIsValid(buffer))
			ReleaseBuffer(buffer);

		Assert(vTuples->num_rows == numWrite);
	}
}

static void
FillValues(vci_virtual_tuples_t *vTuples)
{
	for (int16 columnId = VCI_FIRST_NORMALCOLUMN_ID; columnId < vTuples->num_columns; ++columnId)
	{
		switch (vTuples->column_info[columnId].comp_type)
		{
			case vcis_compression_type_fixed_raw:
				FillFixedWidth(vTuples, columnId,
							   &(vTuples->fetch_context->rel_column[columnId]));
				break;
			case vcis_compression_type_variable_raw:
				FillVariableWidth(vTuples, columnId,
								  &(vTuples->fetch_context->rel_column[columnId]));
				break;
			default:
				abort();
		}
	}
}

static int
GetNullableColumnInfo(uint16 **columnId, uint16 **nullBitId,
					  vci_virtual_tuples_t *vTuples)
{
	int			cId;

	vci_CSQueryContext queryContext = vTuples->fetch_context->query_context;

	*columnId = palloc0_array(uint16, queryContext->num_nullable_columns);
	*nullBitId = palloc0_array(uint16, queryContext->num_nullable_columns);

	cId = 0;
	for (int aId = 0; aId < vTuples->num_columns; ++aId)
	{
		int			bitId = vTuples->column_info[aId].null_bit_id;

		if (0 <= bitId)
		{
			(*columnId)[cId] = aId;
			(*nullBitId)[cId] = bitId;
			++cId;
		}
	}
	Assert(cId <= queryContext->num_nullable_columns);

	return cId;
}

static void
FillIsNull(vci_virtual_tuples_t *vTuples)
{
	int			colOffset[MaxAttrNumber];
	Buffer		buffer = InvalidBuffer;
	Page		page = NULL;
	vci_CSQueryContext queryContext = vTuples->fetch_context->query_context;
	vci_MainRelHeaderInfo *info = GetMainRelHeaderInfoFromFetchContext(vTuples->fetch_context);
	const int32 strideR = 16;
	uint16	   *columnId;
	uint16	   *nullBitId;
	uint8	   *nullCopy = palloc0_array(uint8, (strideR * queryContext->null_width_in_byte));

	BlockNumber bNumCur;
	uint32		offset;

	Relation	rel = vTuples->fetch_context->rel_null.data;

	int			numNullableColumns = GetNullableColumnInfo(&columnId, &nullBitId, vTuples);

	int			facCol = 1;
	int			facRow = vTuples->num_columns;

	if (vTuples->use_column_store)
	{
		facCol = vTuples->num_rows_read_at_once;
		facRow = 1;
	}

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= vTuples->extent_id);
	MemSet(vTuples->isnull, 0, vTuples->num_columns * vTuples->num_rows);

	{
		for (int aId = 0; aId < numNullableColumns; ++aId)
			colOffset[aId] = facCol * columnId[aId];
	}
	vci_GetPositionForFixedColumn(&bNumCur,
								  &offset,
								  info,
								  VCI_COLUMN_ID_NULL,
								  vTuples->extent_id,
								  vTuples->row_id_in_extent,
								  false);

	if (0 < vTuples->num_rows)
	{
		buffer = ReadBuffer(rel, bNumCur);
		page = BufferGetPage(buffer);
	}

	/* This tiling is the best? */
	for (int32 rId = 0; rId < vTuples->num_rows; rId += strideR)
	{
		int32		pIdMax = Min(rId + strideR, vTuples->num_rows);
		int			nwib = queryContext->null_width_in_byte;
		int32		inc = (pIdMax - rId) * nwib;
		uint32		nextOffset = offset + inc;
		uint8	   *ptr = (uint8 *) &(page[VCI_MIN_PAGE_HEADER + offset]);
		uint8	   *ptrSave = NULL;

		Assert((0 <= offset) && (offset < VCI_MAX_PAGE_SPACE));
		if (VCI_MAX_PAGE_SPACE < nextOffset)
		{
			int			size = VCI_MAX_PAGE_SPACE - offset;

			memcpy(nullCopy, ptr, size);
			ReleaseBuffer(buffer);
			buffer = ReadBuffer(rel, ++bNumCur);
			page = BufferGetPage(buffer);
			memcpy(&(nullCopy[size]), &(page[VCI_MIN_PAGE_HEADER]),
				   inc - size);
			ptr = nullCopy;
		}

		ptrSave = ptr;
		for (int cId = 0; cId < numNullableColumns; ++cId)
		{
			int32		pId;
			int			bitId = nullBitId[cId];
			bool	   *dst = &(vTuples->isnull[colOffset[cId] + (rId * facRow)]);

			ptr = ptrSave;
			for (pId = rId; pId <= (pIdMax - 4); pId += 4)
			{
				*dst = vci_GetBit(ptr, bitId);
				ptr += nwib;
				dst += facRow;
				*dst = vci_GetBit(ptr, bitId);
				ptr += nwib;
				dst += facRow;
				*dst = vci_GetBit(ptr, bitId);
				ptr += nwib;
				dst += facRow;
				*dst = vci_GetBit(ptr, bitId);
				ptr += nwib;
				dst += facRow;
			}
			for (; pId < pIdMax; ++pId)
			{
				*dst = vci_GetBit(ptr, bitId);
				ptr += nwib;
				dst += facRow;
			}
		}

		offset = nextOffset;
		if (VCI_MAX_PAGE_SPACE <= offset)
		{
			if (VCI_MAX_PAGE_SPACE == offset)
			{
				ReleaseBuffer(buffer);
				buffer = ReadBuffer(rel, ++bNumCur);
				page = BufferGetPage(buffer);
			}
			offset -= VCI_MAX_PAGE_SPACE;
		}
	}
	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);

	pfree(nullCopy);
	pfree(nullBitId);
	pfree(columnId);
}

/**
 * @brief Fetch or read data in columns specified in \c vTuples,
 * \c numReadRows rows from \c cridStart.
 *
 * @details vci_CSFetchVirtualTuples returns number of rows which can be read
 * from stored data after cridStart.  For example, if cridStart = 50, but
 * actualNumberOfRowsReadAtOnce = 128, vci_CSFetchVirtualTuples() returns 78
 * (= 128 - 50).
 *
 * When all the tuples between cridStart and (cridStart + numReadRows - 1)
 * is stored in vTuples, it does not read ROS.
 * Otherwise, tuples at TYPEALIGN_DOWN(VCI_COMPACTION_UNIT_ROW, cridStart)
 * and following (actualNumRowsReadAtOnce - 1) rows are read from ROS.
 *
 * @param[in,out] vTuples the read data are stored in the pointed area.
 * @param[in] cridStart the data from \c cridStart row are read.
 * @param[in] numReadRows required number of rows to be read.
 * @return number of rows enable to be read out from \c vTuples.
 */
int
vci_CSFetchVirtualTuples(vci_virtual_tuples_t *vTuples,
						 int64 cridStart,
						 uint32 numReadRows)
{
	const int32 extentId = vci_CalcExtentIdFromCrid64(cridStart);
	const uint32 rowId = vci_CalcRowIdInExtentFromCrid64(cridStart);

	Assert(vTuples);

	vTuples->status = vcirvs_out_of_range;
	if (VCI_INVALID_EXTENT_ID == extentId)
	{
		return 0;
	}

	RefillPointersOfVirtualTuples(vTuples, true);

	/* local ROS */
	if (extentId < VCI_FIRST_NORMAL_EXTENT_ID)
	{
		vci_CSFetchContext fetchContext = vTuples->fetch_context;
		vci_CSQueryContext queryContext = fetchContext->query_context;
		vci_local_ros_t *localRos = queryContext->local_ros;
		int			localRosId = -extentId - 1;

		Assert(queryContext->num_local_ros_extents == localRos->num_local_extents);
		if (queryContext->num_local_ros_extents <= localRosId)
		{
			vTuples->status = vcirvs_not_exist;

			return 0;
		}

		if (localRos->extent[localRosId]->num_rows_in_extent <= rowId)
		{
			vTuples->status = vcirvs_out_of_range;

			return 0;
		}

		vTuples->num_rows_in_extent = localRos->extent[localRosId]->num_rows_in_extent;
		vTuples->extent_id = extentId;
		vTuples->num_rows = Min(numReadRows,
								vTuples->num_rows_in_extent - rowId);
		vTuples->offset_of_first_tuple_of_vector = 0;

		if (vTuples->tid)
			memcpy(vTuples->tid, &(localRos->extent[localRosId]->tid[rowId]),
				   sizeof(vTuples->tid[0]) * vTuples->num_rows);

		if (vTuples->crid)
			memcpy(vTuples->crid, &(localRos->extent[localRosId]->crid[rowId]),
				   sizeof(vTuples->crid[0]) * vTuples->num_rows);

		MemSet(vTuples->skip, 0,
			   sizeof(vTuples->skip[0]) * (vTuples->num_rows + 1));

		if (vTuples->use_column_store)
		{
			for (int cId = 0; cId < vTuples->num_columns; ++cId)
			{
				vci_virtual_tuples_column_info_t *dColI;
				vci_virtual_tuples_column_info_t *sColI;

				dColI = &(vTuples->column_info[cId]);
				sColI = &(localRos->extent[localRosId]->column_info[
																	fetchContext->column_link[cId]]);
				memcpy(dColI->values, &(sColI->values[rowId]),
					   sizeof(Datum) * vTuples->num_rows);
				memcpy(dColI->isnull, &(sColI->isnull[rowId]),
					   sizeof(bool) * vTuples->num_rows);
			}
		}
		else
		{
			vTuples->values = (Datum *) TYPEALIGN(sizeof(Datum),
												  vTuples->row_wise_local_ros);
			vTuples->isnull = (bool *) &(vTuples->values[vTuples->num_rows_read_at_once *
														 vTuples->num_columns]);
			for (int rId = 0; rId < vTuples->num_rows; ++rId)
			{
				int			offset = rId * vTuples->num_columns;
				Datum	   *dstValues = &(vTuples->values[offset]);
				bool	   *dstIsNull = &(vTuples->isnull[offset]);

				for (int cId = 0; cId < vTuples->num_columns; ++cId)
				{
					vci_virtual_tuples_column_info_t *sColI;

					sColI = &(localRos->extent[localRosId]->column_info[
																		fetchContext->column_link[cId]]);
					dstValues[cId] = sColI->values[rowId + rId];
					dstIsNull[cId] = sColI->isnull[rowId + rId];
				}
			}
		}

		vTuples->status = (localRos->extent[localRosId]->num_rows_in_extent <=
						   (rowId + vTuples->num_rows))
			? vcirvs_end_of_extent : vcirvs_read_whole;
	}
	else
	{
		vTuples->status = vcirvs_read_whole;
		/* use stored data */
		if ((extentId == vTuples->extent_id) &&
			(vTuples->row_id_in_extent <= rowId) &&
			((rowId + numReadRows) <=
			 (vTuples->row_id_in_extent + vTuples->num_rows)))
		{
			vTuples->offset_of_first_tuple_of_vector = rowId -
				vTuples->row_id_in_extent;
		}
		else
		{
			uint32		numRowsInExtent = vTuples->num_rows_in_extent;

			{
				vci_extent_status_t status;

				vci_CSCheckExtent(&status, vTuples->fetch_context, extentId, false);
				/* check if the extent is visible */
				if (!((status.existence) && (status.visible)))
				{
					vTuples->status = vcirvs_not_visible;

					return 0;	/* not visible */
				}
			}

			if (extentId != vTuples->extent_id)
			{
				Buffer		buffer = InvalidBuffer;
				vcis_m_extent_t *mExtent;

				mExtent = vci_GetMExtent(&buffer,
										 GetMainRelHeaderInfoFromFetchContext(
																			  vTuples->fetch_context),
										 extentId);

				LockBuffer(buffer, BUFFER_LOCK_SHARE);
				numRowsInExtent = mExtent->num_rows;
				UnlockReleaseBuffer(buffer);

				vTuples->num_rows_in_extent = numRowsInExtent;
				vTuples->extent_id = extentId;
				vTuples->num_rows = 0;
			}

			/* no such a row in the extent */
			if (numRowsInExtent <= rowId)
			{
				vTuples->status = vcirvs_out_of_range;

				return 0;
			}

			vTuples->row_id_in_extent = TYPEALIGN_DOWN(VCI_COMPACTION_UNIT_ROW,
													   rowId);
			vTuples->offset_of_first_tuple_of_vector = rowId -
				vTuples->row_id_in_extent;
			vTuples->num_rows = TYPEALIGN(VCI_COMPACTION_UNIT_ROW,
										  vTuples->offset_of_first_tuple_of_vector +
										  numReadRows);
			vTuples->num_rows = Min(vTuples->num_rows,
									vTuples->num_rows_read_at_once);
			vTuples->num_rows = Min(vTuples->num_rows,
									numRowsInExtent - vTuples->row_id_in_extent);

			if (vTuples->crid)
				vci_FillCridInVirtualTuples(vTuples);

			if (vTuples->tid)
				FillFixedWidth(vTuples, VCI_COLUMN_ID_TID,
							   &(vTuples->fetch_context->rel_tid));

			FillSkip(vTuples);

			FillIsNull(vTuples);
			FillValues(vTuples);
		}

		if (vTuples->num_rows_in_extent <= (vTuples->row_id_in_extent +
											vTuples->offset_of_first_tuple_of_vector + numReadRows))
			vTuples->status = vcirvs_end_of_extent;
	}

	Assert(vTuples->offset_of_first_tuple_of_vector <= vTuples->num_rows);

	return Min(vTuples->num_rows - vTuples->offset_of_first_tuple_of_vector,
			   numReadRows);
}

/**
 * @brief Fill data of the specified fixed-field-length column in
 * \c RosChunkStorage into \c vci_virtual_tuples_t.
 *
 * @param[in,out] vTuples the pointer of vci_virtual_tuples_t where data are
 * stored.
 * @param[in] columnId target column ID.
 * @param[in] rosChunkStorage data source.
 */
void
vci_FillFixedWidthColumnarFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
											  int16 columnId,
											  RosChunkStorage *rosChunkStorage)
{
	int16		colIdInVciMain = VCI_FIRST_NORMALCOLUMN_ID;
	Datum	   *dstPtr = NULL;
	char	   *dstData = NULL;

	int			stepDstData = 0;
	int			stepSrc = 0;

	bool		passByRef = false;
	int			offsetCont = 0;

	if (VCI_FIRST_NORMALCOLUMN_ID <= columnId)
	{
		vci_CSFetchContext fetchContext = vTuples->fetch_context;

		Assert(columnId < vTuples->num_columns);
		colIdInVciMain = fetchContext->query_context->column_id[fetchContext->column_link[columnId]];
		dstData = (char *) &(vTuples->values[vTuples->num_rows_in_extent * columnId]);
		if ((passByRef = vTuples->column_info[columnId].strict_datum_type)) /* pgr0011 */
		{
			dstPtr = (Datum *) dstData;
			dstData = vTuples->column_info[columnId].area;
			Assert(dstData);
		}
		else
			Assert(NULL == vTuples->column_info[columnId].area);
		stepSrc = vTuples->column_info[columnId].max_column_size;
		stepDstData = MAXALIGN(stepSrc);
	}
	else
	{
		Assert(VCI_COLUMN_ID_TID == columnId);
		dstData = (char *) (vTuples->tid);
		stepDstData = sizeof(vTuples->tid[0]);
		stepSrc = sizeof(ItemPointerData);
	}

	{
		const Datum zero = 0;
#ifdef WORDS_BIGENDIAN

		if (stepSrc < sizeof(Datum))
		{
			/*
			 * if the value itself is contained in Datum. for example uint32 1
			 * is contained in Datum
			 */
			/*
			 * the value should be 0x0000000000000001 (not 0x0001000000000000)
			 * so offsetCont should be 4
			 */
			offsetCont = stepDstData - stepSrc;
		}
		else
		{
			offsetCont = 0;
		}

#else							/* #ifdef WORDS_BIGENDIAN */
		offsetCont = stepDstData - sizeof(Datum);
#endif							/* #ifdef WORDS_BIGENDIAN */

		for (int sId = 0; sId < rosChunkStorage->numFilled; ++sId)
		{
			RosChunkBuffer *chunk = rosChunkStorage->chunk[sId];
			char	   *srcPtr = chunk->tidData;

			if (VCI_FIRST_NORMALCOLUMN_ID <= columnId)
				srcPtr = chunk->data[colIdInVciMain];
			for (int rId = 0; rId < chunk->numFilled; ++rId)
			{
#ifdef WORDS_BIGENDIAN
				*(Datum *) dstData = zero;
				memcpy(&(dstData[offsetCont]), &(srcPtr[stepSrc * rId]), stepSrc);
#else							/* #ifdef WORDS_BIGENDIAN */
				*(Datum *) &(dstData[offsetCont]) = zero;
				memcpy(dstData, &(srcPtr[stepSrc * rId]), stepSrc);
#endif							/* #ifdef WORDS_BIGENDIAN */
				dstData += stepDstData;
			}
		}
	}

	if (passByRef)
	{
		dstData = vTuples->column_info[columnId].area;
		for (int rId = 0; rId < vTuples->num_rows; ++rId)
			dstPtr[rId] = PointerGetDatum(&(dstData[stepDstData * rId]));
	}
}

/**
 * @brief Fill data of the specified variable-field-length column in
 * \c RosChunkStorage into \c vci_virtual_tuples_t.
 *
 * @param[in,out] vTuples the pointer of vci_virtual_tuples_t where data are
 * stored.
 * @param[in] columnId target column ID.
 * @param[in] rosChunkStorage data source.
 */
void
vci_FillVariableWidthColumnarFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
												 int16 columnId,
												 RosChunkStorage *rosChunkStorage)
{
	int16		colIdInVciMain = VCI_FIRST_NORMALCOLUMN_ID;
	Datum	   *dstPtr = NULL;
	char	   *dstData = NULL;

	Assert((VCI_FIRST_NORMALCOLUMN_ID <= columnId) && (columnId < vTuples->num_columns));
	dstData = (char *) &(vTuples->values[vTuples->num_rows_in_extent * columnId]);
	Assert(vTuples->column_info[columnId].strict_datum_type);

	colIdInVciMain = vTuples->fetch_context->query_context->column_id[vTuples->fetch_context->column_link[columnId]];

	dstPtr = (Datum *) dstData;
	dstData = vTuples->column_info[columnId].area;
	Assert(dstData);

	{
		const Datum zero = 0;

		for (int sId = 0; sId < rosChunkStorage->numFilled; ++sId)
		{
			RosChunkBuffer *chunk = rosChunkStorage->chunk[sId];

			Assert(chunk->data[colIdInVciMain]);
			Assert(chunk->dataOffset[colIdInVciMain]);
			for (int rId = 0; rId < chunk->numFilled; ++rId)
			{
				int			size = chunk->dataOffset[colIdInVciMain][rId + 1] - chunk->dataOffset[colIdInVciMain][rId];

				*(Datum *) &(dstData[TYPEALIGN_DOWN(sizeof(Datum), size - 1)]) = zero;
				memcpy(dstData, &(chunk->data[colIdInVciMain][chunk->dataOffset[colIdInVciMain][rId]]), size);
				*dstPtr++ = PointerGetDatum(dstData);
				dstData += TYPEALIGN(sizeof(Datum), size);
			}
		}
	}
}

/**
 * @brief Get column IDs of nullable columns.
 *
 * The result is stored in a \c palloc()ed area. Thus, caller should \c pfree()
 * the result after use.
 *
 * @param[in] vTuples target vci_virtual_tuples_t.
 * @return the pointer of \c (int16 \*) where the result stored.
 */
int16 *
vci_GetNullableColumnIds(vci_virtual_tuples_t *vTuples)
{
	vci_CSQueryContext queryContext = vTuples->fetch_context->query_context;
	int16	   *result = palloc0_array(int16, queryContext->num_nullable_columns);
#ifdef USE_ASSERT_CHECKING
	int16		cId = 0;
#endif

	for (int i = 0; i < queryContext->num_nullable_columns; i++)
		result[i] = -1;

	for (int16 aId = 0; aId < vTuples->num_columns; ++aId)
	{
		int			bitId = vTuples->column_info[aId].null_bit_id;

		Assert((-1 <= bitId) && (bitId < (int) (queryContext->num_nullable_columns)));
		if (0 <= bitId)
		{
			Assert(-1 == result[bitId]);
			result[bitId] = aId;
#ifdef USE_ASSERT_CHECKING
			++cId;
#endif
		}
	}
	Assert(cId <= queryContext->num_nullable_columns);

	return result;
}
