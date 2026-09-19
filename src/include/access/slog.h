/*-------------------------------------------------------------------------
 *
 * slog.h
 *	  Secondary Log (sLog) for shared-memory tracking
 *
 * The sLog provides a shared-memory Aborted Transaction Map (ATM) for the
 * UNDO subsystem's Constant-Time Recovery:
 *
 *   Transaction radix tree - Aborted transaction entries keyed by
 *   (xid, reloid) packed into a uint64, ordered for efficient xid-based
 *   range operations.  Protected by a single LWLock (modifications are
 *   infrequent).
 *
 * An optional per-tuple flat-hash extension (the tuple sLog) adds
 * bounded-recovery uncommitted-writer tracking for in-place-update table
 * AMs.  It shares this subsystem's shared-memory segment and initialization
 * but is not required by the UNDO core; its API is declared where it is
 * defined.
 *
 * ENGINE-AGNOSTIC: the tuple sLog is independent of which UNDO engine an AM
 * uses.  It keys purely on physical tuple identity (relid, tid) and holds no
 * reference to the cluster-wide or the per-backend UNDO engine.  Any
 * in-place-MVCC AM may use it, regardless of UNDO engine.  Cleanup is driven
 * by the consumer AM (or its UNDO worker) at commit/abort via the public
 * SLogTupleRemoveByXid* entry points; a per-backend AM drives the same
 * removals from the per-backend rollback path (execute_undo_actions) exactly
 * as an AM using its transaction hooks would.
 *
 * WAL: Transaction sLog reuses existing RM_ATM_ID records.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOG_H
#define SLOG_H

#include "access/transam.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "nodes/lockoptions.h"
#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "storage/relfilelocator.h"
#include "utils/dsa.h"

/*
 * Maximum concurrent operations on a single TID.
 *
 * Under EPQ retry patterns with high concurrency, each backend that sees
 * TM_Updated calls table_tuple_lock which adds a LOCK_EXCL sLog entry.
 * These entries persist until the owning transaction commits, so with N
 * concurrent backends hitting the same hot row, up to N-1 LOCK entries
 * can coexist.  Additionally, committed in-place UPDATE markers are retained
 * (one per committing xid) until the oldest-snapshot horizon advances past
 * them, so a hot row updated repeatedly while a long reader holds a snapshot
 * accumulates markers on top of the live LOCK entries.  128 slots handles
 * realistic OLTP concurrency plus retained-marker headroom; per-TID
 * reclamation of the oldest retained marker kicks in before the array fills.
 */
#define SLOG_MAX_TUPLE_OPS		128

/*
 * Tuple hash auto-sizing constants.
 *
 * All DML operations (INSERT/DELETE/UPDATE/LOCK) attempt to create shared
 * hash entries.  On overflow (hash full), the operation proceeds gracefully
 * with local-only tracking rather than crashing.  Auto-sizing formula:
 * MaxBackends * SLOG_TUPLE_PER_BACKEND_SLOTS, clamped.
 *
 * The per-backend slot count must be large enough to accommodate OLTP
 * workloads where UPDATE before-images are retained until eviction.
 * With transactions touching ~25 rows, and retained entries from committed
 * transactions accumulating between eviction passes, 1024 slots per backend
 * provides adequate headroom.
 */
#define SLOG_TUPLE_PER_BACKEND_SLOTS	1024
#define SLOG_TUPLE_MIN_ENTRIES			4096
#define SLOG_TUPLE_MAX_ENTRIES			4194304

/*
 * sLog entry types for tuple operations.
 */
typedef enum SLogOpType
{
	SLOG_ENTRY_ABORTED_TXN = 0, /* Transaction-level abort entry */
	SLOG_OP_INSERT = 1,
	SLOG_OP_DELETE = 2,
	SLOG_OP_UPDATE = 3,
	SLOG_OP_LOCK_SHARE = 4,
	SLOG_OP_LOCK_EXCL = 5,
	SLOG_OP_ABORTED = 6,		/* Tuple-level: op was aborted, UNDO pending */
} SLogOpType;

/* ----------------------------------------------------------------
 * Transaction sLog structures
 *
 * SLogTxnEntry is used as the public output type for lookups.
 * Internally, the ATM radix tree stores only the data fields; the key
 * (xid, reloid) is implicit in the tree path.
 * ----------------------------------------------------------------
 */

/*
 * SLogTxnEntry - Public output structure for transaction lookups.
 * Callers receive copies of this via SLogTxnLookup().
 */
typedef struct SLogTxnEntry
{
	TransactionId xid;
	Oid			reloid;
	XLogRecPtr	last_batch_lsn; /* LSN of last UNDO batch for this xid */
	XLogRecPtr	stop_lsn;		/* apply only batches newer than this, or
								 * InvalidXLogRecPtr for the whole chain */
	XLogRecPtr	first_batch_lsn;	/* oldest batch of the chain: pins WAL */
	Oid			dboid;			/* database OID */
	TimestampTz abort_time;		/* when transaction aborted */
	bool		revert_complete;	/* has Logical Revert finished? */
	bool		fileops_owned;	/* FILEOPS completed or owned by abort WAL */
} SLogTxnEntry;

/* ----------------------------------------------------------------
 * Tuple sLog structures
 * ----------------------------------------------------------------
 */

/*
 * SLogTupleKey - Hash key for the tuple flat hash.
 *
 * Note: ItemPointerData is 6 bytes.  Always memset(&key, 0, sizeof(key))
 * before populating to ensure deterministic byte hashing.
 */
typedef struct SLogTupleKey
{
	Oid			relid;
	ItemPointerData tid;
} SLogTupleKey;

/*
 * SLogTupleOp - Single operation on a tuple.
 *
 * Uses an in_use slot-based model for O(1) removal without array compaction
 * under the exclusive lock.
 */
typedef struct SLogTupleOp
{
	TransactionId xid;
	TransactionId subxid;		/* subtransaction ID, or InvalidTransactionId */
	SLogOpType	op_type;
	CommandId	cid;
	TimestampTz commit_ts;		/* 0 if not yet committed */
	uint32		spec_token;		/* speculative insertion token, or 0 */
	bool		in_use;			/* slot occupied? */

	/*
	 * Precise tuple-lock strength for LOCK_SHARE/LOCK_EXCL markers.  The
	 * SLOG_OP_LOCK_* op_type only distinguishes shared vs. exclusive intent;
	 * this field preserves the full four-way LockTupleMode so a concurrent
	 * updater can apply the real heavyweight conflict matrix (a KeyShare FK
	 * locker is compatible with a NoKeyExclusive UPDATE, but a
	 * Share/Exclusive locker conflicts and must block it).  Ignored for
	 * non-lock ops.
	 */
	LockTupleMode lock_mode;
} SLogTupleOp;

/*
 * SLogTupleEntry - Value in the tuple flat hash.
 */
typedef struct SLogTupleEntry
{
	SLogTupleKey key;			/* hash key */
	int			nops;			/* number of active operations */
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
} SLogTupleEntry;

/* Callback type for tuple iteration */
typedef bool (*SLogTupleIterCallback) (const SLogTupleOp *op, void *arg);

/* ----------------------------------------------------------------
 * Shared state
 *
 * The transaction radix tree is allocated in shared memory; its internal
 * structures are opaque to callers.  The SLogSharedState is defined in
 * slog.c.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 * API: Shared memory
 * ----------------------------------------------------------------
 */
extern Size SLogShmemSize(void);
extern void SLogShmemRequest(void);
extern void SLogShmemInit(void);

/* ----------------------------------------------------------------
 * API: Transaction sLog
 * ----------------------------------------------------------------
 */
extern bool SLogTxnInsert(TransactionId xid, Oid reloid, Oid dboid,
						  XLogRecPtr last_batch_lsn, XLogRecPtr stop_lsn,
						  XLogRecPtr first_batch_lsn, bool fileops_owned,
						  bool check_space);
extern bool SLogTxnLookup(TransactionId xid, Oid reloid,
						  SLogTxnEntry *entry_out);
extern bool SLogTxnLookupByXid(TransactionId xid, XLogRecPtr *lsn_out);
extern void SLogTxnRemove(TransactionId xid, Oid reloid);
extern void SLogTxnRemoveByXid(TransactionId xid);
extern void SLogTxnMarkReverted(TransactionId xid);
extern bool SLogTxnGetNextUnreverted(Oid dboid, TransactionId after_xid,
									 TransactionId *xid_out,
									 XLogRecPtr *lsn_out,
									 XLogRecPtr *stop_lsn_out);
extern int	SLogTxnCollectUnrevertedDatabases(Oid *dboids, int max_dboids);
extern XLogRecPtr SLogTxnGetOldestUnrevertedLSN(void);
extern int	SLogTxnSnapshotForCheckpoint(SLogTxnEntry **entries_out);
extern void SLogRecoveryFinalize(int *total_out, int *unreverted_out);

/* DSA lifecycle (shared-memory area backing the aborted-txn radix tree) */
extern void SLogEnsureDsaAttached(void);

/* GUC: maximum DSA size (in MB) */
extern int	slog_dsa_max_size_mb;

/* ----------------------------------------------------------------
 * API: Tuple sLog (optional per-tuple tracking extension, slog_tuple.c)
 * ----------------------------------------------------------------
 */

/* Dynamic sizing */
extern int	SLogTupleNumEntries(void);

/* Core operations */
extern bool SLogTupleInsert(Oid relid, ItemPointer tid, TransactionId xid,
							SLogOpType op_type, TransactionId subxid,
							CommandId cid, TimestampTz commit_ts,
							uint32 spec_token, LockTupleMode lock_mode);
extern bool SLogTupleInsertRecovery(Oid relid, ItemPointer tid,
									TransactionId xid, SLogOpType op_type);
extern void SLogTupleRestoreLock(Oid relid, ItemPointer tid,
								 TransactionId xid, LockTupleMode mode);
extern bool SLogTupleLookup(Oid relid, ItemPointer tid,
							SLogTupleEntry *entry_out);
extern void SLogTupleRemove(Oid relid, ItemPointer tid, TransactionId xid);
extern void SLogTupleRemoveByXid(TransactionId xid);
extern void SLogTupleRemoveBySubXid(TransactionId xid, TransactionId subxid);
extern void SLogTupleIterateByTid(Oid relid, ItemPointer tid,
								  SLogTupleIterCallback callback, void *arg);

/* Filtered lookup (xid_filter=InvalidTransactionId means all) */
extern int	SLogTupleLookupFiltered(Oid relid, ItemPointer tid,
									TransactionId xid_filter,
									SLogTupleOp *ops_out, int max_ops);

/* Subtransaction re-parenting on subxact commit */
extern void SLogTupleUpdateSubXid(TransactionId xid,
								  TransactionId old_subxid,
								  TransactionId new_subxid);

/* Mark all ops for xid as SLOG_OP_ABORTED */
extern void SLogTupleMarkAborted(TransactionId xid);

/* Global removal for UNDO worker (no backend-local list) */
extern void SLogTupleRemoveByXidGlobal(TransactionId xid);

/* Lightweight local-only tracking (INSERTs only) */
extern void SLogTupleTrackLocalOnly(Oid relid, ItemPointer tid,
									TransactionId xid, TransactionId subxid);
extern void SLogTupleUntrackLocalOnly(Oid relid, ItemPointer tid);

/* Convenience wrappers */
extern bool SLogTupleHasEntry(Oid relid, ItemPointer tid);
extern bool SLogTupleIsInsertedByMe(Oid relid, ItemPointer tid);
extern bool SLogTupleIsDeletedByMe(Oid relid, ItemPointer tid);
extern TransactionId SLogTupleGetDirtyXid(Oid relid, ItemPointer tid,
										  bool *is_insert);
extern TransactionId SLogTupleGetDirtyWriterXid(Oid relid, ItemPointer tid,
												bool *is_insert);
extern TransactionId SLogTupleGetWriteConflictXid(Oid relid, ItemPointer tid,
												  LockTupleMode my_mode,
												  bool *is_insert);
extern bool SLogTupleHasLockConflict(Oid relid, ItemPointer tid,
									 TransactionId my_xid,
									 SLogOpType requested_lock);
extern bool SLogTupleGetLockConflictXid(Oid relid, ItemPointer tid,
										TransactionId my_xid,
										LockTupleMode my_mode,
										TransactionId *xid_out);
extern bool SLogTupleHasAbortedEntry(Oid relid, ItemPointer tid);


/* Backend-private tracking for cleanup at commit/abort */
extern void SLogTupleTrackKey(SLogTupleKey key, TransactionId xid,
							  TransactionId subxid, SLogOpType op_type);
extern void SLogTupleResetTracking(void);
extern bool SLogTupleAnyTracked(void);

/*
 * SLogTrackedKeyInfo -- public snapshot of a tracked key for batch processing.
 *
 * Returned by SLogTupleCollectTrackedKeys() so that callers (e.g. an AM's
 * commit-time stamping) can sort and batch-process tuples without knowledge
 * of the internal tracked-key linked-list structure.
 */
typedef struct SLogTrackedKeyInfo
{
	SLogTupleKey key;
	TransactionId xid;
	TransactionId subxid;
	bool		local_only;
	SLogOpType	op_type;
} SLogTrackedKeyInfo;

/* Collect tracked keys into a sortable array (for batch commit processing) */
extern int	SLogTupleCollectTrackedKeys(TransactionId xid,
										SLogTrackedKeyInfo **out_keys);

/* Iterate tracked keys (for AM-specific pre-commit callbacks) */
typedef bool (*SLogTrackedKeyCallback) (const SLogTupleKey *key,
										TransactionId xid,
										TransactionId subxid,
										bool local_only,
										void *arg);
extern void SLogTupleIterateTrackedKeys(TransactionId xid,
										SLogTrackedKeyCallback callback,
										void *arg);

/* Commit retention: retain committed UPDATE entries with before-images */
extern void SLogTupleCommitByXid(TransactionId xid);

/* Per-tuple operations for two-phase commit resolution */
extern void SLogTupleRemoveByXidSingle(Oid relid, ItemPointer tid,
									   TransactionId xid);
extern void SLogTupleMarkAbortedSingle(Oid relid, ItemPointer tid,
									   TransactionId xid);

/* Cleanup retained entries when no longer needed by any snapshot */
extern void SLogTupleCleanupRetained(void);

/*
 * Throttled cleanup trigger for access methods to call from DML paths OUTSIDE
 * any buffer-locked critical section (drives cleanup when the UNDO worker is
 * disabled).  Cheap to call repeatedly; fires the global sweep at most once
 * every few seconds per backend.
 */
extern void SLogTupleMaybeCleanupRetained(void);

/* GUC: number of sLog flat hash partitions (0 = auto based on CPU count) */
extern int	slog_num_partitions;

#endif							/* SLOG_H */
