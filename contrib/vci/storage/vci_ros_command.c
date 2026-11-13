/*-------------------------------------------------------------------------
 *
 * vci_ros_command.c
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_ros_command.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdint.h>
#include <stdlib.h>

#ifndef WIN32
#include <sys/time.h>
#endif

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/relscan.h"
#include "access/tupdesc.h"
#include "access/genam.h"
#include "access/visibilitymap.h"	/* for visibilitymap_set() */
#include "access/xact.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_operator.h"	/* for TIDLessOperator */
#include "catalog/storage.h"
#include "commands/vacuum.h"
#include "storage/freespace.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"		/* for RelationSetTargetBlock() */
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

#include "postgresql_copy.h"

#include "vci.h"
#include "vci_chunk.h"

#include "vci_columns.h"
#include "vci_columns_data.h"

#include "vci_fetch.h"
#include "vci_freelist.h"
#include "vci_mem.h"
#include "vci_ros.h"
#include "vci_ros_command.h"
#include "vci_tidcrid.h"
#include "vci_wos.h"
#include "vci_xact.h"

extern bool HeapTupleSatisfiesWos2Ros(HeapTuple htup, Snapshot snapshot, Buffer buffer);
extern bool HeapTupleSatisfiesLocalRos(HeapTuple htup, Snapshot snapshot, Buffer buffer);
bool		VCITupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot, Buffer buffer);

typedef enum
{
	CEK_CountDeletedRows,
	CEK_CountUnusedExtents,
} CEKind;

typedef enum
{
	WOS_Data,
	WOS_Whiteout,
} WosKind;

typedef struct
{
	ItemPointerData orig_tid;

	ItemPointerData wos_tid;

	bool		movable;

	int64		xid64;

} vci_tid_tid_xid64_t;

static bool WaitTransactionEndOfLastRosCommand(vci_MainRelHeaderInfo *info);
static void fillTidListFromTidSortState(vci_RosCommandContext *comContext, int numRows);
static int	ConvertWos2Ros(vci_RosCommandContext *comContext);
static void FillValuesColumnwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples, RosChunkStorage *rosChunkStorage);
static void FillIsNullColumnwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples, RosChunkStorage *rosChunkStorage);
static void FillIsNullRowwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples, RosChunkStorage *rosChunkStorage);
static void FillValuesRowwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples, RosChunkStorage *rosChunkStorage);
static void AppendDataToLocalRos(vci_local_ros_t *localRos, RosChunkStorage *storage, vci_MainRelHeaderInfo *info);
static Size ConvertWos2LocalRos(vci_RosCommandContext *comContext);
static void FillOneRosChunkBuffer(vci_RosCommandContext *comContext, int rowId, int numRowsToConvert);
static void ReadOneExtentAndStoreInChunkStorage(vci_RosCommandContext *comContext);
static Size ConvertWhiteOut2LocalDeleteList(vci_RosCommandContext *comContext, int sel);
static bool NeedMainRelHeaderUpdate(vci_ros_command_t command);
static int	CmpUint64(const void *pa, const void *pb);
static void FlushTidCridPairListToTreeForBuild(vci_TidCridRelations *relPair, vcis_tidcrid_pair_list_t *appList, BlockNumber blockNumber);
static void UpdateTidCridForBuild(vci_RosCommandContext *comContext);
static void vci_build_callback(Relation rel, ItemPointer tid, Datum *values, bool *isnull, bool tupleIsAlive, void *state);
static void FinalizeBuild(vci_RosCommandContext *comContext);
static double GetEstimatedNumRows(Oid relid);
static void RemoveWosEntries(vci_RosCommandContext *comContext, WosKind wos_kind);
static uint64 cleanUpWos(vci_RosCommandContext *comContext, vci_MainRelVar wosType);
static uint64 UpdateDelVec(vci_RosCommandContext *comContext, Size workareaSize, uint64 numRowsAtOnce);
static void writeNumDeleteRowsIntoExntetInfo(vci_MainRelHeaderInfo *info, int32 topExtentId, uint32 numExtents, uint32 *numDeletedRows);
static vci_target_extent_info_t CountExtents(Relation mainRel, uint32 threshold, CEKind kind);
static HeapTuple getTupleFromVector(int offset, TupleDesc tupleDesc, vci_virtual_tuples_t *vecSet);
static void FillOneRosChunkBufferFromExtent(vci_RosCommandContext *comContext, int32 extentId, uint32 *rowIdInExtent);
static bool isCdrTargetExtentValid(vci_RosCommandContext *comContext);
static int32 CollectDeletedRows(vci_RosCommandContext *comContext, Snapshot snapshot);
static uint32 SearchUnusedExtent(vci_MainRelHeaderInfo *info);
static void CollectUnusedExtent(vci_RosCommandContext *comContext);
static void UpdateTidCrid(vci_RosCommandContext *comContext, Size workareaSize);
static void collectBlockNumberToMove(vci_RosCommandContext *comContext, int numPages);
static void freezeMainAndRos(vci_RosCommandContext *comContext);
static void freezeWos(vci_RosCommandContext *comContext, vci_MainRelVar wosType, Snapshot snapshot);
static void truncateRos(vci_RosCommandContext *comContext);
static void truncateWos(vci_RosCommandContext *comContext);
static void constructTidArray(vci_RosCommandContext *comContext, int max_data_wos_entries, int max_whiteout_wos_entries);
static int	comparator_orig_tid_xid64(const void *pa, const void *pb);
static bool can_select_candidate_for_wos2ros_conv(vci_tid_tid_xid64_t *data_wos_item, vci_RosCommandContext *comContext, ItemPointer last_whiteout_orig_tid);
static bool can_select_candidate_for_update_delvec(vci_tid_tid_xid64_t *whiteout_wos_item, vci_RosCommandContext *comContext);
static void put_entry_into_tid_list(vci_RosCommandContext *comContext, WosKind wos_kind, ItemPointer orig_tid, ItemPointer wos_tid);
static bool get_entry_into_tid_list(vci_RosCommandContext *comContext, WosKind wos_kind, ItemPointer orig_tid, ItemPointer wos_tid);
static int	readTidListFromWosIntoTidArray(Oid wos_od, WosKind wos_kind, vci_tid_tid_xid64_t *wos_entris, int max_wos_entries, Snapshot snapshot);
static void constructTidSortState(vci_RosCommandContext *comContext);
static void readTidListFromWosIntoTidSortState(Oid wos_oid, WosKind wos_kind, TupleTableSlot *slot, Tuplesortstate *sortstate, Snapshot snapshot, TransactionId wosros_xid);
static bool getValidTidSortState(Tuplesortstate *sortstate, TupleTableSlot *slot, vci_tid_tid_xid64_t *item);
static int32 compareXid64(int64 data_wos_xid64, int64 whiteout_wos_xid64);

/*
 * WOS -> ROS conversion
 * We have two situations of WOS -> ROS conversion.
 * 1. conversion process to reduce WOS and move data into ROS.
 *    In this case, all columns registered to the VCI are converted into
 *    ROS style and stored each relation.  The column meta data relations
 *    are also updated.  We normally convert one full extent at a time.
 *    The precise description is,
 *    A. take an exclusive lock to the main relation header.
 *    B. recover ROS if broken.
 *    C. scan WOS with care of freeze condition and deleted condition
 *       and collect live TID, up to 256 K rows.
 *    D. sort TID.
 *    E. write conversion information into VCI main relation header and
 *       extent info.
 *    F. collect target tuples and build ROS data.  Here we have chunk
 *       the data, since the work area might be limited.
 *    G. Find extent and free spaces to write the data.
 *    H. Write meta data.
 *    I. Write extent.
 *    J. Finalize meta data and VCI main relation.
 *    K. release the main relation header.
 *    For this purpose, we need VCI main relation, size of workarea.
 *
 * 2. local ROS conversion.
 *    In this case, given columns are converted into ROS style and stored
 *    in memory.  All the visible data are converted.
 *    The precise description is,
 *    A. scan WOS with care of visibility and deleted condition and collect
 *       visible TID.
 *    B. sort TID.
 *    C. take an exclusive lock to the main relation header.
 *    D. recover ROS if broken.
 *    E. collect target tuples and build local ROS data.
 *    F. release the main relation header.
 *    For this purpose, we need VCI main relation, size of area to store,
 *    necessary column ID list.
 *
 */

/* -------------------------------------------------------------- */

#define PERIOD_TO_CHECK_TRANSACTION_END	   (INT64CONST(1000))	/* 1 ms */
#define DURATION_TO_CHECK_TRANSACTION_END  (100000) /* 100 s */

/*
 * Copy from vacuumlazy.c
 */
#define VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL     20	/* ms */
#define VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL      50	/* ms */
#define VACUUM_TRUNCATE_LOCK_TIMEOUT            5000	/* ms */

/**
 * @brief This function is designed to detect transaction end after VCI
 * exclusive write lock is released.
 *
 * If the transaction of previous ROS command is not commited nor aborted,
 * wait for the end for time specified by the macro
 * We expect that normally ROS command is soon commited
 * DURATION_TO_CHECK_TRANSACTION_END (originally 100 seconds)
 * after the lock is released.
 * When the end is not detected, the function returns false,
 * otherwise true.
 *
 * @param[in] info Pointer to vci_MainRelHeaderInfo whose VCI index is
 * determined.
 * @retval true The transaction of the previous ROS command is detected
 * in a wait-time.
 * @retval false The transaction end is not detected.
 */
static bool
WaitTransactionEndOfLastRosCommand(vci_MainRelHeaderInfo *info)
{
	/*
	 * current ROS version is the transaction ID of last ROS command
	 */
	TransactionId curRosVer = vci_GetMainRelVar(info, vcimrv_current_ros_version, 0);
	int			checkCount;

	if (!TransactionIdIsValid(curRosVer))
		return true;

	if (TransactionIdIsCurrentTransactionId(curRosVer))
		return true;

	for (checkCount = 0;
		 (checkCount < DURATION_TO_CHECK_TRANSACTION_END) &&
		 (!ConditionalXactLockTableWait(curRosVer, false));
		 ++checkCount)
	{
		pg_usleep(PERIOD_TO_CHECK_TRANSACTION_END); /* wait 1 ms */
	}

	return checkCount < DURATION_TO_CHECK_TRANSACTION_END;
}

/**
 * @brief This function determine the result of the transaction status
 * of the previous ROS command.
 *
 * First, it waits the end of the transaction of the previous if necessary.
 * When it is committed successfully, just update current ROS version.
 * Otherwise, tries to recover VCI relations.
 *
 * @param[in] info Pointer to vci_MainRelHeaderInfo whose VCI index is
 * determined.
 *
 * @note Assuming that this function is called under main relation is locked
 * exclusively.
 */
void
vci_RecoverOneVCIIfNecessary(vci_MainRelHeaderInfo *info)
{
	TransactionId curRosVer;
	TransactionId lastRosVer;
	vci_ros_command_t commandSave = info->command;

	Assert(info);

	vci_ChangeCommand(info, vci_rc_recovery);

	/*
	 * Since the transaction is commited or abort after the lock is released,
	 * we have to wait for it.
	 */
	if (!WaitTransactionEndOfLastRosCommand(info))
		elog(ERROR, "unterminated ROS command");

	curRosVer = vci_GetMainRelVar(info, vcimrv_current_ros_version, 0);
	lastRosVer = vci_GetMainRelVar(info, vcimrv_last_ros_version, 0);

	if (!TransactionIdEquals(curRosVer, lastRosVer))
	{
		switch (vci_transaction_get_type(curRosVer))
		{
			case VCI_XACT_SELF:
				/* The last ROS version has been already updated */
				break;

			case VCI_XACT_IN_PROGRESS:
				elog(PANIC, "internal error. multiple ROS command running");
				break;

			case VCI_XACT_DID_COMMIT:
				/* update last ROS version and others */
				vci_UpdateLastRosVersionAndOthers(info);
				break;

			case VCI_XACT_DID_ABORT:
			case VCI_XACT_DID_CRASH:
				{
					vci_ros_command_t command;

					command = vci_GetMainRelVar(info, vcimrv_ros_command, 0);

					elog(DEBUG1, "crash recovery: previous command=\"%s\"(%d)",
						 vci_GetRosCommandName(command), command);

					switch (command)
					{
						case vci_rc_update_del_vec:
							vci_RecoveryUpdateDelVec(info);
							break;

						case vci_rc_wos_ros_conv:
						case vci_rc_collect_deleted:
						case vci_rc_collect_extent:
							vci_RecoveryExtentInfo(info, command);
							vci_RecoveryFreeSpace(info, command);
							break;

						case vci_rc_update_tid_crid:
							vci_RecoveryTidCrid(info);
							vci_RecoveryFreeSpaceForTidCrid(info);
							break;

						default:
							elog(PANIC, "last recorded ros command is fatally broken.");
							break;
					}

					vci_RecoveryDone(info);
				}
				break;

			case VCI_XACT_INVALID:
				elog(PANIC, "should not reach here");
				break;
		}
	}

	vci_ChangeCommand(info, commandSave);
}

static void
fillTidListFromTidSortState(vci_RosCommandContext *comContext, int numRows)
{
	int			count = 0;

	Assert(numRows <= VCI_NUM_ROWS_IN_EXTENT);

	for (int i = 0; i < numRows; i++)
	{
		Assert(count < comContext->wos2ros_array.max);

		if (!get_entry_into_tid_list(comContext, WOS_Data,
									 &comContext->wos2ros_array.orig_tids[i],
									 &comContext->wos2ros_array.wos_tids[i]))
			break;

		count++;
	}

	comContext->wos2ros_array.num = count;
	comContext->numRowsToConvert = count;
}

static int
ConvertWos2Ros(vci_RosCommandContext *comContext)
{
	int			result = 0;

	if (comContext->numRowsToConvert < 1)
	{
		elog(DEBUG2, "stop WOS to ROS conversion numRowsToConvert = %d", comContext->numRowsToConvert);
		return 0;
	}

	elog(DEBUG2, "start to convert WOS to ROS");

	/* obtain target extent ID */
	/* comContext->extentId = vci_GetFreeExtentId(&(comContext->info)); */
	elog(DEBUG2,
		 "WOS -> ROS conversion: index: %s  extent ID: " INT64_FORMAT,
		 RelationGetRelationName(comContext->info.rel),
		 (int64) comContext->extentId);

	/*
	 * Set WOS->ROS conversion data and write main relation for recovery.
	 * Header and extent info. Here, we also put current ROS version to the
	 * actual current transaction ID.
	 */
	vci_WriteExtentInfoInMainRosForWosRosConvInit(&(comContext->info),
												  comContext->extentId,
												  comContext->xid);

	vci_ResetRosChunkStorage(&(comContext->storage));
	vci_ResetRosChunkBufferCounter(&(comContext->buffer));

	/* read data for one extent */
	ReadOneExtentAndStoreInChunkStorage(comContext);

	/* write one extent into ROS */
	vci_AddTidCridUpdateList(&(comContext->info),
							 &(comContext->storage),
							 comContext->extentId);
	vci_WriteOneExtent(&(comContext->info),
					   &(comContext->storage),
					   comContext->extentId,
					   comContext->xid,
					   InvalidTransactionId,
					   comContext->xid);

	result = comContext->storage.numTotalRows;

	elog(DEBUG2, "converted %d rows into ROS", result);

	return result;
}

static void
FillValuesColumnwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
										RosChunkStorage *rosChunkStorage)
{
	for (int16 columnId = 0; columnId < vTuples->num_columns; ++columnId)
	{
		switch (vTuples->column_info[columnId].comp_type)
		{
			case vcis_compression_type_fixed_raw:
				vci_FillFixedWidthColumnarFromRosChunkStorage(vTuples,
															  columnId, rosChunkStorage);
				break;
			case vcis_compression_type_variable_raw:
				vci_FillVariableWidthColumnarFromRosChunkStorage(vTuples,
																 columnId, rosChunkStorage);
				break;
			default:
				Assert(false);
				elog(ERROR, "internal error: unsupported compression type");
		}
	}
}

static void
FillIsNullColumnwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
										RosChunkStorage *rosChunkStorage)
{
	const int16 strideR = 64;
	int			baseOffset = 0;
	int16	   *nullableColumnId = vci_GetNullableColumnIds(vTuples);

	if (vTuples->num_columns < 1)
		return;

	Assert(0 < rosChunkStorage->numFilled);
	Assert(vTuples->num_columns <= rosChunkStorage->chunk[0]->numColumns);
	Assert(vTuples->fetch_context->query_context->num_nullable_columns <= rosChunkStorage->chunk[0]->numNullableColumns);
	Assert(rosChunkStorage->numTotalRows <= vTuples->num_rows_in_extent);

	MemSet(vTuples->isnull, 0, vTuples->num_columns * vTuples->num_rows_in_extent);

	for (int sId = 0; sId < rosChunkStorage->numFilled; ++sId)
	{
		RosChunkBuffer *chunk = rosChunkStorage->chunk[sId];

		for (int rId = 0; rId < chunk->numFilled; rId += strideR)
		{
			int			pIdMax = Min(rId + strideR, chunk->numFilled);

			for (int bitId = 0; bitId < chunk->numNullableColumns; ++bitId)
			{
				int			colId = nullableColumnId[bitId];

				if (VCI_FIRST_NORMALCOLUMN_ID <= colId)
				{
					uint8	   *dst = (uint8 *) &(vTuples->isnull[(vTuples->num_rows_in_extent * colId) + baseOffset]);

					for (int pId = rId; pId < pIdMax; ++pId)
						dst[pId] = vci_GetBit((uint8 *) &(chunk->nullData[chunk->nullWidthInByte * pId]), bitId);
				}
			}
		}
		baseOffset += chunk->numFilled;
	}
	Assert(rosChunkStorage->numTotalRows == baseOffset);
}

static void
FillIsNullRowwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
									 RosChunkStorage *rosChunkStorage)
{
	abort();
}

static void
FillValuesRowwiseFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
									 RosChunkStorage *rosChunkStorage)
{
	abort();
}

static void
AppendDataToLocalRos(vci_local_ros_t *localRos,
					 RosChunkStorage *storage,
					 vci_MainRelHeaderInfo *info)
{
	MemoryContext oldMemCtx;
	struct vci_virtual_tuples *vTuples;
	int32		extentId;

	oldMemCtx = MemoryContextSwitchTo(localRos->memory_context);

	++(localRos->num_local_extents);
	extentId = -(localRos->num_local_extents);

	localRos->extent = vci_repalloc(localRos->extent,
									sizeof(vci_virtual_tuples_t *) *
									localRos->num_local_extents);
	vTuples = vci_CSCreateVirtualTuplesWithNumRows(localRos->fetch_context,
												   storage->numTotalRows);
	localRos->extent[localRos->num_local_extents - 1] = vTuples;

	/*
	 * Originally, localRos->size_vector_memory_context has the total size of
	 * vector sets.  The third parameter of vci_CSInitializeVectorSet() is the
	 * size for one vector set. Normally, we give up when many data are stored
	 * in ROS.  So, we can fix the maximum number of extents.
	 */

	vTuples->num_rows = storage->numTotalRows;
	vTuples->extent_id = extentId;
	vTuples->num_rows_in_extent = storage->numTotalRows;
	vTuples->row_id_in_extent = 0;
	vTuples->status = vcirvs_read_whole;

	if (vTuples->crid)
		vci_FillCridInVirtualTuples(vTuples);

	MemSet(vTuples->skip, 0, sizeof(uint16) * vTuples->num_rows_in_extent);

	if (vTuples->tid)
		vci_FillFixedWidthColumnarFromRosChunkStorage(vTuples, VCI_COLUMN_ID_TID, storage);

	if (vTuples->use_column_store)
	{
		FillIsNullColumnwiseFromRosChunkStorage(vTuples, storage);
		FillValuesColumnwiseFromRosChunkStorage(vTuples, storage);
	}
	else
	{
		FillIsNullRowwiseFromRosChunkStorage(vTuples, storage);
		FillValuesRowwiseFromRosChunkStorage(vTuples, storage);
	}

	MemoryContextSwitchTo(oldMemCtx);
}

static Size
ConvertWos2LocalRos(vci_RosCommandContext *comContext)
{
	Size		result = 0;

	if (comContext->numRowsToConvert < 1)
		return 0;

	elog(DEBUG2, "start to generate local ROS");

	for (comContext->extentId = -1; (!comContext->done);
		 comContext->extentId -= 1)
	{
		elog(DEBUG3,
			 "WOS -> local ROS conversion: index: %s  extent ID:%d\n",
			 RelationGetRelationName(comContext->info.rel),
			 comContext->extentId);

		vci_ResetRosChunkStorage(&(comContext->storage));
		vci_ResetRosChunkBufferCounter(&(comContext->buffer));

		/* read data for one extent */
		ReadOneExtentAndStoreInChunkStorage(comContext);

		/* write one extent into ROS */
		if (0 < comContext->storage.numTotalRows)
			AppendDataToLocalRos(comContext->local_ros,
								 &(comContext->storage),
								 &(comContext->info));

		result += comContext->storage.numTotalRows;
		elog(DEBUG2, "converted %llu rows into local ROS",
			 (unsigned long long) result);
	}

	return result;
}

/* **************************************
 * ** CAUTION: AttrNumber is 1 origin. **
 * **************************************
 */
/**
 * assuming when tIdList != NULL, TID list in tIdList to be read.
 *     not sequential scan, so scan is NULL.
 *     when tIdList == NULL, scan != NULL, sequential scan.
 *
 * @retval true   some data remain
 * @retval false  no data remain
 */
static void
FillOneRosChunkBuffer(vci_RosCommandContext *comContext,
					  int rowId,
					  int numRowsToConvert)
{
	TupleDesc	tupleDesc = RelationGetDescr(comContext->heapRel);
	Snapshot	snapshot = GetActiveSnapshot();

	if (comContext->wos2ros_array.max > 0)
	{
		uint32		sel PG_USED_FOR_ASSERTS_ONLY;
		vci_ros_command_t command = comContext->command;

#ifdef USE_ASSERT_CHECKING
		vci_TidCridUpdateListContext *oldListContext = NULL;
#endif

		if ((command == vci_rc_wos_ros_conv) ||
			(command == vci_rc_collect_deleted))
		{
			sel = vci_GetMainRelVar(&comContext->info, vcimrv_tid_crid_diff_sel, 0);

#ifdef USE_ASSERT_CHECKING
			oldListContext = vci_OpenTidCridUpdateList(&comContext->info, sel);
#endif
		}
		else if (command == vci_rc_generate_local_ros)
		{
			sel = comContext->local_ros->fetch_context->query_context->tid_crid_diff_sel;
		}

		for (int offset = 0; offset < numRowsToConvert; ++offset)
		{
			HeapTupleData tuple;
			Buffer		buffer;
			int			actualOffset = rowId + comContext->wos2ros_array.offset + offset;

			if (comContext->wos2ros_array.num <= actualOffset)
			{
				comContext->done = true;
				break;
			}

			CHECK_FOR_INTERRUPTS();

			tuple.t_self = comContext->wos2ros_array.orig_tids[actualOffset];

			if (!heap_fetch(comContext->heapRel, snapshot, &tuple, &buffer, true))
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("vci index \"%s\" corrupted", RelationGetRelationName(comContext->info.rel)),
						 errdetail("TID (%d,%d) has been deleted from table \"%s\"",
								   ItemPointerGetBlockNumber(&tuple.t_self),
								   ItemPointerGetOffsetNumber(&tuple.t_self),
								   RelationGetRelationName(comContext->heapRel)),
						 errhint("Use DROP INDEX \"%s\"", RelationGetRelationName(comContext->info.rel))));
			}

#ifdef USE_ASSERT_CHECKING
			if (oldListContext)
			{
				uint64		cridUint = vci_GetCridFromTid(oldListContext, &tuple.t_self, NULL);

				if (cridUint != VCI_INVALID_CRID)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("vci index \"%s\" corrupted", RelationGetRelationName(comContext->info.rel)),
							 errdetail("try to insert TID (%d,%d) into ROS twice: extentId=%d, index=%d",
									   ItemPointerGetBlockNumber(&tuple.t_self),
									   ItemPointerGetOffsetNumber(&tuple.t_self),
									   vci_CalcExtentIdFromCrid64(cridUint),
									   vci_CalcRowIdInExtentFromCrid64(cridUint)),
							 errhint("Use DROP INDEX \"%s\"", RelationGetRelationName(comContext->info.rel))));
			}
#endif

			vci_FillOneRowInRosChunkBuffer(&(comContext->buffer),
										   &(comContext->info),
										   &tuple.t_self,
										   &tuple,
										   comContext->indxColumnIdList,
										   comContext->heapAttrNumList,
										   tupleDesc);

			if (comContext->data_wos_del_list)
			{
				tuplesort_putdatum(comContext->data_wos_del_list,
								   ItemPointerGetDatum(&comContext->wos2ros_array.wos_tids[actualOffset]), false);
			}

			ReleaseBuffer(buffer);
		}

#ifdef USE_ASSERT_CHECKING
		if (oldListContext)
			vci_CloseTidCridUpdateList(oldListContext);
#endif
	}
}

static void
ReadOneExtentAndStoreInChunkStorage(vci_RosCommandContext *comContext)
{
	/* collect data for one extent */
	for (Size rowId = 0;
		 rowId < comContext->numRowsToConvert;
		 rowId += comContext->numRowsAtOnce)
	{
		/* the number of rows in one chunk */
		int			numRowsToConvert = comContext->numRowsToConvert - rowId;

		if (comContext->numRowsAtOnce - comContext->buffer.numFilled < numRowsToConvert)
			numRowsToConvert = comContext->numRowsAtOnce - comContext->buffer.numFilled;

		CHECK_FOR_INTERRUPTS();

		/* fetch the data from original relation */
		FillOneRosChunkBuffer(comContext, rowId, numRowsToConvert);
		if (0 < comContext->buffer.numFilled)
		{
			/* copy chunk buffer in a compact manner */
			vci_RegisterChunkBuffer(&(comContext->storage), &(comContext->buffer));
			vci_ResetRosChunkBufferCounter(&(comContext->buffer));
		}
	}

	comContext->wos2ros_array.offset += comContext->numRowsToConvert;
}

static Size
ConvertWhiteOut2LocalDeleteList(vci_RosCommandContext *comContext,
								int sel)
{
	vci_local_delete_list *list = &(comContext->local_ros->local_delete_list);
	vci_TidCridUpdateListContext *tidCridListContext;

	Assert(list);
	Assert(list->num_entry < list->length);

	tidCridListContext = vci_OpenTidCridUpdateList(&comContext->info, sel);

	for (int cId = 0; cId < comContext->delvec_array.num; cId++)
	{
		ItemPointerData orig_tid;
		uint64		crid;

		orig_tid = comContext->delvec_array.orig_tids[cId];

		crid = vci_GetCridFromTid(tidCridListContext, &orig_tid, NULL);

		if (crid == VCI_INVALID_CRID)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("vci index \"%s\" corrupted", RelationGetRelationName(comContext->info.rel)),
					 errdetail("try to delete TID (%d,%d) into local delete list",
							   ItemPointerGetBlockNumber(&orig_tid),
							   ItemPointerGetOffsetNumber(&orig_tid)),
					 errhint("Use DROP INDEX \"%s\"", RelationGetRelationName(comContext->info.rel))));

		list->crid_list[list->num_entry] = crid;
		list->num_entry++;
	}

	vci_CloseTidCridUpdateList(tidCridListContext);

	return list->num_entry;
}

static bool
NeedMainRelHeaderUpdate(vci_ros_command_t command)
{
	switch (command)
	{
		case vci_rc_recovery:
		case vci_rc_wos_ros_conv:
		case vci_rc_update_del_vec:
		case vci_rc_collect_deleted:
			/* case vci_rc_compaction: */
		case vci_rc_update_tid_crid:
		case vci_rc_collect_extent:
		case vci_rc_copy_command:
		case vci_rc_wos_ros_conv_build:

			return true;

		case vci_rc_wos_delete:
		case vci_rc_wos_insert:
		case vci_rc_probe:
		case vci_rc_query:
		case vci_rc_generate_local_ros:
		case vci_rc_drop_index:
		case vci_rc_vacuum:

			return false;

		default:
			Assert(false);
			elog(ERROR, "internal error: unexpected ROS command");
	}

	return false;
}

void
vci_ReleaseMainRelInCommandContext(vci_RosCommandContext *comContext)
{
	/* release the main relation */
	vci_ReleaseMainRelHeader(&(comContext->info));
}

void
vci_CloseHeapRelInCommandContext(vci_RosCommandContext *comContext)
{
	if (RelationIsValid(comContext->heapRel))
		table_close(comContext->heapRel, AccessShareLock);
	comContext->heapRel = NULL;
}

static int
CmpUint64(const void *pa, const void *pb)
{
	uint64		a = *(uint64 *) pa;
	uint64		b = *(uint64 *) pb;

	return (a < b) ? -1 : ((b < a) ? 1 : 0);
}

void
vci_InitRosCommandContext0(vci_RosCommandContext *context,
						   Relation rel, vci_ros_command_t command)
{
	Assert(context);

	MemSet(context, 0, sizeof(*context));

	context->command = command;
	context->indexOid = RelationGetRelid(rel);

	vci_InitMainRelHeaderInfo(&(context->info), rel, command);
	vci_KeepMainRelHeader(&(context->info));
}

void
vci_InitRosCommandContext1(vci_RosCommandContext *comContext,
						   Size workareaSize,
						   int numInsertRows,
						   int numDeleteRows,
						   bool readOriginalData)
{
	Size		worstCaseTupleSize;
	int			numColumns;

	Assert(comContext);

	comContext->xid = ((vci_rc_query == comContext->command) ||
					   (vci_rc_generate_local_ros == comContext->command)) ?
		InvalidTransactionId : GetCurrentTransactionId();

	comContext->heapOid = IndexGetRelation(comContext->info.rel->rd_id, false);

	comContext->local_ros = NULL;
	comContext->done = false;

	switch (comContext->command)
	{
		case vci_rc_generate_local_ros:
			comContext->wos2ros_array.orig_tids = palloc_array(ItemPointerData, numInsertRows);
			comContext->wos2ros_array.max = numInsertRows;
			comContext->delvec_array.orig_tids = palloc_array(ItemPointerData, numDeleteRows);
			comContext->delvec_array.max = numDeleteRows;
			break;

		case vci_rc_wos_ros_conv:
		case vci_rc_collect_deleted:
			comContext->wos2ros_array.orig_tids = palloc_array(ItemPointerData, numInsertRows);
			comContext->wos2ros_array.wos_tids = palloc_array(ItemPointerData, numInsertRows);
			comContext->wos2ros_array.max = numInsertRows;
			break;

		default:
			break;
	}

	comContext->numRowsToConvert = Min(Max(numInsertRows, numDeleteRows), VCI_NUM_ROWS_IN_EXTENT);

	/*
	 * Column sizes
	 */
	numColumns = vci_GetMainRelVar(&(comContext->info), vcimrv_num_columns, 0);

	/*
	 * get column size in worst case and column ID lists for both original
	 * relation and VCI relation
	 */
	comContext->numColumns = numColumns;

	if (readOriginalData)
	{
		Size		allocatableSize = Min(workareaSize, MaxAllocSize);
		int			numRowsAtOnce;
		int			largestTupleSize;

		comContext->heapAttrNumList = palloc_array(AttrNumber, numColumns);
		comContext->indxColumnIdList = palloc_array(int16, numColumns);
		comContext->columnSizeList = palloc_array(int16, numColumns);
		worstCaseTupleSize = vci_GetColumnIdsAndSizes(
													  comContext->heapAttrNumList,
													  comContext->indxColumnIdList,
													  comContext->columnSizeList,
													  numColumns,
													  &(comContext->info),
													  comContext->heapOid);

		comContext->heapRel = table_open(comContext->heapOid, AccessShareLock);

		/*
		 * PostgreSQL limits the tuple size by TOAST_TUPLE_TARGET, normally.
		 * The upper limit of the tuple size is smaller than BLCKSZ. We use
		 * other area to keep the offset or data size in the chunk buffers or
		 * ROS. Here, we assume the type of offset is uint32.
		 */
		largestTupleSize = worstCaseTupleSize +
			(comContext->numColumns * sizeof(uint32));

		/* The number of rows in one chunk */
		numRowsAtOnce = (int) (allocatableSize * VCI_WOS_ROS_WORKAREA_SAFE_RATIO /
							   largestTupleSize);
		numRowsAtOnce = (numRowsAtOnce / VCI_COMPACTION_UNIT_ROW) * VCI_COMPACTION_UNIT_ROW;
		numRowsAtOnce = Max(numRowsAtOnce, VCI_COMPACTION_UNIT_ROW);
		numRowsAtOnce = Min(numRowsAtOnce, VCI_NUM_ROWS_IN_EXTENT);

		comContext->numRowsAtOnce = numRowsAtOnce;
	}
	else
	{
		comContext->heapAttrNumList = NULL;
		comContext->indxColumnIdList = NULL;
		comContext->columnSizeList = NULL;
		comContext->heapRel = NULL;
		comContext->numRowsAtOnce = VCI_COMPACTION_UNIT_ROW;
	}

	comContext->scan = NULL;

	switch (comContext->command)
	{
		case vci_rc_wos_ros_conv:
		case vci_rc_collect_deleted:
		case vci_rc_update_del_vec:
		case vci_rc_vacuum:
			comContext->oldestXmin = GetOldestNonRemovableTransactionId(comContext->info.rel);
			comContext->wos2rosXid = comContext->oldestXmin;
			break;

		case vci_rc_generate_local_ros:
		default:
			comContext->oldestXmin = InvalidTransactionId;
			comContext->wos2rosXid = InvalidTransactionId;
			break;
	}

}

void
vci_InitRosCommandContext2(vci_RosCommandContext *comContext, Size workareaSize)
{
	bool		make_wos2ros_tid_list = false;
	bool		make_delvec_tid_list = false;

	comContext->data_wos_del_list =
		tuplesort_begin_datum(TIDOID, TIDLessOperator, InvalidOid, false,
							  Min(workareaSize / 1024 / 3, INT_MAX), NULL, TUPLESORT_NONE);
	comContext->whiteout_wos_del_list =
		tuplesort_begin_datum(TIDOID, TIDLessOperator, InvalidOid, false,
							  Min(workareaSize / 1024 / 3, INT_MAX), NULL, TUPLESORT_NONE);

	switch (comContext->command)
	{
		case vci_rc_wos_ros_conv:
			make_wos2ros_tid_list = true;
			break;

		case vci_rc_collect_deleted:
			make_wos2ros_tid_list = true;
			break;

		case vci_rc_update_del_vec:
			make_delvec_tid_list = true;
			break;

		default:
			break;
	}

	if (make_wos2ros_tid_list || make_delvec_tid_list)
	{
		TupleDesc	tupDesc;
		AttrNumber	sortKeys[] = {1};
		Oid			sortOperators[] = {TIDLessOperator};
		Oid			sortCollations[] = {InvalidOid};
		bool		nullsFirstFlags[] = {false};

		tupDesc = CreateTemplateTupleDesc(2);

		TupleDescInitEntry(tupDesc, (AttrNumber) 1, "orig_tid", TIDOID, -1, 0);
		TupleDescInitEntry(tupDesc, (AttrNumber) 2, "wos_tid", TIDOID, -1, 0);

		comContext->tid_tid_tupdesc = tupDesc;
		comContext->tid_tid_slot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsHeapTuple);

		if (make_wos2ros_tid_list)
		{
			comContext->wos2ros_tid_list =
				tuplesort_begin_heap(tupDesc, 1,
									 sortKeys, sortOperators, sortCollations, nullsFirstFlags,
									 Min(workareaSize / 1024 / 3, INT_MAX), NULL, TUPLESORT_NONE);
		}

		if (make_delvec_tid_list)
		{
			comContext->delvec_tid_list =
				tuplesort_begin_heap(tupDesc, 1,
									 sortKeys, sortOperators, sortCollations, nullsFirstFlags,
									 Min(workareaSize / 1024 / 3, INT_MAX), NULL, TUPLESORT_NONE);
		}
	}
}

void
vci_InitRosChunkStroageAndBuffer(vci_RosCommandContext *comContext, bool forAppending)
{
	int			numRowsAtOnce;

	Assert(RelationIsValid(comContext->heapRel));

	numRowsAtOnce = comContext->numRowsAtOnce;

	/* Initialize the buffers for building chunks of ROS data */
	vci_InitOneRosChunkBuffer(&(comContext->buffer),
							  numRowsAtOnce,
							  comContext->columnSizeList,
							  comContext->numColumns,
							  false,
							  &(comContext->info));

	vci_InitRosChunkStorage(&(comContext->storage), numRowsAtOnce, forAppending);
}

void
vci_CleanRosCommandContext(vci_RosCommandContext *comContext, bool neverWrite)
{
	if (comContext->tid_tid_slot)
	{
		ExecClearTuple(comContext->tid_tid_slot);
		pfree(comContext->tid_tid_slot);
		comContext->tid_tid_slot = NULL;
	}

	if (comContext->data_wos_del_list)
	{
		tuplesort_end(comContext->data_wos_del_list);
		comContext->data_wos_del_list = NULL;
	}

	if (comContext->whiteout_wos_del_list)
	{
		tuplesort_end(comContext->whiteout_wos_del_list);
		comContext->whiteout_wos_del_list = NULL;
	}

	if (comContext->wos2ros_tid_list)
	{
		tuplesort_end(comContext->wos2ros_tid_list);
		comContext->wos2ros_tid_list = NULL;
	}

	if (comContext->delvec_tid_list)
	{
		tuplesort_end(comContext->delvec_tid_list);
		comContext->delvec_tid_list = NULL;
	}

	if (comContext->tid_tid_tupdesc)
	{
		FreeTupleDesc(comContext->tid_tid_tupdesc);
		comContext->tid_tid_tupdesc = NULL;
	}

	/* Close original heap relation if it is opened. */
	vci_CloseHeapRelInCommandContext(comContext);

	/*
	 * Release chunk buffers - WOS ROS Conv.
	 */
	if (comContext->command == vci_rc_wos_ros_conv)
	{
		vci_DestroyOneRosChunkBuffer(&(comContext->buffer));
		vci_DestroyRosChunkStorage(&(comContext->storage));
	}

	if (NULL != comContext->heapAttrNumList)
	{
		/* release local work area */
		pfree(comContext->heapAttrNumList);
		pfree(comContext->indxColumnIdList);
		pfree(comContext->columnSizeList);
		comContext->heapAttrNumList = NULL;
		comContext->indxColumnIdList = NULL;
		comContext->columnSizeList = NULL;
	}

	/* release local work area */
	if (comContext->wos2ros_array.orig_tids)
	{
		pfree(comContext->wos2ros_array.orig_tids);
		comContext->wos2ros_array.orig_tids = NULL;
	}

	if (comContext->wos2ros_array.wos_tids)
	{
		pfree(comContext->wos2ros_array.wos_tids);
		comContext->wos2ros_array.wos_tids = NULL;
	}

	if (comContext->delvec_array.orig_tids)
	{
		pfree(comContext->delvec_array.orig_tids);
		comContext->delvec_array.orig_tids = NULL;
	}

	if (comContext->utility_array.orig_blknos)
	{
		pfree(comContext->utility_array.orig_blknos);
		comContext->utility_array.orig_blknos = NULL;
	}

	if (neverWrite)
		return;

	/* write header of the main relation */
	if (NeedMainRelHeaderUpdate(comContext->command))
		vci_WriteMainRelVar(&(comContext->info),
							vci_wmrv_update);
}

void
vci_FinRosCommandContext(vci_RosCommandContext *comContext, bool neverWrite)
{
	vci_CleanRosCommandContext(comContext, neverWrite);

	/* release the main relation */
	vci_ReleaseMainRelInCommandContext(comContext);

	comContext->indexOid = InvalidOid;
	comContext->command = vci_rc_invalid;
}

/**
 * numRows is from 1 to VCI_NUM_ROWS_IN_EXTENT
 * workareaSize should be taken from the configuration parameter
 * in postgresql.conf.
 * It just convert one extent.
 */
int
vci_ConvertWos2Ros(Relation mainRel, Size workareaSize, int numRows)
{
	vci_RosCommandContext comContext;
	MemoryContext memCtxWos2Ros;
	MemoryContext oldMemCtx;
	int			result = -1;

	Assert((0 < numRows) && (numRows <= VCI_NUM_ROWS_IN_EXTENT));

	vci_InitRosCommandContext0(&comContext, mainRel, vci_rc_wos_ros_conv);

	/* recover ROS if necessary */
	vci_RecoverOneVCIIfNecessary(&(comContext.info));

	/* prepare local work area */
	memCtxWos2Ros = AllocSetContextCreate(TopTransactionContext,
										  "WOS->ROS conversion",
										  ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(memCtxWos2Ros);

	vci_InitRosCommandContext1(&comContext,
							   workareaSize / 3 * 2,
							   numRows, 0,
							   true);

	vci_InitRosCommandContext2(&comContext, workareaSize / 3);

	if (TransactionIdPrecedes(GetCurrentTransactionId(),
							  (TransactionId) vci_GetMainRelVar(&comContext.info, vcimrv_current_ros_version, 0)))
		goto done;

	GetActiveSnapshot();

	/* obtain new extent ID */
	comContext.extentIdSrc = VCI_INVALID_EXTENT_ID;
	comContext.extentId = vci_GetFreeExtentId(&(comContext.info));

	/* Write Recovery Information of this command. */
	vci_WriteRecoveryRecordForExtentInfo(&comContext.info, comContext.extentId, comContext.extentIdSrc);
	vci_InitRecoveryRecordForFreeSpace(&comContext.info);

	vci_WriteRecoveryRecordDone(&comContext.info, comContext.command, comContext.xid);

	constructTidSortState(&comContext);

	/* call Main routine */
	fillTidListFromTidSortState(&comContext, numRows);

	vci_InitRosChunkStroageAndBuffer(&comContext, false /* no append */ );

	result = ConvertWos2Ros(&comContext);

	/* remove WOS entries */
	cleanUpWos(&comContext, vcimrv_data_wos_oid);
	cleanUpWos(&comContext, vcimrv_whiteout_wos_oid);

	/* Xmax WOS entry */
	RemoveWosEntries(&comContext, WOS_Data);
	RemoveWosEntries(&comContext, WOS_Whiteout);

done:
	/* Finalize ROS */
	vci_FinRosCommandContext(&comContext, false);

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(memCtxWos2Ros);

	return result;
}

static void
FlushTidCridPairListToTreeForBuild(vci_TidCridRelations *relPair,
								   vcis_tidcrid_pair_list_t *appList,
								   BlockNumber blockNumber)
{
	if (0 < appList->num)
	{
		ItemPointerData treeNode;

		vci_GetTidCridSubTree(relPair, blockNumber, &treeNode);
		if (!ItemPointerIsValid(&treeNode))
			vci_CreateTidCridSubTree(relPair, blockNumber, &treeNode);
		vci_UpdateTidCridSubTree(relPair, &treeNode, appList);
	}
	appList->num = 0;
}

static void
UpdateTidCridForBuild(vci_RosCommandContext *comContext)
{
	RosChunkStorage *src = &(comContext->storage);
	vci_TidCridRelations relPair;
	const LOCKMODE lockmode = ExclusiveLock;
	BlockNumber blockNumber = InvalidBlockNumber;
	int32		offset = offsetof(vcis_tidcrid_pair_list_t, body);
	int			rowIdInExt = 0;
	vcis_tidcrid_pair_list_t *appList = palloc(offset
											   + (sizeof(vcis_tidcrid_pair_item_t) * src->numTotalRows));

	vci_OpenTidCridRelations(&relPair, &comContext->info, lockmode);
	appList->num = 0;

	for (int chunkId = 0; chunkId < src->numFilled; ++chunkId)
	{
		for (int rowId = 0; rowId < src->chunk[chunkId]->numFilled; ++rowId)
		{
			ItemPointer itemPtr = (ItemPointer) &(src->chunk[chunkId]->
												  tidData[sizeof(ItemPointerData) * rowId]);

			if (blockNumber != ItemPointerGetBlockNumber(itemPtr))
			{
				if (BlockNumberIsValid(blockNumber))
					FlushTidCridPairListToTreeForBuild(&relPair, appList,
													   blockNumber);
				blockNumber = ItemPointerGetBlockNumber(itemPtr);
			}

			Assert(appList->num < src->numTotalRows);
			appList->body[appList->num].crid = vci_GetCridFromUint64(
																	 vci_CalcCrid64(comContext->extentId, rowIdInExt));
			ItemPointerCopy(itemPtr, &appList->body[appList->num].page_item_id);
			(appList->num)++;

			Assert(rowIdInExt < src->numTotalRows);
			rowIdInExt++;
		}
	}
	if (BlockNumberIsValid(blockNumber))
		FlushTidCridPairListToTreeForBuild(&relPair, appList, blockNumber);
	pfree(appList);
	vci_CloseTidCridRelations(&relPair, lockmode);
}

/* Implementation of callback interface:IndexBuildCallback */
static void
vci_build_callback(Relation rel,
				   ItemPointer tid,
				   Datum *values,
				   bool *isnull,
				   bool tupleIsAlive,
				   void *state)
{
	vci_RosCommandContext *comContext = (vci_RosCommandContext *) state;

	Assert(comContext);

	if (tupleIsAlive)
	{
		Assert((0 <= comContext->buffer.numFilled) &&
			   (comContext->buffer.numFilled < comContext->numRowsAtOnce));

		vci_FillOneRowInRosChunkBuffer(&(comContext->buffer),
									   &(comContext->info),
									   &IndexHeapTuple->t_self, /* use the original heap
																 * tuple saved in
																 * heapam_index_build_range_scan() */
									   IndexHeapTuple,	/* use the original heap
														 * tuple saved in
														 * heapam_index_build_range_scan() */
									   comContext->indxColumnIdList,
									   comContext->heapAttrNumList,
									   RelationGetDescr(comContext->heapRel));

		if (comContext->numRowsAtOnce <= comContext->buffer.numFilled)
		{
			vci_RegisterChunkBuffer(&(comContext->storage),
									&(comContext->buffer));
			vci_ResetRosChunkBufferCounter(&(comContext->buffer));
		}

		if (VCI_NUM_ROWS_IN_EXTENT <=
			(comContext->storage.numTotalRows + comContext->buffer.numFilled))
		{
			Assert(TransactionIdIsValid(comContext->xid));
			if (0 < comContext->buffer.numFilled)
			{
				vci_RegisterChunkBuffer(&(comContext->storage),
										&(comContext->buffer));
				vci_ResetRosChunkBufferCounter(&(comContext->buffer));
			}
			vci_WriteExtentInfoInMainRosForWosRosConvInit(&(comContext->info),
														  comContext->extentId,
														  comContext->xid);
			UpdateTidCridForBuild(comContext);
			vci_WriteOneExtent(&(comContext->info),
							   &(comContext->storage),
							   comContext->extentId,
							   comContext->xid,
							   InvalidTransactionId,
							   comContext->xid);
			vci_ResetRosChunkStorage(&(comContext->storage));
			comContext->extentId++;
		}
	}
}

static void
FinalizeBuild(vci_RosCommandContext *comContext)
{
	if (0 < comContext->buffer.numFilled)
		vci_RegisterChunkBuffer(&(comContext->storage),
								&(comContext->buffer));

	if (0 < comContext->storage.numTotalRows)
	{
		Assert(TransactionIdIsValid(comContext->xid));
		vci_WriteExtentInfoInMainRosForWosRosConvInit(&(comContext->info),
													  comContext->extentId,
													  comContext->xid);
		UpdateTidCridForBuild(comContext);
		vci_WriteOneExtent(&(comContext->info),
						   &(comContext->storage),
						   comContext->extentId,
						   comContext->xid,
						   InvalidTransactionId,
						   comContext->xid);
		comContext->extentId++;
	}
}

/**
 * @brief Obtain number of rows in the relation estimated by ANALYZE or
 * VACUUM commands.
 *
 * @param[in] relid The Oid of the relation.
 * @return The estimated number of rows.
 */
static double
GetEstimatedNumRows(Oid relid)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		double		result = Max(reltup->reltuples, 0);

		ReleaseSysCache(tp);

		return result;
	}
	else
		return 0.0;
}

/**
 * This function is assumed when the VCI index is newly built, and
 * it converts all the data in the relation of PostgreSQL into ROS.
 */
double
vci_ConvertWos2RosForBuild(Relation mainRel,
						   Size workareaSize,
						   IndexInfo *indexInfo)
{
	vci_RosCommandContext comContext;
	MemoryContext memCtxWos2Ros;
	MemoryContext oldMemCtx;
	double		result = 0;

	vci_InitRosCommandContext0(&comContext, mainRel,
							   vci_rc_wos_ros_conv_build);

	/* prepare local work area */
	memCtxWos2Ros = AllocSetContextCreate(TopTransactionContext,
										  "WOS->ROS conversion",
										  ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(memCtxWos2Ros);

	vci_InitRosCommandContext1(&comContext,
							   workareaSize,
							   VCI_NUM_ROWS_IN_EXTENT, 0,
							   true);

	vci_InitRosChunkStroageAndBuffer(&comContext, false /* no append */ );

	comContext.extentId = VCI_FIRST_NORMAL_EXTENT_ID;

	/*
	 * Initialize information for printing progress
	 */
	comContext.estimatedNumRows = GetEstimatedNumRows(
													  RelationGetRelid(comContext.heapRel));
	if (comContext.estimatedNumRows < 1)
		comContext.estimatedNumRows = 1;
	comContext.numConvertedRows = 0;
	strcpy(comContext.relName, RelationGetRelationName(mainRel));

	result = table_index_build_scan(comContext.heapRel,
									mainRel,
									indexInfo,
									true,	/* allow syncscan */
									true,
									vci_build_callback,
									(void *) &comContext, NULL);
	indexInfo->ii_BrokenHotChain = true;
	FinalizeBuild(&comContext);

	vci_FinRosCommandContext(&comContext, false);

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(memCtxWos2Ros);

	return result;
}

static void
RemoveWosEntries(vci_RosCommandContext *comContext, WosKind wos_kind)
{
	Datum		value;
	bool		isnull;
	Relation	rel;
	Oid			wos_oid;
	Tuplesortstate *sortstate = NULL;

	switch (wos_kind)
	{
		case WOS_Data:
			wos_oid = vci_GetMainRelVar(&comContext->info, vcimrv_data_wos_oid, 0);
			sortstate = comContext->data_wos_del_list;
			break;

		case WOS_Whiteout:
			wos_oid = vci_GetMainRelVar(&comContext->info, vcimrv_whiteout_wos_oid, 0);
			sortstate = comContext->whiteout_wos_del_list;
			break;
		default:
			wos_oid = InvalidOid;
			break;
	}

	tuplesort_performsort(sortstate);

	rel = relation_open(wos_oid, RowExclusiveLock);

	while (tuplesort_getdatum(sortstate, true, true, &value, &isnull, NULL))
	{
		ItemPointer tid;

		tid = DatumGetItemPointer(value);

		simple_heap_delete(rel, tid);
	}

	RelationSetTargetBlock(rel, InvalidBlockNumber);

	relation_close(rel, RowExclusiveLock);
}

static uint64
cleanUpWos(vci_RosCommandContext *comContext, vci_MainRelVar wosType)
{
	const LOCKMODE lockmode = ShareUpdateExclusiveLock;
	vci_MainRelHeaderInfo *info;
	BlockNumber nblocks;
	ItemPointer dead_tuples;
	int			max_dead_tuples;
	uint64		total_live = 0;

	HeapTupleData tuple;

	Oid			oidWosType;
	TransactionId oldestXmin;
	Relation	rel;

	info = &comContext->info;

	oldestXmin = comContext->oldestXmin;

	oidWosType = vci_GetMainRelVar(info, wosType, 0);

	rel = table_open(oidWosType, lockmode);

	max_dead_tuples = MaxHeapTuplesPerPage;
	dead_tuples = palloc0_array(ItemPointerData, max_dead_tuples);

	nblocks = RelationGetNumberOfBlocks(rel);
	for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
	{
		Size		freespace;
		int			num_dead_tuples = 0;
		TransactionId snapshotConflictHorizon = InvalidTransactionId;

		Buffer		buffer;
		Buffer		vmbuffer = InvalidBuffer;
		Page		page;
		OffsetNumber maxoff;

		OffsetNumber unused[MaxOffsetNumber];
		int			nunused = 0;
		bool		is_visible_page = true;

		/* Get a buffer containing the target block. */
		buffer = ReadBuffer(rel, blkno);
		page = BufferGetPage(buffer);

		if (!ConditionalLockBufferForCleanup(buffer))
		{
			ReleaseBuffer(buffer);
			continue;
		}

		/* Collect removable dead tuples in the target block. */
		maxoff = PageGetMaxOffsetNumber(page);
		for (OffsetNumber offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid = PageGetItemId(page, offnum);

			/* Unused items require no processing, but we count 'em */
			if (!ItemIdIsUsed(itemid))
				continue;

			/* Redirect items mustn't be touched */
			if (ItemIdIsRedirected(itemid))
				continue;

			ItemPointerSet(&(tuple.t_self), blkno, offnum);

			/*
			 * DEAD item pointers are to be vacuumed normally; but we don't
			 * count them in tups_vacuumed, else we'd be double-counting (at
			 * least in the common case where heap_page_prune() just freed up
			 * a non-HOT tuple).
			 */
			if (ItemIdIsDead(itemid))
			{
				dead_tuples[num_dead_tuples++] = tuple.t_self;
				continue;
			}

			Assert(ItemIdIsNormal(itemid));

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			tuple.t_tableOid = RelationGetRelid(rel);

			switch (HeapTupleSatisfiesVacuum(&tuple, oldestXmin, buffer))
			{
				case HEAPTUPLE_DEAD:
					dead_tuples[num_dead_tuples++] = tuple.t_self;
					HeapTupleHeaderAdvanceConflictHorizon(tuple.t_data,
														  &snapshotConflictHorizon);
					break;
				case HEAPTUPLE_LIVE:
					++total_live;
					break;
				case HEAPTUPLE_RECENTLY_DEAD:
				case HEAPTUPLE_INSERT_IN_PROGRESS:
				case HEAPTUPLE_DELETE_IN_PROGRESS:
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					break;
			}
		}

		if (num_dead_tuples == 0)
		{
			/*
			 * Skip repair of a fragmentation, because dead tuple is not
			 * exist.
			 */
			UnlockReleaseBuffer(buffer);
			continue;
		}

		visibilitymap_pin(rel, blkno, &vmbuffer);

		/*
		 * this routine is copied from lazy_vacuum_heap_rel() &
		 * lazy_vacuum_heap_page(), and modified.
		*/

		START_CRIT_SECTION();

		for (int tupindex = 0; tupindex < num_dead_tuples; tupindex++)
		{
			BlockNumber tblk;
			OffsetNumber toff;
			ItemId		itemid;

			HeapTupleHeader htup;

			tblk = ItemPointerGetBlockNumber(&dead_tuples[tupindex]);
			if (tblk != blkno)
				break;			/* past end of tuples for this block */
			toff = ItemPointerGetOffsetNumber(&dead_tuples[tupindex]);

			itemid = PageGetItemId(page, toff);
			if (!ItemIdHasStorage(itemid))
				continue;
			if (!ItemIdIsDead(itemid))
				continue;

			htup = (HeapTupleHeader) PageGetItem(page, itemid);
			dead_tuples[tupindex] = *(ItemPointer) ((char *) htup + htup->t_hoff);

			Assert(ItemIdIsDead(itemid) && !ItemIdHasStorage(itemid));
			ItemIdSetUnused(itemid);
			unused[nunused++] = toff;
		}

		/* Attempt to truncate line pointer array now */
		if (nunused > 0)
			PageTruncateLinePointerArray(page);


		/* Mark buffer dirty before we write WAL. */
		MarkBufferDirty(buffer);

		for (OffsetNumber offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid = PageGetItemId(page, offnum);

			if (ItemIdIsUsed(itemid))
			{
				is_visible_page = false;
				break;
			}
		}

		if (BufferIsValid(vmbuffer))
		{
			if (is_visible_page)
			{
				PageSetAllVisible(page);
				MarkBufferDirty(buffer);
				visibilitymap_set(rel, blkno, buffer, InvalidXLogRecPtr,
								  vmbuffer, InvalidTransactionId, VISIBILITYMAP_ALL_VISIBLE);
			}

			ReleaseBuffer(vmbuffer);
		}

		/* XLOG stuff */
		if (nunused > 0 && RelationNeedsWAL(rel))
		{
			/*
			 * Commit add323d added the vmbuffer/vmflags parameters.
			 * A quick fix was needed to allow build to proceed.
			 *
			 * TODO Confirm if passing InvalidBuffer, 0 is OK here.
			 */
			log_heap_prune_and_freeze(rel, buffer,
									  InvalidBuffer, /* vmbuffer */
									  0,			 /* vmflags */
									  snapshotConflictHorizon,
									  false,	/* no cleanup lock required */
									  PRUNE_ON_ACCESS,
									  NULL, 0,	/* frozen */
									  NULL, 0,	/* redirected */
									  NULL, 0,	/* dead */
									  unused, nunused);
		}

		END_CRIT_SECTION();

		freespace = PageGetHeapFreeSpace(page);

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);

		RecordPageWithFreeSpace(rel, blkno, freespace);

		/*
		 * in vci_UnregisterTIDFromTIDTree(), TidTree in memory will be
		 * rebuild when the size was too large to store in memory, and the
		 * size is reduced to fit to the memory size. At that time, data WOS
		 * will be scan to obtain TID list. So, vci_UnregisterTIDFromTIDTree()
		 * can not be called in the critical section above.
		 */
	}

	pfree(dead_tuples);
	table_close(rel, lockmode);

	return total_live;
}

/**
 * generate local ROS.
 * This function is assumed to be called in backend process, not parallel
 * background worker.  Here, vci_CSFetchContext is used unlocalized.
 */
vci_local_ros_t *
vci_GenerateLocalRos(vci_CSQueryContext queryContext,
					 Size workareaSize,
					 int64 numDataWosRows,
					 int64 numWhiteoutWosRows)
{
	vci_RosCommandContext comContext;
	int			numRowsInExtent;
	MemoryContext localMemCtx;
	MemoryContext sharedMemCtx;
	MemoryContext oldMemCtx;
	vci_local_ros_t *result;
	Size		partedWorkareaSize = workareaSize / 4;
	int64		numLocalDeleteListRows;

	numRowsInExtent = vci_GetNumRowsInLocalRosExtent(queryContext->num_columns);

	sharedMemCtx = AllocSetContextCreate(queryContext->shared_memory_context,
										 "Work for Local ROS generation",
										 ALLOCSET_DEFAULT_SIZES);

	result = MemoryContextAllocZero(sharedMemCtx, sizeof(vci_local_ros_t));
	result->num_local_extents = 0;
	result->extent = NULL;
	result->memory_context = sharedMemCtx;
	result->fetch_context = vci_CSCreateFetchContextBase(queryContext,
														 Min(numRowsInExtent, numDataWosRows),
														 queryContext->num_columns,
														 queryContext->attr_num,
														 true,
														 true,
														 true,
														 false);	/* no compression */

	numRowsInExtent = result->fetch_context->num_rows_read_at_once;

	Assert(queryContext == result->fetch_context->query_context);

	/*
	 * Local Delete List
	 */
	numLocalDeleteListRows = numDataWosRows + numWhiteoutWosRows;

	result->local_delete_list.crid_list =
		MemoryContextAllocZero(result->memory_context,
							   sizeof(*(result->local_delete_list.crid_list)) * numLocalDeleteListRows);
	result->local_delete_list.num_entry = 0;
	result->local_delete_list.length = numLocalDeleteListRows;

	Assert(0 == ((uintptr_t) (result->local_delete_list.crid_list) & (MAXIMUM_ALIGNOF - 1)));

	localMemCtx = AllocSetContextCreate(TopTransactionContext,
										"Work for Local ROS generation",
										ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(localMemCtx);

	vci_InitRosCommandContext0(&comContext, queryContext->info->rel,
							   vci_rc_generate_local_ros);
	vci_InitRosCommandContext1(&comContext,
							   partedWorkareaSize,
							   numDataWosRows, numWhiteoutWosRows,
							   true);

	vci_InitRosChunkStroageAndBuffer(&comContext, false /* no append */ );

	comContext.inclusiveXid = queryContext->inclusive_xid;
	comContext.exclusiveXid = queryContext->exclusive_xid;

	Assert(queryContext->num_data_wos_entries <= VCI_NUM_ROWS_IN_EXTENT * VCI_MAX_NUMBER_UNCONVERTED_ROS);
	Assert(queryContext->num_whiteout_wos_entries <= VCI_NUM_ROWS_IN_EXTENT * VCI_MAX_NUMBER_UNCONVERTED_ROS);

	constructTidArray(&comContext,
					  (int) queryContext->num_data_wos_entries,
					  (int) queryContext->num_whiteout_wos_entries);

	comContext.numRowsToConvert = Min(comContext.numRowsToConvert,
									  numRowsInExtent);
	comContext.local_ros = result;
	queryContext->local_ros = result;

	MemoryContextSwitchTo(sharedMemCtx);

	PG_TRY();
	{
		ConvertWos2LocalRos(&comContext);

		comContext.local_ros = result;

		ConvertWhiteOut2LocalDeleteList(&comContext,
										result->fetch_context->query_context->tid_crid_diff_sel);

		qsort(result->local_delete_list.crid_list,
			  result->local_delete_list.num_entry,
			  sizeof(uint64),
			  CmpUint64);

		queryContext->local_ros = result;
		queryContext->num_local_ros_extents = result->num_local_extents;
		queryContext->delete_list = comContext.local_ros->local_delete_list.crid_list;
		queryContext->num_delete = comContext.local_ros->local_delete_list.num_entry;
	}
	PG_CATCH();
	{
		if (geterrcode() == ERRCODE_OUT_OF_MEMORY)
		{
			vci_FinRosCommandContext(&comContext, true /* never write */ );

			MemoryContextSwitchTo(oldMemCtx);
			MemoryContextDelete(localMemCtx);
		}

		PG_RE_THROW();
	}
	PG_END_TRY();

	vci_FinRosCommandContext(&comContext, false);

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(localMemCtx);

	return result;
}

/**
 * in vci_DestroyLocalRos(), release the memory context allocated to the
 * local ros pointed by localRos.
 * We have not need to pfree() each element.
 */
void
vci_DestroyLocalRos(vci_local_ros_t *localRos)
{
	MemoryContext memCtx;

	Assert(localRos);
	memCtx = localRos->memory_context;
	MemoryContextDelete(memCtx);
}

uint32
vci_CountFreezedInDataWos(Relation mainRel, Size workareaSize)
{
	uint32		count = 0;
	vci_MainRelHeaderInfo infoData = {0};
	vci_MainRelHeaderInfo *info = &infoData;

	Oid			dataWosOid;
	Relation	dataWosRel;

	TableScanDesc scan;
	HeapTuple	tuple;
	Snapshot	snapshot;

	vci_InitMainRelHeaderInfo(info, mainRel, vci_rc_probe);
	vci_KeepMainRelHeader(info);

	dataWosOid = (Oid) vci_GetMainRelVar(info, vcimrv_data_wos_oid, 0);
	dataWosRel = table_open(dataWosOid, AccessShareLock);

	snapshot = vci_GetSnapshotForWos2Ros();

	scan = table_beginscan(dataWosRel, snapshot, 0, NULL);
	scan->rs_flags &= ~SO_ALLOW_PAGEMODE;

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		count++;
	}
	table_endscan(scan);

	PopActiveSnapshot();

	/* release the data WOS relation */
	table_close(dataWosRel, AccessShareLock);

	/* release the main relation */
	vci_ReleaseMainRelHeader(info);

	return count;
}

/* --------------------------------------------------------------*/
/*   Update Delete Lists                                         */
/* --------------------------------------------------------------*/

uint32
vci_CountFreezedInWhiteoutWos(Relation mainRel, Size workareaSize)
{
	uint32		count = 0;
	vci_MainRelHeaderInfo infoData = {0};
	vci_MainRelHeaderInfo *info = &infoData;

	Oid			whiteoutWosOid;
	Relation	whiteoutWosRel;

	TableScanDesc scan;
	HeapTuple	tuple;
	Snapshot	snapshot;

	vci_InitMainRelHeaderInfo(info, mainRel, vci_rc_probe);
	vci_KeepMainRelHeader(info);

	whiteoutWosOid = (Oid) vci_GetMainRelVar(info, vcimrv_whiteout_wos_oid, 0);
	whiteoutWosRel = table_open(whiteoutWosOid, AccessShareLock);

	snapshot = vci_GetSnapshotForWos2Ros();

	scan = table_beginscan(whiteoutWosRel, snapshot, 0, NULL);
	scan->rs_flags &= ~SO_ALLOW_PAGEMODE;
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		count++;
	}
	table_endscan(scan);

	PopActiveSnapshot();

	/* release the data WOS relation */
	table_close(whiteoutWosRel, AccessShareLock);

	/* release the main relation */
	vci_ReleaseMainRelHeader(info);

	return count;
}

static uint64
UpdateDelVec(vci_RosCommandContext *comContext, Size workareaSize, uint64 numRowsAtOnce)
{
	uint32		numExtents;
	Tuplesortstate *cridList;
	uint64		result = 0;

	if (comContext->num_delvec_tids == 0)
		return 0;

	numExtents = vci_GetMainRelVar(&comContext->info, vcimrv_num_extents, 0);

	cridList =
		tuplesort_begin_datum(INT8OID, Int8LessOperator, InvalidOid, false,
							  Min(workareaSize / 1024 / 2, INT_MAX), NULL, TUPLESORT_NONE);

	/*
	 * Phase 1. Convert TID List -> CRID List
	 */
	do
	{
		vci_TidCridUpdateListContext *oldListContext;
		Tuplesortstate *addList;

		uint32		oldSel;
		uint32		newSel;

		oldSel = vci_GetMainRelVar(&comContext->info, vcimrv_tid_crid_diff_sel, 0);
		newSel = 1 ^ oldSel;

		oldListContext = vci_OpenTidCridUpdateList(&comContext->info, oldSel);

		addList =
			tuplesort_begin_datum(TIDOID, TIDLessOperator, InvalidOid, false,
								  Min(workareaSize / 1024 / 2, INT_MAX), NULL, TUPLESORT_NONE);
		while (result < numRowsAtOnce)
		{
			ItemPointerData orig_tid;
			ItemPointerData wos_tid;
			uint64		cridUint;

			if (!get_entry_into_tid_list(comContext, WOS_Whiteout, &orig_tid, &wos_tid))
				break;

			if (comContext->whiteout_wos_del_list)
				tuplesort_putdatum(comContext->whiteout_wos_del_list, ItemPointerGetDatum(&wos_tid), false);

			cridUint = vci_GetCridFromTid(oldListContext, &orig_tid, NULL);

			if (cridUint == VCI_INVALID_CRID)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("vci index \"%s\" corrupted", RelationGetRelationName(comContext->info.rel)),
						 errdetail("try to delete TID (%d,%d) into delete vector twice",
								   ItemPointerGetBlockNumber(&orig_tid),
								   ItemPointerGetOffsetNumber(&orig_tid)),
						 errhint("Use DROP INDEX \"%s\"", RelationGetRelationName(comContext->info.rel))));

			/* list for storage */
			tuplesort_putdatum(addList, ItemPointerGetDatum(&orig_tid), false);

			/* list for operation */
			tuplesort_putdatum(cridList, Int64GetDatum((int64) cridUint), false);

			result++;
		}

		vci_CloseTidCridUpdateList(oldListContext);

		tuplesort_performsort(addList);

		/* Insert TID->CRID(Invalid) List */
		vci_MergeAndWriteTidCridUpdateList(&comContext->info, newSel, oldSel, addList, vci_GetCridFromUint64(VCI_INVALID_CRID));

		tuplesort_end(addList);

	} while (false);			/* phase1 */

	elog(DEBUG2, "CRID List OK");

	/*
	 * Phase 2. loop for crid
	 */
	do
	{
		LOCKMODE	lockmode = RowExclusiveLock;
		vci_ColumnRelations delvecCol;

		BlockNumber prevBlkno = InvalidBlockNumber;
		OffsetNumber prevOffset = InvalidOffsetNumber;

		Buffer		buffer = InvalidBuffer;
		Page		page = NULL;

		bool		readFirstBlock = false;
		Datum		value;
		bool		isnull;

		uint32		numDeletedRows[VCI_MAX_PAGE_SPACE / sizeof(vcis_m_extent_t)];
		int32		topExtentId = -1;
		BlockNumber topBlockNumber = InvalidBlockNumber;

		memset(numDeletedRows, 0, sizeof(numDeletedRows));

		tuplesort_performsort(cridList);

		vci_OpenColumnRelations(&delvecCol, &comContext->info,
								VCI_COLUMN_ID_DELETE, lockmode);

		while (tuplesort_getdatum(cridList, true, true, &value, &isnull, NULL))
		{
			HeapTupleHeader htup;
			int32		extentId;
			BlockNumber blkno;
			OffsetNumber offset;
			uint32		byte_num;
			uint32		setBitPos;
			uint64		crid;
			BlockNumber extentInfoBlkno;
			OffsetNumber extentInfoOffset;

			crid = (uint64) DatumGetInt64(value);

			extentId = vci_CalcExtentIdFromCrid64(crid);
			blkno = vci_CalcBlockNumberFromCrid64ForDelete(crid);
			offset = vci_CalcOffsetNumberFromCrid64ForDelete(crid);
			byte_num = vci_CalcByteFromCrid64ForDelete(crid);
			setBitPos = vci_CalcBitFromCrid64ForDelete(crid);

			if ((blkno != prevBlkno) || (offset != prevOffset))
			{
				if (readFirstBlock)
				{
					/* write Tuple & WAL  */
					vci_WriteItem(delvecCol.data, buffer, prevOffset);
				}
			}

			if (blkno != prevBlkno)
			{
				if (readFirstBlock)
					UnlockReleaseBuffer(buffer);

				buffer = vci_ReadBufferWithPageInitDelVec(delvecCol.data, blkno);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buffer);

				readFirstBlock = true;
			}

			/* Calc bits & overwrite */
			htup = (HeapTupleHeader) PageGetItem(page, PageGetItemId(page, offset));
			*((char *) htup + htup->t_hoff + byte_num) |= 1 << setBitPos;

			vci_GetExtentInfoPosition(&extentInfoBlkno, &extentInfoOffset, extentId);

			if (topBlockNumber != extentInfoBlkno)
			{
				writeNumDeleteRowsIntoExntetInfo(&comContext->info, topExtentId, numExtents, numDeletedRows);

				memset(numDeletedRows, 0, sizeof(numDeletedRows));

				topExtentId = extentId;
				topBlockNumber = extentInfoBlkno;
			}

			numDeletedRows[extentId - topExtentId]++;

			prevBlkno = blkno;
			prevOffset = offset;
		}

		/* write remaining Tuple & WAL, and release buffer */
		if (readFirstBlock)
		{
			Assert(BufferIsValid(buffer));
			vci_WriteItem(delvecCol.data, buffer, prevOffset);
			UnlockReleaseBuffer(buffer);
		}

		/* Close Column */
		vci_CloseColumnRelations(&delvecCol, lockmode);

		if (BlockNumberIsValid(topBlockNumber))
			writeNumDeleteRowsIntoExntetInfo(&comContext->info, topExtentId, numExtents, numDeletedRows);

	} while (false);			/* phase 2 */

	tuplesort_end(cridList);

	elog(DEBUG2, "update delvec OK");

	return result;
}

static void
writeNumDeleteRowsIntoExntetInfo(vci_MainRelHeaderInfo *info, int32 topExtentId, uint32 numExtents, uint32 *numDeletedRows)
{
	BlockNumber topBlockNumber;
	OffsetNumber topOffsetNumber;
	Buffer		buffer;
	Page		page;

	if (topExtentId < 0)
		return;

	vci_GetExtentInfoPosition(&topBlockNumber, &topOffsetNumber, topExtentId);

	buffer = vci_ReadBufferWithPageInit(info->rel, topBlockNumber);

	/* LockBuffer(buffer, BUFFER_LOCK_SHARE); */
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buffer);

	for (int32 extentId = topExtentId; (uint32) extentId < numExtents; extentId++)
	{
		BlockNumber curBlockNumber;
		OffsetNumber curOffsetNumber;
		vcis_m_extent_t *extentInfo;

		vci_GetExtentInfoPosition(&curBlockNumber, &curOffsetNumber, extentId);

		if (curBlockNumber != topBlockNumber)
			break;

		extentInfo = (vcis_m_extent_t *) &(((char *) page)[curOffsetNumber]);

		extentInfo->num_deleted_rows += numDeletedRows[extentId - topExtentId];
	}

	vci_WriteOneItemPage(info->rel, buffer);

	UnlockReleaseBuffer(buffer);
}

int
vci_UpdateDelVec(Relation mainRel, Size workareaSize, int numRows)
{
	int			result = -1;
	MemoryContext memCtxWos2Ros;
	MemoryContext oldMemCtx;
	vci_RosCommandContext comContext;

	vci_InitRosCommandContext0(&comContext, mainRel, vci_rc_update_del_vec);

	/* recover ROS if necessary */
	vci_RecoverOneVCIIfNecessary(&(comContext.info));

	/* Change Mem Context */
	memCtxWos2Ros = AllocSetContextCreate(TopTransactionContext,
										  "Delete Vector Update.",
										  ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(memCtxWos2Ros);

	/* Create TID List from Whiteout WOS  */
	vci_InitRosCommandContext1(&comContext,
							   workareaSize / 2,
							   0, numRows,
							   false);

	vci_InitRosCommandContext2(&comContext, workareaSize / 2);

	if (TransactionIdPrecedes(GetCurrentTransactionId(),
							  (TransactionId) vci_GetMainRelVar(&comContext.info, vcimrv_current_ros_version, 0)))
		goto done;

	GetActiveSnapshot();

	/* Write Recovery Information  */
	vci_WriteRecoveryRecordForUpdateDelVec(&comContext.info);

	vci_WriteRecoveryRecordDone(&comContext.info, comContext.command, comContext.xid);

	constructTidSortState(&comContext);

	/* call Main routine */
	result = UpdateDelVec(&comContext, workareaSize / 2, Min(numRows, VCI_NUM_ROWS_IN_EXTENT));

	/* Clean up WOS entry */
	cleanUpWos(&comContext, vcimrv_data_wos_oid);
	cleanUpWos(&comContext, vcimrv_whiteout_wos_oid);

	/* Xmax WOS entry */
	RemoveWosEntries(&comContext, WOS_Data);
	RemoveWosEntries(&comContext, WOS_Whiteout);

done:
	/* Finalize ROS */
	vci_FinRosCommandContext(&comContext, false);

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(memCtxWos2Ros);

	return result;
}

/* -------------------------------------------------------------- */
/*   Correction Deleted Rows                                      */
/* -------------------------------------------------------------- */

static vci_target_extent_info_t
CountExtents(Relation mainRel, uint32 threshold, CEKind kind)
{
	TransactionId wos2rosXid;

	vci_MainRelHeaderInfo infoData = {0};
	vci_MainRelHeaderInfo *info = &infoData;

	vcis_m_extent_t *extentInfo;
	vci_meta_item_scanner_t *scan;

	vci_target_extent_info_t result = {0, -1 /* not-found-value */ };
	uint32		max_deleted_rows = 0;

	wos2rosXid = GetOldestNonRemovableTransactionId(mainRel);

	vci_InitMainRelHeaderInfo(info, mainRel, vci_rc_probe);
	vci_KeepMainRelHeader(info);
	vci_GetMainRelVar(info, vcimrv_num_extents, 0);

	scan = vci_BeginMetaItemScan(info->rel, BUFFER_LOCK_SHARE);
	while ((extentInfo = vci_GetMExtentNext(info, scan)) != NULL)
	{
		if (kind == CEK_CountDeletedRows)
		{
			if (vci_ExtentIsFree(extentInfo))
				continue;

			if (!vci_ExtentIsVisible(extentInfo, wos2rosXid))
				continue;

			if (TransactionIdIsValid(extentInfo->xdel))
				continue;

			if (extentInfo->num_deleted_rows >= threshold)
			{
				if (max_deleted_rows <= extentInfo->num_deleted_rows)
				{
					result.best_extent_id = scan->index;
					max_deleted_rows = extentInfo->num_deleted_rows;
				}
				result.num_fit_extents++;
			}
		}
		else
		{
			if (vci_ExtentIsFree(extentInfo))
				continue;

			if (vci_ExtentIsCollectable(extentInfo, wos2rosXid))
			{
				result.best_extent_id = scan->index;
				result.num_fit_extents++;
			}
		}
	}
	vci_EndMetaItemScan(scan);

	/* release the main relation */
	vci_ReleaseMainRelHeader(info);

	return result;
}

vci_target_extent_info_t
vci_CountDeletedRowsInROS(Relation mainRel, uint32 threshold)
{
	return CountExtents(mainRel, threshold, CEK_CountDeletedRows);
}

static HeapTuple
getTupleFromVector(int offset,
				   TupleDesc tupleDesc,
				   vci_virtual_tuples_t *vecSet)
{
	HeapTuple	result;
	vci_CSFetchContext fetchContext = vecSet->fetch_context;
	vci_CSQueryContext queryContext = fetchContext->query_context;
	Datum		values[MaxAttrNumber];
	bool		isnull[MaxAttrNumber];

	Assert((0 <= offset) && (offset < vecSet->num_rows));
	Assert(tupleDesc->natts == vecSet->num_columns);
	for (int cId = 0; cId < vecSet->num_columns; ++cId)
	{
		int			tgtId = queryContext->column_id[fetchContext->column_link[cId]];

		Assert((0 <= tgtId) && (tgtId < queryContext->num_columns));
		values[tgtId] = vci_CSGetValuesOfVirtualTupleColumnar(vecSet, cId)[offset];
		isnull[tgtId] = vci_CSGetIsNullOfVirtualTupleColumnar(vecSet, cId)[offset];
	}
	result = heap_form_tuple(tupleDesc, values, isnull);
#ifdef __s390x__
	result->t_self = vci_CSGetTidInItemPointerFromVirtualTuples(vecSet, offset);
#else
	result->t_self = *(vci_CSGetTidInItemPointerFromVirtualTuples(vecSet, offset));
#endif

	return result;
}

static void
FillOneRosChunkBufferFromExtent(vci_RosCommandContext *comContext,
								int32 extentId,
								uint32 *rowIdInExtent)
{
	vci_CSQueryContext queryContext;
	vci_CSFetchContext fetchContext;
	vci_CSFetchContext localContext;
	vci_virtual_tuples_t *vectorSet = NULL;

	TupleDesc	tupleDesc;
	AttrNumber *tableAttrNumList;
	AttrNumber *fetchAttrNumList;
	int			numFetchRowsAtOnce = Min(comContext->numRowsAtOnce, VCI_MAX_NUM_ROW_TO_FETCH);
	vci_ros_command_t saveCommand1;

	saveCommand1 = comContext->info.command;

	/* Get a descriptor of the index relation(VCI main relation). */
	/* This is not a descriptor of the table relation. */
	/* This including only target columns for VCI. */
	tupleDesc = vci_GetTupleDescr(&comContext->info);
	Assert(comContext->numColumns == tupleDesc->natts);

	/* Create pg_attribute::attnum list of the table relation for initialize, */
	/* and create serial number of ROS columners for fetch.  */
	tableAttrNumList = palloc_array(AttrNumber, comContext->numColumns);
	fetchAttrNumList = palloc_array(AttrNumber, comContext->numColumns);
	for (int colId = VCI_FIRST_NORMALCOLUMN_ID; colId < comContext->numColumns; ++colId)
	{
		tableAttrNumList[colId] = comContext->heapAttrNumList[colId];
		fetchAttrNumList[colId] = (AttrNumber) (comContext->indxColumnIdList[colId] + 1);
	}

	/* queryContext */
	queryContext = vci_CSCreateQueryContext(RelationGetRelid(comContext->info.rel),
											comContext->numColumns,
											tableAttrNumList,
											TopTransactionContext,
											false,
											false);

	/* fetchContext */
	fetchContext = vci_CSCreateFetchContext(queryContext,
											numFetchRowsAtOnce,
											comContext->numColumns,
											tableAttrNumList,
											true,	/* use ColumnStore */
											true,	/* return Tid */
											false); /* NOT return CRID */

	localContext = vci_CSLocalizeFetchContext(fetchContext,
											  CurrentMemoryContext);

	{
		vci_extent_status_t *status = vci_CSCreateCheckExtent(localContext);
		bool		extent_ok;

		vci_CSCheckExtent(status, localContext, extentId, true);

		elog(DEBUG2, "status: %d, %d, %d, %d", status->size, status->num_rows,
			 status->existence, status->visible);

		extent_ok = status->existence && status->visible;

		vci_CSDestroyCheckExtent(status);

		if (!extent_ok)
		{
			comContext->done = true;
			goto done;
		}
	}

	/* VectorSet */
	vectorSet = vci_CSCreateVirtualTuples(localContext);

	{
		while (comContext->buffer.numFilled < comContext->numRowsAtOnce)
		{
			/* int numFetchRows; */
			int			numRead;

			if ((*rowIdInExtent) >= VCI_NUM_ROWS_IN_EXTENT)
			{
				comContext->done = true;
				goto done;
			}

			/*
			 * if (((*rowIdInExtent) + numFetchRowsAtOnce) <=
			 * VCI_NUM_ROWS_IN_EXTENT) numFetchRows = numFetchRowsAtOnce; else
			 * numFetchRows = VCI_NUM_ROWS_IN_EXTENT - (*rowIdInExtent);
			 */

			/* FIXME: Does it need to use numFetchRows?? */
			/* let the vci_CSFetchVirtualTuples optimize the number of rows */
			numRead = vci_CSFetchVirtualTuples(vectorSet,
											   vci_CalcCrid64(extentId, *rowIdInExtent),
											   numFetchRowsAtOnce);

			if (numRead < 1)
			{
				comContext->done = true;
				goto done;
			}

			/* Read fetched data as HeapTuple */
			for (int offset = 0; offset < numRead; ++offset)
			{
				HeapTuple	tuple = NULL;
				uint16		skip = vci_CSGetSkipFromVirtualTuples(vectorSet)[offset];

				if (0 < skip)
				{
					(*rowIdInExtent) += skip;
					offset += skip - 1;
					continue;
				}

				tuple = getTupleFromVector(offset, tupleDesc, vectorSet);
				(*rowIdInExtent) += 1;

				if (tuple != NULL)
				{
					/* ... and register to ROS Chunk. */
					vci_FillOneRowInRosChunkBuffer(&(comContext->buffer),
												   &(comContext->info),
												   &tuple->t_self,
												   tuple,
												   comContext->indxColumnIdList,
												   fetchAttrNumList,
												   tupleDesc);
					if (comContext->buffer.numFilled == comContext->numRowsAtOnce)
						break;
				}
				else
				{
					Assert(false);
					elog(LOG, "internal error: CDR command failed");
				}
			}
		}
	}

done:
	if (vectorSet)
		vci_CSDestroyVirtualTuples(vectorSet);
	vci_CSDestroyFetchContext(localContext);
	vci_CSDestroyFetchContext(fetchContext);
	vci_CSDestroyQueryContext(queryContext);

	pfree(fetchAttrNumList);
	pfree(tableAttrNumList);

	comContext->info.command = saveCommand1;
}

static bool
isCdrTargetExtentValid(vci_RosCommandContext *comContext)
{
	bool		result;
	uint32		numExtents;
	vcis_m_extent_t *extentInfo;
	Buffer		buffer = InvalidBuffer;

	if (comContext->extentId == comContext->extentIdSrc)
		return false;

	numExtents = vci_GetMainRelVar(&comContext->info, vcimrv_num_extents, 0);
	if (numExtents <= comContext->extentIdSrc)
		return false;

	extentInfo = vci_GetMExtent(&buffer, &comContext->info, comContext->extentIdSrc);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	result = vci_ExtentIsVisible(extentInfo, comContext->wos2rosXid) && !TransactionIdIsValid(extentInfo->xdel);
	UnlockReleaseBuffer(buffer);

	return result;
}

static int32
CollectDeletedRows(vci_RosCommandContext *comContext, Snapshot snapshot)
{
	uint32		rowIdInExtent;

	vcis_m_extent_t *extentInfo;
	Buffer		buffer = InvalidBuffer;

	int			numRows;

	Assert(0 == (comContext->numRowsAtOnce % VCI_COMPACTION_UNIT_ROW));

	/*
	 * Set CDR data and write main relation for recovery. Header and extent
	 * info. Here, we also put current ROS version to the actual current
	 * transaction ID.
	 */
	vci_WriteExtentInfoInMainRosForWriteExtent(&comContext->info,
											   comContext->extentId,
											   comContext->xid,
											   vci_rc_collect_deleted);

	/* Create ROS Chunk from target Extent */
	vci_ResetRosChunkStorage(&(comContext->storage));
	vci_ResetRosChunkBufferCounter(&(comContext->buffer));

	/* collect data from old extent for new extent */
	rowIdInExtent = 0;
	while (!comContext->done)
	{

		CHECK_FOR_INTERRUPTS();

		/* fetch the data from old extents for one chunk */
		FillOneRosChunkBufferFromExtent(comContext,
										comContext->extentIdSrc, &rowIdInExtent);

		if (comContext->buffer.numFilled == comContext->numRowsAtOnce)
		{
			/* copy chunk buffer in a compact manner */
			vci_RegisterChunkBuffer(&(comContext->storage), &(comContext->buffer));
			vci_ResetRosChunkBufferCounter(&(comContext->buffer));

			Assert(comContext->storage.numTotalRows <= VCI_NUM_ROWS_IN_EXTENT);
		}
		else
		{
			Assert(comContext->done);

			/*
			 * We read and fill data in unit of VCI_COMPACTION_UNIT_ROW. The
			 * remaining data is read outside this loop to merge data read
			 * newly from WOS.
			 */
		}
	}
	comContext->done = false;

	elog(DEBUG2, "... collected deleted extent %d -> %d", comContext->extentIdSrc,
		 comContext->extentId);

	/*
	 * Now, reading from old extent was completed. Write Current ROS Version
	 * to VCI main relation as the XDel of old extent.
	 */
	extentInfo = vci_GetMExtent(&buffer, &(comContext->info),
								comContext->extentIdSrc);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	extentInfo->xdel = comContext->xid;
	vci_WriteOneItemPage(comContext->info.rel, buffer);
	UnlockReleaseBuffer(buffer);

	/* Append data from WOS */

	numRows = Min((VCI_NUM_ROWS_IN_EXTENT - comContext->storage.numTotalRows
				   - comContext->buffer.numFilled),
				  comContext->numRowsToConvert);

	if (numRows > 0)
	{
		fillTidListFromTidSortState(comContext, numRows);

		ReadOneExtentAndStoreInChunkStorage(comContext);
	}

	/* Copy the remaining data to chunk buffer in a compact manner */
	if (0 < comContext->buffer.numFilled)
	{
		vci_RegisterChunkBuffer(&(comContext->storage), &(comContext->buffer));
		vci_ResetRosChunkBufferCounter(&(comContext->buffer));

		Assert(comContext->storage.numTotalRows <= VCI_NUM_ROWS_IN_EXTENT);
	}

	/*
	 * Update TID-CRID List, and Write Ros Chunk into new extent.
	 */
	comContext->numRowsToConvert = comContext->storage.numTotalRows;

	if (comContext->numRowsToConvert == 0)
	{

		vci_SetMainRelVar(&comContext->info, vcimrv_new_extent_id, 0, VCI_INVALID_EXTENT_ID);

		return 0;
	}

	vci_AddTidCridUpdateList(&(comContext->info),
							 &(comContext->storage),
							 comContext->extentId);
	vci_WriteOneExtent(&(comContext->info),
					   &(comContext->storage),
					   comContext->extentId,	/* to */
					   comContext->xid,
					   InvalidTransactionId,
					   comContext->xid);

	return comContext->storage.numTotalRows;
}

int
vci_CollectDeletedRows(Relation mainRel, Size workareaSize, int32 extentId)
{
	int			result = -1;

	MemoryContext memCtxWos2Ros;
	MemoryContext oldMemCtx;
	vci_RosCommandContext comContext;
	Snapshot	snapshot;

	vci_InitRosCommandContext0(&comContext, mainRel, vci_rc_collect_deleted);

	/* excute recovery previous ROS command if necessary */
	vci_RecoverOneVCIIfNecessary(&(comContext.info));

	/* Change Mem Context */
	memCtxWos2Ros = AllocSetContextCreate(TopTransactionContext,
										  "Collect Deleted Rows",
										  ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(memCtxWos2Ros);

	/* CommandContext */
	vci_InitRosCommandContext1(&comContext,
							   workareaSize / 3 * 2,
							   VCI_NUM_ROWS_IN_EXTENT, 0,
							   true);

	vci_InitRosCommandContext2(&comContext, workareaSize / 3);

	if (TransactionIdPrecedes(GetCurrentTransactionId(),
							  (TransactionId) vci_GetMainRelVar(&comContext.info, vcimrv_current_ros_version, 0)))
		goto done;

	snapshot = GetActiveSnapshot();

	/* obtain new extent ID */
	comContext.extentIdSrc = extentId;
	comContext.extentId = vci_GetFreeExtentId(&(comContext.info));

	if (!isCdrTargetExtentValid(&comContext))
		goto done;

	/* Write Recovery Information of this command. */
	vci_WriteRecoveryRecordForExtentInfo(&comContext.info, comContext.extentId, comContext.extentIdSrc);
	vci_InitRecoveryRecordForFreeSpace(&comContext.info);

	vci_WriteRecoveryRecordDone(&comContext.info, comContext.command, comContext.xid);

	constructTidSortState(&comContext);

	vci_InitRosChunkStroageAndBuffer(&comContext, true /* append */ );

	/* call Main routine */
	result = CollectDeletedRows(&comContext, snapshot);

	cleanUpWos(&comContext, vcimrv_data_wos_oid);
	cleanUpWos(&comContext, vcimrv_whiteout_wos_oid);

	/* Xmax WOS entry */
	RemoveWosEntries(&comContext, WOS_Data);
	RemoveWosEntries(&comContext, WOS_Whiteout);

done:
	/* Finalize ROS */
	vci_FinRosCommandContext(&comContext, false);

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(memCtxWos2Ros);

	return result;
}

/* -------------------------------------------------------------- */
/*   Collect Unused Extent                                        */
/* -------------------------------------------------------------- */

vci_target_extent_info_t
vci_CountUnusedExtents(Relation mainRel)
{
	return CountExtents(mainRel, 0, CEK_CountUnusedExtents);
}

static uint32
SearchUnusedExtent(vci_MainRelHeaderInfo *info)
{
	int32		extentIdFirstFound = VCI_INVALID_EXTENT_ID;
	TransactionId OldestXmin;
	vcis_m_extent_t *extentInfo;
	vci_meta_item_scanner_t *scan;

	OldestXmin = GetOldestNonRemovableTransactionId(info->rel);

	/* search deleted extent */
	scan = vci_BeginMetaItemScan(info->rel, BUFFER_LOCK_SHARE);
	while ((extentInfo = vci_GetMExtentNext(info, scan)) != NULL)
	{
		if (vci_ExtentIsCollectable(extentInfo, OldestXmin))
		{
			extentIdFirstFound = scan->index;
			break;
		}
	}
	vci_EndMetaItemScan(scan);

	return extentIdFirstFound;
}

static void
CollectUnusedExtent(vci_RosCommandContext *comContext)
{
	int16		numColumns = vci_GetMainRelVar(&comContext->info, vcimrv_num_columns, 0);
	int16		recoveredColId = VCI_INVALID_COLUMN_ID;
	vcis_m_extent_t *extentInfo;
	Buffer		buffer = InvalidBuffer;

	extentInfo = vci_GetMExtent(&buffer, &comContext->info, comContext->extentId);

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	if (extentInfo->flags & VCIS_M_EXTENT_FLAG_ENABLE_RECOVERED_COLID)
		recoveredColId = extentInfo->recovered_colid;
	UnlockReleaseBuffer(buffer);

	for (int16 colId = VCI_COLUMN_ID_NULL; colId < numColumns; ++colId)
	{
		vci_ColumnRelations relPairData;
		vci_ColumnRelations *relPair = &relPairData;
		vcis_c_extent_t *extentPointer;

		LOCKMODE	lockmode = RowExclusiveLock;

		Buffer		bufData;
		Buffer		bufMeta;
		BlockNumber blockNumber;
		BlockNumber startBlockNumber;

		Page		page;

		vcis_extent_t *extentHead;

		vci_OpenColumnRelations(relPair, &comContext->info, colId, lockmode);

		/* target column-extent pointer */
		extentPointer = vci_GetColumnExtent(&bufMeta, &blockNumber,
											relPair->meta,
											comContext->extentId);
		startBlockNumber = extentPointer->enabled ? extentPointer->block_number : InvalidBlockNumber;
		ReleaseBuffer(bufMeta);

		if (!BlockNumberIsValid(startBlockNumber))
		{
			/* Close Column */
			elog(DEBUG2, "this is invalid extent pointer!!");
			vci_CloseColumnRelations(relPair, lockmode);
			continue;
		}

		/* get extent Header */
		bufData = vci_ReadBufferWithPageInit(relPair->data, startBlockNumber);
		page = BufferGetPage(bufData);
		extentHead = vci_GetExtentT(page);

		if (colId == recoveredColId)
			goto skip_collect_freelist;

		/* Freelist link node */
		{
			bool		isFixedLength;

			isFixedLength = true;
			if (VCI_FIRST_NORMALCOLUMN_ID <= colId)
			{
				vcis_m_column_t *colInfo;

				colInfo = vci_GetMColumn(&comContext->info, colId);
				if (colInfo->comp_type != vcis_compression_type_fixed_raw)
					isFixedLength = false;
			}

			if (!isFixedLength)
			{
				vcis_free_space_t newFS;
				BlockNumber newFSBlockNumber;

				vci_MakeFreeSpace(relPair, startBlockNumber, &newFSBlockNumber, &newFS, true);

				/* FIXME */	/* The common dictionary should be collected? */
				vci_WriteRecoveryRecordForFreeSpace(relPair,
													colId, VCI_INVALID_DICTIONARY_ID,
													newFSBlockNumber,
													&newFS);

				ReleaseBuffer(bufData);
				vci_AppendFreeSpaceToLinkList(relPair,
											  newFSBlockNumber,
											  newFS.prev_pos,
											  newFS.next_pos,
											  newFS.size);
			}
			else
			{
				LockBuffer(bufData, BUFFER_LOCK_EXCLUSIVE);
				extentHead->type = vcis_free_space;
				vci_WriteOneItemPage(relPair->data, bufData);
				UnlockReleaseBuffer(bufData);
			}
		}

skip_collect_freelist:
		vci_WriteRawDataExtentInfo(relPair->meta,
								   comContext->extentId,
								   InvalidBlockNumber,
								   0,
								   NULL,	/* min */
								   NULL,	/* max */
								   false,
								   false);

		/* Close Column */
		vci_CloseColumnRelations(relPair, lockmode);
	}
	/* loop for each column */

	vci_WriteExtentInfo(&comContext->info,
						comContext->extentId,
						0,
						0,
						0,
						InvalidTransactionId,
						InvalidTransactionId);
}

int
vci_CollectUnusedExtent(Relation mainRel, Size workareaSize)
{
	int			result = -1;

	MemoryContext memCtxWos2Ros;
	MemoryContext oldMemCtx;
	vci_RosCommandContext comContext;

	vci_InitRosCommandContext0(&comContext, mainRel, vci_rc_collect_extent);

	/* excute recovery previous ROS command if necessary */
	vci_RecoverOneVCIIfNecessary(&(comContext.info));

	/* Change Mem Context */
	memCtxWos2Ros = AllocSetContextCreate(TopTransactionContext,
										  "Collect Deleted Extent",
										  ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(memCtxWos2Ros);

	/* CommandContext */
	vci_InitRosCommandContext1(&comContext,
							   workareaSize,
							   0, 0,
							   false);

	if (TransactionIdPrecedes(GetCurrentTransactionId(),
							  (TransactionId) vci_GetMainRelVar(&comContext.info, vcimrv_current_ros_version, 0)))
		goto done;

	comContext.extentIdSrc = VCI_INVALID_EXTENT_ID;
	comContext.extentId = SearchUnusedExtent(&comContext.info);

	if (comContext.extentId == VCI_INVALID_EXTENT_ID)
		goto done;

	/* Write Recovery Infomation of this command. */
	vci_WriteRecoveryRecordForExtentInfo(&comContext.info, VCI_INVALID_EXTENT_ID, comContext.extentId);
	vci_InitRecoveryRecordForFreeSpace(&comContext.info);

	vci_WriteRecoveryRecordDone(&comContext.info, comContext.command, comContext.xid);

	/* call Main routine */
	CollectUnusedExtent(&comContext);

	result = comContext.extentId;

done:
	/* Finalize ROS */
	vci_FinRosCommandContext(&comContext, false);

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(memCtxWos2Ros);

	return result;
}

/* -------------------------------------------------------------- */
/*   Update TID-CRID Tree                                         */
/* -------------------------------------------------------------- */

int32
vci_CountTidCridUpdateListLength(Relation mainRel, Size workarea)
{
	int32		result;
	vci_MainRelHeaderInfo infoData = {0};
	vci_MainRelHeaderInfo *info = &infoData;
	int32		oldSel;

	vci_InitMainRelHeaderInfo(info, mainRel, vci_rc_probe);
	vci_KeepMainRelHeader(info);

	oldSel = vci_GetMainRelVar(info, vcimrv_tid_crid_diff_sel, 0);
	result = vci_GetTidCridUpdateListLength(info, oldSel);

	/* release the main relation */
	vci_ReleaseMainRelHeader(info);

	return result;
}

/**
 * @param[in] comContext   Conv Context
 * @param[in] workareaSize
 */
static void
UpdateTidCrid(vci_RosCommandContext *comContext, Size workareaSize)
{
	const LOCKMODE lockmode = RowExclusiveLock;
	int			i;

	vci_TidCridRelations relPairData;
	vci_TidCridRelations *relPair = &relPairData;

	vci_TidCridUpdateListContext *oldListContext = NULL;
	BlockNumber prevOldListBlkno = InvalidBlockNumber;
	vcis_tidcrid_pair_item_t *array;

	vcis_tidcrid_pair_list_t *moveList;
	Tuplesortstate *deleteList;

	uint32		oldSel = vci_GetMainRelVar(&comContext->info, vcimrv_tid_crid_diff_sel, 0);
	uint32		newSel = 1 ^ oldSel;

	oldListContext = vci_OpenTidCridUpdateList(&comContext->info, oldSel);

	moveList = palloc(offsetof(vcis_tidcrid_pair_list_t, body) + (sizeof(vcis_tidcrid_pair_item_t) * MaxHeapTuplesPerPage));
	moveList->num = 0;

	deleteList =
		tuplesort_begin_datum(TIDOID, TIDLessOperator, InvalidOid, false,
							  Min(workareaSize / 1024, INT_MAX), NULL, TUPLESORT_NONE);
	array = palloc_array(vcis_tidcrid_pair_item_t, VCI_TID_CRID_UPDATE_PAGE_ITEMS);

	vci_OpenTidCridRelations(relPair, &comContext->info, lockmode);

	i = 0;

	for (uint32 toMove = 0; toMove < comContext->utility_array.num; toMove++)
	{
		ItemPointerData treeNodeData;
		ItemPointer treeNode = &treeNodeData;

		BlockNumber blkToMove;

		blkToMove = comContext->utility_array.orig_blknos[toMove];

		moveList->num = 0;

		for (; i < oldListContext->count; i++)
		{
			BlockNumber blkno = VCI_TID_CRID_UPDATE_BODY_PAGE_ID + (i / VCI_TID_CRID_UPDATE_PAGE_ITEMS);
			vcis_tidcrid_pair_item_t item;

			if (prevOldListBlkno != blkno)
			{
				vci_ReadOneBlockFromTidCridUpdateList(oldListContext, blkno, array);
				prevOldListBlkno = blkno;
			}

			item = array[i % VCI_TID_CRID_UPDATE_PAGE_ITEMS];

			if (ItemPointerGetBlockNumber(&item.page_item_id) != blkToMove)
				break;

			Assert(moveList->num < MaxHeapTuplesPerPage);

			moveList->body[moveList->num] = item;
			moveList->num++;

			tuplesort_putdatum(deleteList, ItemPointerGetDatum(&item.page_item_id), false);
		}

		if (moveList->num == 0)
			continue;

		vci_GetTidCridSubTree(relPair, blkToMove, treeNode);

		if (!ItemPointerIsValid(treeNode))
			vci_CreateTidCridSubTree(relPair, blkToMove, treeNode);

		vci_UpdateTidCridSubTree(relPair, treeNode, moveList);
	}

	pfree(array);
	pfree(moveList);

	vci_CloseTidCridRelations(relPair, lockmode);

	vci_CloseTidCridUpdateList(oldListContext);

	tuplesort_performsort(deleteList);

	vci_MergeAndWriteTidCridUpdateList(&comContext->info, newSel, oldSel, deleteList, vci_GetCridFromUint64(VCI_MOVED_CRID));

	tuplesort_end(deleteList);
}

/**
 * @param[in,out] comContext Conv Context
 * @param[in] numPages
 */
static void
collectBlockNumberToMove(vci_RosCommandContext *comContext, int numPages)
{
	uint32		oldSel;
	vci_TidCridUpdateListContext *oldListContext;
	BlockNumber prevblk = InvalidBlockNumber;
	vcis_tidcrid_pair_item_t *array;
	BlockNumber blockNumber = VCI_TID_CRID_UPDATE_BODY_PAGE_ID;
	uint64		count = 0;

	oldSel = vci_GetMainRelVar(&comContext->info, vcimrv_tid_crid_diff_sel, 0);
	oldListContext = vci_OpenTidCridUpdateList(&comContext->info, oldSel);

	comContext->utility_array.num = 0;

	array = palloc_array(vcis_tidcrid_pair_item_t, VCI_TID_CRID_UPDATE_PAGE_ITEMS);

	while (blockNumber < oldListContext->nblocks)
	{
		int			i;

		vci_ReadOneBlockFromTidCridUpdateList(oldListContext, blockNumber, array);

		for (i = 0; (i < VCI_TID_CRID_UPDATE_PAGE_ITEMS) && (count < oldListContext->count); i++)
		{
			BlockNumber blkno = ItemPointerGetBlockNumber(&array[i].page_item_id);

			if (prevblk != blkno)
			{
				comContext->utility_array.orig_blknos[comContext->utility_array.num++] = blkno;
				prevblk = blkno;

				if (numPages == comContext->utility_array.num)
					goto done;
			}

			count++;
		}

		blockNumber++;
	}

done:
	pfree(array);

	vci_CloseTidCridUpdateList(oldListContext);
}

int
vci_UpdateTidCrid(Relation mainRel, Size workareaSize, int numPages)
{
	int			result = 0;

	MemoryContext memCtxWos2Ros;
	MemoryContext oldMemCtx;
	vci_RosCommandContext comContext;

	vci_InitRosCommandContext0(&comContext, mainRel, vci_rc_update_tid_crid);

	/* excute recovery previous ROS command if necessary */
	vci_RecoverOneVCIIfNecessary(&(comContext.info));

	/* Change Mem Context */
	memCtxWos2Ros = AllocSetContextCreate(TopTransactionContext,
										  "TIDCRID Tree Update",
										  ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(memCtxWos2Ros);

	/* CommandContext */
	vci_InitRosCommandContext1(&comContext,
							   workareaSize,
							   0, 0,
							   false);

	if (TransactionIdPrecedes(GetCurrentTransactionId(),
							  (TransactionId) vci_GetMainRelVar(&comContext.info, vcimrv_current_ros_version, 0)))
		goto done;

	comContext.utility_array.orig_blknos = palloc_array(BlockNumber, numPages);
	comContext.utility_array.max = numPages;

	collectBlockNumberToMove(&comContext, numPages);

	result = comContext.utility_array.num;

	/* Write Recovery Information of this command. */
	vci_InitRecoveryRecordForTidCrid(&comContext.info);
	vci_InitRecoveryRecordForFreeSpace(&comContext.info);

	vci_WriteRecoveryRecordDone(&comContext.info, comContext.command, comContext.xid);

	/* call Main routine */
	UpdateTidCrid(&comContext, workareaSize);

done:
	/* Finalize ROS */
	vci_FinRosCommandContext(&comContext, false);

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(memCtxWos2Ros);

	return result;
}

/* -------------------------------------------------------------- */
/*  Vacuum and Freeze                                             */
/* -------------------------------------------------------------- */

static void
freezeMainAndRos(vci_RosCommandContext *comContext)
{
	vcis_m_extent_t *mExtent;
	TransactionId wos2rosXid = comContext->wos2rosXid;
	vci_meta_item_scanner_t *scan;
	TransactionId lastRosVer;

	lastRosVer = vci_GetMainRelVar(&comContext->info, vcimrv_last_ros_version, 0);
	if (TransactionIdIsNormal(lastRosVer) && TransactionIdPrecedes(lastRosVer, wos2rosXid))
		vci_SetMainRelVar(&comContext->info, vcimrv_last_ros_version, 0, FrozenTransactionId);

	scan = vci_BeginMetaItemScan(comContext->info.rel, BUFFER_LOCK_EXCLUSIVE);
	while ((mExtent = vci_GetMExtentNext(&comContext->info, scan)) != NULL)
	{
		if (TransactionIdIsNormal(mExtent->xgen) &&
			TransactionIdPrecedes(mExtent->xgen, wos2rosXid))	/* mExtent->xgen <
																 * wos2rosXid */
			mExtent->xgen = FrozenTransactionId;

		if (TransactionIdIsNormal(mExtent->xdel) &&
			TransactionIdPrecedes(mExtent->xdel, wos2rosXid))	/* mExtent->xdel <
																 * wos2rosXid */
			mExtent->xdel = FrozenTransactionId;
	}
	vci_EndMetaItemScan(scan);
}

/*
 * VCITupleSatisfiesVisibility
 *		True iff heap tuple satisfies a time qual.
 *
 * Notes:
 *	Assumes heap tuple is valid, and buffer at least share locked.
 *
 * Copy of OSS HeapTupleSatisfiesVisibulity() for VCI snapshot types
 *
 */
bool
VCITupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_VCI_WOS2ROS:
			return HeapTupleSatisfiesWos2Ros(htup, snapshot, buffer);
		case SNAPSHOT_VCI_LOCALROS:
			return HeapTupleSatisfiesLocalRos(htup, snapshot, buffer);
		default:
			return HeapTupleSatisfiesVisibility(htup, snapshot, buffer);
	}
	return false;
}

static void
freezeWos(vci_RosCommandContext *comContext, vci_MainRelVar wosType, Snapshot snapshot)
{
	LOCKMODE	lockmode = ShareUpdateExclusiveLock;
	Oid			oid;
	HeapTupleFreeze *frozen;
	Relation	rel;
	BlockNumber nblocks;

	frozen = palloc0_array(HeapTupleFreeze, MaxHeapTuplesPerPage);

	oid = vci_GetMainRelVar(&comContext->info, wosType, 0);

	rel = table_open(oid, lockmode);

	nblocks = RelationGetNumberOfBlocks(rel);

	for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buffer;
		Page		page;
		OffsetNumber maxoff;
		int			nfrozen = 0;

		buffer = ReadBuffer(rel, blkno);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		page = BufferGetPage(buffer);

		maxoff = PageGetMaxOffsetNumber(page);

		for (OffsetNumber offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;
			HeapTupleData loctup;

			itemid = PageGetItemId(page, offnum);

			if (ItemIdIsNormal(itemid))
			{
				bool		valid;
				TransactionId xmin;

				loctup.t_tableOid = RelationGetRelid(rel);
				loctup.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
				loctup.t_len = ItemIdGetLength(itemid);
				ItemPointerSet(&loctup.t_self, blkno, offnum);

				valid = VCITupleSatisfiesVisibility(&loctup, snapshot, buffer);

				HeapCheckForSerializableConflictOut(valid, rel, &loctup, buffer, snapshot);

				xmin = HeapTupleHeaderGetXmin(loctup.t_data);

				if (valid &&
					!TransactionIdEquals(xmin, FrozenTransactionId) &&
					TransactionIdPrecedes(xmin, comContext->oldestXmin))
				{
					HeapTupleFreeze *frz = &frozen[nfrozen];
					HeapTupleHeader tuple = loctup.t_data;

					frz->frzflags = 0;
					frz->t_infomask2 = tuple->t_infomask2;
					frz->t_infomask = tuple->t_infomask | HEAP_XMIN_FROZEN;
					frz->xmax = HeapTupleHeaderGetRawXmax(tuple);
					frz->offset = offnum;

					nfrozen++;
				}
			}
		}

		if (nfrozen > 0)
		{
			heap_pre_freeze_checks(buffer, frozen, nfrozen);
			START_CRIT_SECTION();
			heap_freeze_prepared_tuples(buffer, frozen, nfrozen);
			MarkBufferDirty(buffer);

			/* Now WAL-log freezing if necessary */
			if (RelationNeedsWAL(rel))
			{
				/*
				 * Commit add323d added the vmbuffer/vmflags parameters.
				 * A quick fix was needed to allow build to proceed.
				 *
				 * TODO Confirm if passing InvalidBuffer, 0 is OK here.
				 */
				log_heap_prune_and_freeze(rel, buffer,
										  InvalidBuffer, /* vmbuffer */
										  0,		/* vmflags */
										  comContext->oldestXmin,
										  false,	/* no cleanup lock
													 * required */
										  PRUNE_VACUUM_SCAN,
										  frozen, nfrozen,
										  NULL, 0,	/* redirected */
										  NULL, 0,	/* dead */
										  NULL, 0); /* unused */
			}
			END_CRIT_SECTION();
		}
		UnlockReleaseBuffer(buffer);
	}

	table_close(rel, lockmode);

	pfree(frozen);
}

/**
 * @param[in] comContext Conv Context
 *
 * @note
 * This is not transaction-safe, because the truncation is done immediately
 * and cannot be rolled back later.  Caller is responsible for having
 * checked permissions etc, and must have obtained AccessExclusiveLock.
 */
static void
truncateRos(vci_RosCommandContext *comContext)
{
	const LOCKMODE lockmode = ShareUpdateExclusiveLock;

	vci_meta_item_scanner_t *scan;
	vcis_m_extent_t *extentInfo;
	int32		lastAvailableExtentId = -1;

	scan = vci_BeginMetaItemScan(comContext->info.rel, BUFFER_LOCK_SHARE);
	while ((extentInfo = vci_GetMExtentNext(&comContext->info, scan)) != NULL)
	{
		if (TransactionIdIsValid(extentInfo->xgen) ||
			TransactionIdIsValid(extentInfo->xdel))
			lastAvailableExtentId = scan->index;
	}
	vci_EndMetaItemScan(scan);

	vci_SetMainRelVar(&comContext->info, vcimrv_num_extents, 0, lastAvailableExtentId + 1);

	for (int colId = VCI_FIRST_NORMALCOLUMN_ID; colId < comContext->numColumns; ++colId)
	{
		vcis_m_column_t *colInfo;

		vci_ColumnRelations relPairData;
		vci_ColumnRelations *relPair = &relPairData;

		BlockNumber nblocks;

		colInfo = vci_GetMColumn(&comContext->info, colId);

		vci_OpenColumnRelations(relPair, &comContext->info, colId, lockmode);

		nblocks = RelationGetNumberOfBlocks(relPair->data);

		if (colInfo->comp_type != vcis_compression_type_fixed_raw)
		{
			BlockNumber sentinelBlockNumber;
			vcis_column_meta_t *columnMeta;

			elog(DEBUG2, "  -- colId %d ,variable column ", colId);

			columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);
			sentinelBlockNumber = columnMeta->free_page_end_id;
			ReleaseBuffer(relPair->bufMeta);

			Assert(sentinelBlockNumber + 1 <= nblocks);

			RelationTruncate(relPair->data, sentinelBlockNumber + 1);
			elog(DEBUG2, "  end");
		}
		else
		{
			int16		columnSize;
			int			extentHeaderSize;
			Size		dataSize;
			int			numExtentPages;
			BlockNumber startBlockNumber;

			elog(DEBUG2, "  -- colId %d ,variable column ", colId);

			columnSize = vci_GetFixedColumnSize(&comContext->info, colId);
			extentHeaderSize = vci_GetExtentFixedLengthRawDataHeaderSize(VCI_NUM_ROWS_IN_EXTENT);
			dataSize = (Size) columnSize * VCI_NUM_ROWS_IN_EXTENT;
			numExtentPages = vci_GetNumBlocks(dataSize + extentHeaderSize);
			startBlockNumber = (lastAvailableExtentId + 1) * numExtentPages;

			if (startBlockNumber < nblocks)
				RelationTruncate(relPair->data, startBlockNumber);

			elog(DEBUG2, "  end");

		}

		vci_CloseColumnRelations(relPair, lockmode);
	}
}

/**
 * @param[in] comContext Conv Context
 */
static void
truncateWos(vci_RosCommandContext *comContext)
{
	LOCKMODE	lockmode = ShareUpdateExclusiveLock;

	Oid			oid[2] = {
		vci_GetMainRelVar(&comContext->info, vcimrv_data_wos_oid, 0),
		vci_GetMainRelVar(&comContext->info, vcimrv_whiteout_wos_oid, 0)
	};

	for (int i = 0; i < 2; i++)
	{
		Relation	rel = table_open(oid[i], lockmode);
		int			lock_retry = 0;
		BlockNumber old_rel_pages;
		BlockNumber new_rel_pages;
		BlockNumber blkno;

		while (true)
		{
			if (ConditionalLockRelation(rel, AccessExclusiveLock))
				break;

			/*
			 * * Check for interrupts while trying to (re-)acquire the
			 * exclusive * lock.
			 */
			CHECK_FOR_INTERRUPTS();

			if (++lock_retry > (VACUUM_TRUNCATE_LOCK_TIMEOUT /
								VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL))
			{
				table_close(rel, lockmode);
				return;
			}

			pg_usleep(VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL);
		}

		blkno = old_rel_pages = new_rel_pages = RelationGetNumberOfBlocks(rel);

		while (blkno > 0)
		{
			Buffer		buffer;
			Page		page;
			OffsetNumber maxoff;

			blkno--;

			buffer = ReadBuffer(rel, blkno);

			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buffer);

			if (PageIsNew(page) || PageIsEmpty(page))
			{
				UnlockReleaseBuffer(buffer);

				new_rel_pages = blkno;
				continue;
			}

			maxoff = PageGetMaxOffsetNumber(page);

			for (OffsetNumber offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
			{
				ItemId		itemid;

				itemid = PageGetItemId(page, offnum);

				if (ItemIdIsUsed(itemid))
				{
					UnlockReleaseBuffer(buffer);
					goto found_use_item;
				}
			}

			UnlockReleaseBuffer(buffer);

			new_rel_pages = blkno;
		}

found_use_item:
		if (new_rel_pages < old_rel_pages)
			RelationTruncate(rel, new_rel_pages);

		UnlockRelation(rel, AccessExclusiveLock);

		table_close(rel, lockmode);
	}
}

void
vci_VacuumRos(Relation mainRel, IndexVacuumInfo *vacuumInfo)
{
	MemoryContext memCtxWos2Ros;
	MemoryContext oldMemCtx;
	vci_RosCommandContext comContext;
	Snapshot	snapshot;

	vci_InitRosCommandContext0(&comContext, mainRel, vci_rc_vacuum);

	/* recover ROS if necessary */
	vci_RecoverOneVCIIfNecessary(&(comContext.info));

	/* Change Mem Context */
	memCtxWos2Ros = AllocSetContextCreate(TopTransactionContext,
										  "Vacuum",
										  ALLOCSET_DEFAULT_SIZES);
	oldMemCtx = MemoryContextSwitchTo(memCtxWos2Ros);

	vci_InitRosCommandContext1(&comContext, 0, 0, 0, false);

	snapshot = GetActiveSnapshot();

	/* remove WOS entries */
	elog(DEBUG2, "  -- wos");
	cleanUpWos(&comContext, vcimrv_data_wos_oid);
	cleanUpWos(&comContext, vcimrv_whiteout_wos_oid);
	freezeWos(&comContext, vcimrv_data_wos_oid, snapshot);
	freezeWos(&comContext, vcimrv_whiteout_wos_oid, snapshot);
	truncateWos(&comContext);

	elog(DEBUG2, "  -- ros");
	freezeMainAndRos(&comContext);
	truncateRos(&comContext);

	elog(DEBUG2, "  -- end");

	vci_UpdateXidGeneration(&comContext.info);

	/* Finalize ROS */
	vci_FinRosCommandContext(&comContext, true /* never write */ );

	MemoryContextSwitchTo(oldMemCtx);
	MemoryContextDelete(memCtxWos2Ros);
}

static void
constructTidArray(vci_RosCommandContext *comContext, int max_data_wos_entries, int max_whiteout_wos_entries)
{
	vci_MainRelHeaderInfo *info;
	Snapshot	snapshot;
	Oid			data_wos_oid;
	Oid			whiteout_wos_oid;
	vci_tid_tid_xid64_t *data_wos_entries;
	vci_tid_tid_xid64_t *whiteout_wos_entries;
	int			num_data_wos_entries = 0;
	int			num_whiteout_wos_entries = 0;
	int			data_wos_entries_pos = 0;
	int			whiteout_wos_entries_pos = 0;

	info = &comContext->info;

	data_wos_oid = vci_GetMainRelVar(info, vcimrv_data_wos_oid, 0);
	whiteout_wos_oid = vci_GetMainRelVar(info, vcimrv_whiteout_wos_oid, 0);

	data_wos_entries = palloc_array(vci_tid_tid_xid64_t, max_data_wos_entries);
	whiteout_wos_entries = palloc_array(vci_tid_tid_xid64_t, max_whiteout_wos_entries);

	snapshot = vci_GetSnapshotForLocalRos(comContext->inclusiveXid, comContext->exclusiveXid);

	num_data_wos_entries =
		readTidListFromWosIntoTidArray(data_wos_oid, WOS_Data,
									   data_wos_entries, max_data_wos_entries,
									   snapshot);

	num_whiteout_wos_entries =
		readTidListFromWosIntoTidArray(whiteout_wos_oid, WOS_Whiteout,
									   whiteout_wos_entries, max_whiteout_wos_entries,
									   snapshot);

	Assert(num_data_wos_entries <= max_data_wos_entries);
	Assert(num_whiteout_wos_entries <= max_whiteout_wos_entries);

	qsort(data_wos_entries, num_data_wos_entries, sizeof(vci_tid_tid_xid64_t), comparator_orig_tid_xid64);
	qsort(whiteout_wos_entries, num_whiteout_wos_entries, sizeof(vci_tid_tid_xid64_t), comparator_orig_tid_xid64);

	while ((data_wos_entries_pos < num_data_wos_entries) &&
		   (whiteout_wos_entries_pos < num_whiteout_wos_entries))
	{
		int32		res;
		vci_tid_tid_xid64_t data_wos_item;
		vci_tid_tid_xid64_t whiteout_wos_item;

		data_wos_item = data_wos_entries[data_wos_entries_pos];
		whiteout_wos_item = whiteout_wos_entries[whiteout_wos_entries_pos];

		res = ItemPointerCompare(&data_wos_item.orig_tid, &whiteout_wos_item.orig_tid);

		if (res == 0)
			res = compareXid64(data_wos_item.xid64, whiteout_wos_item.xid64);

		if (res < 0)
		{
			comContext->wos2ros_array.orig_tids[comContext->wos2ros_array.num] =
				data_wos_item.orig_tid;

			comContext->wos2ros_array.num++;
			data_wos_entries_pos++;
		}
		else if (res > 0)
		{
			comContext->delvec_array.orig_tids[comContext->delvec_array.num] =
				whiteout_wos_item.orig_tid;

			comContext->delvec_array.num++;
			whiteout_wos_entries_pos++;
		}
		else
		{
			data_wos_entries_pos++;
			whiteout_wos_entries_pos++;
		}
	}

	while (data_wos_entries_pos < num_data_wos_entries)
	{
		comContext->wos2ros_array.orig_tids[comContext->wos2ros_array.num] =
			data_wos_entries[data_wos_entries_pos].orig_tid;

		comContext->wos2ros_array.num++;
		data_wos_entries_pos++;
	}

	while (whiteout_wos_entries_pos < num_whiteout_wos_entries)
	{
		comContext->delvec_array.orig_tids[comContext->delvec_array.num] =
			whiteout_wos_entries[whiteout_wos_entries_pos].orig_tid;

		comContext->delvec_array.num++;
		whiteout_wos_entries_pos++;
	}

	PopActiveSnapshot();

	pfree(data_wos_entries);
	pfree(whiteout_wos_entries);
}

static int
comparator_orig_tid_xid64(const void *pa, const void *pb)
{
	vci_tid_tid_xid64_t *a = (vci_tid_tid_xid64_t *) pa;
	vci_tid_tid_xid64_t *b = (vci_tid_tid_xid64_t *) pb;
	int			res;

	res = ItemPointerCompare(&a->orig_tid, &b->orig_tid);

	if (res == 0)
	{
		if (a->xid64 == b->xid64)
			res = 0;
		else if (a->xid64 > b->xid64)
			res = 1;
		else
			res = -1;
	}

	return res;
}

/**
 * @param[in,out] comContext     Conv Context
 * @param[in]     snapshot       Snapshot
 */
static void
constructTidSortState(vci_RosCommandContext *comContext)
{
	vci_MainRelHeaderInfo *info;
	Snapshot	snapshot;
	Oid			data_wos_oid;
	Oid			whiteout_wos_oid;
	MemoryContext workcontext;
	MemoryContext oldcontext;
	TupleDesc	tupDesc;
	Tuplesortstate *data_wos_valid_tid_sortstate;
	Tuplesortstate *whiteout_wos_valid_tid_sortstate;
	AttrNumber	sortKeys[2] = {1, 3};
	Oid			sortOperators[2] = {TIDLessOperator, Int8LessOperator};
	Oid			sortCollations[2] = {InvalidOid, InvalidOid,};
	bool		nullsFirstFlags[2] = {false, false};
	TupleTableSlot *data_wos_valid_slot;
	TupleTableSlot *whiteout_wos_valid_slot;
	vci_tid_tid_xid64_t data_wos_item;
	vci_tid_tid_xid64_t whiteout_wos_item;
	bool		is_terminated_data_wos = false;
	bool		is_terminated_whiteout_wos = false;
	int64		numInsertRows = 0;
	int64		numDeleteRows = 0;
	ItemPointerData last_whiteout_orig_tid;

	info = &comContext->info;

	data_wos_oid = vci_GetMainRelVar(info, vcimrv_data_wos_oid, 0);
	whiteout_wos_oid = vci_GetMainRelVar(info, vcimrv_whiteout_wos_oid, 0);

	workcontext = AllocSetContextCreate(CurrentMemoryContext,
										"Construct Tid Sort State",
										ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(workcontext);

	tupDesc = CreateTemplateTupleDesc(4);

	TupleDescInitEntry(tupDesc, (AttrNumber) 1, "orig_tid", TIDOID, -1, 0);
	TupleDescInitEntry(tupDesc, (AttrNumber) 2, "wos_tid", TIDOID, -1, 0);
	TupleDescInitEntry(tupDesc, (AttrNumber) 3, "xid64", INT8OID, -1, 0);
	TupleDescInitEntry(tupDesc, (AttrNumber) 4, "movable", BOOLOID, -1, 0);

	data_wos_valid_tid_sortstate =
		tuplesort_begin_heap(tupDesc, 2,
							 sortKeys, sortOperators, sortCollations, nullsFirstFlags,
							 VciGuc.maintenance_work_mem / 8 * 3, NULL,
							 TUPLESORT_NONE);

	whiteout_wos_valid_tid_sortstate =
		tuplesort_begin_heap(tupDesc, 2,
							 sortKeys, sortOperators, sortCollations, nullsFirstFlags,
							 VciGuc.maintenance_work_mem / 8 * 3, NULL,
							 TUPLESORT_NONE);

	data_wos_valid_slot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsHeapTuple);
	whiteout_wos_valid_slot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsHeapTuple);

	snapshot = vci_GetSnapshotForWos2Ros();

	readTidListFromWosIntoTidSortState(data_wos_oid, WOS_Data,
									   data_wos_valid_slot,
									   data_wos_valid_tid_sortstate,
									   snapshot,
									   comContext->wos2rosXid);

	readTidListFromWosIntoTidSortState(whiteout_wos_oid, WOS_Whiteout,
									   whiteout_wos_valid_slot,
									   whiteout_wos_valid_tid_sortstate,
									   snapshot,
									   comContext->wos2rosXid);

	tuplesort_performsort(data_wos_valid_tid_sortstate);
	tuplesort_performsort(whiteout_wos_valid_tid_sortstate);

	if (!getValidTidSortState(data_wos_valid_tid_sortstate, data_wos_valid_slot, &data_wos_item))
		is_terminated_data_wos = true;

	if (!getValidTidSortState(whiteout_wos_valid_tid_sortstate, whiteout_wos_valid_slot, &whiteout_wos_item))
		is_terminated_whiteout_wos = true;

	ItemPointerSetInvalid(&last_whiteout_orig_tid);

	while (!is_terminated_data_wos && !is_terminated_whiteout_wos)
	{
		int32		res;

		res = ItemPointerCompare(&data_wos_item.orig_tid, &whiteout_wos_item.orig_tid);

		if (res == 0)
			res = compareXid64(data_wos_item.xid64, whiteout_wos_item.xid64);

		if (res < 0)
		{
			if (can_select_candidate_for_wos2ros_conv(&data_wos_item, comContext, &last_whiteout_orig_tid))
			{
				put_entry_into_tid_list(comContext, WOS_Data, &data_wos_item.orig_tid, &data_wos_item.wos_tid);

				numInsertRows++;
			}

			if (!getValidTidSortState(data_wos_valid_tid_sortstate, data_wos_valid_slot, &data_wos_item))
				is_terminated_data_wos = true;
		}
		else if (res > 0)
		{
			last_whiteout_orig_tid = whiteout_wos_item.orig_tid;

			if (can_select_candidate_for_update_delvec(&whiteout_wos_item, comContext))
			{
				put_entry_into_tid_list(comContext, WOS_Whiteout, &whiteout_wos_item.orig_tid, &whiteout_wos_item.wos_tid);

				numDeleteRows++;
			}

			if (!getValidTidSortState(whiteout_wos_valid_tid_sortstate, whiteout_wos_valid_slot, &whiteout_wos_item))
				is_terminated_whiteout_wos = true;
		}
		else
		{
			if (data_wos_item.movable && whiteout_wos_item.movable)
			{
				if (comContext->data_wos_del_list)
					tuplesort_putdatum(comContext->data_wos_del_list,
									   ItemPointerGetDatum(&data_wos_item.wos_tid), false);

				if (comContext->whiteout_wos_del_list)
					tuplesort_putdatum(comContext->whiteout_wos_del_list,
									   ItemPointerGetDatum(&whiteout_wos_item.wos_tid), false);
			}

			if (!getValidTidSortState(data_wos_valid_tid_sortstate, data_wos_valid_slot, &data_wos_item))
				is_terminated_data_wos = true;

			if (!getValidTidSortState(whiteout_wos_valid_tid_sortstate, whiteout_wos_valid_slot, &whiteout_wos_item))
				is_terminated_whiteout_wos = true;
		}
	}

	if (!is_terminated_data_wos && comContext->wos2ros_tid_list)
	{
		do
		{
			if (can_select_candidate_for_wos2ros_conv(&data_wos_item, comContext, &last_whiteout_orig_tid))
			{
				put_entry_into_tid_list(comContext, WOS_Data, &data_wos_item.orig_tid, &data_wos_item.wos_tid);
				numInsertRows++;
			}
		} while (getValidTidSortState(data_wos_valid_tid_sortstate,
									  data_wos_valid_slot, &data_wos_item));
	}

	if (!is_terminated_whiteout_wos && comContext->delvec_tid_list)
	{
		do
		{
			if (can_select_candidate_for_update_delvec(&whiteout_wos_item, comContext))
			{
				put_entry_into_tid_list(comContext, WOS_Whiteout, &whiteout_wos_item.orig_tid, &whiteout_wos_item.wos_tid);

				numDeleteRows++;
			}
		} while (getValidTidSortState(whiteout_wos_valid_tid_sortstate,
									  whiteout_wos_valid_slot, &whiteout_wos_item));
	}

	tuplesort_end(whiteout_wos_valid_tid_sortstate);
	tuplesort_end(data_wos_valid_tid_sortstate);

	FreeTupleDesc(tupDesc);

	PopActiveSnapshot();

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(workcontext);

	if (comContext->wos2ros_tid_list)
	{
		tuplesort_performsort(comContext->wos2ros_tid_list);
		comContext->num_wos2ros_tids = numInsertRows;
	}

	if (comContext->delvec_tid_list)
	{
		tuplesort_performsort(comContext->delvec_tid_list);
		comContext->num_delvec_tids = numDeleteRows;
	}
}

static bool
can_select_candidate_for_wos2ros_conv(vci_tid_tid_xid64_t *data_wos_item, vci_RosCommandContext *comContext, ItemPointer last_whiteout_orig_tid)
{
	if (!data_wos_item->movable)
		return false;

	if (!comContext->wos2ros_tid_list)
		return false;

	if (!comContext->delvec_tid_list)
		if (ItemPointerIsValid(last_whiteout_orig_tid) &&
			ItemPointerEquals(last_whiteout_orig_tid, &data_wos_item->orig_tid))
			return false;

	return true;
}

static bool
can_select_candidate_for_update_delvec(vci_tid_tid_xid64_t *whiteout_wos_item, vci_RosCommandContext *comContext)
{
	if (!whiteout_wos_item->movable)
		return false;

	if (!comContext->delvec_tid_list)
		return false;

	return true;
}

static void
put_entry_into_tid_list(vci_RosCommandContext *comContext, WosKind wos_kind, ItemPointer orig_tid, ItemPointer wos_tid)
{
	TupleTableSlot *slot;
	Tuplesortstate *sortstate;

	slot = comContext->tid_tid_slot;

	ExecClearTuple(slot);

	if (wos_kind == WOS_Data)
		sortstate = comContext->wos2ros_tid_list;
	else
		sortstate = comContext->delvec_tid_list;

	Assert(sortstate != NULL);

	slot->tts_values[0] = ItemPointerGetDatum(orig_tid);
	slot->tts_values[1] = ItemPointerGetDatum(wos_tid);
	slot->tts_isnull[0] = false;
	slot->tts_isnull[1] = false;

	slot->tts_flags |= TTS_FLAG_EMPTY;

	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(sortstate, slot);
}

static bool
get_entry_into_tid_list(vci_RosCommandContext *comContext, WosKind wos_kind, ItemPointer orig_tid, ItemPointer wos_tid)
{
	bool		isnull;
	TupleTableSlot *slot;
	Tuplesortstate *sortstate;

	slot = MakeSingleTupleTableSlot(comContext->tid_tid_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);

	if (wos_kind == WOS_Data)
		sortstate = comContext->wos2ros_tid_list;
	else
		sortstate = comContext->delvec_tid_list;

	Assert(sortstate != NULL);

	if (!tuplesort_gettupleslot(sortstate, true, false, slot, NULL))
	{
		ExecDropSingleTupleTableSlot(slot);
		return false;
	}

	slot_getsomeattrs(slot, 2);

	*orig_tid = *DatumGetItemPointer(slot_getattr(slot, 1, &isnull));
	*wos_tid = *DatumGetItemPointer(slot_getattr(slot, 2, &isnull));

	ExecDropSingleTupleTableSlot(slot);
	return true;
}

static int
readTidListFromWosIntoTidArray(Oid wos_oid, WosKind wos_kind, vci_tid_tid_xid64_t *wos_entris, int max_wos_entries, Snapshot snapshot)
{
	LOCKMODE	lockmode = AccessShareLock;
	TableScanDesc scan;
	HeapTuple	tuple;
	Relation	rel;
	TupleDesc	tupleDesc;
	int			num_rows = 0;

	rel = relation_open(wos_oid, lockmode);

	tupleDesc = RelationGetDescr(rel);

	CHECK_FOR_INTERRUPTS();

	scan = table_beginscan(rel, snapshot, 0, NULL);

	scan->rs_flags &= ~SO_ALLOW_PAGEMODE;
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		isnull;

		if (max_wos_entries <= num_rows)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("too many WOS rows over estimation")));

		wos_entris[num_rows].orig_tid = *DatumGetItemPointer(heap_getattr(tuple, 1, tupleDesc, &isnull));	/* original_tid in WOS */
		wos_entris[num_rows].wos_tid = tuple->t_self;
		wos_entris[num_rows].xid64 = DatumGetInt64(heap_getattr(tuple, 2, tupleDesc, &isnull)); /* xid64 in WOS */

		wos_entris[num_rows].movable = true;

		Assert(ItemPointerIsValid(&wos_entris[num_rows].orig_tid));

		CHECK_FOR_INTERRUPTS();

		num_rows++;
	}
	table_endscan(scan);

	table_close(rel, lockmode);

	return num_rows;
}

static void
readTidListFromWosIntoTidSortState(Oid wos_oid, WosKind wos_kind,
								   TupleTableSlot *slot, Tuplesortstate *sortstate,
								   Snapshot snapshot,
								   TransactionId wos2ros_xid)
{
	LOCKMODE	lockmode = AccessShareLock;
	TableScanDesc scan;
	HeapTuple	tuple;
	Relation	rel;
	TupleDesc	tupleDesc;

	rel = relation_open(wos_oid, lockmode);
	tupleDesc = RelationGetDescr(rel);

	CHECK_FOR_INTERRUPTS();

	scan = table_beginscan(rel, snapshot, 0, NULL);
	scan->rs_flags &= ~SO_ALLOW_PAGEMODE;
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		TransactionId xmin;
		bool		isnull;
		bool		movable;

		xmin = HeapTupleHeaderGetXmin(tuple->t_data);
		movable = TransactionIdPrecedes(xmin, wos2ros_xid);
		ExecClearTuple(slot);

		slot->tts_values[0] = heap_getattr(tuple, 1, tupleDesc, &isnull);	/* original_tid in WOS */
		slot->tts_values[1] = ItemPointerGetDatum(&tuple->t_self);
		slot->tts_values[2] = heap_getattr(tuple, 2, tupleDesc, &isnull);	/* xid64 in WOS */
		slot->tts_values[3] = BoolGetDatum(movable);

		slot->tts_isnull[0] = false;
		slot->tts_isnull[1] = false;
		slot->tts_isnull[2] = false;
		slot->tts_isnull[3] = false;

		slot->tts_flags |= TTS_FLAG_EMPTY;

		ExecStoreVirtualTuple(slot);

		tuplesort_puttupleslot(sortstate, slot);

		CHECK_FOR_INTERRUPTS();
	}
	table_endscan(scan);

	relation_close(rel, lockmode);
}

static bool
getValidTidSortState(Tuplesortstate *sortstate, TupleTableSlot *slot, vci_tid_tid_xid64_t *item)
{
	bool		isnull;
	TupleTableSlot *tempslot;

	tempslot = MakeSingleTupleTableSlot(slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);

	if (!tuplesort_gettupleslot(sortstate, true, false, tempslot, NULL))
	{
		ExecDropSingleTupleTableSlot(tempslot);
		return false;
	}

	slot_getsomeattrs(tempslot, 4);

	item->orig_tid = *DatumGetItemPointer(slot_getattr(tempslot, 1, &isnull));
	item->wos_tid = *DatumGetItemPointer(slot_getattr(tempslot, 2, &isnull));
	item->xid64 = DatumGetInt64(slot_getattr(tempslot, 3, &isnull));
	item->movable = DatumGetBool(slot_getattr(tempslot, 4, &isnull));

	ExecDropSingleTupleTableSlot(tempslot);
	return true;
}

static int32
compareXid64(int64 data_wos_xid64, int64 whiteout_wos_xid64)
{
	Assert((data_wos_xid64 > 0) && (whiteout_wos_xid64 > 0));

	if (data_wos_xid64 == whiteout_wos_xid64)
	{
		return 0;
	}
	else if (data_wos_xid64 > whiteout_wos_xid64)
	{
		return +1;
	}
	else
	{

		return 0;
	}
}
