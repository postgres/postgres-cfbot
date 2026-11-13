/*-------------------------------------------------------------------------
 *
 * vci_ros_command.h
 *	  Definitions and declarations of ROS control commands
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_ros_command.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_ROS_COMMAND_H
#define VCI_ROS_COMMAND_H

#include "postgres.h"
#include "c.h"
#include "utils/tuplesort.h"
#include "access/genam.h"

#include "vci_ros.h"
#include "vci_chunk.h"

typedef struct
{
	ItemPointerData *orig_tids;

	ItemPointerData *wos_tids;

	int			max;

	int			num;

	int			offset;
} vci_tid_array_t;

typedef struct
{
	BlockNumber *orig_blknos;

	int			max;

	int			num;

} vci_blk_array_t;

/**
 * @brief Context for ROS commands, containing TID list read from data WOS or
 * whiteout WOS, data read from the PostgreSQL heap relation or from ROS,
 * related attribute numbers, OIDs, number of rows, and so on.
 */
typedef struct vci_RosCommandContext
{
	vci_ros_command_t command;	/* command using this context */

	RosChunkBuffer buffer;		/* data are stored primary here */
	RosChunkStorage storage;	/* data are compacted and copied here */
	vci_MainRelHeaderInfo info; /* VCI main relation header */

	/** numRowsToConvert is something tricky.
	 * set VCI_NUM_ROWS_IN_EXTENT in index building phase.
	 * set number of rows (up to VCI_NUM_ROWS_IN_EXTENT) to convert after
	 * building.
	 */
	int			numRowsToConvert;

	int			numRowsAtOnce;	/* maximum number of rows in a chunk */
	Relation	heapRel;		/* the original relation indexed by VCI */
	Oid			heapOid;		/* the original relation indexed by VCI */
	Oid			indexOid;		/* the VCI indexed relation */

	int			numColumns;		/* number of columns in VCI index */

	/** the processing extent ID.  negative IDs for local ROSes */
	int32		extentId;
	int32		extentIdSrc;	/* source extentId in copy operation (wos2ros,
								 * cdr) */

	struct vci_local_ros *local_ros;	/* local ROS */

	/** list of worst case column size */
	int16	   *columnSizeList;

	/** attribute number (1-origin) in the original relation */
	AttrNumber *heapAttrNumList;

	/** index ID (0-origin) in the VCI relation */
	int16	   *indxColumnIdList;

	/** transaction ID using this context */
	TransactionId xid;

	TransactionId oldestXmin;

	TransactionId wos2rosXid;

	TransactionId inclusiveXid;

	TransactionId exclusiveXid;

	vci_tid_array_t wos2ros_array;

	vci_tid_array_t delvec_array;

	vci_blk_array_t utility_array;

	/**
	 * TID on "WOS Relation" list to convert in Item Pointer format
	 */

	bool		done;			/* true if all records are read */

	/**
	 * Number of rows in the relation estimated by analyze or vacuum command.
	 * This is used to build ROS in CREATE INDEX command.
	 */
	double		estimatedNumRows;

	/**
	 * Number of converted rows.
	 * This is used to build ROS in CREATE INDEX command.
	 */
	uint64		numConvertedRows;

	/**
	 * The name of index relation built.
	 * This is used to build ROS in CREATE INDEX command.
	 */
	char		relName[NAMEDATALEN];

	/**
	 * scan context.
	 * This is used only in initial building to scan the original relation
	 * sequentially.
	 */
	HeapScanDesc scan;

	TupleDesc	tid_tid_tupdesc;

	TupleTableSlot *tid_tid_slot;

	/**
	 * a sorted TID list to be converted into ROS extents
	 */
	Tuplesortstate *wos2ros_tid_list;
	int64		num_wos2ros_tids;

	/**
	 * a sorted TID list to be converted into a delete vector
	 */
	Tuplesortstate *delvec_tid_list;
	int64		num_delvec_tids;

	Tuplesortstate *data_wos_del_list;

	Tuplesortstate *whiteout_wos_del_list;

} vci_RosCommandContext;

typedef struct
{
	int32		num_fit_extents;
	int32		best_extent_id;
} vci_target_extent_info_t;

/*
 * *********************************************************
 * Conversion Context operation
 * *********************************************************
 */
extern void vci_InitRosCommandContext0(vci_RosCommandContext *context,
									   Relation rel, vci_ros_command_t command);
extern void vci_InitRosCommandContext1(vci_RosCommandContext *comContext,
									   Size workareaSize,
									   int numInsertRows,
									   int numDeleteRows,
									   bool readOriginalData);
extern void vci_InitRosCommandContext2(vci_RosCommandContext *comContext, Size workareaSize);

extern void vci_InitRosChunkStroageAndBuffer(vci_RosCommandContext *comContext, bool forAppending);

extern void vci_CleanRosCommandContext(vci_RosCommandContext *comContext, bool neverWrite);
extern void vci_FinRosCommandContext(vci_RosCommandContext *comContext, bool neverWrite);

extern void vci_ReleaseMainRelInCommandContext(vci_RosCommandContext *comContext);
extern void vci_CloseHeapRelInCommandContext(vci_RosCommandContext *comContext);

/*
 * *********************************************************
 * Functions for ROS command
 * *********************************************************
 */
extern PGDLLEXPORT int vci_ConvertWos2Ros(Relation mainRel, Size workareaSize, int numRows);
extern double vci_ConvertWos2RosForBuild(Relation mainRel, Size workarea, IndexInfo *indexInfo);
extern PGDLLEXPORT int vci_UpdateDelVec(Relation mainRel, Size workareaSize, int numRows);
extern PGDLLEXPORT int vci_CollectDeletedRows(Relation mainRel, Size workareaSize, int32 extentId);
extern PGDLLEXPORT int vci_UpdateTidCrid(Relation mainRel, Size workareaSize, int numPages);
extern PGDLLEXPORT int vci_CollectUnusedExtent(Relation mainRel, Size workareaSize);

extern void vci_VacuumRos(Relation mainRel, IndexVacuumInfo *vacuumInfo);

/*
 * *********************************************************
 * Probing functions to decided whether to execute the command
 * *********************************************************
 */
extern PGDLLEXPORT uint32 vci_CountFreezedInDataWos(Relation mainRel, Size workarea);
extern PGDLLEXPORT uint32 vci_CountFreezedInWhiteoutWos(Relation mainRel, Size workarea);
extern PGDLLEXPORT vci_target_extent_info_t vci_CountDeletedRowsInROS(Relation mainRel, uint32 threshold);
extern vci_target_extent_info_t vci_CountUnusedExtents(Relation mainRel);
extern int32 vci_CountTidCridUpdateListLength(Relation mainRel, Size workarea);

#endif							/* #ifndef VCI_ROS_COMMAND_H */
