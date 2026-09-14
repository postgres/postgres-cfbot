/*-------------------------------------------------------------------------
 *
 * flux.h
 *	  FLUX table access method definitions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/flux.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FLUX_H
#define FLUX_H

#include "postgres.h"

#include "access/heapam.h"
#include "access/undodefs.h"	/* UndoRecPtr (per-backend version pointer) */
#include "access/toast_helper.h"
#include "storage/shmem.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "port/atomics.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/procnumber.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

/*
 * FLUX special space structure - stored in page special space (8 bytes)
 *
 * Packs the page-level commit timestamp and 3 flag bits into a single
 * uint64.  The top 3 bits (63-61) store page flags; the lower 61 bits
 * store the commit timestamp (sufficient for 73,000+ years of microseconds).
 *
 * pd_free_space was removed -- use PageGetFreeSpace() directly (same as
 * heap).  pd_flags was removed -- flags are packed into the timestamp word.
 */
typedef struct FluxPageOpaqueData
{
	uint64		pd_commit_ts_and_flags; /* bits 63-61: flags, bits 60-0: ts */
} FluxPageOpaqueData;

typedef FluxPageOpaqueData *FluxPageOpaque;

/* Page flags (stored in top 3 bits of pd_commit_ts_and_flags) */
#define FLUX_PAGE_FLAG_SHIFT		61
#define FLUX_PAGE_FLAG_MASK		(UINT64CONST(0x7) << FLUX_PAGE_FLAG_SHIFT)
#define FLUX_PAGE_TS_MASK			(~FLUX_PAGE_FLAG_MASK)

#define FLUX_PAGE_OVERFLOW			(UINT64CONST(1) << 61)
#define FLUX_PAGE_DEFRAG_NEEDED	(UINT64CONST(1) << 62)
#define FLUX_PAGE_FULL				(UINT64CONST(1) << 63)

/* Accessor macros for page opaque */
#define FluxPageGetOpaque(page) \
	((FluxPageOpaque) PageGetSpecialPointer(page))

#define FluxPageGetCommitTs(opaque) \
	((opaque)->pd_commit_ts_and_flags & FLUX_PAGE_TS_MASK)

#define FluxPageSetCommitTs(opaque, ts) \
	((opaque)->pd_commit_ts_and_flags = \
		((opaque)->pd_commit_ts_and_flags & FLUX_PAGE_FLAG_MASK) | \
		((uint64)(ts) & FLUX_PAGE_TS_MASK))

#define FluxPageGetFlags(opaque) \
	((opaque)->pd_commit_ts_and_flags & FLUX_PAGE_FLAG_MASK)

#define FluxPageSetFlag(opaque, flag) \
	((opaque)->pd_commit_ts_and_flags |= (flag))

#define FluxPageClearFlag(opaque, flag) \
	((opaque)->pd_commit_ts_and_flags &= ~(flag))

/*
 * FLUX tuple header structure (v3 -- heap-compatible xmin/xmax MVCC)
 *
 * Visibility is ordinary heap-shaped xmin/xmax + CLOG + snapshot (the same
 * model HeapTupleSatisfiesMVCC uses).  A tuple is visible to a snapshot iff
 * its inserter (t_xmin) is committed-and-visible-to-the-snapshot AND its
 * deleter/updater (t_xmax) is either invalid, not committed, or not visible to
 * the snapshot.
 *
 *   - t_xmin: the inserting transaction's XID.  Always valid once the tuple
 *     exists on the page (it rides through WAL redo verbatim in the logged
 *     tuple body, so it needs no separate redo reconstruction).
 *   - t_xmax: the deleting/updating transaction's XID, or InvalidTransactionId
 *     for a live never-superseded tuple.  Physically stored in the low 32
 *     bits of the t_commit_ts word (see FluxTupleGetXmax below).
 *
 * In-place UPDATE keeps the newest version on the page (new t_xmin) and pushes
 * the pre-update image to the UNDO log via t_verptr.  A snapshot that predates
 * the update reads the old version back from the UNDO log with
 * FluxReconstructVisibleVersion(), which walks t_verptr and stops at the
 * version whose producing xid is visible (FluxWriterVisibleToSnapshot:
 * committed and not in the snapshot).
 *
 * Transient operation state (who is inserting/deleting/locking concurrently)
 * is still tracked in the sLog for write-write conflict serialization and SSI,
 * but it is no longer consulted for read visibility -- CLOG is authoritative.
 */
typedef struct FluxTupleHeader
{
	uint64		t_commit_ts;	/* 8B  low 32 bits = t_xmax (deleter/updater
								 * XID); high 32 bits reserved.  Accessed via
								 * FluxTupleGetXmax/SetXmax.  */
	UndoRecPtr	t_verptr;		/* 8B  per-backend UNDO version-chain head: an
								 * UndoRecPtr returned by PrepareUndoInsert. */

	/*
	 * t_cmin: the command id of the writer identified by t_xmin, while that
	 * writer is still uncommitted.  Heap keeps the same information in t_cid.
	 *
	 * INVARIANT: t_cmin always describes the SAME writer as t_xmin.  A FLUX
	 * in-place UPDATE re-stamps t_xmin to the updater, so it must re-stamp
	 * t_cmin to the updater's command id too.  Carrying the original INSERT's
	 * cid across an in-place UPDATE pairs a new xmin with an old cid and
	 * makes the self-visibility test below compare our own curcid against a
	 * different transaction's cid, which hides legitimately-written rows
	 * (measured: six isolation tests, incl. freeze-the-dead "unexpected
	 * table_tuple_lock status: 1" and a stale SUM in total-cash).
	 *
	 * REQUIRED FOR CORRECTNESS, not an optimisation.  Without it a statement
	 * cannot distinguish its own output rows from its input rows, which is
	 * the Halloween problem: an UPDATE that relocates a row to a new TID
	 * (FLUX's out-of-place path) can have the very same scan re-find the row
	 * it just wrote and update it again.  Measured before this field existed:
	 * `UPDATE hh SET v = v+1, pad = pad||repeat('y',500)` over 2000 rows
	 * reported UPDATE 3194 with 1194 rows left holding v=2, where heap
	 * reports UPDATE 2000 and every row v=1.
	 *
	 * Read only while FLUX_TUPLE_UNCOMMITTED is set and t_xmin is our own
	 * transaction (FluxTupleSatisfiesMVCC).  After commit it is meaningless
	 * and nothing clears it, exactly as heap leaves a stale t_cid behind.
	 *
	 * This occupies the word formerly reserved as the unused t_gen, so tuple
	 * geometry, on-disk layout and WAL byte layout are all unchanged (the
	 * tuple body, including this field, is logged verbatim).
	 */
	CommandId	t_cmin;			/* 4B  Command id of t_xmin's writer */

	/*
	 * t_xmin: the inserting transaction's XID (heap xmin semantics).  Always
	 * valid once the tuple exists.  Visibility resolves it against CLOG and
	 * the reader's snapshot exactly like HeapTupleSatisfiesMVCC, so no sLog
	 * lookup is needed on the common read path.  The tuple body (including
	 * this field) is WAL-logged verbatim, so t_xmin survives redo without
	 * separate reconstruction.
	 *
	 * (Formerly t_xid_hint, which was "valid only while UNCOMMITTED"; the
	 * heap-shaped model makes it permanently authoritative as xmin.)
	 */
	TransactionId t_xmin;		/* 4B  Inserter XID (heap xmin) */

	/*
	 * t_verptr is the head of this tuple's persistent version chain in the
	 * UNDO log.  Every tuple carries it from birth, so the first UPDATE does
	 * not grow the tuple.  InvalidUndoRecPtr means "never updated -- no
	 * history"; readers treat that as "on-page image is current".
	 *
	 * t_cid removed: command ID is now obtained from the sLog entry
	 * (FluxSLogEntry.cid) when FLUX_TUPLE_UNCOMMITTED is set. This saves 4
	 * bytes per tuple. The sLog lookup is mandatory for uncommitted
	 * DELETE/UPDATE visibility anyway, so fetching the cid from there adds
	 * zero extra overhead.  (INSERT visibility no longer needs the sLog at
	 * all in the common case -- see t_xmin above.)
	 */
	ItemPointerData t_ctid;		/* 6B  Own TID, or the new version's TID if
								 * FLUX_TUPLE_SUPERSEDED */
	uint16		t_natts;		/* 2B  Number of attributes */
	uint16		t_flags;		/* 2B  Tuple flags */
	uint8		t_infomask;		/* 1B  HASNULL, HASVARWIDTH, etc. */
	uint8		t_pad;			/* 1B padding */
	uint8		t_attrs_bitmap[FLEXIBLE_ARRAY_MEMBER];
} FluxTupleHeader;

/* The fixed header is 36 bytes; the null bitmap follows without padding. */

/* Tuple flags (uint16) */
#define FLUX_TUPLE_COMPRESSED		0x0001
#define FLUX_TUPLE_HAS_OVERFLOW	0x0002
#define FLUX_TUPLE_DELETED			0x0004
#define FLUX_TUPLE_UPDATED			0x0008
#define FLUX_TUPLE_LOCKED			0x0010
#define FLUX_TUPLE_SPECULATIVE		0x0020

/*
 * FLUX_TUPLE_KEYS_UPDATED -- this in-place image was produced by an UPDATE
 * that changed at least one INDEXED column.  It is FLUX's analogue of heap's
 * HEAP_KEYS_UPDATED (t_infomask2), and exists for exactly heap's reason: a
 * LockTupleKeyShare request (the FK check's parent-row probe) must NOT block
 * behind an in-progress UPDATE that left the key intact, but MUST block behind
 * one that is changing it.
 *
 * FLUX cannot infer this from FLUX_TUPLE_UPDATED: unlike heap, FLUX updates
 * key-changing rows IN PLACE as well (nbtree delete-marking), so
 * FLUX_TUPLE_UPDATED is set for both kinds and cannot distinguish them.  Nor
 * can the tuple sLog answer it: it records row locks, not writers (the
 * on-page header is the authority for those), so its lock-conflict matrix
 * (SLogTupleLockConflictXid) is never consulted for an in-progress in-place
 * updater.  Hence a dedicated header bit.
 *
 * Safety direction: a STALE-SET bit only costs a wait that heap would have
 * skipped (a missed optimization).  A STALE-CLEAR bit would hand out a
 * KeyShare lock against a row whose key is changing -- an FK-integrity hole.
 * So every path that writes an in-place image must set it whenever an indexed
 * column changed, and when in doubt it is set.
 */
#define FLUX_TUPLE_KEYS_UPDATED	0x0040	/* in-place UPDATE changed an indexed
										 * column (heap's HEAP_KEYS_UPDATED
										 * analogue) */
#define FLUX_TUPLE_UNCOMMITTED		0x0080	/* Inserted but not yet committed */
#define FLUX_TUPLE_HAS_VERSION_PTR	0x0100	/* trailing UndoRecPtr
											 * (version-chain head) follows
											 * column data */
#define FLUX_TUPLE_XMIN_COMMITTED	0x0200	/* t_xmin known-committed (CLOG
											 * hint, like heap
											 * HEAP_XMIN_COMMITTED) */
#define FLUX_TUPLE_XMAX_COMMITTED	0x0400	/* t_xmax known-committed (CLOG
											 * hint, like heap
											 * HEAP_XMAX_COMMITTED) */

/*
 * FLUX_TUPLE_MOVED_PARTITION -- this DELETED tuple was deleted by an UPDATE
 * that moved the row to another partition (TABLE_DELETE_CHANGING_PARTITION).
 * It is heap's ItemPointerSetMovedPartitions(&t_ctid) marker.  FLUX cannot
 * keep the marker in t_ctid, which is the tuple's own TID: the sLog and the
 * own-command cid map are looked up by it.  It is set only together with
 * FLUX_TUPLE_DELETED and is meaningful only while that is set, so VACUUM and
 * freezing treat the tuple as the deleted one it is.  The delete's UNDO
 * before-image clears it on rollback, and its WAL record carries it
 * (FLUX_WAL_MOVED_PARTITION), so flux_mask leaves it to be compared.
 * Writers and lockers that find it report TM_Updated with the moved-partitions
 * marker in tmfd->ctid, as heap does.
 */
#define FLUX_TUPLE_MOVED_PARTITION	0x0800

/*
 * FLUX_TUPLE_SUPERSEDED -- this DELETED tuple was deleted by an UPDATE that
 * moved the row to a new TID in the same relation (flux_update_out_of_place:
 * a key change FLUX cannot index in place, or a row that cannot grow on its
 * page), and t_ctid holds that new TID.  It is heap's t_ctid forward link.
 * Without the flag t_ctid holds the tuple's own TID, so nothing may read
 * t_ctid as a link unless the flag is set; the own TID always comes from the
 * caller.  It is set only together with FLUX_TUPLE_DELETED and is meaningful
 * only while that is set, so VACUUM and freezing treat the tuple as the
 * deleted one it is.  XLOG_FLUX_SUPERSEDE sets it, so flux_mask compares it
 * and the forward TID.  The delete's UNDO before-image clears it and restores
 * the own TID on rollback.  Writers and lockers that find it report
 * TM_Updated with the new TID in tmfd->ctid, and a lock that follows the row
 * to its latest version locks the new one, as heap does.
 */
#define FLUX_TUPLE_SUPERSEDED		0x1000

/* The superseding UPDATE left every key column unchanged. */
#define FLUX_TUPLE_KEY_INTACT		0x2000

/*
 * flux_tuple_insert option, used only by the CLUSTER / VACUUM FULL rewrite
 * (flux_relation_copy_for_cluster): store the row with the inserter and
 * deleter of the version it copies, from FluxRewriteVersion, rather than as
 * an insert by the current transaction.  The rewrite holds AccessExclusiveLock
 * and copies only versions whose writers committed (or are the rewriting
 * transaction itself), so a snapshot taken before the rewrite sees exactly
 * the rows it saw before, as with heap's rewriteheap.c.  A table AM option
 * bit above the core TABLE_INSERT_* ones.
 */
#define FLUX_INSERT_REWRITE			0x1000

/*
 * flux_tuple_insert and flux_tuple_delete option, used by an out-of-place
 * UPDATE (flux_update_out_of_place): do not count the insert or the delete in
 * the cumulative statistics, the caller counts the UPDATE once.  Above the
 * core TABLE_INSERT_* and TABLE_DELETE_* bits.
 */
#define FLUX_DML_NO_STATS			0x2000

typedef struct FluxRewriteVersion
{
	TransactionId xmin;			/* inserter of the copied version */
	CommandId	cmin;			/* its command id, if it is our own */
	bool		xmin_committed; /* xmin is known committed */
	TransactionId xmax;			/* deleter, or InvalidTransactionId */
	bool		xmax_committed; /* xmax is known committed */
	bool		moved_partition;	/* deleted by a cross-partition UPDATE */
} FluxRewriteVersion;

extern void FluxSetRewriteVersion(const FluxRewriteVersion *v);

/* Tuple infomask bits (uint8 -- reduced from uint16) */
#define FLUX_INFOMASK_HASNULL		0x01
#define FLUX_INFOMASK_HASVARWIDTH	0x02
#define FLUX_INFOMASK_HASEXTERNAL	0x04
#define FLUX_INFOMASK_COMPRESSED	0x08
#define FLUX_INFOMASK_HASOVERFLOW	0x10

/*
 * FLUX tuple structure
 */
typedef struct FluxTupleData
{
	uint32		t_len;			/* Length of tuple */
	ItemPointerData t_self;		/* TID of this tuple */
	Oid			t_tableOid;		/* Table OID */
	FluxTupleHeader *t_data;	/* Tuple header and data */
} FluxTupleData;

typedef FluxTupleData *FluxTuple;

/*
 * Column-level overflow
 *
 * When an individual column value is too large to store inline in the main
 * tuple, it is stored as one or more "overflow records" on normal FLUX data
 * pages.  The main tuple stores a compact overflow pointer (FluxOverflowPtr)
 * wrapped in a varlena, optionally preceded by an inline prefix of the
 * original data for efficient prefix matching.
 *
 * Overflow records use a lightweight header (FluxOverflowRecordHeader)
 * without MVCC fields -- they share the visibility of the parent tuple.
 * Each overflow record holds a chunk of the column data and a continuation
 * pointer to the next chunk (or InvalidBlockNumber if this is the last).
 *
 * This approach stores overflow data on regular pages that can also hold
 * normal tuples, within the same relation -- no separate out-of-line
 * relation is created.
 */

/*
 * Overflow pointer stored inline in the main tuple (wrapped as varlena).
 *
 * On-disk layout of an overflowed column in the main tuple:
 *   [varlena header][FluxOverflowPtr][inline_prefix_bytes...]
 *
 * The FLUX_OVERFLOW_PTR_MAGIC sentinel distinguishes this from a normal
 * varlena value during deform.
 */
#define FLUX_OVERFLOW_PTR_MAGIC	0x52564F50	/* "RVOP" */

typedef struct FluxOverflowPtr
{
	uint32		ov_magic;		/* FLUX_OVERFLOW_PTR_MAGIC */
	BlockNumber ov_first_block; /* First overflow record's page */
	OffsetNumber ov_first_offset;	/* First overflow record's offset on page */
	uint16		ov_inline_prefix;	/* Bytes of inline prefix stored after ptr */
	uint32		ov_total_length;	/* Total uncompressed column data length */
	uint32		ov_content_hash;	/* 32-bit content-hash prefilter for COW */
} FluxOverflowPtr;

/*
 * ov_content_hash reclaims the two formerly-reserved uint16 fields
 * (ov_padding, ov_flags), so the struct stays 20 bytes and
 * FLUX_OVERFLOW_PTR_SIZE is unchanged.  A wider (64-bit) hash would force
 * struct growth that inflates FLUX_OVERFLOW_PTR_SIZE and defeats the
 * force-shrink UPDATE recovery path, breaking in-place updates on full pages.
 *
 * The hash is only a cheap prefilter: every COW-reference candidate is
 * byte-verified against the fetched old chain before it is accepted, so a
 * 32-bit collision merely costs a wasted fetch and falls through to a normal
 * re-store.  It never produces an incorrect result.
 */

/* Minimum varlena size for an overflow pointer (no inline prefix) */
#define FLUX_OVERFLOW_PTR_SIZE		(VARHDRSZ + sizeof(FluxOverflowPtr))

/* Default inline prefix size (configurable via GUC) */
#define FLUX_OVERFLOW_DEFAULT_PREFIX	128

/*
 * Check if a varlena datum is an overflow pointer.
 *
 * The check requires: correct size range, and magic value match.
 */
static inline bool
FluxIsOverflowPtr(const void *ptr)
{
	Size		vsize;
	const FluxOverflowPtr *ovp;

	if (ptr == NULL)
		return false;

	vsize = VARSIZE_ANY_EXHDR(ptr);
	if (vsize < sizeof(FluxOverflowPtr))
		return false;

	ovp = (const FluxOverflowPtr *) VARDATA_ANY(ptr);
	return ovp->ov_magic == FLUX_OVERFLOW_PTR_MAGIC;
}

/*
 * Extract overflow pointer from a varlena datum.
 */
static inline const FluxOverflowPtr *
FluxGetOverflowPtr(const void *ptr)
{
	return (const FluxOverflowPtr *) VARDATA_ANY(ptr);
}

/*
 * Lightweight header for overflow records stored on normal data pages.
 *
 * Overflow records are stored via PageAddItem just like normal tuples, but
 * they carry this minimal header instead of a full FluxTupleHeader.  The
 * ov_magic field lets us distinguish overflow records from normal tuples
 * during page scans (e.g., sequential scan must skip these).
 */
#define FLUX_OVERFLOW_RECORD_MAGIC		0x524F5643	/* "ROVC" */

typedef struct FluxOverflowRecordHeader
{
	uint32		or_magic;		/* FLUX_OVERFLOW_RECORD_MAGIC */
	uint32		or_data_len;	/* Bytes of column data in this record */
	BlockNumber or_next_block;	/* Next overflow record's page, or Invalid */
	OffsetNumber or_next_offset;	/* Next overflow record's offset */
	uint16		or_flags;		/* Flags (reserved) */
	/* Column data follows immediately after this header */
} FluxOverflowRecordHeader;

/* Maximum column data per overflow record */
#define FLUX_OVERFLOW_RECORD_OVERHEAD	MAXALIGN(sizeof(FluxOverflowRecordHeader))
#define FLUX_OVERFLOW_MAX_CHUNK_SIZE \
	(FLUX_MAX_TUPLE_SIZE - FLUX_OVERFLOW_RECORD_OVERHEAD)

/*
 * Structure to track overflow buffers for atomic WAL logging.
 *
 * When creating overflow chains, we keep buffers pinned and collect them
 * here so the caller can register them all in a single WAL record with
 * the main tuple modification. This ensures atomicity during crash recovery.
 */
#define MAX_OVERFLOW_BUFFERS 32

typedef struct FluxOverflowBuffer
{
	Buffer		buffer;			/* Pinned buffer containing overflow record */
	OffsetNumber offset;		/* Offset of overflow record on page */
	char	   *record_data;	/* FluxOverflowRecordHeader + data */
	uint32		record_len;		/* Total record length */
	uint16		flags;			/* FLUX_OVERFLOW_WAL_NEW_RECORD or
								 * _LINK_UPDATE */
} FluxOverflowBuffer;

typedef struct FluxOverflowBuffers
{
	int			count;			/* Number of overflow buffers */
	FluxOverflowBuffer buffers[MAX_OVERFLOW_BUFFERS];
} FluxOverflowBuffers;

/*
 * Compression types
 */
typedef enum FluxCompressionType
{
	FLUX_COMP_NONE,
	FLUX_COMP_LZ4,
	FLUX_COMP_ZSTD,
	FLUX_COMP_DELTA,			/* For numeric columns */
	FLUX_COMP_DICTIONARY		/* For text columns */
} FluxCompressionType;

/*
 * Values for the flux_compression_algorithm GUC.  AUTO lets
 * FluxChooseCompressionType() pick per attribute type; LZ4/ZSTD force that
 * codec for compressible varlena attributes; NONE disables compression.
 */
typedef enum FluxCompressionAlgoGuc
{
	FLUX_COMP_ALGO_AUTO,
	FLUX_COMP_ALGO_LZ4,
	FLUX_COMP_ALGO_ZSTD,
	FLUX_COMP_ALGO_OFF
} FluxCompressionAlgoGuc;

typedef struct FluxCompressionHeader
{
	uint8		comp_type;
	uint8		comp_level;
	uint16		dict_id;		/* trained-dict id, 0 = FLUX_DICT_INVALID_ID */
	uint32		orig_size;
	uint32		comp_size;
} FluxCompressionHeader;

/*
 * FLUX timestamp word.
 *
 * A uint64 wall-clock timestamp (microseconds since the PG epoch) used for
 * per-page commit-ts bookkeeping (FluxPageSetCommitTs).  It is NOT a
 * visibility timestamp: commit visibility comes from CLOG via heap-shaped
 * xmin/xmax MVCC.
 */

/*
 * Tuple MVCC field accessors (heap-shaped xmin/xmax).
 *
 * t_xmax is physically the low 32 bits of the t_commit_ts word; the high 32
 * bits are reserved.  This preserves the on-disk/WAL byte layout while giving
 * the tuple a heap-compatible deleter/updater XID.  InvalidTransactionId (0)
 * means "live, never superseded".
 */
#define FluxTupleGetXmax(tup)		((TransactionId) ((tup)->t_commit_ts & 0xFFFFFFFFULL))
#define FluxTupleSetXmax(tup, xid) \
	((tup)->t_commit_ts = ((tup)->t_commit_ts & 0xFFFFFFFF00000000ULL) | \
						   ((uint64) (TransactionId) (xid)))
#define FluxTupleGetXmin(tup)		((tup)->t_xmin)
#define FluxTupleSetXmin(tup, xid)	((tup)->t_xmin = (TransactionId) (xid))

/*
 * Transaction state for uncertainty tracking.
 *
 * The full struct definition lives in flux_mvcc.c (private to that module).
 * External code should use the opaque forward declaration below.
 */
typedef struct FluxTransactionState FluxTransactionState;

/*
 * Free space management
 */
typedef struct FluxFreeSpaceMap
{
	uint32		total_pages;
	uint32		pages_with_space;
	uint8	   *fsm_data;		/* Bitmap of page utilization */
	uint32	   *defrag_queue;	/* Pages needing defragmentation */
	uint32		defrag_queue_size;
} FluxFreeSpaceMap;

/* Free space map levels */
#define FLUX_FSM_FULL			0
#define FLUX_FSM_75_PERCENT	1
#define FLUX_FSM_50_PERCENT	2
#define FLUX_FSM_25_PERCENT	3
#define FLUX_FSM_EMPTY			4

/*
 * Visibility Map support for FLUX
 *
 * The visibility map tracks two bits per page:
 * - ALL_VISIBLE: all tuples on page are visible to all transactions
 * - ALL_FROZEN: all tuples on page are frozen (no further VACUUM needed)
 *
 * This enables:
 * - Index-only scans (can skip heap fetch if page is all-visible)
 * - VACUUM optimization (can skip pages marked all-visible/frozen)
 */

/* Visibility map bits */
#define FLUX_VM_ALL_VISIBLE		0x01	/* All tuples visible to all xacts */
#define FLUX_VM_ALL_FROZEN			0x02	/* All tuples frozen */

/* Combined flags for convenience */
#define FLUX_VM_VALID_BITS			(FLUX_VM_ALL_VISIBLE | FLUX_VM_ALL_FROZEN)

/* Visibility map fork number (uses PostgreSQL's fork infrastructure) */
#define FLUX_VM_FORKNUM			VISIBILITYMAP_FORKNUM

/*
 * Scan descriptor for FLUX scans
 */
typedef struct FluxScanDescData
{
	TableScanDescData rs_base;	/* Base scan descriptor */
	Buffer		rs_cbuf;		/* Current buffer */
	BlockNumber rs_cblock;		/* Current block */
	BlockNumber rs_nblocks;		/* Total blocks in relation (cached) */
	BlockNumber rs_startblock;	/* Starting block for sample scans */
	int			rs_cindex;		/* Current candidate index (seqscan), or
								 * offset */
	ScanDirection rs_dir;		/* Read stream direction */
	BlockNumber rs_endblock;	/* Exclusive upper scan bound */
	OffsetNumber rs_coffset;	/* Current offset number */
	bool		rs_inited;		/* True after first block is fetched */
	int			rs_ntuples;		/* Number of tuples on current page */
	OffsetNumber *rs_vistuples; /* Offset numbers of visible tuples */
	uint64		rs_snapshot_ts; /* Snapshot timestamp */
	uint64		rs_xact_ts;		/* Transaction timestamp */
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;	/* Parallel scan worker
																 * state */
	struct ReadStream *rs_read_stream;	/* Read stream for sequential
										 * prefetching */
	BlockNumber rs_prefetch_block;	/* Next block for read stream callback */

	/* Cached visibility map buffer to avoid per-page VM I/O */
	Buffer		rs_vm_buffer;	/* Pinned VM buffer (or InvalidBuffer) */
	BlockNumber rs_vm_blockno;	/* VM block number for rs_vm_buffer */

	/*
	 * A non-MVCC scan drops every DELETED tuple.  A SnapshotAny scan begun
	 * with SO_RETURN_DELETED (CLUSTER / VACUUM FULL, the apply worker's
	 * search for a deleted row), and an index build's, set this to be handed
	 * every version on the page, deleted ones included, and judge each
	 * themselves: a row deleted by a transaction some snapshot does not see
	 * yet is still that snapshot's (heap's RECENTLY_DEAD), and one whose
	 * deleter aborted with the revert still pending is alive.
	 */
	bool		rs_keep_deleted;

	/*
	 * ANALYZE dictionary-refresh sample accumulation.  During an ANALYZE scan
	 * the decompressed bytes of the first varlena column are gathered here so
	 * FluxMaybeRefreshDict() can train a candidate dictionary at scan end.
	 * All fields stay zero/NULL on non-ANALYZE scans.
	 */
	char	   *rs_dict_samplebuf;	/* Concatenated sample bytes */
	size_t	   *rs_dict_sizes;	/* Per-sample lengths, in order */
	int			rs_dict_nsamples;	/* Number of accumulated samples */
	Size		rs_dict_total;	/* Total bytes in rs_dict_samplebuf */
	Size		rs_dict_cap;	/* Capacity of rs_dict_samplebuf */
	int			rs_dict_maxsamples; /* Capacity of rs_dict_sizes */
	int16		rs_dict_attnum; /* 1-based varlena attr sampled, 0 = none */
} FluxScanDescData;

typedef FluxScanDescData *FluxScanDesc;

/*
 * Table AM private state for a FLUX index scan.
 *
 * Post-ddce1da5b1b (slot-based table-AM index scan interface): the table AM
 * keeps its own scan state in IndexScanDesc.xs_table_opaque, set up in
 * flux_index_scan_begin.  FLUX does in-place, stable-TID updates and never
 * walks a HOT-style chain, so all it needs is the relation and the pin held
 * across getnext_slot calls.
 */
typedef struct IndexScanFluxData
{
	Relation	rel;			/* the heap (FLUX) relation being scanned */
	Buffer		buffer;			/* pin held across getnext_slot calls */
	bool		keyless;		/* fetch_tid: the index entry is unknown */
	bool		keep_deleted;	/* SO_RETURN_DELETED: return deleted versions
								 * too, see FluxScanDescData.rs_keep_deleted */
} IndexScanFluxData;

/*
 * Constants
 */
#define FLUX_PAGE_OVERHEAD		(MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(FluxPageOpaqueData)))
#define SizeOfFluxTupleHeader offsetof(FluxTupleHeader, t_attrs_bitmap)
#define FLUX_TUPLE_OVERHEAD SizeOfFluxTupleHeader

/* Align the header and bitmap together before storing attribute data. */
StaticAssertDecl(SizeOfFluxTupleHeader == 36,
				 "unexpected FLUX tuple header layout");
StaticAssertDecl(offsetof(FluxTupleHeader, t_verptr) == sizeof(uint64),
				 "FLUX version pointer must remain aligned");
#define FLUX_MAX_TUPLE_SIZE	MAXALIGN_DOWN(BLCKSZ - FLUX_PAGE_OVERHEAD - sizeof(ItemIdData))
#define FLUX_OVERFLOW_THRESHOLD (FLUX_MAX_TUPLE_SIZE / 4)

/*
 * Hard ceiling on the line-pointer offsets of a FLUX page.  FLUX clamps
 * offsets exactly like heap (every PageAddItem passes PAI_IS_HEAP), so that
 * AM-agnostic code sized by MaxHeapTuplesPerPage -- tidbitmap.c's per-page
 * bitmaps in particular -- covers FLUX pages too.  Live tuples alone never
 * reach the clamp: every item carries a FLUX tuple header, so a page fills
 * by space first.  Only line pointers left LP_DEAD by pruning, which keep
 * their slot until VACUUM, can run the array up to it.
 */
#define MaxFluxItemsPerPage		MaxHeapTuplesPerPage
StaticAssertDecl((BLCKSZ - FLUX_PAGE_OVERHEAD) /
				 (MAXALIGN(SizeOfFluxTupleHeader) + sizeof(ItemIdData)) <=
				 MaxFluxItemsPerPage,
				 "FLUX page holds more tuples than MaxFluxItemsPerPage");

/*
 * Fill factor support.  Unlike heap, FLUX has STABLE TIDs and updates rows
 * IN PLACE -- a row that grows (e.g. an accumulating numeric like TPC-C
 * w_ytd/d_ytd gaining a digit) cannot be relocated to another page the way
 * heap moves a grown tuple.  It must fit on its home page or the UPDATE fails
 * with "does not fit".  So FLUX must reserve per-page headroom by default;
 * packing pages 100%% full (heap's default) guarantees that any in-place
 * growth on a full page aborts.  The default reserves ~10%% of each page,
 * which comfortably covers digit-growth of numeric columns plus transient
 * line-pointer bloat from concurrent updates to a hot page.  Users can raise
 * it with WITH (fillfactor=N) for append-mostly tables that never grow rows.
 */
#define FLUX_MIN_FILLFACTOR		10
#define FLUX_DEFAULT_FILLFACTOR	90

/* Macros for tuple access */
#define FluxTupleGetHeader(tuple) ((tuple)->t_data)
#define FluxTupleGetData(tuple) \
	((char *) (tuple)->t_data + FLUX_TUPLE_OVERHEAD)

/* Version-chain accessors. */
static inline UndoRecPtr
FluxTupleGetVersionPtr(const FluxTupleHeader *hdr)
{
	return hdr->t_verptr;
}

static inline void
FluxTupleSetVersionPtr(FluxTupleHeader *hdr, UndoRecPtr ptr)
{
	hdr->t_verptr = ptr;
}

/*
 * FluxReconstructVisibleVersion
 *
 * Walk the per-tuple version chain in the UNDO log to find the image
 * the reader's MVCC snapshot should see in place of the on-page (newer)
 * data.  Returns true and populates *out_data / *out_len with a palloc'd
 * reconstructed image when a step back was taken; returns false when the
 * on-page image is what the reader should see.
 *
 * See src/backend/access/flux/flux_pvs.c for the algorithm.
 */
extern void FluxSlotCacheSysAttrs(TupleTableSlot *slot,
								  const FluxTupleHeader *tuple);
extern bool FluxSlotGetVersion(TupleTableSlot *slot, TransactionId *xmin,
							   CommandId *cmin);
extern void FluxSlotSetVersion(TupleTableSlot *slot, TransactionId xmin,
							   CommandId cmin);
extern bool FluxFindVersionSuperseder(Relation rel, ItemPointer tid,
									  const char *onpage_image,
									  Size onpage_len,
									  TransactionId xmin, CommandId cmin,
									  TransactionId *sup_xid,
									  CommandId *sup_cid);
extern bool FluxReconstructVisibleVersion(Relation rel, ItemPointer tid,
										  const char *onpage_image,
										  Size onpage_len,
										  Snapshot snapshot,
										  char **out_data, int *out_len);

/*
 * The updates FluxReconstructVisibleVersionEx stepped back over; see
 * flux_pvs.c.
 */
typedef struct FluxSkippedUpdates
{
	TransactionId committed_xid;	/* newest committed update */
	TransactionId committed_key_xid;	/* newest committed key update */
	TransactionId inprogress_key_xid;	/* newest in-progress key update */
	bool		complete;		/* reached the snapshot's version */
} FluxSkippedUpdates;

extern bool FluxReconstructVisibleVersionEx(Relation rel, ItemPointer tid,
											const char *onpage_image,
											Size onpage_len,
											Snapshot snapshot,
											char **out_data, int *out_len,
											FluxSkippedUpdates *skipped);

/* Slot operations for FLUX tuples */
extern PGDLLIMPORT const TupleTableSlotOps TTSOpsFluxTuple;
extern void FluxSlotStoreTuple(TupleTableSlot *slot, FluxTupleHeader *tuple,
							   uint32 tuple_len, Buffer buffer);

#define TTS_IS_FLUXTUPLE(slot) ((slot)->tts_ops == &TTSOpsFluxTuple)

/* Function prototypes */
extern Size FluxComputeDataSize(TupleDesc tupdesc, Datum *values, bool *isnull);
extern void flux_toast_tuple(Relation rel, Datum *values, bool *isnull,
							 Datum *oldvalues, bool *oldisnull,
							 ToastTupleContext *ttc, ToastAttrInfo *toast_attr,
							 bool *changed, uint32 options);
extern void flux_toast_cleanup(ToastTupleContext *ttc);
extern void flux_toast_abandon(ToastTupleContext *ttc);
extern void flux_toast_delete(Relation rel, Datum *values, bool *isnull,
							  bool is_speculative);
extern FluxTuple FluxFormTuple(TupleDesc tupdesc, Datum *values, bool *isnull,
							   Relation rel, FluxOverflowBuffers *overflow_buffers);
extern FluxTuple FluxFormTupleForceShrink(TupleDesc tupdesc, Datum *values,
										  bool *isnull, Relation rel,
										  FluxOverflowBuffers *overflow_buffers);
extern FluxTuple FluxFormTupleUpdate(TupleDesc tupdesc, Datum *values,
									 bool *isnull, Relation rel,
									 FluxOverflowBuffers *overflow_buffers,
									 const FluxOverflowPtr *old_ovptrs,
									 const bool *old_ovpresent);
extern void FluxDeformTuple(Relation rel, FluxTuple tuple, TupleDesc tupdesc, Datum *values, bool *isnull);
extern void FluxDeformTupleUpTo(Relation rel, FluxTuple tuple, TupleDesc tupdesc, Datum *values, bool *isnull, int max_natts);
extern void FluxFreeTuple(FluxTuple tuple);
extern bool FluxTupleToSlot(FluxTupleHeader *tuple_header, TupleTableSlot *slot);
extern bool FluxTupleMovedPartition(FluxTupleHeader *tuple_hdr);
extern bool FluxTupleSuperseded(FluxTupleHeader *tuple_hdr);
extern bool FluxTupleToSlotWithOverflow(FluxTupleHeader *tuple_header,
										TupleTableSlot *slot, Relation rel);

/* Page management */
extern void FluxInitPage(Page page, Size pageSize);
extern OffsetNumber FluxPageAddTuple(Page page, FluxTuple tuple, Size tuple_size);
extern bool FluxPageUpdateTuple(Page page, OffsetNumber offnum, FluxTuple new_tuple,
								uint64 old_commit_ts, uint64 new_commit_ts);
extern int	FluxPageGetLiveTuples(Page page, uint64 snapshot_ts);
extern void FluxPageDefragment(Page page);
extern void FluxPageIndexTupleDelete(Page page, OffsetNumber offnum);
extern int	FluxPagePruneOpt(Relation rel, Buffer buffer);

/* Overflow handling - column-level overflow */
extern Datum FluxStoreOverflowColumn(Relation rel, Datum value, int attnum,
									 Size inline_prefix_size,
									 FluxOverflowBuffers *overflow_buffers);
extern Datum FluxFetchOverflowColumn(Relation rel, const void *overflow_varlena);
extern void FluxDeleteOverflowChain(Relation rel, BlockNumber first_block,
									OffsetNumber first_offset);
extern int	FluxCollectOverflowPtrs(FluxTupleHeader *tuple_hdr,
									TupleDesc tupdesc,
									BlockNumber *blocks, OffsetNumber *offsets,
									int max_ptrs);
extern void FluxCollectOverflowPtrsByAttr(FluxTupleHeader *tuple_hdr,
										  TupleDesc tupdesc,
										  FluxOverflowPtr *out_ptrs,
										  bool *out_present, int natts);
extern void FluxDeleteTupleOverflows(Relation rel, FluxTupleHeader *tuple_hdr,
									 TupleDesc tupdesc);
extern bool FluxIsOverflowRecord(const void *item, Size item_len);

/*
 * Inline version of FluxIsOverflowRecord for hot scan paths.
 * Checks whether an item is an overflow continuation record by testing
 * the magic number in the header.
 */
static inline bool
FluxIsOverflowRecordInline(const void *item, Size item_len)
{
	if (item_len < sizeof(FluxOverflowRecordHeader))
		return false;
	return ((const FluxOverflowRecordHeader *) item)->or_magic ==
		FLUX_OVERFLOW_RECORD_MAGIC;
}
extern void FluxGetOverflowStats(Relation rel, int64 *total_overflow_records,
								 int64 *total_overflow_bytes, int64 *avg_chain_length);

/* Free space management */
extern Size FluxTargetFreeSpace(Relation rel, Size len);
extern BlockNumber FluxGetPageWithFreeSpace(Relation rel, Size needed);
extern void FluxRecordFreeSpace(Relation rel, BlockNumber page, Size freespace);

/* Visibility Map management */
extern void FluxVMInit(Relation rel);
extern void FluxVMSet(Relation rel, BlockNumber heapBlk, Buffer heapBuf, uint8 flags, TransactionId conflict_xid);
extern void FluxVMClear(Relation rel, BlockNumber heapBlk, Buffer heapBuf, uint8 flags);
extern bool FluxVMCheck(Relation rel, BlockNumber heapBlk, uint8 flags);
extern bool FluxVMCheckCached(Relation rel, BlockNumber heapBlk, uint8 flags,
							  Buffer *vmbuf, BlockNumber *vm_blockno);
extern void FluxVMPinBuffer(Relation rel, BlockNumber heapBlk, Buffer *vmbuf);
extern void FluxVMExtend(Relation rel, BlockNumber nheapblocks);
extern Size FluxVMGetPageSize(void);
extern BlockNumber FluxVMMapHeapToVM(BlockNumber heapBlk);
extern Buffer FluxVMPinForChange(Relation rel, Buffer heapBuf);
extern bool FluxVMNeedsClear(Relation rel, Buffer heapBuf);
extern void FluxVMClearForChange(Buffer heapBuf, Buffer vmbuf);
extern void FluxVMRegisterClear(uint16 *flags);
extern void FluxVMFinishClear(XLogRecPtr lsn);
extern bool FluxVMClearMapBits(Page vmpage, BlockNumber heapBlk);
extern void FluxVMVacuumPage(Relation rel, Buffer buffer, bool all_visible, bool all_frozen, TransactionId conflict_xid);

/* MVCC functions */
extern uint64 FluxGetCommitTimestamp(void);
extern uint64 FluxGetTransactionTimestamp(void);
extern uint64 FluxGetOldestActiveTimestamp(void);
extern Size FluxMvccShmemSize(void);
extern void FluxMvccShmemInit(void);
extern const ShmemCallbacks FluxMvccShmemCallbacks;
extern void FluxCommitTransaction(void);
extern void FluxAbortTransaction(void);
extern uint64 FluxGetSnapshotTimestamp(Snapshot snapshot);
extern void FluxUpdateOldestActiveTimestamp(void);
extern void FluxGetMvccStats(uint64 *current_ts, uint64 *oldest_ts, int *active_xacts);

/* SSI (Serializable Snapshot Isolation) via predicate.c integration */
extern void FluxCheckForSerializableConflictOut(bool visible,
												Relation relation,
												FluxTupleHeader *tuple,
												Buffer buffer,
												Snapshot snapshot);
extern void FluxSerializableConflictOutXid(Relation relation,
										   TransactionId xid,
										   Snapshot snapshot);

/* MVCC timestamp helpers (page-level bookkeeping; visibility uses xmin/xmax) */
extern uint64 FluxGetDmlTimestamp(void);

/*
 * Lost-update conflict probe.  Reads the head verptr
 * from the on-page tuple, resolves it in the UNDO log, and reports whether
 * that head record's committer is concurrent-or-later to snapshot (and not
 * exclude_xid).  If it is, the caller returns TM_Updated to drive EPQ.  EPQ
 * dedup lives in FluxTransactionState: FluxEpqReconcileMatches skips a
 * probe we already bounced on for this exact (relid, tid, cid, head verptr,
 * head xid); FluxEpqReconcileMark stamps the identity we just bounced on.
 */
extern bool FluxTupleHasCommittedUpdateAfter(Relation rel,
											 const FluxTupleHeader *tuple,
											 Size tuple_len,
											 Snapshot snapshot,
											 TransactionId exclude_xid,
											 UndoRecPtr *out_head_verptr,
											 TransactionId *out_head_xid,
											 bool *out_inprogress);
extern bool FluxEpqReconcileMatches(Snapshot snapshot, Oid relid,
									ItemPointer tid);
extern void FluxEpqReconcileMark(Snapshot snapshot, Oid relid,
								 ItemPointer tid,
								 UndoRecPtr head_verptr,
								 TransactionId head_xid);
extern bool FluxTupleSatisfiesLatest(FluxTupleHeader *tuple, Oid relid,
									 CommandId curcid, Buffer buffer,
									 ItemPointer tid);
extern bool FluxTupleVisibleToSnapshotDual(FluxTupleHeader *tuple,
										   Snapshot snapshot,
										   Oid relid, Buffer buffer,
										   ItemPointer tid);
extern bool FluxWriterVisibleToSnapshot(TransactionId xid, Snapshot snapshot,
										bool known_committed);
extern TransactionId FluxGetOldestXminHorizon(Relation rel);
extern TransactionId FluxVersionChainOldestXid(Relation rel, ItemPointer tid,
											   const char *image, Size len,
											   TransactionId oldest);

/* An older version of a row that a rewrite copies; see FluxRewriteChain. */
typedef struct FluxChainVersion
{
	char	   *image;			/* the version, palloc'd */
	Size		len;
	TransactionId xmax;			/* the writer that replaced it */
} FluxChainVersion;

extern List *FluxRewriteChain(Relation rel, ItemPointer tid,
							  const char *image, Size len,
							  TransactionId oldest_xmin);

extern void FluxTupleAdvanceConflictHorizon(FluxTupleHeader *tuple,
											TransactionId *horizon);
extern bool FluxTupleDeadToAll(FluxTupleHeader *tuple,
							   TransactionId oldest_xmin);

/* How VACUUM and ANALYZE count a tuple, see FluxTupleStatsCount */
typedef enum FluxStatsCount
{
	FLUX_COUNT_NONE,			/* not counted: another xact is inserting it */
	FLUX_COUNT_LIVE,
	FLUX_COUNT_DEAD,
} FluxStatsCount;

extern FluxStatsCount FluxTupleStatsCount(FluxTupleHeader *tuple);

/*
 * FLUX uses no MultiXacts: row lockers are kept in the tuple sLog
 * (SLogTupleLock*).
 */

extern bool flux_lazy_uncommitted_clear;


/* Lock operations */
extern bool FluxLockTuple(Relation rel, ItemPointer tid, LockTupleMode mode,
						  bool wait, bool *have_tuple_lock);
extern void FluxUnlockTuple(Relation rel, ItemPointer tid, LockTupleMode mode);
extern void FluxLockTupleToWait(Relation rel, ItemPointer tid,
								LockTupleMode mode, bool *have_tuple_lock);
extern void FluxLockPage(Relation rel, BlockNumber blkno, LOCKMODE mode);
extern void FluxUnlockPage(Relation rel, BlockNumber blkno, LOCKMODE mode);
extern bool FluxLockMultipleTuples(Relation rel, ItemPointerData *tids, int ntids,
								   LockTupleMode mode, bool wait);
extern void FluxLockRelationForDDL(Relation rel, LOCKMODE lockmode);
extern bool FluxHoldsTupleLock(Relation rel, ItemPointer tid, LockTupleMode mode);

/* Table operations */
extern void flux_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
							  uint32 options, BulkInsertState bistate);
extern TM_Result flux_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
								   uint32 options, Snapshot snapshot, Snapshot crosscheck,
								   bool wait, TM_FailureData *tmfd);
extern TM_Result flux_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
								   CommandId cid, uint32 options,
								   Snapshot snapshot, Snapshot crosscheck,
								   bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
								   TU_UpdateIndexes *update_indexes);

/*
 * Reverse an in-place indexed-column UPDATE's index changes during
 * ROLLBACK.  Called from flux_undo.c after the before-image tuple has been
 * restored: 'new_hdr' is the current (post-update, k_new) on-page tuple read
 * before the restore; 'old_hdr' is the restored before-image (k_old).  For each
 * index whose key differed it kills the new (k_new,TID) entry and clears the
 * delete-mark on the old (k_old,TID) entry.
 */
extern bool flux_inplace_index_undo(Relation relation, ItemPointer tid,
									FluxTupleHeader *new_hdr, Size new_len,
									FluxTupleHeader *old_hdr, Size old_len,
									const char *indexes, uint16 nindexes,
									TransactionId xid);
extern void flux_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
							  CommandId cid, uint32 options, BulkInsertState bistate);
extern void flux_relation_vacuum(Relation onerel, const VacuumParams *params,
								 BufferAccessStrategy bstrategy);
extern const TableAmRoutine *GetFluxTableAmRoutine(void);


/* In-place update statistics */
extern void FluxGetUpdateStats(int64 *in_place, int64 *out_of_place,
							   int64 *defrag_triggered);

/*
 * FLUX-specific ANALYZE statistics
 *
 * These statistics capture properties unique to the FLUX storage format
 * and are collected during ANALYZE.  They are stored in the relation's
 * pg_class.reloptions and consumed by the planner to improve cost estimates.
 */
typedef struct FluxRelationStats
{
	/* Compression effectiveness */
	double		compression_ratio;	/* avg uncompressed/compressed size */
	double		pct_compressed; /* fraction of tuples that are compressed */

	/* Overflow usage */
	double		pct_overflow;	/* fraction of tuples with overflow attrs */
	double		avg_overflow_chain_len; /* avg overflow records per overflow
										 * tuple */
	int64		total_overflow_bytes;	/* total bytes in overflow records */

	/* Space efficiency */
	double		avg_tuple_size; /* average on-disk tuple size (bytes) */
	double		avg_live_per_page;	/* average live tuples per page */
	double		free_space_frac;	/* average fraction of free space per page */
	double		bloat_factor;	/* allocated space / live data ratio */

	/* Page-level summary */
	int64		total_pages;	/* total pages in relation */
	int64		total_live_tuples;	/* total live tuples counted */
	int64		total_dead_tuples;	/* total dead tuples counted */

	/* Per-tuple commit-ts word distribution */
	bool		commit_ts_stats_valid;	/* true if commit-ts word fields are
										 * populated */
	uint64		commit_ts_min;	/* min commit-ts word seen */
	uint64		commit_ts_max;	/* max commit-ts word seen */
} FluxRelationStats;

/* ANALYZE statistics collection (flux_stats.c) */
extern void FluxCollectRelationStats(Relation rel, FluxRelationStats *stats);
extern void FluxLogRelationStats(Relation rel, const FluxRelationStats *stats,
								 int elevel);

/* sLog transaction callbacks (flux_operations.c) */
extern void FluxEnsureSLogCallbacks(void);

/*
 * Backend-local command-id map for the current transaction's own in-place
 * UPDATE/DELETE ops (replaces the removed on-tuple t_cid and the per-tuple
 * sLog UPDATE/DELETE cid lookup).  Used by self-visibility (flux_mvcc.c) and
 * the cmin/cmax system columns (flux_slot.c).
 */
extern CommandId FluxLocalCidGet(Oid relid, ItemPointer tid);
extern void FluxLocalCidReset(void);

/*
 * Derive the in-progress non-current writer XID for a tuple from its on-page
 * header (t_xmin inserter / t_xmax updater-deleter).  Caller must hold the
 * tuple's buffer lock.  Replaces the per-tuple sLog conflict probe for the
 * common in-place UPDATE/DELETE conflict-detection paths.
 */
extern bool flux_help_revert(Relation relation, Buffer buffer,
							 OffsetNumber offnum);
extern TransactionId flux_header_dirty_xid(FluxTupleHeader *tuple_hdr,
										   bool *is_insert);

/*
 * Was this tuple already modified by OUR OWN transaction in this or a later
 * command?  This is FLUX's equivalent of the TM_SelfModified verdict heap
 * derives from HeapTupleSatisfiesUpdate(); see flux_operations.c.  Caller must
 * hold the tuple's buffer lock.
 */
extern bool FluxCheckSelfModified(Relation relation, ItemPointer tid,
								  FluxTupleHeader *tuple_hdr, CommandId cid,
								  CommandId *cmax_out);

/* Two-phase commit support (flux_operations.c) */
extern void AtPrepare_Flux(void);
extern void flux_twophase_postcommit(FullTransactionId fxid, uint16 info,
									 void *recdata, uint32 len);
extern void flux_twophase_postabort(FullTransactionId fxid, uint16 info,
									void *recdata, uint32 len);
extern void flux_twophase_recover(FullTransactionId fxid, uint16 info,
								  void *recdata, uint32 len);


#endif							/* FLUX_H */
