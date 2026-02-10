/*-------------------------------------------------------------------------
 *
 * vci_columns.c
 *	  Column store which consists ROS
 *
 *	  Column store consists of a main and a meta relation. Main relation
 *	  consists of some extents and dictionaries. This file contains their
 *	  handlings.
 *
 *	  Also, delete vector is also handled here.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_columns.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdint.h>
#include <limits.h>

#include "access/xact.h"
#include "catalog/index.h"

#include "postgresql_copy.h"

#include "vci.h"
#include "vci_chunk.h"

#include "vci_columns.h"
#include "vci_columns_data.h"

#include "vci_freelist.h"
#include "vci_fetch.h"
#include "vci_mem.h"
#include "vci_ros.h"
#include "vci_tidcrid.h"

#define VCI_LIMIT_INEFFICIENT_COUNT				 (10)
#define GROWTH_NODE								 (10)

#define VCI_MINIMUM_DATA_AMOUNT_FOR_COMMON_DICT	 (64 * 1024 * 1024)

/**
 * function to cast from Page to (vcis_column_meta_t *).
 */
#define vci_GetColumnMetaT(page) \
	((vcis_column_meta_t *) &((page)[VCI_MIN_PAGE_HEADER]))

static void
			UpdateInfoInMetaForFixedLengthRawData(vci_ColumnRelations *rel,
												  int numExtentPages);

static uint32 GetVarlenAHeader(Datum *header,
							   Buffer *buffer,
							   BlockNumber *currentBlockNumber,
							   uint32 offsetInPage,
							   Relation rel);

typedef struct vci_CmpInfo
{
	vci_DictInfo dict_info;

	/*
	 * pointer to compressed data. NULL if no compressed data, or the size of
	 * compressed data is larger than or equal to that of raw. In this case,
	 * the memory areas pointed by compressed_data and compressed_offset
	 * should be freed, and compressed_num_offset should be zero.
	 */
	char	   *compressed_data;

	vci_offset_in_extent_t *compressed_offset;	/* offset of each
												 * VCI_COMPACTION_UNIT_ROW */
	uint32		compressed_num_offset;	/* number of offset */
} vci_CmpInfo;

static void
InitializeCmpInfo(vci_CmpInfo *cmpInfo)
{
	Assert(cmpInfo);
	vci_InitializeDictInfo(&(cmpInfo->dict_info));
	cmpInfo->compressed_data = NULL;
	cmpInfo->compressed_offset = NULL;
	cmpInfo->compressed_num_offset = 0;
}

/* ***************************
 * Extent operation function
 * ***************************
 */
void
vci_WriteRawDataExtentInfo(Relation rel,
						   int32 extentId,
						   uint32 startBlockNumber,
						   uint32 numBlocks,
						   char *minData,
						   char *maxData,
						   bool validMinMax,
						   bool checkOverwrite)
{
	Buffer		bufMeta;
	Buffer		buffer;
	BlockNumber blockNumber;
	vcis_c_extent_t *columnExtent;
	vcis_column_meta_t *columnMeta = vci_GetColumnMeta(&bufMeta, rel);

	Assert(false == validMinMax);

	columnExtent = vci_GetColumnExtent(&buffer,
									   &blockNumber,
									   rel,
									   extentId);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/* vci_MinMaxTypeInfo *mmti = vci_GetMinMaxTypeInfo(attr); */
	if (checkOverwrite)
		if (columnExtent->enabled &&
			(0 != columnExtent->block_number))
			elog(ERROR, "overwrite column meta data");	/* FIXME */

	columnExtent->block_number = startBlockNumber;
	columnExtent->num_blocks = numBlocks;
	columnExtent->enabled = (startBlockNumber != InvalidBlockNumber);
	columnExtent->valid_min_max = validMinMax;

	if (minData)
		memcpy(columnExtent->min, minData, columnMeta->min_max_content_size);

	if (maxData)
		memcpy(&(columnExtent->min[columnMeta->min_max_field_size]),
			   maxData, columnMeta->min_max_content_size);

	vci_WriteOneItemPage(rel, buffer);
	UnlockReleaseBuffer(buffer);
	ReleaseBuffer(bufMeta);
}

static void
WriteFixedLengthRawData(vci_MainRelHeaderInfo *info,
						RosChunkStorage *src,
						int16 columnId,
						int extentId)
{
	vci_ColumnRelations rel;
	int16		columnSize;
	Size		dataSize;
	int			extentHeaderSize;
	int			numExtentPages;
	char		minData[VCI_MAX_MIN_MAX_SIZE];
	char		maxData[VCI_MAX_MIN_MAX_SIZE];
	BlockNumber startBlockNumber;
	BlockNumber blockNumber;
	uint32		offsetInPage;
	Buffer		buffer = InvalidBuffer;
	Page		page = NULL;	/* invalid page */
	LOCKMODE	lockmode = RowExclusiveLock;
	bool		fixedPages = true;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);

	Assert(info);
	Assert(src);
	Assert(0 < src->numFilled);
	Assert(src->chunk[0]);
	Assert((VCI_COLUMN_ID_TID == columnId) ||
		   (VCI_COLUMN_ID_NULL == columnId) ||
		   (columnId < src->chunk[0]->numColumns));

	vci_OpenColumnRelations(&rel, info, columnId, lockmode);

	columnSize = vci_GetFixedColumnSize(info, columnId);

	dataSize = (Size) columnSize * VCI_NUM_ROWS_IN_EXTENT;
	extentHeaderSize = vci_GetExtentFixedLengthRawDataHeaderSize(
																 VCI_NUM_ROWS_IN_EXTENT);
	numExtentPages = vci_GetNumBlocks(dataSize + extentHeaderSize);

	startBlockNumber = extentId * numExtentPages;
	if (VCI_FIRST_NORMALCOLUMN_ID <= columnId)
	{
		vcis_m_column_t *colInfo = vci_GetMColumn(info, columnId);

		switch (colInfo->comp_type)
		{
			case vcis_compression_type_fixed_raw:
				vci_WriteRawDataExtentInfo(rel.meta,
										   extentId,
										   startBlockNumber,
										   numExtentPages,
										   NULL,
										   NULL,
										   false,
										   true);
				UpdateInfoInMetaForFixedLengthRawData(&rel,
													  numExtentPages);
				break;
			default:
				Assert(false);
				elog(ERROR, "internal error");
		}
	}

	vci_PreparePagesWithOneItemIfNecessary(rel.data,
										   startBlockNumber + numExtentPages - 1);

	vci_GetBlockNumberAndOffsetInPage(&blockNumber,
									  &offsetInPage,
									  extentHeaderSize);
	blockNumber += startBlockNumber;
	buffer = vci_ReadBufferWithPageInit(rel.data, blockNumber);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);

	{
		vcis_extent_t *extent = vci_GetExtentT(page);

		extent->size = numExtentPages * VCI_MAX_PAGE_SPACE;
		extent->type = vcis_extent_type_data;
		extent->id = extentId;
		extent->comp_type = vcis_compression_type_fixed_raw;
		extent->offset_offset = 0;
		extent->offset_size = 0;
		extent->data_offset = extentHeaderSize;
		extent->data_size = dataSize;
		extent->compressed = 0;
		extent->dict_offset = 0;
		extent->dict_size = 0;
		extent->dict_type = vcis_dict_type_none;
	}

	for (int chunkId = 0; chunkId < src->numFilled; ++chunkId)
	{
		RosChunkBuffer *chunk = src->chunk[chunkId];
		int			size;

		Assert(chunk);

		size = chunk->numFilled * columnSize;
		for (int written = 0; written < size;)
		{
			int			writeSize;

			if (VCI_MAX_PAGE_SPACE <= offsetInPage)
			{
				if (BufferIsValid(buffer))
				{
					vci_WriteOneItemPage(rel.data, buffer);
					UnlockReleaseBuffer(buffer);
				}
				++blockNumber;
				/* FIXME */

				/*
				 * To obtain better performance, each DB page should be
				 * initialized only when it is accessed for the first time.
				 */
				offsetInPage = 0;
				buffer = ReadBuffer(rel.data, blockNumber);
				vci_InitPageCore(buffer, 1, false);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buffer);
			}

			writeSize = Min(VCI_MAX_PAGE_SPACE - offsetInPage, size - written); /* pgr0063 */
			switch (columnId)
			{
				case VCI_COLUMN_ID_TID:
					memcpy(&(((char *) page)[VCI_MIN_PAGE_HEADER + offsetInPage]),
						   &(chunk->tidData[written]),
						   writeSize);
					break;
				case VCI_COLUMN_ID_NULL:
					memcpy(&(((char *) page)[VCI_MIN_PAGE_HEADER + offsetInPage]),
						   &(chunk->nullData[written]),
						   writeSize);
					break;
				default:
					memcpy(&(((char *) page)[VCI_MIN_PAGE_HEADER + offsetInPage]),
						   &(chunk->data[columnId][written]),
						   writeSize);
					break;
			}
			written += writeSize;
			offsetInPage += writeSize;
		}
	}
	if (BufferIsValid(buffer))
	{
		vci_WriteOneItemPage(rel.data, buffer);
		UnlockReleaseBuffer(buffer);
		++blockNumber;

		if (fixedPages)
		{
			for (; blockNumber < (startBlockNumber + numExtentPages);
				 ++blockNumber)
			{
				buffer = vci_ReadBufferWithPageInit(rel.data, blockNumber);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				vci_WriteOneItemPage(rel.data, buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	vci_WriteRawDataExtentInfo(rel.meta,
							   extentId,
							   startBlockNumber,
							   numExtentPages,
							   minData,
							   maxData,
							   false,
							   false);

	vci_CloseColumnRelations(&rel, lockmode);
}

static void
WriteVariableLengthRawData(vci_MainRelHeaderInfo *info,
						   RosChunkStorage *src,
						   int columnId,
						   int extentId,
						   TransactionId xId)
{
	vci_ColumnRelations rel;
	Size		dataSize;
	int			extentHeaderSize;
	int			numExtentPages;
	int			numCommonDictPages;
	char		minData[VCI_MAX_MIN_MAX_SIZE];
	char		maxData[VCI_MAX_MIN_MAX_SIZE];
	BlockNumber startBlockNumber;
	BlockNumber blockNumber;
	BlockNumber blockNumberOld = InvalidBlockNumber;
	uint32		offsetInPage;
	Buffer		buffer = InvalidBuffer;
	LOCKMODE	lockmode = RowExclusiveLock;
	vcis_extent_t *extent;
	vcis_compression_type_t compType;
	vci_CmpInfo cmpInfo;

	vcis_free_space_t *FS;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);
	Assert(info);
	Assert(src);
	Assert(0 < src->numFilled);
	Assert(src->chunk[0]);
	Assert(columnId < src->chunk[0]->numColumns);

	InitializeCmpInfo(&cmpInfo);

	vci_OpenColumnRelations(&rel, info, columnId, lockmode);

	{
		vcis_m_column_t *colInfo = vci_GetMColumn(info, columnId);

		compType = colInfo->comp_type;
	}
	Assert(compType == vcis_compression_type_variable_raw);

	dataSize = vci_GetDataSizeInChunkStorage(src, columnId, false);

	extentHeaderSize = vci_GetExtentVariableLengthRawDataHeaderSize(
																	src->numTotalRows);

	numExtentPages = vci_GetNumBlocks(dataSize + extentHeaderSize);
	numCommonDictPages = 0;

	startBlockNumber = vci_FindFreeSpaceForExtent(&rel, numExtentPages + numCommonDictPages);
	FS = vci_GetFreeSpace((vci_RelationPair *) &rel, startBlockNumber);
	vci_WriteRecoveryRecordForFreeSpace(&rel, columnId, cmpInfo.dict_info.common_dict_id,
										startBlockNumber, FS);
	ReleaseBuffer(rel.bufData);

	vci_RemoveFreeSpaceFromLinkList(&rel,
									startBlockNumber,
									numExtentPages + numCommonDictPages);

	vci_WriteRawDataExtentInfo(rel.meta,
							   extentId,
							   startBlockNumber,
							   numExtentPages + numCommonDictPages,
							   NULL,	/* min */
							   NULL,	/* max */
							   false,
							   true);

	/* write the header part of extent data in data relation */
	blockNumberOld = blockNumber = startBlockNumber + numCommonDictPages;
	buffer = vci_ReadBufferWithPageInit(rel.data, blockNumber);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	extent = vci_GetExtentT(BufferGetPage(buffer));
	extent->size = numExtentPages * VCI_MAX_PAGE_SPACE;
	extent->type = vcis_extent_type_data;
	extent->id = extentId;
	extent->comp_type = compType;
	extent->offset_offset = offsetof(vcis_extent_t, dict_body);
	extent->offset_size = vci_GetOffsetArraySize(src->numTotalRows);
	extent->data_offset = extentHeaderSize;
	extent->data_size = dataSize;
	extent->compressed = 0;
	extent->dict_offset = VCI_INVALID_DICTIONARY_ID;
	extent->dict_size = 0;
	extent->dict_type = vcis_dict_type_none;

	/* write offset data */
	/***************
	 * ** CAUTION **
	 * *************
	 * Here, we only record pointers of the head of each
	 * VCI_COMPACTION_UNIT_ROW entries.
	 */
	vci_GetBlockNumberAndOffsetInPage(&blockNumber,
									  &offsetInPage,
									  extent->offset_offset);
	blockNumber += startBlockNumber + numCommonDictPages;

	{							/* raw data */
		/*
		 * Make offset data
		 */
#ifdef USE_ASSERT_CHECKING
		uint32		numRowSamples = vci_GetOffsetArrayLength(src->numTotalRows);
#endif							/* #ifdef USE_ASSERT_CHECKING */
		uint32		offsetSize = vci_GetOffsetArraySize(src->numTotalRows);
		vci_offset_in_extent_t *offset = palloc(offsetSize);
		int			rowId = 0;
		int			globalOffset = 0;
		int			offsetPtr = 0;

		for (int chunkId = 0; chunkId < src->numFilled; ++chunkId)
		{
			RosChunkBuffer *chunk = src->chunk[chunkId];
			vci_offset_in_extent_t *dataOffset = chunk->dataOffset[columnId];
			int			elemId = rowId % VCI_COMPACTION_UNIT_ROW;

			for (; elemId < chunk->numFilled;
				 elemId += VCI_COMPACTION_UNIT_ROW)
			{
				offset[offsetPtr++] = globalOffset + dataOffset[elemId];
			}
			rowId += chunk->numFilled;
			globalOffset += dataOffset[chunk->numFilled] - dataOffset[0];
		}
		Assert(rowId == src->numTotalRows);
		Assert(globalOffset == vci_GetDataSizeInChunkStorage(src, columnId,
															 false));
		Assert(offsetPtr == (numRowSamples - 1));
		offset[offsetPtr] = globalOffset;

		buffer = vci_WriteDataIntoMultiplePages(rel.data,
												&blockNumber,
												&blockNumberOld,
												&offsetInPage,
												buffer,
												offset,
												offsetSize);
		pfree(offset);
	}

	/* write data */
	vci_GetBlockNumberAndOffsetInPage(&blockNumber,
									  &offsetInPage,
									  extent->data_offset);
	blockNumber += startBlockNumber + numCommonDictPages;

	{
		for (int chunkId = 0; chunkId < src->numFilled; ++chunkId)
		{
			RosChunkBuffer *chunk = src->chunk[chunkId];
			vci_offset_in_extent_t *dataOffset = chunk->dataOffset[columnId];
			int			size = dataOffset[chunk->numFilled] - dataOffset[0];

			Assert(chunk);

			buffer = vci_WriteDataIntoMultiplePages(rel.data,
													&blockNumber, &blockNumberOld, &offsetInPage,
													buffer,
													chunk->data[columnId], size);
		}
	}

	if (BufferIsValid(buffer))
	{
		vci_WriteOneItemPage(rel.data, buffer);
		UnlockReleaseBuffer(buffer);
	}

	vci_WriteRawDataExtentInfo(rel.meta,
							   extentId,
							   startBlockNumber + numCommonDictPages,
							   numExtentPages,
							   minData,
							   maxData,
							   false,
							   false);

	vci_CloseColumnRelations(&rel, lockmode);
}

static void
WriteDeleteVector(vci_MainRelHeaderInfo *info,
				  RosChunkStorage *src,
				  int extentId)
{
	vci_ColumnRelations rel;
	LOCKMODE	lockmode = RowExclusiveLock;
	int			numExtentPages = VCI_NUM_PAGES_IN_EXTENT_FOR_DELETE;
	BlockNumber startBlockNumber = numExtentPages * extentId;

	Buffer		buffer;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);

	vci_OpenColumnRelations(&rel, info, VCI_COLUMN_ID_DELETE, lockmode);

	vci_WriteRawDataExtentInfo(rel.meta,
							   extentId,
							   startBlockNumber,
							   numExtentPages,
							   NULL,
							   NULL,
							   false,
							   false /* don't check ovwerwrite */ );
	UpdateInfoInMetaForFixedLengthRawData(&rel,
										  numExtentPages);
	{
		for (int rId = 0; rId < numExtentPages; ++rId)
		{
			vci_PreparePagesIfNecessaryCore(rel.data,
											startBlockNumber + rId,
											VCI_ITEMS_IN_PAGE_FOR_DELETE,
											true,
											true);

			buffer = ReadBuffer(rel.data, startBlockNumber + rId);
			vci_InitPageCore(buffer, VCI_ITEMS_IN_PAGE_FOR_DELETE, false);
			ReleaseBuffer(buffer);
		}
	}

	for (int chunkId = 0; chunkId < src->numFilled; ++chunkId)
	{
		RosChunkBuffer *chunk = src->chunk[chunkId];

		if (chunk->deleteData)
		{
			abort();			/* FIXME */
		}
	}

	vci_WriteRawDataExtentInfo(rel.meta,
							   extentId,
							   startBlockNumber,
							   numExtentPages,
							   NULL,
							   NULL,
							   false,
							   false);

	vci_CloseColumnRelations(&rel, lockmode);
}

void
vci_WriteOneExtent(vci_MainRelHeaderInfo *info,
				   RosChunkStorage *src,
				   int extentId,
				   TransactionId xgen,	/* xgen in extent info */
				   TransactionId xdel,	/* xdel in extent info */
				   TransactionId xid)	/* in tuple header */
{
	Assert(src);
	if (src->numTotalRows < 1)
		return;
	Assert(src->chunk[0]);
	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);

	WriteDeleteVector(info, src, extentId);
	WriteFixedLengthRawData(info, src, VCI_COLUMN_ID_TID, extentId);
	WriteFixedLengthRawData(info, src, VCI_COLUMN_ID_NULL, extentId);
	for (int16 colId = VCI_FIRST_NORMALCOLUMN_ID; colId < src->chunk[0]->numColumns; ++colId)
	{
		CHECK_FOR_INTERRUPTS();

		switch (src->chunk[0]->compType[colId])
		{
			case vcis_compression_type_fixed_raw:
				WriteFixedLengthRawData(info, src, colId, extentId);
				break;
			case vcis_compression_type_variable_raw:
				WriteVariableLengthRawData(info, src, colId, extentId, xid);
				break;
			default:
				elog(ERROR, "unsupported compression type");	/* FIXME */
		}
	}
	vci_WriteExtentInfo(info,
						extentId,
						src->numTotalRows,
						0,
						0,
						xgen,
						xdel);
}

void
vci_InitializeDictInfo(vci_DictInfo *dictInfo)
{
	Assert(dictInfo);
	dictInfo->dictionary_storage = NULL;
	dictInfo->storage_size = 0;
	dictInfo->extent_id = VCI_INVALID_EXTENT_ID;
	dictInfo->common_dict_id = VCI_INVALID_DICTIONARY_ID;
	dictInfo->dict_type = vcis_dict_type_none;
}

vcis_c_extent_t *
vci_GetColumnExtent(Buffer *buffer,
					BlockNumber *blockNumber,
					Relation rel,
					int32 extentId)
{
	Page		page;
	vcis_column_meta_t *columnMeta = vci_GetColumnMeta(buffer, rel);

	/* vci_MinMaxTypeInfo *mmti = vci_GetMinMaxTypeInfo(attr); */
	int			columnExtentSize;
	int			headerSize = offsetof(vcis_column_meta_t, common_dict_info) +
		(sizeof(vcis_c_common_dict_t) * columnMeta->num_common_dicts);
	int			numExtentsInFirstPage;
	int			numExtentsInPage;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);

	*blockNumber = VCI_NUM_COLUMN_META_HEADER_PAGE - 1;
	columnExtentSize = offsetof(vcis_c_extent_t, min) + (2 * columnMeta->min_max_field_size);
	numExtentsInFirstPage = (VCI_MAX_PAGE_SPACE - headerSize) /
		columnExtentSize;
	if (extentId < numExtentsInFirstPage)
		return (vcis_c_extent_t *) &(((char *) columnMeta)
									 [headerSize + (extentId * columnExtentSize)]);
	ReleaseBuffer(*buffer);

	extentId -= numExtentsInFirstPage;
	numExtentsInPage = VCI_MAX_PAGE_SPACE / columnExtentSize;
	*blockNumber = extentId / numExtentsInPage;
	extentId -= *blockNumber * numExtentsInPage;
	*blockNumber += VCI_NUM_COLUMN_META_HEADER_PAGE;
	vci_PreparePagesWithOneItemIfNecessary(rel, *blockNumber);
	*buffer = ReadBuffer(rel, *blockNumber);
	page = BufferGetPage(*buffer);

	return (vcis_c_extent_t *) &(((char *) page)
								 [VCI_MIN_PAGE_HEADER + (extentId * columnExtentSize)]);
}

vcis_column_meta_t *
vci_GetColumnMeta(Buffer *buffer, Relation rel)
{
	Page		page;

	*buffer = vci_ReadBufferWithPageInit(rel, VCI_COLUMN_META_HEADER_PAGE_ID);
	page = BufferGetPage(*buffer);

	return vci_GetColumnMetaT(page);
}

static void
GetColumnOids(Oid *metaOid,
			  Oid *dataOid,
			  vci_MainRelHeaderInfo *info,
			  int16 columnId)
{
	switch (columnId)
	{
		case VCI_COLUMN_ID_DELETE:
			*metaOid = vci_GetMainRelVar(info, vcimrv_delete_meta_oid, 0);
			*dataOid = vci_GetMainRelVar(info, vcimrv_delete_data_oid, 0);
			break;
		case VCI_COLUMN_ID_CRID:
			*metaOid = InvalidOid;
			*dataOid = InvalidOid;
			break;
		case VCI_COLUMN_ID_TID:
			*metaOid = vci_GetMainRelVar(info, vcimrv_tid_meta_oid, 0);
			*dataOid = vci_GetMainRelVar(info, vcimrv_tid_data_oid, 0);
			break;
		case VCI_COLUMN_ID_NULL:
			*metaOid = vci_GetMainRelVar(info, vcimrv_null_meta_oid, 0);
			*dataOid = vci_GetMainRelVar(info, vcimrv_null_data_oid, 0);
			break;
		default:
			{
				vcis_m_column_t *colInfo = vci_GetMColumn(info, columnId);

				*metaOid = colInfo->meta_oid;
				*dataOid = colInfo->data_oid;
				break;
			}
	}
}

void
vci_OpenColumnRelations(vci_ColumnRelations *rel,
						vci_MainRelHeaderInfo *info,
						int16 columnId,
						LOCKMODE lockmode)
{
	Oid			metaOid;
	Oid			dataOid;

	GetColumnOids(&metaOid, &dataOid, info, columnId);
	rel->meta = table_open(metaOid, lockmode);
	rel->data = table_open(dataOid, lockmode);

	rel->info = info;
}

void
vci_CloseColumnRelations(vci_ColumnRelations *rel, LOCKMODE lockmode)
{
	if (rel)
	{
		if (RelationIsValid(rel->data))
			table_close(rel->data, lockmode);
		if (RelationIsValid(rel->meta))
			table_close(rel->meta, lockmode);
	}
}

static void
UpdateInfoInMetaForFixedLengthRawData(vci_ColumnRelations *rel,
									  int numExtentPages)
{
	vcis_column_meta_t *columnMeta;

	if (0 == numExtentPages)
		return;
	columnMeta = vci_GetColumnMeta(&rel->bufMeta, rel->meta);
	if (0 < numExtentPages)		/* an extent added */
	{
		++(columnMeta->num_extents);
		if (columnMeta->num_free_pages < numExtentPages)
			columnMeta->num_free_pages = 0;
		else
			columnMeta->num_free_pages = columnMeta->num_free_pages -
				numExtentPages;
		if (0 < columnMeta->num_free_page_blocks)
			--(columnMeta->num_free_page_blocks);
	}
	else						/* an extent deleted */
	{
		Assert(0 < columnMeta->num_extents);
		--(columnMeta->num_extents);
		columnMeta->num_free_pages -= numExtentPages;
		++(columnMeta->num_free_page_blocks);
	}

	LockBuffer(rel->bufMeta, BUFFER_LOCK_EXCLUSIVE);
	vci_WriteOneItemPage(rel->meta, rel->bufMeta);
	UnlockReleaseBuffer(rel->bufMeta);
}

static uint32
GetVarlenAHeader(Datum *header,
				 Buffer *buffer,
				 BlockNumber *currentBlockNumber,
				 uint32 offsetInPage,
				 Relation rel)
{
	Page		page = BufferGetPage(*buffer);
	char	   *curPtr = &(page[VCI_MIN_PAGE_HEADER + offsetInPage]);
	int			len = VCI_MAX_PAGE_SPACE - offsetInPage;
	int			reqLen = vci_VARHDSZ_ANY(curPtr);

	if (reqLen <= len)
	{
		memcpy(header, curPtr, reqLen);

		return offsetInPage + reqLen;
	}

	memcpy(header, curPtr, len);
	ReleaseBuffer(*buffer);
	++*currentBlockNumber;
	*buffer = vci_ReadBufferWithPageInit(rel, *currentBlockNumber);
	page = BufferGetPage(*buffer);
	memcpy(&(((char *) header)[len]),
		   &(page[VCI_MIN_PAGE_HEADER]),
		   reqLen - len);

	return reqLen - len;
}

void
vci_GetElementPosition(uint32 *offset,	/* not array */
					   BlockNumber *blockNumberBase,
					   uint32 *dataOffset,
					   vci_ColumnRelations *rel,
					   int32 extentId,
					   uint32 rowIdInExtent,
					   Form_pg_attribute attr)
{
	uint32		offset_[2];
	Size		totalSize;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);
	vci_GetChunkPositionAndSize(offset_,
								&totalSize,
								blockNumberBase,
								dataOffset,
								rel,
								extentId,
								rowIdInExtent,
								1,
								attr);

	*offset = offset_[0] + *dataOffset;

	{
		uint32		rowIdInChunk = rowIdInExtent % VCI_COMPACTION_UNIT_ROW;
		BlockNumber curBN = (*offset) / VCI_MAX_PAGE_SPACE;
		BlockNumber oldBN = InvalidBlockNumber;
		uint32		offsetInPage;
		Buffer		buffer = InvalidBuffer;

		offsetInPage = (*offset) - (curBN * VCI_MAX_PAGE_SPACE);
		curBN += *blockNumberBase;

		for (uint32 rowId = 0; rowId < rowIdInChunk; ++rowId)
		{
			Datum		datum;

			if (oldBN != curBN)
			{
				if (BufferIsValid(buffer))
					ReleaseBuffer(buffer);
				buffer = vci_ReadBufferWithPageInit(rel->data, curBN);
				oldBN = curBN;
			}

			GetVarlenAHeader(&datum,
							 &buffer,
							 &curBN,
							 offsetInPage,
							 rel->data);

			{
				uint32		size = VARSIZE_ANY(&datum);

				(*offset) += size;
				offsetInPage += size;
				if (VCI_MAX_PAGE_SPACE <= offsetInPage)
				{
					offsetInPage -= VCI_MAX_PAGE_SPACE;
					if (oldBN == curBN)
						++curBN;
					else
						oldBN = curBN;
					Assert(offsetInPage < VCI_MAX_PAGE_SPACE);
				}
			}
		}
		if (BufferIsValid(buffer))
			ReleaseBuffer(buffer);
	}
	*offset -= *dataOffset;
	Assert((*offset) < offset_[1]);
}

void
vci_GetChunkPositionAndSize(uint32 *offset,
							Size *totalSize,
							BlockNumber *blockNumberBase,
							uint32 *dataOffset,
							vci_ColumnRelations *rel,
							int32 extentId,
							uint32 rowIdInExtent,
							int32 numUnit,
							Form_pg_attribute attr)
{
	uint32		offsetUnit;

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);
	{
		Buffer		buffer;
		Buffer		bufData;
		BlockNumber blockNumber;
		Page		page;
		uint32		unitId = rowIdInExtent / VCI_COMPACTION_UNIT_ROW;
		vcis_c_extent_t *cExtent = vci_GetColumnExtent(&buffer,
													   &blockNumber,
													   rel->meta,
													   extentId);
		vcis_extent_t *extent;

		*blockNumberBase = cExtent->enabled ? cExtent->block_number : InvalidBlockNumber;
		bufData = vci_ReadBufferWithPageInit(rel->data, *blockNumberBase);
		page = BufferGetPage(bufData);
		extent = vci_GetExtentT(page);
		*dataOffset = extent->data_offset;
		offsetUnit = (sizeof(uint32) * unitId) + extent->offset_offset;
		ReleaseBuffer(bufData);
		ReleaseBuffer(buffer);
	}

	{
		BlockNumber blockNumber;
		uint32		offsetPtr;
		Buffer		buffer;
		Page		page;

		vci_GetBlockNumberAndOffsetInPage(&blockNumber,
										  &offsetPtr,
										  offsetUnit);
		blockNumber += *blockNumberBase;
		buffer = vci_ReadBufferWithPageInit(rel->data, blockNumber);
		page = BufferGetPage(buffer);
		for (int aId = 0; aId <= numUnit; ++aId)
		{
			offset[aId] = *(uint32 *) &(page[offsetPtr + VCI_MIN_PAGE_HEADER]);
			offsetPtr += sizeof(uint32);
			if (VCI_MAX_PAGE_SPACE <= offsetPtr)
			{
				ReleaseBuffer(buffer);
				++blockNumber;
				buffer = vci_ReadBufferWithPageInit(rel->data, blockNumber);
				page = BufferGetPage(buffer);
				offsetPtr = 0;
			}
		}
		*totalSize = offset[numUnit] - offset[0];	/* pgr0063 */
		ReleaseBuffer(buffer);
	}
}

/**
 * @brief Get byte size of an entry in a column with fixed field length.
 *
 * @param[in] info pointer to the target vci_MainRelHeaderInfo.
 * @param[in] columnId column ID in the VCI index.
 * @return byte size of an entry in the column.
 */
uint16
vci_GetFixedColumnSize(vci_MainRelHeaderInfo *info, int16 columnId)
{
	switch (columnId)
	{
		case VCI_COLUMN_ID_TID:

			return sizeof(ItemPointerData);
		case VCI_COLUMN_ID_NULL:

			return vci_GetMainRelVar(info, vcimrv_null_width_in_byte, 0);
		case VCI_COLUMN_ID_DELETE:

			return 1;
		default:;
	}

	{
		vcis_m_column_t *colInfo;

		Assert(VCI_FIRST_NORMALCOLUMN_ID <= columnId);
		colInfo = vci_GetMColumn(info, columnId);

		return colInfo->max_columns_size;
	}
}

/**
 * @brief Get the position of the target entry in the relation of the column
 * with fixed field.
 *
 * @param[out] blockNumber block number of the target entry.
 * @param[out] offset offset in the block where the target is written.
 * @param[in] info pointer to the target vci_MainRelHeaderInfo.
 * @param[in] columnId column ID in the VCI index.
 * @param[in] extentId extent ID of the target entry
 * @param[in] rowIdInExtent entry ID in the extent.
 */
void
vci_GetPositionForFixedColumn(BlockNumber *blockNumber,
							  uint32 *offset,
							  vci_MainRelHeaderInfo *info,
							  int16 columnId,
							  int32 extentId,
							  uint32 rowIdInExtent,
							  bool atEnd)
{
	uint32		columnSize = vci_GetFixedColumnSize(info, columnId);
	Size		dataSize = (Size) columnSize * VCI_NUM_ROWS_IN_EXTENT;
	int32		extentHeaderSize = vci_GetExtentFixedLengthRawDataHeaderSize(
																			 VCI_NUM_ROWS_IN_EXTENT);
	uint32		numExtentPages = vci_GetNumBlocks(dataSize + extentHeaderSize);

	/*
	 * The start block number of extents can be directly calculated in the
	 * case of Fixed field length.
	 */
	uint32		startBlockNumber = extentId * numExtentPages;
	uint32		extraOffset = extentHeaderSize + (rowIdInExtent * columnSize);

	Assert(VCI_FIRST_NORMAL_EXTENT_ID <= extentId);

	if (atEnd)
		extraOffset += columnSize - 1;
	vci_GetBlockNumberAndOffsetInPage(blockNumber, offset, extraOffset);
	*blockNumber += startBlockNumber;
}

static void
InitColumnMetaRelation(vci_ColumnRelations *relPair,
					   Form_pg_attribute attr,
					   vcis_compression_type_t compType,
					   TupleDesc heapTupleDesc)
{
	vcis_column_meta_t *columnMeta;
	BlockNumber firstBlockNumber = VCI_COLUMN_DATA_FIRST_PAGE_ID;

	vci_FormatPageWithOneItem(relPair->meta, VCI_COLUMN_META_HEADER_PAGE_ID);

	columnMeta = vci_GetColumnMeta(&relPair->bufMeta, relPair->meta);
	LockBuffer(relPair->bufMeta, BUFFER_LOCK_EXCLUSIVE);

	if (attr)
	{							/* normal columns */
		columnMeta->pgsql_atttypid = attr->atttypid;
		columnMeta->pgsql_attnum = vci_GetAttNum(heapTupleDesc, NameStr(attr->attname));
		columnMeta->pgsql_attlen = attr->attlen;
		columnMeta->pgsql_atttypmod = attr->atttypmod;

		if (InvalidAttrNumber == columnMeta->pgsql_attnum)
			ereport(ERROR, (errmsg("column missed in VCI index creation"),
							errhint("This must never happen.  "
									"Give up to use VCI index.")));
	}
	else
	{							/* delete, null, or tid */
		columnMeta->pgsql_atttypid = InvalidOid;
		columnMeta->pgsql_attlen = 0;
		columnMeta->pgsql_atttypmod = 0;
	}

	columnMeta->num_extents = 0;
	columnMeta->num_extents_old = 0;
	columnMeta->free_page_begin_id = firstBlockNumber;
	columnMeta->free_page_end_id = firstBlockNumber;
	columnMeta->free_page_prev_id = InvalidBlockNumber;
	columnMeta->free_page_next_id = InvalidBlockNumber;
	columnMeta->num_free_pages = 1;
	columnMeta->num_free_pages_old = 1;
	columnMeta->num_free_page_blocks = 1;
	columnMeta->num_free_page_blocks_old = 1;
	columnMeta->min_max_field_size = 0;
	columnMeta->min_max_content_size = 0;
	columnMeta->latest_common_dict_id = VCI_INVALID_DICTIONARY_ID;

	columnMeta->num_common_dicts = 0;
	columnMeta->common_dict_info_offset = 0;
	columnMeta->block_number_extent_offset = offsetof(vcis_column_meta_t,
													  common_dict_info);

	vci_WriteColumnMetaDataHeader(relPair->meta, relPair->bufMeta);
	UnlockReleaseBuffer(relPair->bufMeta);
}

static void
InitDeleteVectorRelation(vci_ColumnRelations *relPair)
{
	vci_FormatPageWithItems(relPair->data,
							VCI_COLUMN_DATA_FIRST_PAGE_ID,
							VCI_ITEMS_IN_PAGE_FOR_DELETE);
	relPair->bufData = ReadBuffer(relPair->data, VCI_COLUMN_DATA_FIRST_PAGE_ID);
	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);

	for (OffsetNumber oNum = FirstOffsetNumber;
		 oNum <= VCI_ITEMS_IN_PAGE_FOR_DELETE;
		 ++oNum)
		vci_WriteItem(relPair->data, relPair->bufData, oNum);

	UnlockReleaseBuffer(relPair->bufData);
}

static void
InitColumnDataRelation(vci_ColumnRelations *relPair)
{
	vcis_free_space_t *freeSpace;

	vci_FormatPageWithOneItem(relPair->data, VCI_COLUMN_DATA_FIRST_PAGE_ID);

	freeSpace = vci_GetFreeSpace((vci_RelationPair *) relPair, VCI_COLUMN_DATA_FIRST_PAGE_ID);
	freeSpace->size = MaxBlockNumber;
	freeSpace->type = vcis_free_space;
	freeSpace->prev_pos = InvalidBlockNumber;
	freeSpace->next_pos = InvalidBlockNumber;

	LockBuffer(relPair->bufData, BUFFER_LOCK_EXCLUSIVE);
	vci_WriteOneItemPage(relPair->data, relPair->bufData);
	UnlockReleaseBuffer(relPair->bufData);
}

void
vci_InitializeColumnRelations(vci_MainRelHeaderInfo *info,
							  TupleDesc tupdesc,
							  Relation heapRel)
{
	const LOCKMODE lockmode = ShareLock;
	TupleDesc	heapTupleDesc = RelationGetDescr(heapRel);

	Assert((INT64CONST(0xFFFFFFFFFFFF0000) & tupdesc->natts) == 0);

	for (int16 colId = VCI_COLUMN_ID_DELETE; colId < (int16) tupdesc->natts; ++colId)
	{
		vci_ColumnRelations relPairData;
		vci_ColumnRelations *relPair = &relPairData;

		Form_pg_attribute attr;
		vcis_compression_type_t compType;

		if (colId >= VCI_FIRST_NORMALCOLUMN_ID)
		{
			attr = TupleDescAttr(tupdesc, colId);
			compType = vci_GetMColumn(info, colId)->comp_type;
		}
		else
		{
			attr = NULL;
			compType = vcis_compression_type_fixed_raw;
		}

		vci_OpenColumnRelations(relPair, info, colId, lockmode);
		InitColumnMetaRelation(relPair, attr, compType, heapTupleDesc);

		if (colId == VCI_COLUMN_ID_DELETE)
		{
			InitDeleteVectorRelation(relPair);
		}
		else
		{
			InitColumnDataRelation(relPair);
		}
		vci_CloseColumnRelations(relPair, lockmode);
	}
}
