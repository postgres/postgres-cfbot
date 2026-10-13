/*-------------------------------------------------------------------------
 *
 * undorequest.h
 *	  Exports from undo/undorequest.c.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * src/include/access/undorequest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PBU_UNDOREQUEST_H
#define PBU_UNDOREQUEST_H

#include "access/perbackend/pbu_undoinsert.h"
#include "datatype/timestamp.h"
#include "utils/relcache.h"


/* different types of undo worker */
typedef enum
{
	XID_QUEUE = 0,
	SIZE_QUEUE = 1,
	ERROR_QUEUE
} UndoWorkerQueueType;

#define InvalidUndoWorkerQueue -1

/* Remembers the last seen RecentGlobalXmin (defined in pbu_undorequest.c) */
extern TransactionId latestRecentGlobalXmin;

/* GUC: rollback-request size threshold (defined in pbu_undorequest.c) */
extern int	undo_rollback_overflow_size;

/* This is the data structure for each hash table entry for rollbacks. */
typedef struct RollbackHashEntry
{
	FullTransactionId full_xid; /* must be first entry */
	UndoRecPtr	start_urec_ptr;
	UndoRecPtr	end_urec_ptr;
	Oid			dbid;
	bool		in_progress;	/* indicates that undo actions are being
								 * processed */
	bool		in_error_queue; /* failed; only its error queue element may
								 * hand it out again */
	int			nfailures;		/* attempts that did not complete it */
} RollbackHashEntry;

/* This is the data structure for each hash table key for rollbacks. */
typedef struct RollbackHashKey
{
	FullTransactionId full_xid;
	UndoRecPtr	start_urec_ptr;
} RollbackHashKey;

/* This is an entry for undo request queue that is sorted by xid. */
typedef struct UndoXidQueue
{
	FullTransactionId full_xid;
	UndoRecPtr	start_urec_ptr;
	Oid			dbid;
} UndoXidQueue;

/* This is an entry for undo request queue that is sorted by size. */
typedef struct UndoSizeQueue
{
	FullTransactionId full_xid;
	UndoRecPtr	start_urec_ptr;
	Oid			dbid;
	uint64		request_size;
} UndoSizeQueue;

/*
 * This is an entry for undo request queue that is sorted by time at which an
 * error has occurred.
 */
typedef struct UndoErrorQueue
{
	FullTransactionId full_xid;
	UndoRecPtr	start_urec_ptr;
	Oid			dbid;
	TimestampTz err_occurred_at;
} UndoErrorQueue;

/* undo record information */
typedef struct UndoRecInfo
{
	int			index;			/* Index of the element to make qsort stable. */
	UndoRecPtr	urp;			/* undo recptr (undo record location). */
	UnpackedUndoRecord *uur;	/* actual undo record. */
} UndoRecInfo;

/* undo request information */
typedef struct UndoRequestInfo
{
	FullTransactionId full_xid;
	UndoRecPtr	start_urec_ptr;
	UndoRecPtr	end_urec_ptr;
	Oid			dbid;
	uint64		request_size;
	UndoWorkerQueueType undo_worker_queue;
} UndoRequestInfo;

/* Reset the undo request info */
#define ResetUndoRequestInfo(urinfo) \
( \
	(urinfo)->full_xid = InvalidFullTransactionId, \
	(urinfo)->start_urec_ptr = InvalidUndoRecPtr, \
	(urinfo)->end_urec_ptr = InvalidUndoRecPtr, \
	(urinfo)->dbid = InvalidOid, \
	(urinfo)->request_size = 0, \
	(urinfo)->undo_worker_queue = InvalidUndoWorkerQueue \
)

/* set the undo request info from the rollback request */
#define SetUndoRequestInfoFromRHEntry(urinfo, rh, cur_queue) \
( \
	urinfo->full_xid = rh->full_xid, \
	urinfo->start_urec_ptr = rh->start_urec_ptr, \
	urinfo->end_urec_ptr = rh->end_urec_ptr, \
	urinfo->dbid = rh->dbid, \
	urinfo->undo_worker_queue = cur_queue \
)

/* Exposed functions for rollback request queues. */
extern int	PendingUndoShmemSize(void);
extern void PendingUndoShmemRequest(void);
extern void PendingUndoShmemInit(void);
extern bool UndoWorkerQueuesEmpty(void);
extern void InsertRequestIntoUndoQueues(UndoRequestInfo *urinfo);
extern bool InsertRequestIntoErrorUndoQueue(volatile UndoRequestInfo *urinfo);
extern int	RollbackHTCountFailure(FullTransactionId full_xid,
								   UndoRecPtr start_urec_ptr);
extern void PbuMarkRollbackDone(FullTransactionId full_xid,
								UndoRecPtr start_urec_ptr);
extern void SetUndoWorkerQueueStart(UndoWorkerQueueType undo_worker_queue);
extern bool UndoGetWork(bool allow_peek, bool is_undo_launcher,
						UndoRequestInfo *urinfo, bool *in_other_db);

/* Exposed functions for rollback hash table. */
extern bool RegisterRollbackReq(UndoRecPtr end_urec_ptr, UndoRecPtr start_urec_ptr,
								Oid dbid, FullTransactionId full_xid);
extern void RollbackHTRemoveEntry(FullTransactionId full_xid, UndoRecPtr start_urec_ptr);
extern bool RollbackHTIsFull(void);
extern void RollbackHTCleanup(Oid dbid);

/* functions exposed from undoaction.c */
extern UndoRecInfo *UndoRecordBulkFetch(UndoRecPtr *from_urecptr,
										UndoRecPtr to_urecptr, int undo_apply_size,
										int *nrecords, bool one_page);
extern bool execute_undo_actions(FullTransactionId full_xid, UndoRecPtr from_urecptr,
								 UndoRecPtr to_urecptr, bool nopartial);
extern bool execute_undo_actions_page(UndoRecInfo *urp_array, int first_idx,
									  int last_idx, Oid reloid, FullTransactionId full_xid,
									  BlockNumber blkno, bool blk_chain_complete);

/* Crash-recovery + 2PC apply driver (pbu_recovery.c). */
extern int	PbuPerformUndoRecovery(void);
extern void PbuAtPrepare_Undo(void);
extern void pbu_twophase_postabort(FullTransactionId fxid, uint16 info,
								   void *recdata, uint32 len);
extern void pbu_twophase_postcommit(FullTransactionId fxid, uint16 info,
									void *recdata, uint32 len);

#endif							/* PBU_UNDOREQUEST_H */
