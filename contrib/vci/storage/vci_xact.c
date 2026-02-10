/*-------------------------------------------------------------------------
 *
 * vci_xact.c
 *	  Transaction control
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_xact.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "miscadmin.h"

#include "vci.h"
#include "vci_ros.h"
#include "vci_xact.h"

/*
 * Returns the status of the transaction
 */
enum vci_xact_status_kind
vci_transaction_get_type(TransactionId xid)
{
	/*
	 * XXX please preserve the ordering.
	 *
	 * TransactionIdDidCommit() and TransactionIdDidAbort() can be used when
	 * TransactionIdIsInProgress() is false
	 */
	if (!TransactionIdIsValid(xid))
		return VCI_XACT_INVALID;
	else if (xid == FrozenTransactionId)
		return VCI_XACT_DID_COMMIT;
	else if (TransactionIdIsCurrentTransactionId(xid))
		return VCI_XACT_SELF;
	else if (TransactionIdIsInProgress(xid))
		return VCI_XACT_IN_PROGRESS;
	else if (TransactionIdDidCommit(xid))
		return VCI_XACT_DID_COMMIT;
	else if (TransactionIdDidAbort(xid))
		return VCI_XACT_DID_ABORT;
	else
		return VCI_XACT_DID_CRASH;
}

/*==========================================================================*/
/* Extended transaction ID generations                                      */
/*==========================================================================*/

/*
 * WOS extends transaction IDs to 64bit, called "xid64". The upper 32bit is
 * same as normal xid, and the lower 32bit represents the "generation" of the
 * transaction.
 *
 * The generation can be advanced when the 1/4 32-bit transaction has been
 * advanced. Ideally it can be done when whole of them are consumed, but
 * current approach is simpler.
 *
 * vcimrv_xid_generation of the main relation stores the generation, and
 * vcimrv_xid_gen_update_xid stores 32-bit xid when the generation is lastly
 * advanced. E.g., either of XID which happened CREATE XID or VACUUM.
 *
 * While doing VACUUM, the upper 2 bit of vcimrv_xid_gen_update_xid and current
 * xid would be compared, and the generation would be advanced if they are
 * different.
 *
 * When the index is created, the generation is 1. If older transaction than
 * doing CREATE INDEX refers to the index (can happen if the isolation level is
 * READ COMMITTED), their generation would be 0.
 */

/* The length of shift used for advancing generations */
static const int xid_shift_bits = 30;

/*
 * Returns extended xid based on the given one.
 *
 * @param[in] target_xid  32-bit xid
 * @param[in] info information of the main relation
 * @return 64-bit xid
 */
int64
vci_GenerateXid64(TransactionId target_xid, vci_MainRelHeaderInfo *info)
{
	uint64		xid_gen;
	TransactionId base_xid;
	uint32		base_xid_upper_bits;
	uint32		target_xid_upper_bits;
	int32		diff;

	xid_gen = (uint64) vci_GetMainRelVar(info, vcimrv_xid_generation, 0);
	base_xid = vci_GetMainRelVar(info, vcimrv_xid_gen_update_xid, 0);

	base_xid_upper_bits = ((uint32) base_xid) >> xid_shift_bits;
	target_xid_upper_bits = ((uint32) target_xid) >> xid_shift_bits;

	diff = (target_xid_upper_bits - base_xid_upper_bits) << xid_shift_bits;

	return (int64) (((xid_gen + (diff >> xid_shift_bits)) << 32) | (uint64) target_xid);
}

/*
 * Updates the generation based on the current transaction ID.
 *
 * This can be called only from VACUUM, and won't be rolled back.
 */
void
vci_UpdateXidGeneration(vci_MainRelHeaderInfo *info)
{
	TransactionId cur_xid;
	uint32		xid_gen;
	uint32		base_xid;
	uint32		cur_xid_upper_bits;
	uint32		base_xid_upper_bits;
	int32		diff;

	cur_xid = GetCurrentTransactionId();

	xid_gen = (uint32) vci_GetMainRelVar(info, vcimrv_xid_generation, 0);
	base_xid = vci_GetMainRelVar(info, vcimrv_xid_gen_update_xid, 0);

	base_xid_upper_bits = ((uint32) base_xid) >> xid_shift_bits;
	cur_xid_upper_bits = ((uint32) cur_xid) >> xid_shift_bits;

	diff = (cur_xid_upper_bits - base_xid_upper_bits) << xid_shift_bits;

	if (diff != 0)
	{
		LockRelation(info->rel, AccessExclusiveLock);

		vci_SetMainRelVar(info, vcimrv_xid_generation, 0, xid_gen + (diff >> xid_shift_bits));
		vci_SetMainRelVar(info, vcimrv_xid_gen_update_xid, 0, cur_xid);

		vci_WriteMainRelVar(info, vci_wmrv_update);

		UnlockRelation(info->rel, AccessExclusiveLock);
	}
}
