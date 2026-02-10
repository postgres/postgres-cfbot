/*-------------------------------------------------------------------------
 *
 * vci_ros.h
 *	  Definitions and declarations of VCI main relation
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_ros.h
 *
 *-------------------------------------------------------------------------
 */

/****************************************************************************
 * ** CAUTION:  THE STRUCTURES DEFINED IN THIS HEADER FILE WITH THE PREFIX **
 * ** OF "vcis_" AND vci_MainRelVar, vcis_Crid DEFINE THE FORMAT OF THE ROS **
 * ** DATA.  ANY MODIFICATION ON THEM MAY CAUSE FORMAT INCOMPATIBILITY.    **
 * ** PLEASE BE SURE TO CHANGE THE VALUE OF EITHER MACRO                   **
 * ** VCI_ROS_VERSION_MAJOR OR VCI_ROS_VERSION_MINOR, TO DETECT FORMAT     **
 * ** INCOMPATIBILITY.                                                     **
 * **************************************************************************
 */

#ifndef VCI_ROS_H
#define VCI_ROS_H

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "c.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "nodes/execnodes.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "storage/lock.h"
#include "storage/off.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

#include "vci.h"

#include "vci_utils.h"

#if (!defined(WIN32))
#define UINT uint
#endif

#define VCI_ROS_VERSION_MAJOR  ((uint32) 0x00000000)
#define VCI_ROS_VERSION_MINOR  ((uint32) 0x0000000D)

/**
 * @brief IDs of ROS commands.
 */
typedef enum vci_ros_command
{
	vci_rc_invalid = -11,		/* Invalid case. */

	/** For vacuum with vci_mrlm_read_write_exclusive. */
	vci_rc_vacuum = -10,

	/** For normal query with vci_mrlm_read_share. */
	vci_rc_query = -9,

	/** For DROP command with vci_mrlm_read_write_exclusive. */
	vci_rc_drop_index = -8,

	/** For DELETE or UPDATE commands with vci_mrlm_read_share. */
	vci_rc_wos_delete = -7,

	/** For INSERT or UPDATE commands with vci_mrlm_read_share. */
	vci_rc_wos_insert = -6,

	/** For recovering ROS with vci_mrlm_read_share, assumed that this command
	 * is used in vci_mrlm_write_exclusive lock of ROS commands. */
	vci_rc_recovery = -5,

	/** For collecting VCI information with vci_mrlm_read_share.
	 * This is also used by vci_KeepMainRelHeader() and
	 * vci_KeepMainRelHeaderWOVersionCheck() automatically.
	 * */
	vci_rc_probe = -4,

	/** For building ROS in initial index building with
	 * vci_mrlm_read_write_exclusive. */
	vci_rc_wos_ros_conv_build = -3,

	/** For building local ROS with vci_mrlm_read_write_exclusive, to serialize
	 * ROS commands.
	 */
	vci_rc_generate_local_ros = -2,

	/** For COPY command with vci_mrlm_write_share. */
	vci_rc_copy_command = -1,

	/** For WOS -> ROS conversion with vci_mrlm_write_exclusive */
	vci_rc_wos_ros_conv = 0,

	/** For updating delete vector with vci_mrlm_write_exclusive */
	vci_rc_update_del_vec,

	/** For collecting deleted rows with vci_mrlm_write_exclusive */
	vci_rc_collect_deleted,

	/** For collecting deleted extents, unable to access anymore,
	 * with vci_mrlm_write_exclusive
	 */
	vci_rc_collect_extent,

	/** For updating TID -> CRID relations with vci_mrlm_write_exclusive */
	vci_rc_update_tid_crid,

	/** For compaction with vci_mrlm_write_exclusive */
	/* vci_rc_compaction, */

	num_vci_rc,					/* anchor */
} vci_ros_command_t;

/**
 * @brief function to obtain the size of the varlena headers.
 *
 * @param[in] ptr Pointer to the varlena.
 * @return Header size of given varlena.
 */
static inline int32
vci_VARHDSZ_ANY(void *ptr)
{
	return VARATT_IS_1B_E(ptr) ? VARHDRSZ_EXTERNAL
		: ((VARATT_IS_1B(ptr) ? VARHDRSZ_SHORT : VARHDRSZ));
}

typedef uint32 vci_offset_in_extent_t;	/* offset to data */

/** bit width of maximum number of row ID in an extent */
#define VCI_CRID_ROW_ID_BIT_WIDTH	  (18)

/** Calculate CRID in int64 format from extentID and rowID in extent */
static inline int64
vci_CalcCrid64(int32 extentId, uint32 rowIdInExtent)
{
	return ((int64) extentId << VCI_CRID_ROW_ID_BIT_WIDTH) |
		(rowIdInExtent & ((UINT64CONST(1) << VCI_CRID_ROW_ID_BIT_WIDTH) - 1));
}

/** Calculate extentID from CRID in int64 format */
static inline int32
vci_CalcExtentIdFromCrid64(int64 crid64)
{
	return (int32) (crid64 >> VCI_CRID_ROW_ID_BIT_WIDTH);
}

/** Calculate rowID in extent from CRID in int64 format */
static inline uint32
vci_CalcRowIdInExtentFromCrid64(int64 crid64)
{
	return (uint32) (crid64 & ((UINT64CONST(1) << VCI_CRID_ROW_ID_BIT_WIDTH) - 1));
}

/** Maximum number of rows in an extent.  (256 * 1024) for 18 bits */
#define VCI_NUM_ROWS_IN_EXTENT			(1 << VCI_CRID_ROW_ID_BIT_WIDTH)

#define VCI_MAX_NUMBER_UNCONVERTED_ROS	(128)

#define VCI_INVALID_CRID_IN_48_BIT		(UINT64CONST(0xFFFF800000000000))
#define VCI_INVALID_CRID				VCI_INVALID_CRID_IN_48_BIT

#define VCI_MOVED_CRID_IN_48_BIT		(UINT64CONST(0xFFFFC00000000000))
#define VCI_MOVED_CRID					VCI_MOVED_CRID_IN_48_BIT

/** Value indicating invalid extent.  The value is 0xE0000000 */
#define VCI_INVALID_EXTENT_ID \
	((int32) (VCI_INVALID_CRID_IN_48_BIT >> VCI_CRID_ROW_ID_BIT_WIDTH))

/** ID of the first extent stored in the storage. */
#define VCI_FIRST_NORMAL_EXTENT_ID (0)

/** Value indicating invalid dictionary.  The value is -1 */
#define VCI_INVALID_DICTIONARY_ID  (-1)

/** The number of rows converted at once by WOS->ROS converter.
 * Offset is assigned every VCI_COMPACTION_UNIT_ROW rows.
 */
#define VCI_COMPACTION_UNIT_ROW			 (128)

/** The ratio to keep usage of work area in safe level */
#define VCI_WOS_ROS_WORKAREA_SAFE_RATIO	 (0.5)

/** Base alignment in storage.
 * In the storage, normally VCI uses four-byte integers.
 * Thus, we align the data in the storage by four bytes.
 */
#define VCI_DATA_ALIGNMENT_IN_STORAGE  (4)

/** Aligned values, rounded up */
#define vci_RoundUpValue(value, unit) \
	((((value) + (unit) - 1) / (unit)) * (unit))
/** Aligned values, rounded down */
#define vci_RoundDownValue(value, unit) \
	(((value) / (unit)) * (unit))

/** Get byte size of data in an item when a page contains multiple items.
 * @param[in] numItem Number of items in a page.
 * @return The size of data in an item in byte.
 */
#define VCI_ITEM_SPACE(numItem) \
	((((BLCKSZ - offsetof(PageHeaderData, pd_linp) \
		- (numItem * (sizeof(HeapTupleHeaderData) + sizeof(ItemIdData)))) \
	   / numItem) / VCI_DATA_ALIGNMENT_IN_STORAGE) \
	 * VCI_DATA_ALIGNMENT_IN_STORAGE)

/** Get byte size of an item include item header,
 * when a page contains multiple items.
 * @param[in] numItem Number of items in a page.
 * @return The size of an item in byte.
 */
#define VCI_ITEM_SIZE(numItem) \
	(VCI_ITEM_SPACE(numItem) + sizeof(HeapTupleHeaderData))

/**
 * Minimum header space in DB page with one item, normally 52 bytes.
 *
 * Note: VCI sometimes writes structured data to the page immediately after
 * the header part, so we need MAXALIGN to prevent those structures causing
 * run-time alignment errors.
 */
#define VCI_MIN_PAGE_HEADER  \
	MAXALIGN(SizeOfPageHeaderData + sizeof(HeapTupleHeaderData) \
	 + sizeof(ItemIdData))

/** Available area in DB page with one item, normally 8140 bytes */
#define VCI_MAX_PAGE_SPACE  (BLCKSZ - VCI_MIN_PAGE_HEADER)

/**
 * @brief Return ID of the target page and offset in the target page
 * calculated from the position.
 *
 * The position and offsetInPage is measured in data area in DB pages.  We do
 * not care the header of DB page in this macro.
 *
 * @param[out] blockNumber Block number for the given position.
 * @param[out] offsetInPage Offset in page in byte, ignoring page header,
 * for the given position.
 * @param[in] position Byte offset in area formed by multiple DB pages.
 */
static inline void
vci_GetBlockNumberAndOffsetInPage(BlockNumber *blockNumber,
								  uint32 *offsetInPage,
								  uint32 position)
{
	*blockNumber = position / VCI_MAX_PAGE_SPACE;
	*offsetInPage = position - (*blockNumber * VCI_MAX_PAGE_SPACE);
}

/**
 * @brief Get number of pages to write given data size.
 *
 * @param[in] size The data size.
 * @return Number of pages to write.
 */
static inline uint32
vci_GetNumBlocks(Size size)
{
	if (size == MaxBlockNumber)
		return MaxBlockNumber;

	return (size + VCI_MAX_PAGE_SPACE - 1) / VCI_MAX_PAGE_SPACE;
}

/** Maximum data size of maximum and minimum values in extents.  */
#define VCI_MAX_MIN_MAX_SIZE				 (16)

/* Accessing VCI main relation header
 * Because the header of VCI main relation has three pages, we can not map
 * one structure of C on the header pages simply.
 * Instead, we use access functions.
 *
 * In order to, first prepare a variable to keep page info and call the
 * initialize function, with relation opend already.
 * vci_InitMainRelHeaderInfo(info, rel)
 *
 * use one of these two * functions.
 * vci_KeepReadingMainRelHeader()
 *     Read header pages for reading, pin and lock them.
 * vci_KeepWritingMainRelHeader()
 *     Read header pages for writing, pin and lock them.
 *
 * We have to repair all VCI relation, if some of them are broken.
 * Just call the next for the purpose.
 * vci_RecoverOneVCIIfNecessary()
 *
 * Then, use the following two functions,
 *
 * vci_SetMainRelVar()
 *     To set the value to the field.
 * vci_GetMainRelVar()
 *     To get the value of the field.
 *
 * Or, if you access column_info, use
 * vci_GetMColumn()
 * which gives the pointer to the vcis_m_column_t on the DB buffer directly.
 *
 * The field is defined in enum enum vci_MainRelVar.
 *
 *
 * To write the updated data, use the funcition
 * vci_WriteMainRelVar()
 *
 * After accessing the header, release the DB pages with the following
 * function.
 *
 * vci_ReleaseMainRelHeader()
 *     Release header pages, pins and locks.
 */

/**
 * @brief Field names and addresses of VCI main relation.
 *
 * These enum values has the page ID at upper 16 bits, and offset for the
 * field at lower 16 bits.
 * The offset is measured from the top of DB page, not after the page header.
 *
 * This is for struct vcis_main_t.
 * Because the header ov VCI main relation has three pages, we can not map
 * one structure of C on the header pages.
 *
 * Minimum header in DB page is 52 bytes (0x34)
 */
typedef enum vci_MainRelVar
{
	/* page 0 */
	vcimrv_data_wos_oid = 0x00000034,
	vcimrv_whiteout_wos_oid = 0x00000038,
	/* vcimrv_cdr_tid_crid_data_oid       = 0x0000003C, //reserved */
	vcimrv_tid_crid_meta_oid = 0x00000040,
	vcimrv_tid_crid_data_oid = 0x00000044,
	vcimrv_tid_crid_update_oid_0 = 0x00000048,
	vcimrv_tid_crid_update_oid_1 = 0x0000004C,
	/* vcimrv_tid_crid_write_oid          = 0x00000050, //reserved */
	vcimrv_delete_meta_oid = 0x00000054,
	vcimrv_delete_data_oid = 0x00000058,
	vcimrv_null_meta_oid = 0x0000005C,
	vcimrv_null_data_oid = 0x00000060,
	vcimrv_tid_meta_oid = 0x00000064,
	vcimrv_tid_data_oid = 0x00000068,
	vcimrv_ros_version_major = 0x0000006C,	/** MUST BE 0x0000006C */
	vcimrv_ros_version_minor = 0x00000070,	/** MUST BE 0x00000070 */
	vcimrv_num_nullable_columns = 0x00000074,
	vcimrv_null_width_in_byte = 0x00000078, /** byte size of null bit vector for one row. */
	vcimrv_column_info_offset = 0x0000007C,
	vcimrv_num_columns = 0x00000080,
	vcimrv_extent_info_offset = 0x00000084,
	/* page 0 to 2 */
	vcimrv_column_info = 0x00000088,
	/* page 3 */
	vcimrv_size_mr = 0x00030034,	/** @todo Maybe, dose not need */
	vcimrv_size_mr_old = 0x00030038,	/** @todo Maybe, dose not need */
	vcimrv_current_ros_version = 0x0003003C,
	vcimrv_last_ros_version = 0x00030040,
	vcimrv_tid_crid_diff_sel = 0x00030044,
	vcimrv_tid_crid_diff_sel_old = 0x00030048,
	vcimrv_xid_generation = 0x0003004C,
	vcimrv_xid_gen_update_xid = 0x00030050,
	/* vcimrv_xgen_tid_crid_write         = 0x00030054, //reserved */
	/* vcimrv_num_tid_crid_update_oid_0   = 0x00030058, //reserved */
	/* vcimrv_num_tid_crid_update_oid_1   = 0x0003005C, //reserved */
	vcimrv_ros_command = 0x00030060,
	/* vcimrv_ros_conv_extent_id          = 0x00030064, //reserved */
	/* vcimrv_ros_conv_common_dict_id     = 0x00030068, //reserved */
	vcimrv_old_extent_id = 0x0003006C,
	vcimrv_new_extent_id = 0x00030070,
	vcimrv_working_column_id = 0x00030074,
	vcimrv_working_dictionary_id = 0x00030078,
	vcimrv_tid_crid_operation = 0x0003007C,
	vcimrv_tid_crid_target_blocknumber = 0x00030080,
	vcimrv_tid_crid_target_info = 0x00030084,
	vcimrv_tid_crid_free_blocknumber = 0x00030088,
	/* vcimrv_compaction_colmn_id         = 0x0003007C, //reserved */
	/* vcimrv_compaction_extent_id        = 0x00030080, //reserved */
	/* vcimrv_compaction_old_block_number = 0x00030084, //reserved */
	/* vcimrv_compaction_new_block_number = 0x00030088, //reserved */
	vcimrv_num_unterminated_copy_cmd = 0x0003008C,
	vcimrv_tid_crid_tag_bitmap = 0x00030090,
	/* vcimrv_num_request_cdr             = 0x00030090, //reserved */
	/* vcimrv_num_appendable_extents      = 0x00030094, //reserved */
	/* vcimrv_num_compaction              = 0x00030098, //reserved */
	/* vcimrv_extent_id_to_write          = 0x0003009C, //reserved */
	vcimrv_num_extents = 0x000300A0,
	vcimrv_num_extents_old = 0x000300A4,
	vcimrv_extent_info = 0x000300A8,

	/* error code */
	vcimrv_invalid = 0xFFFFFFFF,
} vci_MainRelVar;

/** mask data to get offset for fileds in VCI main relation header in DB page */
#define VCI_MRV_MASK_OFFSET	 (0xFFFF)
/** bit to shift to get DB page ID for fileds in VCI main relation header */
#define VCI_MRV_PAGE_SHIFT	 (16)

/**
 * @brief Get block number for given field of main relation header.
 *
 * @param[in] value value defined in vci_MainRelVar.
 * @return Block number containing given field.
 */
#define vci_MRVGetBlockNumber(value)  ((value) >> VCI_MRV_PAGE_SHIFT)

/**
 * @brief Get offset in DB page for given field of main relation header.
 *
 * @param[in] value value defined in vci_MainRelVar.
 * @return Offset for containing given field from page top including header.
 */
#define vci_MRVGetOffset(value)		  ((value) & VCI_MRV_MASK_OFFSET)

/** Number of header pages of VCI main relation */
#define VCI_NUM_MAIN_REL_HEADER_PAGES  (4)

/** Struct to keep pointers to the header pages of VCI main relation */
typedef struct vci_MainRelHeaderInfo
{
	Relation	rel;			/* Relation of VCI main relation */

	/*
	 * VCI mainrelation header pages should be initialized with InvalidBuffer
	 */
	Buffer		buffer[VCI_NUM_MAIN_REL_HEADER_PAGES];	/* Buffers for the main
														 * relation header
														 * pages. */
	vci_ros_command_t command;	/* Command using this structure. */

	/** number of extents that have the area to store their vcis_m_extent_t
	 * in main relation.
	 * This field is used in query execution, otherwise it has "-1".
	 */
	int32		num_extents_allocated;
	/** To create VCI on more than 32 columns, creating TupleDesc by copying table's
	 * one is required. However, it is too heavy to repeat. So cache the created
	 * one to cached_tupledesc in initctx context.
	 */
	MemoryContext initctx;
	TupleDesc	cached_tupledesc;
} vci_MainRelHeaderInfo;

/** Minimum size of an extent
 * The extents of fixed field length columns has the size.
 * The extents of the other types have larger size.
 * Use vci_GetExtentFixedLengthRawDataHeaderSize() or something to obtain
 * the size actually.
 */
#define VCI_EXTENT_HEADER_SIZE  (offsetof(vcis_extent_t, dict_body))

/** This function returns the size of header of extent for fixed field length
 * data.  The size can be calculated from the format and the number of rows
 * in an extent.  Actually, it is independent of the number of rows, but that
 * of variable length depends.
 * @param[in] numRowsInExtent The number of rows in the extent.
 * @return The size of extent header.
 */
#define vci_GetExtentFixedLengthRawDataHeaderSize(numRowsInExtent) \
	VCI_EXTENT_HEADER_SIZE

/** Function to calculate necessary number of offset data to the chunks
 * of VCI_COMPACTION_UNIT_ROW in ROS.
 * @param[in] numRowsInExtent Number of rows in the extent.
 * @return Number of necessary offsets.
 */
#define vci_GetOffsetArrayLength(numRowsInExtent) \
	(1 + (((numRowsInExtent) + VCI_COMPACTION_UNIT_ROW - 1) \
		  / VCI_COMPACTION_UNIT_ROW))

/** Function to calculate data size of necessary offset data to the chunks
 * of VCI_COMPACTION_UNIT_ROW in ROS.
 * @param[in] numRowsInExtent Number of rows in the extent.
 * @return Necessary data size.
 */
#define vci_GetOffsetArraySize(numRowsInExtent) \
	vci_GetOffsetArrayLength(numRowsInExtent) \
	* sizeof(vci_offset_in_extent_t)

/** This function returns the size of header of extent for variable field
 * length data, and compressed data.
 * The size can be calculated from the format and the number of rows
 * in an extent.  Actually, it is independent of the number of rows, but that
 * of variable length depends.
 * @param[in] numRowsInExtent The number of rows in the extent.
 * @return The size of extent header.
 */
#define vci_GetExtentVariableLengthRawDataHeaderSize(numRowsInExtent) \
	(VCI_EXTENT_HEADER_SIZE + vci_GetOffsetArraySize(numRowsInExtent))

/** One entry of column_info in VCI main relation
 */
typedef struct vcis_m_column
{
	Oid			meta_oid;		/** OID of metadata relation */
	Oid			data_oid;		/** OID of data relation */

	/*
	 * int16 max_columns_size;
	 */
	/** AttrNumber original_attribute_number; */
	int16		max_columns_size;
	int16		comp_type;		/** vcis_compression_type_t */
} vcis_m_column_t;

/** One entry of extent_info in VCI main relation
 */
typedef struct vcis_m_extent
{
	/** number of rows recorded, including marked as deleted. */
	uint32		num_rows;
	uint32		num_deleted_rows;	/* number of rows marked as deleted. */
	uint32		num_deleted_rows_old;	/* num_deleted_rows for recovery */
	TransactionId xgen;			/* like xmin */
	TransactionId xdel;			/* like xmax */

	uint16		flags;
	uint16		recovered_colid;
} vcis_m_extent_t;

#define VCIS_M_EXTENT_FLAG_ENABLE_RECOVERED_COLID (0x0001)

/**
 * @brief VCI main relation header area to store by vci_WriteMainRelVar().
 *
 * vci_wmrv_all is used when the VCI relation is built, since first two or
 * three pages are defined in building time, then not modified at all.
 * The last page has ROS command, current ROS version, and extent information
 * so will be updated after creation.  vci_wmrv_update is used when the last
 * page is updated.
 */
typedef enum vci_wmrv_t
{
	vci_wmrv_update,			/** Only the last header page will be wrote to storage */
	vci_wmrv_all,				/** All the header pages will be wrote to storage */
} vci_wmrv_t;

/** I categorized ROS data like TID, NULL bit vector, normal column data
 * as shown below.
 */
typedef enum vcis_attribute_type_t
{
	vcis_attribute_type_main = 0,	/* data only */
	vcis_attribute_type_data_wos,	/* data only */
	vcis_attribute_type_whiteout_wos,	/* data only */
	vcis_attribute_type_tid_crid,	/* special type, meta and data */
	vcis_attribute_type_tid_crid_update,	/* data only */	/* two elements */
	vcis_attribute_type_delete_vec, /* normal column type */
	vcis_attribute_type_null_vec,	/* normal column type */
	vcis_attribute_type_tid,	/* normal column type */
	vcis_attribute_type_pgsql,	/* normal column type */
	/* number of indexed columns */
	num_vcis_attribute_type,
} vcis_attribute_type_t;

/**
 * @brief Gives how many colums or data belong to the given category.
 *
 * Some categories, defined in vcis_attribute_type_t, have multiple elements.
 * For example, vcis_attribute_type_pgsql category contains all the columns
 * given in CREATE INDEX command. This function gives how many colums or data
 * belong to the given category.
 *
 * @param[in] attrType Attribute type define in vcis_attribute_type_t.
 * For normal columns, it takes vcis_attribute_type_pgsql.
 * @param[in] numColumns The number of columns, which is returned when
 * attrType is vcis_attribute_type_pgsql.
 */
static inline int
vci_GetNumIndexForAttributeType(vcis_attribute_type_t attrType,
								int16 numColumns)
{
	return (vcis_attribute_type_pgsql == attrType) ? numColumns
		: ((vcis_attribute_type_tid_crid_update == attrType) ? 2
		   : ((0 <= attrType) && (attrType < num_vcis_attribute_type)) ? 1
		   : 0);
}

extern PGDLLEXPORT int vci_GetSumOfAttributeIndices(int16 numColumns);
extern PGDLLEXPORT void vci_GetAttrTypeAndIndexFromSumOfIndices(
																vcis_attribute_type_t *attrType,
																int *index,
																int16 numColumns,
																int sumOfIndex);

typedef enum vcis_compression_type_t
{
	vcis_compression_type_invalid = -1,
	vcis_compression_type_fixed_raw = 0,
	vcis_compression_type_variable_raw,
	vcis_compression_type_fixed_comp,	/* reserved */
	vcis_compression_type_auto, /* reserved */
	num_vcis_compression_type,
} vcis_compression_type_t;

typedef enum vcis_extent_type_t
{
	/** initial value is zero, since newly created DB page is filled with zero.
	 */
	vcis_undef_space = 0,

	vcis_extent_type_data,
	vcis_extent_type_dict,
	vcis_free_space,

	vcis_tidcrid_type_leaf,
	vcis_tidcrid_type_trunk,
	vcis_tidcrid_type_pagetag,

	num_vcis_extent_type,
} vcis_extent_type_t ,
vcis_tidcrid_item_type_t;

/** Type(s) of dictionary.
 */
typedef enum vcis_dict_type_t
{
	/** initial value is zero, since newly created DB page is filled with zero.
	 */
	vcis_dict_type_none = 0,
	vcis_dict_type_lzvf,
	num_vcis_dict_type,
} vcis_dict_type_t;

/** Type(s) of operations in updating TID-CRID tree.
 */
typedef enum
{
	vcis_tid_crid_op_none = 0,
	vcis_tid_crid_op_trunk,
	vcis_tid_crid_op_leaf_add,
	vcis_tid_crid_op_leaf_remove,
} vcis_tid_crid_op_type_t;

#define vci_GetBlockNumberFromUint64(tId) \
	((tId) >> (BITS_PER_BYTE * sizeof(OffsetNumber)))
#define vci_GetOffsetFromUint64(tId) \
	((tId) & ((1U << (BITS_PER_BYTE * sizeof(OffsetNumber))) - 1))
#define vci_MakeUint64FromBlockNumberAndOffset(blockNumber, offset) \
	(((uint64) (blockNumber) << (BITS_PER_BYTE * sizeof(OffsetNumber))) | (offset))

/** Local delete list */
typedef struct vci_local_delete_list
{
	uint32		num_entry;		/* the number of CRID stored */
	uint32		length;			/* capacity of crid_list */
	uint64	   *crid_list;		/* actual values taken from whiteout WOS */
} vci_local_delete_list;

struct vci_CSFetchContextData;

/** Local ROS */
typedef struct vci_local_ros
{
	vci_local_delete_list local_delete_list;

	/** Number of extents of local ROS.
	 * The minimum extent ID of the local ROS is (-num_local_extents).
	 */
	uint32		num_local_extents;

	/** Pointer of the array of pointers to extent data.
	 * When release the data, first pfree(extent[i]) where i is from zero
	 * to (num_local_extents - 1), then pfree(extent).
	 */
	struct vci_virtual_tuples **extent;

	/* Memory context to store local ROS data */
	MemoryContext memory_context;

	/* not localized one */
	/** this fetch_context is allocated in shared memory context created
	 * in vci_GenerateLocalRos(), and destructed in vci_DestroyLocalRos().
	 * In the latter function, the fetch_context is freed automatically.
	 */
	struct vci_CSFetchContextData *fetch_context;
} vci_local_ros_t;

typedef struct vci_RelationPair
{
	vci_MainRelHeaderInfo *info;

	Relation	meta;
	Relation	data;

	Buffer		bufMeta;
	Buffer		bufData;
} vci_RelationPair;

extern PGDLLEXPORT void vci_InitMainRelHeaderInfo(vci_MainRelHeaderInfo *info,
												  Relation rel,
												  vci_ros_command_t command);
extern void vci_KeepMainRelHeaderWithoutVersionCheck(vci_MainRelHeaderInfo *info);
extern PGDLLEXPORT void vci_KeepMainRelHeader(vci_MainRelHeaderInfo *info);
extern void vci_ChangeCommand(vci_MainRelHeaderInfo *info, vci_ros_command_t command);

extern PGDLLEXPORT void vci_ReleaseMainRelHeader(vci_MainRelHeaderInfo *info);

extern void vci_SetMainRelVar(vci_MainRelHeaderInfo *info,
							  vci_MainRelVar var,
							  int elemId,
							  uint32 value);
extern PGDLLEXPORT uint32 vci_GetMainRelVar(vci_MainRelHeaderInfo *info,
											vci_MainRelVar var,
											int elemId);
extern void vci_WriteMainRelVar(vci_MainRelHeaderInfo *info,
								vci_wmrv_t writeArea);

extern void vci_InitPageCore(Buffer buffer, int16 numItem, bool locked);
extern void vci_InitPage(Relation rel, BlockNumber blockNumber, int16 numItem);

extern Buffer vci_ReadBufferWithPageInit(Relation reln, BlockNumber blockNumber);
extern Buffer vci_ReadBufferWithPageInitDelVec(Relation reln, BlockNumber blockNumber);

/*
 * In order to keep the heap tuple plane, set 'p' to attstorage in
 * FormData_pg_attribute.
 */

extern PGDLLEXPORT vci_MainRelVar vci_GetMColumnPosition(int16 columnId);
extern PGDLLEXPORT vcis_m_column_t *vci_GetMColumn(vci_MainRelHeaderInfo *info, int16 columnId);
extern PGDLLEXPORT vcis_m_extent_t *vci_GetMExtent(Buffer *buffer, vci_MainRelHeaderInfo *info, int32 extentId);

extern void vci_GetExtentInfoPosition(BlockNumber *blockNumber,
									  OffsetNumber *offset,
									  int32 extentId);
extern bool vci_ExtentInfoExists(vci_MainRelHeaderInfo *info, int32 extentId);
extern bool vci_ExtentIsVisible(vcis_m_extent_t *mExtent, TransactionId xid);
extern bool vci_ExtentIsCollectable(vcis_m_extent_t *mExtent, TransactionId wos2rosXid);
extern bool vci_ExtentIsFree(vcis_m_extent_t *extentInfo);

extern uint32 vci_GetFreeExtentId(vci_MainRelHeaderInfo *info);
extern PGDLLEXPORT int16 vci_GetColumnWorstSize(Form_pg_attribute attr);

/* **************************************
 * ** CAUTION: AttrNumber is 1 origin. **
 * **************************************
 */
extern Size vci_GetColumnIdsAndSizes(AttrNumber *heapAttrNumList,
									 int16 *indxColumnIdList,
									 int16 *columnSizeList,
									 int numColumn,
									 vci_MainRelHeaderInfo *info,
									 Oid heapOid);
extern void vci_WriteExtentInfoInMainRosForWriteExtentOrCommonDict(
																   vci_MainRelHeaderInfo *info,
																   int32 extentId,
																   int32 dictionaryId,
																   TransactionId xid,
																   vci_ros_command_t command);

static inline void
vci_WriteExtentInfoInMainRosForWriteExtent(vci_MainRelHeaderInfo *info,
										   int32 extentId,
										   TransactionId xid,
										   vci_ros_command_t command)
{
	vci_WriteExtentInfoInMainRosForWriteExtentOrCommonDict(info, extentId,
														   VCI_INVALID_DICTIONARY_ID,
														   xid, command);
}

static inline void
vci_SetItemPointerFromTid64(ItemPointer item, uint64 tId)
{
	ItemPointerSet(item,
				   vci_GetBlockNumberFromUint64(tId),
				   vci_GetOffsetFromUint64(tId));
}

static inline uint64
vci_GetTid64FromItemPointer(ItemPointer item)
{
	uint64		blockNumber;

	Assert(NULL != item);
	blockNumber = BlockIdGetBlockNumber(&(item->ip_blkid));

	return vci_MakeUint64FromBlockNumberAndOffset(blockNumber, item->ip_posid);
}

/* **************************************
 * ** CAUTION: AttrNumber is 1 origin. **
 * **************************************
 */
extern Buffer vci_WriteOnePageIfNecessaryAndGetBuffer(Relation relation,
													  BlockNumber blockNumber,
													  BlockNumber blockNumberOld,
													  Buffer buffer);
extern void vci_WriteExtentInfo(vci_MainRelHeaderInfo *info,
								int32 extentId,
								uint32 numRows,
								uint32 numDeletedRows,
								uint32 numDeletedRowsOld,
								TransactionId xgen,
								TransactionId xdel);

/*
 * *********************************************************
 * functions to recover ROS
 * *********************************************************
 */
extern void vci_RecoverOneVCIIfNecessary(vci_MainRelHeaderInfo *info);

extern PGDLLEXPORT void
			vci_PreparePagesIfNecessaryCore(Relation rel,
											BlockNumber blockNumber,
											uint16 numItems,
											bool forceInit,
											bool logItems);

/**
 * @brief This function checks if the relation has the DB page with the page ID
 * blockNumber.
 *
 * When it does not exists, the function extends the relation and initialize
 * extended pages with one item per page.
 *
 * @param[in] rel The relation.
 * @param[in] blockNumber The block number to be examined.
 * @param[in] numItems The number of items the page is initialized with.
 */
static inline void
vci_FormatPageWithItems(Relation rel, BlockNumber blockNumber, int16 numItems)
{
	vci_PreparePagesIfNecessaryCore(rel, blockNumber, numItems, true, false);
}

static inline void
vci_PreparePagesIfNecessary(Relation rel, BlockNumber blockNumber, uint16 numItems)
{
	vci_PreparePagesIfNecessaryCore(rel, blockNumber, numItems, false, false);
}

extern PGDLLEXPORT void vci_WriteItem(Relation rel,
									  Buffer buffer,
									  OffsetNumber itemId);

extern void
			vci_UpdateOldFieldsInMetaHeader(Relation rel, TransactionId xId);
extern PGDLLEXPORT uint16
			vci_GetFixedColumnSize(vci_MainRelHeaderInfo *info, int16 columnId);
extern PGDLLEXPORT void
			vci_GetPositionForFixedColumn(BlockNumber *blockNumber,
										  uint32 *offset,
										  vci_MainRelHeaderInfo *info,
										  int16 columnId,
										  int32 extentId,
										  uint32 rowIdInExtent,
										  bool atEnd);

extern int	vci_GetNumberOfNullableColumn(TupleDesc tupleDesc);
extern PGDLLEXPORT int16 vci_GetBitIdInNullBits(TupleDesc tupleDesc, int16 columnId);

extern PGDLLEXPORT Snapshot vci_GetCurrentSnapshot(void);
extern void vci_FinalizeCopyCommand(void);

struct vci_CSQueryContextData;
extern struct vci_local_ros *vci_GenerateLocalRos(
												  struct vci_CSQueryContextData *queryContext,

 /* maximum memory size to generate and keep local ROS */
												  Size workareaSize,

 /* the number of rows from data WOS to local ROS */
												  int64 numDataWosRows,

 /* the number of rows from whiteout WOS to local delete list */
												  int64 numWhiteoutWosRows);

static inline unsigned int
vci_GetNumRowsInLocalRosExtent(int numColumns)
{
	unsigned int numRowsInExtent = MaxAllocSize / Max(

	/*
	 * The size of area to store pointers to larger data or values of small
	 * fixed length directly, say each size is smaller than or equal to
	 * sizeof(Datum). We allocate one are for all columns to support both row
	 * wise and column wise access.
	 */
													  sizeof(Datum) * numColumns,

	/*
	 * The size of area to store with larger size than sizeof(Datum). The data
	 * in the area is pointed from pointers stored in above area, so we can
	 * allocate separately.
	 */
													  MaxHeapTupleSize);

	return 1U << vci_GetHighestBit(Min(numRowsInExtent, VCI_NUM_ROWS_IN_EXTENT));
}

extern void vci_DestroyLocalRos(vci_local_ros_t *localRos);

#define vci_WriteExtentInfoInMainRosForWosRosConvInit(info, extentId, xid) \
	vci_WriteExtentInfoInMainRosForWriteExtent((info), \
											   (extentId), \
											   (xid), \
											   vci_rc_wos_ros_conv)

#define vci_WriteExtentInfoInMainRosForCopyInit(info, extentId, xid) \
	vci_WriteExtentInfoInMainRosForWriteExtent((info), \
											   (extentId), \
											   (xid), \
											   vci_rc_copy_command)

/*
 *
 */
static inline void
vci_PreparePagesWithOneItemIfNecessary(Relation relation,
									   BlockNumber blockNumber)
{
	vci_PreparePagesIfNecessary(relation, blockNumber, 1);
}

/* this function set the dirty bit, and write all the items in the page
 * to the WAL.
 * arguments
 * Relation rel
 * Buffer buffer
 */
static inline void
vci_WriteOneItemPage(Relation rel,
					 Buffer buffer)
{
	vci_WriteItem(rel, buffer, FirstOffsetNumber);
}

/* Initialize a DB page with one item format
 * argumtents
 * Relation relation
 * BlockNumber blockNumber
 */
static inline void
vci_InitOneItemPage(Relation relation, BlockNumber blockNumber)
{
	vci_InitPage(relation, blockNumber, 1);
}

static inline void
vci_FormatPageWithOneItem(Relation rel, BlockNumber blockNumber)
{
	vci_FormatPageWithItems(rel, blockNumber, 1);
}

static inline uint32
vci_VarSizeAny(char *ptr)
{
	if (!VARATT_IS_1B(ptr))
	{
		static varattrib_4b tmp;

		memcpy(&tmp, ptr, sizeof(varattrib_4b));

		return VARSIZE_4B(&tmp);
	}

	return VARSIZE_ANY(ptr);
}

static inline bool
vci_PassByRefForFixed(Form_pg_attribute attr)
{
#ifndef USE_FLOAT8_BYVAL
	if (8 == attr->attlen)
		return true;
#endif							/* #ifndef USE_FLOAT8_BYVAL */

	return sizeof(Datum) < (unsigned long) attr->attlen;
}

static inline void *
vci_repalloc(void *ptr, size_t size)
{
	return ptr ? repalloc_array(ptr, char, size) : palloc_array(char, size);
}

static inline bool
vci_GetBit(uint8 *bitArray, int bitId)
{
	return (bitArray[bitId >> 3] >> (bitId & 7)) & 1;
}

typedef struct vci_DictInfo
{
	/*
	 * Memory area to read dictionary. This is not used when create new
	 * dictionaries.
	 */
	unsigned char *dictionary_storage;

	Size		storage_size;	/* byte size of dictionary_storage */

	/*
	 * The extent ID for individual dictionary. VCI_INVALID_EXTENT_ID for
	 * common dictionaries.
	 */
	int32		extent_id;

	/* VCI_INVALID_DICTIONARY_ID for individual dictionary */
	int16		common_dict_id;

	vcis_dict_type_t dict_type;

} vci_DictInfo;

Buffer
			vci_WriteDataIntoMultiplePages(Relation rel,
										   BlockNumber *blockNumber,
										   BlockNumber *blockNumberOld,
										   uint32 *offsetInPage,
										   Buffer buffer,
										   const void *data_,
										   Size size);

typedef struct vci_meta_item_scanner
{
	bool		inited;

	Relation	rel;
	int			index;

	BlockNumber end_block;		/* inclusive */
	BlockNumber start_block;

	Buffer		buffer;
	BlockNumber current_block;

	int			max_item;
	int			max_item_in_page;
	int			item_size;

	int			buf_lockmode;

} vci_meta_item_scanner_t;

typedef struct
{
	Oid			oid;			/* Oid of VCI main relation */
	Oid			dbid;			/* Oid of database to which a VCI main
								 * relation belongs */
	bool		force_next_wosros_conv; /* flag to force WOS->ROS conversion
										 * on next time */
} vci_wosros_conv_worker_arg_t;

extern vcis_m_extent_t *vci_GetMExtentNext(vci_MainRelHeaderInfo *info, vci_meta_item_scanner_t *scan);
extern vci_meta_item_scanner_t *vci_BeginMetaItemScan(Relation rel, int buf_lock);
extern void vci_EndMetaItemScan(vci_meta_item_scanner_t *scan);

/* recovery functions for command */
extern void vci_UpdateLastRosVersionAndOthers(vci_MainRelHeaderInfo *info);
extern void vci_RecoveryDone(vci_MainRelHeaderInfo *info);
extern void vci_WriteRecoveryRecordDone(vci_MainRelHeaderInfo *info, vci_ros_command_t command, TransactionId xid);

extern void vci_WriteRecoveryRecordForExtentInfo(vci_MainRelHeaderInfo *info,
												 int32 newExtentId, int32 oldExtentId);
extern void vci_RecoveryExtentInfo(vci_MainRelHeaderInfo *info, vci_ros_command_t command);

extern void vci_WriteRecoveryRecordForUpdateDelVec(vci_MainRelHeaderInfo *info);
extern void vci_RecoveryUpdateDelVec(vci_MainRelHeaderInfo *info);
extern const char *vci_GetRosCommandName(vci_ros_command_t command);

/* ----------------
 *   vci_index.c
 * ----------------
 */

extern bool vci_isVciAdditionalRelation(Relation rel);
extern bool vci_isVciAdditionalRelationTuple(Oid reloid, Form_pg_class reltuple);

/* ----------------
 *   vci_internal_view.c
 * ----------------
 */

extern void vci_check_prohibited_operation(Node *parseTree, bool *creating_vci_extension);

#endif							/* VCI_ROS_H */
