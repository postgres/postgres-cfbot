/*-------------------------------------------------------------------------
 *
 * vci_freelist.h
 *	   Definitions and declarations of Free space link list
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_freelist.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_FREELIST_H
#define VCI_FREELIST_H

#include "postgres.h"

#include "vci.h"
#include "vci_columns.h"
#include "vci_ros.h"

#define VCI_FREESPACE_ITEM_ID  FirstOffsetNumber

typedef struct vcis_free_space
{
	uint32		size;

	vcis_extent_type_t type;

	BlockNumber prev_pos;

	BlockNumber next_pos;
} vcis_free_space_t;

#define vci_hasFreeLinkNode(freespace) \
	(vcis_free_space == (freespace)->type) \
	||  (vcis_tidcrid_type_pagetag == (freespace)->type)

extern PGDLLEXPORT vcis_free_space_t *vci_GetFreeSpace(vci_RelationPair *relPair, BlockNumber blk);

extern int32 vci_MakeFreeSpace(vci_RelationPair *relPair,
							   BlockNumber startBlockNumber,
							   BlockNumber *newFSBlockNumber,
							   vcis_free_space_t *newFS,
							   bool coalesce);

extern void vci_AppendFreeSpaceToLinkList(vci_RelationPair *relPair,
										  BlockNumber startBlockNumber,
										  BlockNumber prevFreeBlockNumber,
										  BlockNumber nextFreeBlockNumber,
										  BlockNumber size);

extern BlockNumber vci_FindFreeSpaceForExtent(vci_RelationPair *relPair,
											  BlockNumber requiredSize);

extern void vci_RemoveFreeSpaceFromLinkList(vci_RelationPair *relPair,
											BlockNumber startBlockNumber,
											BlockNumber numExtentPages);

/* *************** */
/* Recovery        */
/* *************** */

extern void vci_InitRecoveryRecordForFreeSpace(vci_MainRelHeaderInfo *info);

extern void vci_WriteRecoveryRecordForFreeSpace(vci_RelationPair *relPair,
												int16 colId,
												int16 dictId,
												BlockNumber StartBlockNumber,
												vcis_free_space_t *FS);

extern void vci_RecoveryFreeSpace(vci_MainRelHeaderInfo *info, vci_ros_command_t command);

#endif							/* VCI_FREELIST_H */
