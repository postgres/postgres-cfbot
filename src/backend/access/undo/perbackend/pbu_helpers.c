/*-------------------------------------------------------------------------
 *
 * pbu_helpers.c
 *	  small helpers for the per-backend UNDO engine (see pbu_helpers.h).
 *
 * These back the handful of zheap-private helpers the vendored engine calls.
 * GetEpochForXid/dbid_exists are real implementations against master;
 * SetCurrentUndoLocation records the undo location on the TransactionState
 * (xact.c).
 *
 * Per-backend undo engine derives from EnterpriseDB zheap
 * (Amit Kapila, Dilip Kumar, Kuntal Ghosh, Mahendra Singh Thalor,
 *  Rafia Sabih, Thomas Munro et al.).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/perbackend/pbu_helpers.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/perbackend/pbu_compat.h"
#include "access/perbackend/pbu_helpers.h"
#include "access/transam.h"
#include "access/xact.h"
#include "utils/syscache.h"

/*
 * GetEpochForXid
 *		Return the epoch that the given plain TransactionId belongs to,
 *		relative to the current nextXid.  zheap kept this in varsup.c; master
 *		tracks a FullTransactionId nextXid in TransamVariables, so we derive
 *		the epoch from it, accounting for xid wraparound relative to nextXid.
 */
uint32
GetEpochForXid(TransactionId xid)
{
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		epoch;

	nextFullXid = ReadNextFullTransactionId();
	nextXid = XidFromFullTransactionId(nextFullXid);
	epoch = EpochFromFullTransactionId(nextFullXid);

	/*
	 * If xid is greater than the low 32 bits of nextXid, it must belong to
	 * the previous epoch (nextXid has wrapped past it).
	 */
	if (xid > nextXid)
		epoch--;

	return epoch;
}

/*
 * dbid_exists
 *		True iff a database with the given OID currently exists.
 */
bool
dbid_exists(Oid dbid)
{
	return SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(dbid));
}

/*
 * SetCurrentUndoLocation
 *		Record the transaction's current per-backend undo insertion location so
 *		abort-time rollback can find the chain.  Stored on the current
 *		TransactionState (kept separate from the cluster-wide engine's
 *		undoRecPtr) via xact.c; the first location recorded in a transaction is
 *		its rollback start point.
 */
void
SetCurrentUndoLocation(UndoRecPtr urec_ptr)
{
	UndoLogControl *log = UndoLogGet(UndoRecPtrGetLogNo(urec_ptr));

	StaticAssertStmt(UndoPersistenceLevels == PBU_UNDO_PERSISTENCE_LEVELS,
					 "per-backend undo persistence levels out of sync");
	SetCurrentTransactionPbuUndoLocation(log->meta.persistence,
										 (uint64) urec_ptr);
}
