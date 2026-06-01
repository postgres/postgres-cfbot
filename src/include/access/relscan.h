/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/relscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/htup_details.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "nodes/tidbitmap.h"
#include "port/atomics.h"
#include "storage/relfilelocator.h"
#include "storage/spin.h"
#include "utils/relcache.h"


struct ParallelTableScanDescData;
struct TableScanInstrumentation;
struct TupleTableSlot;

/*
 * Generic descriptor for table scans. This is the base-class for table scans,
 * which needs to be embedded in the scans of individual AMs.
 */
typedef struct TableScanDescData
{
	/* scan parameters */
	Relation	rs_rd;			/* heap relation descriptor */
	struct SnapshotData *rs_snapshot;	/* snapshot to see */
	int			rs_nkeys;		/* number of scan keys */
	struct ScanKeyData *rs_key; /* array of scan key descriptors */

	/*
	 * Scan type-specific members
	 */
	union
	{
		/* Iterator for Bitmap Table Scans */
		TBMIterator rs_tbmiterator;

		/*
		 * Range of ItemPointers for table_scan_getnextslot_tidrange() to
		 * scan.
		 */
		struct
		{
			ItemPointerData rs_mintid;
			ItemPointerData rs_maxtid;
		}			tidrange;
	}			st;

	/*
	 * Information about type and behaviour of the scan, a bitmask of members
	 * of the ScanOptions enum (see tableam.h).
	 */
	uint32		rs_flags;

	struct ParallelTableScanDescData *rs_parallel;	/* parallel scan
													 * information */

	/*
	 * Instrumentation counters maintained by all table AMs.
	 */
	struct TableScanInstrumentation *rs_instrument;
} TableScanDescData;
typedef struct TableScanDescData *TableScanDesc;

/*
 * Shared state for parallel table scan.
 *
 * Each backend participating in a parallel table scan has its own
 * TableScanDesc in backend-private memory, and those objects all contain a
 * pointer to this structure.  The information here must be sufficient to
 * properly initialize each new TableScanDesc as workers join the scan, and it
 * must act as a information what to scan for those workers.
 */
typedef struct ParallelTableScanDescData
{
	RelFileLocator phs_locator; /* physical relation to scan */
	bool		phs_syncscan;	/* report location to syncscan logic? */
	bool		phs_snapshot_any;	/* SnapshotAny, not phs_snapshot_data? */
	Size		phs_snapshot_off;	/* data for snapshot */
} ParallelTableScanDescData;
typedef struct ParallelTableScanDescData *ParallelTableScanDesc;

/*
 * Shared state for parallel table scans, for block oriented storage.
 */
typedef struct ParallelBlockTableScanDescData
{
	ParallelTableScanDescData base;

	BlockNumber phs_nblocks;	/* # blocks in relation at start of scan */
	slock_t		phs_mutex;		/* mutual exclusion for setting startblock */
	BlockNumber phs_startblock; /* starting block number */
	BlockNumber phs_numblock;	/* # blocks to scan, or InvalidBlockNumber if
								 * no limit */
	pg_atomic_uint64 phs_nallocated;	/* number of blocks allocated to
										 * workers so far. */
}			ParallelBlockTableScanDescData;
typedef struct ParallelBlockTableScanDescData *ParallelBlockTableScanDesc;

/*
 * Per backend state for parallel table scan, for block-oriented storage.
 */
typedef struct ParallelBlockTableScanWorkerData
{
	uint64		phsw_nallocated;	/* Current # of blocks into the scan */
	uint32		phsw_chunk_remaining;	/* # blocks left in this chunk */
	uint32		phsw_chunk_size;	/* The number of blocks to allocate in
									 * each I/O chunk for the scan */
} ParallelBlockTableScanWorkerData;
typedef struct ParallelBlockTableScanWorkerData *ParallelBlockTableScanWorker;

/*
 * Data structures used by amgetbatch index scans.
 *
 * These structs are defined here, rather than in access/indexbatch.h, only
 * because IndexScanDescData embeds them by value.  relscan.h defines data
 * structures only; all of the functions that operate on them appear in
 * access/indexbatch.h.
 */

/*
 * Location of a BatchMatchingItem within the scan's ring buffer
 */
typedef struct BatchRingItemPos
{
	/* Position references a valid IndexScanDescData.batchbuf[] entry? */
	bool		valid;

	/* IndexScanDescData.batchbuf[]-wise index to relevant IndexScanBatch */
	uint8		batch;

	/* IndexScanBatch.items[]-wise index to relevant BatchMatchingItem */
	int			item;

} BatchRingItemPos;

/*
 * Matching item returned by amgetbatch (in returned IndexScanBatch) during an
 * index scan.  Used by table AM to locate relevant matching table tuple.
 */
typedef struct BatchMatchingItem
{
	ItemPointerData tableTid;	/* TID of referenced table item */
	OffsetNumber indexOffset;	/* index item's location within page */
	LocationIndex tupleOffset;	/* index tuple's currTuples offset, if any */
} BatchMatchingItem;

/*
 * Data about one batch of items returned by (and passed to) amgetbatch during
 * index scans.
 *
 * The batch pointer returned by amgetbatch points into the interior of a
 * larger allocation, which also carries opaque areas for the table AM and
 * the index AM.  See access/indexbatch.h for the layout of batch allocations
 * and for the accessors used to reach each constituent area.
 */
typedef struct IndexScanBatchData
{
	/* Index page's LSN, optionally used by amkillitemsbatch routines */
	XLogRecPtr	lsn;

	/* scan direction when the index page was read */
	ScanDirection dir;

	/*
	 * knownEndBackward and knownEndForward indicate that this batch is the
	 * last one with matching items in the relevant scan direction.  When
	 * amgetbatch returns NULL for a given direction, the corresponding flag
	 * is set on the priorbatch that was passed to that call.  Index AMs may
	 * also set either flag proactively when amgetbatch returns a batch that
	 * is provably the last (or the first) batch in the current direction.
	 *
	 * This allows table AMs to avoid redundant amgetbatch calls with the same
	 * priorbatch -- the index AM might need to read additional index pages to
	 * determine there are no more matching items beyond caller's priorbatch.
	 */
	bool		knownEndBackward;
	bool		knownEndForward;

	/*
	 * Batch still holds TID recycling interlock?
	 */
	bool		isGuarded;

	/*
	 * Matching items state for this batch.  Output by index AM for table AM.
	 *
	 * The items array is always ordered in index order (ie, by increasing
	 * indexoffset).  When scanning backwards it is convenient for index AMs
	 * to fill the array back-to-front, starting at the last item slot and
	 * filling downwards.  This is why we need both a first-valid-entry and a
	 * last-valid-entry counter.
	 *
	 * Note: these are signed because it's sometimes convenient to use -1 to
	 * represent an out-of-bounds space just before firstItem (when it's 0).
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */

	/* info about dead items, if any (palloc'd separately, NULL if unused) */
	int			numDead;		/* number of currently stored items */
	int		   *deadItems;		/* items[]-wise indexes of dead items */

	/*
	 * If we are doing an index-only scan, this is the tuple storage workspace
	 * for the matching tuples (tuples referenced by items[]).  The workspace
	 * size is determined by the index AM (batch_tuples_workspace).
	 *
	 * currTuples points into the trailing portion of this allocation,
	 * directly past items[].  It is NULL for plain index scans.
	 */
	char	   *currTuples;		/* tuple storage for items[] */
	BatchMatchingItem items[FLEXIBLE_ARRAY_MEMBER]; /* matching items */
} IndexScanBatchData;

typedef struct IndexScanBatchData *IndexScanBatch;

/*
 * State used by table AMs to manage an index scan that uses the amgetbatch
 * interface.  Scans use a ring buffer of batches returned by amgetbatch.
 *
 * This data structure provides table AMs with a way to read ahead of the
 * current read position by _multiple_ batches/index pages.  The further out
 * the table AM reads ahead like this, the further it can see into the future.
 * That way the table AM is able to reorder work as aggressively as desired.
 */
typedef struct BatchRingBuffer
{
	/* current positions in IndexScanDescData.batchbuf[] for scan */
	BatchRingItemPos scanPos;	/* scan's read position */
	BatchRingItemPos markPos;	/* mark/restore position */

	/* markPos's batch (not in ring buffer when markBatch != scanBatch) */
	IndexScanBatch markBatch;

	/*
	 * headBatch is an index to the earliest still-valid ring buffer batch
	 * slot in batchbuf[].  The actual array position for its IndexScanBatch
	 * is headBatch & (INDEX_SCAN_MAX_BATCHES - 1), since these indexes use
	 * unsigned wrapping arithmetic.  headBatch must be the scan's current
	 * scanBatch (i.e. the current scanPos batch).
	 */
	uint8		headBatch;

	/*
	 * nextBatch is an index to the next _empty_ ring buffer batch slot in
	 * batchbuf[] (i.e. it's the tail entry of our ring buffer).  The actual
	 * batchbuf[] array position is nextBatch & (INDEX_SCAN_MAX_BATCHES - 1).
	 * New batches can only be safely appended to this tail position when
	 * !index_scan_batch_full() (see access/indexbatch.h).
	 *
	 * Note: the scan's most recently appended batch is always located at
	 * (nextBatch - 1) & (INDEX_SCAN_MAX_BATCHES - 1).
	 */
	uint8		nextBatch;
} BatchRingBuffer;

struct IndexScanInstrumentation;

/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetbitmap-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.  Others are only used in amgetbatch-based scans.
 *
 * The ring buffer used by amgetbatch scans is stored here as a fixed array of
 * pointers to batches.  We need a minimum of two ring buffer batches (but use
 * INDEX_SCAN_MAX_BATCHES), since table AMs only remove a batch after they've
 * already called amgetbatch again and appended the returned batch.
 */
#define INDEX_SCAN_CACHE_BATCHES	2
#define INDEX_SCAN_MAX_BATCHES		64

typedef struct IndexScanDescData
{
	/* scan parameters */
	Relation	heapRelation;	/* heap relation descriptor, or NULL */
	Relation	indexRelation;	/* index relation descriptor */
	struct SnapshotData *xs_snapshot;	/* snapshot to see */
	int			numberOfKeys;	/* number of index qualifier conditions */
	int			numberOfOrderBys;	/* number of ordering operators */
	struct ScanKeyData *keyData;	/* array of index qualifier descriptors */
	struct ScanKeyData *orderByData;	/* array of ordering op descriptors */

	/* index access method's private state */
	void	   *opaque;			/* access-method-specific info */

	/* scan's amgetbatch state (only used by amgetbatch/usebatchring scans) */
	BatchRingBuffer batchringbuf;

	/*
	 * Array of pointers to recyclable batches, used by all amgetbatch scans
	 * and by amgetbitmap scans of an index AM that supports amgetbatch
	 */
	IndexScanBatch batchcache[INDEX_SCAN_CACHE_BATCHES];

	/* Array of pointers to batches, referenced within batchringbuf */
	IndexScanBatch batchbuf[INDEX_SCAN_MAX_BATCHES];

	bool		usebatchring;	/* scan uses amgetbatch/batchringbuf? */
	bool		batchImmediateUnguard;	/* eagerly drop TID recycling
										 * interlock? */

	bool		xs_want_itup;	/* caller requests index tuples */
	bool		xs_temp_snap;	/* unregister snapshot at scan end? */

	/* signaling to index AM about killing index tuples */
	bool		kill_prior_tuple;	/* last-returned tuple is dead */
	bool		ignore_killed_tuples;	/* do not return killed entries */
	bool		xactStartedInRecovery;	/* prevents killing/seeing killed
										 * tuples */
	/* xs_snapshot uses an MVCC snapshot? */
	bool		MVCCScan;

	/*
	 * Instrumentation counters maintained during amgetbatch, amgetbitmap, and
	 * amgettuple scans (unless field remains NULL)
	 */
	struct IndexScanInstrumentation *instrument;

	/*
	 * In an index-only scan, the index AM fills either xs_itup or xs_hitup
	 * with the data to be returned by the scan (it can fill both, in which
	 * case the heap format is used).  The table AM consumes these to fill the
	 * caller's slot during table_index_getnext_slot.
	 */
	IndexTuple	xs_itup;		/* index tuple returned by AM */
	struct TupleDescData *xs_itupdesc;	/* rowtype descriptor of xs_itup */
	HeapTuple	xs_hitup;		/* index data returned by AM, as HeapTuple */
	struct TupleDescData *xs_hitupdesc; /* rowtype descriptor of xs_hitup */

	ItemPointerData xs_heaptid; /* result */
	bool		xs_heap_continue;	/* T if must keep walking, potential
									 * further results */

	bool		xs_recheck;		/* T means scan keys must be rechecked */

	/* Table access method's private state (not used during bitmap scans) */
	void	   *xs_table_opaque;

	/*
	 * Resolved table_index_getnext_slot callback, which is set by
	 * table_index_scan_begin at the start of amgetbatch/amgettuple scans.
	 */
	bool		(*xs_getnext_slot) (struct IndexScanDescData *scan,
									ScanDirection direction,
									struct TupleTableSlot *slot);

	/* batch size information, set once by index AM in ambeginscan */
	uint16		maxitemsbatch;	/* size of each batch's items[] array */
	uint16		batch_index_opaque_static;	/* compile-time opaque size */
	uint16		batch_tuples_workspace; /* currTuples workspace size */

	/*
	 * Optional table AM per-batch opaque area size, set once by
	 * index_scan_begin (except during bitmap scans)
	 */
	uint32		batch_table_opaque_size;	/* table AM opaque area size */

	/*
	 * Optional dynamic opaque size, also set by index AM in ambeginscan
	 */
	uint32		batch_index_opaque_dyn;

	/*
	 * Offset used by index_scan_batch_base (set on first batch alloc).  See
	 * access/indexbatch.h.
	 */
	size_t		batch_base_offset;

	/*
	 * When fetching with an ordering operator, the values of the ORDER BY
	 * expressions of the last returned tuple, according to the index.  If
	 * xs_recheckorderby is true, these need to be rechecked just like the
	 * scan keys, and the values returned here are a lower-bound on the actual
	 * values.
	 */
	Datum	   *xs_orderbyvals;
	bool	   *xs_orderbynulls;
	bool		xs_recheckorderby;

	/*
	 * Index attributes holding "name" columns stored as cstrings, which the
	 * table AM re-pads to NAMEDATALEN when filling a slot from xs_itup.  The
	 * re-padded allocations live in xs_name_cstring_cxt, which the table AM
	 * resets before filling each slot.
	 */
	AttrNumber *xs_name_cstring_attnums;
	int			xs_name_cstring_count;
	MemoryContext xs_name_cstring_cxt;

	/*
	 * Approximate limits on the amount of work, measured in pages touched,
	 * imposed on the index scan.  The default, 0, means no limit.  Only
	 * index-only scans may set a limit (plain index scans leave these zero).
	 * Used by selfuncs.c to bound the cost of get_actual_variable_endpoint().
	 */
	uint8		xs_visited_pages_limit;
	uint8		xs_index_pages_limit;

	/* parallel index scan information, in shared memory */
	struct ParallelIndexScanDescData *parallel_scan;
} IndexScanDescData;

/* Generic structure for parallel scans */
typedef struct ParallelIndexScanDescData
{
	RelFileLocator ps_locator;	/* physical table relation to scan */
	RelFileLocator ps_indexlocator; /* physical index relation to scan */
	Size		ps_offset_am;	/* Offset to am-specific structure */
	char		ps_snapshot_data[FLEXIBLE_ARRAY_MEMBER];
}			ParallelIndexScanDescData;

/* Struct for storage-or-index scans of system tables */
typedef struct SysScanDescData
{
	Relation	heap_rel;		/* catalog being scanned */
	Relation	irel;			/* NULL if doing heap scan */
	struct TableScanDescData *scan; /* only valid in storage-scan case */
	struct IndexScanDescData *iscan;	/* only valid in index-scan case */
	struct SnapshotData *snapshot;	/* snapshot to unregister at end of scan */
	struct TupleTableSlot *slot;
} SysScanDescData;

#endif							/* RELSCAN_H */
