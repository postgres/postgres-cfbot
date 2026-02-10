/*-------------------------------------------------------------------------
 *
 * vci_xact.h
 *	  Transaction control
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_xact.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_XACT_H
#define VCI_XACT_H

#include "access/xact.h"

struct vci_MainRelHeaderInfo;

/*
 * States of transactions
 */
enum vci_xact_status_kind
{
	VCI_XACT_INVALID,			/* invalid transaction ID */
	VCI_XACT_SELF,				/* my transaction */
	VCI_XACT_IN_PROGRESS,		/* in-progress transaction (not mine) */
	VCI_XACT_DID_COMMIT,		/* committed transaction */
	VCI_XACT_DID_ABORT,			/* aborted transaction */
	VCI_XACT_DID_CRASH			/* crash was happened during the transaction */
};

extern enum vci_xact_status_kind vci_transaction_get_type(TransactionId xid);

extern int64 vci_GenerateXid64(TransactionId target_xid, struct vci_MainRelHeaderInfo *info);
extern void vci_UpdateXidGeneration(struct vci_MainRelHeaderInfo *info);

#endif							/* VCI_XACT_H */
