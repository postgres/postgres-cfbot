/*-------------------------------------------------------------------------
 *
 * vci_chunk.h
 *	  Definitions and Declarations of ROS chunk buffer strage.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_chunk.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_CHUNK_H
#define VCI_CHUNK_H

#include "postgres.h"

#include "miscadmin.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "vci.h"
#include "vci_ros.h"

/**
 * @brief RosChunkBuffer is a buffer to store one chunk.
 *
 * We use RosChunkBuffer in two purposes.  One is to store data obtaind
 * directly from PostgreSQL relation.  For this purpose, we prepare this
 * buffer to have enough space to store data even when all the attributes
 * have the size of worst case, that never happens.  Once the chunk is
 * stored in this buffer, we inspect the size of each column in the chunk.
 * Afterward, we copy all the chunk data into RosChunkStorage with removing
 * unused spaces.  Here, we use RosChunkBuffer for each chunk, but this
 * time we prepare the buffer with the size suitable for each chunk.  ROS
 * without compression is built from RosChunkStorage directly.
 */
typedef struct RosChunkBuffer
{
	int16		numColumns;		/* number of columns */
	int16		numNullableColumns; /* number of nullable columns */

	/** number of columns which need offset data for each entry because they
	 * have variable-length fields or fields longer than eight bytes, say,
	 * reference Datum.
	 */
	int16		numColumnsWithIndex;

	int			nullWidthInByte;	/* The byte width of null bit vector. */
	int			numRowsAtOnce;	/* the maximum number of rows in the chunk */
	int			numFilled;		/* the number of rows actually contained here */
	vcis_compression_type_t *compType;	/* Array of compression type for
										 * columns. */
	int16	   *nullBitId;		/* -1 for NOT NULLABLE */
	int16	   *columnSizeList; /* the sizes of columns in the worst case */
	void	   *dataAllocPtr;	/* pointer keeping allocated area */
	char	  **data;			/* buffer for each column */
	vci_offset_in_extent_t **dataOffset;	/* offset to each datum */
	char	   *nullData;		/* pointer to array of null bit vector. */
	char	   *tidData;		/* pointer to array of TID. */
	char	   *deleteData;		/* pointer to array of delete information */
} RosChunkBuffer;

/**
 * @brief Structure to keep buffers that keeps column-wise data built from WOS.
 */
typedef struct RosChunkStorage
{
	int			numChunks;		/* The length of allocated chunk. */
	int			numFilled;		/* The number of chunk actually used. */
	int			numTotalRows;	/* The sum of rows in registered chunks. */
	bool		forAppending;	/* True to append data to the shrunken extent. */

	/** Array of pointers to RosChunkBuffer, which is copied in a compact
	 * manner to reduce the memory.
	 */
	RosChunkBuffer **chunk;
} RosChunkStorage;

extern void
			vci_InitOneRosChunkBuffer(RosChunkBuffer *rosChunkBuffer,
									  int numRowsAtOnce,
									  int16 *columnSizeList,
									  int numColumns,
									  bool useDeleteVector,
									  vci_MainRelHeaderInfo *info);
extern void
			vci_InitRosChunkStorage(RosChunkStorage *rosChunkStorage,
									int numRowsAtOnce,
									bool forAppending);
extern void
			vci_DestroyOneRosChunkBuffer(RosChunkBuffer *rosChunkBuffer);
extern void
			vci_DestroyRosChunkStorage(RosChunkStorage *rosChunkStorage);
extern PGDLLEXPORT void
			vci_ResetRosChunkStorage(RosChunkStorage *rosChunkStorage);
extern void
			vci_FillOneRowInRosChunkBuffer(RosChunkBuffer *rosChunkBuffer,
										   vci_MainRelHeaderInfo *info,
										   ItemPointer tid,
										   HeapTuple tuple,
										   int16 *dstColumnIdList,
										   AttrNumber *heapAttrNumList,
										   TupleDesc tupleDesc);
extern void
			vci_ResetRosChunkBufferCounter(RosChunkBuffer *buffer);
extern void
			vci_RegisterChunkBuffer(RosChunkStorage *rosChunkStorage, RosChunkBuffer *src);
extern Size
			vci_GetDataSizeInChunkStorage(RosChunkStorage *src, int columnId, bool asFixed);

#endif							/* #ifndef VCI_CHUNK_H */
