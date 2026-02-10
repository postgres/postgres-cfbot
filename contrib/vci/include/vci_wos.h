/*-------------------------------------------------------------------------
 *
 * vci_wos.h
 *	  Declarations of WOS functions
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_wos.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_WOS_H
#define VCI_WOS_H

#include "postgres.h"

#include "storage/itemptr.h"
#include "lib/rbtree.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

extern Snapshot vci_GetSnapshotForWos2Ros(void);
extern Snapshot vci_GetSnapshotForLocalRos(TransactionId inclusive_xid, TransactionId exclusive_xid);

extern PGDLLEXPORT uint64 vci_EstimateNumEntriesInHeapRelation(Oid oid);

#endif							/* VCI_WOS_H */
