/*-------------------------------------------------------------------------
 *
 * vci_ros.c
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_ros.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdint.h>

#include "access/heapam_xlog.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"		/* for MAX_MULTIBYTE_CHAR_LEN */
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varbit.h"

#include "vci.h"
#include "vci_columns.h"
#include "vci_freelist.h"
#include "vci_ros.h"
#include "vci_mem.h"
#include "vci_wos.h"

/*
 * This file has four parts.
 * 1. Accessing VCI main relation header
 * 2. Relation and buffer control
 * 3. Attributes (columns)
 * 4. VCI "columns"
 */

/*
 * *********************************************************
 * Accessing VCI main relation header
 * *********************************************************
 */
/* Accessing VCI main relation header
 * Because the header of VCI main relation has three pages, we can not map
 * one structure of C on the header pages simply.
 * Instead, we use access functions.
 *
 * In order to, first use one of these two * functions,
 *
 * vci_KeepReadingMainRelHeader()
 *     Read header pages for reading, and pin them.
 * vci_KeepWritingMainRelHeader()
 *     Read header pages for writing, and pin them.
 *
 * Then, use the following two functions,
 *
 * vci_SetMainRelVar()
 *     To set the value to the field.
 * vci_GetMainRelVar()
 *     To get the value of the field.
 *
 * The field is defined in enum enum vci_MainRelVar.
 * The format is, page ID is in upper 16 bits, and offset from
 * the page top is in lower 16 bits.
 *
 * To write the header pages out to storage, use the next function.
 *
 * vci_WriteMainRelVar()
 *
 * After accessing the header, release the DB pages with the following
 * function.
 *
 * vci_ReleaseMainRelHeader()
 *     Release header pages.
 *
 * Other helper functions.
 *
 * vci_GetMColumnPosition()
 *     Gives the position of vcis_m_column_t.
 *
 * vci_GetMColumn()
 *     Gives vcis_m_column_t.
 *
 * vci_GetExtentInfoPosition()
 *     Get the position of vcis_m_extent_t structure for the target
 *     extentId.
 *
 * FIXME  Lock check function necessary?
 * Memo: I think the functions to check the lock status of the VCI main
 * relation may be convenient, in order to determine if it is possible to
 * start a ROS command.  It will be used to avoid conflict between building
 * local ROS, the vacuum operation, and other ROS commands.  For other ROS
 * commands, we do not need to use such functions, just try to lock and
 * wait.  Vacuum, too.  For local ROS conversion, we have to determine if
 * other ROS command is running when we evaluate the cost of plans.
 */

/**
 * @brief Initialize the structure info to access the header of VCI main
 * relation.
 *
 * This function "just" initializes the given object.
 * To access the information in the header, keep the DB pages in buffer
 * using vci_KeepMainRelHeader().
 * The accessors are vci_GetMainRelVar() and vci_SetMainRelVar().
 * After modifying the information, call vci_WriteMainRelVar() to write
 * the page back to the storage.
 * Finally to release the buffer, call vci_ReleaseMainRelHeader().
 *
 * @param[out] info Pointer to the target vci_MainRelHeaderInfo,
 * which will be initialized
 * @param[in] rel VCI main relation.
 * @param[in] command ROS command which uses this structure.
 */
void
vci_InitMainRelHeaderInfo(vci_MainRelHeaderInfo *info,
						  Relation rel,
						  vci_ros_command_t command)
{
	Assert(NULL != info);
	info->rel = rel;
	for (int aId = 0; aId < lengthof(info->buffer); ++aId)
		info->buffer[aId] = InvalidBuffer;
	info->command = command;
	info->num_extents_allocated = -1;
	info->initctx = CurrentMemoryContext;
	info->cached_tupledesc = NULL;
}

static void
KeepMainRelHeader(vci_MainRelHeaderInfo *info)
{
	Assert(NULL != info);
	Assert(NULL != info->rel);
	for (int blockNum = 0; blockNum < lengthof(info->buffer); ++blockNum)
		info->buffer[blockNum] = vci_ReadBufferWithPageInit(info->rel, blockNum);
}

static void
CheckRosVersion(vci_MainRelHeaderInfo *info)
{
	uint32		major = vci_GetMainRelVar(info, vcimrv_ros_version_major, 0);
	uint32		minor = vci_GetMainRelVar(info, vcimrv_ros_version_minor, 0);

	if ((major == 0) && (minor == 0))
		ereport(ERROR, (errmsg("ROS has not been formatted yet."),
						errhint("This might happen when CREATE INDEX fails. "
								"\"DROP INDEX %s;\" and CREATE INDEX again may help.",
								RelationGetRelationName(info->rel))));

	if ((VCI_ROS_VERSION_MAJOR != major) || (VCI_ROS_VERSION_MINOR != minor))
		ereport(ERROR, (errmsg("incompatible VCI version: expected (%d, %d), stored (%d, %d).", VCI_ROS_VERSION_MAJOR, VCI_ROS_VERSION_MINOR, major, minor),
						errhint("This can happen when accessing old database with newer VCI modules.  DROP and CREATE INDEX may help.")));
}

static int32
GetNumberOfExtentsFromSizeOfMainRelation(Relation rel)
{
	const int	headerBlockNumber = vcimrv_extent_info >> VCI_MRV_PAGE_SHIFT;
	const int	maxExtentInfoInFirstPage = (BLCKSZ -
											(vcimrv_extent_info & VCI_MRV_MASK_OFFSET)) /
		sizeof(vcis_m_extent_t);
	const int	maxExtentInfoInPage = VCI_MAX_PAGE_SPACE /
		sizeof(vcis_m_extent_t);
	int			numBlocks = RelationGetNumberOfBlocks(rel);

	if (numBlocks <= headerBlockNumber)
		return -1;

	return ((numBlocks - (headerBlockNumber + 1)) * maxExtentInfoInPage)
		+ maxExtentInfoInFirstPage;
}

static void
UpdateNumberOfExtentsInMainRelHeader(vci_MainRelHeaderInfo *info)
{
	if (vci_rc_query == info->command)
		info->num_extents_allocated = GetNumberOfExtentsFromSizeOfMainRelation(
																			   info->rel);
	else
		info->num_extents_allocated = -1;
}

/**
 * @brief Keep DB pages of VCI header in buffer.
 *
 * This function acquire one read lock with AccessShareLock.
 * This is called only by vci_inner_build().
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 */
void
vci_KeepMainRelHeaderWithoutVersionCheck(vci_MainRelHeaderInfo *info)
{
	Assert(info);
	Assert(RelationIsValid(info->rel));
	elog(DEBUG3, "open VCI \"%s\" ignoring ROS version",
		 RelationGetRelationName(info->rel));
	KeepMainRelHeader(info);
}

/**
 * @brief Change command ID stored in vci_MainRelHeaderInfo.
 *
 * @param[in] info pointer to the target vci_MainRelHeaderInfo.
 * @param[in] command new command ID.
 */
void
vci_ChangeCommand(vci_MainRelHeaderInfo *info, vci_ros_command_t command)
{
	Assert(info);
	info->command = command;
	UpdateNumberOfExtentsInMainRelHeader(info);
}

/**
 * @brief Keep DB pages of VCI header in buffer after checking the ROS version.
 *
 * This function acquire one read lock with AccessShareLock.
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 */
void
vci_KeepMainRelHeader(vci_MainRelHeaderInfo *info)
{
	Assert(info);
	Assert(RelationIsValid(info->rel));
	elog(DEBUG3, "open VCI \"%s\"",
		 RelationGetRelationName(info->rel));
	KeepMainRelHeader(info);
	CheckRosVersion(info);
	UpdateNumberOfExtentsInMainRelHeader(info);
}

/**
 * @brief Write header pages of VCI main relation.
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 * @param[in] writeArea Give vci_wmrv_update for updating the pages for
 * recovery, or vci_wmrv_all for all pages. The latter should only be used in
 * building the index.
 */
void
vci_WriteMainRelVar(vci_MainRelHeaderInfo *info,
					vci_wmrv_t writeArea)
{
	int			start = 0;

	Assert(NULL != info);
	Assert(NULL != info->rel);

	elog(DEBUG3, "flush header pages of VCI \"%s\" main relation",
		 RelationGetRelationName(info->rel));

	switch (writeArea)
	{
		case vci_wmrv_update:
			start = lengthof(info->buffer) - 1;
			break;
		case vci_wmrv_all:
			start = 0;
			break;
		default:
			ereport(ERROR, (errmsg("internal error.  unsupported parameter."), errhint("Disable VCI by 'SELECT vci_disable();'")));
	}

	for (int blockNum = start; blockNum < lengthof(info->buffer); ++blockNum)
	{
		LockBuffer(info->buffer[blockNum], BUFFER_LOCK_EXCLUSIVE);
		MarkBufferDirty(info->buffer[blockNum]);
		vci_WriteOneItemPage(info->rel, info->buffer[blockNum]);
		LockBuffer(info->buffer[blockNum], BUFFER_LOCK_UNLOCK);
	}
}

/**
 * @brief Release buffer for the VCI header.
 *
 * This function release one read lock with AccessShareLock.
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 */
void
vci_ReleaseMainRelHeader(vci_MainRelHeaderInfo *info)
{
	Assert(NULL != info);
	Assert(NULL != info->rel);

	elog(DEBUG3, "release VCI \"%s\"",
		 RelationGetRelationName(info->rel));
	for (int blockNum = 0; blockNum < lengthof(info->buffer); ++blockNum)
	{
		ReleaseBuffer(info->buffer[blockNum]);
		info->buffer[blockNum] = InvalidBuffer;
	}
	info->rel = NULL;
	info->cached_tupledesc = NULL;
}

/**
 * @brief Set values in the header part of VCI main relation.
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 * @param[in] var "virtual address" of the variable, defined in
 * enum vci_MainRelVar.
 * @param[in] elemId Give 0 normally.
 * When the target variable has multiple of elements, say an array,
 * the element ID should be placed.
 * @param[in] value The value to write.
 */
void
vci_SetMainRelVar(vci_MainRelHeaderInfo *info,
				  vci_MainRelVar var,
				  int elemId,
				  uint32 value)
{
	Page		page;
	unsigned int blockNumber = vci_MRVGetBlockNumber(var);
	unsigned int offset = vci_MRVGetOffset(var);

	Assert(blockNumber < lengthof(info->buffer));
	Assert(offset < BLCKSZ);

	page = BufferGetPage(info->buffer[blockNumber]);
	((uint32 *) &(((char *) page)[offset]))[elemId] = value;
}

/**
 * @brief Get values in the header part of VCI main relation.
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 * @param[in] var "virtual address" of the variable, defined in
 * enum vci_MainRelVar.
 * @param[in] elemId Give 0 normally.
 * When the target variable has multiple of elements, say an array,
 * the element ID should be placed.
 * @return The gotten value.
 */
uint32
vci_GetMainRelVar(vci_MainRelHeaderInfo *info,
				  vci_MainRelVar var,
				  int elemId)
{
	Page		page;
	unsigned int blockNumber = vci_MRVGetBlockNumber(var);
	unsigned int offset = vci_MRVGetOffset(var);

	Assert(blockNumber < lengthof(info->buffer));
	Assert(offset < BLCKSZ);
	page = BufferGetPage(info->buffer[blockNumber]);

	return ((uint32 *) &(((char *) page)[offset]))[elemId];
}

/**
 * @brief Get the position of column information in the VCI main relation.
 *
 * @param[in] columnId The column ID in the VCI index.
 * @return The offset in the page, which including DB page header part.
 */
vci_MainRelVar
vci_GetMColumnPosition(int16 columnId)
{
	const int	firstBlockNumber = vci_MRVGetBlockNumber(vcimrv_column_info);
	const int	numInFirstPage = (BLCKSZ - vci_MRVGetOffset(vcimrv_column_info)) /
		sizeof(vcis_m_column_t);
	const int	numInPage = VCI_MAX_PAGE_SPACE / sizeof(vcis_m_column_t);
	int			blockNumber;

	Assert(VCI_FIRST_NORMALCOLUMN_ID <= columnId);
	if (columnId < numInFirstPage)
	{
		return (firstBlockNumber << VCI_MRV_PAGE_SHIFT) +
			vci_MRVGetOffset(vcimrv_column_info) +
			(columnId * sizeof(vcis_m_column_t));
	}

	columnId -= numInFirstPage;
	blockNumber = columnId / numInPage;
	columnId -= blockNumber * numInPage;
	blockNumber += 1 + firstBlockNumber;
	Assert(blockNumber < (VCI_NUM_MAIN_REL_HEADER_PAGES - 1));

	return (blockNumber << VCI_MRV_PAGE_SHIFT) +
		VCI_MIN_PAGE_HEADER +
		(columnId * sizeof(vcis_m_column_t));
}

/**
 * @brief Get the column information in the VCI main relation.
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 * @param[in] columnId The column ID in the VCI index.
 * @return The pointer to the column information in the header page of
 * VCI main relation.
 *
 * @note
 * AFTER ACCESSING vcis_m_column_t, RELEASE BUFFER WITH ReleaseBuffer(buffer);
 */
vcis_m_column_t *
vci_GetMColumn(vci_MainRelHeaderInfo *info, int16 columnId)
{
	Page		page;
	vci_MainRelVar mrv = vci_GetMColumnPosition(columnId);

	page = BufferGetPage(info->buffer[vci_MRVGetBlockNumber(mrv)]);

	return (vcis_m_column_t *) &(((char *) page)[vci_MRVGetOffset(mrv)]);
}

/**
 * @brief Obtain the position of vcis_m_extent_t structure for
 * the target extentId.
 *
 * vcis_m_extent_t is the information of extents in VCI main relation.
 *
 * @param[out] blockNumber The block number contains the information is written
 * in * blockNumber.
 * @param[out] offset The offset number contains the information is written
 * in * offset.
 * @param[in] extentId The target extent ID.
 */
void
vci_GetExtentInfoPosition(BlockNumber *blockNumber,
						  OffsetNumber *offset,
						  int32 extentId)
{
	const int	maxExtentInfoInFirstPage = (BLCKSZ -
											(vcimrv_extent_info & VCI_MRV_MASK_OFFSET)) /
		sizeof(vcis_m_extent_t);
	const int	maxExtentInfoInPage = VCI_MAX_PAGE_SPACE /
		sizeof(vcis_m_extent_t);

	Assert(blockNumber);
	Assert(offset);

	if (extentId < maxExtentInfoInFirstPage)
	{
		*blockNumber = vcimrv_extent_info >> VCI_MRV_PAGE_SHIFT;
		*offset = (vcimrv_extent_info & VCI_MRV_MASK_OFFSET) +
			(extentId * sizeof(vcis_m_extent_t));
	}
	else
	{
		int32		extentIdRem = extentId - maxExtentInfoInFirstPage;

		*blockNumber = extentIdRem / maxExtentInfoInPage;
		extentIdRem -= *blockNumber * maxExtentInfoInPage;
		*blockNumber += 1 + (vcimrv_extent_info >> VCI_MRV_PAGE_SHIFT);
		*offset = VCI_MIN_PAGE_HEADER +
			(extentIdRem * sizeof(vcis_m_extent_t));
	}
}

static void
WriteAllItemsInPage(Relation rel,
					Buffer buffer,
					uint16 numItems)
{
	for (uint16 iId = 0; iId < numItems; ++iId)
		vci_WriteItem(rel, buffer, iId + FirstOffsetNumber);
}

/*
 * *********************************************************
 * Relation and buffer control
 * *********************************************************
 */
/*
 * vci_PreparePagesWithOneItemIfNecessary()
 *     This function checks if the relation has the DB page pointed
 *     by an argument.  If it does not exists, the function extends
 *     the relation and initialize extended pages with one item per
 *     page.  Mind that this function does not touch existing pages.
 *     If you need to format existing pages, use vci_InitPage().
 *
 * vci_InitPage()
 *     Low level function.
 *
 *     This function formats the existing DB page, pointed by
 *     relation and page ID (block number), with empty items.
 *     The number of items are also passed by an argument.
 *
 *     vci_PreparePagesWithOneItemIfNecessary() is more convenient.
 *     For pages with one item, the macro vci_InitOneItemPage() is
 *     defined.
 *
 * vci_WriteItem()
 *     Mark the buffer dirty, and write out WAL from the pointed
 *     item in the buffer.
 *
 * vci_WriteOnePageIfNecessaryAndNext()
 *     A utility function.
 *     This function takes new page ID and old page ID in the
 *     arguments.  If they are different, write out the old page,
 *     assumed which is loaded in the given buffer, and read
 *     the new page.
 *     If the page IDs are same, do nothing.
 *
 */

/**
 * @brief This function checks if the relation has the DB page with the page ID
 * blockNumber.
 *
 * When it does not exists, the function extends the relation and initialize
 * extended pages with one item per page.
 *
 * @param[in] rel The relation.
 * @param[in] blockNumber The block number to be examined.
 * @param[in] numItems The number of items the page is initialized with.
 * @param[in] forceInit If true, the block is initialized anyway.
 * @param[in] logItems If true, write all items in the pages into WAL.
 */
void
vci_PreparePagesIfNecessaryCore(Relation rel,
								BlockNumber blockNumber,
								uint16 numItems,
								bool forceInit,
								bool logItems)
{
	BlockNumber existingPages = RelationGetNumberOfBlocks(rel);

	Assert(0 < numItems);

	if (!BlockNumberIsValid(blockNumber))
		ereport(ERROR, (errmsg("data relation full"), errhint("Normally relations of VCI index are smaller than the table relation, therefore this error must not happen.  Disable VCI by 'SELECT vci_disable();'")));

	if (existingPages <= blockNumber)
	{
		for (BlockNumber pId = existingPages; pId <= blockNumber; ++pId)
		{
			Buffer		buffer = ReadBufferExtended(rel, MAIN_FORKNUM,
													P_NEW, RBM_ZERO_AND_LOCK, NULL);

			vci_InitPageCore(buffer, numItems, true);
			if (logItems)
				WriteAllItemsInPage(rel, buffer, numItems);
			UnlockReleaseBuffer(buffer);
		}
	}
	else
	{
		Buffer		buffer = ReadBuffer(rel, blockNumber);
		Page		page = BufferGetPage(buffer);
		bool		needUnlock = false;

		if (PageIsNew(page) || forceInit)
		{
			vci_InitPageCore(buffer, numItems, false);

			if (logItems)
			{
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				WriteAllItemsInPage(rel, buffer, numItems);
				needUnlock = true;
			}
		}
		if (needUnlock)
			UnlockReleaseBuffer(buffer);
		else
			ReleaseBuffer(buffer);
	}
}

/**
 * @brief This function writes a given number of items in the buffer.
 *
 * @param[in] buffer Postgres DB buffer to be initialized.
 * @param[in] numItems The number of items the page is initialized with.
 * @param[in] locked true if the buffer is locked, false otherwise.
 */
void
vci_InitPageCore(Buffer buffer, int16 numItems, bool locked)
{
	if (!locked)
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	{
		uint32		size;
		uint32		itemSize;
		Page		page = BufferGetPage(buffer);
		PageHeader	pageHeader = (PageHeader) page;

		PageInit(page, BLCKSZ, 0);
		pageHeader->pd_lower += sizeof(ItemIdData) * numItems;
		size = pageHeader->pd_upper - pageHeader->pd_lower;
		itemSize = vci_RoundDownValue(size / numItems,
									  VCI_DATA_ALIGNMENT_IN_STORAGE);
		for (int32 aId = numItems; aId--;)
		{
			HeapTupleHeader hTup;

			pageHeader->pd_upper -= itemSize;
			pageHeader->pd_linp[aId].lp_off = pageHeader->pd_upper;
			pageHeader->pd_linp[aId].lp_len = itemSize;
			pageHeader->pd_linp[aId].lp_flags = LP_NORMAL;
			hTup = (HeapTupleHeader) PageGetItem(page, &(pageHeader->pd_linp[aId]));
			hTup->t_infomask2 = 0;
			hTup->t_infomask = HEAP_XMIN_FROZEN | HEAP_XMAX_INVALID;
			hTup->t_hoff = vci_RoundUpValue(offsetof(HeapTupleHeaderData, t_bits),
											VCI_DATA_ALIGNMENT_IN_STORAGE);
		}
		MarkBufferDirty(buffer);
		Assert(pageHeader->pd_lower <= pageHeader->pd_upper);
	}

	if (!locked)
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
}

/**
 * @brief This function get or newly create a DB buffer page, and put the
 * header information that only one item is in the page, and the size of
 * item is 8140 bytes, and the data type is bytea.
 *
 * @param[in] rel The relation.
 * @param[in] blockNumber The block number to be initialized.
 * @param[in] numItems The number of items the page is initialized with.
 */
/*
 * dead code
 * LCOV_EXCL_START
 */
void
vci_InitPage(Relation rel, BlockNumber blockNumber, int16 numItems)
{
	Buffer		buffer;

	Assert(BlockNumberIsValid(blockNumber));
	buffer = ReadBuffer(rel, blockNumber);
	vci_InitPageCore(buffer, numItems, false);
	ReleaseBuffer(buffer);
}

/* LCOV_EXCL_STOP */

/**
 * @brief This function mark the buffer dirty, and make WAL from the item
 * in the buffer.
 *
 * We assume that the relation is only modified by ROS command exclusively.
 * So, we do not put strict lock here.
 *
 * @param[in] rel The relation.
 * @param[in] buffer PostgreSQL DB buffer having the page data.
 * @param[in] numItems The number of items the page is initialized with.
 */
void
vci_WriteItem(Relation rel,
			  Buffer buffer,
			  OffsetNumber offsetNumber)
{
	Page		page = BufferGetPage(buffer);
	ItemId		tup = PageGetItemId(page, offsetNumber);
	HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, tup);

	Assert(BufferIsValid(buffer));
	Assert(OffsetNumberIsValid(offsetNumber));

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(rel))
	{
		xl_heap_inplace xlrec;
		XLogRecPtr	recptr;
		uint8		info = 0;
		uint32		newlen;

		xlrec.offnum = offsetNumber;
		xlrec.dbId = MyDatabaseId;
		xlrec.tsId = MyDatabaseTableSpace;
		xlrec.relcacheInitFileInval = false;
		xlrec.nmsgs = 0;

		/*
		 * originally taken from heap_inplace_update() in
		 * src/backend/access/heap/heapam.c
		 */
		XLogBeginInsert();
		XLogRegisterData(&xlrec, MinSizeOfHeapInplace);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		newlen = VCI_ITEM_SPACE(PageGetMaxOffsetNumber(page));
		XLogRegisterBufData(0, (char *) htup + htup->t_hoff, newlen);

		START_CRIT_SECTION();
		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE | info);

		PageSetLSN(page, recptr);

		END_CRIT_SECTION();
	}
}

/**
 * @brief This function first compares blockNumber and blockNumberOld.
 *
 * If they differ each other, write out the buffer in the DB page of
 * blockNumberOld, and read the DB page of blockNumber.
 * If the are same, do nothing.
 *
 * @param[in] relation The relation.
 * @param[in] blockNumber New page ID.
 * @param[in] blockNumberOld Old page ID.  The data is in buffer.
 * @param[in] buffer The buffer contains the old page.
 * @return buffer contains new page, exclusively locked.
 */
Buffer
vci_WriteOnePageIfNecessaryAndGetBuffer(Relation relation,
										BlockNumber blockNumber,
										BlockNumber blockNumberOld,
										Buffer buffer)
{
	if (blockNumber == blockNumberOld)
		return buffer;
	if (BlockNumberIsValid(blockNumberOld))
	{
		vci_WriteOneItemPage(relation, buffer);
		UnlockReleaseBuffer(buffer);
	}
	buffer = vci_ReadBufferWithPageInit(relation, blockNumber);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	return buffer;
}

/*
 * *********************************************************
 * PostgreSQL Attributes (columns)
 * *********************************************************
 */

/*
 * *********************************************************
 * VCI "columns"
 *     Here, a "column" may have only one data relation,
 *     or a pair of meta data relation and data relation.
 *     It includes delete vector, null vector, TID relation,
 *
 * *********************************************************
 */
/*
 * vci_GetSumOfAttributeIndices()
 *    This function counts up all the VCI "columns" defined
 *    in num_vcis_attribute_type.
 *
 * vci_GetAttrTypeAndIndexFromSumOfIndices()
 *    Get vcis_attribute_type_t and index from given
 *    sequential index.
 */

/**
 * @brief This function counts up all the VCI "columns" defined
 * in num_vcis_attribute_type.
 *
 * @param[in] numColumns Number of normal columns in VCI index.
 * @return number of total columns, not only of indexed columns, but also
 * auxiliary columns.
 */
int
vci_GetSumOfAttributeIndices(int16 numColumns)
{
	int			result = 0;

	for (int aId = 0; aId < num_vcis_attribute_type; ++aId)
		result += vci_GetNumIndexForAttributeType(aId, numColumns);

	return result;
}

/**
 * @brief Get Attribute type defined in vcis_attribute_type_t and
 * index of the target category.
 *
 * @param[out] attrType The attribute type is wirtten in *attrType.
 * @param[out] index The index is wirtten in *index.
 * If no corresponding attribute exists, *index set to -1.
 * @param[in] numColumns The number of normal columns in VCI index.
 * @param[in] sumOfIndex The sequential index of target column.
 */
void
vci_GetAttrTypeAndIndexFromSumOfIndices(vcis_attribute_type_t *attrType,
										int *index,
										int16 numColumns,
										int sumOfIndex)
{
	int			sum = 0;

	*index = 0;
	for (*attrType = 0; *attrType < num_vcis_attribute_type; ++*attrType)
	{
		int			inc = vci_GetNumIndexForAttributeType(*attrType, numColumns);

		if ((sum <= sumOfIndex) && (sumOfIndex < (sum + inc)))
		{
			*index = sumOfIndex - sum;

			return;
		}
		sum += inc;
	}
	*index = -1;
}

/**
 * @brief Calculate the bid ID of null bit vector for given column ID.
 *
 * @param[in] tupleDesc The tuple descriptor of VCI main relation.
 * @param[in] columnId Target column ID.
 * @return The bit ID in null bit vector. For not nullable columns, return -1.
 */
int16
vci_GetBitIdInNullBits(TupleDesc tupleDesc, int16 columnId)
{
	return columnId;
}

/**
 * @brief Get the column widths in the worst case.
 *
 * @param attr Attribute information of the columns.
 * @return The width in the worst case.
 */
int16
vci_GetColumnWorstSize(Form_pg_attribute attr)
{
	if (0 <= attr->attlen)		/* fixed length data */
		return attr->attlen;

	/* variable or long length data */
	if (0 <= attr->atttypmod)
	{
		int32		columnSize;

		switch (attr->atttypid)
		{
				/* for bit(n), varbit(n).  */
			case BITOID:
			case VARBITOID:
				columnSize = VARBITTOTALLEN(attr->atttypmod);
				break;

				/* for numeric(p,q), retrun 'p'+LL .   */
			case NUMERICOID:
				columnSize = (attr->atttypmod >> 16) + VARHDRSZ;
				break;

			case BPCHAROID:
			case VARCHAROID:
				if (attr->atttypmod < VARHDRSZ)
					columnSize = (attr->atttypmod - VARHDRSZ) * MAX_MULTIBYTE_CHAR_LEN + VARHDRSZ;
				else
					columnSize = attr->atttypmod * MAX_MULTIBYTE_CHAR_LEN;
				break;

			default:
				{
#ifdef VCI_USE_COMPACT_VARLENA
					if (attr->atttypmod < VARATT_SHORT_MAX)
						columnSize = attr->atttypmod - VARHDRSZ + VARHDRSZ_SHORT;
					else
						columnSize = attr->atttypmod;
#else
					columnSize = attr->atttypmod;
#endif
				}
				break;
		}

		if (columnSize < MaxHeapTupleSize)
			return (int16) columnSize;
	}

	/* worst size -> MaxHeapTupleSize(8k) */
	/* unlimited data size */
	return MaxHeapTupleSize;

	/*
	 * Large data are externally toasted and the size of tuple including the
	 * large attribute is limited to TOAST_TUPLE_TARGET, which is BLCKSZ / 4
	 * normally. But, UN-TOASTED -> MaxHeapTupleSize.
	 */
}

/**
 * @brief from vci_MainRelHeaderInfo, column IDs in original heap relation
 * and VCI index relation are collected.
 *
 * This function also collect the worst-case sizes of columns.
 * attributes, just packed.
 *
 * @param[out] heapAttrNumList Pointer to an array of AttrNumber.
 * The attribute numbers (column ID) in the heap relation are stored here.
 * The AttrNumber is one-origin.
 * The length of array must be larger than numColumns.
 *
 * @param[out] indxColumnIdList Pointer to an array of int16.
 * The column IDs in the VCI main relation are stored here.
 * This is zero-origin.
 * The length of array must be larger than numColumns.
 *
 * @param[out] columnSizeList Pointer to an array of int16.
 * The worst-case widths are stored here.
 * The length of array must be larger than numColumns.
 *
 * @param[in] numColumn Number of columns defined in VCI index.
 * @param[in] info VCI main relation header information.
 * @param[in] heapOid OID of original PostgreSQL tables.
 * @return sum of columnSizeList.
 */
Size
vci_GetColumnIdsAndSizes(AttrNumber *heapAttrNumList,
						 int16 *indxColumnIdList,
						 int16 *columnSizeList,
						 int numColumn,
						 vci_MainRelHeaderInfo *info,
						 Oid heapOid)
{
	LOCKMODE	lockmode = AccessShareLock;
	Oid			tableOid = info->rel->rd_index->indrelid;
	Relation	tableRel;
	TupleDesc	tupleDesc;
	Size		result = 0;

	tableRel = table_open(tableOid, lockmode);
	tupleDesc = RelationGetDescr(tableRel);

	for (int colId = VCI_FIRST_NORMALCOLUMN_ID; colId < numColumn; ++colId)
	{
		Form_pg_attribute attr;
		vcis_m_column_t *mColumn = vci_GetMColumn(info, colId);
		Buffer		buffer;
		Relation	rel = table_open(mColumn->meta_oid, lockmode);
		vcis_column_meta_t *metaHeader = vci_GetColumnMeta(&buffer, rel);
		int16		attnum = metaHeader->pgsql_attnum;

		heapAttrNumList[colId] = attnum;
		attr = TupleDescAttr(tupleDesc, attnum - 1);

		ReleaseBuffer(buffer);
		table_close(rel, lockmode);

		/*
		 * Previously, "attr->attnum - 1" was used for the right value instead
		 * of the simple sequencial number, colId (The attr is extracted from
		 * indexRel). This was for future expanding to enable to add columns
		 * to or delete ones from VCI after creating. But this is not
		 * implemented. And then, the attr is no longer reliable because real
		 * columns information is stored in the vci_column_ids option not in
		 * indexRel when using vci_create().
		 */
		indxColumnIdList[colId] = colId;

		if (!AttributeNumberIsValid(heapAttrNumList[colId]))
			elog(ERROR, "column not found.");	/* FIXME */

		result += columnSizeList[colId] = vci_GetColumnWorstSize(attr);
	}

	table_close(tableRel, lockmode);

	return result;
}

/**
 * @brief Count number of nullable columns in a tuple descriptor.
 *
 * @param[in] tupleDesc tuple descriptor
 * @return Number of nullable columns in the relation.
 */
int
vci_GetNumberOfNullableColumn(TupleDesc tupleDesc)
{
	int			result = 0;

	for (int aId = 0; aId < tupleDesc->natts; ++aId)
	{
		Assert(!((TupleDescAttr(tupleDesc, aId)->attnotnull)));
		++result;
	}

	return result;
}

/**
 * @brief Sarch for free extent and return the extent ID.
 *
 * This function reads extent information in the ROS main relation and checks
 * if the extent has its xgen and xdel are both InvalidTransactionId.
 * The check is done in vci_isFreeExtent().
 */
static uint32
SearchFreeExtent(vci_MainRelHeaderInfo *info)
{
	int32		numExtents = vci_GetMainRelVar(info, vcimrv_num_extents, 0);
	int32		extentId = numExtents;
	BlockNumber blockNumber;
	OffsetNumber offset;
	Buffer		buffer = InvalidBuffer;
	Page		pageHeader = NULL;

	/* search deleted extent first */

	vcis_m_extent_t *extentInfo;
	vci_meta_item_scanner_t *scan =
		vci_BeginMetaItemScan(info->rel, BUFFER_LOCK_SHARE);

	while ((extentInfo = vci_GetMExtentNext(info, scan)) != NULL)
	{
		if (vci_ExtentIsFree(extentInfo))
		{
			extentId = scan->index;
			break;
		}
	}
	vci_EndMetaItemScan(scan);

	/* if no deleted extent, create a new extent */
	if (extentId == numExtents)
	{
		while (true)
		{
			vcis_m_extent_t *extentInfo_new;
			bool		extentIsFree;

			vci_GetExtentInfoPosition(&blockNumber, &offset, extentId);
			vci_PreparePagesWithOneItemIfNecessary(info->rel, blockNumber);
			buffer = ReadBuffer(info->rel, blockNumber);

			LockBuffer(buffer, BUFFER_LOCK_SHARE);

			pageHeader = BufferGetPage(buffer);
			extentInfo_new = (vcis_m_extent_t *) &(((char *) pageHeader)[offset]);
			Assert(extentInfo_new->xgen == InvalidTransactionId);
			Assert((extentInfo_new->xdel == InvalidTransactionId) || (extentInfo_new->xdel == FrozenTransactionId));
			extentIsFree = vci_ExtentIsFree(extentInfo_new);

			UnlockReleaseBuffer(buffer);

			if (extentIsFree)
				break;
			else
				++extentId;
		}
	}

	return extentId;
}

/**
 * @brief Get free extent Id.
 *
 * This function first check the pointer in main relation to one free extent.
 * It it is not free extent, then scan the main relation to find free one.
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 * @return ID of a free extent.
 */
uint32
vci_GetFreeExtentId(vci_MainRelHeaderInfo *info)
{
	Buffer		buffer;
	int32		extentId;
	vcis_m_extent_t *extentInfo;
	bool		isFreeExtent;

	/* first, check the pointed extent */
	extentId = 0;
	{
		extentInfo = vci_GetMExtent(&buffer, info, extentId);

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		isFreeExtent = vci_ExtentIsFree(extentInfo);
		UnlockReleaseBuffer(buffer);

		if (isFreeExtent)
			return extentId;
	}

	/* scan the VCI main relation to find free extent */
	extentId = SearchFreeExtent(info);
	extentInfo = vci_GetMExtent(&buffer, info, extentId);

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	Assert(vci_ExtentIsFree(extentInfo));
	UnlockReleaseBuffer(buffer);

	return extentId;
}

/*
 * *************
 * ** CAUTION **
 * *************
 * USE vci_WriteExtentInfoInMainRosForWosRosConvInit() IN SOME TRANSACTION.
 * GetCurrentTransactionId() IS USED.
 */

/**
 * @brief The function to call before starting WOS -> ROS conversion to write
 * recovery information.
 *
 * This function write new current ROS ID to the header area of ROS main
 * relation, ROS command, and target extent ID.  It also write
 * InvalidTransactionId at the target extent info.
 *
 * @param[in] info pointer to the target vci_MainRelHeaderInfo.
 * @param[in] extentId target extent ID.
 * @param[in] extentId target common dictionary ID.
 * @param[in] xid transaction ID of this write operation.
 * @param[in] command command of this operation.
 */
void
vci_WriteExtentInfoInMainRosForWriteExtentOrCommonDict(
													   vci_MainRelHeaderInfo *info,
													   int32 extentId,
													   int32 dictionaryId,
													   TransactionId xid,
													   vci_ros_command_t command)
{
	int32		numExtents = vci_GetMainRelVar(info, vcimrv_num_extents, 0);

	Assert(0 <= numExtents);
	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);
	if (numExtents <= extentId)
	{
		BlockNumber blockNumber;
		OffsetNumber offset;

		numExtents = extentId + 1;
		vci_GetExtentInfoPosition(&blockNumber, &offset, extentId);
		vci_PreparePagesWithOneItemIfNecessary(info->rel, blockNumber);
	}
	vci_SetMainRelVar(info, vcimrv_num_extents, 0, numExtents);
	vci_SetMainRelVar(info, vcimrv_current_ros_version, 0, xid);
	vci_SetMainRelVar(info, vcimrv_ros_command, 0, command);
	vci_WriteMainRelVar(info, vci_wmrv_update);
}

vcis_m_extent_t *
vci_GetMExtent(Buffer *buffer, vci_MainRelHeaderInfo *info, int32 extentId)
{
	BlockNumber blockNumber;
	OffsetNumber offset;
	Page		page;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);
	vci_GetExtentInfoPosition(&blockNumber, &offset, extentId);

	/*
	 * info->num_extents_allocated is normally -1. When vci_rc_query ==
	 * info->command, it has the expected number of extents calcuated from
	 * number of blocks in VCI main relation.
	 */
	if (info->num_extents_allocated <= extentId)
		vci_PreparePagesWithOneItemIfNecessary(info->rel, blockNumber);

	*buffer = vci_ReadBufferWithPageInit(info->rel, blockNumber);
	page = BufferGetPage(*buffer);

	return (vcis_m_extent_t *) &(((char *) page)[offset]);
}

vcis_m_extent_t *
vci_GetMExtentNext(vci_MainRelHeaderInfo *info, vci_meta_item_scanner_t *scan)
{
	OffsetNumber offset;
	BlockNumber block;

	if (!scan->inited)
	{
		Page		page;

		scan->max_item = vci_GetMainRelVar(info, vcimrv_num_extents, 0);
		vci_GetExtentInfoPosition(&scan->start_block, &offset, 0);
		vci_GetExtentInfoPosition(&scan->end_block, &offset, scan->max_item);
		scan->item_size = sizeof(vcis_m_extent_t);
		scan->current_block = scan->start_block;

		scan->buffer = ReadBuffer(scan->rel, scan->current_block);
		LockBuffer(scan->buffer, scan->buf_lockmode);

		page = BufferGetPage(scan->buffer);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(scan->buffer);
			return NULL;
		}

		Assert(scan->index == -1);
		Assert(scan->max_item >= 0);

		scan->inited = true;
	}

	scan->index++;

	if (scan->index >= scan->max_item)
		return NULL;

	vci_GetExtentInfoPosition(&block, &offset, scan->index);

	if (scan->current_block != block)
	{
		Page		page;

		Assert(BufferIsValid(scan->buffer));

		if (scan->buf_lockmode == BUFFER_LOCK_EXCLUSIVE)
			vci_WriteOneItemPage(scan->rel, scan->buffer);

		UnlockReleaseBuffer(scan->buffer);

		scan->buffer = ReadBuffer(scan->rel, block);
		scan->current_block = block;

		LockBuffer(scan->buffer, scan->buf_lockmode);

		page = BufferGetPage(scan->buffer);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(scan->buffer);
			return NULL;
		}
	}

	return (vcis_m_extent_t *) &(((char *) BufferGetPage(scan->buffer))[offset]);
}

vci_meta_item_scanner_t *
vci_BeginMetaItemScan(Relation rel, int buf_lock)
{
	vci_meta_item_scanner_t *scan = palloc0_object(vci_meta_item_scanner_t);

	Assert((buf_lock == BUFFER_LOCK_SHARE) || (buf_lock == BUFFER_LOCK_EXCLUSIVE));

	scan->inited = false;

	scan->rel = rel;
	scan->index = -1;

	scan->end_block = InvalidBlockNumber;
	scan->start_block = InvalidBlockNumber;
	scan->buffer = InvalidBuffer;
	scan->current_block = InvalidBlockNumber;
	scan->max_item = 0;
	scan->max_item_in_page = 0;
	scan->item_size = 0;
	scan->buf_lockmode = buf_lock;

	return scan;
}

void
vci_EndMetaItemScan(vci_meta_item_scanner_t *scan)
{
	Assert(scan);

	if (BufferIsValid(scan->buffer))
	{
		if (scan->buf_lockmode == BUFFER_LOCK_EXCLUSIVE)
			vci_WriteOneItemPage(scan->rel, scan->buffer);

		UnlockReleaseBuffer(scan->buffer);
	}

	pfree(scan);
}

void
vci_WriteExtentInfo(vci_MainRelHeaderInfo *info,
					int32 extentId,
					uint32 numRows,
					uint32 numDeletedRows,
					uint32 numDeletedRowsOld,
					TransactionId xgen,
					TransactionId xdel)
{
	Buffer		buffer;
	vcis_m_extent_t *extentInfo = vci_GetMExtent(&buffer, info, extentId);

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	extentInfo->num_rows = numRows;
	extentInfo->num_deleted_rows = numDeletedRows;
	extentInfo->num_deleted_rows_old = numDeletedRowsOld;
	extentInfo->xgen = xgen;
	extentInfo->xdel = xdel;
	extentInfo->flags = 0;
	vci_WriteOneItemPage(info->rel, buffer);
	UnlockReleaseBuffer(buffer);
}

/**
 * @brief This function checks if the extentID is 0 <= extentID and
 * extentID < numExtents written in header part of main relation.
 *
 * If it passes, check the existence of the DB page where the extent ID
 * information is written.
 * It might happen that the page has vanished in some trouble...?
 * In recovery process, the record of the number of extents should be
 * corrected.  If so, elog is better...
 *
 * @param[in] info Pointer to the target vci_MainRelHeaderInfo.
 * @param[in] extentId The target extent ID.
 * @retval true The DB page is allocated for the information with given
 * extent ID.
 * @retval false Need to allocate new DB page for the information.
 */
bool
vci_ExtentInfoExists(vci_MainRelHeaderInfo *info, int32 extentId)
{
	BlockNumber blockNumber;
	OffsetNumber offset;
	int32		numExtents = vci_GetMainRelVar(info, vcimrv_num_extents, 0);

	Assert(0 <= numExtents);
	if (numExtents <= extentId)
		return false;

	if (0 <= info->num_extents_allocated)
		return extentId < info->num_extents_allocated;

	vci_GetExtentInfoPosition(&blockNumber, &offset, extentId);

	return blockNumber < RelationGetNumberOfBlocks(info->rel);
}

static bool
VisibilityCheck(TransactionId objectXidMin,
				TransactionId objectXidMax,
				TransactionId readerXid)
{
	/* visibility from generation */
	bool		result = TransactionIdIsValid(objectXidMin) &&
		(TransactionIdEquals(objectXidMin, FrozenTransactionId) ||
	/* objectXidMin <= readerXid */
		 TransactionIdPrecedesOrEquals(objectXidMin, readerXid));

	if (!result)
		return false;

	/* visibility from deletion */
	return (!TransactionIdIsValid(objectXidMax)) ||
		(TransactionIdIsNormal(objectXidMax) &&
		 NormalTransactionIdPrecedes(readerXid, objectXidMax));
}

/**
 * @brief Test if the extent is visible.
 *
 * @param[in] mExtent Pointer to the extent information.
 * @param[in] xid The transaction ID to access the information.
 * @retval true Visible.
 * @retval false Invisible.
 */
bool
vci_ExtentIsVisible(vcis_m_extent_t *mExtent, TransactionId xid)
{
	return VisibilityCheck(mExtent->xgen, mExtent->xdel, xid);
}

bool
vci_ExtentIsCollectable(vcis_m_extent_t *mExtent, TransactionId wos2rosXid)
{
	bool		result = false;

	if (TransactionIdIsValid(mExtent->xdel))
	{
		result = TransactionIdEquals(mExtent->xdel, FrozenTransactionId) ||
		/* mExtent->xdel < wos2rosXid */
			TransactionIdPrecedes(mExtent->xdel, wos2rosXid);
	}

	return result;
}

bool
vci_ExtentIsFree(vcis_m_extent_t *extentInfo)
{
	return !TransactionIdIsValid(extentInfo->xdel) && !TransactionIdIsValid(extentInfo->xgen);
}

/* -------------------------------------------------- */
/*   Recovery function around VCI Main Relation       */
/* -------------------------------------------------- */

void
vci_UpdateLastRosVersionAndOthers(vci_MainRelHeaderInfo *info)
{
	uint32		val;

	val = vci_GetMainRelVar(info, vcimrv_current_ros_version, 0);
	vci_SetMainRelVar(info, vcimrv_last_ros_version, 0, val);
	val = vci_GetMainRelVar(info, vcimrv_size_mr, 0);
	vci_SetMainRelVar(info, vcimrv_size_mr_old, 0, val);
	val = vci_GetMainRelVar(info, vcimrv_tid_crid_diff_sel, 0);
	vci_SetMainRelVar(info, vcimrv_tid_crid_diff_sel_old, 0, val);

	vci_WriteMainRelVar(info, vci_wmrv_update);
}

void
vci_RecoveryDone(vci_MainRelHeaderInfo *info)
{
	uint32		val;

	val = vci_GetMainRelVar(info, vcimrv_last_ros_version, 0);
	vci_SetMainRelVar(info, vcimrv_current_ros_version, 0, val);

	val = vci_GetMainRelVar(info, vcimrv_size_mr_old, 0);
	vci_SetMainRelVar(info, vcimrv_size_mr, 0, val);

	val = vci_GetMainRelVar(info, vcimrv_tid_crid_diff_sel_old, 0);
	vci_SetMainRelVar(info, vcimrv_tid_crid_diff_sel, 0, val);

	vci_WriteMainRelVar(info, vci_wmrv_update);
}

void
vci_WriteRecoveryRecordDone(vci_MainRelHeaderInfo *info, vci_ros_command_t command,
							TransactionId xid)
{
	vci_SetMainRelVar(info, vcimrv_current_ros_version, 0, xid);
	vci_SetMainRelVar(info, vcimrv_ros_command, 0, command);
	vci_WriteMainRelVar(info, vci_wmrv_update);
}

void
vci_WriteRecoveryRecordForExtentInfo(vci_MainRelHeaderInfo *info, int32 newExtentId, int32 oldExtentId)
{
	/*
	 * ConvertWos2Ros oldExtentId = VCI_INVALID_EXTENT_ID newExtentId = New
	 * Extent
	 *
	 * CollectDeletedRows oldExtentId = Src Extent( -> Unused Extent)
	 * newExtentId = New Extent
	 *
	 * CollectUnusedExtent oldExtentId = Unused Extent newExtentId =
	 * VCI_INVALID_EXTENT_ID
	 */
	vci_SetMainRelVar(info, vcimrv_old_extent_id, 0, oldExtentId);
	vci_SetMainRelVar(info, vcimrv_new_extent_id, 0, newExtentId);
}

void
vci_RecoveryExtentInfo(vci_MainRelHeaderInfo *info, vci_ros_command_t command)
{
	int32		numExtents;
	int32		oldExtentId;
	int32		newExtentId;
	Buffer		s_buffer = InvalidBuffer;
	Buffer		d_buffer = InvalidBuffer;
	vcis_m_extent_t *extentInfo;
	int16		colId;

	numExtents = vci_GetMainRelVar(info, vcimrv_num_extents, 0);
	oldExtentId = vci_GetMainRelVar(info, vcimrv_old_extent_id, 0);
	newExtentId = vci_GetMainRelVar(info, vcimrv_new_extent_id, 0);
	colId = vci_GetMainRelVar(info, vcimrv_working_column_id, 0);

	if (oldExtentId != VCI_INVALID_EXTENT_ID)
	{
		TransactionId recovery_xdel;

		switch (command)
		{
			case vci_rc_collect_deleted:
				Assert(oldExtentId < numExtents);
				recovery_xdel = InvalidTransactionId;
				break;
			case vci_rc_collect_extent:
				/* unuse extent Xdel -> Frozen(2) */
				recovery_xdel = FrozenTransactionId;
				break;
			default:
				Assert(0);
				recovery_xdel = InvalidTransactionId;
				break;
		}

		extentInfo = vci_GetMExtent(&s_buffer, info, oldExtentId);	/* from */

		LockBuffer(s_buffer, BUFFER_LOCK_EXCLUSIVE);
		extentInfo->xdel = recovery_xdel;
		vci_WriteOneItemPage(info->rel, s_buffer);
		UnlockReleaseBuffer(s_buffer);
	}

	if ((newExtentId != VCI_INVALID_EXTENT_ID) && (newExtentId < numExtents))
	{
		Assert((command == vci_rc_wos_ros_conv) || (command == vci_rc_collect_deleted));
		extentInfo = vci_GetMExtent(&d_buffer, info, newExtentId);	/* to */

		LockBuffer(d_buffer, BUFFER_LOCK_EXCLUSIVE);
		extentInfo->xgen = InvalidTransactionId;
		Assert((extentInfo->xdel == InvalidTransactionId) || (extentInfo->xdel == FrozenTransactionId));
		extentInfo->xdel = FrozenTransactionId;
		extentInfo->flags |= VCIS_M_EXTENT_FLAG_ENABLE_RECOVERED_COLID;
		extentInfo->recovered_colid = colId;
		vci_WriteOneItemPage(info->rel, d_buffer);
		UnlockReleaseBuffer(d_buffer);
	}
}

void
vci_WriteRecoveryRecordForUpdateDelVec(vci_MainRelHeaderInfo *info)
{
	vcis_m_extent_t *extentInfo;
	vci_meta_item_scanner_t *scan;

	scan = vci_BeginMetaItemScan(info->rel, BUFFER_LOCK_EXCLUSIVE);
	while ((extentInfo = vci_GetMExtentNext(info, scan)) != NULL)
	{
		extentInfo->num_deleted_rows_old = extentInfo->num_deleted_rows;
	}
	vci_EndMetaItemScan(scan);
}

void
vci_RecoveryUpdateDelVec(vci_MainRelHeaderInfo *info)
{
	vcis_m_extent_t *extentInfo;
	vci_meta_item_scanner_t *scan;

	scan = vci_BeginMetaItemScan(info->rel, BUFFER_LOCK_EXCLUSIVE);
	while ((extentInfo = vci_GetMExtentNext(info, scan)) != NULL)
	{
		extentInfo->num_deleted_rows = extentInfo->num_deleted_rows_old;
	}
	vci_EndMetaItemScan(scan);
}

const char *
vci_GetRosCommandName(vci_ros_command_t command)
{
	switch (command)
	{
		case vci_rc_invalid:
			return "invalid";

		case vci_rc_vacuum:
			return "vacuum";

		case vci_rc_query:
			return "query";

		case vci_rc_drop_index:
			return "drop index";

		case vci_rc_wos_delete:
			return "wos delete";

		case vci_rc_wos_insert:
			return "wos insert";

		case vci_rc_recovery:
			return "recovery";

		case vci_rc_probe:
			return "probe";

		case vci_rc_wos_ros_conv_build:
			return "wos ros conv build";

		case vci_rc_generate_local_ros:
			return "generate local ros";

		case vci_rc_copy_command:
			return "copy command";

		case vci_rc_wos_ros_conv:
			return "wos2ros conversion";

		case vci_rc_update_del_vec:
			return "update delete vector";

		case vci_rc_collect_deleted:
			return "collect deleted rows";

		case vci_rc_collect_extent:
			return "collect extent";

		case vci_rc_update_tid_crid:
			return "update tid-crid tree";

		default:
			return "unknown";
	}
}

static Buffer
ReadBufferWithPageInitCore(Relation reln, BlockNumber blockNumber, int16 numItem)
{
	Buffer		buffer;
	Page		page;

	Assert((reln->rd_rel->relkind == 'i') || (reln->rd_rel->relkind == 'm'));
	buffer = ReadBuffer(reln, blockNumber);

	page = BufferGetPage(buffer);
	if (PageIsNew(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(page))
			vci_InitPageCore(buffer, numItem, true);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}

	return buffer;
}

/**
 * @brief Read a buffer containing the requested block of the requested VCI
 * relation.
 *
 * Same as ReadBuffer(), but initialize new page.
 *
 * We must generally use this function instead of ReadBuffer(), to access a kind
 * of VCI relations except Data WOS, Whiteout WOS, and delete vector. But we
 * don't need to replace ReadBuffer() immediately after vci_PreparePagesIfNecessaryCore().
 *
 * @param[in] reln        The relation.
 * @param[in] blockNumber The block number to be read.
 */
Buffer
vci_ReadBufferWithPageInit(Relation reln, BlockNumber blockNumber)
{
	return ReadBufferWithPageInitCore(reln, blockNumber, 1);
}

/**
 * @brief Read a buffer containing the requested block of the requested delete
 * vector.
 *
 * Same as ReadBuffer(), but initialize new page.
 *
 * We must generally use this function instead of ReadBuffer(), to access a
 * delete vector. But we don't need to replace ReadBuffer() immediately after
 * vci_PreparePagesIfNecessaryCore().
 *
 * @param[in] reln        The relation.
 * @param[in] blockNumber The block number to be read.
 */
Buffer
vci_ReadBufferWithPageInitDelVec(Relation reln, BlockNumber blockNumber)
{
	return ReadBufferWithPageInitCore(reln, blockNumber, VCI_ITEMS_IN_PAGE_FOR_DELETE);
}
