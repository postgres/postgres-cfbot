/*-------------------------------------------------------------------------
*
 * varsup.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/varsup.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VARSUP_H
#define VARSUP_H

#include "access/transam.h"
#include "port/atomics.h"
#ifdef USE_ASSERT_CHECKING
#include "storage/lwlock.h"
#endif


/*
 * TransamVariables is a data structure in shared memory that is used to track
 * OID and XID assignment state.  For largely historical reasons, there is
 * just one struct with different fields that are protected by different
 * LWLocks.
 *
 * Note: xidWrapLimit and oldestXidDB are not "active" values, but are
 * used just to generate useful messages when xidWarnLimit or xidStopLimit
 * are exceeded.
 */
typedef struct TransamVariablesData
{
	/*
	 * These fields are protected by OidGenLock.
	 */
	Oid8		nextOid;		/* next OID (8 bytes) to assign */
	uint32		oidCount;		/* OIDs available before must do XLOG work */

	/*
	 * These fields are protected by XidGenLock.
	 */
	FullTransactionId nextXid;	/* next XID to assign */

	TransactionId oldestXid;	/* cluster-wide minimum datfrozenxid */
	TransactionId xidVacLimit;	/* start forcing autovacuums here */
	TransactionId xidWarnLimit; /* start complaining here */
	TransactionId xidStopLimit; /* refuse to advance nextXid beyond here */
	TransactionId xidWrapLimit; /* where the world ends */
	Oid			oldestXidDB;	/* database with minimum datfrozenxid */

	/*
	 * These fields are protected by CommitTsLock
	 */
	TransactionId oldestCommitTsXid;
	TransactionId newestCommitTsXid;

	/*
	 * These fields are protected by ProcArrayLock.
	 */
	FullTransactionId latestCompletedXid;	/* newest full XID that has
											 * committed or aborted */

	/*
	 * Number of top-level transactions with xids (i.e. which may have
	 * modified the database) that completed in some form since the start of
	 * the server. This currently is solely used to check whether
	 * GetSnapshotData() needs to recompute the contents of the snapshot, or
	 * not. There are likely other users of this.  Always above 1.
	 */
	pg_atomic_uint64 xactCompletionCount;

	/*
	 * These fields are protected by XactTruncationLock
	 */
	TransactionId oldestClogXid;	/* oldest it's safe to look up in clog */

} TransamVariablesData;


extern PGDLLIMPORT TransamVariablesData *TransamVariables;

static inline void
IncrementXactCompletionCount(void)
{
	uint64	xcc;

	Assert(LWLockHeldByMeInMode(ProcArrayLock, LW_EXCLUSIVE));

	xcc = pg_atomic_read_u64(&TransamVariables->xactCompletionCount);
	xcc += 1;
	pg_atomic_write_u64(&TransamVariables->xactCompletionCount, xcc);
}

#endif /* VARSUP_H */
