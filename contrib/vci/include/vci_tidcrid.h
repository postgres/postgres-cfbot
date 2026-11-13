/*-------------------------------------------------------------------------
 *
 * vci_tidcrid.h
 *	  Definitions and Declarations of TIDCRID update list and
 *	  TIDCRID Tree relation
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_tidcrid.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_TIDCRID_H
#define VCI_TIDCRID_H

#include "postgres.h"

#include "utils/tuplesort.h"

#include "vci.h"
#include "vci_ros.h"
#include "vci_chunk.h"

/** header page ID of TID->CRID update (differential) list */
#define VCI_TID_CRID_UPDATE_HEADER_PAGE_ID	 (0)

/** first body page ID of TID->CRID update (differential) list */
#define VCI_TID_CRID_UPDATE_BODY_PAGE_ID	 (1)

/** First page of tidcrid tree meta relation */
#define VCI_TID_CRID_META_FIRST_PAGE_ID		 (0)

/** First page of tidcrid tree data relation */
#define VCI_TID_CRID_DATA_FIRST_PAGE_ID		 (0)

/** Item number in page for tidcrid tree relation */
#define VCI_ITEMS_IN_PAGE_FOR_TID_CRID_TREE	 (18)

/** Offset number of page tag */
#define VCI_TID_CRID_PAGETAG_ITEM_ID		 (VCI_FREESPACE_ITEM_ID)

/** Capacity of tidcrid leaf node in bit*/
#define VCI_TID_CRID_LEAF_CAPACITY_BITS		 (6)

/** Capacity of tidcrid leaf node in bit*/
#define VCI_TID_CRID_LEAF_CAPACITY			 (1 << VCI_TID_CRID_LEAF_CAPACITY_BITS)

/** Capacity of tidcrid trunk node in bit*/
#define VCI_TID_CRID_TRUNK_CAPACITY_BITS	 (6)

/** Capacity of tidcrid trunk node in bit*/
#define VCI_TID_CRID_TRUNK_CAPACITY			 (1 << VCI_TID_CRID_TRUNK_CAPACITY_BITS)

/** Index of trunk node */
#define VCI_TID_CRID_TRUNKNODE				 (-1)

/** The number of items in DB page of TID-CRID Update List, normally 678 */
#define VCI_TID_CRID_UPDATE_PAGE_ITEMS       (VCI_MAX_PAGE_SPACE / sizeof(vcis_tidcrid_pair_item_t))

/** Available area in DB page of TID-CRID Update List, normally 8136 bytes */
#define VCI_TID_CRID_UPDATE_PAGE_SPACE       (VCI_TID_CRID_UPDATE_PAGE_ITEMS * sizeof(vcis_tidcrid_pair_item_t))

#define VCI_TID_CRID_UPDATE_CONTEXT_SAMPLES  (1353)

/*
 * On-disk data structure for CRID
 *
 * GetUin64tFromCrid() can be used to convert to uint64
 *
 * Sometimes v2 has special meanings, it represents special CRID.
 */
typedef struct vcis_Crid
{
	uint16		v0;
	uint16		v1;
	uint16		v2;
}
#ifdef __arm__
			__attribute__((packed))
#endif
vcis_Crid;

/*
 * Convert vcis_Crid to uint64, on-memory structure
 */
static inline uint64
vci_GetUint64FromCrid(vcis_Crid crid)
{
	/* Handle special values */
	if (crid.v2 == 0x8000)
		return VCI_INVALID_CRID;
	if (crid.v2 == 0xc000)
		return VCI_MOVED_CRID;

	return ((uint64) crid.v2 << 32) | ((uint64) crid.v1 << 16) | crid.v0;
}

/*
 * Convert uint64 to vcis_Crid, on-disk structure
 */
static inline vcis_Crid
vci_GetCridFromUint64(uint64 crid_uint64)
{
	vcis_Crid	crid;

	crid.v0 = crid_uint64 & ((uint64) 0xFFFF);
	crid.v1 = (crid_uint64 >> 16) & ((uint64) 0xFFFF);
	crid.v2 = (crid_uint64 >> 32) & ((uint64) 0xFFFF);

	return crid;
}

/*
 * TID-CRID tree relation
 *
 * The relation for the TID-CRID tree adds 18 tuples per page. In more detail,
 * each tuple can use only 424 bytes.
 *
 * Each node of the tree has 64 slots, and each slot has 6 bytes, so 384 bytes
 * are used to represent the tree. The remaining part is used for maintenance.
 * Also, the initial tuple of each page is used for maintaining the page.
 */

/*
 * Entries of flexible array in vcis_tidcrid_meta
 */
typedef struct vcis_tidcrid_meta_item
{
	BlockNumber block_number;	/* block number in TID-CRID tree relation */
	BlockNumber block_number_old;	/* previous block_number, used for
									 * recovery purpose */
	int16		item_id;		/* item id on TID-CRID tree relation */
	int16		item_id_old;	/* previous item_id, used for recovery purpose */
} vcis_tidcrid_meta_item_t;

/*
 * Meta relation for TID-CRID tree
 *
 * XXX: Several arrtibutes are not used but retained, to be consistent with
 * Column Meta Relation.
 */
typedef struct vcis_tidcrid_meta
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
	BlockNumber free_page_begin_id_old; /* previous free_page_begin_id (for
										 * recovery) */

	BlockNumber free_page_end_id;	/* page ID of the last free area */
	BlockNumber free_page_end_id_old;	/* previous free_page_end_id (for
										 * recovery) */

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
	BlockNumber new_freespace_head; /* @todo unused field */

	BlockNumber num_free_pages; /* number of free DB pages in the listed free
								 * area */
	BlockNumber num_free_pages_old; /* for recovery */
	BlockNumber num_free_page_blocks;	/* number of free areas, not number of
										 * free DB pages */
	BlockNumber num_free_page_blocks_old;	/* for recovery */

	/*--- Above must be same as column Meta ---*/

	BlockNumber num;			/* number of Stored items */
	BlockNumber num_old;		/* previous num, used for recovery purpose */
	BlockNumber free_block_number;	/* number of free blocks */
	int32		offset;			/* Offset from the head */
	vcis_tidcrid_meta_item_t body[1];	/* Flexible array of
										 * vcis_tidcrid_meta_item_t */
} vcis_tidcrid_meta_t;

/*
 * Metadata at the initial tuple
 */
typedef struct vcis_tidcrid_pagetag
{
	uint32		size;
	vcis_extent_type_t type;
	BlockNumber prev_pos;
	BlockNumber next_pos;

	uint32		num;
	uint32		free_size;
	uint32		bitmap;
	char		rsv[4];
} vcis_tidcrid_pagetag_t;

/*
 * Leaf in the TID-CRID tree
 */
typedef struct vcis_tidcrid_leaf
{
	uint32		size;
	vcis_tidcrid_item_type_t type;

	uint64		bitmap;
	uint64		unused;

	/* Sum of above must be less than 40 bytes */

	vcis_Crid	crid[VCI_TID_CRID_LEAF_CAPACITY];	/* CRIDs related with TID */
} vcis_tidcrid_leaf_t;

/*
 * Intermediate (trunk) node in TID-CRID tree
 */
typedef struct vcis_tidcrid_trunk
{
	uint32		size;
	vcis_tidcrid_item_type_t type;

	uint64		bitmap;
	uint64		unused;

	/* Sum of above must be less than 40 bytes */

	ItemPointerData leaf_item[VCI_TID_CRID_TRUNK_CAPACITY]; /* Pointer to the leaf */
} vcis_tidcrid_trunk_t;

/*
 * TID-CRID pair used for TIDCRID update list
 */
typedef struct vcis_tidcrid_pair_item
{
	ItemPointerData page_item_id;	/* TID on the original relation */
	vcis_Crid	crid;			/* CRID */
} vcis_tidcrid_pair_item_t;

/*
 * TID-CRID Update List
 */
typedef struct vcis_tidcrid_pair_list
{
	uint64		num;			/* Number of items in the list */

	uint16		blocks_per_samp;	/* Number of blocks each entries in
									 * samples_tids[] handles */
	uint16		num_samples;	/* Number of entries in samples_tids[] */

	/*
	 * TID samples from update list. Sampling condition:
	 *
	 * 1. Initial entries in each blocks_per_samp blocks 2. Final entry
	 */
	ItemPointerData sample_tids[VCI_TID_CRID_UPDATE_CONTEXT_SAMPLES + 1];

	vcis_tidcrid_pair_item_t body[1];	/* Flexible array of
										 * vcis_tidcrid_pair_item_t */
} vcis_tidcrid_pair_list_t;

typedef struct vci_TidCridUpdateListContext
{
	vci_MainRelHeaderInfo *info;	/* Parent VCI main relation */

	Relation	rel;

	/* Number of vcis_tidcrid_pair_item_t entries in the rel */
	uint64		count;

	/* Number of blocks of the rel */
	BlockNumber nblocks;

	/* Head pointer to the TID-CRID Update List */
	vcis_tidcrid_pair_list_t header;

} vci_TidCridUpdateListContext;

typedef vci_RelationPair vci_TidCridRelations;

/* initialize function */
extern void vci_InitializeTidCridUpdateLists(vci_MainRelHeaderInfo *info);
extern void vci_InitializeTidCridTree(vci_MainRelHeaderInfo *info);

/* TIDCRID Update List access functions */

extern PGDLLEXPORT vci_TidCridUpdateListContext *vci_OpenTidCridUpdateList(vci_MainRelHeaderInfo *info, int sel);
extern PGDLLEXPORT void vci_CloseTidCridUpdateList(vci_TidCridUpdateListContext *context);

extern PGDLLEXPORT void vci_ReadOneBlockFromTidCridUpdateList(vci_TidCridUpdateListContext *context, BlockNumber blkno, vcis_tidcrid_pair_item_t *array);

extern int32 vci_GetTidCridUpdateListLength(vci_MainRelHeaderInfo *info, int sel);
extern void vci_MergeAndWriteTidCridUpdateList(vci_MainRelHeaderInfo *info, int newSel, int oldSel, Tuplesortstate *newList, vcis_Crid crid);

/* TIDCRID Tree access functions */
extern void vci_OpenTidCridRelations(vci_TidCridRelations *rel,
									 vci_MainRelHeaderInfo *info,
									 LOCKMODE lockmode);
extern void vci_CloseTidCridRelations(vci_TidCridRelations *rel, LOCKMODE lockmode);

extern void vci_GetTidCridSubTree(vci_TidCridRelations *relPair, BlockNumber blkOrig,
								  ItemPointer retPtr);
extern void vci_CreateTidCridSubTree(vci_TidCridRelations *relPair, BlockNumber blkOrig,
									 ItemPointer retPtr);
extern void vci_UpdateTidCridSubTree(vci_TidCridRelations *relPair, ItemPointer trunkPtr,
									 vcis_tidcrid_pair_list_t *newItems);

/* TID->CRID Conversion */
extern PGDLLEXPORT uint64 vci_GetCridFromTid(vci_TidCridUpdateListContext *context, ItemPointer tId, bool *fromTree);

/* Recovery functions */

extern void vci_RecoveryFreeSpaceForTidCrid(vci_MainRelHeaderInfo *info);
extern void vci_RecoveryTidCrid(vci_MainRelHeaderInfo *info);
extern void vci_InitRecoveryRecordForTidCrid(vci_MainRelHeaderInfo *info);

extern void vci_AddTidCridUpdateList(vci_MainRelHeaderInfo *info,
									 RosChunkStorage *src,
									 int32 extentId);

#endif							/* VCI_TIDCRID_H */
