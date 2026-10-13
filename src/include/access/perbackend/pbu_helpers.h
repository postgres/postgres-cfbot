/*-------------------------------------------------------------------------
 *
 * pbu_helpers.h
 *	  small helpers the per-backend UNDO engine relied on that were either
 *	  zheap-private (GetEpochForXid, dbid_exists,
 *	  SetCurrentUndoLocation) or provided as macros in a zheap-patched
 *	  access/transam.h (GetXidFromEpochXid, GetEpochFromEpochXid).
 *
 * The helpers here are real implementations against the current master API.
 *
 * Per-backend undo engine derives from EnterpriseDB zheap
 * (Amit Kapila, Dilip Kumar, Kuntal Ghosh, Mahendra Singh Thalor,
 *  Rafia Sabih, Thomas Munro et al.).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/perbackend/pbu_helpers.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PBU_HELPERS_H
#define PBU_HELPERS_H

#include "access/perbackend/pbu_undolog.h"
#include "access/transam.h"

/* Extract xid / epoch from a 64-bit value comprised of epoch<<32 | xid. */
#ifndef GetXidFromEpochXid
#define GetXidFromEpochXid(epochxid)	((uint32) (epochxid) & 0XFFFFFFFF)
#endif
#ifndef GetEpochFromEpochXid
#define GetEpochFromEpochXid(epochxid)	((uint32) ((epochxid) >> 32))
#endif

/* Compute the epoch of a (possibly past) plain TransactionId. */
extern uint32 GetEpochForXid(TransactionId xid);

/* True iff a database with the given OID currently exists. */
extern bool dbid_exists(Oid dbid);

/*
 * Record the transaction's current undo insertion location on the
 * TransactionState (via SetCurrentTransactionPbuUndoLocation in xact.c).
 */
extern void SetCurrentUndoLocation(UndoRecPtr urec_ptr);

#endif							/* PBU_HELPERS_H */
