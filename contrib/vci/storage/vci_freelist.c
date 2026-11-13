/*-------------------------------------------------------------------------
 *
 * vci_freelist.c
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_freelist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "vci.h"

#include "vci_freelist.h"
#include "vci_ros.h"
#include "vci_columns.h"

static vcis_free_space_t *GetFreeSpaceT(Page page);
static void UpdatePrevNextFreeSpace(vci_RelationPair *relPair,
									BlockNumber prevFreeBlockNumber,
									BlockNumber nextFreeBlockNumber,
									BlockNumber prev_next,
									BlockNumber next_prev,
									vcis_column_meta_t *columnMeta);

/**
 * function to cast from Page to (vcis_freespace_t *)
 */
static vcis_free_space_t *
GetFreeSpaceT(Page page)
{
	HeapTupleHeader htup;

	htup = (HeapTupleHeader) PageGetItem(page, PageGetItemId(page, VCI_FREESPACE_ITEM_ID));

	return (vcis_free_space_t *) ((char *) htup + htup->t_hoff);
}

vcis_free_space_t *
vci_GetFreeSpace(vci_RelationPair *relPair, BlockNumber blk)
{
	Page		page;

	relPair->bufData = vci_ReadBufferWithPageInit(relPair->data, blk);
	page = BufferGetPage(relPair->bufData);

	return GetFreeSpaceT(page);
}

static void
UpdatePrevNextFreeSpace(vci_RelationPair *relPair,
						BlockNumber prevFreeBlockNumber,
						BlockNumber nextFreeBlockNumber,
						BlockNumber prev_next,
						BlockNumber next_prev,
						vcis_column_meta_t *columnMeta)
{
	/* update link information in previous free space */
	if (BlockNumberIsValid(prevFreeBlockNumber))
	{
		vcis_free_space_t *prevFreePtr = vci_GetFreeSpace(relPair,
														  prevFreeBlockNumber);

		Assert(vci_hasFreeLinkNode(prevFreePtr));
		LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
		prevFreePtr->next_pos = prev_next;
		vci_WriteOneItemPage(relPair->data, relPair->bufData);
		UnlockReleaseBuffer(relPair->bufData);
	}
	else
		columnMeta->free_page_begin_id = prev_next;

	/* update link information in next free space */
	if (BlockNumberIsValid(nextFreeBlockNumber))
	{
		vcis_free_space_t *nextFreePtr = vci_GetFreeSpace(relPair,
														  nextFreeBlockNumber);

		Assert(vci_hasFreeLinkNode(nextFreePtr));
		LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
		nextFreePtr->prev_pos = next_prev;
		vci_WriteOneItemPage(relPair->data, relPair->bufData);
		UnlockReleaseBuffer(relPair->bufData);
	}
	else
		columnMeta->free_page_end_id = next_prev;
}

int32
vci_MakeFreeSpace(vci_RelationPair *relPair,
				  BlockNumber startBlockNumber,
				  BlockNumber *newFSBlockNumber,
				  vcis_free_space_t *newFS,
				  bool coalesce)
{
	int			numMerged = 0;

	vcis_free_space_t *origSpace;
	vcis_free_space_t *freeSpace;

	BlockNumber freeSpacePtr;

	/* -- Start Block -- */
	origSpace = vci_GetFreeSpace(relPair, startBlockNumber);
	newFS->size = origSpace->size;
	ReleaseBuffer(relPair->bufData);

	newFS->type = vcis_free_space;
	newFS->prev_pos = InvalidBlockNumber;
	newFS->next_pos = InvalidBlockNumber;
	*newFSBlockNumber = startBlockNumber;
	numMerged = 1;

	freeSpacePtr = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta)->free_page_begin_id;
	Assert(BlockNumberIsValid(freeSpacePtr));
	ReleaseBuffer(relPair->bufMeta);

	while (BlockNumberIsValid(freeSpacePtr))
	{
		freeSpace = vci_GetFreeSpace(relPair, freeSpacePtr);

		Assert(freeSpacePtr != startBlockNumber);
		Assert(!BlockNumberIsValid(freeSpace->next_pos) ||
			   freeSpace->next_pos != startBlockNumber);

		if (startBlockNumber < freeSpacePtr)
		{
			newFS->prev_pos = InvalidBlockNumber;
			newFS->next_pos = freeSpacePtr;
			ReleaseBuffer(relPair->bufData);
			break;
		}
		else if ((freeSpacePtr < startBlockNumber) &&
				 (startBlockNumber < freeSpace->next_pos))
		{
			newFS->prev_pos = freeSpacePtr;
			newFS->next_pos = freeSpace->next_pos;
			ReleaseBuffer(relPair->bufData);
			break;
		}
		else if (!BlockNumberIsValid(freeSpace->next_pos))
		{
			Assert(freeSpacePtr > startBlockNumber);
			newFS->prev_pos = freeSpace->prev_pos;
			newFS->next_pos = freeSpacePtr;
			ReleaseBuffer(relPair->bufData);
			break;
		}

		freeSpacePtr = freeSpace->next_pos;
		ReleaseBuffer(relPair->bufData);
	}

	if (coalesce)
	{
		if (BlockNumberIsValid(newFS->prev_pos))
		{
			freeSpace = vci_GetFreeSpace(relPair, newFS->prev_pos);
			Assert(vci_hasFreeLinkNode(freeSpace));

			if (newFS->prev_pos + vci_GetNumBlocks(freeSpace->size) ==
				*newFSBlockNumber)
			{
				*newFSBlockNumber = newFS->prev_pos;

				newFS->size += freeSpace->size;
				newFS->prev_pos = freeSpace->prev_pos;

				numMerged++;
				elog(DEBUG2, "privious FreeSpace marged ,size %d! ", newFS->size);
			}
			ReleaseBuffer(relPair->bufData);
		}

		if (BlockNumberIsValid(newFS->next_pos))
		{
			freeSpace = vci_GetFreeSpace(relPair, newFS->next_pos);
			Assert(vci_hasFreeLinkNode(freeSpace));

			if (newFS->next_pos ==
				*newFSBlockNumber + vci_GetNumBlocks(newFS->size))
			{
				newFS->size += freeSpace->size;
				if (freeSpace->size == MaxBlockNumber)
					newFS->size = MaxBlockNumber;
				newFS->next_pos = freeSpace->next_pos;

				numMerged++;
				elog(DEBUG2, "next FreeSpace marged ,size %d! ", newFS->size);

			}
			ReleaseBuffer(relPair->bufData);
		}
	}

	return numMerged;
}

void
vci_AppendFreeSpaceToLinkList(vci_RelationPair *relPair,
							  BlockNumber startBlockNumber,
							  BlockNumber prevFreeBlockNumber,
							  BlockNumber nextFreeBlockNumber,
							  BlockNumber size)
{
	vcis_column_meta_t *columnMeta;
	vcis_free_space_t *freeSpace;
	vcis_extent_type_t type;

	Assert(startBlockNumber != prevFreeBlockNumber);
	Assert(startBlockNumber != nextFreeBlockNumber);

	columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);
	LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);

	freeSpace = vci_GetFreeSpace(relPair, columnMeta->free_page_end_id);
	Assert(vci_hasFreeLinkNode(freeSpace));
	type = freeSpace->type;
	ReleaseBuffer(relPair->bufData);

	/* rebuild freespace */
	UpdatePrevNextFreeSpace(relPair, prevFreeBlockNumber, nextFreeBlockNumber,
							startBlockNumber, startBlockNumber, columnMeta);

	freeSpace = vci_GetFreeSpace(relPair, startBlockNumber);
	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);

	freeSpace->prev_pos = prevFreeBlockNumber;
	freeSpace->next_pos = nextFreeBlockNumber;
	freeSpace->type = type;
	freeSpace->size = size;

	vci_WriteOneItemPage(relPair->data, relPair->bufData);
	UnlockReleaseBuffer(relPair->bufData);

	columnMeta->num_extents -= 1;
	columnMeta->num_free_pages += vci_GetNumBlocks(size);
	columnMeta->num_free_page_blocks += 1;

	vci_WriteColumnMetaDataHeader(relPair->meta, relPair->bufMeta);
	UnlockReleaseBuffer(relPair->bufMeta);
}

void
vci_RemoveFreeSpaceFromLinkList(vci_ColumnRelations *relPair,
								BlockNumber startBlockNumber,
								BlockNumber numExtentPages)
{
	vcis_column_meta_t *columnMeta;
	vcis_free_space_t *freeSpace = vci_GetFreeSpace(relPair, startBlockNumber);
	BlockNumber prevFreeBlockNumber = freeSpace->prev_pos;
	BlockNumber nextFreeBlockNumber = freeSpace->next_pos;
	uint32		size = freeSpace->size;
	vcis_extent_type_t type = freeSpace->type;

	BlockNumber next_prev = prevFreeBlockNumber;
	BlockNumber prev_next = nextFreeBlockNumber;

	BlockNumber numBlocksInCurrentFreeSpace = vci_GetNumBlocks(size);

	Assert(vci_hasFreeLinkNode(freeSpace));
	ReleaseBuffer(relPair->bufData);

	Assert(numExtentPages <= numBlocksInCurrentFreeSpace);

	columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);
	LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * prepare new free space from tail part
	 */
	if (numExtentPages < numBlocksInCurrentFreeSpace)
	{
		vcis_free_space_t *freeSpace_new;
		BlockNumber newFreeBlockNumber = startBlockNumber + numExtentPages;

		freeSpace_new = vci_GetFreeSpace(relPair, newFreeBlockNumber);
		freeSpace_new->type = type;
		freeSpace_new->size = size - (numExtentPages * VCI_MAX_PAGE_SPACE);

		/* it is sentinel */
		if (numBlocksInCurrentFreeSpace == MaxBlockNumber)
		{
			freeSpace_new->size = MaxBlockNumber;
			columnMeta->num_free_pages += numExtentPages;
		}

		/* construct new link */
		freeSpace_new->prev_pos = prevFreeBlockNumber;
		freeSpace_new->next_pos = nextFreeBlockNumber;
		prev_next = next_prev = newFreeBlockNumber;

		++columnMeta->num_free_page_blocks;

		LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
		vci_WriteOneItemPage(relPair->data, relPair->bufData);
		UnlockReleaseBuffer(relPair->bufData);
	}

	UpdatePrevNextFreeSpace(relPair, prevFreeBlockNumber, nextFreeBlockNumber,
							prev_next, next_prev, columnMeta);

	++(columnMeta->num_extents);
	columnMeta->num_free_pages -= numExtentPages;
	--(columnMeta->num_free_page_blocks);

	vci_WriteOneItemPage(relPair->meta, relPair->bufMeta);
	UnlockReleaseBuffer(relPair->bufMeta);
}

BlockNumber
vci_FindFreeSpaceForExtent(vci_RelationPair *relPair, BlockNumber requiredSize)
{
	vcis_column_meta_t *columnMeta;
	vcis_free_space_t *freeSpace;

	BlockNumber freeSpacePtr;
	BlockNumber found = InvalidBlockNumber;
	bool		is_sentinel = false;

	columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);
	freeSpacePtr = columnMeta->free_page_begin_id;

	while (BlockNumberIsValid(freeSpacePtr))
	{
		freeSpace = vci_GetFreeSpace(relPair, freeSpacePtr);
		Assert(vci_hasFreeLinkNode(freeSpace));

		if (vci_GetNumBlocks(freeSpace->size) >= requiredSize)
		{
			found = freeSpacePtr;
			if (!BlockNumberIsValid(freeSpace->next_pos))
				is_sentinel = true;
			ReleaseBuffer(relPair->bufData);
			break;
		}
		freeSpacePtr = freeSpace->next_pos;
		ReleaseBuffer(relPair->bufData);
	}

	if (is_sentinel)
	{
		/*
		 * vci_AppendNewPages(relPair->data, requiredSize +
		 * columnMeta->free_page_end_id - numRelPages + 1);
		 */
		int16		numItems;

		relPair->bufData = ReadBuffer(relPair->data, 0);
		numItems = PageGetMaxOffsetNumber(BufferGetPage(relPair->bufData));
		ReleaseBuffer(relPair->bufData);

		vci_PreparePagesIfNecessary(relPair->data,
									requiredSize + columnMeta->free_page_end_id,
									numItems);
	}

	ReleaseBuffer(relPair->bufMeta);

	return found;
}

void
vci_WriteRecoveryRecordForFreeSpace(vci_RelationPair *relPair,
									int16 colId,
									int16 dictId,
									BlockNumber StartBlockNumber,
									vcis_free_space_t *FS)
{
	vcis_column_meta_t *columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);

	Assert(!BlockNumberIsValid(FS->prev_pos) || FS->prev_pos < StartBlockNumber);
	Assert(!BlockNumberIsValid(FS->next_pos) || StartBlockNumber < FS->next_pos);

	LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);

	columnMeta->new_data_head = StartBlockNumber;
	columnMeta->free_page_prev_id = FS->prev_pos;
	columnMeta->free_page_next_id = FS->next_pos;
	columnMeta->free_page_old_size = FS->size;

	columnMeta->num_extents_old = columnMeta->num_extents;
	columnMeta->num_free_pages_old = columnMeta->num_free_pages;
	columnMeta->num_free_page_blocks_old = columnMeta->num_free_page_blocks;

	vci_WriteOneItemPage(relPair->meta, relPair->bufMeta);
	UnlockReleaseBuffer(relPair->bufMeta);

	vci_SetMainRelVar(relPair->info, vcimrv_working_column_id, 0, colId);
	vci_SetMainRelVar(relPair->info, vcimrv_working_dictionary_id, 0, dictId);
	vci_WriteMainRelVar(relPair->info, vci_wmrv_update);
}

void
vci_InitRecoveryRecordForFreeSpace(vci_MainRelHeaderInfo *info)
{
	vci_SetMainRelVar(info, vcimrv_working_column_id, 0, VCI_INVALID_COLUMN_ID);
}

void
vci_RecoveryFreeSpace(vci_MainRelHeaderInfo *info, vci_ros_command_t command)
{
	LOCKMODE	lockmode = AccessShareLock; /** @todo ? */

	int16		colId;
	vci_ColumnRelations relPairData;
	vci_ColumnRelations *relPair = &relPairData;
	vcis_column_meta_t *columnMeta;

	BlockNumber startBlockNumber;
	BlockNumber prevFreeBlockNumber;
	BlockNumber nextFreeBlockNumber;
	uint32		oldSize;

	int32		extentId;

	/* get last working column */
	colId = vci_GetMainRelVar(info, vcimrv_working_column_id, 0);

	if (colId != VCI_INVALID_COLUMN_ID)
	{
		vci_OpenColumnRelations(relPair, info, colId, lockmode);

		/* get column rel set */
		columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);
		LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);

		/* restore from old fields */
		columnMeta->num_extents = columnMeta->num_extents_old;
		columnMeta->num_free_pages = columnMeta->num_free_pages_old;
		columnMeta->num_free_page_blocks = columnMeta->num_free_page_blocks_old;

		/* read free link list recovery information */
		startBlockNumber = columnMeta->new_data_head;
		prevFreeBlockNumber = columnMeta->free_page_prev_id;
		nextFreeBlockNumber = columnMeta->free_page_next_id;
		oldSize = columnMeta->free_page_old_size;

		vci_WriteColumnMetaDataHeader(relPair->meta, relPair->bufMeta);
		UnlockReleaseBuffer(relPair->bufMeta);

		vci_AppendFreeSpaceToLinkList(relPair, startBlockNumber, prevFreeBlockNumber,
									  nextFreeBlockNumber, oldSize);

		switch (command)
		{
			case vci_rc_wos_ros_conv:
			case vci_rc_collect_deleted:
				extentId = vci_GetMainRelVar(info, vcimrv_new_extent_id, 0);
				break;
			case vci_rc_collect_extent:
				extentId = vci_GetMainRelVar(info, vcimrv_old_extent_id, 0);
				break;
			default:
				extentId = VCI_INVALID_EXTENT_ID;
				break;
		}
		Assert(extentId != VCI_INVALID_EXTENT_ID);

		vci_WriteRawDataExtentInfo(relPair->meta,
								   extentId,
								   InvalidBlockNumber,
								   0,
								   NULL,	/* min */
								   NULL,	/* max */
								   false,
								   false);

		vci_CloseColumnRelations(relPair, lockmode);
	}
}
