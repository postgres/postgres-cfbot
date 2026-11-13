/*-------------------------------------------------------------------------
 *
 * vci_fetch.h
 *	  Definitions and declarations of Column store fetch
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_fetch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_FETCH_H
#define VCI_FETCH_H

#include "postgres.h"

#include "access/attnum.h"
#include "utils/guc.h"

#include "vci.h"
#include "vci_columns.h"

#include "vci_mem.h"
#include "vci_ros.h"

/* Get function of virtual tuples may used to get the storage area.
 * In that case, no rows may stored.
 * So, skipping the assertion check by default.
 * To use the assertion check, define CHECK_VTUPLE_GET_RANGE.
 */
#define CHECK_VTUPLE_GET_RANGE

/*
 * memory image of data loaded by vci_CSFetchVirtualTuples().
 * The area is allocated by vci_CSCreateVirtualTuples(), and the addresses
 * are fixed except for each data in "column N area"s.
 *
 * ADDRESS               CONTENT
 * allocated             (palloc-ed address)
 * (no padding)
 * flags (or skip)       uint8 of tuple[0]
 * (bit 0 is a copy of   uint8 of tuple[1]
 *  delete vector)       .
 *                       .
 *                       uint8 of tuple[num_rows_read_at_once-1]
 *                       uint8 of tuple[num_rows_read_at_once] (extra element)
 * (no padding)
 * isnull                bool[0]--bool[num_columns-1] of tuple[0]
 *                       bool[0]--bool[num_columns-1] of tuple[1]
 *                       .
 *                       .
 *                       bool[0]--bool[num_columns-1] of tuple[num_..._once-1]
 * (padding if necessary)
 * crid (aligned)        int64 of tuple[0]
 * (when need_crid is    int64 of tuple[1]
 *  true)                .
 *                       .
 *                       int64 of tuple[num_rows_read_at_once-1]
 * (no padding)
 * tid (aligned)         int64 of tuple[0]
 * (when need_tid is     int64 of tuple[1]
 *  true)                .
 *                       .
 *                       .
 *                       int64 of tuple[num_rows_read_at_once-1]
 * (no padding)
 * values (aligned)      Datum[0]--Datum[num_columns-1] of tuple[0]
 *                       Datum[0]--Datum[num_columns-1] of tuple[1]
 *                       .
 *                       .
 *                       Datum[0]--Datum[num_columns-1] of tuple[num_..._once-1]
 * (padding if necessary)
 * column 0 area         aligned data are stored when the element size is
 * (aligned)             larger than sizeof(Datum).  Each datum are pointed
 *                       by Datum[0] of tuples in the upper "data" area.
 *                       The size for the area is calculated using worst
 *                       case size.
 * (free space)
 * (padding if necessary)
 * column 1 area         aligned data are stored when the element size is
 * (aligned)             larger than sizeof(Datum).  Each datum are pointed
 *                       by Datum[1] of tuples in the upper "data" area.
 *                       The size for the area is calculated using worst
 *                       case size.
 *                       .
 *                       .
 * (free space)
 * (padding if necessary)
 * column (num_rows-1) area
 * (aligned)
 *
 *
 * usage:
 *
 * ---- in backend process ----
 * vci_CSQueryContext queryContext = vci_CSCreateQueryContext( mainRelationOid,
 *                                    numReadColumns, attrNum, sharedMemCtx);
 *
 * Size localRosSize = vci_CSEstimateLocalRosSize(queryContext);
 * if (limitLocalRos <= localRosSize)
 *       goto PostgreSQLQueryExecution;
 * vci_local_ros_t *localRos = vci_CSGenerateLocalRos(queryContext);
 *
 * vci_CSFetchContext fetchContext = vci_CSCreateFetchContext( queryContext,
 *                                    numRowsReadAtOnce,
 *                                    useColumnStore,
 *                                    numReadColumns, attrNum,
 *                                    returnTid, returnCrid);
 * Size fetchContextSize = vci_CSGetFetchContextSize(fetchContext);
 * if (limitFetchContext <= sumOfFetchContextSize)
 *       goto PostgreSQLQueryExecution;
 *
 * ---- in background worker ----
 * int lenVector = vci_CSGetActualNumRowsReadAtOnce(fetchContext);
 * vci_CSFetchContext localContext = vci_CSLocalizeFetchContext(fetchContext);
 * vci_virtual_tuples_t *vTuples = vci_CSCreateVirtualTuples(localContext);
 *
 * ** here you can make pointers to vTuples from PostgreSQL virtual tuples. **
 *
 * vci_extent_status_t *status =vci_CSCreateCheckExtent(fetchContext);
 *
 * for (extentID)
 * {
 *     vci_CSCheckExtent(status, localContext, extentId, readMinMax);
 *     if (status->existence && status->visible)
 *     {
 *         ** loop of vectors and rows **
 *         ** number of rows in the extent is in status->num_rows **
 *         for (vectorID)
 *         {
 *             int readableRows = vci_CSFetchVirtualTuples(vTuples,
 *                                                   vectroID * lenVector,
 *                                                   lenVector);
 *             for (idInVector = 0; idInVector < readableRows; ++ idInVector)
 *             {
 *
 *                 ** normal style from here **
 *                 int8 *flags = vci_CSGetSkipOfVirtualTuple(vTuples);
 *                 if ((* flags) & vcivtf_delete)
 *                     continue;
 *
 *                 ** Row wise **
 *                 Datum *values = vci_CSGetValuesOfVirtualTuple(vTuples,
 *                                                               idInVector);
 *                 bool *isnull = vci_CSGetIsNullOfVirtualTuple(vTuples,
 *                                                               idInVector);
 *
 *                 ** Column wise **
 *                 Datum *values = vci_CSGetValuesOfVirtualTupleColumnar(vTuples,
 *                                                               columnId);
 *                 bool *isnull = vci_CSGetIsNullOfVirtualTupleColumnar(vTuples,
 *                                                               columnId);
 *
 *                 int64 *crid = vci_CSGetCridOfVirtualTuple(vTuples,
 *                                                               idInVector);
 *                 int64 *tid = vci_CSGetTidOfVirtualTuple(vTuples,
 *                                                               idInVector);
 *                 UpdateVirtualTupleLinks();
 *                 EvaluateQualsEtc();
 *                 ** normal style to here **
 *
 *                 ** if you use fixed linked virtual tuples from here **
 *                 SelectPostgreSQLVirtualTuple();
 *                 EvaluateQualsEtc();
 *                 ** if you use fixed linked virtual tuples to here **
 *
 *             }
 *         }
 *     }
 * }
 *
 * vci_CSDestroyCheckExtent(status)
 * vci_CSDestroyVirtualTuples(vTuples);
 * vci_CSDestroyFetchContext(localContext);
 *
 * ---- in backend process ----
 * vci_CSDestroyFetchContext(fetchContext);
 * vci_CSDestroyLocalRos(localRos);
 * vci_CSDestroyQueryContext(queryContext);
 */

/**
 * @brief Information to fetch data from one relation used in a query.
 *
 * When multiple relations are used in one query,
 * multiple vci_CSQueryContextData should be created.
 */
typedef struct vci_CSQueryContextData
{
	/** Number of columns of the relation used in the query. */
	int			num_columns;

	/** Attribute number in original PostgreSQL relation. */
	AttrNumber *volatile attr_num;

	/** Column ID in VCI main relation. */
	int16	   *volatile column_id;

	/* Number of maximum WOS entries */
	int64		num_data_wos_entries;

	/* Number of maximum whiteout WOS entries */
	int64		num_whiteout_wos_entries;

	/**
	 * Number of entries in delete_list, just a copy of
	 * vci_local_ros_t.local_delete_list->num_entry.
	 */
	int			num_delete;

	/**
	 * Local delete list, containing whiteout WOS.
	 * CAUTION : THIS POINTER VALUE IS JUST A COPY OF
	 * vci_local_ros_t.local_delete_list->crid_list.
	 * NEVER pfree().
	 */
	uint64	   *delete_list;

	/**
	 * Number of extents of local ROS.
	 * To keep the extnets of local ROS at reasonable size,
	 * they may contain fewer rows than 262,144 rows.
	 */
	int			num_local_ros_extents;

	vci_local_ros_t *local_ros; /* pointer to the local ROS. */

	/** Number of extents in ROS. */
	int			num_ros_extents;

	/**
	 * Pointer to main relation information.
	 * The object is allocated in shared_memory_context,
	 * but info->rel cannot access from other process than that creates
	 * vci_CSFetchContext.
	 * In order to access main relation, open using main_relation_oid.
	 */
	vci_MainRelHeaderInfo *volatile info;

	/** Heap relation indexed by VCI to keep shared lock. */
	volatile Relation heap_rel;

	/** Oid of VCI main relation. */
	Oid			main_relation_oid;

	uint32		num_nullable_columns;	/* Number of nullable columns */
	uint32		null_width_in_byte; /* Size of null bit vector per row */

	/**
	 * ROS version taken from current ROS version or last ROS version.
	 */
	TransactionId ros_version;

	/**
	 * @see inclusiveXid of struct vci_RosCommandContext
	 */
	TransactionId inclusive_xid;

	/**
	 * @see exclusiveXid of struct vci_RosCommandContext
	 */
	TransactionId exclusive_xid;

	uint32		tid_crid_diff_sel;	/* Selection of TID CRID difference.  */

	/**
	 * Memory context where all the shared data are allocate,
	 * including the elements in this sturcture.
	 */
	MemoryContext shared_memory_context;

	/** lockmode of index relation (main relation) */
	LOCKMODE	lockmode;

} vci_CSQueryContextData;
typedef vci_CSQueryContextData *vci_CSQueryContext;

/**
 * @brief Buffer for decompression,
 *
 * and concatenate data separated into multiple pages.
 */
typedef struct vci_seq_scan_buffer
{
	int			num_buffers;
} vci_seq_scan_buffer_t;

/**
 * @brief Context to fetch vectors.
 *
 * Vector itself is in vci_virtual_tuples_t,
 * and the running parameters are kept in it.
 * A master instance of vci_CSFetchContextData is created by backend process,
 * then background workers copy to have locally.
 * Some member variables in local copy is over-written, marked as
 * \b LOCALIZED \b VARIABLE .
 */
typedef struct vci_CSFetchContextData
{
	uint32		size;			/* Size of this structure. */

	int32		extent_id;		/* The extent ID of stored virtual tuples. */
	uint16		num_rows;		/* Number of stored virtual tuples. */

	int16		num_columns;	/* Number of columns to fetch in this context. */

	/**
	 * Number of rows for the context to read at once.
	 * The fetcher read multiple lines at once and store them into the
	 * virtual tuple storage.
	 */
	uint32		num_rows_read_at_once;

	bool		use_column_store;	/* Store data in columnar style (true) or
									 * not. */

	bool		need_crid;		/* Fetch CRID or not. */
	bool		need_tid;		/* Fetch TID or not. */

	/** Used in decompression or data concatenation. */
	vci_seq_scan_buffer_t *buffer;

	/** \b LOCALIZED \b VARIABLE \n
	 * The ROS data fetched are stored in this context.
	 * virtual tuple storage is located here.
	 */
	MemoryContext local_memory_context;

	/** The size of virtual Tuple storage.
	 * This is sum of size_values, size_flags, and sizes of area pointed by
	 * vci_virtual_tuples_t->column_info[columnId].al_area.
	 */
	Size		size_vector_memory_context;

	/** area where Datum and pointers are stores */
	Size		size_values;

	/** The area where nulls, skip information, local skip information,
	 * TIDs, CRIDs, dictionaries, compression workarea and temporay
	 * area for wor-wise mode are placed.
	 * The amount of dictionary sizes is in size_dictionary_area.
	 * The workarea size for compression and decompression is in
	 * size_decompression_area.
	 */
	Size		size_flags;

	/** The memory size for dictionaries
	 * This is included in size_flags.
	 */
	Size		size_dictionary_area;

	/** Workarea size to decompress one VCI_COMPACTION_UNIT_ROW.
	 * The size is calculated as
	 * MAXALIGN(VCI_MAX_PAGE_SPACE * VCI_COMPACTION_UNIT_ROW)
	 * when size_dictionary_area != 0, or zero.
	 * This is included in size_flags.
	 */
	Size		size_decompression_area;

	/** The query context this fetch context belongs to. */
	vci_CSQueryContext query_context;

	/** \b LOCALIZED \b VARIABLE \n
	 * VCI main relation information used in localized fetch.
	 * Since the file discriptor or Relation structure must be obtained
	 * in each process, the main relation information also calculated in
	 * each process.
	 */
	vci_MainRelHeaderInfo *info;

	/** \b LOCALIZED \b VARIABLE \n
	 * Relations of the delete vector.
	 */
	vci_ColumnRelations rel_delete;

	/** \b LOCALIZED \b VARIABLE \n
	 * Relations of the null bit vector.
	 */
	vci_ColumnRelations rel_null;

	/** \b LOCALIZED \b VARIABLE \n
	 * Relations of the TID vector.
	 */
	vci_ColumnRelations rel_tid;

	/** \b LOCALIZED \b VARIABLE \n
	 * Pointer to the array of relations of normal columns.
	 */
	vci_ColumnRelations *rel_column;

	/**
	 * The column ID translation table.
	 * Since the column IDs in fetch vector are differ from those of
	 * VCI main relations,
	 * we have the translation table from the former to the latter here.
	 */
	int16		column_link[1]; /* VARIABLE LENGTH ARRAY */
} vci_CSFetchContextData;		/* VARIABLE LENGTH STRUCT */
typedef vci_CSFetchContextData *vci_CSFetchContext;

/**
 * @brief Structure to keep minimum and maximum value for a column.
 */
typedef struct vci_minmax
{
	bool		valid;			/* min and max are meaningful (true) or not
								 * (false). */
	char		min[VCI_MAX_MIN_MAX_SIZE];	/* Minimum value. */
	char		max[VCI_MAX_MIN_MAX_SIZE];	/* Maximum value. */
} vci_minmax_t;

/**
 * @brief The extent information which is obtained before fetching the
 * extent itself.
 *
 * It has information of existence, visibility of the extent,
 * number of rows in the extent,
 * and the minimum and maximum values of the extent.
 */
typedef struct vci_extent_status
{
	uint32		size;			/* Size of this structure. */
	uint32		num_rows;		/* Number of rows in the extent. */
	bool		existence;		/* Existence of the extent. */
	bool		visible;		/* Visibility of the extent. */

	/** The minimum and the maximum values of columns to be fetched. */
	vci_minmax_t minmax[1];		/* VARIABLE LENGTH ARRAY */
} vci_extent_status_t;			/* VARIABLE LENGTH STRUCT */

/**
 * @brief The status after reading vector.
 */
typedef enum vci_read_vector_status_t
{
	vcirvs_read_whole,			/* Whole the data, that are required, are
								 * read. */
	vcirvs_out_of_memory,		/* Partially read since out of memory. */
	vcirvs_end_of_extent,		/* Reaches the end of extent. */

	/** Failed to read since the parameter is out of range. */
	vcirvs_out_of_range,

	vcirvs_not_visible,			/* Failed to read since the extent is
								 * invisible. */
	vcirvs_not_exist,			/* The specified extent is not exists. */
} vci_read_vector_status_t;

/**
 * @brief Information of a fetched column in virtual tuple.
 */
typedef struct vci_virtual_tuples_column_info
{
	char	   *area;			/* Aligned pointer of al_area. NEVER pfree() */

	/** Allocated pointer, actual palloced() address is kept. */
	char	   *al_area;

	int32		null_bit_id;	/* Null bit ID in null bit vector. */
	uint32		max_column_size;	/* The maximum size of data in the column. */

	/** true when the value is passed by the pointer (datum by reference).
	 * false when the value itself is contained in Datum (datum by value).
	 */
	bool		strict_datum_type;

	vcis_compression_type_t comp_type;	/* Compression method used. */
	Oid			atttypid;		/* Type ID of attribute. */
	bool	   *isnull;			/* Pointer to the isnull flag area. */
	Datum	   *values;			/* Pointer to the Datum array area. */

	/** The information of the dictionary of LZVF compression.  */
	vci_DictInfo *dict_info;
} vci_virtual_tuples_column_info_t;

/**
 * @brief Information of virtual tuple, a set of fetched data.
 *
 * In the form, both colum-wise and row-wise are supported.
 */
typedef struct vci_virtual_tuples
{
	uint32		size;			/* Size of this instance. */
	uint16		num_columns;	/* Number of columns to store. */
	int32		extent_id;		/* The extent ID of stored data. */

	/** Physically recorded number of rows in the target extent. */
	uint32		num_rows_in_extent;

	/** The row ID in extent of the stored first datum. */
	uint32		row_id_in_extent;

	uint32		num_rows;		/* Number of stored rows in this structure. */

	uint32		buffer_capacity;	/* Capacity in unit of rows in this
									 * structure. */

	vci_read_vector_status_t status;	/* Read status. */

	/**
	 * This keeps the position of first tuple of vector,
	 * since the first virtual tuple of the vector is not always the first
	 * entry of stored data.
	 * At present, the upstream users requre that always the first data
	 * to be placed at the same address, this member variable is always
	 * set to zero.
	 */
	uint32		offset_of_first_tuple_of_vector;

	/**
	 * Number of rows for the context to read at once.
	 * The fetcher read multiple lines at once and store them into the
	 * virtual tuple storage.
	 */
	uint32		num_rows_read_at_once;

	/** The fetch context for this virtual tuple. */
	vci_CSFetchContext fetch_context;

	/** True for store in column-wise style.  False for row-wise. */
	bool		use_column_store;

	/**
	 * The size of virtual Tuple storage.
	 * This is sum of size_values, size_flags, and sizes of area pointed by
	 * vci_virtual_tuples_t->column_info[columnId].al_area.
	 */
	Size		size_vector_memory_context;

	/** The size of the area where Datum and pointers are stores. */
	Size		size_values;

	/**
	 * The size of the area where nulls, skip information,
	 * local skip information, TIDs, CRIDs, dictionaries,
	 * compression workarea and temporay area for wor-wise mode are placed.
	 * The amount of dictionary sizes is in size_dictionary_area.
	 * The workarea size for compression / decompression is in
	 * size_decompression_area.
	 */
	Size		size_flags;

	/**
	 * The memory size for dictionaries.
	 * This is included in size_flags.
	 */
	Size		size_dictionary_area;

	/**
	 * Workarea size to decompress one VCI_COMPACTION_UNIT_ROW.
	 * The size is calculated as
	 * MAXALIGN(VCI_MAX_PAGE_SPACE * VCI_COMPACTION_UNIT_ROW)
	 * when size_dictionary_area != 0, or zero.
	 * This is included in size_flags.
	 */
	Size		size_decompression_area;

	int64	   *crid;			/* Aligned pointer to CRID list in al_flags */

	/** Aligned pointer to TID list in al_flags.
	 * ItemPointerData are wrtten.
	 */
	int64	   *tid;

	/** Aligned pointer to skip list. */
	uint16	   *skip;

	/** Aligned pointer to skip list for local ROS. */
	uint16	   *local_skip;

	/** Aligned pointer to the area for isnull of all columns. */
	bool	   *isnull;

	/**
	 * In row-wise mode, the vector in local ROS is once built here.
	 * The area is allocated in local_memory_context.
	 * The size is
	 * num_rows_read_at_once * num_columns * (sizeof(Datum) + sizeof(bool))
	 */
	char	   *row_wise_local_ros;

	/**
	 * Workarea to decompress data.
	 * Dictionaries follow work_decompression
	 */
	char	   *work_decompression;

	/** Aligned pointer to the area for values of all columns in al_values. */
	Datum	   *values;

	/** Aligned pointer to the area for meta information like skip, TID,
	 * NULL, and so on.
	 */
	char	   *flags;

	char	   *al_values;		/* Allocated pointer for values. */
	char	   *al_flags;		/* Allocated pointer for flags. */

	/** Array of column informations. */
	vci_virtual_tuples_column_info_t column_info[1];	/* VARIABLE LENGTH ARRAY */
} vci_virtual_tuples_t;			/* VARIABLE LENGTH STRUCT */

extern PGDLLEXPORT vci_CSQueryContext vci_CSCreateQueryContextWLockMode(Oid mainRelationOid,
																		int numReadColumns,
 /* attribute number in original relation */
																		AttrNumber *attrNum,
																		MemoryContext sharedMemCtx,
																		LOCKMODE lockmode);

/**
 * @brief Create query context.
 *
 * @param[in] mainRelationOid Oid of VCI main relation.
 * @param[in] numReadColumns The number of read columns in the part of query.
 * @param[in] attrNum The attribute numbers in the original heap relation,
 * not those of the VCI main relation.
 * @param[in] sharedMemCtx The shared memory context to keep elements of
 * query context, fetch context, local ROS.
 * @param[in] recoveryInProgress true if recovery is still in progress.
 * @param[in] estimatingLocalROSSize true if creating a local ROS.
 * @return The pointer to the allocated vci_CSQueryContext.
 */
static inline vci_CSQueryContext
vci_CSCreateQueryContext(Oid mainRelationOid,
						 int numReadColumns,
						 AttrNumber *attrNum,
 /* attribute number in original relation */
						 MemoryContext sharedMemCtx,
						 bool recoveryInProgress,
						 bool estimatingLocalROSSize)
{
	/*
	 * ShareUpdateExclusiveLock is used for creating local ROS. But on the
	 * standby, AccessShareLock is used because queries on the standby can be
	 * used only RowExclusiveLock or weaker ones.
	 */
	LOCKMODE	lockmode = (recoveryInProgress || estimatingLocalROSSize) ? AccessShareLock : ShareUpdateExclusiveLock;

	return vci_CSCreateQueryContextWLockMode(mainRelationOid, numReadColumns,
											 attrNum, sharedMemCtx, lockmode);
}

extern PGDLLEXPORT void vci_CSDestroyQueryContext(vci_CSQueryContext queryContext);

/* obtain the worst size of local ROS to be estimated */
extern Size vci_CSEstimateLocalRosSize(vci_CSQueryContext queryContext);

extern PGDLLEXPORT vci_local_ros_t *vci_CSGenerateLocalRos(vci_CSQueryContextData *queryContext);

/**
 * @brief Entry point to destroy local ROS.
 *
 * @param[in] localRos Local ROS to be destroyed.
 */
static inline void
vci_CSDestroyLocalRos(vci_local_ros_t *localRos)
{
	vci_DestroyLocalRos(localRos);
}

extern PGDLLEXPORT vci_CSFetchContext vci_CSCreateFetchContextBase(
																   vci_CSQueryContext queryContext,
																   uint32 numRowsReadAtOnce,
																   int16 numReadColumns,
 /* attribute number in original relation */
																   AttrNumber *attrNum,
																   bool useColumnStore,
																   bool returnTid,
																   bool returnCrid,
																   bool useCompression);

#define VCI_MAX_NUM_ROW_TO_FETCH  (65536 - VCI_COMPACTION_UNIT_ROW)

/**
 * @brief The entry point to the function creating fetch context.
 *
 * The actual number of rows read at once is quantized
 * by VCI_COMPACTION_UNIT_ROW by the formula,
 * actualNumRowsReadAtOnce
 * = TYPEALIGN(VCI_COMPACTION_UNIT_ROW, numRowsReadAtOnce),
 * and numRowsReadAtOnce is unsigned 16 bit integer, it should be smaller than
 * or equal to VCI_MAX_NUM_ROW_TO_FETCH.  Otherwise, it returns NULL.
 *
 * @param[in] queryContext The query context.
 * @param[in] numRowsReadAtOnce The number of rows which read at once and
 * stored in the virtual tuples.
 * @param[in] numReadColumns The number of columns to be read.
 * @param[in] attrNum The pointer to the array which has the attribute numbers
 * of the original heap relation, not VCI main relation.
 * @param[in] useColumnStore True for column-wise store.  False for row-wise.
 * @param[in] returnTid True to get TID in virtual tuples.
 * @param[in] returnCrid True to get CRID in virtual tuples.
 * @return The pointer to the created fetch context.
 * NULL if some parameters are invald resulting no fetch context is created.
 */
static inline vci_CSFetchContext
vci_CSCreateFetchContext(vci_CSQueryContext queryContext,
						 uint16 numRowsReadAtOnce,
						 int16 numReadColumns,
 /* attribute number in original relation */
						 AttrNumber *attrNum,
						 bool useColumnStore,
						 bool returnTid,
						 bool returnCrid)
{
	return vci_CSCreateFetchContextBase(queryContext,
										numRowsReadAtOnce,
										numReadColumns,
										attrNum,
										useColumnStore,
										returnTid,
										returnCrid,
										false);
}

extern PGDLLEXPORT void vci_CSDestroyFetchContext(vci_CSFetchContext fetchContext);
extern PGDLLEXPORT vci_CSFetchContext vci_CSLocalizeFetchContext(
																 vci_CSFetchContext fetchContext,
																 MemoryContext memoryContext);
extern PGDLLEXPORT vci_extent_status_t *vci_CSCreateCheckExtent(
																vci_CSFetchContext localContext);
extern PGDLLEXPORT void vci_CSDestroyCheckExtent(vci_extent_status_t *status);
extern PGDLLEXPORT void vci_CSCheckExtent(vci_extent_status_t *status,
										  vci_CSFetchContext fetchContext,
										  int32 extentId,
										  bool readMinMax);

extern PGDLLEXPORT vci_virtual_tuples_t *vci_CSCreateVirtualTuplesWithNumRows(vci_CSFetchContext fetchContext, uint32 numRows);

/**
 * @brief Create virtual tuples according to the context.
 *
 * @param[in] localContext The localized fetch context.
 * @return The created virtual tuples.
 */
static inline vci_virtual_tuples_t *
vci_CSCreateVirtualTuples(vci_CSFetchContext localContext)
{
	return vci_CSCreateVirtualTuplesWithNumRows(localContext,
												localContext->num_rows_read_at_once);
}

extern PGDLLEXPORT void vci_CSDestroyVirtualTuples(vci_virtual_tuples_t *vTuples);

/**
 * @brief Get the address of the area where Datum of the specified column
 * is stored.
 *
 * At present, the upstream requester requires the start address fixed.
 * For better performance, it is better that the start address is modifiable,
 * to fetch many rows at once, or to use local ROS directly.
 *
 * @param[in] vTuples The virtual tuples.
 * @param[in] columnId Target column ID.
 * @return The pointer to the Datum array.
 */
static inline Datum *
vci_CSGetValueAddrFromVirtualTuplesColumnwise(vci_virtual_tuples_t *vTuples, uint16 columnId)
{
	return vTuples->column_info[columnId].values;
}

/**
 * @brief Get the address of the area where isnull of the specified column
 * is stored.
 *
 * At present, the upstream requester requires the start address fixed.
 * For better performance, it is better that the start address is modifiable,
 * to fetch many rows at once, or to use local ROS directly.
 *
 * @param[in] vTuples The virtual tuples.
 * @param[in] columnId Target column ID.
 * @return The pointer to the bool array.
 */
static inline bool *
vci_CSGetIsNullAddrFromVirtualTuplesColumnwise(vci_virtual_tuples_t *vTuples, uint16 columnId)
{
	return vTuples->column_info[columnId].isnull;
}

/**
 * @brief Get the address of the skip information of the specified column
 * is stored.
 *
 * @param[in] vTuples The virtual tuples.
 * @return The pointer to the skip information array.
 */
static inline uint16 *
vci_CSGetSkipAddrFromVirtualTuples(vci_virtual_tuples_t *vTuples)
{
	return vTuples->skip;
}

/**
 * @brief Get the vector of specified skip information.
 *
 * @param[in] vTuples The virtual tuples.
 * @return The pointer to the array of skip information.
 *
 * @note The instrtuction is the same as
 * vci_CSGetValuesOfVirtualTupleColumnar().
 */
static inline uint16 *
vci_CSGetSkipFromVirtualTuples(vci_virtual_tuples_t *vTuples)
{
#ifdef CHECK_VTUPLE_GET_RANGE
	Assert((0 <= vTuples->offset_of_first_tuple_of_vector) &&
		   (vTuples->offset_of_first_tuple_of_vector < vTuples->num_rows));
#endif							/* #ifdef CHECK_VTUPLE_GET_RANGE */

	return &(vTuples->skip[vTuples->offset_of_first_tuple_of_vector]);
}

/**
 * @brief Get the vector of TID.
 *
 * @param[in] vTuples The virtual tuples.
 * @return The pointer to the array of TID information in int64* form.
 *
 * @note This function is available when the fetch context is created
 * with the option returnTid is true.
 * This function can be available independent of useColumnStore option.
 */
/* Cast please */
static inline int64 *
vci_CSGetTidFromVirtualTuples(vci_virtual_tuples_t *vTuples)
{
#ifdef CHECK_VTUPLE_GET_RANGE
	Assert((0 <= vTuples->offset_of_first_tuple_of_vector) &&
		   (vTuples->offset_of_first_tuple_of_vector < vTuples->num_rows));
#endif							/* #ifdef CHECK_VTUPLE_GET_RANGE */

	return &(vTuples->tid[vTuples->offset_of_first_tuple_of_vector]);
}

/**
 * @brief Get the TID of specified tuple.
 *
 * @param[in] vTuples The virtual tuples.
 * @param[in] offsetInVector offset in the vector.
 * @return TID information.
 *
 * @note The instruction is the same as vci_GetTidFromVirtualTuples().
 */
#ifdef __s390x__
static inline ItemPointerData
vci_CSGetTidInItemPointerFromVirtualTuples(vci_virtual_tuples_t *vTuples,
										   int offsetInVector)
{
	ItemPointerData ipd;
	int64		result = (vci_CSGetTidFromVirtualTuples(vTuples))[offsetInVector];
#ifdef WORDS_BIGENDIAN
	result = result << 16;
#else
#endif
	ipd = *((ItemPointer) &result);
	return ipd;
}
#else
static inline ItemPointer
vci_CSGetTidInItemPointerFromVirtualTuples(vci_virtual_tuples_t *vTuples,
										   int offsetInVector)
{
	return (ItemPointer) &(vci_CSGetTidFromVirtualTuples(vTuples)
						   [offsetInVector]);
}
#endif

extern PGDLLEXPORT int vci_CSFetchVirtualTuples(vci_virtual_tuples_t *vTuples,
												int64 cridStart,
												uint32 numReadRows);

/**
 * @brief Get the tuple specified.
 *
 * @param[in] vTuples The virtual tuples.
 * @param[in] offsetInVector offset in the vector.
 * @return The pointer to the array of Datum.
 *
 * @note This function can be used when the fetch context is created in
 * row-wise mode, i.e. useColumnStore = false.
 * The column fetcher is read rows in unit of VCI_COMPACTION_UNIT_ROW.
 * Therefore, at the start address of the buffer does not always have
 * the specified data.
 * The specified data is pointed by the offset of
 * vTuples->offset_of_first_tuple_of_vector, actually.
 * To have the data at the start address, always read rows of multiples
 * of VCI_COMPACTION_UNIT_ROW at once.
 * For example, when VCI_COMPACTION_UNIT_ROW = 128, then
 * read 128 rows at once from the row ID in the extent, 0, 128, 256, 384, ....
 * Or, read 256 rows at once from the row ID in the extent, 0, 256, 512, ...
 */
static inline Datum *
vci_CSGetValuesOfVirtualTuple(vci_virtual_tuples_t *vTuples,
							  uint32 offsetInVector)
{
	offsetInVector += vTuples->offset_of_first_tuple_of_vector;

#ifdef CHECK_VTUPLE_GET_RANGE
	Assert(!vTuples->use_column_store);
	Assert((0 <= offsetInVector) && (offsetInVector < vTuples->num_rows));
#endif							/* #ifdef CHECK_VTUPLE_GET_RANGE */

	return &(vTuples->values[vTuples->num_columns * offsetInVector]);
}

/**
 * @brief Get the isnull of specified tuple.
 *
 * @param[in] vTuples The virtual tuples.
 * @param[in] offsetInVector offset in the vector.
 * @return The pointer to the array of bool.
 *
 * @note See instruction of vci_CSGetValuesOfVirtualTuple().
 */
static inline bool *
vci_CSGetIsNullOfVirtualTuple(vci_virtual_tuples_t *vTuples,
							  int32 offsetInVector)
{
	offsetInVector += vTuples->offset_of_first_tuple_of_vector;

#ifdef CHECK_VTUPLE_GET_RANGE
	Assert(!vTuples->use_column_store);
	Assert((0 <= offsetInVector) && ((uint32) offsetInVector < vTuples->num_rows));
#endif							/* #ifdef CHECK_VTUPLE_GET_RANGE */

	return &(vTuples->isnull[vTuples->num_columns * offsetInVector]);
}

/**
 * @brief Get the vector of specified column data.
 *
 * @param[in] vTuples The virtual tuples.
 * @param[in] columnId The column ID.
 * @return The pointer to the array of Datum.
 *
 * @note This function can be used when the fetch context is created in
 * column-wise mode, i.e. useColumnStore = true.
 * The other instruction is the same as vci_CSGetValuesOfVirtualTuple().
 */
static inline Datum *
vci_CSGetValuesOfVirtualTupleColumnar(vci_virtual_tuples_t *vTuples, uint16 columnId)
{
#ifdef CHECK_VTUPLE_GET_RANGE
	Assert(vTuples->use_column_store);
	Assert((VCI_FIRST_NORMALCOLUMN_ID <= columnId) && (columnId < vTuples->num_columns));
#endif							/* #ifdef CHECK_VTUPLE_GET_RANGE */

	return &(vTuples->column_info[columnId].values
			 [vTuples->offset_of_first_tuple_of_vector]);
}

/**
 * @brief Get the vector of specified isnull information.
 *
 * @param[in] vTuples The virtual tuples.
 * @param[in] columnId The column ID.
 * @return The pointer to the array of bool.
 *
 * @note The instrtuction is the same as
 * vci_CSGetValuesOfVirtualTupleColumnar().
 */
static inline bool *
vci_CSGetIsNullOfVirtualTupleColumnar(vci_virtual_tuples_t *vTuples, uint16 columnId)
{
#ifdef CHECK_VTUPLE_GET_RANGE
	Assert(vTuples->use_column_store);
	Assert((VCI_FIRST_NORMALCOLUMN_ID <= columnId) && (columnId < vTuples->num_columns));
#endif							/* #ifdef CHECK_VTUPLE_GET_RANGE */

	return &(vTuples->column_info[columnId].isnull[vTuples->offset_of_first_tuple_of_vector]);
}

/**
 * @brief Obtains the column ID in the VCI main relation from the serial number
 * in a set of read columns listed in vci_CSFetchContext.
 *
 * @param[in] fetchContext The fetch context.
 * @param[in] serialNumber The serial number in a set of read columns.
 * @return the columnID in the VCI main relation.
 */
static inline int16
vci_GetColumnIdFromFetchContext(vci_CSFetchContext fetchContext,
								int16 serialNumber)
{
	int			cId;

	Assert((0 <= serialNumber) && (serialNumber < fetchContext->num_columns));
	cId = fetchContext->column_link[serialNumber];
	Assert((0 <= cId) && (cId < fetchContext->query_context->num_columns));

	return fetchContext->query_context->column_id[cId];
}

extern void vci_FillCridInVirtualTuples(vci_virtual_tuples_t *vTuples);
extern void
			vci_FillFixedWidthColumnarFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
														  int16 columnId,
														  RosChunkStorage *rosChunkStorage);
extern void
			vci_FillVariableWidthColumnarFromRosChunkStorage(vci_virtual_tuples_t *vTuples,
															 int16 columnId,
															 RosChunkStorage *rosChunkStorage);
extern int16 *vci_GetNullableColumnIds(vci_virtual_tuples_t *vTuples);

#endif							/* VCI_FETCH_H */
