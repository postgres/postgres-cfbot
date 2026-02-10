/*-------------------------------------------------------------------------
 *
 * vci_tidcrid.c
 *	  TIDCRID update list and TIDCRID Tree relation handlings
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_tidcrid.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdlib.h>

#include "catalog/storage.h"
#include "utils/tuplesort.h"

#include "vci.h"
#include "vci_freelist.h"
#include "vci_ros.h"
#include "vci_tidcrid.h"

/*
 * Add TID-CRID tree page to the free list if number of free items exceeds
 * VCI_TID_CRID_FREESPACE_THRESHOLD
 */
#define VCI_TID_CRID_FREESPACE_THRESHOLD  (4)

/*
 * Dummy column id for the main relation
 */
#define VCI_TID_CRID_COLID_DUMMY ((int16) 1)

#define VCI_TID_CRID_RECOVERY_CURRENT_VAL (InvalidOffsetNumber)

static void InitializeTidCridUpdateList(Oid relOid);

static void WriteTidCridUpdateList(vci_MainRelHeaderInfo *info, int sel, bool (*callback) (vcis_tidcrid_pair_item_t *item, void *data), void *data);
static void SampleTidCridUpdateList(Relation rel, uint64 count, vcis_tidcrid_pair_list_t *dest);

static vcis_tidcrid_meta_t *vci_GetTidCridMeta(vci_TidCridRelations *relPair);
static vcis_tidcrid_pagetag_t *vci_GetTidCridTag(vci_TidCridRelations *relPair, BlockNumber blk);
static void GetTidCridMetaItemPosition(BlockNumber *blockNumber, uint32 *offset, BlockNumber blkNum);
static vcis_tidcrid_meta_item_t *vci_GetTidCridMetaItem(vci_TidCridRelations *relPair, BlockNumber blkNum);
static char *vci_GetTidCridTreeNode(vci_TidCridRelations *relPair, ItemPointer trunkPtr, int64 leafNo, ItemPointer retPtr);

static void RemoveLeafTidCridTree(vci_TidCridRelations *relPair, ItemPointer trunkPtr, uint32 leafNo);
static void AddNewLeafTidCridTree(vci_TidCridRelations *relPair, ItemPointer trunkPtr, uint32 leafNo);

static uint64 SearchFromTidCridTree(vci_MainRelHeaderInfo *info, ItemPointer tId);

static uint64 SearchCridFromTidCridUpdateListContext(vci_TidCridUpdateListContext *context, ItemPointer tId);
static uint64 SearchCridInBlockRange(vci_TidCridUpdateListContext *context, ItemPointer tId, BlockNumber start, BlockNumber end);
static uint64 SearchCridInBlock(vci_TidCridUpdateListContext *context, ItemPointer tId, vcis_tidcrid_pair_item_t *array, int first, int last);

static OffsetNumber FindFreeItem(vci_TidCridRelations *relPair, BlockNumber freeBlk);

static void SetFreeSpaceBitmap(vci_TidCridRelations *relPair, BlockNumber blk, OffsetNumber bit);
static void UnsetFreeSpaceBitmap(vci_TidCridRelations *relPair, BlockNumber blk, OffsetNumber bit);

static void WriteRecoveryRecordForTidCridTrunk(vci_TidCridRelations *relPair, BlockNumber origBlkno, BlockNumber trunkBlkno, OffsetNumber trunkOffset);
static void WriteRecoveryRecordForTidCridLeaf(vci_TidCridRelations *relPair, ItemPointer trunkPtr, uint32 leafNo, BlockNumber leafBlkno, OffsetNumber leafOffset);
static void WriteRecoveryRecordForTidCridCommon(vci_TidCridRelations *relPair, vcis_tid_crid_op_type_t operation, BlockNumber targetBlkno, uint32 targetInfo, BlockNumber freeBlkno, OffsetNumber freeOffset);

/**
 * function to cast from Page to (vcis_tidcrid_pair_list_t *).
 */
#define vci_GetTidCridPairListT(page) \
	((vcis_tidcrid_pair_list_t *) &((page)[VCI_MIN_PAGE_HEADER]))

#define vci_GetTidCridPairItemT(page) \
	((vcis_tidcrid_pair_item_t *) &((page)[VCI_MIN_PAGE_HEADER]))

#define ROUND_UP(value, size) ((((value) + (size) - 1) / (size)) * (size))

/*
 * Initialize TID-CRID update list and create on the storage
 */
static void
InitializeTidCridUpdateList(Oid relOid)
{
	Relation	rel = table_open(relOid, ShareLock);
	Buffer		buffer;
	Page		page;
	vcis_tidcrid_pair_list_t *pairList;
	BlockNumber blockNumber = VCI_TID_CRID_UPDATE_HEADER_PAGE_ID;

	Assert(offsetof(vcis_tidcrid_pair_list_t, body) == VCI_TID_CRID_UPDATE_PAGE_SPACE);

	vci_PreparePagesWithOneItemIfNecessary(rel, blockNumber);
	buffer = ReadBuffer(rel, blockNumber);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	pairList = vci_GetTidCridPairListT(page);
	pairList->num = 0;

	vci_WriteOneItemPage(rel, buffer);
	UnlockReleaseBuffer(buffer);
	table_close(rel, ShareLock);
}

/*
 * Same as above, but the argument is the main relation info
 */
void
vci_InitializeTidCridUpdateLists(vci_MainRelHeaderInfo *info)
{
	Oid			oid;

	oid = vci_GetMainRelVar(info, vcimrv_tid_crid_update_oid_0, 0);
	InitializeTidCridUpdateList(oid);
	oid = vci_GetMainRelVar(info, vcimrv_tid_crid_update_oid_1, 0);
	InitializeTidCridUpdateList(oid);
}

/*
 * Initialize TID-CRID tree relation and create on the storage
 */
void
vci_InitializeTidCridTree(vci_MainRelHeaderInfo *info)
{
	LOCKMODE	lockmode = ShareLock;

	vci_TidCridRelations relPairData = {0};
	vci_TidCridRelations *relPair = &relPairData;
	vcis_tidcrid_meta_t *tidcridMeta;
	vcis_tidcrid_pagetag_t *tidcridTag;

	vci_OpenTidCridRelations(relPair, info, lockmode);

	/* --- Meta --- */

	vci_FormatPageWithOneItem(relPair->meta,
							  VCI_TID_CRID_DATA_FIRST_PAGE_ID);

	tidcridMeta = vci_GetTidCridMeta(relPair);
	LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);

	tidcridMeta->free_page_begin_id = VCI_TID_CRID_DATA_FIRST_PAGE_ID;
	tidcridMeta->free_page_begin_id_old = VCI_TID_CRID_DATA_FIRST_PAGE_ID;
	tidcridMeta->free_page_end_id = VCI_TID_CRID_DATA_FIRST_PAGE_ID;
	tidcridMeta->free_page_end_id_old = VCI_TID_CRID_DATA_FIRST_PAGE_ID;
	tidcridMeta->free_page_prev_id = InvalidBlockNumber;
	tidcridMeta->free_page_next_id = InvalidBlockNumber;
	tidcridMeta->num_free_pages = 1;
	tidcridMeta->num_free_pages_old = 1;
	tidcridMeta->num_free_page_blocks = 1;
	tidcridMeta->num_free_page_blocks_old = 1;

	tidcridMeta->num = 0;
	tidcridMeta->num_old = 0;
	tidcridMeta->free_block_number = 1;
	tidcridMeta->offset = offsetof(vcis_tidcrid_meta_t, body);

	/* need to set invalid to first item ? */

	vci_WriteOneItemPage(relPair->meta, relPair->bufMeta);
	UnlockReleaseBuffer(relPair->bufMeta);

	/* --- Data --- */

	vci_FormatPageWithItems(relPair->data,
							VCI_TID_CRID_DATA_FIRST_PAGE_ID,
							VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE);

	tidcridTag = vci_GetTidCridTag(relPair, VCI_TID_CRID_DATA_FIRST_PAGE_ID);
	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);

	tidcridTag->size = MaxBlockNumber;
	tidcridTag->type = vcis_tidcrid_type_pagetag;
	tidcridTag->prev_pos = InvalidBlockNumber;
	tidcridTag->next_pos = InvalidBlockNumber;

	tidcridTag->num = 0;

	/* Meta data has already been added, so subtract from the free_size */
	tidcridTag->free_size = VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE - 1;
	tidcridTag->bitmap = 0x1;

	vci_WriteItem(relPair->data, relPair->bufData, VCI_TID_CRID_PAGETAG_ITEM_ID);
	UnlockReleaseBuffer(relPair->bufData);

	vci_CloseTidCridRelations(relPair, lockmode);
}

/* **************************************
 * TID CRID Update List Functions
 * *************************************
 */

/*
 * Open TID-CRID Update List
 *
 * Returns the alloced vci_TidCridUpdateListContext
 */
vci_TidCridUpdateListContext *
vci_OpenTidCridUpdateList(vci_MainRelHeaderInfo *info, int sel)
{
	Oid			oid;
	Buffer		buffer;
	Page		page;
	BlockNumber blkno;
	vcis_tidcrid_pair_list_t *src;
	vci_TidCridUpdateListContext *context;

	context = palloc0_object(vci_TidCridUpdateListContext);

	Assert((0 <= sel) && (sel < 2));
	oid = vci_GetMainRelVar(info, vcimrv_tid_crid_update_oid_0, sel);

	context->info = info;
	context->rel = table_open(oid, AccessShareLock);

	blkno = VCI_TID_CRID_UPDATE_HEADER_PAGE_ID;

	buffer = vci_ReadBufferWithPageInit(context->rel, blkno);

	page = BufferGetPage(buffer);
	src = vci_GetTidCridPairListT(page);

	/* Copy header parts */
	memcpy(&context->header, src, offsetof(vcis_tidcrid_pair_list_t, body));

	ReleaseBuffer(buffer);

	context->count = src->num;

	/* Calculate number of blocks in CRID-TID Update List */
	context->nblocks =
		VCI_TID_CRID_UPDATE_BODY_PAGE_ID + ROUND_UP(context->count, VCI_TID_CRID_UPDATE_PAGE_ITEMS) / VCI_TID_CRID_UPDATE_PAGE_ITEMS;

	return context;
}

/*
 * Close TID-CRID Update List
 */
void
vci_CloseTidCridUpdateList(vci_TidCridUpdateListContext *context)
{
	table_close(context->rel, AccessShareLock);

	pfree(context);
}

/*
 * Read one TID-CRID pair from TID-CRID update list
 */
void
vci_ReadOneBlockFromTidCridUpdateList(vci_TidCridUpdateListContext *context, BlockNumber blkno, vcis_tidcrid_pair_item_t *array)
{
	Buffer		buffer;
	Page		page;

	buffer = vci_ReadBufferWithPageInit(context->rel, blkno);
	page = BufferGetPage(buffer);
	memcpy(array, &page[VCI_MIN_PAGE_HEADER], VCI_TID_CRID_UPDATE_PAGE_SPACE);
	ReleaseBuffer(buffer);
}

/*
 * Get the length of TID-CRID update list
 */
int32
vci_GetTidCridUpdateListLength(vci_MainRelHeaderInfo *info, int sel)
{
	Oid			oid;
	Relation	rel;
	Buffer		buffer;
	Page		page;
	vcis_tidcrid_pair_list_t *src;
	int32		length;
	BlockNumber blockNumber;

	Assert((0 <= sel) && (sel < 2));
	oid = vci_GetMainRelVar(info, vcimrv_tid_crid_update_oid_0, sel);
	rel = table_open(oid, AccessShareLock);

	blockNumber = VCI_TID_CRID_UPDATE_HEADER_PAGE_ID;
	buffer = vci_ReadBufferWithPageInit(rel, blockNumber);
	page = BufferGetPage(buffer);

	src = vci_GetTidCridPairListT(page);
	length = src->num;
	ReleaseBuffer(buffer);

	table_close(rel, AccessShareLock);

	return length;
}

/*
 * Serialize TID-CRID update list
 */
static void
WriteTidCridUpdateList(vci_MainRelHeaderInfo *info,
					   int sel,
					   bool (*callback) (vcis_tidcrid_pair_item_t *item, void *data),
					   void *data)
{
	Oid			oid;
	Relation	rel;
	BlockNumber blockNumber;
	vcis_tidcrid_pair_item_t *array;
	Page		page;
	Buffer		buffer;
	bool		is_terminated = false;
	vcis_tidcrid_pair_list_t tidcrid_pair_list = {0};
	uint64		count = 0;

	array = palloc_array(vcis_tidcrid_pair_item_t, VCI_TID_CRID_UPDATE_PAGE_ITEMS);

	Assert((0 <= sel) && (sel < 2));
	oid = vci_GetMainRelVar(info, vcimrv_tid_crid_update_oid_0, sel);
	rel = table_open(oid, AccessExclusiveLock);

	RelationTruncate(rel, 0);

	vci_PreparePagesWithOneItemIfNecessary(rel, VCI_TID_CRID_UPDATE_HEADER_PAGE_ID);

	blockNumber = VCI_TID_CRID_UPDATE_BODY_PAGE_ID;

	while (!is_terminated)
	{
		int			count_in_page = 0;

		for (int i = 0; i < VCI_TID_CRID_UPDATE_PAGE_ITEMS; i++)
		{
			if (!callback(&array[i], data))
			{
				is_terminated = true;
				break;
			}

			count_in_page++;
		}

		if (count_in_page > 0)
		{
			vci_PreparePagesWithOneItemIfNecessary(rel, blockNumber);
			buffer = ReadBuffer(rel, blockNumber);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buffer);
			memcpy(&page[VCI_MIN_PAGE_HEADER], array, VCI_TID_CRID_UPDATE_PAGE_SPACE);
			vci_WriteOneItemPage(rel, buffer);
			UnlockReleaseBuffer(buffer);

			blockNumber++;
			count += count_in_page;
		}
	}

	/* Write the initial block */
	tidcrid_pair_list.num = count;

	if (count > 0)
		SampleTidCridUpdateList(rel, count, &tidcrid_pair_list);

	buffer = vci_ReadBufferWithPageInit(rel, VCI_TID_CRID_UPDATE_HEADER_PAGE_ID);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	memcpy(&page[VCI_MIN_PAGE_HEADER], &tidcrid_pair_list, offsetof(vcis_tidcrid_pair_list_t, body));
	vci_WriteOneItemPage(rel, buffer);
	UnlockReleaseBuffer(buffer);

	table_close(rel, AccessExclusiveLock);

	vci_SetMainRelVar(info, vcimrv_tid_crid_diff_sel, 0, sel);

	pfree(array);
}

static void
SampleTidCridUpdateList(Relation rel, uint64 count, vcis_tidcrid_pair_list_t *dest)
{
	BlockNumber nblocks;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	nblocks = VCI_TID_CRID_UPDATE_BODY_PAGE_ID + ROUND_UP(count, VCI_TID_CRID_UPDATE_PAGE_ITEMS) / VCI_TID_CRID_UPDATE_PAGE_ITEMS;

	dest->blocks_per_samp =
		ROUND_UP(nblocks - 1 /* Except the header */ , VCI_TID_CRID_UPDATE_CONTEXT_SAMPLES) / VCI_TID_CRID_UPDATE_CONTEXT_SAMPLES;

	blkno = VCI_TID_CRID_UPDATE_BODY_PAGE_ID;

	while (blkno < nblocks)
	{
		buffer = vci_ReadBufferWithPageInit(rel, blkno);
		page = BufferGetPage(buffer);

		Assert(dest->num_samples < VCI_TID_CRID_UPDATE_CONTEXT_SAMPLES);

		dest->sample_tids[dest->num_samples++] = vci_GetTidCridPairItemT(page)[0].page_item_id;

		ReleaseBuffer(buffer);

		blkno += dest->blocks_per_samp;
	}

	/* Put final entry */
	buffer = vci_ReadBufferWithPageInit(rel, nblocks - 1);
	page = BufferGetPage(buffer);

	dest->sample_tids[dest->num_samples++] = vci_GetTidCridPairItemT(page)[(count - 1) % VCI_TID_CRID_UPDATE_PAGE_ITEMS].page_item_id;

	ReleaseBuffer(buffer);

	/* Discard if the final entry is duplicated */
	if (ItemPointerEquals(&dest->sample_tids[dest->num_samples - 1],
						  &dest->sample_tids[dest->num_samples - 2]))
		dest->num_samples--;
}

/* **************************************
 * TID CRID Tree Functions
 * *************************************
 */

/*
 * Open the meta and data relation for TID-CRID tree relation
 *
 * Caller must release via vci_CloseTidCridRelations()
 */
void
vci_OpenTidCridRelations(vci_TidCridRelations *rel,
						 vci_MainRelHeaderInfo *info,
						 LOCKMODE lockmode)
{
	rel->meta = table_open(vci_GetMainRelVar(info, vcimrv_tid_crid_meta_oid, 0), lockmode);
	rel->data = table_open(vci_GetMainRelVar(info, vcimrv_tid_crid_data_oid, 0), lockmode);

	rel->info = info;
}

/*
 * Close TID-CRID tree relation
 */
void
vci_CloseTidCridRelations(vci_TidCridRelations *rel, LOCKMODE lockmode)
{
	if (rel)
	{
		if (RelationIsValid(rel->data))
			table_close(rel->data, lockmode);
		if (RelationIsValid(rel->meta))
			table_close(rel->meta, lockmode);
	}
}

#define vci_GetTidCridMetaT(page) \
	((vcis_tidcrid_meta_t *)& ((page)[VCI_MIN_PAGE_HEADER]))

/*
 * Read metadata from the relation
 */
static vcis_tidcrid_meta_t *
vci_GetTidCridMeta(vci_TidCridRelations *relPair)
{
	Page		page;

	relPair->bufMeta = vci_ReadBufferWithPageInit(relPair->meta, VCI_COLUMN_META_HEADER_PAGE_ID);
	page = BufferGetPage(relPair->bufMeta);

	return vci_GetTidCridMetaT(page);
}

/*
 * Read the metadata in the initial tuple of pages
 */
static vcis_tidcrid_pagetag_t *
vci_GetTidCridTag(vci_TidCridRelations *relPair, BlockNumber blk)
{
	Page		page;
	HeapTupleHeader htup;

	relPair->bufData = vci_ReadBufferWithPageInit(relPair->data, blk);
	page = BufferGetPage(relPair->bufData);

	htup = (HeapTupleHeader) PageGetItem(page,
										 PageGetItemId(page, VCI_TID_CRID_PAGETAG_ITEM_ID));

	return (vcis_tidcrid_pagetag_t *) ((char *) htup + htup->t_hoff);
}

/*
 * Calculate offset (page number and the position in the page) to access the
 * flexible array in meta relation
 */
static void
GetTidCridMetaItemPosition(BlockNumber *blockNumber,
						   uint32 *offset,
						   BlockNumber blkNum)
{
	const int	maxTidCridMetaItemInFirstPage =
		(VCI_MAX_PAGE_SPACE - offsetof(vcis_tidcrid_meta_t, body)) / sizeof(vcis_tidcrid_meta_item_t);
	const int	maxTidCridMetaItem = VCI_MAX_PAGE_SPACE / sizeof(vcis_tidcrid_meta_item_t);

	Assert(blockNumber);
	Assert(offset);

	if (blkNum < maxTidCridMetaItemInFirstPage)
	{
		*blockNumber = 0;
		*offset = VCI_MIN_PAGE_HEADER + offsetof(vcis_tidcrid_meta_t, body) +
			(blkNum * sizeof(vcis_tidcrid_meta_item_t));
	}
	else
	{
		int32		blkNumRem = blkNum - maxTidCridMetaItemInFirstPage;

		*blockNumber = blkNumRem / maxTidCridMetaItem;
		blkNumRem -= *blockNumber * maxTidCridMetaItem;
		*blockNumber += 1;
		*offset = VCI_MIN_PAGE_HEADER +
			(blkNumRem * sizeof(vcis_tidcrid_meta_item_t));
	}
}

/*
 * read an entry from vcis_tidcrid_meta
 */
static vcis_tidcrid_meta_item_t *
vci_GetTidCridMetaItem(vci_TidCridRelations *relPair, BlockNumber blkNum)
{
	BlockNumber blockNumber;
	uint32		offset;
	Page		page;
	BlockNumber currentBlocks = RelationGetNumberOfBlocks(relPair->meta);

	GetTidCridMetaItemPosition(&blockNumber, &offset, blkNum);

	if (blockNumber >= currentBlocks)
		vci_FormatPageWithOneItem(relPair->meta, blockNumber);
	else
		vci_PreparePagesWithOneItemIfNecessary(relPair->meta, blockNumber);

	relPair->bufMeta = ReadBuffer(relPair->meta, blockNumber);
	page = BufferGetPage(relPair->bufMeta);

	return (vcis_tidcrid_meta_item_t *) &(((char *) page)[offset]);
}

/*
 * Returns the pointer to nodes (trunk or leaf)
 */
static char *
vci_GetTidCridTreeNode(vci_TidCridRelations *relPair, ItemPointer trunkPtr, int64 leafNo,
					   ItemPointer retPtr)
{
	Page		page;
	HeapTupleHeader htup;
	vcis_tidcrid_trunk_t *trunk;
	ItemPointerData leafPtrData;
	ItemPointer leafPtr = &leafPtrData;

	Assert(ItemPointerIsValid(trunkPtr));

	relPair->bufData = vci_ReadBufferWithPageInit(relPair->data, ItemPointerGetBlockNumber(trunkPtr));
	page = BufferGetPage(relPair->bufData);
	htup = (HeapTupleHeader) PageGetItem(page, PageGetItemId(page, ItemPointerGetOffsetNumber(trunkPtr)));
	trunk = (vcis_tidcrid_trunk_t *) ((char *) htup + htup->t_hoff);

	if (leafNo == VCI_TID_CRID_TRUNKNODE)
	{
		Assert(retPtr == NULL);

		return (char *) trunk;
	}

	Assert(leafNo >= 0 && leafNo < VCI_TID_CRID_LEAF_CAPACITY);
	leafPtrData = trunk->leaf_item[leafNo]; /* copy */

	ReleaseBuffer(relPair->bufData);

	if (ItemPointerIsValid(leafPtr))
	{
		vcis_tidcrid_pagetag_t *tag PG_USED_FOR_ASSERTS_ONLY;

		tag = vci_GetTidCridTag(relPair, ItemPointerGetBlockNumber(leafPtr));

		Assert(tag->bitmap & (1U << (ItemPointerGetOffsetNumber(leafPtr) - 1)));

		ReleaseBuffer(relPair->bufData);

		relPair->bufData = vci_ReadBufferWithPageInit(relPair->data, ItemPointerGetBlockNumber(leafPtr));
		page = BufferGetPage(relPair->bufData);
		htup = (HeapTupleHeader) PageGetItem(page,
											 PageGetItemId(page, ItemPointerGetOffsetNumber(leafPtr)));

		if (retPtr)
			*retPtr = leafPtrData;

		return (char *) htup + htup->t_hoff;
	}

	return NULL;
}

/*
 * Removes LeafNode
 */
static void
RemoveLeafTidCridTree(vci_TidCridRelations *relPair, ItemPointer trunkPtr, uint32 leafNo)
{
	vcis_tidcrid_leaf_t *leaf PG_USED_FOR_ASSERTS_ONLY;
	vcis_tidcrid_trunk_t *trunk;

	ItemPointerData leafPtrData;
	ItemPointer leafPtr = &leafPtrData;

	/* leaf */
	leaf = (vcis_tidcrid_leaf_t *) vci_GetTidCridTreeNode(relPair, trunkPtr,
														  leafNo, leafPtr);
	ReleaseBuffer(relPair->bufData);
	Assert(leaf);

	/* Write recovery record */
	WriteRecoveryRecordForTidCridLeaf(relPair, trunkPtr, leafNo,
									  ItemPointerGetBlockNumber(leafPtr),
									  ItemPointerGetOffsetNumber(leafPtr));

	UnsetFreeSpaceBitmap(relPair,
						 ItemPointerGetBlockNumber(leafPtr),
						 ItemPointerGetOffsetNumber(leafPtr));

	/* Remove forom the trunk node */
	trunk = (vcis_tidcrid_trunk_t *)
		vci_GetTidCridTreeNode(relPair, trunkPtr, VCI_TID_CRID_TRUNKNODE, NULL);
	Assert(trunk->type == vcis_tidcrid_type_trunk);
	Assert((trunk->bitmap & (UINT64CONST(1) << leafNo)) != 0);

	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
	trunk->bitmap &= ~(UINT64CONST(1) << leafNo);
	MemSet(&trunk->leaf_item[leafNo], 0, sizeof(ItemPointerData));

	vci_WriteItem(relPair->data, relPair->bufData,
				  ItemPointerGetOffsetNumber(trunkPtr));
	UnlockReleaseBuffer(relPair->bufData);
}

/*
 * Add new leaf node
 */
static void
AddNewLeafTidCridTree(vci_TidCridRelations *relPair, ItemPointer trunkPtr, uint32 leafNo)
{
	Page		page;
	HeapTupleHeader htup;
	BlockNumber freeBlk;
	OffsetNumber newOffset;
	vcis_tidcrid_leaf_t *leaf;
	vcis_tidcrid_trunk_t *trunk;
	vcis_tidcrid_pagetag_t *tag;

	ItemPointerData leafPtrData;
	ItemPointer leafPtr = &leafPtrData;

	/* Firstly search from the same page as trunk */
	tag = vci_GetTidCridTag(relPair, ItemPointerGetBlockNumber(trunkPtr));
	Assert(tag->type == vcis_tidcrid_type_pagetag);
	newOffset = vci_GetLowestBit(~tag->bitmap) + 1;
	ReleaseBuffer(relPair->bufData);

	if (newOffset <= VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE)
	{
		/* Free space is found */
		freeBlk = ItemPointerGetBlockNumber(trunkPtr);
	}
	else
	{
		freeBlk = vci_FindFreeSpaceForExtent((vci_ColumnRelations *) relPair, 1);
		newOffset = FindFreeItem(relPair, freeBlk);
	}

	WriteRecoveryRecordForTidCridLeaf(relPair, trunkPtr, leafNo, freeBlk, VCI_TID_CRID_RECOVERY_CURRENT_VAL);

	ItemPointerSet(leafPtr, freeBlk, newOffset);

	/* Connect to the leaf from the trunk */
	trunk = (vcis_tidcrid_trunk_t *)
		vci_GetTidCridTreeNode(relPair, trunkPtr, VCI_TID_CRID_TRUNKNODE, NULL);
	Assert(trunk->type == vcis_tidcrid_type_trunk);
	Assert((trunk->bitmap & (UINT64CONST(1) << leafNo)) == 0);

	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
	trunk->bitmap |= (UINT64CONST(1) << leafNo);
	trunk->leaf_item[leafNo] = *leafPtr;

	vci_WriteItem(relPair->data, relPair->bufData,
				  ItemPointerGetOffsetNumber(trunkPtr));
	UnlockReleaseBuffer(relPair->bufData);

	/* Write a tag to the page */
	SetFreeSpaceBitmap(relPair, freeBlk, newOffset);

	relPair->bufData = vci_ReadBufferWithPageInit(relPair->data, ItemPointerGetBlockNumber(leafPtr));
	page = BufferGetPage(relPair->bufData);
	htup = (HeapTupleHeader) PageGetItem(page, PageGetItemId(page, ItemPointerGetOffsetNumber(leafPtr)));

	leaf = (vcis_tidcrid_leaf_t *) ((char *) htup + htup->t_hoff);
	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);

	leaf->type = vcis_tidcrid_type_leaf;
	leaf->bitmap = UINT64CONST(0);

	for (int i = 0; i < VCI_TID_CRID_LEAF_CAPACITY; i++)
	{
		leaf->crid[i] = vci_GetCridFromUint64(VCI_INVALID_CRID);
	}

	vci_WriteItem(relPair->data, relPair->bufData, newOffset);
	UnlockReleaseBuffer(relPair->bufData);
}

/*
 * Returns the item pointer to the subtree related with original TID
 */
void
vci_GetTidCridSubTree(vci_TidCridRelations *relPair, BlockNumber blkOrig,
					  ItemPointer retPtr)
{
	vcis_tidcrid_meta_item_t *metaItem;
	vcis_tidcrid_pagetag_t *tag PG_USED_FOR_ASSERTS_ONLY;

	metaItem = vci_GetTidCridMetaItem(relPair, blkOrig);
	ItemPointerSet(retPtr, metaItem->block_number, metaItem->item_id);

	if (ItemPointerIsValid(retPtr))
	{
		tag = vci_GetTidCridTag(relPair, metaItem->block_number);

		Assert((tag->bitmap & (UINT64CONST(1) << (metaItem->item_id - 1))) != 0);

		ReleaseBuffer(relPair->bufData);
	}

	ReleaseBuffer(relPair->bufMeta);
}

/*
 * Create a new trunk in the subtree
 */
void
vci_CreateTidCridSubTree(vci_TidCridRelations *relPair, BlockNumber blkOrig,
						 ItemPointer retPtr)
{
	BlockNumber freeBlk;
	OffsetNumber newOffset;

	vcis_tidcrid_trunk_t *trunk;
	vcis_tidcrid_meta_item_t *metaItem;

	Assert(retPtr);

	/* Find the free page from the list */
	freeBlk = vci_FindFreeSpaceForExtent((vci_ColumnRelations *) relPair, 1);

	/* Find the free item from the free page */
	newOffset = FindFreeItem(relPair, freeBlk);

	WriteRecoveryRecordForTidCridTrunk(relPair, blkOrig, freeBlk, VCI_TID_CRID_RECOVERY_CURRENT_VAL);

	/* Set ItemPointer to the meta relation item */
	metaItem = vci_GetTidCridMetaItem(relPair, blkOrig);
	LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);
	metaItem->block_number = freeBlk;
	metaItem->item_id = newOffset;

	vci_WriteOneItemPage(relPair->meta, relPair->bufMeta);
	UnlockReleaseBuffer(relPair->bufMeta);

	/* Write a tag in the page */
	SetFreeSpaceBitmap(relPair, freeBlk, newOffset);

	ItemPointerSet(retPtr, freeBlk, newOffset);
	trunk = (vcis_tidcrid_trunk_t *)
		vci_GetTidCridTreeNode(relPair, retPtr, VCI_TID_CRID_TRUNKNODE, NULL);
	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
	Assert(trunk);

	trunk->type = vcis_tidcrid_type_trunk;
	trunk->bitmap = UINT64CONST(0);

	MemSet((trunk->leaf_item), 0, sizeof(trunk->leaf_item));

	vci_WriteItem(relPair->data, relPair->bufData, newOffset);
	UnlockReleaseBuffer(relPair->bufData);
}

void
vci_UpdateTidCridSubTree(vci_TidCridRelations *relPair, ItemPointer trunkPtr,
						 vcis_tidcrid_pair_list_t *newItems)
{
	for (int i = 0; i < newItems->num; i++)
	{
		vcis_tidcrid_leaf_t *leaf;
		ItemPointerData leafPtrData;
		ItemPointer leafPtr = &leafPtrData;
		int			prevBitCount = 0;
		uint32		offset = ItemPointerGetOffsetNumber(&newItems->body[i].page_item_id) - 1;
		int8		itemIdUpperBits;

		/* Extract upper bits from item_id */
		itemIdUpperBits = (offset >> VCI_TID_CRID_LEAF_CAPACITY_BITS) &
			((1 << VCI_TID_CRID_LEAF_CAPACITY_BITS) - 1);

		Assert(itemIdUpperBits < VCI_TID_CRID_LEAF_CAPACITY);

		leaf = (vcis_tidcrid_leaf_t *) vci_GetTidCridTreeNode(relPair, trunkPtr,
															  itemIdUpperBits, leafPtr);
		if (leaf == NULL)
		{
			AddNewLeafTidCridTree(relPair, trunkPtr, itemIdUpperBits);
			leaf = (vcis_tidcrid_leaf_t *) vci_GetTidCridTreeNode(relPair, trunkPtr,
																  itemIdUpperBits, leafPtr);
		}

		LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);

		prevBitCount = vci_GetBitCount(leaf->bitmap);

		for (; i < newItems->num; i++)
		{
			uint32		innerOffset = ItemPointerGetOffsetNumber(&newItems->body[i].page_item_id) - 1;
			int8		innerItemIdUpperBits;
			int8		itemIdLowerBits;

			/* Extract upper bits from item_id */
			innerItemIdUpperBits = (innerOffset >> VCI_TID_CRID_LEAF_CAPACITY_BITS) &
				((1 << VCI_TID_CRID_LEAF_CAPACITY_BITS) - 1);

			if (itemIdUpperBits != innerItemIdUpperBits)
			{
				i--;
				break;
			}

			/* Extract lower bits from item_id */
			itemIdLowerBits = innerOffset & ((1 << VCI_TID_CRID_LEAF_CAPACITY_BITS) - 1);

			leaf->crid[itemIdLowerBits] = newItems->body[i].crid;

			if (vci_GetUint64FromCrid(leaf->crid[itemIdLowerBits]) == VCI_INVALID_CRID)
				leaf->bitmap &= ~(UINT64CONST(1) << itemIdLowerBits);
			else
				leaf->bitmap |= UINT64CONST(1) << itemIdLowerBits;
		}

		vci_WriteItem(relPair->data, relPair->bufData,
					  ItemPointerGetOffsetNumber(leafPtr));
		UnlockReleaseBuffer(relPair->bufData);

		if (prevBitCount != 0 && leaf->bitmap == 0)
			RemoveLeafTidCridTree(relPair, trunkPtr, itemIdUpperBits);
	}
}

/*
 * Covert TID->CRID from TID-CRID tree
 *
 * Returns CRID corresponds to the given tid, otherwise VCI_INVALID_CRID
 */
static uint64
SearchFromTidCridTree(vci_MainRelHeaderInfo *info, ItemPointer tId)
{
	const LOCKMODE lockmode = AccessShareLock;

	uint64		retVal = VCI_INVALID_CRID;
	ItemPointerData trunkNodeData;
	ItemPointer trunkNode = &trunkNodeData;

	vcis_tidcrid_leaf_t *leaf;

	BlockNumber blk = ItemPointerGetBlockNumber(tId);
	uint32		offset = ItemPointerGetOffsetNumber(tId) - 1;
	int8		itemIdLowerBits;
	int8		itemIdUpperBits;
	vci_TidCridRelations relPairData;
	vci_TidCridRelations *relPair = &relPairData;

	/* Separate item id into uppper/lower parts */
	itemIdLowerBits = offset & ((1 << VCI_TID_CRID_LEAF_CAPACITY_BITS) - 1);
	itemIdUpperBits = (offset >> VCI_TID_CRID_LEAF_CAPACITY_BITS) &
		((1 << VCI_TID_CRID_LEAF_CAPACITY_BITS) - 1);

	vci_OpenTidCridRelations(relPair, info, lockmode);
	vci_GetTidCridSubTree(relPair, blk, trunkNode);

	if (ItemPointerIsValid(trunkNode))
	{
		leaf = (vcis_tidcrid_leaf_t *) vci_GetTidCridTreeNode(relPair, trunkNode, itemIdUpperBits, NULL);
		if (leaf)
		{
			retVal = vci_GetUint64FromCrid(leaf->crid[itemIdLowerBits]);
			ReleaseBuffer(relPair->bufData);
		}
	}

	vci_CloseTidCridRelations(relPair, lockmode);

	return retVal;
}

/*
 * Covert TID to CRID
 *
 * Firstly checks the TID-CRID update list, then search TID-CRID tree
 *
 * @param[in]  context  context for the TID-CRID update list
 * @param[in]  tId      target tid
 * @param[out] fromTree true if the CRID is found from the tree
 *
 * Returns found CID, otherwise VCI_INVALID_CRID
 */
uint64
vci_GetCridFromTid(vci_TidCridUpdateListContext *context, ItemPointer tId, bool *fromTree)
{
	bool		viaTree = false;
	uint64		result = VCI_MOVED_CRID;

	if (context->count > 0)
		result = SearchCridFromTidCridUpdateListContext(context, tId);

	if (result == VCI_MOVED_CRID)
	{
		result = SearchFromTidCridTree(context->info, tId);
		viaTree = true;
	}

	if (fromTree)
		*fromTree = viaTree;

	return result;
}

/*
 * Search tid from TID-CRID update list
 */
static uint64
SearchCridFromTidCridUpdateListContext(vci_TidCridUpdateListContext *context, ItemPointer tId)
{
	int			ret;
	int			min,
				max,
				pivot;
	BlockNumber blk_start,
				blk_end;

	/* Compare with the first sample */
	ret = ItemPointerCompare(tId, &context->header.sample_tids[0]);
	if (ret < 0)				/* tId < context->samp_tids[0] */
		return VCI_MOVED_CRID;

	/* Compare with the last sample */
	ret = ItemPointerCompare(&context->header.sample_tids[context->header.num_samples - 1], tId);
	if (ret < 0)				/* context->samp_tids[context->num_samples -
								 * 1] < tId */
		return VCI_MOVED_CRID;

	min = 0;
	max = context->header.num_samples - 1;

	while (max - min > 1)
	{
		pivot = (min + max) / 2;

		ret = ItemPointerCompare(tId, &context->header.sample_tids[pivot]);

		if (ret < 0)			/* tId < pivot */
			max = pivot;
		else if (0 < ret)		/* pivot < tId */
			min = pivot;
		else
			min = max = pivot;
	}

	blk_start = VCI_TID_CRID_UPDATE_BODY_PAGE_ID + min * context->header.blocks_per_samp;
	blk_end = VCI_TID_CRID_UPDATE_BODY_PAGE_ID + max * context->header.blocks_per_samp + context->header.blocks_per_samp - 1;

	if (context->nblocks <= blk_start)
		blk_start = context->nblocks - 1;

	if (context->nblocks <= blk_end)
		blk_end = context->nblocks - 1;

	return SearchCridInBlockRange(context, tId, blk_start, blk_end);
}

static uint64
SearchCridInBlockRange(vci_TidCridUpdateListContext *context,
					   ItemPointer tId,
					   BlockNumber start, BlockNumber end /* inclusive */ )
{
	bool		found = false;
	uint64		ret = VCI_MOVED_CRID;

	do
	{
		BlockNumber pivot;
		int			first,
					last;
		Buffer		buffer;
		Page		page;
		vcis_tidcrid_pair_item_t *array;
		bool		less_lower_bound;
		bool		more_upper_bound;

		pivot = (start + end) / 2;

		if (pivot < context->nblocks - 1)
		{
			first = 0;
			last = VCI_TID_CRID_UPDATE_PAGE_ITEMS - 1;
		}
		else
		{
			first = 0;
			last = (context->count - 1) % VCI_TID_CRID_UPDATE_PAGE_ITEMS;
		}

		buffer = vci_ReadBufferWithPageInit(context->rel, pivot);
		page = BufferGetPage(buffer);

		array = vci_GetTidCridPairItemT(page);

		less_lower_bound = (ItemPointerCompare(tId, &array[first].page_item_id) < 0);
		more_upper_bound = (ItemPointerCompare(&array[last].page_item_id, tId) < 0);

		if ((start == end) && (less_lower_bound || more_upper_bound))
		{
			found = true;
			ret = VCI_MOVED_CRID;
		}
		else if (less_lower_bound)
		{
			end = pivot;
		}
		else if (more_upper_bound)
		{
			start = pivot + 1;
		}
		else
		{
			found = true;
			ret = SearchCridInBlock(context, tId, array, first, last);
		}

		ReleaseBuffer(buffer);
	} while (!found);

	return ret;
}

/*
 * Search CRID from the one block in TID-CRID update list
 */
static uint64
SearchCridInBlock(vci_TidCridUpdateListContext *context,
				  ItemPointer tId,
				  vcis_tidcrid_pair_item_t *array,
				  int first, int last /* inclusive */ )
{
	int			pivot;

	while (last - first > 1)
	{
		int			ret;

		pivot = (first + last) / 2;

		ret = ItemPointerCompare(&array[pivot].page_item_id, tId);

		if (ret < 0)			/* array[pivot].page_item_id < tId */
			first = pivot;
		else if (ret > 0)		/* array[pivot].page_item_id > tId */
			last = pivot;
		else
			return vci_GetUint64FromCrid(array[pivot].crid);
	}

	if (ItemPointerEquals(&array[first].page_item_id, tId))
		return vci_GetUint64FromCrid(array[first].crid);
	else if (ItemPointerEquals(&array[last].page_item_id, tId))
		return vci_GetUint64FromCrid(array[last].crid);
	else
		return VCI_MOVED_CRID;
}

/*
 * Find free item from pages in data relation of TID-CRID free
 *
 * Returns offset to the free item
 */
static OffsetNumber
FindFreeItem(vci_TidCridRelations *relPair, BlockNumber freeBlk)
{
	vcis_tidcrid_pagetag_t *tag;
	OffsetNumber newOffset;

	tag = vci_GetTidCridTag(relPair, freeBlk);
	Assert(tag->type == vcis_tidcrid_type_pagetag);

	/* Initialize if not done yet */
	if ((tag->bitmap & 1) == 0)
	{
		tag->num = 0;
		tag->free_size = VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE - 1;
		tag->bitmap = 0x1;
	}

	newOffset = vci_GetLowestBit(~tag->bitmap) + 1; /* LSB = 0 */

	Assert((newOffset >= 1) && (newOffset <= VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE));
	ReleaseBuffer(relPair->bufData);

	return newOffset;
}

/*
 * Set a bit to the page tag
 */
static void
SetFreeSpaceBitmap(vci_TidCridRelations *relPair, BlockNumber blk, OffsetNumber offset)
{
	vcis_tidcrid_pagetag_t *tag = vci_GetTidCridTag(relPair, blk);
	uint32		bit = offset - 1;	/* one-origin -> zero-origin */
	uint32		nextBitmap;

	Assert((offset >= 1) && (offset <= VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE));
	Assert((tag->bitmap & (uint32) (1U << bit)) == 0);

	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
	nextBitmap = tag->bitmap | (uint32) (1U << bit);

	/*
	 * Remove from the free space list if the number of free items is less
	 * than threshold
	 */
	if (VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE - vci_GetBitCount(nextBitmap) ==
		VCI_TID_CRID_FREESPACE_THRESHOLD)
	{
		vcis_free_space_t *FS;

		/* Release once to pass relPair to vci_RemoveFreeSpaceFromLinkLis */
		UnlockReleaseBuffer(relPair->bufData);

		FS = vci_GetFreeSpace((vci_RelationPair *) relPair, blk);
		vci_WriteRecoveryRecordForFreeSpace(relPair,
											VCI_TID_CRID_COLID_DUMMY,
											VCI_INVALID_DICTIONARY_ID,
											blk,
											FS);
		ReleaseBuffer(relPair->bufData);

		vci_RemoveFreeSpaceFromLinkList((vci_ColumnRelations *) relPair, blk, 1);

		/* Adjust size and positions */
		tag = vci_GetTidCridTag(relPair, blk);
		LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);

		tag->size = 1;
		tag->prev_pos = blk;
		tag->next_pos = blk;
	}

	tag->bitmap = nextBitmap;
	vci_WriteItem(relPair->data, relPair->bufData, VCI_TID_CRID_PAGETAG_ITEM_ID);
	UnlockReleaseBuffer(relPair->bufData);

}

/*
 * Unset a bit to the page tag
 */
static void
UnsetFreeSpaceBitmap(vci_TidCridRelations *relPair, BlockNumber blk, OffsetNumber offset)
{
	vcis_tidcrid_pagetag_t *tag = vci_GetTidCridTag(relPair, blk);
	int			bit = offset - 1;	/* one-origin -> zero-origin */

	Assert((offset >= 1) && (offset <= VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE));
	Assert((tag->bitmap & (uint32) (1U << bit)) != 0);

	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
	tag->bitmap &= ~(uint32) (1U << bit);
	vci_WriteItem(relPair->data, relPair->bufData, VCI_TID_CRID_PAGETAG_ITEM_ID);
	UnlockReleaseBuffer(relPair->bufData);

	if (VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE - (vci_GetBitCount(tag->bitmap) + 1) ==
		VCI_TID_CRID_FREESPACE_THRESHOLD)
	{
		vcis_free_space_t newFS;
		BlockNumber newFSBlockNumber;

		vci_MakeFreeSpace((vci_ColumnRelations *) relPair, blk, &newFSBlockNumber, &newFS, false);
		Assert(newFSBlockNumber == blk);

		vci_WriteRecoveryRecordForFreeSpace(relPair,
											VCI_TID_CRID_COLID_DUMMY,
											VCI_INVALID_DICTIONARY_ID,
											newFSBlockNumber,
											&newFS);

		vci_AppendFreeSpaceToLinkList((vci_ColumnRelations *) relPair,
									  newFSBlockNumber,
									  newFS.prev_pos,
									  newFS.next_pos,
									  newFS.size);
	}
}

/*
 * Write a recovery record while creating trunk node in the subtree
 */
static void
WriteRecoveryRecordForTidCridTrunk(vci_TidCridRelations *relPair, BlockNumber origBlkno, BlockNumber trunkBlkno, OffsetNumber trunkOffset)
{
	WriteRecoveryRecordForTidCridCommon(relPair, vcis_tid_crid_op_trunk, origBlkno, 0, trunkBlkno, trunkOffset);
}

/*
 * Write a recovery record while creating leaf node
 */
static void
WriteRecoveryRecordForTidCridLeaf(vci_TidCridRelations *relPair, ItemPointer trunkPtr, uint32 leafNo, BlockNumber leafBlkno, OffsetNumber leafOffset)
{
	vcis_tid_crid_op_type_t operation;
	OffsetNumber trunkOffset;
	uint32		targetInfo;

	if (leafOffset == VCI_TID_CRID_RECOVERY_CURRENT_VAL)
		operation = vcis_tid_crid_op_leaf_add;
	else
		operation = vcis_tid_crid_op_leaf_remove;

	trunkOffset = ItemPointerGetOffsetNumber(trunkPtr);
	Assert((trunkOffset <= 0xFFFF) && (leafNo <= 0xFFFF));
	targetInfo = (trunkOffset & 0xFFFF) | ((leafNo & 0xFFFF) << 16);

	WriteRecoveryRecordForTidCridCommon(relPair, operation, ItemPointerGetBlockNumber(trunkPtr), targetInfo, leafBlkno, leafOffset);
}

/*
 * Write a recovery record while updating TID-CRID tree
 */
static void
WriteRecoveryRecordForTidCridCommon(vci_TidCridRelations *relPair, vcis_tid_crid_op_type_t operation, BlockNumber targetBlkno, uint32 targetInfo, BlockNumber freeBlkno, OffsetNumber freeOffset)
{
	vcis_tidcrid_pagetag_t *tag;
	uint32		tag_bitmap;

	/*
	 * 1. Obtains the bitmap to write the meta relation
	 */
	tag = vci_GetTidCridTag(relPair, freeBlkno);
	Assert(tag->type == vcis_tidcrid_type_pagetag);
	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);

	if (freeOffset == VCI_TID_CRID_RECOVERY_CURRENT_VAL)
		tag_bitmap = tag->bitmap;
	else
		tag_bitmap = tag->bitmap & ~(UINT64CONST(1) << (freeOffset - 1));

	UnlockReleaseBuffer(relPair->bufData);

	/* 2. Write information to the meta relation */
	Assert(relPair->info);

	vci_SetMainRelVar(relPair->info, vcimrv_tid_crid_operation, 0, operation);
	vci_SetMainRelVar(relPair->info, vcimrv_tid_crid_target_blocknumber, 0, targetBlkno);
	vci_SetMainRelVar(relPair->info, vcimrv_tid_crid_target_info, 0, targetInfo);
	vci_SetMainRelVar(relPair->info, vcimrv_tid_crid_free_blocknumber, 0, freeBlkno);
	vci_SetMainRelVar(relPair->info, vcimrv_tid_crid_tag_bitmap, 0, tag_bitmap);
	vci_SetMainRelVar(relPair->info, vcimrv_working_column_id, 0, VCI_INVALID_COLUMN_ID);
	vci_WriteMainRelVar(relPair->info, vci_wmrv_update);
}

/*
 * Initialize recovery record for the TID-CRID
 */
void
vci_InitRecoveryRecordForTidCrid(vci_MainRelHeaderInfo *info)
{
	vci_SetMainRelVar(info, vcimrv_tid_crid_operation, 0, vcis_tid_crid_op_none);

	vci_SetMainRelVar(info, vcimrv_working_column_id, 0, VCI_INVALID_COLUMN_ID);
}

/*
 * Recovery the lastly modifying bitmap
 *
 * @param[in] info main relation
 */
void
vci_RecoveryTidCrid(vci_MainRelHeaderInfo *info)
{
	LOCKMODE	lockmode = RowExclusiveLock;

	vci_TidCridRelations relPairData = {0};
	vci_TidCridRelations *relPair = &relPairData;

	vcis_tid_crid_op_type_t operation;
	BlockNumber targetBlkno;
	uint32		targetInfo;
	BlockNumber freeBlkno;
	uint32		tag_bitmap;

	operation = vci_GetMainRelVar(info, vcimrv_tid_crid_operation, 0);
	targetBlkno = vci_GetMainRelVar(info, vcimrv_tid_crid_target_blocknumber, 0);
	targetInfo = vci_GetMainRelVar(info, vcimrv_tid_crid_target_info, 0);
	freeBlkno = vci_GetMainRelVar(info, vcimrv_tid_crid_free_blocknumber, 0);
	tag_bitmap = vci_GetMainRelVar(info, vcimrv_tid_crid_tag_bitmap, 0);

	if (operation == vcis_tid_crid_op_none)
		return;

	Assert(BlockNumberIsValid(freeBlkno));
	vci_OpenTidCridRelations(relPair, info, lockmode);
	{
		vcis_tidcrid_pagetag_t *tag;

		tag = vci_GetTidCridTag(relPair, freeBlkno);
		Assert(tag->type == vcis_tidcrid_type_pagetag);
		LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
		tag->bitmap = tag_bitmap;
		vci_WriteItem(relPair->data, relPair->bufData, VCI_TID_CRID_PAGETAG_ITEM_ID);
		UnlockReleaseBuffer(relPair->bufData);
	}
	vci_CloseTidCridRelations(relPair, lockmode);

	switch (operation)
	{
		case vcis_tid_crid_op_trunk:
			{
				vcis_tidcrid_meta_item_t *metaItem;

				metaItem = vci_GetTidCridMetaItem(relPair, targetBlkno);
				LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);
				metaItem->block_number = InvalidBlockNumber;
				metaItem->item_id = InvalidOffsetNumber;

				vci_WriteOneItemPage(relPair->meta, relPair->bufMeta);
				UnlockReleaseBuffer(relPair->bufMeta);
			}
			break;

		case vcis_tid_crid_op_leaf_add:
		case vcis_tid_crid_op_leaf_remove:
			{
				vcis_tidcrid_trunk_t *trunk;
				ItemPointerData trunkItem;
				uint32		leafNo;

				/*
				 * In vcis_tid_crid_op_leaf, targetBlkno represents a block
				 * number for the trunck, and lower 16 bit of targetInfo is
				 * the offset to the trunk.
				 */
				ItemPointerSet(&trunkItem, targetBlkno, (targetInfo & 0xFFFF));

				/*
				 * Upper 16 bit of targetInfo represents the leafNo in the
				 * trunk.
				 */
				leafNo = targetInfo >> 16;

				trunk = (vcis_tidcrid_trunk_t *)
					vci_GetTidCridTreeNode(relPair, &trunkItem, VCI_TID_CRID_TRUNKNODE, NULL);

				LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
				trunk->bitmap &= ~(UINT64CONST(1) << leafNo);
				MemSet(&trunk->leaf_item[leafNo], 0, sizeof(ItemPointerData));

				vci_WriteItem(relPair->data, relPair->bufData,
							  ItemPointerGetOffsetNumber(&trunkItem));
				UnlockReleaseBuffer(relPair->bufData);
			}
			break;

		default:
			break;
	}
}

/*
 * Recovery the free list for TID-CRID tree relation
 */
void
vci_RecoveryFreeSpaceForTidCrid(vci_MainRelHeaderInfo *info)
{
	LOCKMODE	lockmode = RowExclusiveLock;

	int16		colId;
	vci_ColumnRelations relPairData = {0};
	vci_ColumnRelations *relPair = &relPairData;
	vcis_column_meta_t *columnMeta;

	BlockNumber startBlockNumber;
	BlockNumber prevFreeBlockNumber;
	BlockNumber nextFreeBlockNumber;
	uint32		oldSize;

	vci_OpenTidCridRelations(relPair, info, lockmode);

	/* get last working column */
	colId = vci_GetMainRelVar(info, vcimrv_working_column_id, 0);

	if (colId != VCI_INVALID_COLUMN_ID)
	{
		/* get column rel set */
		columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);
		LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);

		/* restore from old fieleds */
		columnMeta->num_extents = columnMeta->num_extents_old;
		columnMeta->num_free_pages = columnMeta->num_free_pages_old;
		columnMeta->num_free_page_blocks = columnMeta->num_free_page_blocks_old;

		/* read freelink list recovery information */
		startBlockNumber = columnMeta->new_data_head;
		prevFreeBlockNumber = columnMeta->free_page_prev_id;
		nextFreeBlockNumber = columnMeta->free_page_next_id;
		oldSize = columnMeta->free_page_old_size;

		vci_WriteColumnMetaDataHeader(relPair->meta, relPair->bufMeta);
		UnlockReleaseBuffer(relPair->bufMeta);

		/* Recovery the free link list */

		vci_AppendFreeSpaceToLinkList(relPair, startBlockNumber, prevFreeBlockNumber,
									  nextFreeBlockNumber, oldSize);
	}
	else
	{
		/*
		 * Connect to the free list if the previous crash was done before leaf
		 * was removed from the trunk.
		 */
		vcis_tid_crid_op_type_t operation;
		BlockNumber freeBlkno;
		uint32		tag_bitmap;
		vcis_free_space_t newFS;
		BlockNumber newFSBlockNumber;

		operation = vci_GetMainRelVar(info, vcimrv_tid_crid_operation, 0);
		freeBlkno = vci_GetMainRelVar(info, vcimrv_tid_crid_free_blocknumber, 0);
		tag_bitmap = vci_GetMainRelVar(info, vcimrv_tid_crid_tag_bitmap, 0);

		switch (operation)
		{
			case vcis_tid_crid_op_none:
			case vcis_tid_crid_op_trunk:
			case vcis_tid_crid_op_leaf_add:
				break;

			case vcis_tid_crid_op_leaf_remove:
				if (VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE - (vci_GetBitCount(tag_bitmap) + 1) ==
					VCI_TID_CRID_FREESPACE_THRESHOLD)
				{
					vci_MakeFreeSpace((vci_ColumnRelations *) relPair, freeBlkno, &newFSBlockNumber, &newFS, false);
					Assert(newFSBlockNumber == freeBlkno);

					vci_WriteRecoveryRecordForFreeSpace(relPair,
														VCI_TID_CRID_COLID_DUMMY,
														VCI_INVALID_DICTIONARY_ID,
														newFSBlockNumber,
														&newFS);

					vci_AppendFreeSpaceToLinkList((vci_ColumnRelations *) relPair,
												  newFSBlockNumber,
												  newFS.prev_pos,
												  newFS.next_pos,
												  newFS.size);
				}
				break;

			default:
				break;
		}
	}

	vci_CloseTidCridRelations(relPair, lockmode);
}

static int
CmpTidCridPairbyTID(const void *pa, const void *pb)
{
	vcis_tidcrid_pair_item_t *a = (vcis_tidcrid_pair_item_t *) pa;
	vcis_tidcrid_pair_item_t *b = (vcis_tidcrid_pair_item_t *) pb;

	uint64		a_tid = vci_GetTid64FromItemPointer(&a->page_item_id);
	uint64		b_tid = vci_GetTid64FromItemPointer(&b->page_item_id);

	return (a_tid < b_tid) ? -1 : ((b_tid < a_tid) ? 1 : 0);
}

static vcis_tidcrid_pair_item_t *
CreateTidCridUpdateListFromRosChunkStorage(RosChunkStorage *src,
										   int32 extentId)
{
	vcis_tidcrid_pair_item_t *dst;
	int			ptr = 0;
	uint64		crid = vci_CalcCrid64(extentId, 0);
	vcis_tidcrid_pair_item_t temp;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);
	dst = palloc_array(vcis_tidcrid_pair_item_t, src->numTotalRows);
	for (int chunkId = 0; chunkId < src->numFilled; ++chunkId)
	{
		RosChunkBuffer *chunk = src->chunk[chunkId];

		for (uint32 lId = 0; lId < chunk->numFilled; ++lId)
		{
			temp.page_item_id = *(ItemPointerData *)
				&(chunk->tidData[lId * sizeof(ItemPointerData)]);
			temp.crid = vci_GetCridFromUint64(crid);
			dst[ptr++] = temp;
			++crid;
		}
	}

	qsort(dst, ptr, sizeof(vcis_tidcrid_pair_item_t), CmpTidCridPairbyTID);

	return dst;
}

/*
 * Callback structure passed to MergeTidCridUpdateListCallback
 */
typedef struct
{
	/*
	 * oldList: base list for the merge
	 */

	/*
	 * Context for TID-CRID Update List
	 */
	vci_TidCridUpdateListContext *oldListContext;

	/*
	 * Current position in old list
	 */
	uint64		oldListContextIndex;

	/*
	 * Record one block from the oldListContext
	 */
	vcis_tidcrid_pair_item_t oldListInBlock[VCI_TID_CRID_UPDATE_PAGE_ITEMS];

	/*
	 * Position of reading block
	 */
	BlockNumber prevOldListContextBlkno;

	/*
	 * addList1: add different entries to oldList (exclusively used with
	 * addList2)
	 */

	/*
	 * Pair TID-CRID list
	 */
	vcis_tidcrid_pair_item_t *addList1;

	/*
	 * Maximum entries in addList1
	 */
	int32		numAddList1;

	/*
	 * Current position in addList1
	 */
	int32		addList1Index;

	/*
	 * addList2: add different entries to oldList (exclusively used with
	 * addList1)
	 */
	Tuplesortstate *addList2;
	ItemPointerData addList2CurrentTid;
	vcis_Crid	addList2Crid;
	bool		addList2Terminated;

} vci_MergeTidCridUpdateListContext;

/*
 * Callback function passed to WriteTidCridUpdateList()
 *
 * Merge oldList and {addList1, addList2} and outputs with TID ordering.
 */
static bool
MergeTidCridUpdateListCallback(vcis_tidcrid_pair_item_t *item, void *data)
{
	vci_MergeTidCridUpdateListContext *mergeContext = (vci_MergeTidCridUpdateListContext *) data;
	bool		old_entry_valid;
	bool		add_entry_valid;
	vcis_tidcrid_pair_item_t old_item,
				add_item;

retry:
	old_entry_valid = false;
	add_entry_valid = false;

	if (mergeContext->addList1)
	{
		/* addList1 */
		if (mergeContext->addList1Index < mergeContext->numAddList1)
		{
			add_item = mergeContext->addList1[mergeContext->addList1Index];
			add_entry_valid = true;
		}
	}
	else
	{
		/* addList2 */
		if (!mergeContext->addList2Terminated)
		{
			if (!ItemPointerIsValid(&mergeContext->addList2CurrentTid))
			{
				Datum		value;
				bool		isnull;

				if (tuplesort_getdatum(mergeContext->addList2, true, true, &value, &isnull, NULL))
				{
					mergeContext->addList2CurrentTid = *DatumGetItemPointer(value);
				}
				else
				{
					mergeContext->addList2Terminated = true;
					goto get_old_list;
				}
			}

			add_item.page_item_id = mergeContext->addList2CurrentTid;
			add_item.crid = mergeContext->addList2Crid;

			add_entry_valid = true;
		}
	}

get_old_list:
	if (mergeContext->oldListContextIndex < mergeContext->oldListContext->count)
	{
		BlockNumber blkno;

		blkno = VCI_TID_CRID_UPDATE_BODY_PAGE_ID + mergeContext->oldListContextIndex / VCI_TID_CRID_UPDATE_PAGE_ITEMS;

		if (blkno != mergeContext->prevOldListContextBlkno)
		{
			vci_ReadOneBlockFromTidCridUpdateList(mergeContext->oldListContext, blkno, mergeContext->oldListInBlock);
			mergeContext->prevOldListContextBlkno = blkno;
		}

		old_item = mergeContext->oldListInBlock[mergeContext->oldListContextIndex % VCI_TID_CRID_UPDATE_PAGE_ITEMS];

		old_entry_valid = true;
	}

	if (old_entry_valid && add_entry_valid)
	{
		int32		res = ItemPointerCompare(&old_item.page_item_id, &add_item.page_item_id);

		if (res == 0)
		{
			/*
			 * Retain latter one if same TID item has come
			 */
			mergeContext->oldListContextIndex++;
			mergeContext->addList1Index++;
			ItemPointerSetInvalid(&mergeContext->addList2CurrentTid);

			if (vci_GetUint64FromCrid(add_item.crid) == VCI_MOVED_CRID)
				goto retry;

			*item = add_item;
		}
		else if (res < 0)
		{
			mergeContext->oldListContextIndex++;

			*item = old_item;
		}
		else
		{
			mergeContext->addList1Index++;
			ItemPointerSetInvalid(&mergeContext->addList2CurrentTid);

			Assert(vci_GetUint64FromCrid(add_item.crid) != VCI_MOVED_CRID);

			*item = add_item;
		}

		return true;
	}
	else if (old_entry_valid)
	{
		mergeContext->oldListContextIndex++;

		*item = old_item;

		return true;
	}
	else if (add_entry_valid)
	{
		mergeContext->addList1Index++;
		ItemPointerSetInvalid(&mergeContext->addList2CurrentTid);

		Assert(vci_GetUint64FromCrid(add_item.crid) != VCI_MOVED_CRID);

		*item = add_item;

		return true;
	}
	else
	{
		return false;
	}
}

/*
 * Add TID-CRID pair into the TID-CRID Update List
 *
 * @param[in] info     info main relation
 * @param[in] src      extent to be added
 * @param[in] extentId extent id to be added
 */
void
vci_AddTidCridUpdateList(vci_MainRelHeaderInfo *info,
						 RosChunkStorage *src,
						 int32 extentId)
{
	uint32		oldSel = vci_GetMainRelVar(info, vcimrv_tid_crid_diff_sel, 0);
	uint32		newSel = 1 ^ oldSel;
	vci_MergeTidCridUpdateListContext mergeContext = {0};

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);
	mergeContext.oldListContext = vci_OpenTidCridUpdateList(info, oldSel);

	mergeContext.addList1 = CreateTidCridUpdateListFromRosChunkStorage(src, extentId);
	mergeContext.numAddList1 = src->numTotalRows;

	mergeContext.prevOldListContextBlkno = InvalidBlockNumber;

	WriteTidCridUpdateList(info, newSel, MergeTidCridUpdateListCallback, &mergeContext);

	pfree(mergeContext.addList1);
	vci_CloseTidCridUpdateList(mergeContext.oldListContext);
}

void
vci_MergeAndWriteTidCridUpdateList(vci_MainRelHeaderInfo *info,
								   int newSel, int oldSel,
								   Tuplesortstate *newList, vcis_Crid crid)
{
	vci_MergeTidCridUpdateListContext mergeContext = {0};

	mergeContext.oldListContext = vci_OpenTidCridUpdateList(info, oldSel);

	mergeContext.addList2 = newList;
	ItemPointerSetInvalid(&mergeContext.addList2CurrentTid);
	mergeContext.addList2Crid = crid;

	mergeContext.prevOldListContextBlkno = InvalidBlockNumber;

	WriteTidCridUpdateList(info, newSel, MergeTidCridUpdateListCallback, &mergeContext);

	vci_CloseTidCridUpdateList(mergeContext.oldListContext);
}
