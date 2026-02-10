/*-------------------------------------------------------------------------
 *
 * vci_chunk.c
 *	  Buffering mechanism used for WOS->ROS conversion
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_chunk.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "vci.h"
#include "vci_chunk.h"
#include "vci_columns.h"
#include "vci_columns_data.h"
#include "vci_ros.h"

static void
InitOneRosChunkBufferCore(RosChunkBuffer *rosChunkBuffer,
						  int numRowsAtOnce,
						  int16 *columnIdList,
						  int16 *columnSizeList,
						  bool useTid,
						  bool useDeleteVector,
						  vci_MainRelHeaderInfo *info)
{
	int16		nullBitId = 0;
	char	   *bufferIndex;
	char	   *bufferData;
	int			sizeIndexArray;
	Size		sizeTuple = 0;

	const int16 numColumns = rosChunkBuffer->numColumns;
	const int16 numNullableColumns = rosChunkBuffer->numNullableColumns;

	rosChunkBuffer->numColumnsWithIndex = 0;
	rosChunkBuffer->nullWidthInByte = (numNullableColumns + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
	rosChunkBuffer->numFilled = 0;
	rosChunkBuffer->compType = palloc_array(vcis_compression_type_t, numColumns);
	rosChunkBuffer->nullBitId = palloc_array(int16, numColumns);
	rosChunkBuffer->columnSizeList = palloc_array(int16, numColumns);
	rosChunkBuffer->data = palloc0_array(char *, numColumns * 2);
	rosChunkBuffer->dataOffset = (vci_offset_in_extent_t **)
		&(rosChunkBuffer->data[numColumns]);
	memcpy(rosChunkBuffer->columnSizeList,
		   columnSizeList,
		   sizeof(int16) * numColumns);
	rosChunkBuffer->nullData = palloc_array(char, rosChunkBuffer->nullWidthInByte *
									  numRowsAtOnce);
	rosChunkBuffer->tidData = useTid ? palloc(sizeof(ItemPointerData) * numRowsAtOnce)
		: NULL;
	rosChunkBuffer->deleteData = useDeleteVector
		? palloc(vci_RoundUpValue(numRowsAtOnce, 8))
		: NULL;

	for (int16 colId = VCI_FIRST_NORMALCOLUMN_ID; colId < numColumns; ++colId)
	{
		vcis_compression_type_t compType;

		compType = vci_GetMColumn(info, columnIdList ? columnIdList[colId] : colId)
			->comp_type;
		rosChunkBuffer->compType[colId] = compType;
		switch (compType)
		{
			case vcis_compression_type_fixed_raw:
				rosChunkBuffer->dataOffset[colId] = NULL;
				break;
			case vcis_compression_type_variable_raw:

				/*
				 * we put the value 1 in rosChunkBuffer->dataOffset[colId] as
				 * a mark that later in this function the memory area to keep
				 * offsets should be allocated.
				 */
				rosChunkBuffer->dataOffset[colId] = (vci_offset_in_extent_t *) 1;
				++(rosChunkBuffer->numColumnsWithIndex);
				break;
			default:
				elog(ERROR, "unsupported compression type");	/* FIXME */
		}
		sizeTuple += columnSizeList[colId];
		if (0 < numNullableColumns)
			rosChunkBuffer->nullBitId[colId] = nullBitId++;
	}
	Assert(nullBitId == numNullableColumns);

	sizeIndexArray = sizeof(vci_offset_in_extent_t) *
		rosChunkBuffer->numColumnsWithIndex;
	rosChunkBuffer->dataAllocPtr = palloc((sizeIndexArray *
										   (numRowsAtOnce + 1)) +
										  (sizeTuple * numRowsAtOnce) +
										  (VCI_DATA_ALIGNMENT_IN_STORAGE
										   * numColumns));
	bufferIndex = rosChunkBuffer->dataAllocPtr;
	bufferData = &(bufferIndex[sizeIndexArray * (numRowsAtOnce + 1)]);
	for (int16 colId = VCI_FIRST_NORMALCOLUMN_ID; colId < numColumns; ++colId)
	{
		int			colSize = vci_RoundUpValue(columnSizeList[colId] * numRowsAtOnce,
											   VCI_DATA_ALIGNMENT_IN_STORAGE);

		rosChunkBuffer->data[colId] = bufferData;
		bufferData += colSize;

		/*
		 * we put 1 in rosChunkBuffer->dataOffset[colId] for those columns to
		 * need offset data.
		 */
		if (rosChunkBuffer->dataOffset[colId])
		{
			rosChunkBuffer->dataOffset[colId] =
				(vci_offset_in_extent_t *) bufferIndex;
			bufferIndex += sizeof(vci_offset_in_extent_t) *
				(numRowsAtOnce + 1);
			rosChunkBuffer->dataOffset[colId][0] = 0;
		}
	}
}

/**
 * @brief Initialize a buffer to keep a chunk for ROS.
 *
 * The buffer initialized by this function must be destroyed by
 * vci_DestroyOneRosChunkBuffer().
 *
 * @param[out] rosChunkBuffer data in rosChunkBuffer is initialized.
 * @param[in] numRowsAtOnce number of rows to be stored in a chunk.
 * @param[in] columnSizeList worst-case column sizes.
 * @param[in] numColumns number of columns.
 * @param[in] info VCI main relation header information.
 */
void
vci_InitOneRosChunkBuffer(RosChunkBuffer *rosChunkBuffer,
						  int numRowsAtOnce,
						  int16 *columnSizeList,
						  int numColumns,
						  bool useDeleteVector,
						  vci_MainRelHeaderInfo *info)
{
	rosChunkBuffer->numColumns = numColumns;
	rosChunkBuffer->numNullableColumns = vci_GetNumberOfNullableColumn(
																	   vci_GetTupleDescr(info));
	InitOneRosChunkBufferCore(rosChunkBuffer,
							  numRowsAtOnce,
							  NULL,
							  columnSizeList,
							  true,
							  useDeleteVector,
							  info);
}

/**
 * @brief Destroy chunk buffer.
 *
 * @param[in] rosChunkBuffer target to destroy.
 */
void
vci_DestroyOneRosChunkBuffer(RosChunkBuffer *rosChunkBuffer)
{
	Assert(rosChunkBuffer);

	if (NULL == rosChunkBuffer->compType)
		return;

	vci_PfreeAndNull(&(rosChunkBuffer->compType));
	vci_PfreeAndNull(&(rosChunkBuffer->nullBitId));
	vci_PfreeAndNull(&(rosChunkBuffer->columnSizeList));
	vci_PfreeAndNull(&(rosChunkBuffer->data));
	vci_PfreeAndNull(&(rosChunkBuffer->nullData));
	vci_PfreeAndNull(&(rosChunkBuffer->tidData));
	vci_PfreeAndNull(&(rosChunkBuffer->deleteData));
	vci_PfreeAndNull(&(rosChunkBuffer->dataAllocPtr));
	rosChunkBuffer->numColumns = 0;
}

/**
 * @brief Initialize a RosChunkStorage, which holds multiple
 * RosChunkBuffer.
 *
 * @param[out] rosChunkStorage pointer to the target RosChunkStorage that
 * is initialized.
 * @param[in] numRowsAtOnce number of rows to be stored in a chunk
 * @param[in] forAppending false for normal ROS creation.
 * make true only for collect-deleted-rows with appending new data.
 *
 * @note The instance should be destroyed by vci_DestroyRosChunkStorage().
 */
void
vci_InitRosChunkStorage(RosChunkStorage *rosChunkStorage,
						int numRowsAtOnce,
						bool forAppending)
{
	Assert(rosChunkStorage);
	rosChunkStorage->numChunks = (VCI_NUM_ROWS_IN_EXTENT + numRowsAtOnce - 1) /
		numRowsAtOnce;

	if ((rosChunkStorage->forAppending = forAppending)) /* pgr0011 */
		rosChunkStorage->numChunks *= 2;

	rosChunkStorage->numFilled = 0;
	rosChunkStorage->numTotalRows = 0;
	rosChunkStorage->chunk = palloc0_array(RosChunkBuffer *,
									 rosChunkStorage->numChunks);
}

/**
 * @brief Reset RosChunkStorage to reuse it for new extent creation.
 *
 * The RosChunkBuffer's held by the storage are destroyed.
 *
 * @param[in] rosChunkStorage the target to be reset.
 */
void
vci_ResetRosChunkStorage(RosChunkStorage *rosChunkStorage)
{
	int			cId;

	for (cId = 0; cId < rosChunkStorage->numFilled; ++cId)
	{
		vci_DestroyOneRosChunkBuffer(rosChunkStorage->chunk[cId]);
		rosChunkStorage->chunk[cId] = NULL;
	}
	rosChunkStorage->numFilled = 0;
	rosChunkStorage->numTotalRows = 0;
}

/**
 * @brief Destroy RosChunkStorage.
 *
 * The RosChunkBuffer's held by the storage are also destroyed.
 *
 * @param[in] rosChunkStorage the target to be destroyed.
 */
void
vci_DestroyRosChunkStorage(RosChunkStorage *rosChunkStorage)
{
	int			cId;

	Assert(rosChunkStorage);
	if (NULL == rosChunkStorage->chunk)
		return;

	for (cId = 0; cId < rosChunkStorage->numFilled; ++cId)
		vci_DestroyOneRosChunkBuffer(rosChunkStorage->chunk[cId]);
	pfree(rosChunkStorage->chunk);
	rosChunkStorage->chunk = NULL;
	rosChunkStorage->numChunks = 0;
	rosChunkStorage->numFilled = 0;
}

/**
 * @brief Fill one tuple in a RosChunkBuffer.
 *
 * @param[in] rosChunkBuffer the buffer where the tuple is stored into.
 * @param[in] info VCI main relation header information.
 * @param[in] tid the tid to be stored.
 * @param[in] tuple the tuple to be stored.
 * @param[in] dstColumnIdList the target column IDs in the VCI.
 * @param[in] heapAttrNumList attribute numbers of the target columns
 * in the original heap tuple.
 * @param[in] tupleDesc the tuple descriptor of the original heap
 * relation.
 */
void
vci_FillOneRowInRosChunkBuffer(RosChunkBuffer *rosChunkBuffer,
							   vci_MainRelHeaderInfo *info,
							   ItemPointer tid,
							   HeapTuple tuple,
							   int16 *dstColumnIdList,
							   AttrNumber *heapAttrNumList,
							   TupleDesc tupleDesc)
{
	int16		colId;
	int			offset = (rosChunkBuffer->numFilled)++;
	int			nullWidthInByte = rosChunkBuffer->nullWidthInByte;
	char	   *nullData = (NULL == rosChunkBuffer->nullData) ? NULL :
		&(rosChunkBuffer->nullData[nullWidthInByte * offset]);

	if (nullData)
		MemSet(nullData, 0, nullWidthInByte);

	if (rosChunkBuffer->tidData)
		memcpy(&(rosChunkBuffer->tidData[sizeof(ItemPointerData) * offset]), tid, sizeof(ItemPointerData));

	for (colId = VCI_FIRST_NORMALCOLUMN_ID; colId < rosChunkBuffer->numColumns; ++colId)
	{
		bool		isnull;
		Datum		datum = heap_getattr(tuple,
										 heapAttrNumList[colId],
										 tupleDesc,
										 &isnull);

		if (isnull)
		{
			Assert((VCI_FIRST_NORMALCOLUMN_ID <= dstColumnIdList[colId]) &&
				   (dstColumnIdList[colId] <
					vci_GetMainRelVar(info, vcimrv_num_columns, 0)));
			if (nullData)
				vci_SetBit(nullData,
						   rosChunkBuffer->nullBitId[dstColumnIdList[colId]]);

			switch (rosChunkBuffer->compType[colId])
			{
				case vcis_compression_type_fixed_raw:
					{
						int			size = rosChunkBuffer->columnSizeList[colId];
						char	   *ptr;

						ptr = &(rosChunkBuffer->data[colId][offset * size]);
						if (0 == offset)
							MemSet(ptr, 0, size);
						else
							memcpy(ptr, &(ptr[-size]), size);
					}
					break;
				case vcis_compression_type_variable_raw:
					{
						static struct varlena datumNull;
						static vci_offset_in_extent_t size = 0;
						vci_offset_in_extent_t curOffset;

						if (size == 0)
						{
							/* One-time initialization */

							MemSet(&datumNull, 0, sizeof(datumNull));

							/*
							 * varlena for extenal is type of 1B_E and has the
							 * the length of zero.  We must give 1 or larger
							 * length to normal varlena data.
							 */
							SET_VARSIZE_SHORT(&datumNull, 1);
							size = 1;
						}
						curOffset = rosChunkBuffer->dataOffset[colId][offset];
						rosChunkBuffer->dataOffset[colId][offset + 1] =
							curOffset + size;
						memcpy(&(rosChunkBuffer->data[colId][curOffset]),
							   &datumNull,
							   size);
					}
					break;
				default:
					elog(ERROR, "unsupported compression type");	/* FIXME */

			}
		}
		else
		{
			switch (rosChunkBuffer->compType[colId])
			{
				case vcis_compression_type_fixed_raw:
					{
						int			size = rosChunkBuffer->columnSizeList[colId];
						char	   *ptr;

						ptr = &(rosChunkBuffer->data[colId][offset * size]);
						if (size <= sizeof(Datum))
						{
							switch (size)
							{
								case 1:
									*ptr = DatumGetUInt8(datum);
									break;
								case 2:
									{
										uint16		val = DatumGetUInt16(datum);

										memcpy(ptr, &val, sizeof(uint16));
									}
									break;
								case 4:
									{
										uint32		val = DatumGetUInt32(datum);

										memcpy(ptr, &val, sizeof(uint32));
									}
									break;
								case 8:
									{
										uint64		val = DatumGetInt64(datum);

										memcpy(ptr, &val, sizeof(uint64));
									}
									break;
								default:
									elog(ERROR, "unsupported fixed length");
							}
						}
						else
						{
							size = rosChunkBuffer->columnSizeList[colId];

							/* FIXME */

							/*
							 * sizeof(TimeTzADT) is 16, 4 bytes are padding,
							 * so we cannot use (sizeof(TimeTzADT) == size).
							 * Instead, (12U == size). Can we use better way?
							 */
							Assert((12U == size) ||
								   (sizeof(Interval) == size) ||
								   (UUID_LEN == size) ||
								   (NAMEDATALEN == size));
							memcpy(ptr, DatumGetPointer(datum), size);
						}
					}
					break;

					/* FIXME */

					/*
					 * We need to fill variable length data into fixed length
					 * area in order to reduce the space for the offsets and
					 * headers.
					 */
				case vcis_compression_type_variable_raw:
					{
						vci_offset_in_extent_t curOffset;
						vci_offset_in_extent_t size = VARSIZE_ANY(DatumGetPointer(datum));

						size = MAXALIGN(size);

						/* Check worst size. */
						Assert(size <= rosChunkBuffer->columnSizeList[colId]);

						curOffset = rosChunkBuffer->dataOffset[colId][offset];
						rosChunkBuffer->dataOffset[colId][offset + 1] =
							curOffset + size;
						memcpy(&(rosChunkBuffer->data[colId][curOffset]),
							   DatumGetPointer(datum),
							   size);
					}
					break;
				default:
					elog(ERROR, "unsupported compression type");	/* FIXME */
			}
		}
	}
}

/**
 * @brief Reset counter of a RosChunkBuffer.
 *
 * @param[in] buffer the target RosChunkBuffer.
 */
void
vci_ResetRosChunkBufferCounter(RosChunkBuffer *buffer)
{
	int			colId;

	for (colId = VCI_FIRST_NORMALCOLUMN_ID; colId < buffer->numColumns; ++colId)
	{
		if (NULL != buffer->dataOffset[colId])
			buffer->dataOffset[colId][0] = 0;
	}
	buffer->numFilled = 0;
}

/**
 * @brief Create a copy of a RosChunkBuffer.
 *
 * In creation, memory are allocated only by the necessary capacity.
 *
 * @param[in] src the original RosChunkBuffer.
 * @return pointer to the copy.
 *
 * @note  The created RosChunkBuffer should be destroyed by
 * vci_DestroyOneRosChunkBuffer().
 */
static RosChunkBuffer *
vci_CopyRosChunkBuffer(RosChunkBuffer *src)
{
	Size		totalSize;
	int16		colId;
	char	   *bufferIndex;
	char	   *bufferData;
	int			sizeIndexArray;
	int16		numColumns = src->numColumns;
	int			numFilled = src->numFilled;
	RosChunkBuffer *dst;

	CHECK_FOR_INTERRUPTS();

	dst = vci_AllocateAndCopy(src, sizeof(RosChunkBuffer));

	dst->compType = vci_AllocateAndCopy(src->compType,
										sizeof(vcis_compression_type_t) * numColumns);
	dst->nullBitId = vci_AllocateAndCopy(src->nullBitId,
										 sizeof(int16) * numColumns);
	dst->columnSizeList = vci_AllocateAndCopy(src->columnSizeList,
											  sizeof(int16) * numColumns);
	dst->nullData = vci_AllocateAndCopy(src->nullData,
										src->nullWidthInByte * numFilled);
	dst->tidData = vci_AllocateAndCopy(src->tidData,
									   sizeof(ItemPointerData) * numFilled);
	dst->deleteData = NULL;
	if (src->deleteData)
		dst->deleteData = vci_AllocateAndCopy(src->deleteData,
											  vci_RoundUpValue(numFilled, 8));

	CHECK_FOR_INTERRUPTS();

	dst->data = palloc_array(char *, numColumns * 2);
	dst->dataOffset = (vci_offset_in_extent_t **) &(dst->data[numColumns]);

	sizeIndexArray = sizeof(vci_offset_in_extent_t) * src->numColumnsWithIndex;
	totalSize = sizeIndexArray * (numFilled + 1);	/* pgr0062 */
	for (colId = VCI_FIRST_NORMALCOLUMN_ID; colId < numColumns; ++colId)
	{
		if (NULL == src->dataOffset[colId])
			totalSize += src->columnSizeList[colId] * numFilled;
		else
			totalSize += src->dataOffset[colId][numFilled] -
				src->dataOffset[colId][0];
	}
	dst->dataAllocPtr = palloc(totalSize);
	bufferIndex = dst->dataAllocPtr;
	bufferData = &(bufferIndex[sizeIndexArray * (numFilled + 1)]);
	for (colId = VCI_FIRST_NORMALCOLUMN_ID; colId < numColumns; ++colId)
	{
		if (0 == (colId & 3))
			CHECK_FOR_INTERRUPTS();

		if (NULL == src->dataOffset[colId])
		{
			Size		copySize = src->columnSizeList[colId] * numFilled;

			dst->data[colId] = bufferData;
			memcpy(bufferData, src->data[colId], copySize);
			bufferData += copySize;
			dst->dataOffset[colId] = NULL;
		}
		else
		{
			Size		copySize = src->dataOffset[colId][numFilled] -
				src->dataOffset[colId][0];

			dst->data[colId] = bufferData;
			memcpy(bufferData, src->data[colId], copySize);
			bufferData += copySize;
			copySize = sizeof(vci_offset_in_extent_t) * (numFilled + 1);
			dst->dataOffset[colId] = (vci_offset_in_extent_t *) bufferIndex;
			memcpy(bufferIndex, src->dataOffset[colId], copySize);
			bufferIndex += copySize;
		}
	}

	return dst;
}

/**
 * @brief Register a RosChunkBuffer to a RosChunkStorage.
 *
 * @param[in] rosChunkStorage the holder of RosChunkBuffer.
 * @param[in] src the RosChunkBuffer to be registered.
 */
void
vci_RegisterChunkBuffer(RosChunkStorage *rosChunkStorage, RosChunkBuffer *src)
{
	Assert(rosChunkStorage->numFilled < rosChunkStorage->numChunks);
	rosChunkStorage->chunk[rosChunkStorage->numFilled] =
		vci_CopyRosChunkBuffer(src);
	++(rosChunkStorage->numFilled);
	rosChunkStorage->numTotalRows += src->numFilled;
}

/**
 * @brief Calculate the data size of the specified column
 * in the RosChunkStorage.
 *
 * @param[in] src the target RosChunkStorage to be inspected.
 * @param[in] columnId the ID of the target column.
 * @param[in] asFixed true to treat a variable-field-length column as a
 * fixed-field-length column.
 */
Size
vci_GetDataSizeInChunkStorage(RosChunkStorage *src, int columnId, bool asFixed)
{
	Size		dataSize;
	int			chunkId;

	if (src->numFilled < 1)
		return 0;

	Assert((VCI_FIRST_NORMALCOLUMN_ID <= columnId) && (columnId < src->chunk[0]->numColumns));

	switch (src->chunk[0]->compType[columnId])
	{
		case vcis_compression_type_fixed_raw:
			return src->numTotalRows * src->chunk[0]->columnSizeList[columnId];

		default:
			;
	}

	dataSize = 0;
	for (chunkId = 0; chunkId < src->numFilled; ++chunkId)
	{
		RosChunkBuffer *chunk = src->chunk[chunkId];
		vci_offset_in_extent_t *dataOffset = chunk->dataOffset[columnId];

		dataSize += dataOffset[chunk->numFilled] - dataOffset[0];
	}

	return dataSize;
}
