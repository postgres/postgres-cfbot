/*-------------------------------------------------------------------------
 *
 * xactundo.c
 *	  Management of undo record sets for transactions
 *
 * Undo records that need to be applied after a transaction or
 * subtransaction abort should be inserted using the functions defined
 * in this file; thus, every table or index access method that wants to
 * use undo for post-abort cleanup should invoke these interfaces.
 *
 * The reason for this design is that we want to pack all of the undo
 * records for a single transaction into one place, regardless of the
 * AM which generated them. That way, we can apply the undo actions
 * which pertain to that transaction in the correct order; namely,
 * backwards as compared with the order in which the records were
 * generated.
 *
 * We may use up to three undo record sets per transaction, one per
 * persistence level (permanent, unlogged, temporary). We assume that
 * it's OK to apply the undo records for each persistence level
 * independently of the others. This is safe since the modifications
 * must necessarily touch disjoint sets of pages.
 *
 * CROSS-RELATION ORDERING INVARIANT (important for TOAST correctness):
 *
 * All UNDO records for all relations touched by a single transaction are
 * packed into the same UndoRecordSet, in strict WAL-LSN emission order.
 * The newest-first application order during rollback guarantees correct
 * restoration ordering for multi-object operations (e.g. a cluster-wide
 * consumer that touches several relations in one transaction).
 *
 * PARALLEL RECOVERY: XLOG_UNDO_BATCH records are handled by the startup
 * process via ApplyUndoChainFromWAL(); they are not dispatched to parallel
 * workers because UNDO application requires coordinated per-transaction
 * state.
 *
 * SUBTRANSACTION TRACKING:
 *
 * Subtransaction state is tracked using a dynamically-grown array allocated
 * in TopMemoryContext.  The array starts at INITIAL_SUBXACT_CAPACITY (64)
 * slots and doubles when needed via repalloc().  The array persists across
 * transactions within the same backend to avoid repeated allocation for
 * steady-state workloads.
 *
 * Growth via repalloc() in TopMemoryContext during SubXactCallbacks is safe
 * because it does not interact with pgstat's per-subtransaction tracking
 * (the original corruption bug was caused by palloc of new per-subtransaction
 * nodes in CurTransactionContext, not by growing an existing TopMemoryContext
 * allocation).
 *
 * This design follows the EDB undo-record-set branch architecture
 * (xactundo.c) adapted for the physical undo approach used here.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/xactundo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/atm.h"
#include "access/undo.h"
#include "access/undo_xlog.h"
#include "access/xlog.h"
#include "access/undolog.h"
#include "access/undormgr.h"
#include "access/undorecord.h"
#include "access/xact.h"
#include "access/xactundo.h"
#include "access/xlogdefs.h"
#include "access/xloginsert.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "storage/aio_subsys.h"
#include "storage/condition_variable.h"
#include "storage/lwlock.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "utils/injection_point.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* GUC: UNDO bytes threshold for instant abort via ATM */
int			undo_instant_abort_threshold = 65536;

/*
 * Initial capacity for the dynamically-grown subtransaction stack.
 * Covers 99.9% of workloads without needing reallocation.  The stack
 * doubles when needed, so there is no artificial upper limit.
 */
#define INITIAL_SUBXACT_CAPACITY	64

/* Per-subtransaction backend-private undo state (array element). */
typedef struct XactUndoSubTransactionState
{
	SubTransactionId nestingLevel;
	UndoRecPtr	start_location[NUndoPersistenceLevels];

	/*
	 * Snapshot of the parent's last_batch_lsn at the time this subtransaction
	 * started.  On subtransaction abort, after applying this subtransaction's
	 * UNDO chain, we restore XactUndo.last_batch_lsn to these saved values so
	 * the parent's subsequent abort (or further subtransactions) only covers
	 * the parent's own records and does not double-apply already-reversed
	 * batches.
	 */
	XLogRecPtr	last_batch_lsn[NUndoPersistenceLevels];

	/* Parent's chain_bytes[] at subtransaction start, restored likewise. */
	Size		chain_bytes[NUndoPersistenceLevels];
} XactUndoSubTransactionState;

/* Backend-private undo state. */
typedef struct XactUndoData
{
	bool		has_undo;		/* has this xact generated any undo? */

	/*
	 * Has this xact written UNDO that only its own abort may apply?  Index
	 * UNDO (nbtree, hash) just marks aborted entries dead, so it can be left
	 * to the logical revert worker.  Anything else -- FILEOPS restoring files
	 * -- must be undone by the time ROLLBACK returns, whatever its size; see
	 * undo_instant_abort_threshold.
	 */
	bool		needs_inline_abort;
	int			subxact_depth;	/* 0 = top-level, 1+ = savepoints */
	int			subxact_capacity;	/* allocated slots in subxact_stack */

	/* Dynamically-grown subtransaction stack (TopMemoryContext). */
	XactUndoSubTransactionState *subxact_stack;

	/*
	 * Per-persistence-level record sets. These are created lazily on first
	 * use and destroyed at transaction end.
	 */
	UndoRecordSet *record_set[NUndoPersistenceLevels];

	/* Tracking for the most recent undo insertion per persistence level. */
	UndoRecPtr	last_location[NUndoPersistenceLevels];

	/*
	 * WAL-based UNDO chain heads.  When UNDO records are routed through WAL
	 * via XLOG_UNDO_BATCH, this tracks the LSN of the most recent batch per
	 * persistence level.  Used for rollback chain walking.
	 */
	XLogRecPtr	last_batch_lsn[NUndoPersistenceLevels];

	/*
	 * Bytes of UNDO written to WAL per persistence level, i.e. the size of
	 * the chain that last_batch_lsn[] heads.  The record sets themselves
	 * cannot tell us this: every flush resets their buffer.
	 * AtAbort_XactUndo() compares this against undo_instant_abort_threshold.
	 */
	Size		chain_bytes[NUndoPersistenceLevels];

	/*
	 * The first (oldest) batch of each chain.  A rollback handed to the ATM
	 * or the 2PC state must keep WAL from here on, not only from the head:
	 * the revert worker reads every batch back to this one.
	 */
	XLogRecPtr	first_batch_lsn[NUndoPersistenceLevels];

	/*
	 * Deferred-batch state (see DeferXactUndoData).  When an index AM defers
	 * a record instead of writing it, the record sits in this UndoRecordSet's
	 * buffer until a choke point flushes it.  NULL means nothing is pending,
	 * which is the state every path outside a single statement must see.
	 */
	UndoRecordSet *pending_uset;
	UndoPersistenceLevel pending_plevel;
} XactUndoData;

static XactUndoData XactUndo;
static bool subxact_callback_registered = false;

/*
 * Per-backend copy of the current transaction's UNDO chains, for the
 * checkpointer (see LogXactUndoInflight).  Only the owning backend writes its
 * slot, under the slot's spinlock; xid is invalid when it has none.
 */
typedef struct XactUndoInflightSlot
{
	slock_t		mutex;
	xl_undo_inflight_xact chain;
} XactUndoInflightSlot;

static XactUndoInflightSlot *XactUndoInflight = NULL;

static void PublishXactUndoChain(TransactionId xid);

/*
 * Compile-time guard: xl_xact_prepare.last_batch_lsn[3] must match
 * NUndoPersistenceLevels.  Both headers are available here; xact.h avoids
 * including undodefs.h to keep its include footprint minimal.
 */
StaticAssertDecl(NUndoPersistenceLevels == 3,
				 "xl_xact_prepare.last_batch_lsn array size (3) must match NUndoPersistenceLevels");

static void ResetXactUndo(void);
static void CollapseXactUndoSubTransactions(void);
static UndoPersistenceLevel GetUndoPersistenceLevel(char relpersistence);
static void EnsureSubxactStackCapacity(void);
static void XactUndo_SubXactCallback(SubXactEvent event, SubTransactionId mySubid,
									 SubTransactionId parentSubid, void *arg);

/* Convenience macro: pointer to current subtransaction state. */
#define CURRENT_SUBXACT() (&XactUndo.subxact_stack[XactUndo.subxact_depth])

/*
 * XactUndoShmemSize
 *		How much shared memory do we need for transaction undo state?
 *
 * One XactUndoInflightSlot per backend.
 */
Size
XactUndoShmemSize(void)
{
	return mul_size(MaxBackends, sizeof(XactUndoInflightSlot));
}

/*
 * XactUndoShmemInit
 *		Initialize shared memory for transaction undo state.
 */
void
XactUndoShmemInit(void)
{
	bool		found;

	XactUndoInflight = (XactUndoInflightSlot *)
		ShmemInitStruct("UNDO In-flight Chains", XactUndoShmemSize(), &found);
	if (!found)
	{
		for (int i = 0; i < MaxBackends; i++)
		{
			SpinLockInit(&XactUndoInflight[i].mutex);
			XactUndoInflight[i].chain.xid = InvalidTransactionId;
		}
	}
}

/*
 * PublishXactUndoChain
 *		Make this backend's chain heads visible to LogXactUndoInflight().
 *
 * xid invalid clears the slot.  Called in critical sections: no palloc.
 */
static void
PublishXactUndoChain(TransactionId xid)
{
	XactUndoInflightSlot *slot;

	if (XactUndoInflight == NULL || MyProcNumber < 0 ||
		MyProcNumber >= MaxBackends)
		return;
	slot = &XactUndoInflight[MyProcNumber];
	if (!TransactionIdIsValid(xid) && !TransactionIdIsValid(slot->chain.xid))
		return;

	SpinLockAcquire(&slot->mutex);
	slot->chain.xid = xid;
	slot->chain.dboid = MyDatabaseId;
	for (int i = 0; i < NUndoPersistenceLevels; i++)
	{
		slot->chain.first_batch_lsn[i] = XactUndo.first_batch_lsn[i];
		slot->chain.last_batch_lsn[i] = XactUndo.last_batch_lsn[i];
	}
	SpinLockRelease(&slot->mutex);
}

/*
 * LogXactUndoInflight
 *		WAL-log the UNDO chains of every transaction in flight.
 *
 * Called by the checkpointer after the checkpoint's redo pointer has been
 * determined.  Redo starts at that pointer and so does not see the batches a
 * transaction wrote before it; this record tells recovery about them.  A
 * transaction that commits or aborts after we read its slot is still listed;
 * redo ignores the entry if the transaction's outcome is already known, and
 * otherwise the commit or abort record, which follows this one, removes it.
 */
void
LogXactUndoInflight(void)
{
	xl_undo_inflight *rec;
	uint32		n = 0;

	if (XactUndoInflight == NULL)
		return;

	rec = palloc(SizeOfUndoInflight +
				 MaxBackends * sizeof(xl_undo_inflight_xact));
	for (int i = 0; i < MaxBackends; i++)
	{
		XactUndoInflightSlot *slot = &XactUndoInflight[i];

		SpinLockAcquire(&slot->mutex);
		if (TransactionIdIsValid(slot->chain.xid))
			rec->xacts[n++] = slot->chain;
		SpinLockRelease(&slot->mutex);
	}

	/*
	 * With a standby, written even when empty: the standby recomputes from it
	 * the WAL it must keep for the in-flight chains
	 * (UndoSetRecoveryBatchLSN). Unimportant, so that an idle system still
	 * skips checkpoints.
	 */
	if (n > 0 || XLogStandbyInfoActive())
	{
		rec->nxacts = n;
		XLogBeginInsert();
		XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
		XLogRegisterData(rec, SizeOfUndoInflight +
						 n * sizeof(xl_undo_inflight_xact));
		(void) XLogInsert(RM_UNDO_ID, XLOG_UNDO_INFLIGHT);
	}
	pfree(rec);
}

/*
 * InitializeXactUndo
 *		Per-backend initialization for transaction undo.
 */
void
InitializeXactUndo(void)
{
	/* Ensure the dynamic subxact stack is allocated */
	EnsureSubxactStackCapacity();

	ResetXactUndo();

	/*
	 * Register callback to track subtransaction lifecycle. Do this lazily on
	 * first transaction to ensure it's registered for the backend that will
	 * actually use UNDO.
	 */
	if (!subxact_callback_registered)
	{
		RegisterSubXactCallback(XactUndo_SubXactCallback, NULL);
		subxact_callback_registered = true;
	}
}

/*
 * GetUndoPersistenceLevel
 *		Map relation persistence character to UndoPersistenceLevel.
 */
static UndoPersistenceLevel
GetUndoPersistenceLevel(char relpersistence)
{
	switch (relpersistence)
	{
		case RELPERSISTENCE_PERMANENT:
			return UNDOPERSISTENCE_PERMANENT;
		case RELPERSISTENCE_UNLOGGED:
			return UNDOPERSISTENCE_UNLOGGED;
		case RELPERSISTENCE_TEMP:
			return UNDOPERSISTENCE_TEMP;
		default:
			elog(ERROR, "unrecognized relpersistence: %c", relpersistence);
			return UNDOPERSISTENCE_PERMANENT;	/* keep compiler quiet */
	}
}

/*
 * PrepareXactUndoData
 *		Prepare to insert a transactional undo record.
 *
 * Finds or creates the appropriate per-persistence-level UndoRecordSet
 * for the current transaction and adds the record to it.
 *
 * The API is AM-agnostic: callers pass an RM ID, RM-specific info,
 * a relation OID, and an opaque payload.
 *
 * Returns the UndoRecPtr where the record will be inserted (or
 * InvalidUndoRecPtr if undo is disabled).
 */
UndoRecPtr
PrepareXactUndoData(XactUndoContext *ctx, char persistence,
					uint8 rmid, uint16 info, Oid reloid,
					const char *payload, Size payload_len)
{
	int			nestingLevel = GetCurrentTransactionNestLevel();
	UndoPersistenceLevel plevel = GetUndoPersistenceLevel(persistence);
	UndoRecordSet *uset;
	XactUndoSubTransactionState *cur;
	UndoRecPtr *sub_start_location;

	/* Remember that we've done something undo-related. */
	XactUndo.has_undo = true;
	if (rmid != UNDO_RMID_NBTREE && rmid != UNDO_RMID_HASH)
		XactUndo.needs_inline_abort = true;

	/* The current (sub)transaction must have an xid; see below. */
	(void) GetCurrentTransactionId();

	/*
	 * Load the UndoRecordSetInsert() injection points into this backend's
	 * local cache now, while we are guaranteed not to be inside a critical
	 * section (PrepareXactUndoData may itself palloc).  UndoRecordSetInsert()
	 * runs inside the caller's crit section and can only fire these via the
	 * palloc-free INJECTION_POINT_CACHED variant.
	 */
	INJECTION_POINT_LOAD("undo-batch-before-wal-insert");
	INJECTION_POINT_LOAD("undo-batch-after-wal-insert");

	/*
	 * If we've entered a subtransaction deeper than what's currently tracked,
	 * push a new entry onto the subxact_stack.  This handles the case where
	 * PrepareXactUndoData is called for the first time in a subtransaction
	 * that was started before the SubXactCallback fired (e.g., if the
	 * callback hadn't been registered yet when the subtransaction began).
	 */
	cur = CURRENT_SUBXACT();
	if (nestingLevel > (int) cur->nestingLevel)
	{
		int			i;

		XactUndo.subxact_depth++;
		EnsureSubxactStackCapacity();

		cur = CURRENT_SUBXACT();
		cur->nestingLevel = nestingLevel;
		for (i = 0; i < NUndoPersistenceLevels; ++i)
		{
			cur->start_location[i] = InvalidUndoRecPtr;
			cur->last_batch_lsn[i] = XactUndo.last_batch_lsn[i];
			cur->chain_bytes[i] = XactUndo.chain_bytes[i];
		}
	}

	/*
	 * Make sure we have an UndoRecordSet of the appropriate type open for
	 * this persistence level.  These record sets are always associated with
	 * the toplevel transaction, not a subtransaction, to avoid fragmentation.
	 *
	 * The set, and so every batch it writes, is stamped with the top-level
	 * xid even when the first record comes from a subtransaction.  Crash
	 * recovery tracks loser chains by that xid and forgets them at the top
	 * level's commit or abort record; a subxid stamp would outlive the commit
	 * and get a committed transaction's UNDO applied.
	 */
	uset = XactUndo.record_set[plevel];
	if (uset == NULL)
	{
		uset = UndoRecordSetCreate(GetTopTransactionId(),
								   GetCurrentTransactionUndoRecPtr());
		XactUndo.record_set[plevel] = uset;
	}

	/* Remember persistence level for InsertXactUndoData. */
	ctx->plevel = plevel;
	ctx->uset = uset;

	/* Add the record to the record set using generic payload API. */
	UndoRecordAddPayload(uset, rmid, info, reloid, payload, payload_len);

	/*
	 * If this is the first undo for this persistence level in this
	 * subtransaction, record the start location. The actual UndoRecPtr is not
	 * known until insertion, so we use a sentinel for now and the caller will
	 * update it after InsertXactUndoData.
	 */
	sub_start_location = &cur->start_location[plevel];
	if (!UndoRecPtrIsValid(*sub_start_location))
		*sub_start_location = (UndoRecPtr) 1;	/* will be set properly */

	return InvalidUndoRecPtr;	/* actual ptr assigned during insert */
}

/*
 * PrepareXactUndoDataParts
 *		Like PrepareXactUndoData, but with scatter-gather payload.
 *
 * Used when the payload is in two non-contiguous pieces (e.g., a fixed
 * header struct followed by variable-length tuple data).  Avoids the
 * need to assemble an intermediate contiguous buffer.
 */
UndoRecPtr
PrepareXactUndoDataParts(XactUndoContext *ctx, char persistence,
						 uint8 rmid, uint16 info, Oid reloid,
						 const char *part1, Size part1_len,
						 const char *part2, Size part2_len)
{
	int			nestingLevel = GetCurrentTransactionNestLevel();
	UndoPersistenceLevel plevel = GetUndoPersistenceLevel(persistence);
	UndoRecordSet *uset;
	XactUndoSubTransactionState *cur;
	UndoRecPtr *sub_start_location;

	/* Remember that we've done something undo-related. */
	XactUndo.has_undo = true;
	if (rmid != UNDO_RMID_NBTREE && rmid != UNDO_RMID_HASH)
		XactUndo.needs_inline_abort = true;

	/* The current (sub)transaction must have an xid; see below. */
	(void) GetCurrentTransactionId();

	/*
	 * If we've entered a subtransaction deeper than what's currently tracked,
	 * push a new entry onto the subxact_stack.
	 */
	cur = CURRENT_SUBXACT();
	if (nestingLevel > (int) cur->nestingLevel)
	{
		int			i;

		XactUndo.subxact_depth++;
		EnsureSubxactStackCapacity();

		cur = CURRENT_SUBXACT();
		cur->nestingLevel = nestingLevel;
		for (i = 0; i < NUndoPersistenceLevels; ++i)
		{
			cur->start_location[i] = InvalidUndoRecPtr;
			cur->last_batch_lsn[i] = XactUndo.last_batch_lsn[i];
			cur->chain_bytes[i] = XactUndo.chain_bytes[i];
		}
	}

	/*
	 * Make sure we have an UndoRecordSet of the appropriate type open for
	 * this persistence level, stamped with the top-level xid (see
	 * PrepareXactUndoData).
	 */
	uset = XactUndo.record_set[plevel];
	if (uset == NULL)
	{
		uset = UndoRecordSetCreate(GetTopTransactionId(),
								   GetCurrentTransactionUndoRecPtr());
		XactUndo.record_set[plevel] = uset;
	}

	/* Remember persistence level for InsertXactUndoData. */
	ctx->plevel = plevel;
	ctx->uset = uset;

	/* Add the record using scatter-gather payload API. */
	UndoRecordAddPayloadParts(uset, rmid, info, reloid,
							  part1, part1_len, part2, part2_len);

	/*
	 * If this is the first undo for this persistence level in this
	 * subtransaction, record the start location.
	 */
	sub_start_location = &cur->start_location[plevel];
	if (!UndoRecPtrIsValid(*sub_start_location))
		*sub_start_location = (UndoRecPtr) 1;	/* will be set properly */

	return InvalidUndoRecPtr;	/* actual ptr assigned during insert */
}

/*
 * InsertXactUndoData
 *		Insert the prepared undo data into the undo log.
 *
 * This performs the actual write of the accumulated records.
 * Also updates the transaction-level undo record pointer (undoRecPtr
 * in TransactionState) so that subsequent UNDO records chain correctly.
 */
void
InsertXactUndoData(XactUndoContext *ctx)
{
	UndoRecordSet *uset = ctx->uset;
	UndoRecPtr	ptr;
	int			saved_delay = 0;

	Assert(uset != NULL);

	/*
	 * Hold off a checkpoint's redo-pointer phase while the batch is inserted
	 * and published for LogXactUndoInflight(): a batch that precedes the redo
	 * pointer must be visible to the checkpointer when it reads the slots.
	 */
	if (MyProc != NULL)
	{
		saved_delay = MyProc->delayChkptFlags;
		MyProc->delayChkptFlags |= DELAY_CHKPT_START;
	}

	ptr = UndoRecordSetInsert(uset);
	if (UndoRecPtrIsValid(ptr))
	{
		XactUndoSubTransactionState *cur = CURRENT_SUBXACT();

		XactUndo.last_location[ctx->plevel] = ptr;
		XactUndo.chain_bytes[ctx->plevel] += UndoRecordSetGetSize(uset);

		/*
		 * Track the WAL LSN of the most recent UNDO batch for this
		 * persistence level.  This is used during rollback to walk the UNDO
		 * chain backward through WAL.
		 *
		 * The pointer must be a valid LSN; the caller must have produced an
		 * RM_UNDO_ID XLOG_UNDO_BATCH record via UndoRecordSetInsert.  We do
		 * not validate the rmid here because UndoValidateBatchLSN reads from
		 * the on-disk WAL via read_local_xlog_page, which waits on flush and
		 * cannot be safely called from the WAL-insertion hot path.  The
		 * rollback path at AtAbort_XactUndo validates before applying.
		 */
		Assert(XLogRecPtrIsValid(uset->last_batch_lsn));
		XactUndo.last_batch_lsn[ctx->plevel] = uset->last_batch_lsn;
		if (!XLogRecPtrIsValid(XactUndo.first_batch_lsn[ctx->plevel]))
			XactUndo.first_batch_lsn[ctx->plevel] = uset->last_batch_lsn;
		PublishXactUndoChain(uset->xid);

		/* Fix up subtransaction start location if needed */
		if (cur->start_location[ctx->plevel] == (UndoRecPtr) 1)
			cur->start_location[ctx->plevel] = ptr;

		/*
		 * Update the per-transaction undo pointer in TransactionState so that
		 * the next UndoRecordSetCreate (if called directly by heap AM or
		 * other subsystems) picks up the correct chain pointer.
		 */
		SetCurrentTransactionUndoRecPtr(ptr);
	}
	else
	{
		XactUndoSubTransactionState *cur = CURRENT_SUBXACT();

		/*
		 * The batch was empty or elided, so no record pointer exists to fix
		 * up the sentinel with.  Leaving the (UndoRecPtr) 1 sentinel in place
		 * would make UndoRecPtrIsValid() treat it as a real start location
		 * and leak it into subxact->parent merges and rollback chain walks;
		 * reset it so this level records no start location.
		 */
		if (cur->start_location[ctx->plevel] == (UndoRecPtr) 1)
			cur->start_location[ctx->plevel] = InvalidUndoRecPtr;
	}

	if (MyProc != NULL)
		MyProc->delayChkptFlags = saved_delay;
}

/*
 * CleanupXactUndoInsertion
 *		Clean up after an undo insertion cycle.
 *
 * Resets the record set's buffer position and record count so it can
 * accumulate more records.  Does NOT free the record set -- that
 * happens at transaction end (AtCommit_XactUndo / AtAbort_XactUndo).
 *
 * The record set's prev_undo_ptr is preserved across resets (it was
 * updated by UndoRecordSetInsert), so subsequent records chain
 * correctly through the undo log.
 */
void
CleanupXactUndoInsertion(XactUndoContext *ctx)
{
	if (ctx->uset != NULL)
		UndoRecordSetReset(ctx->uset);
}

/*
 * DeferXactUndoData
 *		Leave this record in the UndoRecordSet buffer to share a batch.
 *
 * See the contract in access/xactundo.h.  PrepareXactUndoData[Parts] has
 * already appended the record to ctx->uset and set XactUndo.has_undo; the only
 * thing InsertXactUndoData() would add right now is a WAL record per call,
 * which is precisely the cost we are avoiding.  So we do nothing except
 * remember which record set is pending and enforce the memory bound.
 *
 * The thresholds are the existing undo_batch_size_kb / undo_batch_record_limit
 * GUCs, so an unbounded statement (COPY, a 10M-row INSERT ... SELECT) still
 * flushes at a fixed ceiling rather than buffering the whole thing.  An
 * intermediate flush is a complete, self-contained batch: UndoRecordSetInsert()
 * publishes its LSN into uset->last_batch_lsn, InsertXactUndoData() copies that
 * to XactUndo.last_batch_lsn[] and chains the next batch onto it via
 * SetCurrentTransactionUndoRecPtr(), so the chain that rollback walks is the
 * same whether it was written in one batch or fifty.
 */
bool
DeferXactUndoData(XactUndoContext *ctx)
{
	UndoRecordSet *uset = ctx->uset;

	Assert(uset != NULL);

	/*
	 * Remember the pending set and its persistence level so the flush choke
	 * points can find it without the original caller's context.
	 */
	XactUndo.pending_uset = uset;
	XactUndo.pending_plevel = ctx->plevel;

	/*
	 * Bound the buffer.  Without this a single large statement would hold
	 * every record in memory until transaction end.
	 */
	if (UndoRecordSetGetSize(uset) >= (Size) undo_batch_size_kb * 1024 ||
		UndoRecordSetGetNumRecords(uset) >= undo_batch_record_limit)
	{
		XactUndoFlushPending();
		return false;
	}

	return true;
}

/*
 * XactUndoHasPendingData
 *		Is a deferred record waiting to be written?
 */
bool
XactUndoHasPendingData(void)
{
	return XactUndo.pending_uset != NULL &&
		UndoRecordSetGetNumRecords(XactUndo.pending_uset) > 0;
}

/*
 * XactUndoFlushPending
 *		Write all deferred records as a single UNDO batch.
 *
 * This is the whole safety argument for deferral, so it is deliberately
 * callable from anywhere that the chain might be needed and is a no-op when
 * there is nothing pending.  It performs exactly what the old per-record path
 * did -- InsertXactUndoData() + CleanupXactUndoInsertion() -- once, for the
 * accumulated batch.
 *
 * The crit section matches the contract documented at UndoRecordSetInsert():
 * the WAL insert itself runs in one, while the palloc-capable preparation
 * already happened back in PrepareXactUndoData[Parts].
 */
void
XactUndoFlushPending(void)
{
	XactUndoContext ctx;
	UndoRecordSet *uset = XactUndo.pending_uset;

	if (uset == NULL || UndoRecordSetGetNumRecords(uset) == 0)
	{
		XactUndo.pending_uset = NULL;
		return;
	}

	ctx.uset = uset;
	ctx.plevel = XactUndo.pending_plevel;

	/*
	 * Clear the pending marker BEFORE writing.  If InsertXactUndoData()
	 * throws (it should not -- it is a WAL insert in a crit section, which
	 * PANICs rather than throws), the error path must not find a stale
	 * pending set and write these records a second time.
	 */
	XactUndo.pending_uset = NULL;

	START_CRIT_SECTION();
	InsertXactUndoData(&ctx);
	CleanupXactUndoInsertion(&ctx);
	END_CRIT_SECTION();
}


/*
 * GetCurrentXactUndoRecPtr
 *		Get the most recent undo record pointer for a persistence level.
 */
UndoRecPtr
GetCurrentXactUndoRecPtr(UndoPersistenceLevel plevel)
{
	return XactUndo.last_location[plevel];
}

/*
 * GetCurrentXactLastBatchLSN
 *		Get the WAL LSN of the most recent UNDO batch for a persistence level.
 *
 * Used during transaction abort to start the WAL-based UNDO chain walk.
 */
XLogRecPtr
GetCurrentXactLastBatchLSN(UndoPersistenceLevel plevel)
{
	return XactUndo.last_batch_lsn[plevel];
}

/*
 * GetCurrentXactFirstBatchLSN
 *		Get the WAL LSN of the oldest UNDO batch for a persistence level.
 *
 * WAL from here on must survive until the chain has been applied; PREPARE
 * saves it so the prepared transaction keeps pinning it.
 */
XLogRecPtr
GetCurrentXactFirstBatchLSN(UndoPersistenceLevel plevel)
{
	return XactUndo.first_batch_lsn[plevel];
}

/*
 * XActUndoUpdateLastBatchLSN
 *		Record the LSN of an UNDO batch for the current transaction.
 *
 * Called from the heap DML code after writing an UNDO batch -- either
 * embedded inside a heap WAL record (HAS_UNDO path) or as a standalone
 * XLOG_UNDO_BATCH overflow record.  Updates last_batch_lsn so that
 * AtAbort_XactUndo() can find the head of the UNDO chain, and registers
 * the batch LSN for WAL retention tracking on first call per transaction.
 */
void
XActUndoUpdateLastBatchLSN(XLogRecPtr lsn, UndoPersistenceLevel plevel)
{
	if (!XLogRecPtrIsValid(lsn) || plevel >= NUndoPersistenceLevels)
		return;

	/*
	 * The pointer must be a valid LSN.  Rmid validation is deferred to the
	 * rollback path; see comment in InsertXactUndoData for why we cannot call
	 * UndoValidateBatchLSN here.
	 */
	Assert(XLogRecPtrIsValid(lsn));

	XactUndo.has_undo = true;
	XactUndo.last_batch_lsn[plevel] = lsn;
}

/*
 * XactUndoHasUnrecoverableUndo
 *		Does the current transaction hold UNDO that has no working ROLLBACK
 *		PREPARED apply path yet?
 *
 * Both UNDO mechanisms are now recoverable across 2PC:
 *
 *   - Cluster-wide UNDO (nbtree/hash and other cluster-wide consumers;
 *     last_batch_lsn): the permanent chain-head LSN is durably saved in
 *     xl_xact_prepare, its WAL is pinned while the xact stays prepared
 *     (undo_batch_lsn in twophase.c / UndoGetOldestBatchLSN), and
 *     FinishPreparedTransaction() feeds it to ATMAddAborted() on ROLLBACK
 *     PREPARED.
 *
 * Nothing remains that PREPARE must reject on UNDO grounds, so this returns
 * false unconditionally.  Kept as a single choke point (rather than deleting
 * the call in PrepareTransaction) so any future UNDO mechanism that is not
 * 2PC-safe has one obvious place to re-assert a guard.
 */
bool
XactUndoHasUnrecoverableUndo(void)
{
	return false;
}

/*
 * AtCommit_XactUndo
 *		Post-commit cleanup of the undo state.
 *
 * On commit, undo records are no longer needed for rollback.
 * Free all record sets and reset state.
 *
 * UNDO pages are managed by shared_buffers and flushed by the
 * checkpointer -- no per-commit fdatasync is needed.  We only
 * flush the deferred WAL allocation records so recovery can
 * reconstruct the UNDO log insert pointer.
 *
 * NB: This code MUST NOT FAIL, since it is run as a post-commit step.
 */
void
AtCommit_XactUndo(void)
{
	int			i;

	/*
	 * Write any deferred records before we decide there is nothing to do.
	 *
	 * A committing transaction will not apply its own UNDO, but the records
	 * must still reach WAL: a later ROLLBACK PREPARED, the recovery UNDO
	 * phase, or (for a subxact-aborted chain) the revert worker can all need
	 * them, and the WAL retention bookkeeping keyed off the batch LSN must
	 * see them too. Flushing here also keeps the invariant that no
	 * UndoRecordSet is freed below while still holding unwritten records.
	 */
	XactUndoFlushPending();

	if (!XactUndo.has_undo)
	{
		/* Flush any deferred WAL even if has_undo is false */
		UndoWalBatchFlush();
		return;
	}

	/*
	 * With UNDO-in-WAL, all UNDO data was already written to WAL via
	 * XLOG_UNDO_BATCH records during the transaction.  The single XLogFlush()
	 * at commit time (in RecordTransactionCommit) ensures both the UNDO data
	 * and the commit record are durable.  No separate fdatasync is needed.
	 *
	 * Legacy WAL batch flush is now a no-op but kept for safety.
	 */
	UndoWalBatchFlush();

	/*
	 * Free all per-persistence-level record sets.
	 *
	 * We can safely call UndoRecordSetFree() during commit because we're in
	 * CurTransactionContext, not BumpContext (which is only used during
	 * abort). The record sets are allocated in CurTransactionContext and will
	 * be freed when that context is destroyed at transaction end.
	 */
	for (i = 0; i < NUndoPersistenceLevels; i++)
	{
		if (XactUndo.record_set[i] != NULL)
		{
			UndoRecordSetFree(XactUndo.record_set[i]);
			XactUndo.record_set[i] = NULL;
		}
	}

	/* Release WAL retention hold acquired in UndoRecordSetInsert(). */
	UndoClearBatchLSN();

	ResetXactUndo();
}

/*
 * AtPrepare_XactUndo
 *		End-of-PREPARE cleanup of the backend-private undo state.
 *
 * PREPARE TRANSACTION hands ownership of this transaction's cluster-wide UNDO
 * to the 2PC state: the permanent chain-head LSN was durably saved in
 * xl_xact_prepare (twophase.c gxact->undo_batch_lsn), its WAL is pinned while
 * the xact stays prepared, and ROLLBACK PREPARED feeds that LSN to
 * ATMAddAborted() so the logical revert worker walks the chain.
 *
 * The in-memory UndoRecordSets, however, live in TopTransactionContext, which
 * PrepareTransaction() destroys (AtCommit_Memory) right after this.  We must
 * therefore drop the backend-private references to them here, or the dangling
 * XactUndo.record_set[] pointers get freed at backend exit (AtProcExit_XactUndo
 * -> UndoRecordSetFree on already-freed memory) and crash.  We only NULL the
 * pointers (via ResetXactUndo); the record sets themselves are freed by
 * TopTransactionContext destruction.
 *
 * Unlike AtCommit_XactUndo, we do NOT call UndoClearBatchLSN(): the WAL
 * retention hold must persist for as long as the transaction stays prepared.
 */
void
AtPrepare_XactUndo(void)
{
	/* Flush any deferred UNDO WAL so the prepared xact's chain is durable. */
	UndoWalBatchFlush();

	/*
	 * Any deferred batch was already written in PrepareTransaction(), before
	 * the 2PC state captured the chain-head LSN.  Assert that, because a
	 * pending record here would have been silently excluded from the prepared
	 * transaction's chain.
	 */
	Assert(!XactUndoHasPendingData());

	/*
	 * Drop the backend-private record-set references (memory freed with
	 * TopTransactionContext); keep the WAL retention hold.
	 */
	ResetXactUndo();
}

/*
 * AtAbort_XactUndo
 *		Post-abort cleanup of the undo state.
 *
 * On abort, we need to apply the undo chain to roll back changes.
 * The actual undo application is triggered by xact.c before calling
 * this function.  Here we apply the transaction's UNDO and clean up the record sets.
 *
 * The UNDO records are already in WAL.  If we crash during rollback,
 * before the abort record is written, recovery finds the transaction still
 * incomplete and rolls it back from WAL again.
 */
void
AtAbort_XactUndo(void)
{
	int			i;
	bool		lsn_safely_held = false;	/* true if inline UNDO or ATM
											 * holds LSN */

	/*
	 * Write any deferred records FIRST.  This is the ordering the whole
	 * deferred-batch scheme rests on.
	 *
	 * The has_undo test immediately below, and the last_batch_lsn[] chain
	 * head the apply path walks, are both only set by InsertXactUndoData(). A
	 * record that is still pending has therefore published nothing, and every
	 * one of the aborted index entries it describes would be left for VACUUM
	 * -- the exact silent no-op this module's header warns about. Flushing
	 * here, at the top of the abort path and before RecordTransactionAbort()
	 * runs, makes the deferral invisible to everything downstream.
	 *
	 * This also covers an error thrown mid-statement: the error unwinds into
	 * AbortTransaction(), which calls us, so the partial statement's records
	 * are written and applied exactly like a completed statement's.
	 */
	XactUndoFlushPending();

	/* Always clean up the recycled context; see AtCommit_XactUndo. */
	UndoRecordSetResetCache();

	if (!XactUndo.has_undo)
	{
		/* No UNDO data was written; nothing to do */
		UndoWalBatchReset();
		return;
	}

	/*
	 * With UNDO-in-WAL, all UNDO data is already in the WAL stream. No
	 * separate sync is needed.  The UNDO data can be read back from WAL for
	 * rollback via UndoReadBatchFromWAL().
	 *
	 * For crash safety during abort: if we crash mid-rollback, the recovery
	 * undo phase will find this transaction's UNDO batches in WAL and
	 * complete the rollback.
	 */
	UndoWalBatchFlush();		/* no-op, kept for safety */

	INJECTION_POINT("undo-xact-abort-before-apply", NULL);

	/* Collapse all subtransaction state. */
	CollapseXactUndoSubTransactions();

	/*
	 * UNDO application strategy: inline for small transactions, deferred for
	 * large ones.  Controlled by undo_instant_abort_threshold GUC.
	 *
	 * For small transactions (< threshold bytes of UNDO): apply UNDO
	 * synchronously in this backend.  This avoids ATM pool accumulation and
	 * eliminates the dependency on the background logical revert worker.
	 *
	 * For large transactions (>= threshold): register in the ATM for deferred
	 * asynchronous rollback by the logical revert worker.  Only a transaction
	 * whose UNDO is all index UNDO may be deferred (needs_inline_abort): the
	 * effects of anything else, such as FILEOPS, must be gone when ROLLBACK
	 * returns.
	 *
	 * The BumpContext issue (pfree crashes during abort) is avoided by: -
	 * Creating a temporary AllocSetContext for inline UNDO application -
	 * ApplyUndoChainFromWAL already avoids pfree on its allocations -
	 * Switching back to the abort context afterward
	 */
	{
		XLogRecPtr	perm_lsn =
			XactUndo.last_batch_lsn[UNDOPERSISTENCE_PERMANENT];

		if (XLogRecPtrIsValid(perm_lsn))
		{
			/*
			 * Size of the chain we are about to apply or defer.  Only the
			 * permanent chain is ever applied, so only its size counts.
			 */
			Size		total_undo_bytes =
				XactUndo.chain_bytes[UNDOPERSISTENCE_PERMANENT];

			/*
			 * The UNDO batch records were inserted into the WAL buffers
			 * during this transaction but may not yet be flushed to disk. The
			 * inline apply path reads them back via the local XLog reader,
			 * which would otherwise busy-wait (pg_usleep) for the walwriter
			 * to flush past the batch LSN -- adding multi-second latency to
			 * every rollback. Flush WAL up to the end of the last record this
			 * backend wrote so the batch is immediately readable. We must not
			 * flush to GetXLogInsertRecPtr(): under concurrency that global
			 * insert position can point into the middle of a record another
			 * backend has reserved but not finished copying, so XLogFlush
			 * would fail with "xlog flush request is not satisfied".
			 * XactLastRecEnd is always a valid record boundary and, since
			 * this backend wrote the UNDO batch, is guaranteed to be at or
			 * past the batch end. This mirrors the commit-path flush in
			 * RecordTransactionCommit.
			 */
			XLogFlush(XactLastRecEnd);

			if (XactUndo.needs_inline_abort ||
				(undo_instant_abort_threshold > 0 &&
				 total_undo_bytes < (Size) undo_instant_abort_threshold))
			{
				/*
				 * Small transaction: apply UNDO inline.  Use a dedicated
				 * AllocSetContext to avoid BumpContext pfree issues.
				 */
				MemoryContext undo_ctx;
				MemoryContext old_ctx;
				int			saved_trans_state;
				uint32		saved_holdoff;

				undo_ctx = AllocSetContextCreate(TopMemoryContext,
												 "Inline UNDO Apply",
												 ALLOCSET_DEFAULT_SIZES);
				old_ctx = MemoryContextSwitchTo(undo_ctx);

				{
					volatile bool undo_applied = false;

					/*
					 * Validate the batch LSN points to an actual UNDO record
					 * before attempting inline application.  Stale LSNs from
					 * chain_prev tracking anomalies can point to non-UNDO WAL
					 * records, which would cause "not an UNDO batch"
					 * warnings.
					 */
					if (!UndoValidateBatchLSN(perm_lsn))
					{
						elog(DEBUG1, "inline UNDO: last_batch_lsn %X/%X is not "
							 "a valid UNDO batch, deferring to ATM",
							 LSN_FORMAT_ARGS(perm_lsn));
						undo_applied = false;
						goto inline_undo_done;
					}

					/*
					 * AbortTransaction() has already advanced the transaction
					 * state to TRANS_ABORT, but the relcache, locks, and
					 * resource owner are all still live.  UNDO appliers open
					 * relations, which asserts IsTransactionState();
					 * temporarily present TRANS_INPROGRESS for the duration
					 * of the inline apply and always restore the real state
					 * afterward.
					 *
					 * The error that is aborting us may have been raised
					 * while this backend held buffer content locks, which
					 * resource-owner cleanup releases only later.  The
					 * appliers lock pages, possibly those very ones, so
					 * release the stragglers first (as PbuAtAbort_ApplyUndo
					 * does).  LWLockReleaseAll does not cover them.
					 */
					BufferLockReleaseAll();
					saved_trans_state = EnterInlineUndoApplyState();
					saved_holdoff = InterruptHoldoffCount;
					PG_TRY();
					{
						undo_applied = ApplyUndoChainFromWAL(perm_lsn);
					}
					PG_CATCH();
					{
						/*
						 * Already inside AbortTransaction: we cannot abort
						 * again or rethrow. Repeat its early cleanup and
						 * fall back to ATM; its ResourceOwnerRelease will
						 * release pins and other resources later.
						 * Release any content locks the failed chain left
						 * held before unwinding.  The error reset
						 * InterruptHoldoffCount, but the abort that runs this
						 * still holds interrupts.
						 */
						InterruptHoldoffCount = saved_holdoff;
						BufferLockReleaseAll();
						LWLockReleaseAll();
						XLogResetInsertion();
						ConditionVariableCancelSleep();
						pgaio_error_cleanup();
						FlushErrorState();
						undo_applied = false;
					}
					PG_END_TRY();
					LeaveInlineUndoApplyState(saved_trans_state);

					MemoryContextSwitchTo(old_ctx);
					MemoryContextDelete(undo_ctx);

			inline_undo_done:
					if (!undo_applied)
					{
						/*
						 * Inline UNDO failed (WAL recycled, wrong record
						 * type, a record that could not be applied, or an
						 * error).  Register in ATM for deferred processing by
						 * the revert worker; re-applying the records that did
						 * apply is harmless (see ApplyOneUndoRecord).
						 */
						elog(DEBUG1, "inline UNDO failed for xid %u, "
							 "deferring to ATM",
							 GetCurrentTransactionId());

						if (ATMAddAborted(GetCurrentTransactionId(),
										  MyDatabaseId, perm_lsn,
										  XactUndo.first_batch_lsn[UNDOPERSISTENCE_PERMANENT]))
							lsn_safely_held = true; /* ATM holds the LSN */
						else
							elog(WARNING, "ATM full: could not record aborted transaction %u", GetCurrentTransactionId());




					}
					else
					{
						lsn_safely_held = true; /* UNDO fully applied, WAL can
												 * be recycled */
						ereport(DEBUG2,
								(errmsg("inline UNDO applied for xid %u "
										"(%zu bytes)",
										GetCurrentTransactionId(),
										total_undo_bytes)));
					}
				}
			}
			else
			{
				/*
				 * Large transaction or threshold=0: register in ATM for
				 * deferred rollback by the logical revert worker.
				 */
				if (ATMAddAborted(GetCurrentTransactionId(),
								  MyDatabaseId, perm_lsn,
								  XactUndo.first_batch_lsn[UNDOPERSISTENCE_PERMANENT]))
					lsn_safely_held = true;
				else
					elog(WARNING,
						 "ATM full: could not record aborted transaction %u",
						 GetCurrentTransactionId());
			}
		}
	}

	INJECTION_POINT("undo-xact-abort-after-atm", NULL);

	/* Free all per-persistence-level record sets. */
	for (i = 0; i < NUndoPersistenceLevels; i++)
	{
		if (XactUndo.record_set[i] != NULL)
		{
			UndoRecordSetFree(XactUndo.record_set[i]);
			XactUndo.record_set[i] = NULL;
		}
	}

	/*
	 * Release the WAL retention hold if nothing needs the UNDO records any
	 * more: inline UNDO completed, or the ATM recorded the transaction (the
	 * revert worker reads the chain from the ATM's copy of the LSN).
	 *
	 * Otherwise the slot keeps the hold, but only until this backend's next
	 * transaction commits or aborts, or the backend exits; both clear it.
	 * Nothing re-applies the chain in the meantime.  The abort record is
	 * already written, so crash recovery does not treat the transaction as a
	 * loser either.
	 */
	if (lsn_safely_held)
		UndoClearBatchLSN();
	else
		elog(DEBUG1, "retaining per-backend UNDO LSN slot (ATM and inline both failed)");

	ResetXactUndo();
}

/*
 * AtSubCommit_XactUndo
 *		Subtransaction commit: merge sub undo state into parent.
 */
void
AtSubCommit_XactUndo(int level)
{
	XactUndoSubTransactionState *cur;
	XactUndoSubTransactionState *parent;
	int			i;

	/*
	 * Flush before merging this subtransaction's state into the parent.
	 *
	 * On subxact COMMIT the records stay live -- they now belong to the
	 * parent and must still be applied if the parent later aborts.  Writing
	 * them here rather than letting them ride into the parent's batch keeps
	 * the chain segments aligned with the savepoint boundaries that
	 * AtSubAbort's bounded walk uses, so a subsequent ROLLBACK TO an outer
	 * savepoint reverts exactly the intended range.  This is also what makes
	 * the START_SUB flush sufficient: every boundary, entering or leaving,
	 * has a batch edge.
	 */
	XactUndoFlushPending();

	if (XactUndo.subxact_depth <= 0)
		return;

	cur = CURRENT_SUBXACT();
	if ((int) cur->nestingLevel != level)
		return;

	parent = &XactUndo.subxact_stack[XactUndo.subxact_depth - 1];

	/*
	 * Merge start locations into parent.
	 *
	 * Invariant: all UNDO records for this transaction, regardless of nesting
	 * level, are stored in a single chain per persistence level (one
	 * UndoRecordSet).  start_location tracks the earliest record the
	 * subtransaction generated.  Since records are strictly append-only, the
	 * parent's start location is always earlier than the subtransaction's if
	 * it exists.  We only update the parent's start when it is not yet set
	 * (the parent wrote no UNDO before this subtransaction).
	 */
	for (i = 0; i < NUndoPersistenceLevels; i++)
	{
		if (UndoRecPtrIsValid(cur->start_location[i]) &&
			!UndoRecPtrIsValid(parent->start_location[i]))
		{
			parent->start_location[i] = cur->start_location[i];
		}
	}

	XactUndo.subxact_depth--;
}

/*
 * AtSubAbort_XactUndo
 *		Subtransaction abort: apply undo for this sub-level, clean up.
 *
 * We apply the subtransaction's records synchronously, in this backend, so
 * that the effects of the aborting subtransaction are reversed before control
 * returns to the caller.  If that fails, the subtransaction's part of the
 * chain is handed to the logical revert worker through the ATM.
 *
 * Any AM-private per-subtransaction tracking state is cleaned up by the
 * owning AM's own SubXactCallback.
 */
void
AtSubAbort_XactUndo(int level)
{
	XactUndoSubTransactionState *cur;
	XactUndoSubTransactionState *parent;
	int			i;

	/*
	 * Write any deferred records before deciding what this subtransaction
	 * wrote.
	 *
	 * Records accumulated in the pending batch belong to whichever
	 * subtransaction was running when they were added -- this one.  The eager
	 * rollback below detects the subtransaction's work by comparing
	 * XactUndo.last_batch_lsn[i] against the value saved at subxact start,
	 * and only InsertXactUndoData() moves that pointer.  Leaving records
	 * pending would therefore make the comparison find nothing, and this is
	 * the one place where that loses data permanently rather than deferring
	 * it: an aborted subtransaction whose parent goes on to COMMIT creates no
	 * top-level ATM entry, so nothing would ever revert its index entries --
	 * they would persist as if committed.  Flushing here makes them part of
	 * this subtransaction's chain segment, which the bounded walk then
	 * reverses.
	 *
	 * Note the flush must precede the loop, not sit inside it: it advances
	 * last_batch_lsn[] for the pending level, which is exactly the value the
	 * loop reads.
	 */
	XactUndoFlushPending();

	if (XactUndo.subxact_depth <= 0)
		return;

	cur = CURRENT_SUBXACT();
	if ((int) cur->nestingLevel != level)
		return;

	parent = &XactUndo.subxact_stack[XactUndo.subxact_depth - 1];

	/*
	 * Apply UNDO for records generated during this subtransaction.  We
	 * iterate the record sets and apply records whose UndoRecPtr is at or
	 * after this subtransaction's start_location.
	 *
	 * For each persistence level where this subtransaction generated UNDO
	 * records, apply them synchronously.  The parent transaction's records
	 * (before the subtransaction's start_location) are preserved.
	 */
	for (i = 0; i < NUndoPersistenceLevels; i++)
	{
		UndoRecPtr	sub_start = cur->start_location[i];

		if (!UndoRecPtrIsValid(sub_start))
			continue;

		/*
		 * Eagerly roll back the cluster-wide UNDO this subtransaction wrote.
		 *
		 * If the subtransaction advanced XactUndo.last_batch_lsn[i] beyond
		 * the value saved at subtransaction start, it wrote UNDO batches that
		 * must now be reversed.  We must apply them here rather than defer to
		 * the top-level ATM entry: a subtransaction can abort while its
		 * parent goes on to COMMIT, in which case no top-level ATM entry is
		 * ever created and the deferred UNDO would never run, leaking the
		 * aborted subtransaction's writes as if committed.
		 *
		 * Apply only the batches strictly newer than the parent's saved head
		 * (cur->last_batch_lsn[i]) via the bounded chain walk, so the
		 * parent's and earlier subtransactions' batches are preserved.
		 * Afterward restore last_batch_lsn[i] to the parent's value so a
		 * later parent abort walks only the parent's batches and does not
		 * double-apply ours.
		 *
		 * BumpContext safety: subtransaction abort may run under a
		 * BumpContext that does not support pfree(), so apply inside a
		 * dedicated AllocSetContext and present an in-progress transaction
		 * state for the relation opens, mirroring the inline path in
		 * AtAbort_XactUndo().
		 */
		if (XLogRecPtrIsValid(XactUndo.last_batch_lsn[i]) &&
			XactUndo.last_batch_lsn[i] != cur->last_batch_lsn[i])
		{
			XLogRecPtr	sub_head = XactUndo.last_batch_lsn[i];
			XLogRecPtr	parent_head = cur->last_batch_lsn[i];
			volatile bool applied = false;

			/*
			 * Invariant: the current head must be strictly newer (larger)
			 * than the parent's saved head.  If this fires, a sibling
			 * subtransaction improperly modified last_batch_lsn after its own
			 * abort, which would cause us to skip or double-apply batches.
			 */
			Assert(!XLogRecPtrIsValid(parent_head) ||
				   sub_head > parent_head);

			/*
			 * Flush WAL so the subtransaction's batches are readable by the
			 * local XLog reader without busy-waiting on the walwriter.  See
			 * the matching flush in AtAbort_XactUndo() for why XactLastRecEnd
			 * is the correct, always-valid record boundary to flush to.
			 */
			XLogFlush(XactLastRecEnd);

			if (UndoValidateBatchLSN(sub_head))
			{
				MemoryContext undo_ctx;
				MemoryContext old_ctx;
				int			saved_trans_state;
				uint32		saved_holdoff;

				undo_ctx = AllocSetContextCreate(TopMemoryContext,
												 "Subxact UNDO Apply",
												 ALLOCSET_DEFAULT_SIZES);
				old_ctx = MemoryContextSwitchTo(undo_ctx);

				/*
				 * Release buffer content locks the failed subtransaction may
				 * still hold -- an error raised mid-split leaves both halves
				 * locked -- before the appliers lock pages again.  See
				 * AtAbort_XactUndo.
				 */
				BufferLockReleaseAll();
				saved_trans_state = EnterInlineUndoApplyState();
				saved_holdoff = InterruptHoldoffCount;
				PG_TRY();
				{
					applied = ApplyUndoChainFromWALBounded(sub_head,
														   parent_head);
				}
				PG_CATCH();
				{
					/*
					 * Already inside AbortSubTransaction, so do not abort
					 * again or rethrow. Repeat its early cleanup; its
					 * ResourceOwnerRelease will release the rest.
					 * Release any content locks the failed subxact UNDO chain
					 * left held before unwinding, and hold interrupts again
					 * as the subtransaction abort did (see AtAbort_XactUndo).
					 */
					InterruptHoldoffCount = saved_holdoff;
					BufferLockReleaseAll();
					LWLockReleaseAll();
					XLogResetInsertion();
					ConditionVariableCancelSleep();
					pgaio_error_cleanup();
					FlushErrorState();
					applied = false;
				}
				PG_END_TRY();
				LeaveInlineUndoApplyState(saved_trans_state);

				MemoryContextSwitchTo(old_ctx);
				MemoryContextDelete(undo_ctx);
			}
			else
				elog(DEBUG1, "subxact UNDO: head %X/%X is not a valid UNDO "
					 "batch, deferring to ATM",
					 LSN_FORMAT_ARGS(sub_head));

			/*
			 * If the chain could not be applied in full, hand exactly this
			 * subtransaction's part of it to the logical revert worker.  The
			 * parent may go on to commit, in which case nothing else would
			 * ever revert it.  The entry is keyed by the subtransaction's own
			 * xid, so it cannot collide with a later entry for the top-level
			 * transaction, and bounded by parent_head, so the worker leaves
			 * the parent's batches alone.  We cannot raise an error here (we
			 * are already aborting), and rolling the parent back instead
			 * would discard work the user did not roll back.
			 *
			 * Only the permanent chain can be handed off, as for a top-level
			 * abort.  A revert worker cannot reach this backend's temporary
			 * relations, and an unlogged relation's UNDO is not applied by
			 * crash recovery either (its contents are reset), so there is no
			 * later point at which those chains could be completed; all we
			 * can do is report them.  (No relation that writes UNDO today is
			 * temporary or unlogged: see RelationUsesIndexUndo().)
			 */
			if (!applied && i != UNDOPERSISTENCE_PERMANENT)
				ereport(WARNING,
						(errmsg("could not apply the %s UNDO of rolled-back subtransaction %u",
								i == UNDOPERSISTENCE_TEMP ? "temporary" : "unlogged",
								GetCurrentTransactionIdIfAny()),
						 errdetail("Its effects are left in place.")));
			else if (!applied)
			{
				TransactionId subxid = GetCurrentTransactionIdIfAny();

				/*
				 * Its oldest batch is the first one after parent_head; WAL
				 * from parent_head on covers it (or from the transaction's
				 * first batch, if the parent had written none).
				 */
				XLogRecPtr	first = XLogRecPtrIsValid(parent_head) ?
					parent_head : XactUndo.first_batch_lsn[i];

				Assert(TransactionIdIsValid(subxid));
				if (!ATMAddAbortedBounded(subxid, MyDatabaseId, sub_head,
										  parent_head, first))
					elog(WARNING,
						 "ATM full: could not record rolled-back subtransaction %u",
						 subxid);
			}

			XactUndo.last_batch_lsn[i] = parent_head;
			XactUndo.chain_bytes[i] = cur->chain_bytes[i];

			/*
			 * The next batch chains onto the record set's last batch: point
			 * it back at the parent's, or a later walk of the chain would
			 * reach the batches just applied and apply them again.
			 */
			if (XactUndo.record_set[i] != NULL)
			{
				XactUndo.record_set[i]->last_batch_lsn = parent_head;
				XactUndo.record_set[i]->prev_undo_ptr = (UndoRecPtr) parent_head;
			}
			PublishXactUndoChain(GetTopTransactionIdIfAny());
		}

		/*
		 * Reset the last_location to what it was before this subtransaction,
		 * so that if the parent transaction continues and then aborts, only
		 * the parent's records are applied (the subtransaction's records have
		 * already been applied).
		 */
		if (UndoRecPtrIsValid(parent->start_location[i]))
			XactUndo.last_location[i] = parent->start_location[i];
	}

	XactUndo.subxact_depth--;
}

/*
 * AtProcExit_XactUndo
 *		Process exit cleanup for transaction undo.
 */
void
AtProcExit_XactUndo(void)
{
	int			i;

	/* Free any lingering record sets. */
	for (i = 0; i < NUndoPersistenceLevels; i++)
	{
		if (XactUndo.record_set[i] != NULL)
		{
			UndoRecordSetFree(XactUndo.record_set[i]);
			XactUndo.record_set[i] = NULL;
		}
	}

	/* Release any WAL retention hold (in case process exits mid-transaction). */
	UndoClearBatchLSN();

	ResetXactUndo();
}

/*
 * XactUndo_SubXactCallback
 *		Subtransaction callback to manage UNDO subtransaction state.
 *
 * This ensures the UNDO subsystem properly tracks all subtransactions,
 * including those created by ROLLBACK TO SAVEPOINT.
 *
 * The subtransaction state is stored in a dynamically-grown array in
 * TopMemoryContext.  In the common case (depth < capacity), no allocation
 * occurs.  Growth via repalloc is safe here because it operates on a
 * TopMemoryContext allocation, not a per-subtransaction context.
 */
static void
XactUndo_SubXactCallback(SubXactEvent event, SubTransactionId mySubid,
						 SubTransactionId parentSubid, void *arg)
{
	int			i;

	/* These parameters are mandated by the callback signature. */
	(void) parentSubid;
	(void) arg;

	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:

			/*
			 * Write out any deferred records before this subtransaction
			 * begins.
			 *
			 * A pending batch must never span a subtransaction boundary.  The
			 * records in it belong to the OUTER level, but the flush that
			 * will eventually write them stamps them all with one batch LSN;
			 * if that flush happened after this point, the outer level's
			 * records would land above the boundary we are about to save and
			 * a later ROLLBACK TO SAVEPOINT would revert them along with this
			 * subtransaction's own -- discarding work that was never rolled
			 * back.  (Concretely: savepoint s1, insert, release s1, savepoint
			 * s2, insert, rollback to s2 would also undo the s1 insert.)
			 *
			 * This must run BEFORE the boundary is saved below, so the
			 * flushed batch sits strictly below it.
			 */
			XactUndoFlushPending();

			/*
			 * A new subtransaction is starting.  Push an entry onto the
			 * dynamically-grown stack, extending it if needed.
			 */
			XactUndo.subxact_depth++;
			EnsureSubxactStackCapacity();
			{
				XactUndoSubTransactionState *s = CURRENT_SUBXACT();

				s->nestingLevel = mySubid;
				for (i = 0; i < NUndoPersistenceLevels; ++i)
				{
					s->start_location[i] = InvalidUndoRecPtr;
					/* Save parent's last_batch_lsn for restore on abort */
					s->last_batch_lsn[i] = XactUndo.last_batch_lsn[i];
					s->chain_bytes[i] = XactUndo.chain_bytes[i];
				}
			}
			break;

		case SUBXACT_EVENT_COMMIT_SUB:

			/*
			 * Subtransaction is committing. Merge its UNDO state into parent.
			 */
			AtSubCommit_XactUndo(mySubid);
			break;

		case SUBXACT_EVENT_ABORT_SUB:

			/*
			 * Subtransaction is aborting. Apply UNDO and clean up.
			 */
			AtSubAbort_XactUndo(mySubid);
			break;

		case SUBXACT_EVENT_PRE_COMMIT_SUB:
			/* Nothing to do at pre-commit */
			break;
	}
}

/*
 * EnsureSubxactStackCapacity
 *		Ensure the subxact_stack has room for the current depth.
 *
 * If the stack hasn't been allocated yet, allocate it with the initial
 * capacity.  If we've exceeded the current capacity, double it.
 * The stack lives in TopMemoryContext so it persists across transactions
 * within the same backend.
 */
static void
EnsureSubxactStackCapacity(void)
{
	if (XactUndo.subxact_stack == NULL)
	{
		/* First-time allocation */
		XactUndo.subxact_capacity = INITIAL_SUBXACT_CAPACITY;
		XactUndo.subxact_stack = (XactUndoSubTransactionState *)
			MemoryContextAllocZero(TopMemoryContext,
								   XactUndo.subxact_capacity *
								   sizeof(XactUndoSubTransactionState));
	}
	else if (XactUndo.subxact_depth >= XactUndo.subxact_capacity)
	{
		int			new_capacity = XactUndo.subxact_capacity * 2;

		XactUndo.subxact_stack = (XactUndoSubTransactionState *)
			repalloc(XactUndo.subxact_stack,
					 new_capacity * sizeof(XactUndoSubTransactionState));
		/* Zero the newly-allocated portion */
		memset(&XactUndo.subxact_stack[XactUndo.subxact_capacity], 0,
			   (new_capacity - XactUndo.subxact_capacity) *
			   sizeof(XactUndoSubTransactionState));
		XactUndo.subxact_capacity = new_capacity;
	}
}

/*
 * ResetXactUndo
 *		Reset all backend-private undo state for the next transaction.
 */
static void
ResetXactUndo(void)
{
	int			i;

	XactUndo.has_undo = false;
	XactUndo.needs_inline_abort = false;
	XactUndo.subxact_depth = 0;

	/*
	 * Drop any pending-batch reference.  Every caller of this function has
	 * already flushed (commit/abort/prepare choke points), so a non-NULL
	 * value here would mean records were lost; the record sets themselves are
	 * freed or reparented by the caller, so the pointer must not survive
	 * either way.
	 */
	XactUndo.pending_uset = NULL;

	/*
	 * The subxact_stack allocation persists across transactions (it's in
	 * TopMemoryContext).  We just reset the depth and initialize slot 0.
	 */
	if (XactUndo.subxact_stack != NULL)
	{
		XactUndo.subxact_stack[0].nestingLevel = 1;
		for (i = 0; i < NUndoPersistenceLevels; i++)
		{
			XactUndo.subxact_stack[0].start_location[i] = InvalidUndoRecPtr;
			XactUndo.subxact_stack[0].last_batch_lsn[i] = InvalidXLogRecPtr;
			XactUndo.subxact_stack[0].chain_bytes[i] = 0;
		}
	}

	for (i = 0; i < NUndoPersistenceLevels; i++)
	{
		XactUndo.record_set[i] = NULL;
		XactUndo.last_location[i] = InvalidUndoRecPtr;
		XactUndo.last_batch_lsn[i] = InvalidXLogRecPtr;
		XactUndo.chain_bytes[i] = 0;
		XactUndo.first_batch_lsn[i] = InvalidXLogRecPtr;
	}

	/*
	 * The chain is now the commit's, the abort's (applied, or handed to the
	 * ATM) or the 2PC state's; none of those needs the in-flight record.
	 */
	PublishXactUndoChain(InvalidTransactionId);
}

/*
 * CollapseXactUndoSubTransactions
 *		Collapse all subtransaction state into the top level.
 */
static void
CollapseXactUndoSubTransactions(void)
{
	while (XactUndo.subxact_depth > 0)
	{
		/*
		 * Merge current level into parent by calling AtSubCommit_XactUndo
		 * with the current level's nestingLevel.
		 */
		AtSubCommit_XactUndo(
							 XactUndo.subxact_stack[XactUndo.subxact_depth].nestingLevel);
	}
}
