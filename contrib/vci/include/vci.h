/*-------------------------------------------------------------------------
 *
 * vci.h
 *	  Primary include file for VCI .c files
 *
 * This should be the first file included by VCI modules.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_H
#define VCI_H

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("vci")

#include "postgres.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/relscan.h"
#include "c.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_am.h"
#include "executor/nodeModifyTable.h"
#include "storage/itemptr.h"	/* for ItemPointer */
#include "tcop/utility.h"
#include "utils/rel.h"
#include "utils/relcache.h"		/* for Relation */
#include "utils/syscache.h"

#define VCI_STRING    "vci"

#define VCI_INTERNAL_RELATION_TEMPLATE	"pg_vci_%010d_%05d_%c"

/** Use compact form to keep varlena with short header at some parts */
#define VCI_USE_COMPACT_VARLENA

#ifdef WIN32
#define strtok_r strtok_s
#endif

/** Restart time for Daemon(Background Worker) */
#define VCI_DAEMON_RESTART_TIME		(3)

/**
 * Scan policy
 */
typedef enum
{
	VCI_TABLE_SCAN_POLICY_NONE,
	VCI_TABLE_SCAN_POLICY_COLUMN_ONLY,	/* Only reads column store */
} VciTableScanPolicy;

/**
 * VCI Scan mode
 */
typedef enum
{
	VCI_SCAN_MODE_NONE,
	VCI_SCAN_MODE_COLUMN_STORE, /* Reads column store */
} VciScanMode;

typedef struct VciFetchPos
{
	int64		fetch_starting_crid;

	int32		current_extent_id;

	int			num_rows_in_extent;

	int			offset_in_extent;

	int			num_fetched_rows;

	int			current_row;
} VciFetchPos;

extern void vci_add_index_delete(Relation heapRel, const ItemPointerData *heap_tid, TransactionId xmin);
extern List *vci_add_should_index_insert(ResultRelInfo *resultRelInfo, TupleTableSlot *slot, ItemPointer tupleid, EState *estate);
extern bool vci_add_drop_relation(const ObjectAddress *object, int flags);
extern bool vci_add_reindex_index(Relation indexRel);
extern bool vci_add_skip_vci_index(Relation indexRel);
extern bool vci_add_alter_tablespace(Relation indexRel);
extern void vci_process_utility(PlannedStmt *pstmt, const char *queryString,
								bool readOnlyTree,
								ProcessUtilityContext context,
								ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest,
								QueryCompletion *qc);
extern void vci_alter_table_change_owner(Oid relOid, char relKind, Oid newOwnerId);
extern void vci_alter_table_change_schema(Oid relOid, char relKind, Oid newNspOid);

extern void vci_read_guc_variables(void);
extern void vci_setup_shmem(void);
extern void vci_shmem_startup_routine(void);
extern void vci_setup_executor_hook(void);
extern void vci_xact_change_handler(XactEvent event);
extern void vci_subxact_change_handler(SubXactEvent event, SubTransactionId mySubid);
extern void vci_set_copy_transaction_and_command_id(TransactionId xid,
													CommandId cid);

extern bool VCITupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot, Buffer buffer);

/*  for index_build */
typedef enum
{
	vcirc_invalid = 0,
	vcirc_reindex,
	vcirc_truncate,
	vcirc_vacuum_full,
	vcirc_cluster,
	vcirc_alter_table,

	vcirc_num
} vci_RebuildCommand;

extern vci_RebuildCommand vci_rebuild_command;

extern bool vci_is_in_vci_create_extension;

extern ProcessUtility_hook_type process_utility_prev;
extern ProcessUtility_hook_type post_process_utility_prev;

static inline
bool
isVciIndexRelation(Relation rel)
{
	if (rel->rd_rel->relam != 0)
	{
		Form_pg_am	aform;
		HeapTuple	amtuple;

		amtuple = SearchSysCache1(AMOID, ObjectIdGetDatum(rel->rd_rel->relam));
		if (!HeapTupleIsValid(amtuple))
			elog(ERROR, "cache lookup failed for access method %u",
				 rel->rd_rel->relam);
		aform = (Form_pg_am) GETSTRUCT(amtuple);
		ReleaseSysCache(amtuple);

		if (strcmp(NameStr(aform->amname), "vci") == 0)
			return true;
	}
	return false;
}
#endif							/* VCI_H */
