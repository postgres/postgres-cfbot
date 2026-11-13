/*-------------------------------------------------------------------------
 *
 * vci_columns.h
 *	  Definitions and declarations of VCI column store and extents
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_columns.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_COLUMNS_H
#define VCI_COLUMNS_H

#include "postgres.h"

#include "vci.h"
#include "vci_chunk.h"
#include "vci_ros.h"
#include "vci_tidcrid.h"

/** header page ID of column meta data */
#define VCI_COLUMN_META_HEADER_PAGE_ID		 (0)

/** First page of Column data relations */
#define VCI_COLUMN_DATA_FIRST_PAGE_ID		 (0)

/** Column number of Column meta header page */
#define VCI_NUM_COLUMN_META_HEADER_PAGE		 (1)

/** Column ID of first Normal Column */
#define VCI_FIRST_NORMALCOLUMN_ID			 (0)

/** Column ID of special column */
#define VCI_COLUMN_ID_TID					 (-1)
#define VCI_COLUMN_ID_NULL					 (-2)
#define VCI_COLUMN_ID_DELETE				 (-3)
#define VCI_COLUMN_ID_CRID					 (-4)	/** @todo what is this? */

/**  The data below are not column-stored data.
 * We prepare them for convenience.
 */
#define VCI_COLUMN_ID_TID_CRID				 (-5)
#define VCI_COLUMN_ID_TID_CRID_UPDATE		 (-6)
#define VCI_COLUMN_ID_TID_CRID_WRITE		 (-7)
#define VCI_COLUMN_ID_TID_CRID_CDR			 (-8)
#define VCI_COLUMN_ID_DATA_WOS				 (-9)
#define VCI_COLUMN_ID_WHITEOUT_WOS			 (-10)

#define VCI_INVALID_COLUMN_ID ((int16) -11)

/** Vector bit count in one item (tuple) for delete vector  */
#define VCI_NUM_ROWS_IN_ONE_ITEM_FOR_DELETE	 (1024)

/** Item number in page for delete vector */
#define VCI_ITEMS_IN_PAGE_FOR_DELETE		 (52)

/** Page number in extent for delete vector */
#define VCI_NUM_PAGES_IN_EXTENT_FOR_DELETE	 (5)

static inline BlockNumber
vci_CalcBlockNumberFromCrid64ForDelete(uint64 crid64)
{
	return (vci_CalcExtentIdFromCrid64(crid64) *
			VCI_NUM_PAGES_IN_EXTENT_FOR_DELETE) +
		(vci_CalcRowIdInExtentFromCrid64(crid64) /
		 (VCI_ITEMS_IN_PAGE_FOR_DELETE *
		  VCI_NUM_ROWS_IN_ONE_ITEM_FOR_DELETE));
}

static inline OffsetNumber
vci_CalcOffsetNumberFromCrid64ForDelete(uint64 crid64)
{
	return ((vci_CalcRowIdInExtentFromCrid64(crid64) /
			 VCI_NUM_ROWS_IN_ONE_ITEM_FOR_DELETE) %
			VCI_ITEMS_IN_PAGE_FOR_DELETE) + FirstOffsetNumber;
}

static inline uint32
vci_CalcByteFromCrid64ForDelete(uint64 crid64)
{
	return (crid64 % VCI_NUM_ROWS_IN_ONE_ITEM_FOR_DELETE) / BITS_PER_BYTE;
}

static inline uint32
vci_CalcBitFromCrid64ForDelete(uint64 crid64)
{
	return crid64 & (BITS_PER_BYTE - 1);
}

/**
 * Pointing extent position of each column in BlockNumber.
 *
 * @description
 * This is used in vcis_column_meta_t.block_number_extent.
 * The field is not defined in the definition of the structure, because
 * we have the other variable length field "common_dict_info".
 * This block_number_extent follows the field.
 *
 * @note
 * unused entries have InvalidBlockNumber in block_number and
 * zero in num_blocks.
 */
typedef struct vcis_c_extent
{
	BlockNumber block_number;	/* the position in the column data relation */
	BlockNumber num_blocks;		/* the length in DB page unit */

	bool		enabled;		/* block_number is enabled if true */

	/* FIXME */	/* fill me */
	bool		valid_min_max;	/* size of min is
								 * vcis_column_meta_t.min_max_element_size */
	char		min[1];			/* max follows min. */
} vcis_c_extent_t;

/**
 * common dictionary info of each column
 *
 * @descriptions
 *This is used in vcis_column_meta_t.common_dict_info
 *
 * @note
 * unused entries have InvalidBlockNumber in block_number and
 * zero in num_blocks.
 */
typedef struct vcis_c_common_dict
{
	BlockNumber block_number;	/* the position in the column data relation */
	BlockNumber num_blocks;		/* the length in DB page unit */
} vcis_c_common_dict_t;

typedef struct vcis_column_meta
{
	vcis_attribute_type_t vcis_attr_type;	/* Attribute type */

	Oid			pgsql_atttypid; /* taken from FormData_pg_attribute.atttypid */
	int16		pgsql_attnum;	/* taken from FormData_pg_attribute.attnum */
	int16		pgsql_attlen;	/* taken from FormData_pg_attribute.attlen */
	int32		pgsql_atttypmod;	/* taken from
									 * FormData_pg_attribute.atttypmod */
	uint32		num_extents;	/* number of extents (for debug) */
	uint32		num_extents_old;	/* previous number of extents (for
									 * recovery) */

	BlockNumber free_page_begin_id; /* page ID of the first free area */

	BlockNumber free_page_end_id;	/* page ID of the last free area */

	/**
	 * The DB page ID of free area that located in front of the added or
	 * deleted extent by the ROS command.  (for recovery)
	 * This is used to recover free area list.
	 */
	BlockNumber free_page_prev_id;

	/**
	 * Same as free_page_prev_id, but just behind the added or deleted extent.
	 */
	BlockNumber free_page_next_id;

	/**
	 * The freespace size of added or deleted extent by the ROS command (for recovery)
	 */
	uint32		free_page_old_size;

	/**
	 * The freespace position of added or deleted extent in BlockNumber
	 * by the ROS command (for recovery)
	 */
	BlockNumber new_data_head;

	BlockNumber num_free_pages; /* number of free DB pages in the listed free
								 * area */
	BlockNumber num_free_pages_old; /* for recovery */
	BlockNumber num_free_page_blocks;	/* number of free areas, not number of
										 * free DB pages */
	BlockNumber num_free_page_blocks_old;	/* for recovery */

	/*--- Above must be same as vcis_tidcrid_meta_t ---*/

	uint32		common_flag_0;	/* vcis_column_meta_flag */

	uint32		min_max_field_size; /* size of min_max field size */
	uint32		min_max_content_size;	/* size of min_max content size */
	uint16		num_common_dicts;	/* Number of common dictionarys */
	int16		latest_common_dict_id;	/* Id of the latest common dictionary */
	uint32		common_dict_info_offset;	/* offset of common_dict_info[0] */
	uint32		block_number_extent_offset; /* offset of extent_pointer[0] */

	vcis_c_common_dict_t common_dict_info[1];	/* common dictionary
												 * informations */
	/* block_number_extent follows common_dict_info[num_common_dict - 1] */
} vcis_column_meta_t;

/**
 * @brief Get pointer to vcis_extent_t in the give DB page.
 */
#define vci_GetExtentT(page) \
	((vcis_extent_t *) &((page)[VCI_MIN_PAGE_HEADER]))

/*
 * Extend headers
 */
typedef struct vcis_extent
{
	uint32		size;			/* Size of extent */
	vcis_extent_type_t type;
	uint32		id;				/* Extend id */
	vcis_compression_type_t comp_type;	/* Compression method */
	uint32		offset_offset;	/* Offset to the offset */
	uint32		offset_size;	/* Size of the offset size */
	uint32		data_offset;	/* Offset to the data */
	uint32		data_size;		/* Data size */
	uint16		compressed;		/* 0 for not compressed, 1 for compressed */
	int16		dict_offset;	/* or common dictionary ID (>= -1) when
								 * dict_size == 0 */
	uint32		dict_size;		/* Size to the dictionary data */
	vcis_dict_type_t dict_type; /* The type of dictionary */
	char		dict_body[1];	/* the mainbody of the dictionary */
	/* offset_body and data_body follows dict_body */
} vcis_extent_t;

typedef vci_RelationPair vci_ColumnRelations;

extern PGDLLEXPORT vcis_column_meta_t *vci_GetColumnMeta(Buffer *buffer, Relation rel);
extern PGDLLEXPORT vcis_c_extent_t *vci_GetColumnExtent(Buffer *buffer,
														BlockNumber *blockNumber,
														Relation rel,
														int32 extentId);

extern PGDLLEXPORT void vci_OpenColumnRelations(vci_ColumnRelations *rel,
												vci_MainRelHeaderInfo *info,
												int16 columnId,
												LOCKMODE lockmode);

extern void vci_CloseColumnRelations(vci_ColumnRelations *rel,
									 LOCKMODE lockmode);

extern void vci_InitializeColumnRelations(vci_MainRelHeaderInfo *info,
										  TupleDesc tupdesc,
										  Relation heapRel);

extern void vci_WriteRawDataExtentInfo(Relation rel,
									   int32 extentId,
									   uint32 startPageID,
									   uint32 numBlocks,
									   char *minData,
									   char *maxData,
									   bool validMinMax,
									   bool checkOverwrite);

extern void vci_WriteOneExtent(vci_MainRelHeaderInfo *info,
							   RosChunkStorage *src,
							   int extentId,
							   TransactionId xgen,	/* xgen in extent info */
							   TransactionId xdel,	/* xdel in extent info */
							   TransactionId xid);	/* in tuple header */

/* columns to fetcher Interface */
extern void vci_GetElementPosition(uint32 *offset,
								   BlockNumber *blockNumberBase,
								   uint32 *dataOffset,
								   vci_ColumnRelations *rel,
								   int32 extentId,
								   uint32 rowIdInExtent,
								   Form_pg_attribute attr);

extern PGDLLEXPORT void vci_GetChunkPositionAndSize(uint32 *offset,
													Size *totalSize,
													BlockNumber *blockNumberBase,
													uint32 *dataOffset,
													vci_ColumnRelations *rel,
													int32 extentId,
													uint32 rowIdInExtent,
													int32 numUnit,
													Form_pg_attribute attr);

extern uint16
			vci_GetFixedColumnSize(vci_MainRelHeaderInfo *info, int16 columnId);
extern void
			vci_GetPositionForFixedColumn(BlockNumber *blockNumber,
										  uint32 *offset,
										  vci_MainRelHeaderInfo *info,
										  int16 columnId,
										  int32 extentId,
										  uint32 rowIdInExtent,
										  bool atEnd);

extern PGDLLEXPORT void
			vci_InitializeDictInfo(vci_DictInfo *dictInfo);

/* ***************************
 * Min-Max info
 * ***************************
 */

static inline void
vci_Initvci_ColumnRelations(vci_ColumnRelations *rel)
{
	rel->meta = NULL;
	rel->data = NULL;
}

/* function to write meta data header
 * argumtents
 * Relation relMeta
 * Buffer buffer
 */
static inline void
vci_WriteColumnMetaDataHeader(Relation relMeta,
							  Buffer buffer)
{
	vci_WriteOneItemPage(relMeta, buffer);
}

#endif							/* VCI_COLUMNS_H */
