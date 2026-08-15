/*-------------------------------------------------------------------------
 *
 * syncrep.c
 *
 * Synchronous replication is new as of PostgreSQL 9.1.
 *
 * If requested, transaction commits wait until their commit LSN are
 * acknowledged by the synchronous standbys.
 *
 * This module contains the code for waiting and release of backends.
 * All code in this module executes on the primary. The core streaming
 * replication transport remains within WALreceiver/WALsender modules.
 *
 * The essence of this design is that it isolates all logic about
 * waiting/releasing onto the primary. The primary defines which standbys
 * it wishes to wait for. The standbys are completely unaware of the
 * durability requirements of transactions on the primary, reducing the
 * complexity of the code and streamlining both standby operations and
 * network bandwidth because there is no requirement to ship
 * per-transaction state information.
 *
 * Replication is either synchronous or not synchronous (async). If it is
 * async, we just fastpath out of here. If it is sync, then we wait for
 * the write, flush or apply location on the standby before releasing
 * the waiting backend. Further complexity in that interaction is
 * expected in later releases.
 *
 * The best performing way to manage the waiting backends is to have a
 * single ordered queue of waiting backends, so that we can avoid
 * searching the through all waiters each time we receive a reply.
 *
 * In 9.5 or before only a single standby could be considered as
 * synchronous. In 9.6 we support a priority-based multiple synchronous
 * standbys. In 10.0 a quorum-based multiple synchronous standbys is also
 * supported. The number of synchronous standbys that transactions
 * must wait for replies from is specified in synchronous_standby_names.
 * This parameter also specifies a list of standby names and the method
 * (FIRST and ANY) to choose synchronous standbys from the listed ones.
 *
 * The method FIRST specifies a priority-based synchronous replication
 * and makes transaction commits wait until their WAL records are
 * replicated to the requested number of synchronous standbys chosen based
 * on their priorities. The standbys whose names appear earlier in the list
 * are given higher priority and will be considered as synchronous.
 * Other standby servers appearing later in this list represent potential
 * synchronous standbys. If any of the current synchronous standbys
 * disconnects for whatever reason, it will be replaced immediately with
 * the next-highest-priority standby.
 *
 * The method ANY specifies a quorum-based synchronous replication
 * and makes transaction commits wait until their WAL records are
 * replicated to at least the requested number of synchronous standbys
 * in the list. All the standbys appearing in the list are considered as
 * candidates for quorum synchronous standbys.
 *
 * If neither FIRST nor ANY is specified, FIRST is used as the method.
 * This is for backward compatibility with 9.6 or before where only a
 * priority-based sync replication was supported.
 *
 * Before the standbys chosen from synchronous_standby_names can
 * become the synchronous standbys they must have caught up with
 * the primary; that may take some time. Once caught up,
 * the standbys which are considered as synchronous at that moment
 * will release waiters from the queue.
 *
 * Portions Copyright (c) 2010-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "common/int.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/wait_event.h"

/* User-settable parameters for sync rep */
char	   *SyncRepStandbyNames;

#define SyncStandbysDefined() \
	(SyncRepStandbyNames != NULL && SyncRepStandbyNames[0] != '\0')

static bool announce_next_takeover = true;

SyncRepConfigData *SyncRepConfig = NULL;
static int	SyncRepWaitMode = SYNC_REP_NO_WAIT;

static struct
{
	Latch	  **arr;
	int			n;
}			SyncRepWakeList = {NULL, 0};

static void SyncRepQueueInsert(int mode);
static void SyncRepCancelWait(void);
static void SyncRepInitWakeList(void);
static void SyncRepWakeFromList(void);
static int	SyncRepWakeQueue(bool all, int mode);

static bool SyncRepGetSyncRecPtr(XLogRecPtr *writePtr,
								 XLogRecPtr *flushPtr,
								 XLogRecPtr *applyPtr,
								 bool *am_sync);
static void SyncRepGetOldestSyncRecPtr(XLogRecPtr *writePtr,
									   XLogRecPtr *flushPtr,
									   XLogRecPtr *applyPtr,
									   SyncRepStandbyData *sync_standbys,
									   int num_standbys);
static void SyncRepGetNthLatestSyncRecPtr(XLogRecPtr *writePtr,
										  XLogRecPtr *flushPtr,
										  XLogRecPtr *applyPtr,
										  SyncRepStandbyData *sync_standbys,
										  int num_standbys,
										  uint8 nth);
static int	SyncRepGetStandbyPriority(void);
static int	standby_priority_comparator(const void *a, const void *b);
static int	cmp_lsn(const void *a, const void *b);

#ifdef USE_ASSERT_CHECKING
static bool SyncRepQueueIsOrderedByLSN(int mode);
#endif

/*
 * Accessors for the position a queue's waiters have been released up to.
 *
 * Writers hold SyncRepLock and only ever move it forward, so a reader
 * without the lock can be behind, never ahead.
 */
static inline XLogRecPtr
SyncRepGetLSN(int mode)
{
	return pg_atomic_read_u64(&WalSndCtl->lsn[mode]);
}

static inline void
SyncRepSetLSN(int mode, XLogRecPtr lsn)
{
	Assert(LWLockHeldByMeInMode(SyncRepLock, LW_EXCLUSIVE));
	pg_atomic_write_u64(&WalSndCtl->lsn[mode], lsn);
}

/*
 * ===========================================================
 * Synchronous Replication functions for normal user backends
 * ===========================================================
 */

/*
 * Wait for synchronous replication, if requested by user.
 *
 * Initially backends start in state SYNC_REP_NOT_WAITING and then
 * change that state to SYNC_REP_WAITING before adding ourselves
 * to the wait queue. During SyncRepWakeQueue() a WALSender changes
 * the state to SYNC_REP_WAIT_COMPLETE once replication is confirmed.
 * This backend then resets its state to SYNC_REP_NOT_WAITING.
 *
 * 'lsn' represents the LSN to wait for.  'commit' indicates whether this LSN
 * represents a commit record.  If it doesn't, then we wait only for the WAL
 * to be flushed if synchronous_commit is set to the higher level of
 * remote_apply, because only commit records provide apply feedback.
 */
void
SyncRepWaitForLSN(XLogRecPtr lsn, bool commit)
{
	int			mode;

	/*
	 * This should be called while holding interrupts during a transaction
	 * commit to prevent the follow-up shared memory queue cleanups to be
	 * influenced by external interruptions.
	 */
	Assert(InterruptHoldoffCount > 0);

	/*
	 * Fast exit if user has not requested sync replication, or there are no
	 * sync replication standby names defined.
	 *
	 * Since this routine gets called every commit time, it's important to
	 * exit quickly if sync replication is not requested.
	 *
	 * We check WalSndCtl->sync_standbys_status flag without the lock and exit
	 * immediately if SYNC_STANDBY_INIT is set (the checkpointer has
	 * initialized this data) but SYNC_STANDBY_DEFINED is missing (no sync
	 * replication requested).
	 *
	 * If SYNC_STANDBY_DEFINED is set, we need to check the status again later
	 * while holding the lock, to check the flag and operate the sync rep
	 * queue atomically.  This is necessary to avoid the race condition
	 * described in SyncRepUpdateSyncStandbysDefined().  On the other hand, if
	 * SYNC_STANDBY_DEFINED is not set, the lock is not necessary because we
	 * don't touch the queue.
	 */
	if (!SyncRepRequested() ||
		((((volatile WalSndCtlData *) WalSndCtl)->sync_standbys_status) &
		 (SYNC_STANDBY_INIT | SYNC_STANDBY_DEFINED)) == SYNC_STANDBY_INIT)
		return;

	/* Cap the level for anything other than commit to remote flush only. */
	if (commit)
		mode = SyncRepWaitMode;
	else
		mode = Min(SyncRepWaitMode, SYNC_REP_WAIT_FLUSH);

	Assert(dlist_node_is_detached(&MyProc->syncRepLinks));
	Assert(WalSndCtl != NULL);
	Assert(MyProc->syncRepState == SYNC_REP_NOT_WAITING);

	/*
	 * A released position that already covers this LSN says a valid quorum
	 * acknowledged it, which is the same answer the check below the lock
	 * would give.  It only ever moves forward, so a stale read can only send
	 * us to take the lock for nothing, never past a wait we owe.  How often
	 * this exit fires depends on the wait mode: it needs the acknowledgement
	 * to have arrived before the committer got here.
	 */
	if (lsn <= SyncRepGetLSN(mode))
		return;

	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

	/*
	 * Read it again under the lock.  The standby may have replied in the
	 * window since the check above, and this is the read that decides:
	 * anything the queue is released up to needs no wait, whether or not the
	 * sync standby data has been initialized yet.
	 */
	if (lsn <= SyncRepGetLSN(mode))
	{
		LWLockRelease(SyncRepLock);
		return;
	}

	/*
	 * We don't wait for sync rep if SYNC_STANDBY_DEFINED is not set.  See
	 * SyncRepUpdateSyncStandbysDefined().
	 *
	 * If the sync standby data has not been initialized yet
	 * (SYNC_STANDBY_INIT is not set), fall back to a direct GUC check.
	 */
	if (WalSndCtl->sync_standbys_status & SYNC_STANDBY_INIT)
	{
		if ((WalSndCtl->sync_standbys_status & SYNC_STANDBY_DEFINED) == 0)
		{
			LWLockRelease(SyncRepLock);
			return;
		}
	}
	else if (!SyncStandbysDefined())
	{
		/*
		 * If we are here, the sync standby data has not been initialized yet,
		 * and the LSN is newer than what need to wait for, so we have fallen
		 * back to the best thing we could do in this case: a check on
		 * SyncStandbysDefined() to see if the GUC is set or not.
		 *
		 * When the GUC has a value, we wait until the checkpointer updates
		 * the status data because we cannot be sure yet if we should wait or
		 * not. Here, the GUC has *no* value, we are sure that there is no
		 * point to wait; this matters for example when initializing a
		 * cluster, where we should never wait, and no sync standbys is the
		 * default behavior.
		 */
		LWLockRelease(SyncRepLock);
		return;
	}

	/*
	 * Set our waitLSN so WALSender will know when to wake us, and add
	 * ourselves to the queue.
	 */
	MyProc->waitLSN = lsn;
	MyProc->syncRepState = SYNC_REP_WAITING;
	SyncRepQueueInsert(mode);
	Assert(SyncRepQueueIsOrderedByLSN(mode));
	LWLockRelease(SyncRepLock);

	/* Alter ps display to show waiting for sync rep. */
	if (update_process_title)
	{
		char		buffer[32];

		sprintf(buffer, "waiting for %X/%08X", LSN_FORMAT_ARGS(lsn));
		set_ps_display_suffix(buffer);
	}

	/*
	 * Wait for specified LSN to be confirmed.
	 *
	 * Each proc has its own wait latch, so we perform a normal latch
	 * check/wait loop here.
	 */
	for (;;)
	{
		int			rc;

		/* Must reset the latch before testing state. */
		ResetLatch(MyLatch);

		/*
		 * Acquiring the lock is not needed, the latch ensures proper
		 * barriers. If it looks like we're done, we must really be done,
		 * because once walsender changes the state to SYNC_REP_WAIT_COMPLETE,
		 * it will never update it again, so we can't be seeing a stale value
		 * in that case.
		 */
		if (MyProc->syncRepState == SYNC_REP_WAIT_COMPLETE)
			break;

		/*
		 * If a wait for synchronous replication is pending, we can neither
		 * acknowledge the commit nor raise ERROR or FATAL.  The latter would
		 * lead the client to believe that the transaction aborted, which is
		 * not true: it's already committed locally. The former is no good
		 * either: the client has requested synchronous replication, and is
		 * entitled to assume that an acknowledged commit is also replicated,
		 * which might not be true. So in this case we issue a WARNING (which
		 * some clients may be able to interpret) and shut off further output.
		 * We do NOT reset ProcDiePending, so that the process will die after
		 * the commit is cleaned up.
		 */
		if (ProcDiePending)
		{
			if (ProcDieSenderPid != 0)
				ereport(WARNING,
						(errcode(ERRCODE_ADMIN_SHUTDOWN),
						 errmsg("canceling the wait for synchronous replication and terminating connection due to administrator command"),
						 errdetail("The transaction has already committed locally, but might not have been replicated to the standby."),
						 errdetail_log("The transaction has already committed locally, but might not have been replicated to the standby.  Signal sent by PID %d, UID %d.",
									   (int) ProcDieSenderPid,
									   (int) ProcDieSenderUid)));
			else
				ereport(WARNING,
						(errcode(ERRCODE_ADMIN_SHUTDOWN),
						 errmsg("canceling the wait for synchronous replication and terminating connection due to administrator command"),
						 errdetail("The transaction has already committed locally, but might not have been replicated to the standby.")));
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}

		/*
		 * It's unclear what to do if a query cancel interrupt arrives.  We
		 * can't actually abort at this point, but ignoring the interrupt
		 * altogether is not helpful, so we just terminate the wait with a
		 * suitable warning.
		 */
		if (QueryCancelPending)
		{
			QueryCancelPending = false;
			ereport(WARNING,
					(errmsg("canceling wait for synchronous replication due to user request"),
					 errdetail("The transaction has already committed locally, but might not have been replicated to the standby.")));
			SyncRepCancelWait();
			break;
		}

		/*
		 * Wait on latch.  Any condition that should wake us up will set the
		 * latch, so no need for timeout.
		 */
		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1,
					   WAIT_EVENT_SYNC_REP);

		/*
		 * If the postmaster dies, we'll probably never get an acknowledgment,
		 * because all the wal sender processes will exit. So just bail out.
		 */
		if (rc & WL_POSTMASTER_DEATH)
		{
			ProcDiePending = true;
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}
	}

	/*
	 * WalSender has checked our LSN and has removed us from queue. Clean up
	 * state and leave.  It's OK to reset these shared memory fields without
	 * holding SyncRepLock, because any walsenders will ignore us anyway when
	 * we're not on the queue.  We need a read barrier to make sure we see the
	 * changes to the queue link (this might be unnecessary without
	 * assertions, but better safe than sorry).
	 */
	pg_read_barrier();
	Assert(dlist_node_is_detached(&MyProc->syncRepLinks));
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	MyProc->waitLSN = InvalidXLogRecPtr;

	/* reset ps display to remove the suffix */
	if (update_process_title)
		set_ps_display_remove_suffix();
}

/*
 * Insert MyProc into the specified SyncRepQueue, maintaining sorted invariant.
 *
 * Usually we will go at tail of queue, though it's possible that we arrive
 * here out of order, so start at tail and work back to insertion point.
 */
static void
SyncRepQueueInsert(int mode)
{
	dlist_head *queue;
	dlist_iter	iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	queue = &WalSndCtl->SyncRepQueue[mode];

	dlist_reverse_foreach(iter, queue)
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * Stop at the queue element that we should insert after to ensure the
		 * queue is ordered by LSN.
		 */
		if (proc->waitLSN < MyProc->waitLSN)
		{
			dlist_insert_after(&proc->syncRepLinks, &MyProc->syncRepLinks);
			return;
		}
	}

	/*
	 * If we get here, the list was either empty, or this process needs to be
	 * at the head.
	 */
	dlist_push_head(queue, &MyProc->syncRepLinks);
}

/*
 * Acquire SyncRepLock and cancel any wait currently in progress.
 */
static void
SyncRepCancelWait(void)
{
	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
	if (!dlist_node_is_detached(&MyProc->syncRepLinks))
		dlist_delete_thoroughly(&MyProc->syncRepLinks);
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	LWLockRelease(SyncRepLock);
}

void
SyncRepCleanupAtProcExit(void)
{
	/*
	 * First check if we are removed from the queue without the lock to not
	 * slow down backend exit.
	 */
	if (!dlist_node_is_detached(&MyProc->syncRepLinks))
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/* maybe we have just been removed, so recheck */
		if (!dlist_node_is_detached(&MyProc->syncRepLinks))
			dlist_delete_thoroughly(&MyProc->syncRepLinks);

		LWLockRelease(SyncRepLock);
	}
}

/*
 * ===========================================================
 * Synchronous Replication functions for wal sender processes
 * ===========================================================
 */

/*
 * Take any action required to initialise sync rep state from config
 * data. Called at WALSender startup and after each SIGHUP.
 */
void
SyncRepInitConfig(void)
{
	int			priority;

	/*
	 * Determine if we are a potential sync standby and remember the result
	 * for handling replies from standby.
	 */
	priority = SyncRepGetStandbyPriority();
	if (MyWalSnd->sync_standby_priority != priority)
	{
		SpinLockAcquire(&MyWalSnd->mutex);
		MyWalSnd->sync_standby_priority = priority;
		SpinLockRelease(&MyWalSnd->mutex);

		ereport(DEBUG1,
				(errmsg_internal("standby \"%s\" now has synchronous standby priority %d",
								 application_name, priority)));
	}
}

/*
 * Update the LSNs on each queue based upon our latest state. This
 * implements a simple policy of first-valid-sync-standby-releases-waiter.
 *
 * Other policies are possible, which would change what we do here and
 * perhaps also which information we store as well.
 */
void
SyncRepReleaseWaiters(void)
{
	XLogRecPtr	writePtr;
	XLogRecPtr	flushPtr;
	XLogRecPtr	applyPtr;
	bool		got_recptr;
	bool		am_sync;
	int			numwrite = 0;
	int			numflush = 0;
	int			numapply = 0;

	/*
	 * If this WALSender is serving a standby that is not on the list of
	 * potential sync standbys then we have nothing to do. If we are still
	 * starting up, still running base backup or the current flush position is
	 * still invalid, then leave quickly also.  Streaming or stopping WAL
	 * senders are allowed to release waiters.
	 */
	if (MyWalSnd->sync_standby_priority == 0 ||
		(MyWalSnd->state != WALSNDSTATE_STREAMING &&
		 MyWalSnd->state != WALSNDSTATE_STOPPING) ||
		!XLogRecPtrIsValid(MyWalSnd->flush))
	{
		announce_next_takeover = true;
		return;
	}

	/*
	 * We're a potential sync standby.  Check whether we are a sync standby
	 * and calculate the synced positions among all sync standbys before
	 * taking the lock: the walk over the walsender slots takes their
	 * spinlocks, allocates, and possibly sorts, and doing all of it under
	 * SyncRepLock delays every committer.
	 *
	 * Positions gone stale by the time the lock is held cost nothing.  The
	 * guards further down only ever move lsn[] forward, so a reading older
	 * than a concurrent walsender's simply releases nobody.
	 */
	got_recptr = SyncRepGetSyncRecPtr(&writePtr, &flushPtr, &applyPtr, &am_sync);

	/*
	 * If we are managing a sync standby, though we weren't prior to this,
	 * then announce we are now a sync standby.
	 */
	if (announce_next_takeover && am_sync)
	{
		announce_next_takeover = false;

		if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY)
			ereport(LOG,
					(errmsg("standby \"%s\" is now a synchronous standby with priority %d",
							application_name, MyWalSnd->sync_standby_priority)));
		else
			ereport(LOG,
					(errmsg("standby \"%s\" is now a candidate for quorum synchronous standby",
							application_name)));
	}

	/*
	 * If the number of sync standbys is less than requested or we aren't
	 * managing a sync standby then just leave.
	 */
	if (!got_recptr || !am_sync)
	{
		announce_next_takeover = !am_sync;
		return;
	}

	SyncRepInitWakeList();

	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

	/*
	 * Set the lsn first so that when we wake backends they will release up to
	 * this location.
	 */
	if (SyncRepGetLSN(SYNC_REP_WAIT_WRITE) < writePtr)
	{
		SyncRepSetLSN(SYNC_REP_WAIT_WRITE, writePtr);
		numwrite = SyncRepWakeQueue(false, SYNC_REP_WAIT_WRITE);
	}
	if (SyncRepGetLSN(SYNC_REP_WAIT_FLUSH) < flushPtr)
	{
		SyncRepSetLSN(SYNC_REP_WAIT_FLUSH, flushPtr);
		numflush = SyncRepWakeQueue(false, SYNC_REP_WAIT_FLUSH);
	}
	if (SyncRepGetLSN(SYNC_REP_WAIT_APPLY) < applyPtr)
	{
		SyncRepSetLSN(SYNC_REP_WAIT_APPLY, applyPtr);
		numapply = SyncRepWakeQueue(false, SYNC_REP_WAIT_APPLY);
	}

	LWLockRelease(SyncRepLock);

	/* wake the released backends now that the lock is down */
	SyncRepWakeFromList();

	elog(DEBUG3, "released %d procs up to write %X/%08X, %d procs up to flush %X/%08X, %d procs up to apply %X/%08X",
		 numwrite, LSN_FORMAT_ARGS(writePtr),
		 numflush, LSN_FORMAT_ARGS(flushPtr),
		 numapply, LSN_FORMAT_ARGS(applyPtr));
}

/*
 * Calculate the synced Write, Flush and Apply positions among sync standbys.
 *
 * Return false if the number of sync standbys is less than
 * synchronous_standby_names specifies. Otherwise return true and
 * store the positions into *writePtr, *flushPtr and *applyPtr.
 *
 * On return, *am_sync is set to true if this walsender is connecting to
 * sync standby. Otherwise it's set to false.
 */
static bool
SyncRepGetSyncRecPtr(XLogRecPtr *writePtr, XLogRecPtr *flushPtr,
					 XLogRecPtr *applyPtr, bool *am_sync)
{
	SyncRepStandbyData *sync_standbys;
	int			num_standbys;
	int			i;

	/* Initialize default results */
	*writePtr = InvalidXLogRecPtr;
	*flushPtr = InvalidXLogRecPtr;
	*applyPtr = InvalidXLogRecPtr;
	*am_sync = false;

	/* Quick out if not even configured to be synchronous */
	if (SyncRepConfig == NULL)
		return false;

	/* Get standbys that are considered as synchronous at this moment */
	num_standbys = SyncRepGetCandidateStandbys(&sync_standbys);

	/* Am I among the candidate sync standbys? */
	for (i = 0; i < num_standbys; i++)
	{
		if (sync_standbys[i].is_me)
		{
			*am_sync = true;
			break;
		}
	}

	/*
	 * Nothing more to do if we are not managing a sync standby or there are
	 * not enough synchronous standbys.
	 */
	if (!(*am_sync) ||
		num_standbys < SyncRepConfig->num_sync)
	{
		pfree(sync_standbys);
		return false;
	}

	/*
	 * In a priority-based sync replication, the synced positions are the
	 * oldest ones among sync standbys. In a quorum-based, they are the Nth
	 * latest ones.
	 *
	 * SyncRepGetNthLatestSyncRecPtr() also can calculate the oldest
	 * positions. But we use SyncRepGetOldestSyncRecPtr() for that calculation
	 * because it's a bit more efficient.
	 *
	 * XXX If the numbers of current and requested sync standbys are the same,
	 * we can use SyncRepGetOldestSyncRecPtr() to calculate the synced
	 * positions even in a quorum-based sync replication.
	 */
	if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY)
	{
		SyncRepGetOldestSyncRecPtr(writePtr, flushPtr, applyPtr,
								   sync_standbys, num_standbys);
	}
	else
	{
		SyncRepGetNthLatestSyncRecPtr(writePtr, flushPtr, applyPtr,
									  sync_standbys, num_standbys,
									  SyncRepConfig->num_sync);
	}

	pfree(sync_standbys);
	return true;
}

/*
 * Calculate the oldest Write, Flush and Apply positions among sync standbys.
 */
static void
SyncRepGetOldestSyncRecPtr(XLogRecPtr *writePtr,
						   XLogRecPtr *flushPtr,
						   XLogRecPtr *applyPtr,
						   SyncRepStandbyData *sync_standbys,
						   int num_standbys)
{
	int			i;

	/*
	 * Scan through all sync standbys and calculate the oldest Write, Flush
	 * and Apply positions.  We assume *writePtr et al were initialized to
	 * InvalidXLogRecPtr.
	 */
	for (i = 0; i < num_standbys; i++)
	{
		XLogRecPtr	write = sync_standbys[i].write;
		XLogRecPtr	flush = sync_standbys[i].flush;
		XLogRecPtr	apply = sync_standbys[i].apply;

		if (!XLogRecPtrIsValid(*writePtr) || *writePtr > write)
			*writePtr = write;
		if (!XLogRecPtrIsValid(*flushPtr) || *flushPtr > flush)
			*flushPtr = flush;
		if (!XLogRecPtrIsValid(*applyPtr) || *applyPtr > apply)
			*applyPtr = apply;
	}
}

/*
 * Calculate the Nth latest Write, Flush and Apply positions among sync
 * standbys.
 */
static void
SyncRepGetNthLatestSyncRecPtr(XLogRecPtr *writePtr,
							  XLogRecPtr *flushPtr,
							  XLogRecPtr *applyPtr,
							  SyncRepStandbyData *sync_standbys,
							  int num_standbys,
							  uint8 nth)
{
	XLogRecPtr *write_array;
	XLogRecPtr *flush_array;
	XLogRecPtr *apply_array;
	int			i;

	/* Should have enough candidates, or somebody messed up */
	Assert(nth > 0 && nth <= num_standbys);

	write_array = palloc_array(XLogRecPtr, num_standbys);
	flush_array = palloc_array(XLogRecPtr, num_standbys);
	apply_array = palloc_array(XLogRecPtr, num_standbys);

	for (i = 0; i < num_standbys; i++)
	{
		write_array[i] = sync_standbys[i].write;
		flush_array[i] = sync_standbys[i].flush;
		apply_array[i] = sync_standbys[i].apply;
	}

	/* Sort each array in descending order */
	qsort(write_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);
	qsort(flush_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);
	qsort(apply_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);

	/* Get Nth latest Write, Flush, Apply positions */
	*writePtr = write_array[nth - 1];
	*flushPtr = flush_array[nth - 1];
	*applyPtr = apply_array[nth - 1];

	pfree(write_array);
	pfree(flush_array);
	pfree(apply_array);
}

/*
 * Compare lsn in order to sort array in descending order.
 */
static int
cmp_lsn(const void *a, const void *b)
{
	XLogRecPtr	lsn1 = *((const XLogRecPtr *) a);
	XLogRecPtr	lsn2 = *((const XLogRecPtr *) b);

	return pg_cmp_u64(lsn2, lsn1);
}

/*
 * Return data about walsenders that are candidates to be sync standbys.
 *
 * *standbys is set to a palloc'd array of structs of per-walsender data,
 * and the number of valid entries (candidate sync senders) is returned.
 * (This might be more or fewer than num_sync; caller must check.)
 */
int
SyncRepGetCandidateStandbys(SyncRepStandbyData **standbys)
{
	int			i;
	int			n;

	/* Create result array */
	*standbys = palloc_array(SyncRepStandbyData, max_wal_senders);

	/* Quick exit if sync replication is not requested */
	if (SyncRepConfig == NULL)
		return 0;

	/* Collect raw data from shared memory */
	n = 0;
	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd;
		SyncRepStandbyData *stby;
		WalSndState state;		/* not included in SyncRepStandbyData */

		walsnd = &WalSndCtl->walsnds[i];
		stby = *standbys + n;

		SpinLockAcquire(&walsnd->mutex);
		stby->pid = walsnd->pid;
		state = walsnd->state;
		stby->write = walsnd->write;
		stby->flush = walsnd->flush;
		stby->apply = walsnd->apply;
		stby->sync_standby_priority = walsnd->sync_standby_priority;
		SpinLockRelease(&walsnd->mutex);

		/* Must be active */
		if (stby->pid == 0)
			continue;

		/* Must be streaming or stopping */
		if (state != WALSNDSTATE_STREAMING &&
			state != WALSNDSTATE_STOPPING)
			continue;

		/* Must be synchronous */
		if (stby->sync_standby_priority == 0)
			continue;

		/* Must have a valid flush position */
		if (!XLogRecPtrIsValid(stby->flush))
			continue;

		/* OK, it's a candidate */
		stby->walsnd_index = i;
		stby->is_me = (walsnd == MyWalSnd);
		n++;
	}

	/*
	 * In quorum mode, we return all the candidates.  In priority mode, if we
	 * have too many candidates then return only the num_sync ones of highest
	 * priority.
	 */
	if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY &&
		n > SyncRepConfig->num_sync)
	{
		/* Sort by priority ... */
		qsort(*standbys, n, sizeof(SyncRepStandbyData),
			  standby_priority_comparator);
		/* ... then report just the first num_sync ones */
		n = SyncRepConfig->num_sync;
	}

	return n;
}

/*
 * qsort comparator to sort SyncRepStandbyData entries by priority
 */
static int
standby_priority_comparator(const void *a, const void *b)
{
	const SyncRepStandbyData *sa = (const SyncRepStandbyData *) a;
	const SyncRepStandbyData *sb = (const SyncRepStandbyData *) b;

	/* First, sort by increasing priority value */
	if (sa->sync_standby_priority != sb->sync_standby_priority)
		return sa->sync_standby_priority - sb->sync_standby_priority;

	/*
	 * We might have equal priority values; arbitrarily break ties by position
	 * in the WalSnd array.  (This is utterly bogus, since that is arrival
	 * order dependent, but there are regression tests that rely on it.)
	 */
	return sa->walsnd_index - sb->walsnd_index;
}


/*
 * Check if we are in the list of sync standbys, and if so, determine
 * priority sequence. Return priority if set, or zero to indicate that
 * we are not a potential sync standby.
 *
 * Compare the parameter SyncRepStandbyNames against the application_name
 * for this WALSender, or allow any name if we find a wildcard "*".
 */
static int
SyncRepGetStandbyPriority(void)
{
	const char *standby_name;
	int			priority;
	bool		found = false;

	/*
	 * Since synchronous cascade replication is not allowed, we always set the
	 * priority of cascading walsender to zero.
	 */
	if (am_cascading_walsender)
		return 0;

	if (!SyncStandbysDefined() || SyncRepConfig == NULL)
		return 0;

	standby_name = SyncRepConfig->member_names;
	for (priority = 1; priority <= SyncRepConfig->nmembers; priority++)
	{
		if (pg_strcasecmp(standby_name, application_name) == 0 ||
			strcmp(standby_name, "*") == 0)
		{
			found = true;
			break;
		}
		standby_name += strlen(standby_name) + 1;
	}

	if (!found)
		return 0;

	/*
	 * In quorum-based sync replication, all the standbys in the list have the
	 * same priority, one.
	 */
	return (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY) ? priority : 1;
}

/*
 * Make the list a release collects the latches to set into ready for use,
 * allocating it the first time this process releases anybody.  It is sized
 * for every backend to be waiting at once; a proc waits in at most one queue,
 * so one list of that size is enough for a pass over all three.
 *
 * Every pass empties the list again, so it starts out empty here.
 */
static void
SyncRepInitWakeList(void)
{
	Assert(SyncRepWakeList.n == 0);

	if (SyncRepWakeList.arr == NULL)
		SyncRepWakeList.arr = (Latch **)
			MemoryContextAlloc(TopMemoryContext,
							   MaxBackends * sizeof(Latch *));
}

/*
 * Set the latches the pass under the lock collected, and empty the list.
 *
 * Each latch is a kill() for a sleeping proc, and running one per released
 * commit inside the exclusive section makes every committer wait for those
 * syscalls.  The procs here are off the queue with their state already
 * complete, so nothing below needs the lock's protection.  Nothing here can
 * error out either: only this process dying outright could leave a released
 * proc completed but unlatched, and the in-lock SetLatch had that same
 * window.  A proc that noticed its state on its own and moved on, even into
 * a new wait, gets a spurious latch set, which every latch sleeper tolerates.
 */
static void
SyncRepWakeFromList(void)
{
	for (int i = 0; i < SyncRepWakeList.n; i++)
		SetLatch(SyncRepWakeList.arr[i]);

	SyncRepWakeList.n = 0;
}

/*
 * Walk the specified queue from head.  Set the state of any backends that
 * need to be woken and remove them from the queue; their latches are
 * appended to the wake list for SyncRepWakeFromList to set once the lock is
 * down.  Pass all = true to release the whole queue; otherwise, just release
 * up to the walsender's LSN.
 *
 * The caller must hold SyncRepLock in exclusive mode, and must set the
 * latches with SyncRepWakeFromList after releasing it.  Unlink, barrier and
 * state stay together in here: a waiter reads syncRepState without the lock
 * and must never find itself completed while still on the queue.
 */
static int
SyncRepWakeQueue(bool all, int mode)
{
	int			numprocs = 0;
	dlist_mutable_iter iter;
	XLogRecPtr	lsn = SyncRepGetLSN(mode);

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	Assert(LWLockHeldByMeInMode(SyncRepLock, LW_EXCLUSIVE));
	Assert(SyncRepQueueIsOrderedByLSN(mode));

	dlist_foreach_modify(iter, &WalSndCtl->SyncRepQueue[mode])
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * Assume the queue is ordered by LSN
		 */
		if (!all && lsn < proc->waitLSN)
			return numprocs;

		/*
		 * Remove from queue.
		 */
		dlist_delete_thoroughly(&proc->syncRepLinks);

		/*
		 * SyncRepWaitForLSN() reads syncRepState without holding the lock, so
		 * make sure that it sees the queue link being removed before the
		 * syncRepState change.
		 */
		pg_write_barrier();

		/*
		 * Set state to complete; see SyncRepWaitForLSN() for discussion of
		 * the various states.
		 */
		proc->syncRepState = SYNC_REP_WAIT_COMPLETE;

		/* the list is sized to every process that can ever queue here */
		Assert(SyncRepWakeList.n < MaxBackends);
		SyncRepWakeList.arr[SyncRepWakeList.n++] = &proc->procLatch;

		numprocs++;
	}

	return numprocs;
}

/*
 * The checkpointer calls this as needed to update the shared
 * sync_standbys_status flag, so that backends don't remain permanently wedged
 * if synchronous_standby_names is unset.  It's safe to check the current value
 * without the lock, because it's only ever updated by one process.  But we
 * must take the lock to change it.
 */
void
SyncRepUpdateSyncStandbysDefined(void)
{
	bool		sync_standbys_defined = SyncStandbysDefined();

	if (sync_standbys_defined !=
		((WalSndCtl->sync_standbys_status & SYNC_STANDBY_DEFINED) != 0))
	{
		SyncRepInitWakeList();

		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/*
		 * If synchronous_standby_names has been reset to empty, it's futile
		 * for backends to continue waiting.  Since the user no longer wants
		 * synchronous replication, we'd better wake them up.
		 */
		if (!sync_standbys_defined)
		{
			int			i;

			for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
				SyncRepWakeQueue(true, i);
		}

		/*
		 * Only allow people to join the queue when there are synchronous
		 * standbys defined.  Without this interlock, there's a race
		 * condition: we might wake up all the current waiters; then, some
		 * backend that hasn't yet reloaded its config might go to sleep on
		 * the queue (and never wake up).  This prevents that.
		 */
		WalSndCtl->sync_standbys_status = SYNC_STANDBY_INIT |
			(sync_standbys_defined ? SYNC_STANDBY_DEFINED : 0);

		LWLockRelease(SyncRepLock);

		/* wake the released backends now that the lock is down */
		SyncRepWakeFromList();
	}
	else if ((WalSndCtl->sync_standbys_status & SYNC_STANDBY_INIT) == 0)
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/*
		 * Note that there is no need to wake up the queues here.  We would
		 * reach this path only if SyncStandbysDefined() returns false, or it
		 * would mean that some backends are waiting with the GUC set.  See
		 * SyncRepWaitForLSN().
		 */
		Assert(!SyncStandbysDefined());

		/*
		 * Even if there is no sync standby defined, let the readers of this
		 * information know that the sync standby data has been initialized.
		 * This can just be done once, hence the previous check on
		 * SYNC_STANDBY_INIT to avoid useless work.
		 */
		WalSndCtl->sync_standbys_status |= SYNC_STANDBY_INIT;

		LWLockRelease(SyncRepLock);
	}
}

#ifdef USE_ASSERT_CHECKING
static bool
SyncRepQueueIsOrderedByLSN(int mode)
{
	XLogRecPtr	lastLSN;
	dlist_iter	iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);

	lastLSN = InvalidXLogRecPtr;

	dlist_foreach(iter, &WalSndCtl->SyncRepQueue[mode])
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * Check the queue is ordered by LSN and that multiple procs don't
		 * have matching LSNs
		 */
		if (proc->waitLSN <= lastLSN)
			return false;

		lastLSN = proc->waitLSN;
	}

	return true;
}
#endif

/*
 * ===========================================================
 * Synchronous Replication functions executed by any process
 * ===========================================================
 */

bool
check_synchronous_standby_names(char **newval, void **extra, GucSource source)
{
	if (*newval != NULL && (*newval)[0] != '\0')
	{
		yyscan_t	scanner;
		int			parse_rc;
		SyncRepConfigData *pconf;

		/* Result of parsing is returned in one of these two variables */
		SyncRepConfigData *syncrep_parse_result = NULL;
		char	   *syncrep_parse_error_msg = NULL;

		/* Parse the synchronous_standby_names string */
		syncrep_scanner_init(*newval, &scanner);
		parse_rc = syncrep_yyparse(&syncrep_parse_result, &syncrep_parse_error_msg, scanner);
		syncrep_scanner_finish(scanner);

		if (parse_rc != 0 || syncrep_parse_result == NULL)
		{
			GUC_check_errcode(ERRCODE_SYNTAX_ERROR);
			if (syncrep_parse_error_msg)
				GUC_check_errdetail("%s", syncrep_parse_error_msg);
			else
				/* translator: %s is a GUC name */
				GUC_check_errdetail("\"%s\" parser failed.",
									"synchronous_standby_names");
			return false;
		}

		if (syncrep_parse_result->num_sync <= 0)
		{
			GUC_check_errmsg("number of synchronous standbys (%d) must be greater than zero",
							 syncrep_parse_result->num_sync);
			return false;
		}

		/* GUC extra value must be guc_malloc'd, not palloc'd */
		pconf = (SyncRepConfigData *)
			guc_malloc(LOG, syncrep_parse_result->config_size);
		if (pconf == NULL)
			return false;
		memcpy(pconf, syncrep_parse_result, syncrep_parse_result->config_size);

		*extra = pconf;

		/*
		 * We need not explicitly clean up syncrep_parse_result.  It, and any
		 * other cruft generated during parsing, will be freed when the
		 * current memory context is deleted.  (This code is generally run in
		 * a short-lived context used for config file processing, so that will
		 * not be very long.)
		 */
	}
	else
		*extra = NULL;

	return true;
}

void
assign_synchronous_standby_names(const char *newval, void *extra)
{
	SyncRepConfig = (SyncRepConfigData *) extra;
}

void
assign_synchronous_commit(int newval, void *extra)
{
	switch (newval)
	{
		case SYNCHRONOUS_COMMIT_REMOTE_WRITE:
			SyncRepWaitMode = SYNC_REP_WAIT_WRITE;
			break;
		case SYNCHRONOUS_COMMIT_REMOTE_FLUSH:
			SyncRepWaitMode = SYNC_REP_WAIT_FLUSH;
			break;
		case SYNCHRONOUS_COMMIT_REMOTE_APPLY:
			SyncRepWaitMode = SYNC_REP_WAIT_APPLY;
			break;
		default:
			SyncRepWaitMode = SYNC_REP_NO_WAIT;
			break;
	}
}
