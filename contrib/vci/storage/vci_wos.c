/*-------------------------------------------------------------------------
 *
 * vci_wos.c
 *		Manipulate WOS
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_wos.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdint.h>

#include "access/heapam_xlog.h"
#include "access/relscan.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "c.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "vci.h"

#include "vci_mem.h"
#include "vci_ros.h"
#include "vci_wos.h"
#include "vci_xact.h"

bool		HeapTupleSatisfiesWos2Ros(HeapTuple htup, Snapshot snapshot, Buffer buffer);
bool		HeapTupleSatisfiesLocalRos(HeapTuple htup, Snapshot snapshot, Buffer buffer);
static bool IsXmaxHasCommitted(HeapTuple htup);

/* Cache used by IsXmaxHasCommitted */
static struct
{
	TransactionId xid;
	bool		committed;
}			cachedTransactionInfo;

/*
 * vci_GetSnapshotForWos2Ros
 *
 * Creates a snapshot which is used for WOS->ROS and WOS->Delete vector
 * conversions.
 *
 * WOS entries are created when CRUD commands are executed, and the visibility
 * check in WOS is done with the normal snapshot.
 *
 * ROS control commands can removes WOS entries, and the result can be seen by
 * everyone as soon as the command is done.
 *
 * Caller must call PopActiveSnapshot() afterward.
 */
Snapshot
vci_GetSnapshotForWos2Ros(void)
{
	Snapshot	snapshot;

	snapshot = vci_GetCurrentSnapshot();

	snapshot->snapshot_type = SNAPSHOT_VCI_WOS2ROS;

	/* Clean up the cache */
	cachedTransactionInfo.xid = InvalidTransactionId;

	return snapshot;
}

bool
HeapTupleSatisfiesWos2Ros(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{

	SnapshotType temp_snapshot_type;

	temp_snapshot_type = snapshot->snapshot_type;
	snapshot->snapshot_type = SNAPSHOT_MVCC;

	if (HeapTupleSatisfiesVisibility(htup, snapshot, buffer))
	{
		snapshot->snapshot_type = temp_snapshot_type;

		if (IsXmaxHasCommitted(htup))
			return false;

		return true;
	}

	snapshot->snapshot_type = temp_snapshot_type;
	return false;
}

static TransactionId exclusiveTransactionId;

/*
 * vci_GetSnapshotForLocalRos
 *
 * Creates a snapshot which is used for local ROS conversion
 *
 * @param[in] inclusive_xid Visible xid regardless of the MVCC snapshot
 * @param[in] exclusive_xid Invisible xid regardless of the MVCC snapshot
 *
 * Mostly same as vci_GetSnapshotForWos2Ros(), but sometimes results by ROS
 * control commands cannot be seen by MVCC. Because the transactions creating
 * local ROS and ROS control commands are sometimes overlapped.
 */
Snapshot
vci_GetSnapshotForLocalRos(TransactionId inclusive_xid, TransactionId exclusive_xid)
{
	Snapshot	snapshot;

	snapshot = vci_GetCurrentSnapshot();

	snapshot->snapshot_type = SNAPSHOT_VCI_LOCALROS;

	/* Removes transaction inclusive_xid from MVCC control */
	if (TransactionIdIsValid(inclusive_xid))
	{
		for (int i = 0; i < snapshot->xcnt; i++)
		{
			if (TransactionIdEquals(snapshot->xip[i], inclusive_xid))
			{
				i++;
				for (; i < snapshot->xcnt; i++)
					snapshot->xip[i - 1] = snapshot->xip[i];
				snapshot->xcnt--;
				break;
			}
		}
	}

	exclusiveTransactionId = exclusive_xid;

	/* Clean up the cache */
	cachedTransactionInfo.xid = InvalidTransactionId;

	return snapshot;
}

bool
HeapTupleSatisfiesLocalRos(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	SnapshotType temp_snapshot_type;

	/* Store away the VCI specific type and check for MVCC visibility */
	temp_snapshot_type = snapshot->snapshot_type;
	snapshot->snapshot_type = SNAPSHOT_MVCC;

	if (HeapTupleSatisfiesVisibility(htup, snapshot, buffer))
	{
		snapshot->snapshot_type = temp_snapshot_type;
		if (IsXmaxHasCommitted(htup))
		{
			TransactionId xmax;

			xmax = HeapTupleHeaderGetRawXmax(htup->t_data);

			if (TransactionIdEquals(xmax, exclusiveTransactionId))
				return true;

			return false;
		}

		return true;
	}

	snapshot->snapshot_type = temp_snapshot_type;
	return false;
}

/*
 * Checks whether the htup has been removed
 */
static bool
IsXmaxHasCommitted(HeapTuple htup)
{
	TransactionId xmax;
	bool		result = false;

	if (htup->t_data->t_infomask & HEAP_XMAX_COMMITTED)
		return true;

	xmax = HeapTupleHeaderGetRawXmax(htup->t_data);

	if (!TransactionIdIsValid(xmax))
		return false;

	if (htup->t_data->t_infomask & HEAP_XMAX_INVALID)
		return false;

	if (TransactionIdEquals(xmax, cachedTransactionInfo.xid))
		return cachedTransactionInfo.committed;

	switch (vci_transaction_get_type(xmax))
	{
		case VCI_XACT_SELF:
		case VCI_XACT_DID_COMMIT:
			result = true;
			break;

		default:
			break;
	}

	cachedTransactionInfo.xid = xmax;
	cachedTransactionInfo.committed = result;

	return result;
}

/**
 * @brief This function estimate the number of items in all pages of a heap
 * relation, from the item size and number of pages, assuming that all the
 * entries has the same size, and no HOT chains.
 *
 * @param[in] oid Oid of relation.
 * @return estimated number of items in the relation.
 */
uint64
vci_EstimateNumEntriesInHeapRelation(Oid oid)
{
	if (OidIsValid(oid))
	{
		Relation	rel;
		TableScanDesc scan;
		HeapTuple	tuple;
		uint64		result = 0;

		rel = table_open(oid, AccessShareLock);
		scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
		scan->rs_flags &= ~SO_ALLOW_PAGEMODE;
		tuple = heap_getnext(scan, ForwardScanDirection);

		if (NULL != tuple)
		{
			BlockNumber relallvisible;
			uint64		numEntriesPerPage = (BLCKSZ - SizeOfPageHeaderData) /
				(tuple->t_len + sizeof(ItemIdData));

			/*
			 * Estimated value would be calculated as: - Subtract the free
			 * page from the total number of pages, - then multiple the
			 * maximum entries per page.
			 */
			visibilitymap_count(rel, &relallvisible, NULL);
			result = (RelationGetNumberOfBlocks(rel) - relallvisible) * numEntriesPerPage;
		}

		table_endscan(scan);
		table_close(rel, AccessShareLock);

		return result;
	}

	return 0;
}
