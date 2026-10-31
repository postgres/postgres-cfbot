/*-------------------------------------------------------------------------
 *
 * flux_mvcc.c
 *	  FLUX heap-compatible xmin/xmax MVCC implementation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_mvcc.c
 *
 * NOTES
 *	  FLUX uses ordinary heap-compatible xmin/xmax MVCC visibility, the same
 *	  model as HeapTupleSatisfiesMVCC: a tuple is visible to a snapshot iff its
 *	  inserter (t_xmin) is committed-and-visible-to-the-snapshot AND its
 *	  deleter/updater (t_xmax) is invalid, not committed, or not visible.  CLOG
 *	  is the commit-status authority; the snapshot's xmin/xmax/xip decide
 *	  visibility of committed XIDs; subtransaction aborts (ROLLBACK TO
 *	  savepoint) are handled by stamping t_xmin/t_xmax with the current subxid
 *	  so CLOG marks a rolled-back subxact's tuples aborted (plus a transient
 *	  sLog ABORTED marker consulted for the still-in-progress window).
 *
 *	  In-place UPDATE keeps the NEWEST version on the page (new t_xmin) and
 *	  pushes the pre-update image to the UNDO log via t_verptr.
 *	  A snapshot that cannot see the updater's xmin reads the
 *	  old version back from the UNDO log with FluxReconstructVisibleVersion()
 *	  (flux_pvs.c), which walks t_verptr and stops at the version whose
 *	  producing xid is visible.  The single visibility function is
 *	  FluxTupleSatisfiesMVCC(), reached via FluxTupleVisibleToSnapshotDual()
 *	  from every read site; it and the chain walk decide "is this writer
 *	  visible?" through one rule, FluxWriterVisibleToSnapshot().
 *
 *	  The pre-commit "dirty read" window closes for free: a reader resolves
 *	  t_xmin against CLOG, which reports committed only at the durability point
 *	  (RecordTransactionCommit flushes the commit record).  A not-yet-committed
 *	  (hence not-yet-durable) inserter is simply invisible.
 *
 *	  The FLUX_TUPLE_UNCOMMITTED flag (0x0080) is set on insert/delete/update
 *	  and cleared as a hint at commit, for at most a bounded number of pages,
 *	  and lazily after it.  Nothing takes a set flag for a writer in progress
 *	  (see its definition in flux.h); it only lets the current transaction
 *	  tell its own uncommitted writes apart without a command id lookup.
 *
 * ISOLATION LEVEL SEMANTICS
 *
 *	  FLUX integrates with PostgreSQL's Serializable Snapshot Isolation
 *	  (SSI) infrastructure in predicate.c.  The scan path acquires SIREAD
 *	  predicate locks via PredicateLockTID(), and the DML paths (INSERT,
 *	  UPDATE, DELETE) call CheckForSerializableConflictIn() to detect
 *	  rw-antidependencies.  The FluxCheckForSerializableConflictOut()
 *	  function handles the reverse direction (reader encounters a tuple
 *	  written by a concurrent transaction) by looking up the writer's
 *	  XID via the sLog and delegating to predicate.c.
 *
 *	  BEFORE-IMAGE SERVING:
 *
 *	  In-place UPDATEs destroy the pre-image on the page.  The visible
 *	  prior version is reconstructed by walking the durable UNDO log
 *	  chain (FluxReconstructVisibleVersion), not from any shared sLog DSA
 *	  entry: readers whose snapshot cannot see the committing xid follow the
 *	  on-page head verptr into the UNDO log.  This restores correct snapshot
 *	  semantics for concurrent readers under REPEATABLE READ and SERIALIZABLE.
 *
 *	  CONCURRENCY:
 *
 *	  1. Same-tuple write-write conflicts serialize correctly: the
 *	     second writer blocks (via XactLockTableWait on the writer xid in
 *	     the tuple header) until the first commits or aborts.
 *
 *	  2. Write Skew (A5B) on disjoint tuples IS detected via predicate
 *	     locking (SIREAD locks on tuples read + conflict-in checks on
 *	     writes).
 *
 *	  In summary, FLUX's isolation guarantees are:
 *	  - READ COMMITTED: Correct (no dirty reads, each statement gets
 *	    fresh visibility via a per-statement snapshot)
 *	  - REPEATABLE READ: Full Snapshot Isolation; concurrent committed
 *	    UPDATEs are reconstructed from the UNDO log per reader snapshot
 *	  - SERIALIZABLE: Full SSI via predicate.c integration; write skew
 *	    and phantom anomalies are prevented through predicate locking
 *	    and rw-antidependency cycle detection
 *
 *	  References:
 *	  - Berenson et al., "A Critique of ANSI SQL Isolation Levels" (1995)
 *	  - Adya, "Weak Consistency: A Generalized Theory and Optimistic
 *	    Implementations for Distributed Transactions" (2000)
 *	  - Cahill et al., "Serializable Isolation for Snapshot Databases" (2009)
 *	  - Ports & Grittner, "Serializable Snapshot Isolation in PostgreSQL" (2012)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/atm.h"
#include "access/flux.h"
#include "access/flux_pbu.h"
#include "access/slog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "utils/injection_point.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "utils/memutils.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

/*
 * Total number of PGPROC slots, matching the allProcs array size in proc.c.
 * This must cover regular backends, auxiliary procs, and prepared transactions
 * since GetNumberFromPGProc() can return indices up to TotalProcs - 1.
 */
#define FLUX_TOTAL_PROCS \
	(MaxBackends + NUM_AUXILIARY_PROCS + max_prepared_xacts)

/*
 * Shared memory structures for MVCC
 */
typedef struct FluxMvccShmemData
{
	LWLock		mvcc_lock;		/* Protects serializable horizon only */
	uint64		oldest_active_ts;	/* Cached oldest active transaction ts;
									 * backs FluxGetOldestActiveTimestamp(),
									 * which still feeds the VM all-visible
									 * check.  NOT a vacuum horizon. */
	uint64		serializable_horizon;	/* Serializable isolation horizon */

	pg_atomic_uint32 oldest_active_generation;	/* Bumped when cache is
												 * invalidated */
	pg_atomic_uint32 active_xact_count; /* Number of active transactions
										 * (atomic) */

	/*
	 * Per-backend active transaction start timestamps.  Each backend slot
	 * stores the start timestamp of its current FLUX transaction, or 0 if
	 * idle.  This array is indexed by pgprocno (the offset into
	 * ProcGlobal->allProcs) and is sized to FLUX_TOTAL_PROCS so that
	 * auxiliary procs and prepared transactions are covered.
	 *
	 * Each slot is written only by its owning backend and read by VACUUM, so
	 * no lock is needed; just a compiler barrier via volatile access.
	 */
	int			num_xact_slots; /* Number of slots (== FLUX_TOTAL_PROCS) */
	uint64		xact_start_ts_slots[FLEXIBLE_ARRAY_MEMBER];

} FluxMvccShmemData;

static FluxMvccShmemData *FluxMvccShmem = NULL;


/*
 * Per-transaction MVCC state.
 *
 * SSI (Serializable Snapshot Isolation) conflict detection is now delegated
 * entirely to PostgreSQL's predicate.c infrastructure.  FLUX integrates
 * with it by calling PredicateLockTID() in the scan path and
 * CheckForSerializableConflictIn/Out() in the DML paths.  The private
 * rw-conflict graph that was previously here has been removed.
 *
 * Commit visibility uses CLOG (heap-shaped xmin/xmax); the timestamp fields
 * below are page-level bookkeeping only.
 */
struct FluxTransactionState
{
	uint64		xact_start_ts;	/* Transaction start timestamp */
	uint64		xact_commit_ts; /* Transaction commit timestamp */

	/*
	 * READ COMMITTED per-command read point.  Under RC the visibility read
	 * point is "now", refreshed each command so a scan sees concurrent
	 * commits.  Captured once per command (keyed by rc_read_point_cid) and
	 * reused for every tuple.
	 */
	CommandId	rc_read_point_cid;
	uint64		rc_read_point_xcc;	/* snapXactCompletionCount at capture */
	bool		is_serializable;	/* Serializable isolation level */
	bool		is_read_only;	/* Transaction has not performed writes */

	/*
	 * EPQ reconcile identity for the in-place write-write conflict path.
	 *
	 * When flux_tuple_update detects a committed concurrent UPDATE (head
	 * verptr's committer is invisible to snapshot) it returns TM_Updated and
	 * the executor runs EvalPlanQual: table_tuple_lock re-reads the latest
	 * on-page value (which, because FLUX updates in place, already reflects
	 * every commit so far) and the quals/SET expression recompute on top of
	 * it.  But EPQ re-evaluates against the same statement snapshot (same xid
	 * xip / curcid), so a naive probe would re-detect the same committed
	 * marker and livelock.  Heap escapes this because EPQ advances tupleid to
	 * a new tuple version with a clean xmax; FLUX has no new version to
	 * advance to.
	 *
	 * EPQ dedup: identity of the (verptr, xid) marker we bounced on to
	 * EvalPlanQual for this exact (relid, tid, cid).  If the next probe on
	 * the same tuple in the same EPQ retry finds an unchanged head verptr and
	 * xid, we already reconciled it -- skip to prevent livelock.  A
	 * strictly-newer committer stamps a NEW record with a fresh verptr, so
	 * dedup only suppresses re-firing on the identical marker.  No temporal
	 * ordering is required: this is a physical identity check.
	 */
	Oid			epq_reconcile_relid;
	ItemPointerData epq_reconcile_tid;
	CommandId	epq_reconcile_cid;
	UndoRecPtr	epq_reconcile_verptr;
	TransactionId epq_reconcile_xid;
};

/* Restart reasons */
#define FLUX_RESTART_NONE				0
#define FLUX_RESTART_UNCERTAINTY		1
#define FLUX_RESTART_SERIALIZABLE		2

/*
 * Per-backend static transaction state.  Using a static struct avoids
 * a palloc/pfree cycle per transaction.  The struct is reset at the start
 * of each transaction by FluxInitTransactionState().
 *
 * MyFluxXactState points to &MyFluxXactStateData when a transaction is
 * active, and is NULL between transactions.  This preserves the existing
 * NULL-check pattern throughout the codebase.
 */
static FluxTransactionState MyFluxXactStateData;
static FluxTransactionState *MyFluxXactState = NULL;

/*
 * GUC variables (flux_enable_serializable and flux_max_transactions removed;
 * SSI is unconditionally provided via predicate.c)
 */

/*
 * Function prototypes
 */
static void FluxInitTransactionState(void);
static void FluxCleanupTransactionState(void);
static void FluxShmemExit(int code, Datum arg);

/* FluxCheckUncommittedDelete removed -- logic inlined in visibility checks */


/*
 * Shared memory size calculation
 *
 * Per-transaction state (FluxTransactionState) is allocated in
 * backend-local TopTransactionContext, NOT in shared memory, so it
 * does not appear here.  The only shared-memory array is the
 * per-backend xact_start_ts_slots[], which scales naturally with
 * FLUX_TOTAL_PROCS (and therefore MaxBackends).  During bootstrap
 * MaxBackends is ~4, keeping this allocation tiny.
 */
Size
FluxMvccShmemSize(void)
{
	Size		size;

	/*
	 * Base struct (includes the flexible array header but not the array
	 * elements), plus one uint64 slot per PGPROC (regular backends, auxiliary
	 * procs, prepared transactions) for tracking active transaction start
	 * timestamps.
	 */
	size = offsetof(FluxMvccShmemData, xact_start_ts_slots);
	size = add_size(size, mul_size(FLUX_TOTAL_PROCS, sizeof(uint64)));

	return size;
}


/*
 * FluxGetDmlTimestamp -- return the transaction's start timestamp for DML
 * page-level bookkeeping.
 *
 * Within a single transaction, all DML operations (INSERT, UPDATE, DELETE)
 * stamp the page-level commit-ts word (FluxPageSetCommitTs) with the
 * transaction's start timestamp.
 *
 * This value is NOT a visibility timestamp.  Commit visibility comes from
 * CLOG via heap-shaped xmin/xmax.  Intra-transaction ordering (multiple DMLs
 * in the same txn) is handled by command id (t_cmin and the backend-local cid
 * map).
 */
uint64
FluxGetDmlTimestamp(void)
{
	/*
	 * Callers must have already called FluxGetTransactionTimestamp() or
	 * equivalent, which initializes the transaction state.  We assert rather
	 * than lazily initializing, keeping this function as lean as possible on
	 * the hot path.
	 */
	Assert(MyFluxXactState != NULL);

	return MyFluxXactState->xact_start_ts;
}

/*
 * FluxGetCommitTimestamp
 *
 * Return a wall-clock timestamp (microseconds since the PostgreSQL epoch)
 * used only for page-level commit-ts bookkeeping and the VM all-visible
 * hint.  Commit visibility comes from CLOG (heap-shaped xmin/xmax); nothing
 * reads this value for a correctness/visibility DECISION, so it does not
 * need strict cross-backend monotonicity.
 *
 * Historically this walked an atomic compare-and-swap loop over a single
 * shared global_commit_ts counter to keep timestamps strictly increasing.
 * That shared RMW was a hot-path serialization point (one cache line CAS'd
 * ~2x per write txn) with no surviving consumer of strict monotonicity, so
 * it has been replaced with a plain GetCurrentTimestamp() read: no shared
 * counter, no CAS, no spin.  Wall-clock microseconds are already
 * monotonic-enough for a "min over active start-stamps" horizon and a
 * ">= oldest" VM hint (both use non-strict comparisons).
 */
uint64
FluxGetCommitTimestamp(void)
{
	if (FluxMvccShmem == NULL)
		elog(ERROR, "FLUX MVCC not initialized");

	return (uint64) GetCurrentTimestamp();
}

/*
 * FluxGetTransactionTimestamp
 *
 * Return the start timestamp of the current transaction.  Initializes
 * per-transaction MVCC state on first call within a transaction.
 *
 * Returns the transaction's start timestamp (uint64).
 */
uint64
FluxGetTransactionTimestamp(void)
{
	if (MyFluxXactState == NULL)
		FluxInitTransactionState();

	return MyFluxXactState->xact_start_ts;
}

/*
 * Subsystem callback wrappers for PG_SHMEM_SUBSYSTEM infrastructure
 */
static void
FluxMvccShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "FLUX MVCC Data",
					   .size = FluxMvccShmemSize(),
					   .ptr = (void **) &FluxMvccShmem);
}

static void
FluxMvccShmemInit_cb(void *arg)
{
	int			total_procs = FLUX_TOTAL_PROCS;

	/* FluxMvccShmem is already set by the ShmemRequestStruct .ptr mechanism */
	Assert(FluxMvccShmem != NULL);

	/* Initialize shared memory fields */
	LWLockInitialize(&FluxMvccShmem->mvcc_lock, LWTRANCHE_BUFFER_MAPPING);
	FluxMvccShmem->oldest_active_ts = 1;
	pg_atomic_init_u32(&FluxMvccShmem->oldest_active_generation, 0);
	FluxMvccShmem->serializable_horizon = 1;
	pg_atomic_init_u32(&FluxMvccShmem->active_xact_count, 0);

	/* Initialize per-backend active timestamp slots to 0 (idle) */
	FluxMvccShmem->num_xact_slots = total_procs;
	memset(FluxMvccShmem->xact_start_ts_slots, 0,
		   total_procs * sizeof(uint64));

	/* Register cleanup function */
	on_shmem_exit(FluxShmemExit, 0);
}

const ShmemCallbacks FluxMvccShmemCallbacks = {
	.request_fn = FluxMvccShmemRequest,
	.init_fn = FluxMvccShmemInit_cb,
};

/*
 * Initialize per-transaction MVCC state
 *
 * The start timestamp is a plain wall-clock value from
 * FluxGetCommitTimestamp() held in the uint64 xact_start_ts field for
 * per-backend slot tracking.
 */
/*
 * Transaction callback for FLUX MVCC cleanup.
 *
 * This is registered once per backend via RegisterXactCallback.
 * On transaction commit or abort, it calls FluxCommitTransaction()
 * or FluxCleanupTransactionState() to reset MyFluxXactState,
 * ensuring the next transaction in this backend gets a fresh start
 * timestamp from FluxGetCommitTimestamp().
 */
static bool flux_xact_callback_registered = false;

static void
FluxXactCallback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			FluxCommitTransaction();
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			FluxCleanupTransactionState();
			break;

		case XACT_EVENT_PREPARE:
			/* Prepared xids pin the pruning horizon through their PGPROC. */
			FluxCleanupTransactionState();
			break;

		default:
			/* Pre-commit, pre-prepare -- nothing to do */
			break;
	}
}

static void
FluxInitTransactionState(void)
{
	if (MyFluxXactState != NULL)
		return;

	/* Register cleanup callback on first use in this backend */
	if (!flux_xact_callback_registered)
	{
		RegisterXactCallback(FluxXactCallback, NULL);
		flux_xact_callback_registered = true;
	}

	/* Use the static per-backend struct; zero it to start fresh */
	memset(&MyFluxXactStateData, 0, sizeof(FluxTransactionState));
	MyFluxXactState = &MyFluxXactStateData;

	/*
	 * Stamp the transaction start timestamp (monotonic wall-clock via
	 * FluxGetCommitTimestamp).  This is a within-transaction bookkeeping
	 * value only; commit visibility comes from CLOG (heap-shaped xmin/xmax).
	 */
	MyFluxXactState->xact_start_ts = FluxGetCommitTimestamp();

	MyFluxXactState->xact_commit_ts = 0;

	/* No RC per-command read point captured yet this transaction. */
	MyFluxXactState->rc_read_point_cid = InvalidCommandId;
	MyFluxXactState->rc_read_point_xcc = 0;
	MyFluxXactState->is_serializable = (XactIsoLevel == XACT_SERIALIZABLE);
	MyFluxXactState->is_read_only = true;	/* Until first write */

	/* Register in shared memory for oldest-active-timestamp tracking */
	if (FluxMvccShmem != NULL)
	{
		int			my_slot = MyProc ? (int) GetNumberFromPGProc(MyProc) : -1;

		/*
		 * Write our start timestamp into our per-backend slot.  This is a
		 * single-writer/multi-reader pattern (only we write our slot, VACUUM
		 * reads it), so no lock is needed; just a write barrier.
		 */
		if (my_slot >= 0 && my_slot < FluxMvccShmem->num_xact_slots)
		{
			pg_write_barrier();
			FluxMvccShmem->xact_start_ts_slots[my_slot] =
				MyFluxXactState->xact_start_ts;
		}

		pg_atomic_fetch_add_u32(&FluxMvccShmem->active_xact_count, 1);

		/*
		 * If our start timestamp is older than the cached oldest, invalidate
		 * the cache by bumping the generation counter.
		 */
		if (MyFluxXactState->xact_start_ts < FluxMvccShmem->oldest_active_ts)
			pg_atomic_fetch_add_u32(&FluxMvccShmem->oldest_active_generation, 1);
	}
}

/*
 * Cleanup per-transaction MVCC state
 */
static void
FluxCleanupTransactionState(void)
{
	if (MyFluxXactState == NULL)
		return;

	/*
	 * Clear our slot in shared memory.  No lock needed: each backend only
	 * writes its own slot, and the generation counter invalidates the cached
	 * oldest_active_ts when needed.
	 */
	if (FluxMvccShmem != NULL)
	{
		int			my_slot = MyProc ? (int) GetNumberFromPGProc(MyProc) : -1;
		uint64		my_ts = MyFluxXactState->xact_start_ts;

		/* Clear our per-backend slot */
		if (my_slot >= 0 && my_slot < FluxMvccShmem->num_xact_slots)
		{
			FluxMvccShmem->xact_start_ts_slots[my_slot] = 0;
			pg_write_barrier();
		}

		pg_atomic_fetch_sub_u32(&FluxMvccShmem->active_xact_count, 1);

		/*
		 * Invalidate the cached oldest_active_ts if we might have been the
		 * oldest.  Bump the generation counter so that
		 * FluxGetOldestActiveTimestamp() rescans on the next call. If no
		 * transactions remain, advance the cached value cheaply.
		 */
		if (pg_atomic_read_u32(&FluxMvccShmem->active_xact_count) == 0)
		{
			FluxMvccShmem->oldest_active_ts = (uint64) GetCurrentTimestamp();
			pg_atomic_fetch_add_u32(&FluxMvccShmem->oldest_active_generation, 1);
		}
		else if (my_ts == FluxMvccShmem->oldest_active_ts)
		{
			/*
			 * Only invalidate the cache when we were the actual oldest active
			 * transaction.  If my_ts < oldest_active_ts, the cached value was
			 * already advanced past us by another backend's rescan, so our
			 * departure cannot change the oldest.  Using strict equality
			 * instead of <= dramatically reduces invalidation frequency under
			 * high concurrency.
			 */
			pg_atomic_fetch_add_u32(&FluxMvccShmem->oldest_active_generation, 1);
		}
	}

	MyFluxXactState = NULL;
}

/*
 * SSI conflict detection is handled by PostgreSQL's predicate.c infrastructure
 * via CheckForSerializableConflictIn/Out calls in the DML and scan paths.
 * The FluxCheckSerializableConflict compatibility stub has been removed.
 */

/*
 * FluxSerializableConflictOutXid -- report a rw-conflict out to the writer xid
 * of a version a serializable reader examined.
 *
 * The tail of HeapCheckForSerializableConflictOut.  The writer either
 * superseded the version we read, or wrote one our snapshot cannot see.  A
 * writer that is still running counts, and so does one that committed after
 * our snapshot: SSI needs the edge either way.  Skip writers that precede
 * TransactionXmin (they cannot be concurrent, and SUBTRANS may no longer
 * hold them, so test that before looking them up), our own, and aborted ones
 * (heap's HEAPTUPLE_DEAD: an aborted writer's version never existed).
 *
 * The caller has checked CheckForSerializableConflictOutNeeded.
 */
void
FluxSerializableConflictOutXid(Relation relation, TransactionId xid,
							   Snapshot snapshot)
{
	if (!TransactionIdIsNormal(xid) ||
		TransactionIdPrecedes(xid, TransactionXmin) ||
		TransactionIdIsCurrentTransactionId(xid))
		return;

	/* In progress before committed, as heap: CLOG is set before procarray. */
	if (!TransactionIdIsInProgress(xid) && !TransactionIdDidCommit(xid))
		return;

	xid = SubTransGetTopmostTransaction(xid);
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return;

	CheckForSerializableConflictOut(relation, xid, snapshot);
}

/*
 * FluxCheckForSerializableConflictOut -- we are reading the version tuple of
 * a row.  If it is not visible, there may be a rw-conflict out with the
 * transaction that wrote it; if it is visible but has been removed, with the
 * one that removed it.
 *
 * This is HeapCheckForSerializableConflictOut on FLUX's header.  The writer
 * of an image is t_xmin: the inserter, or for an in-place updated image the
 * updater.  The remover is t_xmax, when the image is DELETED or UPDATED (the
 * same rule FluxTupleSatisfiesMVCC applies).  As in heap, a writer counts
 * whether it is still running or committed after our snapshot; the commit
 * status comes from CLOG, not from the wait-only flux_header_dirty_xid,
 * which sees running writers only.
 *
 * A FLUX image hides the versions before it in UNDO.  When a reader walks
 * back to one of them, the walk reports each writer it steps over
 * (FluxReconstructVisibleVersionEx); this function covers only the image it
 * is given.
 */
void
FluxCheckForSerializableConflictOut(bool visible, Relation relation,
									FluxTupleHeader *tuple,
									Buffer buffer,
									Snapshot snapshot)
{
	TransactionId xid;

	if (!CheckForSerializableConflictOutNeeded(relation, snapshot))
		return;

	if (visible)
	{
		/* We read a version; is someone superseding it? */
		if (!(tuple->t_flags & (FLUX_TUPLE_DELETED | FLUX_TUPLE_UPDATED)))
			return;
		xid = FluxTupleGetXmax(tuple);
	}
	else
		xid = FluxTupleGetXmin(tuple);

	FluxSerializableConflictOutXid(relation, xid, snapshot);
}

/*
 * Commit the current transaction and assign commit timestamp.
 *
 * The commit timestamp is a monotonic wall-clock value (FluxGetCommitTimestamp)
 * used only for page-level bookkeeping; commit visibility comes from CLOG
 * (heap-shaped xmin/xmax MVCC).
 */
void
FluxCommitTransaction(void)
{
	if (MyFluxXactState == NULL)
		return;

	MyFluxXactState->xact_commit_ts = FluxGetCommitTimestamp();

	/* Update serializable horizon (only for serializable transactions) */
	if (FluxMvccShmem != NULL && MyFluxXactState->is_serializable)
	{
		LWLockAcquire(&FluxMvccShmem->mvcc_lock, LW_EXCLUSIVE);
		FluxMvccShmem->serializable_horizon =
			Min(FluxMvccShmem->serializable_horizon,
				MyFluxXactState->xact_commit_ts);
		LWLockRelease(&FluxMvccShmem->mvcc_lock);
	}

	FluxCleanupTransactionState();
}

/*
 * Abort the current transaction
 */
void
FluxAbortTransaction(void)
{
	if (MyFluxXactState == NULL)
		return;

	FluxCleanupTransactionState();
}

/*
 * Get snapshot timestamp for reads
 */
uint64
FluxGetSnapshotTimestamp(Snapshot snapshot)
{
	if (IsMVCCSnapshot(snapshot))
	{
		if (MyFluxXactState == NULL)
			FluxInitTransactionState();

		/*
		 * REPEATABLE READ / SERIALIZABLE: return transaction-start timestamp
		 * for a consistent point-in-time snapshot across all statements.
		 */
		if (IsolationUsesXactSnapshot())
			return MyFluxXactState->xact_start_ts;

		/*
		 * READ COMMITTED: return current timestamp so each visibility check
		 * sees the latest committed state.
		 */
		return (uint64) FluxGetCommitTimestamp();
	}
	else
	{
		/* SnapshotAny or other non-MVCC snapshots */
		return 0;
	}
}

/*
 * Invalidate the cached oldest active timestamp, forcing the next call
 * to FluxGetOldestActiveTimestamp() to rescan all per-backend slots.
 *
 * Also callable from VACUUM or any code that needs to force a refresh.
 */
void
FluxUpdateOldestActiveTimestamp(void)
{
	if (FluxMvccShmem == NULL)
		return;

	pg_atomic_fetch_add_u32(&FluxMvccShmem->oldest_active_generation, 1);
}

/*
 * Per-backend cache of the oldest-active-timestamp computation.
 * Avoids rescanning all per-backend slots on every call; only rescans
 * when the global generation counter has been bumped.
 */
static uint32 my_oldest_active_gen = 0;
static uint64 my_oldest_active_cached = 0;

/*
 * FluxGetOldestActiveTimestamp -- return the oldest active transaction's
 * start timestamp.
 *
 * This is the FLUX analog of PostgreSQL's GetOldestNonRemovableTransactionId.
 * VACUUM uses this to determine which deleted tuples can be safely removed:
 * a deleted tuple whose commit timestamp is older than this value is no
 * longer visible to any running transaction and can be reclaimed.
 *
 * If no transactions are active, returns the current global commit timestamp,
 * meaning all committed deletions are eligible for cleanup.
 *
 * Uses a per-backend cache that is invalidated when the global generation
 * counter changes.  No LWLock acquisition needed in the common case.
 */
uint64
FluxGetOldestActiveTimestamp(void)
{
	uint32		current_gen;

	if (FluxMvccShmem == NULL)
		elog(ERROR, "FLUX MVCC not initialized");

	/* Fast path: check if our cached value is still valid */
	current_gen = pg_atomic_read_u32(&FluxMvccShmem->oldest_active_generation);
	if (current_gen == my_oldest_active_gen && my_oldest_active_cached != 0)
		return my_oldest_active_cached;

	/* Slow path: rescan all per-backend slots (lockless) */
	{
		uint64		oldest = 0;
		int			i;

		pg_read_barrier();

		for (i = 0; i < FluxMvccShmem->num_xact_slots; i++)
		{
			uint64		ts = FluxMvccShmem->xact_start_ts_slots[i];

			if (ts != 0 && (oldest == 0 || ts < oldest))
				oldest = ts;
		}

		if (oldest == 0)
			oldest = (uint64) GetCurrentTimestamp();

		/* Update the shared cached value (benign race with other backends) */
		FluxMvccShmem->oldest_active_ts = oldest;

		/* Cache locally */
		my_oldest_active_cached = oldest;
		my_oldest_active_gen = current_gen;

		return oldest;
	}
}

/*
 * Get MVCC statistics
 */
void
FluxGetMvccStats(uint64 *current_ts, uint64 *oldest_ts, int *active_xacts)
{
	if (FluxMvccShmem == NULL)
	{
		*current_ts = 0;
		*oldest_ts = 0;
		*active_xacts = 0;
		return;
	}

	*current_ts = (uint64) GetCurrentTimestamp();
	*oldest_ts = FluxMvccShmem->oldest_active_ts;
	*active_xacts = (int) pg_atomic_read_u32(&FluxMvccShmem->active_xact_count);
}

/*
 * Shared memory exit cleanup
 */
static void
FluxShmemExit(int code, Datum arg)
{
	FluxCleanupTransactionState();
}


/*
 * FluxTupleHasCommittedUpdateAfter -- fork-driven lost-update conflict probe.
 *
 * A FLUX in-place UPDATE stamps a verptr on the new on-page image
 * pointing at the UNDO record it just wrote.
 * That verptr is the head of the tuple's version chain: the record it
 * refers to describes the update that PRODUCED the current on-page image,
 * so its urec_xid IS the last committer of this tuple.  Older commits have
 * already been absorbed into the on-page bytes we are about to overwrite;
 * they are not lost-update candidates.  A single-step probe is therefore
 * sufficient.
 *
 * Conflict iff:
 *   1) the head verptr resolves (FluxPbuFetchXid returns true), and
 *   2) urec_xid is invisible to snapshot (XidInMVCCSnapshot returns true),
 *      and
 *   3) urec_xid is not our own xid, and
 *   4) urec_xid did commit (guarding against in-progress/aborted xids that
 *      the writer has yet to reach the wait path for).
 *
 * If FluxPbuFetchXid returns false the record was discarded.  Undo discard
 * removes only the records of transactions older than the oldest xmin, so
 * the committer then precedes every live snapshot's xmin: it is visible to
 * every reader and cannot be a lost-update candidate.  Safe terminator.
 *
 * *out_head_verptr and *out_head_xid receive the observed (verptr, xid)
 * identity so the caller can pass them to FluxEpqReconcileMark before
 * returning TM_Updated, and to FluxEpqReconcileMatches to skip a
 * previously-bounced marker.
 */
bool
FluxTupleHasCommittedUpdateAfter(Relation rel,
								 const FluxTupleHeader *tuple,
								 Size tuple_len,
								 Snapshot snapshot,
								 TransactionId exclude_xid,
								 UndoRecPtr *out_head_verptr,
								 TransactionId *out_head_xid,
								 bool *out_inprogress)
{
	UndoRecPtr	head;
	TransactionId head_xid;

	if (out_head_verptr != NULL)
		*out_head_verptr = InvalidUndoRecPtr;
	if (out_head_xid != NULL)
		*out_head_xid = InvalidTransactionId;
	if (out_inprogress != NULL)
		*out_inprogress = false;

	if (snapshot == NULL || !IsMVCCSnapshot(snapshot))
		return false;

	head = FluxTupleGetVersionPtr(tuple);
	if (!UndoRecPtrIsValid(head))
		return false;			/* never updated -- no committed conflict */

	if (!FluxPbuFetchXid(head, &head_xid))
		return false;			/* discarded -> visible to every snapshot */

	if (out_head_verptr != NULL)
		*out_head_verptr = head;
	if (out_head_xid != NULL)
		*out_head_xid = head_xid;

	/*
	 * Our own update is never a lost-update conflict.  Use
	 * TransactionIdIsCurrentTransactionId, which recognizes the current
	 * transaction's TOP xid AND every one of its subtransaction xids: the
	 * version-chain head may have been stamped by an earlier command at a
	 * different subtransaction nesting level (e.g. an UPDATE before a
	 * SAVEPOINT, then a re-UPDATE of the same row inside the savepoint), so a
	 * plain equality test against a single captured exclude_xid can miss it
	 * and wrongly drive a self-conflict -- which then self-waits in
	 * XactLockTableWait and trips its no-self-wait assertion.  The
	 * exclude_xid parameter is kept for callers that want to exclude a
	 * specific xid, but the current-transaction test below is the
	 * authoritative self-check.
	 */
	if (TransactionIdIsCurrentTransactionId(head_xid))
		return false;			/* our own update, not a conflict */

	if (TransactionIdIsValid(exclude_xid) &&
		TransactionIdEquals(head_xid, exclude_xid))
		return false;			/* our own update, not a conflict */

	if (!TransactionIdIsValid(head_xid))
		return false;

	/*
	 * XidInMVCCSnapshot(xid, snap) returns true iff the xid is NOT visible to
	 * snap -- i.e. in-progress or later than the snapshot's xmax.  A conflict
	 * requires that the head committer be invisible to the writer's snapshot
	 * AND actually committed (an in-progress or aborted xid is handled by the
	 * writer's dirty-xid / abort path, not here).
	 */
	if (!XidInMVCCSnapshot(head_xid, snapshot))
		return false;

	/*
	 * The head committer is invisible to our snapshot and is not our own xid.
	 * Normally it must have committed to be a lost-update conflict.  But
	 * there is a commit-visibility window: the committer's PRE_COMMIT
	 * callback removes its in-progress sLog marker and clears the on-page
	 * UNCOMMITTED flag BEFORE RecordTransactionCommit() marks CLOG and BEFORE
	 * ProcArrayEndTransaction() clears it from the running list.  In that
	 * window TransactionIdDidCommit() is still false while
	 * TransactionIdIsInProgress() is still true, and the sLog marker the
	 * writer-wait path keys off is already gone.  If we returned "no
	 * conflict" here, a concurrent updater would clobber the just-committed
	 * value with no 40001 / EPQ -- a lost update.  Signal the caller to wait
	 * on the in-flight head_xid (heap semantics: see live modifier -> wait ->
	 * re-check CLOG) rather than treating not-yet-committed as no-conflict.
	 * recovery/t/160.
	 */

	/*
	 * Ask the proc array before CLOG, as heap's visibility code does (see the
	 * header comment of heapam_visibility.c).  A transaction is marked
	 * committed in CLOG before it leaves the proc array, so asking CLOG first
	 * could see it as not committed and then, once it has done both, as not
	 * in progress: neither, and its update would be lost.  In the order below
	 * a writer that is not in progress has already set its CLOG state.
	 */
	if (TransactionIdIsInProgress(head_xid))
	{
		if (out_inprogress != NULL)
			*out_inprogress = true;
		return false;
	}

	/* Tests stop here, between the two questions about head_xid. */
	INJECTION_POINT("flux-head-commit-state", NULL);

	/* Not running: committed (a conflict), or aborted or crashed (not). */
	return TransactionIdDidCommit(head_xid);
}

/*
 * FluxEpqReconcileMatches -- true iff we already bounced this (relid, tid,
 * cid) to EvalPlanQual in the current command.
 *
 * IMPORTANT (EPQ termination under contention): we deliberately match on
 * (relid, tid, cid) ALONE and ignore head_verptr/head_xid.  Matching on the
 * head marker's physical identity caused an unbounded EPQ livelock on a hot
 * row (e.g. TPC-C warehouse/district under high concurrency): every retry
 * observed a DIFFERENT, newer committer (fresh head_verptr/xid), so the
 * marker never matched, EPQ re-fired forever, and each cycle leaked into
 * TopTransactionContext until OOM.  Heap does not re-loop when a still-newer
 * committer appears mid-EPQ -- it re-evaluates against the latest version
 * once and then applies.  Matching on (relid, tid, cid) reproduces that: we
 * bounce to EPQ at most once per tuple per command, then proceed with the
 * update against whatever the current version is.
 */
bool
FluxEpqReconcileMatches(Snapshot snapshot, Oid relid, ItemPointer tid)
{
	return MyFluxXactState != NULL &&
		TransactionIdIsValid(MyFluxXactState->epq_reconcile_xid) &&
		MyFluxXactState->epq_reconcile_relid == relid &&
		MyFluxXactState->epq_reconcile_cid == snapshot->curcid &&
		ItemPointerEquals(&MyFluxXactState->epq_reconcile_tid, tid);
}

/*
 * FluxEpqReconcileMark -- record the (verptr, xid) marker identity we are
 * bouncing to EvalPlanQual so the retry does not re-report it.
 */
void
FluxEpqReconcileMark(Snapshot snapshot, Oid relid, ItemPointer tid,
					 UndoRecPtr head_verptr, TransactionId head_xid)
{
	if (MyFluxXactState == NULL)
		FluxInitTransactionState();

	MyFluxXactState->epq_reconcile_relid = relid;
	ItemPointerCopy(tid, &MyFluxXactState->epq_reconcile_tid);
	MyFluxXactState->epq_reconcile_cid = snapshot->curcid;
	MyFluxXactState->epq_reconcile_verptr = head_verptr;
	MyFluxXactState->epq_reconcile_xid = head_xid;
}

/*
 * FluxTupleSatisfiesMVCC -- heap-compatible xmin/xmax visibility check.
 *
 * This is the single visibility function for FLUX's heap-shaped MVCC model.
 * It mirrors HeapTupleSatisfiesMVCC: a tuple is visible to `snapshot` iff its
 * inserter (t_xmin) is committed-and-visible-to-the-snapshot AND its
 * deleter/updater (t_xmax) is invalid, not committed, or not visible to the
 * snapshot.  CLOG is the authoritative commit-status oracle; the snapshot's
 * xmin/xmax/xip decide visibility of committed XIDs.  The sLog is consulted
 * ONLY for the command-id (cid) of the current transaction's own uncommitted
 * work, which FLUX stores there instead of in the tuple header.
 *
 * The pre-commit "dirty read" window closes for free here: a reader resolves
 * t_xmin against CLOG, and CLOG marks a transaction committed only at the
 * durability point (RecordTransactionCommit flushes the commit record before
 * ProcArrayEndTransaction / TransactionIdDidCommit reports true for a
 * crash-safe reader path).  A not-yet-committed inserter -- which includes a
 * not-yet-durable one -- is simply invisible.
 *
 * `curcid` is the reader's command id for MVCC snapshots (InvalidCommandId
 * for SELF/ANY, which see all of the current transaction's work).
 */

/*
 * FluxSetHintBits -- cache a CLOG commit result on the tuple as a hint bit.
 *
 * Mirrors heap's SetHintBits: once TransactionIdDidCommit() has confirmed a
 * tuple's xmin (or xmax) committed, record it in t_flags so subsequent
 * visibility checks skip the CLOG SLRU lookup entirely.  This is the
 * optimization that keeps a hot-set scan off the CLOG buffer LWLock (without
 * it, every tuple examined by every scan did a TransactionIdDidCommit -> CLOG
 * lookup, serializing on the CLOG buffer lock).  Non-WAL hint write: the
 * flag is reconstructible from CLOG, so a torn/lost hint is harmless.
 *
 * done says that the hint is about the version's last writer, so that the
 * UNCOMMITTED hint goes too (see flux.h); it is false for an inserter's
 * commit under a delete mark, whose deleter may still be running.
 */
static inline void
FluxSetHintBits(FluxTupleHeader *tuple, Buffer buffer, uint16 hint, bool done)
{
	/*
	 * Cache the CLOG result as a hint bit, but ONLY when we hold the buffer
	 * content lock: BufferSetHintBits16 (like MarkBufferDirtyHint) errors on
	 * an unlocked buffer, and FluxTupleSatisfiesMVCC is reached from some
	 * paths (post-VACUUM reads, replication reads, detached-tuple checks)
	 * where the buffer is pinned but not lock-held.  If we can't safely set
	 * the shared hint, just skip it -- the result is still correct, the CLOG
	 * lookup simply isn't cached on this call; the next scan holding the lock
	 * will cache it. Setting the in-memory bit without the lock would race a
	 * concurrent writer's t_flags update, so we do nothing rather than a bare
	 * |=.
	 */
	if (BufferIsValid(buffer) && BufferIsLockedByMe(buffer))
		BufferSetHintBits16(&tuple->t_flags,
							(tuple->t_flags | hint) &
							~(done ? FLUX_TUPLE_UNCOMMITTED : 0), buffer);
}

/*
 * FluxWriterVisibleToSnapshot -- is a version written by another
 * transaction's xid visible to an MVCC snapshot?
 *
 * This is the single "is this writer visible?" rule for every FLUX
 * visibility path: the on-page xmin/xmax checks in FluxTupleSatisfiesMVCC
 * and each step of the UNDO version-chain walk in
 * FluxReconstructVisibleVersion.  The caller handles the current
 * transaction's own xids (those go by command id).
 *
 * A writer is visible only if it is not in the snapshot's in-progress set
 * AND it committed.  "Not in the snapshot" is not enough on its own.  A
 * writer that finished before the snapshot without committing is aborted,
 * and its in-place images can still be on the page: a rollback handed to an
 * undo apply worker, and every crash loser, is reverted only later.  Such a
 * writer must be treated as invisible so that readers walk past its versions
 * to the last committed one.  (OrioleDB gets the same property by marking an
 * aborted transaction's CSN ABORTED before its undo is applied.)
 *
 * known_committed lets a caller that holds a CLOG hint bit for xid skip the
 * CLOG lookup.
 */
bool
FluxWriterVisibleToSnapshot(TransactionId xid, Snapshot snapshot,
							bool known_committed)
{
	if (TransactionIdEquals(xid, FrozenTransactionId))
		return true;
	if (XidInMVCCSnapshot(xid, snapshot))
		return false;
	return known_committed || TransactionIdDidCommit(xid);
}

/*
 * Is a version whose writer (t_xmin) is the current transaction visible to
 * command curcid?  Shared by FluxTupleSatisfiesMVCC and
 * FluxTupleSatisfiesLatest.
 */
static bool
flux_own_xmin_visible(FluxTupleHeader *tuple, Oid relid, CommandId curcid,
					  Buffer buffer, ItemPointer tid)
{
	/*
	 * Written by our own transaction (t_xmin is the writing subtransaction's
	 * xid, so a version a rolled-back savepoint wrote is not current and
	 * never gets here).  Visible unless a later command wrote it.  A
	 * committed self-insert from an earlier command is unconditionally
	 * visible.
	 */
	if (tuple->t_flags & FLUX_TUPLE_UNCOMMITTED)
	{
		/*
		 * Our own LATER command wrote this image: not visible to the current
		 * command.  This is heap's `HeapTupleHeaderGetCmin(tuple) >=
		 * snapshot->curcid` rule, and it is what stops a statement from
		 * seeing the rows it is itself producing.  Without it FLUX's
		 * out-of-place UPDATE (delete + insert at a new TID) let the same
		 * scan re-find its own output and update it twice -- the Halloween
		 * problem.  The plain INSERT path writes no per-tuple sLog entry
		 * (per-page conflict tracking), so the cid must come off the tuple
		 * header.
		 *
		 * Safe because t_cmin always describes the same writer as t_xmin, and
		 * we have just established that t_xmin is our own transaction: every
		 * path that re-stamps t_xmin also re-stamps t_cmin.
		 */
		if (curcid != InvalidCommandId &&
			tuple->t_cmin != InvalidCommandId &&
			tuple->t_cmin >= curcid)
			return false;
	}
	return true;
}

static bool
FluxTupleSatisfiesMVCC(FluxTupleHeader *tuple, Snapshot snapshot,
					   Oid relid, CommandId curcid, Buffer buffer,
					   ItemPointer tid)
{
	TransactionId xmin;
	TransactionId xmax;
	bool		xmin_hint;
	bool		xmax_hint;

	if (tuple == NULL)
		return false;

	xmin = FluxTupleGetXmin(tuple);
	xmax = FluxTupleGetXmax(tuple);

	/*
	 * SnapshotAny / non-MVCC "see everything": show every non-deleted tuple.
	 * A committed-and-visible xmax still hides it (the row is gone), but for
	 * SnapshotAny the caller wants raw existence, so only the DELETED flag
	 * matters.  Callers that need SNAPSHOT_DIRTY semantics handle that
	 * separately in flux_handler.c before reaching here.
	 */
	if (!IsMVCCSnapshot(snapshot))
		return !(tuple->t_flags & FLUX_TUPLE_DELETED) ||
			!TransactionIdIsValid(xmax);

	/* ---- xmin side: is the inserter committed-and-visible? ---- */
	if (!TransactionIdIsValid(xmin))
		return false;			/* no inserter -- cannot be visible */

	if (TransactionIdIsCurrentTransactionId(xmin))
	{
		if (!flux_own_xmin_visible(tuple, relid, curcid, buffer, tid))
			return false;
		/* our insert, visible so far -- fall through to xmax check */
	}
	else
	{
		xmin_hint = (tuple->t_flags & FLUX_TUPLE_XMIN_COMMITTED) != 0;

		/* inserter in progress, aborted, or not yet visible to snapshot */
		if (!FluxWriterVisibleToSnapshot(xmin, snapshot, xmin_hint))
			return false;

		/*
		 * Inserter committed.  Cache it as a hint bit (like heap's
		 * HEAP_XMIN_COMMITTED) so subsequent visibility checks skip the CLOG
		 * SLRU lookup -- this is what keeps a hot-set scan off the CLOG
		 * buffer LWLock.  Non-WAL hint write via MarkBufferDirtyHint.
		 */
		if (!xmin_hint)
			FluxSetHintBits(tuple, buffer, FLUX_TUPLE_XMIN_COMMITTED,
							!(tuple->t_flags & FLUX_TUPLE_DELETED));
	}

	/* Inserter is committed-and-visible.  Now the xmax (deleter) side. */

	if (!TransactionIdIsValid(xmax) ||
		!(tuple->t_flags & (FLUX_TUPLE_DELETED | FLUX_TUPLE_UPDATED)))
	{
		/*
		 * No deleter, or the tuple carries no delete/supersede marker: the
		 * on-page image is live for this snapshot.
		 *
		 * NOTE (in-place UPDATE): when this tuple was updated in place, the
		 * on-page image is the NEWEST version and its xmin is the updater. If
		 * that updater is invisible to `snapshot`, the xmin check above
		 * already returned false, and the caller (flux_handler.c)
		 * reconstructs the older visible version from the UNDO log via
		 * FluxReconstructVisibleVersion().  So an old snapshot never sees the
		 * wrong (too-new) bytes: it is either hidden here and reconstructed,
		 * or the updater is visible and the new image is correct.
		 */
		return true;
	}

	/* There is a deleter/updater xmax; resolve it heap-style. */
	if (TransactionIdIsCurrentTransactionId(xmax))
	{
		/*
		 * Deleted/superseded by our own transaction.  Invisible to us unless
		 * the delete happened in a later command (then we still see the row):
		 * heap's HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid.
		 */
		if (curcid != InvalidCommandId &&
			(tuple->t_flags & FLUX_TUPLE_UNCOMMITTED))
		{
			CommandId	own_cmax = FluxTupleOwnCmax(tuple);

			if (own_cmax != InvalidCommandId && own_cmax >= curcid)
				return true;	/* deleted after our scan started */
		}
		return false;			/* our own delete, visible to this command */
	}

	xmax_hint = (tuple->t_flags & FLUX_TUPLE_XMAX_COMMITTED) != 0;

	/* deleter in progress, aborted, or not yet visible -- row still here */
	if (!FluxWriterVisibleToSnapshot(xmax, snapshot, xmax_hint))
		return true;

	/* deleter committed-and-visible -- gone.  Cache the CLOG result. */
	if (!xmax_hint)
		FluxSetHintBits(tuple, buffer, FLUX_TUPLE_XMAX_COMMITTED, true);
	return false;
}

/*
 * FluxTupleVisibleToSnapshotDual -- primary visibility entry point.
 *
 * Forwards to FluxTupleSatisfiesMVCC.
 */
bool
FluxTupleVisibleToSnapshotDual(FluxTupleHeader *tuple, Snapshot snapshot,
							   Oid relid, Buffer buffer, ItemPointer tid)
{
	/*
	 * Only apply CID filtering for MVCC snapshots.  SNAPSHOT_SELF and
	 * SNAPSHOT_ANY must see all of the current transaction's work.
	 */
	return FluxTupleSatisfiesMVCC(tuple, snapshot, relid,
								  (snapshot != NULL &&
								   snapshot->snapshot_type == SNAPSHOT_MVCC)
								  ? snapshot->curcid : InvalidCommandId,
								  buffer, tid);
}

/*
 * FluxTupleSatisfiesLatest -- may a caller without a snapshot lock the version
 * on the page?
 *
 * The NULL-snapshot counterpart of FluxTupleVisibleToSnapshotDual for
 * flux_tuple_lock.  Heap's tuple_lock never looks at the snapshot: it decides
 * with HeapTupleSatisfiesUpdate, against CLOG and the current command.  FLUX
 * keeps a row's latest version at its TID, so with no snapshot the only
 * question is whether that version's writer is committed, or is the current
 * transaction in a command before curcid.  An in-progress or aborted writer,
 * or our own write in curcid or later, makes it false: the caller then waits
 * for the writer or reports TM_Invisible, as heap does.  The delete mark is
 * the caller's to test, as xmax is in heap_lock_tuple.
 */
bool
FluxTupleSatisfiesLatest(FluxTupleHeader *tuple, Oid relid, CommandId curcid,
						 Buffer buffer, ItemPointer tid)
{
	TransactionId xmin = FluxTupleGetXmin(tuple);

	if (!TransactionIdIsValid(xmin))
		return false;
	if (TransactionIdIsCurrentTransactionId(xmin))
		return flux_own_xmin_visible(tuple, relid, curcid, buffer, tid);
	return (tuple->t_flags & FLUX_TUPLE_XMIN_COMMITTED) != 0 ||
		TransactionIdDidCommit(xmin);
}

/*
 * FluxGetOldestXminHorizon -- XID retention horizon for VACUUM/prune.
 *
 * A committed-deleted tuple whose deleter (t_xmax) precedes this horizon is
 * invisible to every current and future snapshot and can be physically
 * removed.  Uses the standard non-removable-transaction horizon so it tracks
 * the cluster's oldest snapshot xmin exactly like heap VACUUM.
 */
TransactionId
FluxGetOldestXminHorizon(Relation rel)
{
	return GetOldestNonRemovableTransactionId(rel);
}

/*
 * FluxTupleAdvanceConflictHorizon -- advance *horizon past a tuple that is
 * about to be removed because FluxTupleDeadToAll said so.
 *
 * The FLUX counterpart of HeapTupleHeaderAdvanceConflictHorizon.  A committed
 * DELETE was visible to every snapshot that does not see its deleter
 * (t_xmax) as committed, so a hot standby must cancel those snapshots before
 * it replays the removal.  An aborted INSERT was never visible to anyone and
 * does not advance the horizon, nor does a row deleted by its own inserter.
 */
void
FluxTupleAdvanceConflictHorizon(FluxTupleHeader *tuple, TransactionId *horizon)
{
	TransactionId xmax = FluxTupleGetXmax(tuple);

	if ((tuple->t_flags & FLUX_TUPLE_DELETED) &&
		TransactionIdIsNormal(xmax) &&
		!TransactionIdEquals(xmax, FluxTupleGetXmin(tuple)) &&
		(!TransactionIdIsValid(*horizon) ||
		 TransactionIdFollows(xmax, *horizon)))
		*horizon = xmax;
}

/*
 * FluxTupleDeadToAll -- true iff a tuple can be reclaimed.
 *
 * Heap-shaped, two cases:
 *
 * - A committed DELETE (DELETED set) whose deleter (t_xmax) committed before
 *   the oldest-xmin horizon, so no active or future snapshot can see it.
 *   CLOG says so, not the UNCOMMITTED hint, which a committed delete can
 *   still carry (a released savepoint's, COMMIT PREPARED's, one restored by
 *   redo, or one past the commit-time bound; see flux.h).
 *
 * - An aborted INSERT: the inserter (t_xmin) did not commit, is not running,
 *   and the tuple has no version history (t_verptr invalid).  INSERT writes
 *   no UNDO, so nothing else ever marks such a row dead; heap reclaims it
 *   the same way (HEAPTUPLE_DEAD for an aborted xmin).  A tuple with a valid
 *   t_verptr is an in-place UPDATE or a DELETE whose writer is t_xmin or
 *   t_xmax: if that writer aborted, the row is still alive at its
 *   before-image and must be reverted, not removed.
 */
bool
FluxTupleDeadToAll(FluxTupleHeader *tuple, TransactionId oldest_xmin)
{
	TransactionId xmax;

	if (!UndoRecPtrIsValid(tuple->t_verptr) &&
		!(tuple->t_flags & FLUX_TUPLE_XMIN_COMMITTED))
	{
		TransactionId xmin = FluxTupleGetXmin(tuple);

		if (TransactionIdIsNormal(xmin) &&
			TransactionIdPrecedes(xmin, oldest_xmin) &&
			!TransactionIdIsInProgress(xmin) &&
			!TransactionIdDidCommit(xmin))
			return true;
	}

	if (!(tuple->t_flags & FLUX_TUPLE_DELETED))
		return false;

	xmax = FluxTupleGetXmax(tuple);
	if (!TransactionIdIsValid(xmax))
		return false;
	if (!(tuple->t_flags & FLUX_TUPLE_XMAX_COMMITTED) &&
		!TransactionIdDidCommit(xmax))
		return false;			/* delete aborted / in flight: keep */

	return TransactionIdPrecedes(xmax, oldest_xmin);
}

/*
 * FluxTupleStatsCount -- how VACUUM and ANALYZE count a tuple in the
 * reltuples and live/dead numbers they report, as heap counts by
 * HeapTupleSatisfiesVacuum.
 *
 * A row that another transaction is inserting is not counted, and one that
 * another transaction is deleting is live: that transaction reports the
 * insert or delete to the cumulative statistics when it ends, so counting it
 * here too would count it twice.  A row whose deleter committed, or is the
 * current transaction, is dead; one whose deleter aborted is live (its revert
 * is pending).  An aborted INSERT's row is dead.  An in-place UPDATE keeps
 * one row whatever its writer's state, so a row with UNDO history is live.
 */
FluxStatsCount
FluxTupleStatsCount(FluxTupleHeader *tuple)
{
	if (tuple->t_flags & FLUX_TUPLE_DELETED)
	{
		TransactionId xmax = FluxTupleGetXmax(tuple);

		if (tuple->t_flags & FLUX_TUPLE_XMAX_COMMITTED ||
			!TransactionIdIsNormal(xmax) ||
			TransactionIdIsCurrentTransactionId(xmax))
			return FLUX_COUNT_DEAD;
		if (TransactionIdIsInProgress(xmax))
			return FLUX_COUNT_LIVE;
		return TransactionIdDidCommit(xmax) ? FLUX_COUNT_DEAD : FLUX_COUNT_LIVE;
	}

	if (!UndoRecPtrIsValid(tuple->t_verptr) &&
		!(tuple->t_flags & FLUX_TUPLE_XMIN_COMMITTED))
	{
		TransactionId xmin = FluxTupleGetXmin(tuple);

		if (TransactionIdIsNormal(xmin) &&
			!TransactionIdIsCurrentTransactionId(xmin))
		{
			if (TransactionIdIsInProgress(xmin))
				return FLUX_COUNT_NONE;
			if (!TransactionIdDidCommit(xmin))
				return FLUX_COUNT_DEAD;
		}
	}

	return FLUX_COUNT_LIVE;
}
