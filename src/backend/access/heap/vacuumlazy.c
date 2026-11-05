/*-------------------------------------------------------------------------
 *
 * vacuumlazy.c
 *	  Concurrent ("lazy") vacuuming.
 *
 * Heap relations are vacuumed in three main phases. In phase I, vacuum scans
 * relation pages, pruning and freezing tuples and saving dead tuples' TIDs in
 * a TID store. If that TID store fills up or vacuum finishes scanning the
 * relation, it progresses to phase II: index vacuuming. Index vacuuming
 * deletes the dead index entries referenced in the TID store. In phase III,
 * vacuum scans the blocks of the relation referred to by the TIDs in the TID
 * store and reaps the corresponding dead items, freeing that space for future
 * tuples.
 *
 * If there are no indexes or index scanning is disabled, phase II may be
 * skipped. If phase I identified very few dead index entries or if vacuum's
 * failsafe mechanism has triggered (to avoid transaction ID wraparound),
 * vacuum may skip phases II and III.
 *
 * If the TID store fills up in phase I, vacuum suspends phase I and proceeds
 * to phases II and III, cleaning up the dead tuples referenced in the current
 * TID store. This empties the TID store, allowing vacuum to resume phase I.
 *
 * In a way, the phases are more like states in a state machine, but they have
 * been referred to colloquially as phases for so long that they are referred
 * to as such here.
 *
 * Phase I and the index phases may each use parallel workers. See "Parallel
 * Heap Scan" below, and the comment at the top of vacuumparallel.c for the
 * index phases.
 *
 * In between phases, vacuum updates the freespace map (every
 * VACUUM_FSM_EVERY_PAGES).
 *
 * After completing all three phases, vacuum may truncate the relation if it
 * has emptied pages at the end. Finally, vacuum updates relation statistics
 * in pg_class and the cumulative statistics subsystem.
 *
 * Relation Scanning:
 *
 * Vacuum scans the heap relation, starting at the beginning and progressing
 * to the end, skipping pages as permitted by their visibility status, vacuum
 * options, and various other requirements.
 *
 * Vacuums are either aggressive or normal. Aggressive vacuums must scan every
 * unfrozen tuple in order to advance relfrozenxid and avoid transaction ID
 * wraparound. Normal vacuums may scan otherwise skippable pages for one of
 * two reasons:
 *
 * When page skipping is not disabled, a normal vacuum may scan pages that are
 * marked all-visible (and even all-frozen) in the visibility map if the range
 * of skippable pages is below SKIP_PAGES_THRESHOLD. This is primarily for the
 * benefit of kernel readahead (see comment in heap_vac_scan_next_block()).
 *
 * A normal vacuum may also scan skippable pages in an effort to freeze them
 * and decrease the backlog of all-visible but not all-frozen pages that have
 * to be processed by the next aggressive vacuum. These are referred to as
 * eagerly scanned pages. Pages scanned due to SKIP_PAGES_THRESHOLD do not
 * count as eagerly scanned pages.
 *
 * Eagerly scanned pages that are set all-frozen in the VM are successful
 * eager freezes and those not set all-frozen in the VM are failed eager
 * freezes.
 *
 * Because we want to amortize the overhead of freezing pages over multiple
 * vacuums, normal vacuums cap the number of successful eager freezes to
 * MAX_EAGER_FREEZE_SUCCESS_RATE of the number of all-visible but not
 * all-frozen pages at the beginning of the vacuum. Since eagerly frozen pages
 * may be unfrozen before the next aggressive vacuum, capping the number of
 * successful eager freezes also caps the downside of eager freezing:
 * potentially wasted work.
 *
 * Once the success cap has been hit, eager scanning is disabled for the
 * remainder of the vacuum of the relation.
 *
 * Success is capped globally because we don't want to limit our successes if
 * old data happens to be concentrated in a particular part of the table. This
 * is especially likely to happen for append-mostly workloads where the oldest
 * data is at the beginning of the unfrozen portion of the relation.
 *
 * On the assumption that different regions of the table are likely to contain
 * similarly aged data, normal vacuums use a localized eager freeze failure
 * cap. The failure count is reset for each region of the table -- comprised
 * of EAGER_SCAN_REGION_SIZE blocks. In each region, we tolerate
 * vacuum_max_eager_freeze_failure_rate of EAGER_SCAN_REGION_SIZE failures
 * before suspending eager scanning until the end of the region.
 * vacuum_max_eager_freeze_failure_rate is configurable both globally and per
 * table.
 *
 * Aggressive vacuums must examine every unfrozen tuple and thus are not
 * subject to any of the limits imposed by the eager scanning algorithm.
 *
 * Once vacuum has decided to scan a given block, it must read the block and
 * obtain a cleanup lock to prune tuples on the page. A non-aggressive vacuum
 * may choose to skip pruning and freezing if it cannot acquire a cleanup lock
 * on the buffer right away. In this case, it may miss cleaning up dead tuples
 * and their associated index entries (though it is free to reap any existing
 * dead items on the page).
 *
 * After pruning and freezing, pages that are newly all-visible and all-frozen
 * are marked as such in the visibility map.
 *
 * Parallel Heap Scan:
 *
 * Phase I can be done by parallel workers. The leader creates a parallel
 * context of its own, separate from the one vacuumparallel.c creates for the
 * index phases, and both share the dead items TID store and the cost-based
 * vacuum delay.
 *
 * Each process, the leader included, claims PARALLEL_LV_CHUNK_SIZE blocks at
 * a time and scans them like a serial vacuum. The cutoffs come from the
 * leader, and each process fills its own LVScanData for the leader to
 * combine. When the dead items reach the memory limit, which a process checks
 * only between chunks, the leader vacuums the indexes and the heap and
 * launches workers again for the blocks that are left.
 *
 * Dead TID Storage:
 *
 * The major space usage for vacuuming is storage for the dead tuple IDs that
 * are to be removed from indexes.  We want to ensure we can vacuum even the
 * very largest relations with finite memory space usage.  To do that, we set
 * upper bounds on the memory that can be used for keeping track of dead TIDs
 * at once.
 *
 * We are willing to use at most maintenance_work_mem (or perhaps
 * autovacuum_work_mem) memory space to keep track of dead TIDs.  If the
 * TID store is full, we must call lazy_vacuum to vacuum indexes (and to vacuum
 * the pages that we've pruned). This frees up the memory space dedicated to
 * store dead TIDs.
 *
 * In practice VACUUM will often complete its initial pass over the target
 * heap relation without ever running out of space to store TIDs.  This means
 * that there only needs to be one call to lazy_vacuum, after the initial pass
 * completes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/vacuumlazy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/tidstore.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/storage.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "common/int.h"
#include "common/pg_prng.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "optimizer/paths.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/read_stream.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/pg_rusage.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"


/*
 * Space/time tradeoff parameters: do these need to be user-tunable?
 *
 * To consider truncating the relation, we want there to be at least
 * REL_TRUNCATE_MINIMUM or (relsize / REL_TRUNCATE_FRACTION) (whichever
 * is less) potentially-freeable pages.
 */
#define REL_TRUNCATE_MINIMUM	1000
#define REL_TRUNCATE_FRACTION	16

/*
 * Timing parameters for truncate locking heuristics.
 *
 * These were not exposed as user tunable GUC values because it didn't seem
 * that the potential for improvement was great enough to merit the cost of
 * supporting them.
 */
#define VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL		20	/* ms */
#define VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL		50	/* ms */
#define VACUUM_TRUNCATE_LOCK_TIMEOUT			5000	/* ms */

/*
 * Threshold that controls whether we bypass index vacuuming and heap
 * vacuuming as an optimization
 */
#define BYPASS_THRESHOLD_PAGES	0.02	/* i.e. 2% of rel_pages */

/*
 * Perform a failsafe check each time we scan another 4GB of pages.
 * (Note that this is deliberately kept to a power-of-two, usually 2^19.)
 */
#define FAILSAFE_EVERY_PAGES \
	((BlockNumber) (((uint64) 4 * 1024 * 1024 * 1024) / BLCKSZ))

/*
 * When a table has no indexes, vacuum the FSM after every 8GB, approximately
 * (it won't be exact because we only vacuum FSM after processing a heap page
 * that has some removable tuples).  When there are indexes, this is ignored,
 * and we vacuum FSM after each index/heap cleaning pass.
 */
#define VACUUM_FSM_EVERY_PAGES \
	((BlockNumber) (((uint64) 8 * 1024 * 1024 * 1024) / BLCKSZ))

/*
 * Before we consider skipping a page that's marked as clean in
 * visibility map, we must've seen at least this many clean pages.
 */
#define SKIP_PAGES_THRESHOLD	((BlockNumber) 32)

/*
 * Size of the prefetch window for lazy vacuum backwards truncation scan.
 * Needs to be a power of 2.
 */
#define PREFETCH_SIZE			((BlockNumber) 32)

/* DSM keys for parallel heap vacuum, in a parallel context of its own */
#define PARALLEL_LV_KEY_SHARED			1
#define PARALLEL_LV_KEY_QUERY_TEXT		2
#define PARALLEL_LV_KEY_BUFFER_USAGE	3
#define PARALLEL_LV_KEY_WAL_USAGE		4
#define PARALLEL_LV_KEY_SCAN_DATA		5

/*
 * Number of blocks a process claims at once in a parallel heap scan. A chunk
 * is also an eager scan region, and the unit a process finishes before it
 * checks the dead items memory limit.
 *
 * XXX raise this to 32768 blocks, about what one VM page covers?
 */
#define PARALLEL_LV_CHUNK_SIZE	((BlockNumber) 1024)

/*
 * Macros to check if we are in a parallel vacuum. If true, we are in the
 * parallel mode and the DSM segment is initialized. The first is for the index
 * phases, the second for the heap scan; both can be true.
 */
#define ParallelVacuumIsActive(vacrel) ((vacrel)->pvs != NULL)
#define ParallelLVIsActive(vacrel) ((vacrel)->plvstate != NULL)

/* Phases of vacuum during which we report error context. */
typedef enum
{
	VACUUM_ERRCB_PHASE_UNKNOWN,
	VACUUM_ERRCB_PHASE_SCAN_HEAP,
	VACUUM_ERRCB_PHASE_VACUUM_INDEX,
	VACUUM_ERRCB_PHASE_VACUUM_HEAP,
	VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
	VACUUM_ERRCB_PHASE_TRUNCATE,
} VacErrPhase;

/*
 * An eager scan of a page that is set all-frozen in the VM is considered
 * "successful". To spread out freezing overhead across multiple normal
 * vacuums, we limit the number of successful eager page freezes. The maximum
 * number of eager page freezes is calculated as a ratio of the all-visible
 * but not all-frozen pages at the beginning of the vacuum.
 */
#define MAX_EAGER_FREEZE_SUCCESS_RATE 0.2

/*
 * On the assumption that different regions of the table tend to have
 * similarly aged data, once vacuum fails to freeze
 * vacuum_max_eager_freeze_failure_rate of the blocks in a region of size
 * EAGER_SCAN_REGION_SIZE, it suspends eager scanning until it has progressed
 * to another region of the table with potentially older data.
 */
#define EAGER_SCAN_REGION_SIZE 4096

/* Struct for data and counters updated during the heap scan. */
typedef struct LVScanData
{
	BlockNumber scanned_pages;	/* # pages examined (not skipped via VM) */

	/*
	 * Count of all-visible blocks eagerly scanned (for logging only). This
	 * does not include skippable blocks scanned due to SKIP_PAGES_THRESHOLD.
	 */
	BlockNumber eager_scanned_pages;

	BlockNumber removed_pages;	/* # pages removed by relation truncation */
	BlockNumber new_frozen_tuple_pages; /* # pages with newly frozen tuples */

	/* # pages newly set all-visible in the VM */
	BlockNumber new_all_visible_pages;

	/*
	 * # pages newly set all-visible and all-frozen in the VM. This is a
	 * subset of new_all_visible_pages. That is, new_all_visible_pages
	 * includes all pages set all-visible, but
	 * new_all_visible_all_frozen_pages includes only those which were also
	 * set all-frozen.
	 */
	BlockNumber new_all_visible_all_frozen_pages;

	/* # all-visible pages newly set all-frozen in the VM */
	BlockNumber new_all_frozen_pages;

	BlockNumber lpdead_item_pages;	/* # pages with LP_DEAD items */
	BlockNumber missed_dead_pages;	/* # pages with missed dead tuples */
	BlockNumber nonempty_pages; /* actually, last nonempty page + 1 */

	/* Counters that follow are only for scanned_pages */
	int64		tuples_deleted; /* # deleted from table */
	int64		tuples_frozen;	/* # newly frozen */
	int64		lpdead_items;	/* # deleted from indexes */
	int64		live_tuples;	/* # live tuples remaining */
	int64		recently_dead_tuples;	/* # dead, but not yet removable */
	int64		missed_dead_tuples; /* # removable, but not removed */

	/* Tracks oldest extant XID/MXID for setting relfrozenxid/relminmxid. */
	TransactionId NewRelfrozenXid;
	MultiXactId NewRelminMxid;
	bool		skippedallvis;
} LVScanData;

/* Shared information for parallel heap vacuum, in DSM */
typedef struct ParallelLVShared
{
	PVShared	base;

	/* Set once by the leader */
	int			nindexes;
	bool		aggressive;
	bool		skipwithvm;
	BlockNumber rel_pages;
	struct VacuumCutoffs cutoffs;
	TransactionId NewRelfrozenXid;	/* initial values for each worker */
	MultiXactId NewRelminMxid;
	BlockNumber eager_scan_success_limit;	/* for logging */
	BlockNumber eager_scan_max_fails_per_chunk;

	/* Set by the leader before each launch */
	bool		do_index_vacuuming;

	/* Has the leader triggered the wraparound failsafe? */
	pg_atomic_uint32 failsafe_active;

	/* Eager scan successes left */
	pg_atomic_uint32 eager_scan_remaining_successes;

	/* Have the dead items outgrown their memory limit? */
	pg_atomic_uint32 dead_items_full;

	/* Blocks handed out so far; chunks are claimed here */
	pg_atomic_uint64 nallocated;
} ParallelLVShared;

/* Per-process state for parallel heap vacuum, in the leader and workers */
typedef struct ParallelLVState
{
	ParallelLVShared *shared;
	int64		unreported_dead_items;	/* workers: not yet sent to the leader */

	/* Fields below are set and used only by the leader */
	ParallelContext *pcxt;
	BufferUsage *buffer_usage;	/* per worker, in DSM */
	WalUsage   *wal_usage;
	LVScanData *scan_data_array;	/* per worker, in DSM */
	int			nworkers_launched;	/* by the latest launch */
	int			nlaunches;
	BlockNumber next_failsafe_check;	/* in blocks handed out */
} ParallelLVState;

typedef struct LVRelState
{
	/* Target heap relation and its indexes */
	Relation	rel;
	Relation   *indrels;
	int			nindexes;

	/* Buffer access strategy and parallel vacuum state */
	BufferAccessStrategy bstrategy;
	ParallelVacuumState *pvs;	/* index phases, from vacuumparallel.c */
	ParallelLVState *plvstate;	/* heap scan */

	/* Aggressive VACUUM? (must set relfrozenxid >= FreezeLimit) */
	bool		aggressive;
	/* Use visibility map to skip? (disabled by DISABLE_PAGE_SKIPPING) */
	bool		skipwithvm;
	/* Consider index vacuuming bypass optimization? */
	bool		consider_bypass_optimization;

	/* Doing index vacuuming, index cleanup, rel truncation? */
	bool		do_index_vacuuming;
	bool		do_index_cleanup;
	bool		do_rel_truncate;

	/* VACUUM operation's cutoffs for freezing and pruning */
	struct VacuumCutoffs cutoffs;
	GlobalVisState *vistest;

	/* Error reporting state */
	char	   *dbname;
	char	   *relnamespace;
	char	   *relname;
	char	   *indname;		/* Current index name */
	BlockNumber blkno;			/* used only for heap operations */
	OffsetNumber offnum;		/* used only for heap operations */
	VacErrPhase phase;
	bool		verbose;		/* VACUUM VERBOSE? */

	/*
	 * dead_items stores TIDs whose index tuples are deleted by index
	 * vacuuming. Each TID points to an LP_DEAD line pointer from a heap page
	 * that has been processed by lazy_scan_prune.  Also needed by
	 * lazy_vacuum_heap_rel, which marks the same LP_DEAD line pointers as
	 * LP_UNUSED during second heap pass.
	 *
	 * Both dead_items and dead_items_info are allocated in shared memory in
	 * parallel vacuum cases.
	 */
	TidStore   *dead_items;		/* TIDs whose index tuples we'll delete */
	VacDeadItemsInfo *dead_items_info;
	bool		dead_items_full;	/* outgrown dead_items_info->max_bytes? */

	BlockNumber rel_pages;		/* total number of pages */

	/* Data and counters updated during the heap scan */
	LVScanData *scan_data;

	/* Statistics output by us, for table */
	double		new_rel_tuples; /* new estimated total # of tuples */
	double		new_live_tuples;	/* new estimated total # of live tuples */
	/* Statistics output by index AMs */
	IndexBulkDeleteResult **indstats;

	/* Instrumentation counters */
	int			num_index_scans;
	int			num_dead_items_resets;
	Size		total_dead_items_bytes;

	/*
	 * Total number of planned and actually launched parallel workers for heap
	 * scanning, index vacuuming and index cleanup.
	 */
	PVWorkerUsage worker_usage;

	/* Next block to vacuum the FSM from, in phase I */
	BlockNumber next_fsm_block_to_vacuum;

	/* State maintained by heap_vac_scan_next_block() */
	BlockNumber current_block;	/* last block returned */
	BlockNumber scan_end;		/* end of the current chunk (parallel only) */
	BlockNumber next_unskippable_block; /* next unskippable block */
	bool		next_unskippable_eager_scanned; /* if it was eagerly scanned */
	Buffer		next_unskippable_vmbuffer;	/* buffer containing its VM bit */

	/* State related to managing eager scanning of all-visible pages */

	/*
	 * A normal vacuum that has failed to freeze too many eagerly scanned
	 * blocks in a region suspends eager scanning.
	 * next_eager_scan_region_start is the block number of the first block
	 * eligible for resumed eager scanning.
	 *
	 * When eager scanning is permanently disabled, either initially
	 * (including for aggressive vacuum) or due to hitting the success cap,
	 * this is set to InvalidBlockNumber.
	 */
	BlockNumber next_eager_scan_region_start;

	/*
	 * The remaining number of blocks a normal vacuum will consider eager
	 * scanning when it is successful. When eager scanning is enabled, this is
	 * initialized to MAX_EAGER_FREEZE_SUCCESS_RATE of the total number of
	 * all-visible but not all-frozen pages. For each eager freeze success,
	 * this is decremented. Once it hits 0, eager scanning is permanently
	 * disabled. It is initialized to 0 if eager scanning starts out disabled
	 * (including for aggressive vacuum).
	 *
	 * The processes of a parallel heap scan share the count in
	 * ParallelLVShared instead.
	 */
	BlockNumber eager_scan_remaining_successes;
	BlockNumber eager_scan_success_limit;	/* its initial value, for logging */

	/*
	 * The maximum number of blocks which may be eagerly scanned and not
	 * frozen before eager scanning is temporarily suspended. This is
	 * configurable both globally, via the
	 * vacuum_max_eager_freeze_failure_rate GUC, and per table, with a table
	 * storage parameter of the same name. It is calculated as
	 * vacuum_max_eager_freeze_failure_rate of EAGER_SCAN_REGION_SIZE blocks.
	 * It is 0 when eager scanning is disabled.
	 *
	 * In a parallel heap scan, each chunk is a region, and this is the same
	 * fraction of PARALLEL_LV_CHUNK_SIZE blocks.
	 */
	BlockNumber eager_scan_max_fails_per_region;

	/*
	 * The number of eagerly scanned blocks vacuum failed to freeze (due to
	 * age) in the current eager scan region. Vacuum resets it to
	 * eager_scan_max_fails_per_region each time it enters a new region of the
	 * relation. If eager_scan_remaining_fails hits 0, eager scanning is
	 * suspended until the next region. It is also 0 if eager scanning has
	 * been permanently disabled.
	 */
	BlockNumber eager_scan_remaining_fails;
} LVRelState;


/* Struct for saving and restoring vacuum error information. */
typedef struct LVSavedErrInfo
{
	BlockNumber blkno;
	OffsetNumber offnum;
	VacErrPhase phase;
} LVSavedErrInfo;


/* non-export function prototypes */
static void lazy_scan_heap(LVRelState *vacrel);
static void do_lazy_scan_heap(LVRelState *vacrel);
static void heap_vacuum_eager_scan_setup(LVRelState *vacrel,
										 const VacuumParams *params);
static bool lazy_scan_eager_freeze_success(LVRelState *vacrel);
static BlockNumber heap_vac_scan_next_block(ReadStream *stream,
											void *callback_private_data,
											void *per_buffer_data);
static bool find_next_unskippable_block(LVRelState *vacrel, bool *skipsallvis,
										BlockNumber start, BlockNumber end);
static int	parallel_lazy_scan_compute_workers(LVRelState *vacrel,
											   int nrequested);
static void parallel_lazy_scan_init(LVRelState *vacrel, int nworkers,
									int vac_work_mem);
static void parallel_lazy_scan_shutdown(LVRelState *vacrel);
static void parallel_lazy_scan_heap_leader(LVRelState *vacrel);
static void parallel_lazy_scan_heap_begin(LVRelState *vacrel);
static void parallel_lazy_scan_heap_end(LVRelState *vacrel);
static void parallel_lazy_scan_chunks(LVRelState *vacrel);
static bool parallel_lazy_scan_next_chunk(LVRelState *vacrel,
										  BlockNumber *next_block);
static void parallel_lazy_scan_eager_region(LVRelState *vacrel,
											BlockNumber start);
static void parallel_lazy_scan_report_dead_items(LVRelState *vacrel);
static void parallel_lazy_scan_heap_worker(LVRelState *vacrel, shm_toc *toc);
static bool lazy_scan_new_or_empty(LVRelState *vacrel, Buffer buf,
								   BlockNumber blkno, Page page,
								   bool sharelock, Buffer vmbuffer);
static int	lazy_scan_prune(LVRelState *vacrel, Buffer buf,
							BlockNumber blkno, Page page,
							Buffer vmbuffer,
							bool *has_lpdead_items, bool *vm_page_frozen);
static bool lazy_scan_noprune(LVRelState *vacrel, Buffer buf,
							  BlockNumber blkno, Page page,
							  bool *has_lpdead_items);
static void lazy_vacuum(LVRelState *vacrel);
static bool lazy_vacuum_all_indexes(LVRelState *vacrel);
static void lazy_vacuum_heap_rel(LVRelState *vacrel);
static void lazy_vacuum_heap_page(LVRelState *vacrel, BlockNumber blkno,
								  Buffer buffer, OffsetNumber *deadoffsets,
								  int num_offsets, Buffer vmbuffer);
static bool lazy_check_wraparound_failsafe(LVRelState *vacrel);
static void lazy_cleanup_all_indexes(LVRelState *vacrel);
static IndexBulkDeleteResult *lazy_vacuum_one_index(Relation indrel,
													IndexBulkDeleteResult *istat,
													double reltuples,
													LVRelState *vacrel);
static IndexBulkDeleteResult *lazy_cleanup_one_index(Relation indrel,
													 IndexBulkDeleteResult *istat,
													 double reltuples,
													 bool estimated_count,
													 LVRelState *vacrel);
static bool should_attempt_truncation(LVRelState *vacrel);
static void lazy_truncate_heap(LVRelState *vacrel);
static BlockNumber count_nondeletable_pages(LVRelState *vacrel,
											bool *lock_waiter_detected);
static void dead_items_alloc(LVRelState *vacrel, int nworkers);
static void dead_items_add(LVRelState *vacrel, BlockNumber blkno, OffsetNumber *offsets,
						   int num_offsets);
static bool dead_items_check_memory_limit(LVRelState *vacrel);
static void dead_items_reset(LVRelState *vacrel);
static void dead_items_cleanup(LVRelState *vacrel);

static bool heap_page_would_be_all_visible(Relation rel, Buffer buf,
										   GlobalVisState *vistest,
										   bool allow_update_vistest,
										   OffsetNumber *deadoffsets,
										   int ndeadoffsets,
										   bool *all_frozen,
										   TransactionId *newest_live_xid,
										   OffsetNumber *logging_offnum);
static void update_relstats_all_indexes(LVRelState *vacrel);
static void vacuum_error_callback(void *arg);
static void update_vacuum_error_info(LVRelState *vacrel,
									 LVSavedErrInfo *saved_vacrel,
									 int phase, BlockNumber blkno,
									 OffsetNumber offnum);
static void restore_vacuum_error_info(LVRelState *vacrel,
									  const LVSavedErrInfo *saved_vacrel);



/*
 * Helper to set up the eager scanning state for vacuuming a single relation.
 * Initializes the eager scan management related members of the LVRelState.
 *
 * Caller provides whether or not an aggressive vacuum is required due to
 * vacuum options or for relfrozenxid/relminmxid advancement.
 */
static void
heap_vacuum_eager_scan_setup(LVRelState *vacrel, const VacuumParams *params)
{
	uint32		randseed;
	BlockNumber allvisible;
	BlockNumber allfrozen;
	float		first_region_ratio;
	bool		oldest_unfrozen_before_cutoff = false;

	/*
	 * Initialize eager scan management fields to their disabled values.
	 * Aggressive vacuums, normal vacuums of small tables, and normal vacuums
	 * of tables without sufficiently old tuples disable eager scanning.
	 */
	vacrel->next_eager_scan_region_start = InvalidBlockNumber;
	vacrel->eager_scan_max_fails_per_region = 0;
	vacrel->eager_scan_remaining_fails = 0;
	vacrel->eager_scan_remaining_successes = 0;
	vacrel->eager_scan_success_limit = 0;

	/* If eager scanning is explicitly disabled, just return. */
	if (params->max_eager_freeze_failure_rate == 0)
		return;

	/*
	 * The caller will have determined whether or not an aggressive vacuum is
	 * required by either the vacuum parameters or the relative age of the
	 * oldest unfrozen transaction IDs. An aggressive vacuum must scan every
	 * all-visible page to safely advance the relfrozenxid and/or relminmxid,
	 * so scans of all-visible pages are not considered eager.
	 */
	if (vacrel->aggressive)
		return;

	/*
	 * Aggressively vacuuming a small relation shouldn't take long, so it
	 * isn't worth amortizing. We use two times the region size as the size
	 * cutoff because the eager scan start block is a random spot somewhere in
	 * the first region, making the second region the first to be eager
	 * scanned normally.
	 */
	if (vacrel->rel_pages < 2 * EAGER_SCAN_REGION_SIZE)
		return;

	/*
	 * We only want to enable eager scanning if we are likely to be able to
	 * freeze some of the pages in the relation.
	 *
	 * Tuples with XIDs older than OldestXmin or MXIDs older than OldestMxact
	 * are technically freezable, but we won't freeze them unless the criteria
	 * for opportunistic freezing is met. Only tuples with XIDs/MXIDs older
	 * than the FreezeLimit/MultiXactCutoff are frozen in the common case.
	 *
	 * So, as a heuristic, we wait until the FreezeLimit has advanced past the
	 * relfrozenxid or the MultiXactCutoff has advanced past the relminmxid to
	 * enable eager scanning.
	 */
	if (TransactionIdIsNormal(vacrel->cutoffs.relfrozenxid) &&
		TransactionIdPrecedes(vacrel->cutoffs.relfrozenxid,
							  vacrel->cutoffs.FreezeLimit))
		oldest_unfrozen_before_cutoff = true;

	if (!oldest_unfrozen_before_cutoff &&
		MultiXactIdIsValid(vacrel->cutoffs.relminmxid) &&
		MultiXactIdPrecedes(vacrel->cutoffs.relminmxid,
							vacrel->cutoffs.MultiXactCutoff))
		oldest_unfrozen_before_cutoff = true;

	if (!oldest_unfrozen_before_cutoff)
		return;

	/* We have met the criteria to eagerly scan some pages. */

	/*
	 * Our success cap is MAX_EAGER_FREEZE_SUCCESS_RATE of the number of
	 * all-visible but not all-frozen blocks in the relation.
	 */
	visibilitymap_count(vacrel->rel, &allvisible, &allfrozen);

	vacrel->eager_scan_remaining_successes =
		(BlockNumber) (MAX_EAGER_FREEZE_SUCCESS_RATE *
					   (allvisible - allfrozen));
	vacrel->eager_scan_success_limit = vacrel->eager_scan_remaining_successes;

	/* If every all-visible page is frozen, eager scanning is disabled. */
	if (vacrel->eager_scan_remaining_successes == 0)
		return;

	/*
	 * Now calculate the bounds of the first eager scan region. Its end block
	 * will be a random spot somewhere in the first EAGER_SCAN_REGION_SIZE
	 * blocks. This affects the bounds of all subsequent regions and avoids
	 * eager scanning and failing to freeze the same blocks each vacuum of the
	 * relation.
	 */
	randseed = pg_prng_uint32(&pg_global_prng_state);

	vacrel->next_eager_scan_region_start = randseed % EAGER_SCAN_REGION_SIZE;

	Assert(params->max_eager_freeze_failure_rate > 0 &&
		   params->max_eager_freeze_failure_rate <= 1);

	vacrel->eager_scan_max_fails_per_region =
		params->max_eager_freeze_failure_rate *
		EAGER_SCAN_REGION_SIZE;

	/*
	 * The first region will be smaller than subsequent regions. As such,
	 * adjust the eager freeze failures tolerated for this region.
	 */
	first_region_ratio = 1 - (float) vacrel->next_eager_scan_region_start /
		EAGER_SCAN_REGION_SIZE;

	vacrel->eager_scan_remaining_fails =
		vacrel->eager_scan_max_fails_per_region *
		first_region_ratio;
}

/*
 *	heap_vacuum_rel() -- perform VACUUM for one heap relation
 *
 *		This routine sets things up for and then calls lazy_scan_heap, where
 *		almost all work actually takes place.  Finalizes everything after call
 *		returns by managing relation truncation and updating rel's pg_class
 *		entry. (Also updates pg_class entries for any indexes that need it.)
 *
 *		At entry, we have already established a transaction and opened
 *		and locked the relation.
 */
void
heap_vacuum_rel(Relation rel, const VacuumParams *params,
				BufferAccessStrategy bstrategy)
{
	LVRelState *vacrel;
	bool		verbose,
				instrument,
				skipwithvm,
				frozenxid_updated,
				minmulti_updated;
	BlockNumber orig_rel_pages,
				new_rel_pages,
				new_rel_allvisible,
				new_rel_allfrozen;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	WalUsage	startwalusage = pgWalUsage;
	BufferUsage startbufferusage = pgBufferUsage;
	ErrorContextCallback errcallback;
	char	  **indnames = NULL;
	Size		dead_items_max_bytes = 0;

	verbose = (params->options & VACOPT_VERBOSE) != 0;
	instrument = (verbose || (AmAutoVacuumWorkerProcess() &&
							  params->log_vacuum_min_duration >= 0));
	if (instrument)
		pg_rusage_init(&ru0);

	/* Used for instrumentation and stats report */
	starttime = GetCurrentTimestamp();

	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
								  RelationGetRelid(rel));
	if (AmAutoVacuumWorkerProcess())
		pgstat_progress_update_param(PROGRESS_VACUUM_STARTED_BY,
									 params->is_wraparound
									 ? PROGRESS_VACUUM_STARTED_BY_AUTOVACUUM_WRAPAROUND
									 : PROGRESS_VACUUM_STARTED_BY_AUTOVACUUM);
	else
		pgstat_progress_update_param(PROGRESS_VACUUM_STARTED_BY,
									 PROGRESS_VACUUM_STARTED_BY_MANUAL);

	/*
	 * Setup error traceback support for ereport() first.  The idea is to set
	 * up an error context callback to display additional information on any
	 * error during a vacuum.  During different phases of vacuum, we update
	 * the state so that the error context callback always display current
	 * information.
	 *
	 * Copy the names of heap rel into local memory for error reporting
	 * purposes, too.  It isn't always safe to assume that we can get the name
	 * of each rel.  It's convenient for code in lazy_scan_heap to always use
	 * these temp copies.
	 */
	vacrel = palloc0_object(LVRelState);
	vacrel->dbname = get_database_name(MyDatabaseId);
	vacrel->relnamespace = get_namespace_name(RelationGetNamespace(rel));
	vacrel->relname = pstrdup(RelationGetRelationName(rel));
	vacrel->indname = NULL;
	vacrel->phase = VACUUM_ERRCB_PHASE_UNKNOWN;
	vacrel->verbose = verbose;
	errcallback.callback = vacuum_error_callback;
	errcallback.arg = vacrel;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Set up high level stuff about rel and its indexes */
	vacrel->rel = rel;
	vac_open_indexes(vacrel->rel, RowExclusiveLock, &vacrel->nindexes,
					 &vacrel->indrels);
	vacrel->bstrategy = bstrategy;
	if (instrument && vacrel->nindexes > 0)
	{
		/* Copy index names used by instrumentation (not error reporting) */
		indnames = palloc_array(char *, vacrel->nindexes);
		for (int i = 0; i < vacrel->nindexes; i++)
			indnames[i] = pstrdup(RelationGetRelationName(vacrel->indrels[i]));
	}

	/*
	 * The index_cleanup param either disables index vacuuming and cleanup or
	 * forces it to go ahead when we would otherwise apply the index bypass
	 * optimization.  The default is 'auto', which leaves the final decision
	 * up to lazy_vacuum().
	 *
	 * The truncate param allows user to avoid attempting relation truncation,
	 * though it can't force truncation to happen.
	 */
	Assert(params->index_cleanup != VACOPTVALUE_UNSPECIFIED);
	Assert(params->truncate != VACOPTVALUE_UNSPECIFIED &&
		   params->truncate != VACOPTVALUE_AUTO);

	/*
	 * While VacuumFailSafeActive is reset to false before calling this, we
	 * still need to reset it here due to recursive calls.
	 */
	VacuumFailsafeActive = false;
	vacrel->consider_bypass_optimization = true;
	vacrel->do_index_vacuuming = true;
	vacrel->do_index_cleanup = true;
	vacrel->do_rel_truncate = (params->truncate != VACOPTVALUE_DISABLED);
	if (params->index_cleanup == VACOPTVALUE_DISABLED)
	{
		/* Force disable index vacuuming up-front */
		vacrel->do_index_vacuuming = false;
		vacrel->do_index_cleanup = false;
	}
	else if (params->index_cleanup == VACOPTVALUE_ENABLED)
	{
		/* Force index vacuuming.  Note that failsafe can still bypass. */
		vacrel->consider_bypass_optimization = false;
	}
	else
	{
		/* Default/auto, make all decisions dynamically */
		Assert(params->index_cleanup == VACOPTVALUE_AUTO);
	}

	/* All counters start at zero; a few fields are initialized below */
	vacrel->scan_data = palloc0_object(LVScanData);

	/* Allocate/initialize output statistics state */
	vacrel->new_rel_tuples = 0;
	vacrel->new_live_tuples = 0;
	vacrel->indstats = palloc0_array(IndexBulkDeleteResult *, vacrel->nindexes);

	/* Initialize remaining counters (be tidy) */
	vacrel->num_index_scans = 0;
	vacrel->num_dead_items_resets = 0;
	vacrel->total_dead_items_bytes = 0;
	vacrel->next_fsm_block_to_vacuum = 0;

	vacrel->worker_usage.heap_scan.nlaunched = 0;
	vacrel->worker_usage.heap_scan.nplanned = 0;
	vacrel->worker_usage.vacuum.nlaunched = 0;
	vacrel->worker_usage.vacuum.nplanned = 0;
	vacrel->worker_usage.cleanup.nlaunched = 0;
	vacrel->worker_usage.cleanup.nplanned = 0;

	/* dead_items_alloc allocates vacrel->dead_items later on */

	/*
	 * Get cutoffs that determine which deleted tuples are considered DEAD,
	 * not just RECENTLY_DEAD, and which XIDs/MXIDs to freeze.  Then determine
	 * the extent of the blocks that we'll scan in lazy_scan_heap.  It has to
	 * happen in this order to ensure that the OldestXmin cutoff field works
	 * as an upper bound on the XIDs stored in the pages we'll actually scan
	 * (NewRelfrozenXid tracking must never be allowed to miss unfrozen XIDs).
	 *
	 * Next acquire vistest, a related cutoff that's used in pruning.  We use
	 * vistest in combination with OldestXmin to ensure that
	 * heap_page_prune_and_freeze() always removes any deleted tuple whose
	 * xmax is < OldestXmin.  lazy_scan_prune must never become confused about
	 * whether a tuple should be frozen or removed.  (In the future we might
	 * want to teach lazy_scan_prune to recompute vistest from time to time,
	 * to increase the number of dead tuples it can prune away.)
	 */
	vacrel->aggressive = vacuum_get_cutoffs(rel, params, &vacrel->cutoffs);
	vacrel->rel_pages = orig_rel_pages = RelationGetNumberOfBlocks(rel);
	vacrel->vistest = GlobalVisTestFor(rel);

	/* Initialize state used to track oldest extant XID/MXID */
	vacrel->scan_data->NewRelfrozenXid = vacrel->cutoffs.OldestXmin;
	vacrel->scan_data->NewRelminMxid = vacrel->cutoffs.OldestMxact;

	/*
	 * Initialize state related to tracking all-visible page skipping. This is
	 * very important to determine whether or not it is safe to advance the
	 * relfrozenxid/relminmxid.
	 */
	vacrel->scan_data->skippedallvis = false;
	skipwithvm = true;
	if (params->options & VACOPT_DISABLE_PAGE_SKIPPING)
	{
		/*
		 * Force aggressive mode, and disable skipping blocks using the
		 * visibility map (even those set all-frozen)
		 */
		vacrel->aggressive = true;
		skipwithvm = false;
	}

	vacrel->skipwithvm = skipwithvm;

	/*
	 * Set up eager scan tracking state. This must happen after determining
	 * whether or not the vacuum must be aggressive, because only normal
	 * vacuums use the eager scan algorithm.
	 */
	heap_vacuum_eager_scan_setup(vacrel, params);

	/* Report the vacuum mode: 'normal' or 'aggressive' */
	pgstat_progress_update_param(PROGRESS_VACUUM_MODE,
								 vacrel->aggressive
								 ? PROGRESS_VACUUM_MODE_AGGRESSIVE
								 : PROGRESS_VACUUM_MODE_NORMAL);

	if (verbose)
	{
		if (vacrel->aggressive)
			ereport(INFO,
					(errmsg("aggressively vacuuming \"%s.%s.%s\"",
							vacrel->dbname, vacrel->relnamespace,
							vacrel->relname)));
		else
			ereport(INFO,
					(errmsg("vacuuming \"%s.%s.%s\"",
							vacrel->dbname, vacrel->relnamespace,
							vacrel->relname)));
	}

	/*
	 * Allocate dead_items memory using dead_items_alloc.  This handles
	 * parallel VACUUM initialization as part of allocating shared memory
	 * space used for dead_items.  (But do a failsafe precheck first, to
	 * ensure that parallel VACUUM won't be attempted at all when relfrozenxid
	 * is already dangerously old.)
	 */
	lazy_check_wraparound_failsafe(vacrel);
	dead_items_alloc(vacrel, params->nworkers);

#ifdef USE_INJECTION_POINTS

	/*
	 * Used by tests to pause before parallel vacuum is launched, allowing
	 * test code to modify configuration that the leader then propagates to
	 * workers.
	 */
	if (AmAutoVacuumWorkerProcess() &&
		(ParallelVacuumIsActive(vacrel) || ParallelLVIsActive(vacrel)))
		INJECTION_POINT("autovacuum-start-parallel-vacuum", NULL);
#endif

	/*
	 * Call lazy_scan_heap to perform all required heap pruning, index
	 * vacuuming, and heap vacuuming (plus related processing)
	 */
	lazy_scan_heap(vacrel);

	/*
	 * Save dead items max_bytes and update the memory usage statistics before
	 * cleanup, they are freed in parallel vacuum cases during
	 * dead_items_cleanup().
	 */
	dead_items_max_bytes = vacrel->dead_items_info->max_bytes;
	vacrel->total_dead_items_bytes += TidStoreMemoryUsage(vacrel->dead_items);

	/*
	 * Free resources managed by dead_items_alloc.  This ends parallel mode in
	 * passing when necessary.
	 */
	dead_items_cleanup(vacrel);
	Assert(!IsInParallelMode());

	/*
	 * Update pg_class entries for each of rel's indexes where appropriate.
	 *
	 * Unlike the later update to rel's pg_class entry, this is not critical.
	 * Maintains relpages/reltuples statistics used by the planner only.
	 */
	if (vacrel->do_index_cleanup)
		update_relstats_all_indexes(vacrel);

	/* Done with rel's indexes */
	vac_close_indexes(vacrel->nindexes, vacrel->indrels, NoLock);

	/* Optionally truncate rel */
	if (should_attempt_truncation(vacrel))
		lazy_truncate_heap(vacrel);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/* Report that we are now doing final cleanup */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);

	/*
	 * Prepare to update rel's pg_class entry.
	 *
	 * Aggressive VACUUMs must always be able to advance relfrozenxid to a
	 * value >= FreezeLimit, and relminmxid to a value >= MultiXactCutoff.
	 * Non-aggressive VACUUMs may advance them by any amount, or not at all.
	 */
	Assert(vacrel->scan_data->NewRelfrozenXid == vacrel->cutoffs.OldestXmin ||
		   TransactionIdPrecedesOrEquals(vacrel->aggressive ? vacrel->cutoffs.FreezeLimit :
										 vacrel->cutoffs.relfrozenxid,
										 vacrel->scan_data->NewRelfrozenXid));
	Assert(vacrel->scan_data->NewRelminMxid == vacrel->cutoffs.OldestMxact ||
		   MultiXactIdPrecedesOrEquals(vacrel->aggressive ? vacrel->cutoffs.MultiXactCutoff :
									   vacrel->cutoffs.relminmxid,
									   vacrel->scan_data->NewRelminMxid));
	if (vacrel->scan_data->skippedallvis)
	{
		/*
		 * Must keep original relfrozenxid in a non-aggressive VACUUM that
		 * chose to skip an all-visible page range.  The state that tracks new
		 * values will have missed unfrozen XIDs from the pages we skipped.
		 */
		Assert(!vacrel->aggressive);
		vacrel->scan_data->NewRelfrozenXid = InvalidTransactionId;
		vacrel->scan_data->NewRelminMxid = InvalidMultiXactId;
	}

	/*
	 * For safety, clamp relallvisible to be not more than what we're setting
	 * pg_class.relpages to
	 */
	new_rel_pages = vacrel->rel_pages;	/* After possible rel truncation */
	visibilitymap_count(rel, &new_rel_allvisible, &new_rel_allfrozen);
	if (new_rel_allvisible > new_rel_pages)
		new_rel_allvisible = new_rel_pages;

	/*
	 * An all-frozen block _must_ be all-visible. As such, clamp the count of
	 * all-frozen blocks to the count of all-visible blocks. This matches the
	 * clamping of relallvisible above.
	 */
	if (new_rel_allfrozen > new_rel_allvisible)
		new_rel_allfrozen = new_rel_allvisible;

	/*
	 * Now actually update rel's pg_class entry.
	 *
	 * In principle new_live_tuples could be -1 indicating that we (still)
	 * don't know the tuple count.  In practice that can't happen, since we
	 * scan every page that isn't skipped using the visibility map.
	 */
	vac_update_relstats(rel, new_rel_pages, vacrel->new_live_tuples,
						new_rel_allvisible, new_rel_allfrozen,
						vacrel->nindexes > 0,
						vacrel->scan_data->NewRelfrozenXid,
						vacrel->scan_data->NewRelminMxid,
						&frozenxid_updated, &minmulti_updated, false);

	/*
	 * Report results to the cumulative stats system, too.
	 *
	 * Deliberately avoid telling the stats system about LP_DEAD items that
	 * remain in the table due to VACUUM bypassing index and heap vacuuming.
	 * ANALYZE will consider the remaining LP_DEAD items to be dead "tuples".
	 * It seems like a good idea to err on the side of not vacuuming again too
	 * soon in cases where the failsafe prevented significant amounts of heap
	 * vacuuming.
	 */
	pgstat_report_vacuum(rel,
						 Max(vacrel->new_live_tuples, 0),
						 vacrel->scan_data->recently_dead_tuples +
						 vacrel->scan_data->missed_dead_tuples,
						 starttime);
	pgstat_progress_end_command();

	if (instrument)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (verbose || params->log_vacuum_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params->log_vacuum_min_duration))
		{
			long		secs_dur;
			int			usecs_dur;
			WalUsage	walusage;
			BufferUsage bufferusage;
			StringInfoData buf;
			char	   *msgfmt;
			int32		diff;
			double		read_rate = 0,
						write_rate = 0;
			int64		total_blks_hit;
			int64		total_blks_read;
			int64		total_blks_dirtied;

			TimestampDifference(starttime, endtime, &secs_dur, &usecs_dur);
			memset(&walusage, 0, sizeof(WalUsage));
			WalUsageAccumDiff(&walusage, &pgWalUsage, &startwalusage);
			memset(&bufferusage, 0, sizeof(BufferUsage));
			BufferUsageAccumDiff(&bufferusage, &pgBufferUsage, &startbufferusage);

			total_blks_hit = bufferusage.shared_blks_hit +
				bufferusage.local_blks_hit;
			total_blks_read = bufferusage.shared_blks_read +
				bufferusage.local_blks_read;
			total_blks_dirtied = bufferusage.shared_blks_dirtied +
				bufferusage.local_blks_dirtied;

			initStringInfo(&buf);
			if (verbose)
			{
				/*
				 * Aggressiveness already reported earlier, in dedicated
				 * VACUUM VERBOSE ereport
				 */
				Assert(!params->is_wraparound);
				msgfmt = _("finished vacuuming \"%s.%s.%s\": index scans: %d\n");
			}
			else if (params->is_wraparound)
			{
				/*
				 * While it's possible for a VACUUM to be both is_wraparound
				 * and !aggressive, that's just a corner-case -- is_wraparound
				 * implies aggressive.  Produce distinct output for the corner
				 * case all the same, just in case.
				 */
				if (vacrel->aggressive)
					msgfmt = _("automatic aggressive vacuum to prevent wraparound of table \"%s.%s.%s\": index scans: %d\n");
				else
					msgfmt = _("automatic vacuum to prevent wraparound of table \"%s.%s.%s\": index scans: %d\n");
			}
			else
			{
				if (vacrel->aggressive)
					msgfmt = _("automatic aggressive vacuum of table \"%s.%s.%s\": index scans: %d\n");
				else
					msgfmt = _("automatic vacuum of table \"%s.%s.%s\": index scans: %d\n");
			}
			appendStringInfo(&buf, msgfmt,
							 vacrel->dbname,
							 vacrel->relnamespace,
							 vacrel->relname,
							 vacrel->num_index_scans);
			appendStringInfo(&buf, _("pages: %u removed, %u remain, %u scanned (%.2f%% of total), %u eagerly scanned\n"),
							 vacrel->scan_data->removed_pages,
							 new_rel_pages,
							 vacrel->scan_data->scanned_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->scan_data->scanned_pages /
							 orig_rel_pages,
							 vacrel->scan_data->eager_scanned_pages);
			appendStringInfo(&buf,
							 _("tuples: %" PRId64 " removed, %" PRId64 " remain, %" PRId64 " are dead but not yet removable\n"),
							 vacrel->scan_data->tuples_deleted,
							 (int64) vacrel->new_rel_tuples,
							 vacrel->scan_data->recently_dead_tuples);
			if (vacrel->scan_data->missed_dead_tuples > 0)
				appendStringInfo(&buf,
								 _("tuples missed: %" PRId64 " dead from %u pages not removed due to cleanup lock contention\n"),
								 vacrel->scan_data->missed_dead_tuples,
								 vacrel->scan_data->missed_dead_pages);
			diff = (int32) (ReadNextTransactionId() -
							vacrel->cutoffs.OldestXmin);
			appendStringInfo(&buf,
							 _("removable cutoff: %u, which was %d XIDs old when operation ended\n"),
							 vacrel->cutoffs.OldestXmin, diff);
			if (frozenxid_updated)
			{
				diff = (int32) (vacrel->scan_data->NewRelfrozenXid -
								vacrel->cutoffs.relfrozenxid);
				appendStringInfo(&buf,
								 _("new relfrozenxid: %u, which is %d XIDs ahead of previous value\n"),
								 vacrel->scan_data->NewRelfrozenXid, diff);
			}
			if (minmulti_updated)
			{
				diff = (int32) (vacrel->scan_data->NewRelminMxid -
								vacrel->cutoffs.relminmxid);
				appendStringInfo(&buf,
								 _("new relminmxid: %u, which is %d MXIDs ahead of previous value\n"),
								 vacrel->scan_data->NewRelminMxid, diff);
			}
			appendStringInfo(&buf, _("frozen: %u pages from table (%.2f%% of total) had %" PRId64 " tuples frozen\n"),
							 vacrel->scan_data->new_frozen_tuple_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->scan_data->new_frozen_tuple_pages /
							 orig_rel_pages,
							 vacrel->scan_data->tuples_frozen);

			appendStringInfo(&buf,
							 _("visibility map: %u pages set all-visible, %u pages set all-frozen (%u were all-visible)\n"),
							 vacrel->scan_data->new_all_visible_pages,
							 vacrel->scan_data->new_all_visible_all_frozen_pages +
							 vacrel->scan_data->new_all_frozen_pages,
							 vacrel->scan_data->new_all_frozen_pages);
			if (vacrel->do_index_vacuuming)
			{
				if (vacrel->nindexes == 0 || vacrel->num_index_scans == 0)
					appendStringInfoString(&buf, _("index scan not needed: "));
				else
					appendStringInfoString(&buf, _("index scan needed: "));

				msgfmt = _("%u pages from table (%.2f%% of total) had %" PRId64 " dead item identifiers removed\n");
			}
			else
			{
				if (!VacuumFailsafeActive)
					appendStringInfoString(&buf, _("index scan bypassed: "));
				else
					appendStringInfoString(&buf, _("index scan bypassed by failsafe: "));

				msgfmt = _("%u pages from table (%.2f%% of total) have %" PRId64 " dead item identifiers\n");
			}
			appendStringInfo(&buf, msgfmt,
							 vacrel->scan_data->lpdead_item_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->scan_data->lpdead_item_pages / orig_rel_pages,
							 vacrel->scan_data->lpdead_items);

			if (vacrel->worker_usage.heap_scan.nplanned > 0)
				appendStringInfo(&buf,
								 _("parallel workers: heap scan: %d planned, %d launched in total\n"),
								 vacrel->worker_usage.heap_scan.nplanned,
								 vacrel->worker_usage.heap_scan.nlaunched);

			if (vacrel->worker_usage.vacuum.nplanned > 0)
				appendStringInfo(&buf,
								 _("parallel workers: index vacuum: %d planned, %d launched in total\n"),
								 vacrel->worker_usage.vacuum.nplanned,
								 vacrel->worker_usage.vacuum.nlaunched);

			if (vacrel->worker_usage.cleanup.nplanned > 0)
				appendStringInfo(&buf,
								 _("parallel workers: index cleanup: %d planned, %d launched\n"),
								 vacrel->worker_usage.cleanup.nplanned,
								 vacrel->worker_usage.cleanup.nlaunched);

			for (int i = 0; i < vacrel->nindexes; i++)
			{
				IndexBulkDeleteResult *istat = vacrel->indstats[i];

				if (!istat)
					continue;

				appendStringInfo(&buf,
								 _("index \"%s\": pages: %u in total, %u newly deleted, %u currently deleted, %u reusable\n"),
								 indnames[i],
								 istat->num_pages,
								 istat->pages_newly_deleted,
								 istat->pages_deleted,
								 istat->pages_free);
			}
			if (track_cost_delay_timing)
			{
				/*
				 * We bypass the changecount mechanism because this value is
				 * only updated by the calling process.  We also rely on the
				 * above call to pgstat_progress_end_command() to not clear
				 * the st_progress_param array.
				 */
				appendStringInfo(&buf, _("delay time: %.3f ms\n"),
								 (double) MyBEEntry->st_progress_param[PROGRESS_VACUUM_DELAY_TIME] / 1000000.0);
			}
			if (track_io_timing)
			{
				/*
				 * Take the timings from the same buffer usage delta as the
				 * block counts, so that the parallel workers are included in
				 * both.
				 */
				double		read_ms =
					INSTR_TIME_GET_MILLISEC(bufferusage.shared_blk_read_time) +
					INSTR_TIME_GET_MILLISEC(bufferusage.local_blk_read_time);
				double		write_ms =
					INSTR_TIME_GET_MILLISEC(bufferusage.shared_blk_write_time) +
					INSTR_TIME_GET_MILLISEC(bufferusage.local_blk_write_time);

				appendStringInfo(&buf, _("I/O timings: read: %.3f ms, write: %.3f ms\n"),
								 read_ms, write_ms);
			}
			if (secs_dur > 0 || usecs_dur > 0)
			{
				read_rate = (double) BLCKSZ * total_blks_read /
					(1024 * 1024) / (secs_dur + usecs_dur / 1000000.0);
				write_rate = (double) BLCKSZ * total_blks_dirtied /
					(1024 * 1024) / (secs_dur + usecs_dur / 1000000.0);
			}
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			appendStringInfo(&buf,
							 _("buffer usage: %" PRId64 " hits, %" PRId64 " reads, %" PRId64 " dirtied\n"),
							 total_blks_hit,
							 total_blks_read,
							 total_blks_dirtied);
			appendStringInfo(&buf,
							 _("WAL usage: %" PRId64 " records, %" PRId64 " full page images, %" PRIu64 " bytes, %" PRIu64 " full page image bytes, %" PRId64 " buffers full\n"),
							 walusage.wal_records,
							 walusage.wal_fpi,
							 walusage.wal_bytes,
							 walusage.wal_fpi_bytes,
							 walusage.wal_buffers_full);

			/*
			 * Report the dead items memory usage.
			 *
			 * The num_dead_items_resets counter increases when we reset the
			 * collected dead items, so the counter is non-zero if at least
			 * one dead items are collected, even if index vacuuming is
			 * disabled.
			 */
			appendStringInfo(&buf,
							 ngettext("memory usage: dead item storage %.2f MB accumulated across %d reset (limit %.2f MB each)\n",
									  "memory usage: dead item storage %.2f MB accumulated across %d resets (limit %.2f MB each)\n",
									  vacrel->num_dead_items_resets),
							 (double) vacrel->total_dead_items_bytes / (1024 * 1024),
							 vacrel->num_dead_items_resets,
							 (double) dead_items_max_bytes / (1024 * 1024));
			appendStringInfo(&buf, _("system usage: %s"), pg_rusage_show(&ru0));

			ereport(verbose ? INFO : LOG,
					(errmsg_internal("%s", buf.data)));
			pfree(buf.data);
		}
	}

	/* Cleanup index statistics and index names */
	for (int i = 0; i < vacrel->nindexes; i++)
	{
		if (vacrel->indstats[i])
			pfree(vacrel->indstats[i]);

		if (instrument)
			pfree(indnames[i]);
	}
}

/*
 *	lazy_scan_heap() -- workhorse function for VACUUM
 *
 *		This routine prunes each page in the heap, and considers the need to
 *		freeze remaining tuples with storage (not including pages that can be
 *		skipped using the visibility map).  Also performs related maintenance
 *		of the FSM and visibility map.  These steps all take place during an
 *		initial pass over the target heap relation.
 *
 *		Also invokes lazy_vacuum_all_indexes to vacuum indexes, which largely
 *		consists of deleting index tuples that point to LP_DEAD items left in
 *		heap pages following pruning.  Earlier initial pass over the heap will
 *		have collected the TIDs whose index tuples need to be removed.
 *
 *		Finally, invokes lazy_vacuum_heap_rel to vacuum heap pages, which
 *		largely consists of marking LP_DEAD items (from vacrel->dead_items)
 *		as LP_UNUSED.  This has to happen in a second, final pass over the
 *		heap, to preserve a basic invariant that all index AMs rely on: no
 *		extant index tuple can ever be allowed to contain a TID that points to
 *		an LP_UNUSED line pointer in the heap.  We must disallow premature
 *		recycling of line pointers to avoid index scans that get confused
 *		about which TID points to which tuple immediately after recycling.
 *		(Actually, this isn't a concern when target heap relation happens to
 *		have no indexes, which allows us to safely apply the one-pass strategy
 *		as an optimization).
 *
 *		In practice we often have enough space to fit all TIDs, and so won't
 *		need to call lazy_vacuum more than once, after our initial pass over
 *		the heap has totally finished.  Otherwise things are slightly more
 *		complicated: our "initial pass" over the heap applies only to those
 *		pages that were pruned before we needed to call lazy_vacuum, and our
 *		"final pass" over the heap only vacuums these same heap pages.
 *		However, we process indexes in full every time lazy_vacuum is called,
 *		which makes index processing very inefficient when memory is in short
 *		supply.
 */
static void
lazy_scan_heap(LVRelState *vacrel)
{
	BlockNumber rel_pages = vacrel->rel_pages;
	const int	initprog_index[] = {
		PROGRESS_VACUUM_PHASE,
		PROGRESS_VACUUM_TOTAL_HEAP_BLKS,
		PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES
	};
	int64		initprog_val[3];

	/* Report that we're scanning the heap, advertising total # of blocks */
	initprog_val[0] = PROGRESS_VACUUM_PHASE_SCAN_HEAP;
	initprog_val[1] = rel_pages;
	initprog_val[2] = vacrel->dead_items_info->max_bytes;
	pgstat_progress_update_multi_param(3, initprog_index, initprog_val);

	/* Initialize for the first heap_vac_scan_next_block() call */
	vacrel->current_block = InvalidBlockNumber;
	vacrel->next_unskippable_block = InvalidBlockNumber;
	vacrel->next_unskippable_eager_scanned = false;
	vacrel->next_unskippable_vmbuffer = InvalidBuffer;

	if (ParallelLVIsActive(vacrel))
		parallel_lazy_scan_heap_leader(vacrel);
	else
		do_lazy_scan_heap(vacrel);

	/*
	 * Report that everything is now scanned. We never skip scanning the last
	 * block in the relation, so we can pass rel_pages here.
	 */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED,
								 rel_pages);

	/* now we can compute the new value for pg_class.reltuples */
	vacrel->new_live_tuples = vac_estimate_reltuples(vacrel->rel, rel_pages,
													 vacrel->scan_data->scanned_pages,
													 vacrel->scan_data->live_tuples);

	/*
	 * Also compute the total number of surviving heap entries.  In the
	 * (unlikely) scenario that new_live_tuples is -1, take it as zero.
	 */
	vacrel->new_rel_tuples =
		Max(vacrel->new_live_tuples, 0) + vacrel->scan_data->recently_dead_tuples +
		vacrel->scan_data->missed_dead_tuples;

	/*
	 * Do index vacuuming (call each index's ambulkdelete routine), then do
	 * related heap vacuuming
	 */
	if (vacrel->dead_items_info->num_items > 0)
		lazy_vacuum(vacrel);

	/*
	 * Vacuum the remainder of the Free Space Map.  We must do this whether or
	 * not there were indexes, and whether or not we bypassed index vacuuming.
	 * We can pass rel_pages here because we never skip scanning the last
	 * block of the relation.
	 */
	if (rel_pages > vacrel->next_fsm_block_to_vacuum)
		FreeSpaceMapVacuumRange(vacrel->rel, vacrel->next_fsm_block_to_vacuum,
								rel_pages);

	/* report all blocks vacuumed */
	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, rel_pages);

	/* Do final index cleanup (call each index's amvacuumcleanup routine) */
	if (vacrel->nindexes > 0 && vacrel->do_index_cleanup)
		lazy_cleanup_all_indexes(vacrel);
}

/*
 * Workhorse for lazy_scan_heap(). Scans the heap with a read stream until
 * heap_vac_scan_next_block() has no more blocks to give: the whole heap in a
 * serial vacuum, pausing for a round of index and heap vacuuming when the dead
 * items exceed the memory limit; in a parallel heap scan, the chunks this
 * process claims, until the dead items exceed the memory limit.
 */
static void
do_lazy_scan_heap(LVRelState *vacrel)
{
	ReadStream *stream;
	BlockNumber blkno = InvalidBlockNumber;
	Buffer		vmbuffer = InvalidBuffer;
	bool		strategy_cleared = false;

	/*
	 * Set up the read stream for vacuum's first pass through the heap.
	 *
	 * This could be made safe for READ_STREAM_USE_BATCHING, but only with
	 * explicit work in heap_vac_scan_next_block.
	 */
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE,
										vacrel->bstrategy,
										vacrel->rel,
										MAIN_FORKNUM,
										heap_vac_scan_next_block,
										vacrel,
										sizeof(bool));

	while (true)
	{
		Buffer		buf;
		Page		page;
		bool		was_eager_scanned = false;
		int			ndeleted = 0;
		bool		has_lpdead_items;
		void	   *per_buffer_data = NULL;
		bool		vm_page_frozen = false;
		bool		got_cleanup_lock = false;

		vacuum_delay_point(false);

		/*
		 * Regularly check if wraparound failsafe should trigger.
		 *
		 * There is a similar check inside lazy_vacuum_all_indexes(), but
		 * relfrozenxid might start to look dangerously old before we reach
		 * that point.  This check also provides failsafe coverage for the
		 * one-pass strategy, and the two-pass strategy with the index_cleanup
		 * param set to 'off'.
		 *
		 * In a parallel heap scan, only the leader checks, by the blocks
		 * handed out to all the processes so far.
		 */
		if (ParallelLVIsActive(vacrel))
		{
			if (!IsParallelWorker() &&
				pg_atomic_read_u64(&vacrel->plvstate->shared->nallocated) >=
				vacrel->plvstate->next_failsafe_check)
			{
				vacrel->plvstate->next_failsafe_check += FAILSAFE_EVERY_PAGES;
				lazy_check_wraparound_failsafe(vacrel);
			}
		}
		else if (vacrel->scan_data->scanned_pages > 0 &&
				 vacrel->scan_data->scanned_pages % FAILSAFE_EVERY_PAGES == 0)
			lazy_check_wraparound_failsafe(vacrel);

		/*
		 * Consider if we definitely have enough space to process TIDs on page
		 * already.  If we are close to overrunning the available space for
		 * dead_items TIDs, pause and do a cycle of vacuuming before we tackle
		 * this page. (In a parallel heap scan, heap_vac_scan_next_block()
		 * ends the stream at the end of the chunk instead, and the leader
		 * vacuums once the workers have finished.)
		 */
		if (!ParallelLVIsActive(vacrel) &&
			dead_items_check_memory_limit(vacrel))
		{
			/*
			 * Before beginning index vacuuming, we release any pin we may
			 * hold on the visibility map page.  This isn't necessary for
			 * correctness, but we do it anyway to avoid holding the pin
			 * across a lengthy, unrelated operation.
			 */
			if (BufferIsValid(vmbuffer))
			{
				ReleaseBuffer(vmbuffer);
				vmbuffer = InvalidBuffer;
			}

			/* Perform a round of index and heap vacuuming */
			vacrel->consider_bypass_optimization = false;
			lazy_vacuum(vacrel);

			/*
			 * Vacuum the Free Space Map to make newly-freed space visible on
			 * upper-level FSM pages. Note that blkno is the previously
			 * processed block.
			 */
			FreeSpaceMapVacuumRange(vacrel->rel, vacrel->next_fsm_block_to_vacuum,
									blkno + 1);
			vacrel->next_fsm_block_to_vacuum = blkno;

			/* Report that we are once again scanning the heap */
			pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
										 PROGRESS_VACUUM_PHASE_SCAN_HEAP);
		}

		/*
		 * If the wraparound failsafe has engaged -- either via the check
		 * above or during index vacuuming invoked from this loop -- stop
		 * using the buffer access strategy so that the rest of the vacuum may
		 * use all of shared buffers. Failsafe mode stays engaged once
		 * triggered, so we only need to do this once.
		 */
		if (unlikely(VacuumFailsafeActive) && !strategy_cleared)
		{
			read_stream_clear_strategy(stream);
			strategy_cleared = true;
		}

		buf = read_stream_next_buffer(stream, &per_buffer_data);

		/* The relation is exhausted. */
		if (!BufferIsValid(buf))
			break;

		was_eager_scanned = *((bool *) per_buffer_data);
		CheckBufferIsPinnedOnce(buf);
		page = BufferGetPage(buf);
		blkno = BufferGetBlockNumber(buf);

		vacrel->scan_data->scanned_pages++;
		if (was_eager_scanned)
			vacrel->scan_data->eager_scanned_pages++;

		/*
		 * Report as block scanned, update error traceback information. In a
		 * parallel heap scan, the blocks are processed out of order, so the
		 * leader reports the blocks handed out so far, and the workers don't
		 * report.
		 */
		if (!ParallelLVIsActive(vacrel))
			pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED, blkno);
		else if (!IsParallelWorker())
			pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED,
										 Min(pg_atomic_read_u64(&vacrel->plvstate->shared->nallocated),
											 vacrel->rel_pages));
		update_vacuum_error_info(vacrel, NULL, VACUUM_ERRCB_PHASE_SCAN_HEAP,
								 blkno, InvalidOffsetNumber);

		/*
		 * Pin the visibility map page in case we need to mark the page
		 * all-visible.  In most cases this will be very cheap, because we'll
		 * already have the correct page pinned anyway.
		 */
		visibilitymap_pin(vacrel->rel, blkno, &vmbuffer);

		/*
		 * We need a buffer cleanup lock to prune HOT chains and defragment
		 * the page in lazy_scan_prune.  But when it's not possible to acquire
		 * a cleanup lock right away, we may be able to settle for reduced
		 * processing using lazy_scan_noprune.
		 */
		got_cleanup_lock = ConditionalLockBufferForCleanup(buf);

		if (!got_cleanup_lock)
			LockBuffer(buf, BUFFER_LOCK_SHARE);

		/* Check for new or empty pages before lazy_scan_[no]prune call */
		if (lazy_scan_new_or_empty(vacrel, buf, blkno, page, !got_cleanup_lock,
								   vmbuffer))
		{
			/* Processed as new/empty page (lock and pin released) */
			continue;
		}

		/*
		 * If we didn't get the cleanup lock, we can still collect LP_DEAD
		 * items in the dead_items area for later vacuuming, count live and
		 * recently dead tuples for vacuum logging, and determine if this
		 * block could later be truncated. If we encounter any xid/mxids that
		 * require advancing the relfrozenxid/relminxid, we'll have to wait
		 * for a cleanup lock and call lazy_scan_prune().
		 */
		if (!got_cleanup_lock &&
			!lazy_scan_noprune(vacrel, buf, blkno, page, &has_lpdead_items))
		{
			/*
			 * lazy_scan_noprune could not do all required processing.  Wait
			 * for a cleanup lock, and call lazy_scan_prune in the usual way.
			 */
			Assert(vacrel->aggressive);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBufferForCleanup(buf);
			got_cleanup_lock = true;
		}

		/*
		 * If we have a cleanup lock, we must now prune, freeze, and count
		 * tuples. We may have acquired the cleanup lock originally, or we may
		 * have gone back and acquired it after lazy_scan_noprune() returned
		 * false. Either way, the page hasn't been processed yet.
		 *
		 * Like lazy_scan_noprune(), lazy_scan_prune() will count
		 * recently_dead_tuples and live tuples for vacuum logging, determine
		 * if the block can later be truncated, and accumulate the details of
		 * remaining LP_DEAD line pointers on the page into dead_items. These
		 * dead items include those pruned by lazy_scan_prune() as well as
		 * line pointers previously marked LP_DEAD.
		 */
		if (got_cleanup_lock)
			ndeleted = lazy_scan_prune(vacrel, buf, blkno, page,
									   vmbuffer,
									   &has_lpdead_items, &vm_page_frozen);

		/*
		 * Count an eagerly scanned page as a failure or a success.
		 *
		 * Only lazy_scan_prune() freezes pages, so if we didn't get the
		 * cleanup lock, we won't have frozen the page. However, we only count
		 * pages that were too new to require freezing as eager freeze
		 * failures.
		 *
		 * We could gather more information from lazy_scan_noprune() about
		 * whether or not there were tuples with XIDs or MXIDs older than the
		 * FreezeLimit or MultiXactCutoff. However, for simplicity, we simply
		 * exclude pages skipped due to cleanup lock contention from eager
		 * freeze algorithm caps.
		 */
		if (got_cleanup_lock && was_eager_scanned)
		{
			/* Aggressive vacuums do not eager scan. */
			Assert(!vacrel->aggressive);

			if (vm_page_frozen)
			{
				if (lazy_scan_eager_freeze_success(vacrel))
				{
					/*
					 * We hit our success cap. Permanently disable eager
					 * scanning by setting the other eager scan management
					 * fields to their disabled values.
					 */
					vacrel->eager_scan_remaining_fails = 0;
					vacrel->next_eager_scan_region_start = InvalidBlockNumber;
					vacrel->eager_scan_max_fails_per_region = 0;
				}
			}
			else if (vacrel->eager_scan_remaining_fails > 0)
				vacrel->eager_scan_remaining_fails--;
		}

		/*
		 * Now drop the buffer lock and, potentially, update the FSM.
		 *
		 * Our goal is to update the freespace map the last time we touch the
		 * page. If we'll process a block in the second pass, we may free up
		 * additional space on the page, so it is better to update the FSM
		 * after the second pass. If the relation has no indexes, or if index
		 * vacuuming is disabled, there will be no second heap pass; if this
		 * particular page has no dead items, the second heap pass will not
		 * touch this page. So, in those cases, update the FSM now.
		 *
		 * Note: In corner cases, it's possible to miss updating the FSM
		 * entirely. If index vacuuming is currently enabled, we'll skip the
		 * FSM update now. But if failsafe mode is later activated, or there
		 * are so few dead tuples that index vacuuming is bypassed, there will
		 * also be no opportunity to update the FSM later, because we'll never
		 * revisit this page. Since updating the FSM is desirable but not
		 * absolutely required, that's OK.
		 */
		if (vacrel->nindexes == 0
			|| !vacrel->do_index_vacuuming
			|| !has_lpdead_items)
		{
			Size		freespace = PageGetHeapFreeSpace(page);

			UnlockReleaseBuffer(buf);
			RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);

			/*
			 * Periodically perform FSM vacuuming to make newly-freed space
			 * visible on upper FSM pages. This is done after vacuuming if the
			 * table has indexes. There will only be newly-freed space if we
			 * held the cleanup lock and lazy_scan_prune() was called.
			 *
			 * A parallel heap scan leaves it to the leader, which only knows
			 * which blocks are done once the workers have finished.
			 */
			if (got_cleanup_lock && vacrel->nindexes == 0 && ndeleted > 0 &&
				!ParallelLVIsActive(vacrel))
			{
				if (blkno > vacrel->next_fsm_block_to_vacuum &&
					blkno - vacrel->next_fsm_block_to_vacuum >= VACUUM_FSM_EVERY_PAGES)
				{
					FreeSpaceMapVacuumRange(vacrel->rel, vacrel->next_fsm_block_to_vacuum,
											blkno);
					vacrel->next_fsm_block_to_vacuum = blkno;
				}
			}
		}
		else
			UnlockReleaseBuffer(buf);
	}

	vacrel->blkno = InvalidBlockNumber;
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	read_stream_end(stream);
}

/*
 * Count a successful eager freeze. Returns true if the success cap has been
 * hit, so that the caller disables eager scanning. The processes of a parallel
 * heap scan share the cap; the one that uses it up reports it.
 */
static bool
lazy_scan_eager_freeze_success(LVRelState *vacrel)
{
	bool		hit,
				report;

	if (ParallelLVIsActive(vacrel))
	{
		pg_atomic_uint32 *remaining =
			&vacrel->plvstate->shared->eager_scan_remaining_successes;
		uint32		old = pg_atomic_read_u32(remaining);

		while (old > 0 &&
			   !pg_atomic_compare_exchange_u32(remaining, &old, old - 1))
			;

		/* Hit if the count was 0 already, or we just made it 0 */
		hit = (old <= 1);
		report = (old == 1);
	}
	else
	{
		if (vacrel->eager_scan_remaining_successes > 0)
			vacrel->eager_scan_remaining_successes--;

		hit = (vacrel->eager_scan_remaining_successes == 0);

		/*
		 * Report only once that we disabled eager scanning. We may eagerly
		 * read ahead blocks in excess of the success or failure caps before
		 * attempting to freeze them, so we could reach here even after
		 * disabling additional eager scanning.
		 */
		report = (hit && vacrel->eager_scan_max_fails_per_region > 0);
	}

	if (report)
		ereport(vacrel->verbose ? INFO : DEBUG2,
				(errmsg("disabling eager scanning after freezing %u eagerly scanned blocks of relation \"%s.%s.%s\"",
						vacrel->eager_scan_success_limit,
						vacrel->dbname, vacrel->relnamespace,
						vacrel->relname)));

	return hit;
}

/*
 *	heap_vac_scan_next_block() -- read stream callback to get the next block
 *	for vacuum to process
 *
 * Every time lazy_scan_heap() needs a new block to process during its first
 * phase, it invokes read_stream_next_buffer() with a stream set up to call
 * heap_vac_scan_next_block() to get the next block.
 *
 * heap_vac_scan_next_block() uses the visibility map, vacuum options, and
 * various thresholds to skip blocks which do not need to be processed and
 * returns the next block to process or InvalidBlockNumber if there are no
 * remaining blocks. In a parallel heap scan, the blocks come from the chunks
 * the process claims, and InvalidBlockNumber is also returned once the dead
 * items exceed the memory limit, so that the stream ends and the process stops
 * where it is.
 *
 * The visibility status of the next block to process and whether or not it
 * was eager scanned is set in the per_buffer_data.
 *
 * callback_private_data contains a reference to the LVRelState, passed to the
 * read stream API during stream setup. The LVRelState is an in/out parameter
 * here (locally named `vacrel`). Vacuum options and information about the
 * relation are read from it. vacrel->scan_data->skippedallvis is set if we
 * skip a block that's all-visible but not all-frozen (to ensure that we don't
 * update relfrozenxid in that case). vacrel also holds information about the
 * next unskippable block -- as bookkeeping for this function.
 */
static BlockNumber
heap_vac_scan_next_block(ReadStream *stream,
						 void *callback_private_data,
						 void *per_buffer_data)
{
	BlockNumber next_block;
	LVRelState *vacrel = callback_private_data;
	bool		end_of_scan;

retry:

	/* relies on InvalidBlockNumber + 1 overflowing to 0 on first call */
	next_block = vacrel->current_block + 1;

	/* Have we reached the end of the relation (or of our chunks)? */
	if (ParallelLVIsActive(vacrel))
		end_of_scan = (next_block >= vacrel->scan_end &&
					   (dead_items_check_memory_limit(vacrel) ||
						!parallel_lazy_scan_next_chunk(vacrel, &next_block)));
	else
		end_of_scan = (next_block >= vacrel->rel_pages);

	if (end_of_scan)
	{
		if (BufferIsValid(vacrel->next_unskippable_vmbuffer))
		{
			ReleaseBuffer(vacrel->next_unskippable_vmbuffer);
			vacrel->next_unskippable_vmbuffer = InvalidBuffer;
		}
		return InvalidBlockNumber;
	}

	/*
	 * We must be in one of the three following states:
	 */
	if (next_block > vacrel->next_unskippable_block ||
		vacrel->next_unskippable_block == InvalidBlockNumber)
	{
		/*
		 * 1. We have just processed an unskippable block (or we're at the
		 * beginning of the scan).  Find the next unskippable block using the
		 * visibility map.
		 */
		bool		skipsallvis;
		BlockNumber end = ParallelLVIsActive(vacrel) ?
			vacrel->scan_end : vacrel->rel_pages;

		if (!find_next_unskippable_block(vacrel, &skipsallvis, next_block, end))
		{
			/*
			 * The rest of our chunk is skippable: skip it and move on to the
			 * next chunk, unless it's too short to be worth skipping (see
			 * below). Only a parallel heap scan gets here, as the last block
			 * of the relation is never skippable.
			 */
			Assert(ParallelLVIsActive(vacrel));

			if (end - next_block >= SKIP_PAGES_THRESHOLD)
			{
				if (skipsallvis)
					vacrel->scan_data->skippedallvis = true;
				vacrel->current_block = end - 1;
				vacrel->next_unskippable_block = InvalidBlockNumber;
				goto retry;
			}

			vacrel->next_unskippable_block = end - 1;
		}

		/*
		 * We now know the next block that we must process.  It can be the
		 * next block after the one we just processed, or something further
		 * ahead.  If it's further ahead, we can jump to it, but we choose to
		 * do so only if we can skip at least SKIP_PAGES_THRESHOLD consecutive
		 * pages.  Since we're reading sequentially, the OS should be doing
		 * readahead for us, so there's no gain in skipping a page now and
		 * then.  Skipping such a range might even discourage sequential
		 * detection.
		 *
		 * This test also enables more frequent relfrozenxid advancement
		 * during non-aggressive VACUUMs.  If the range has any all-visible
		 * pages then skipping makes updating relfrozenxid unsafe, which is a
		 * real downside.
		 */
		if (vacrel->next_unskippable_block - next_block >= SKIP_PAGES_THRESHOLD)
		{
			next_block = vacrel->next_unskippable_block;
			if (skipsallvis)
				vacrel->scan_data->skippedallvis = true;
		}
	}

	/* Now we must be in one of the two remaining states: */
	if (next_block < vacrel->next_unskippable_block)
	{
		/*
		 * 2. We are processing a range of blocks that we could have skipped
		 * but chose not to.  We know that they are all-visible in the VM,
		 * otherwise they would've been unskippable.
		 */
		vacrel->current_block = next_block;
		/* Block was not eager scanned */
		*((bool *) per_buffer_data) = false;
		return vacrel->current_block;
	}
	else
	{
		/*
		 * 3. We reached the next unskippable block.  Process it.  On next
		 * iteration, we will be back in state 1.
		 */
		Assert(next_block == vacrel->next_unskippable_block);

		vacrel->current_block = next_block;
		*((bool *) per_buffer_data) = vacrel->next_unskippable_eager_scanned;
		return vacrel->current_block;
	}
}

/*
 * Find the next unskippable block in a vacuum scan using the visibility map,
 * from block 'start' up to block 'end'. The next unskippable block and its
 * visibility information is updated in vacrel.
 *
 * Returns false if there is none before 'end', which only a parallel heap scan
 * can see, as the last block of the relation is never skippable.
 * vacrel->next_unskippable_block is then 'end'.
 *
 * Note: our opinion of which blocks can be skipped can go stale immediately.
 * It's okay if caller "misses" a page whose all-visible or all-frozen marking
 * was concurrently cleared, though.  All that matters is that caller scan all
 * pages whose tuples might contain XIDs < OldestXmin, or MXIDs < OldestMxact.
 * (Actually, non-aggressive VACUUMs can choose to skip all-visible pages with
 * older XIDs/MXIDs.  The *skippedallvis flag will be set here when the choice
 * to skip such a range is actually made, making everything safe.)
 */
static bool
find_next_unskippable_block(LVRelState *vacrel, bool *skipsallvis,
							BlockNumber start, BlockNumber end)
{
	BlockNumber rel_pages = vacrel->rel_pages;
	BlockNumber next_unskippable_block = start;
	Buffer		next_unskippable_vmbuffer = vacrel->next_unskippable_vmbuffer;
	bool		next_unskippable_eager_scanned = false;
	bool		found;

	*skipsallvis = false;

	for (; next_unskippable_block < end; next_unskippable_block++)
	{
		uint8		mapbits = visibilitymap_get_status(vacrel->rel,
													   next_unskippable_block,
													   &next_unskippable_vmbuffer);


		/*
		 * At the start of each eager scan region, normal vacuums with eager
		 * scanning enabled reset the failure counter, allowing vacuum to
		 * resume eager scanning if it had been suspended in the previous
		 * region.
		 */
		if (next_unskippable_block >= vacrel->next_eager_scan_region_start)
		{
			vacrel->eager_scan_remaining_fails =
				vacrel->eager_scan_max_fails_per_region;
			vacrel->next_eager_scan_region_start += EAGER_SCAN_REGION_SIZE;
		}

		/*
		 * A block is unskippable if it is not all visible according to the
		 * visibility map.
		 */
		if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) == 0)
		{
			Assert((mapbits & VISIBILITYMAP_ALL_FROZEN) == 0);
			break;
		}

		/*
		 * Caller must scan the last page to determine whether it has tuples
		 * (caller must have the opportunity to set vacrel->nonempty_pages).
		 * This rule avoids having lazy_truncate_heap() take access-exclusive
		 * lock on rel to attempt a truncation that fails anyway, just because
		 * there are tuples on the last page (it is likely that there will be
		 * tuples on other nearby pages as well, but those can be skipped).
		 *
		 * Implement this by always treating the last block as unsafe to skip.
		 */
		if (next_unskippable_block == rel_pages - 1)
			break;

		/* DISABLE_PAGE_SKIPPING makes all skipping unsafe */
		if (!vacrel->skipwithvm)
			break;

		/*
		 * All-frozen pages cannot contain XIDs < OldestXmin (XIDs that aren't
		 * already frozen by now), so this page can be skipped.
		 */
		if ((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0)
			continue;

		/*
		 * Aggressive vacuums cannot skip any all-visible pages that are not
		 * also all-frozen.
		 */
		if (vacrel->aggressive)
			break;

		/*
		 * Normal vacuums with eager scanning enabled only skip all-visible
		 * but not all-frozen pages if they have hit the failure limit for the
		 * current eager scan region.
		 */
		if (vacrel->eager_scan_remaining_fails > 0)
		{
			next_unskippable_eager_scanned = true;
			break;
		}

		/*
		 * All-visible blocks are safe to skip in a normal vacuum. But
		 * remember that the final range contains such a block for later.
		 */
		*skipsallvis = true;
	}

	found = (next_unskippable_block < end);
	Assert(found || ParallelLVIsActive(vacrel));

	/* write the local variables back to vacrel */
	vacrel->next_unskippable_block = next_unskippable_block;
	vacrel->next_unskippable_eager_scanned = next_unskippable_eager_scanned;
	vacrel->next_unskippable_vmbuffer = next_unskippable_vmbuffer;

	return found;
}

/*
 *	lazy_scan_new_or_empty() -- lazy_scan_heap() new/empty page handling.
 *
 * Must call here to handle both new and empty pages before calling
 * lazy_scan_prune or lazy_scan_noprune, since they're not prepared to deal
 * with new or empty pages.
 *
 * It's necessary to consider new pages as a special case, since the rules for
 * maintaining the visibility map and FSM with empty pages are a little
 * different (though new pages can be truncated away during rel truncation).
 *
 * Empty pages are not really a special case -- they're just heap pages that
 * have no allocated tuples (including even LP_UNUSED items).  You might
 * wonder why we need to handle them here all the same.  It's only necessary
 * because of a corner-case involving a hard crash during heap relation
 * extension.  If we ever make relation-extension crash safe, then it should
 * no longer be necessary to deal with empty pages here (or new pages, for
 * that matter).
 *
 * Caller must hold at least a shared lock.  We might need to escalate the
 * lock in that case, so the type of lock caller holds needs to be specified
 * using 'sharelock' argument.
 *
 * Returns false in common case where caller should go on to call
 * lazy_scan_prune (or lazy_scan_noprune).  Otherwise returns true, indicating
 * that lazy_scan_heap is done processing the page, releasing lock on caller's
 * behalf.
 *
 * No vm_page_frozen output parameter (like that passed to lazy_scan_prune())
 * is passed here because neither empty nor new pages can be eagerly frozen.
 * New pages are never frozen. Empty pages are always set frozen in the VM at
 * the same time that they are set all-visible, and we don't eagerly scan
 * frozen pages.
 */
static bool
lazy_scan_new_or_empty(LVRelState *vacrel, Buffer buf, BlockNumber blkno,
					   Page page, bool sharelock, Buffer vmbuffer)
{
	Size		freespace;

	if (PageIsNew(page))
	{
		/*
		 * All-zeroes pages can be left over if either a backend extends the
		 * relation by a single page, but crashes before the newly initialized
		 * page has been written out, or when bulk-extending the relation
		 * (which creates a number of empty pages at the tail end of the
		 * relation), and then enters them into the FSM.
		 *
		 * Note we do not enter the page into the visibilitymap. That has the
		 * downside that we repeatedly visit this page in subsequent vacuums,
		 * but otherwise we'll never discover the space on a promoted standby.
		 * The harm of repeated checking ought to normally not be too bad. The
		 * space usually should be used at some point, otherwise there
		 * wouldn't be any regular vacuums.
		 *
		 * Make sure these pages are in the FSM, to ensure they can be reused.
		 * Do that by testing if there's any space recorded for the page. If
		 * not, enter it. We do so after releasing the lock on the heap page,
		 * the FSM is approximate, after all.
		 */
		UnlockReleaseBuffer(buf);

		if (GetRecordedFreeSpace(vacrel->rel, blkno) == 0)
		{
			freespace = BLCKSZ - SizeOfPageHeaderData;

			RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		}

		return true;
	}

	if (PageIsEmpty(page))
	{
		/*
		 * It seems likely that caller will always be able to get a cleanup
		 * lock on an empty page.  But don't take any chances -- escalate to
		 * an exclusive lock (still don't need a cleanup lock, though).
		 */
		if (sharelock)
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			if (!PageIsEmpty(page))
			{
				/* page isn't new or empty -- keep lock and pin for now */
				return false;
			}
		}
		else
		{
			/* Already have a full cleanup lock (which is more than enough) */
		}

		/*
		 * Unlike new pages, empty pages are always set all-visible and
		 * all-frozen.
		 */
		if (!PageIsAllVisible(page))
		{
			/* Lock vmbuffer before entering critical section */
			LockBuffer(vmbuffer, BUFFER_LOCK_EXCLUSIVE);

			START_CRIT_SECTION();

			/* mark buffer dirty before writing a WAL record */
			MarkBufferDirty(buf);

			PageSetAllVisible(page);
			PageClearPrunable(page);
			(void) visibilitymap_set(blkno,
									 vmbuffer,
									 VISIBILITYMAP_ALL_VISIBLE |
									 VISIBILITYMAP_ALL_FROZEN,
									 vacrel->rel->rd_locator);

			/*
			 * Emit WAL for setting PD_ALL_VISIBLE on the heap page and
			 * setting the VM.
			 */
			if (RelationNeedsWAL(vacrel->rel))
				log_heap_prune_and_freeze(vacrel->rel, buf,
										  vmbuffer,
										  VISIBILITYMAP_ALL_VISIBLE |
										  VISIBILITYMAP_ALL_FROZEN,
										  InvalidTransactionId, /* conflict xid */
										  false,	/* cleanup lock */
										  PRUNE_VACUUM_SCAN,	/* reason */
										  NULL, 0,
										  NULL, 0,
										  NULL, 0,
										  NULL, 0);

			END_CRIT_SECTION();

			LockBuffer(vmbuffer, BUFFER_LOCK_UNLOCK);

			/* Count the newly all-frozen pages for logging */
			vacrel->scan_data->new_all_visible_pages++;
			vacrel->scan_data->new_all_visible_all_frozen_pages++;
		}

		freespace = PageGetHeapFreeSpace(page);
		UnlockReleaseBuffer(buf);
		RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		return true;
	}

	/* page isn't new or empty -- keep lock and pin */
	return false;
}

/* qsort comparator for sorting OffsetNumbers */
static int
cmpOffsetNumbers(const void *a, const void *b)
{
	return pg_cmp_u16(*(const OffsetNumber *) a, *(const OffsetNumber *) b);
}

/*
 *	lazy_scan_prune() -- lazy_scan_heap() pruning and freezing.
 *
 * Caller must hold pin and buffer cleanup lock on the buffer.
 *
 * vmbuffer is the buffer containing the VM block with visibility information
 * for the heap block, blkno.
 *
 * *has_lpdead_items is set to true or false depending on whether, upon return
 * from this function, any LP_DEAD items are still present on the page.
 *
 * *vm_page_frozen is set to true if the page is newly set all-frozen in the
 * VM. The caller currently only uses this for determining whether an eagerly
 * scanned page was successfully set all-frozen.
 *
 * Returns the number of tuples deleted from the page during HOT pruning.
 */
static int
lazy_scan_prune(LVRelState *vacrel,
				Buffer buf,
				BlockNumber blkno,
				Page page,
				Buffer vmbuffer,
				bool *has_lpdead_items,
				bool *vm_page_frozen)
{
	Relation	rel = vacrel->rel;
	PruneFreezeResult presult;
	PruneFreezeParams params = {
		.relation = rel,
		.buffer = buf,
		.vmbuffer = vmbuffer,
		.reason = PRUNE_VACUUM_SCAN,
		.options = HEAP_PAGE_PRUNE_FREEZE | HEAP_PAGE_PRUNE_SET_VM,
		.vistest = vacrel->vistest,
		.cutoffs = &vacrel->cutoffs,
	};

	Assert(BufferGetBlockNumber(buf) == blkno);

	/*
	 * Prune all HOT-update chains and potentially freeze tuples on this page.
	 *
	 * If the relation has no indexes, we can immediately mark would-be dead
	 * items LP_UNUSED.
	 *
	 * The number of tuples removed from the page is returned in
	 * presult.ndeleted.  It should not be confused with presult.lpdead_items;
	 * presult.lpdead_items's final value can be thought of as the number of
	 * tuples that were deleted from indexes.
	 *
	 * We will update the VM after collecting LP_DEAD items and freezing
	 * tuples. Pruning will have determined whether or not the page is
	 * all-visible.
	 */
	if (vacrel->nindexes == 0)
		params.options |= HEAP_PAGE_PRUNE_MARK_UNUSED_NOW;

	/*
	 * Allow skipping full inspection of pages that the VM indicates are
	 * already all-frozen (which may be scanned due to SKIP_PAGES_THRESHOLD).
	 * However, if DISABLE_PAGE_SKIPPING was specified, we can't trust the VM,
	 * so we must examine the page to make sure it is truly all-frozen and fix
	 * it otherwise.
	 */
	if (vacrel->skipwithvm)
		params.options |= HEAP_PAGE_PRUNE_ALLOW_FAST_PATH;

	heap_page_prune_and_freeze(&params,
							   &presult,
							   &vacrel->offnum,
							   &vacrel->scan_data->NewRelfrozenXid,
							   &vacrel->scan_data->NewRelminMxid);

	Assert(MultiXactIdIsValid(vacrel->scan_data->NewRelminMxid));
	Assert(TransactionIdIsValid(vacrel->scan_data->NewRelfrozenXid));

	if (presult.nfrozen > 0)
	{
		/*
		 * We don't increment the new_frozen_tuple_pages instrumentation
		 * counter when nfrozen == 0, since it only counts pages with newly
		 * frozen tuples (don't confuse that with pages newly set all-frozen
		 * in VM).
		 */
		vacrel->scan_data->new_frozen_tuple_pages++;
	}

	/*
	 * Now save details of the LP_DEAD items from the page in vacrel
	 */
	if (presult.lpdead_items > 0)
	{
		vacrel->scan_data->lpdead_item_pages++;

		/*
		 * deadoffsets are collected incrementally in
		 * heap_page_prune_and_freeze() as each dead line pointer is recorded,
		 * with an indeterminate order, but dead_items_add requires them to be
		 * sorted.
		 */
		qsort(presult.deadoffsets, presult.lpdead_items, sizeof(OffsetNumber),
			  cmpOffsetNumbers);

		dead_items_add(vacrel, blkno, presult.deadoffsets, presult.lpdead_items);
	}

	/* Finally, add page-local counts to whole-VACUUM counts */
	if (presult.newly_all_visible)
		vacrel->scan_data->new_all_visible_pages++;
	if (presult.newly_all_visible_frozen)
		vacrel->scan_data->new_all_visible_all_frozen_pages++;
	if (presult.newly_all_frozen)
		vacrel->scan_data->new_all_frozen_pages++;

	/* Capture if the page was newly set frozen */
	*vm_page_frozen = presult.newly_all_visible_frozen ||
		presult.newly_all_frozen;

	vacrel->scan_data->tuples_deleted += presult.ndeleted;
	vacrel->scan_data->tuples_frozen += presult.nfrozen;
	vacrel->scan_data->lpdead_items += presult.lpdead_items;
	vacrel->scan_data->live_tuples += presult.live_tuples;
	vacrel->scan_data->recently_dead_tuples += presult.recently_dead_tuples;

	/* Can't truncate this page (blocks come out of order when parallel) */
	if (presult.hastup)
		vacrel->scan_data->nonempty_pages =
			Max(blkno + 1, vacrel->scan_data->nonempty_pages);

	/* Did we find LP_DEAD items? */
	*has_lpdead_items = (presult.lpdead_items > 0);

	return presult.ndeleted;
}

/*
 *	lazy_scan_noprune() -- lazy_scan_prune() without pruning or freezing
 *
 * Caller need only hold a pin and share lock on the buffer, unlike
 * lazy_scan_prune, which requires a full cleanup lock.  While pruning isn't
 * performed here, it's quite possible that an earlier opportunistic pruning
 * operation left LP_DEAD items behind.  We'll at least collect any such items
 * in dead_items for removal from indexes.
 *
 * For aggressive VACUUM callers, we may return false to indicate that a full
 * cleanup lock is required for processing by lazy_scan_prune.  This is only
 * necessary when the aggressive VACUUM needs to freeze some tuple XIDs from
 * one or more tuples on the page.  We always return true for non-aggressive
 * callers.
 *
 * If this function returns true, *has_lpdead_items gets set to true or false
 * depending on whether, upon return from this function, any LP_DEAD items are
 * present on the page. If this function returns false, *has_lpdead_items
 * is not updated.
 */
static bool
lazy_scan_noprune(LVRelState *vacrel,
				  Buffer buf,
				  BlockNumber blkno,
				  Page page,
				  bool *has_lpdead_items)
{
	OffsetNumber offnum,
				maxoff;
	int			lpdead_items,
				live_tuples,
				recently_dead_tuples,
				missed_dead_tuples;
	bool		hastup;
	HeapTupleHeader tupleheader;
	TransactionId NoFreezePageRelfrozenXid = vacrel->scan_data->NewRelfrozenXid;
	MultiXactId NoFreezePageRelminMxid = vacrel->scan_data->NewRelminMxid;
	OffsetNumber deadoffsets[MaxHeapTuplesPerPage];

	Assert(BufferGetBlockNumber(buf) == blkno);

	hastup = false;				/* for now */

	lpdead_items = 0;
	live_tuples = 0;
	recently_dead_tuples = 0;
	missed_dead_tuples = 0;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;
		HeapTupleData tuple;

		vacrel->offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		if (!ItemIdIsUsed(itemid))
			continue;

		if (ItemIdIsRedirected(itemid))
		{
			hastup = true;
			continue;
		}

		if (ItemIdIsDead(itemid))
		{
			/*
			 * Deliberately don't set hastup=true here.  See same point in
			 * lazy_scan_prune for an explanation.
			 */
			deadoffsets[lpdead_items++] = offnum;
			continue;
		}

		hastup = true;			/* page prevents rel truncation */
		tupleheader = (HeapTupleHeader) PageGetItem(page, itemid);
		if (heap_tuple_should_freeze(tupleheader, &vacrel->cutoffs,
									 &NoFreezePageRelfrozenXid,
									 &NoFreezePageRelminMxid))
		{
			/* Tuple with XID < FreezeLimit (or MXID < MultiXactCutoff) */
			if (vacrel->aggressive)
			{
				/*
				 * Aggressive VACUUMs must always be able to advance rel's
				 * relfrozenxid to a value >= FreezeLimit (and be able to
				 * advance rel's relminmxid to a value >= MultiXactCutoff).
				 * The ongoing aggressive VACUUM won't be able to do that
				 * unless it can freeze an XID (or MXID) from this tuple now.
				 *
				 * The only safe option is to have caller perform processing
				 * of this page using lazy_scan_prune.  Caller might have to
				 * wait a while for a cleanup lock, but it can't be helped.
				 */
				vacrel->offnum = InvalidOffsetNumber;
				return false;
			}

			/*
			 * Non-aggressive VACUUMs are under no obligation to advance
			 * relfrozenxid (even by one XID).  We can be much laxer here.
			 *
			 * Currently we always just accept an older final relfrozenxid
			 * and/or relminmxid value.  We never make caller wait or work a
			 * little harder, even when it likely makes sense to do so.
			 */
		}

		ItemPointerSet(&(tuple.t_self), blkno, offnum);
		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationGetRelid(vacrel->rel);

		switch (HeapTupleSatisfiesVacuum(&tuple, vacrel->cutoffs.OldestXmin,
										 buf))
		{
			case HEAPTUPLE_DELETE_IN_PROGRESS:
			case HEAPTUPLE_LIVE:

				/*
				 * Count both cases as live, just like lazy_scan_prune
				 */
				live_tuples++;

				break;
			case HEAPTUPLE_DEAD:

				/*
				 * There is some useful work for pruning to do, that won't be
				 * done due to failure to get a cleanup lock.
				 */
				missed_dead_tuples++;
				break;
			case HEAPTUPLE_RECENTLY_DEAD:

				/*
				 * Count in recently_dead_tuples, just like lazy_scan_prune
				 */
				recently_dead_tuples++;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * Do not count these rows as live, just like lazy_scan_prune
				 */
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}
	}

	vacrel->offnum = InvalidOffsetNumber;

	/*
	 * By here we know for sure that caller can put off freezing and pruning
	 * this particular page until the next VACUUM.  Remember its details now.
	 * (lazy_scan_prune expects a clean slate, so we have to do this last.)
	 */
	vacrel->scan_data->NewRelfrozenXid = NoFreezePageRelfrozenXid;
	vacrel->scan_data->NewRelminMxid = NoFreezePageRelminMxid;

	/* Save any LP_DEAD items found on the page in dead_items */
	if (vacrel->nindexes == 0)
	{
		/* Using one-pass strategy (since table has no indexes) */
		if (lpdead_items > 0)
		{
			/*
			 * Perfunctory handling for the corner case where a single pass
			 * strategy VACUUM cannot get a cleanup lock, and it turns out
			 * that there is one or more LP_DEAD items: just count the LP_DEAD
			 * items as missed_dead_tuples instead. (This is a bit dishonest,
			 * but it beats having to maintain specialized heap vacuuming code
			 * forever, for vanishingly little benefit.)
			 */
			hastup = true;
			missed_dead_tuples += lpdead_items;
		}
	}
	else if (lpdead_items > 0)
	{
		/*
		 * Page has LP_DEAD items, and so any references/TIDs that remain in
		 * indexes will be deleted during index vacuuming (and then marked
		 * LP_UNUSED in the heap)
		 */
		vacrel->scan_data->lpdead_item_pages++;

		dead_items_add(vacrel, blkno, deadoffsets, lpdead_items);

		vacrel->scan_data->lpdead_items += lpdead_items;
	}

	/*
	 * Finally, add relevant page-local counts to whole-VACUUM counts
	 */
	vacrel->scan_data->live_tuples += live_tuples;
	vacrel->scan_data->recently_dead_tuples += recently_dead_tuples;
	vacrel->scan_data->missed_dead_tuples += missed_dead_tuples;
	if (missed_dead_tuples > 0)
		vacrel->scan_data->missed_dead_pages++;

	/* Can't truncate this page (blocks come out of order when parallel) */
	if (hastup)
		vacrel->scan_data->nonempty_pages =
			Max(blkno + 1, vacrel->scan_data->nonempty_pages);

	/* Did we find LP_DEAD items? */
	*has_lpdead_items = (lpdead_items > 0);

	/* Caller won't need to call lazy_scan_prune with same page */
	return true;
}

/*
 * Main entry point for index vacuuming and heap vacuuming.
 *
 * Removes items collected in dead_items from table's indexes, then marks the
 * same items LP_UNUSED in the heap.  See the comments above lazy_scan_heap
 * for full details.
 *
 * Also empties dead_items, freeing up space for later TIDs.
 *
 * We may choose to bypass index vacuuming at this point, though only when the
 * ongoing VACUUM operation will definitely only have one index scan/round of
 * index vacuuming.
 */
static void
lazy_vacuum(LVRelState *vacrel)
{
	bool		bypass;

	/* Should not end up here with no indexes */
	Assert(vacrel->nindexes > 0);
	Assert(vacrel->scan_data->lpdead_item_pages > 0);

	if (!vacrel->do_index_vacuuming)
	{
		Assert(!vacrel->do_index_cleanup);
		dead_items_reset(vacrel);
		return;
	}

	/*
	 * Consider bypassing index vacuuming (and heap vacuuming) entirely.
	 *
	 * We currently only do this in cases where the number of LP_DEAD items
	 * for the entire VACUUM operation is close to zero.  This avoids sharp
	 * discontinuities in the duration and overhead of successive VACUUM
	 * operations that run against the same table with a fixed workload.
	 * Ideally, successive VACUUM operations will behave as if there are
	 * exactly zero LP_DEAD items in cases where there are close to zero.
	 *
	 * This is likely to be helpful with a table that is continually affected
	 * by UPDATEs that can mostly apply the HOT optimization, but occasionally
	 * have small aberrations that lead to just a few heap pages retaining
	 * only one or two LP_DEAD items.  This is pretty common; even when the
	 * DBA goes out of their way to make UPDATEs use HOT, it is practically
	 * impossible to predict whether HOT will be applied in 100% of cases.
	 * It's far easier to ensure that 99%+ of all UPDATEs against a table use
	 * HOT through careful tuning.
	 */
	bypass = false;
	if (vacrel->consider_bypass_optimization && vacrel->rel_pages > 0)
	{
		BlockNumber threshold;

		Assert(vacrel->num_index_scans == 0);
		Assert(vacrel->scan_data->lpdead_items ==
			   vacrel->dead_items_info->num_items);
		Assert(vacrel->do_index_vacuuming);
		Assert(vacrel->do_index_cleanup);

		/*
		 * This crossover point at which we'll start to do index vacuuming is
		 * expressed as a percentage of the total number of heap pages in the
		 * table that are known to have at least one LP_DEAD item.  This is
		 * much more important than the total number of LP_DEAD items, since
		 * it's a proxy for the number of heap pages whose visibility map bits
		 * cannot be set on account of bypassing index and heap vacuuming.
		 *
		 * We apply one further precautionary test: the space currently used
		 * to store the TIDs (TIDs that now all point to LP_DEAD items) must
		 * not exceed 32MB.  This limits the risk that we will bypass index
		 * vacuuming again and again until eventually there is a VACUUM whose
		 * dead_items space is not CPU cache resident.
		 *
		 * We don't take any special steps to remember the LP_DEAD items (such
		 * as counting them in our final update to the stats system) when the
		 * optimization is applied.  Though the accounting used in analyze.c's
		 * acquire_sample_rows() will recognize the same LP_DEAD items as dead
		 * rows in its own stats report, that's okay. The discrepancy should
		 * be negligible.  If this optimization is ever expanded to cover more
		 * cases then this may need to be reconsidered.
		 */
		threshold = (double) vacrel->rel_pages * BYPASS_THRESHOLD_PAGES;
		bypass = (vacrel->scan_data->lpdead_item_pages < threshold &&
				  TidStoreMemoryUsage(vacrel->dead_items) < 32 * 1024 * 1024);
	}

	if (bypass)
	{
		/*
		 * There are almost zero TIDs.  Behave as if there were precisely
		 * zero: bypass index vacuuming, but do index cleanup.
		 *
		 * We expect that the ongoing VACUUM operation will finish very
		 * quickly, so there is no point in considering speeding up as a
		 * failsafe against wraparound failure. (Index cleanup is expected to
		 * finish very quickly in cases where there were no ambulkdelete()
		 * calls.)
		 */
		vacrel->do_index_vacuuming = false;
	}
	else if (lazy_vacuum_all_indexes(vacrel))
	{
		/*
		 * We successfully completed a round of index vacuuming.  Do related
		 * heap vacuuming now.
		 */
		lazy_vacuum_heap_rel(vacrel);
	}
	else
	{
		/*
		 * Failsafe case.
		 *
		 * We attempted index vacuuming, but didn't finish a full round/full
		 * index scan.  This happens when relfrozenxid or relminmxid is too
		 * far in the past.
		 *
		 * From this point on the VACUUM operation will do no further index
		 * vacuuming or heap vacuuming.  This VACUUM operation won't end up
		 * back here again.
		 */
		Assert(VacuumFailsafeActive);
	}

	/*
	 * Forget the LP_DEAD items that we just vacuumed (or just decided to not
	 * vacuum)
	 */
	dead_items_reset(vacrel);
}

/*
 *	lazy_vacuum_all_indexes() -- Main entry for index vacuuming
 *
 * Returns true in the common case when all indexes were successfully
 * vacuumed.  Returns false in rare cases where we determined that the ongoing
 * VACUUM operation is at risk of taking too long to finish, leading to
 * wraparound failure.
 */
static bool
lazy_vacuum_all_indexes(LVRelState *vacrel)
{
	bool		allindexes = true;
	double		old_live_tuples = vacrel->rel->rd_rel->reltuples;
	const int	progress_start_index[] = {
		PROGRESS_VACUUM_PHASE,
		PROGRESS_VACUUM_INDEXES_TOTAL
	};
	const int	progress_end_index[] = {
		PROGRESS_VACUUM_INDEXES_TOTAL,
		PROGRESS_VACUUM_INDEXES_PROCESSED,
		PROGRESS_VACUUM_NUM_INDEX_VACUUMS
	};
	int64		progress_start_val[2];
	int64		progress_end_val[3];

	Assert(vacrel->nindexes > 0);
	Assert(vacrel->do_index_vacuuming);
	Assert(vacrel->do_index_cleanup);

	/* Precheck for XID wraparound emergencies */
	if (lazy_check_wraparound_failsafe(vacrel))
	{
		/* Wraparound emergency -- don't even start an index scan */
		return false;
	}

	/*
	 * Report that we are now vacuuming indexes and the number of indexes to
	 * vacuum.
	 */
	progress_start_val[0] = PROGRESS_VACUUM_PHASE_VACUUM_INDEX;
	progress_start_val[1] = vacrel->nindexes;
	pgstat_progress_update_multi_param(2, progress_start_index, progress_start_val);

	if (!ParallelVacuumIsActive(vacrel))
	{
		for (int idx = 0; idx < vacrel->nindexes; idx++)
		{
			Relation	indrel = vacrel->indrels[idx];
			IndexBulkDeleteResult *istat = vacrel->indstats[idx];

			vacrel->indstats[idx] = lazy_vacuum_one_index(indrel, istat,
														  old_live_tuples,
														  vacrel);

			/* Report the number of indexes vacuumed */
			pgstat_progress_update_param(PROGRESS_VACUUM_INDEXES_PROCESSED,
										 idx + 1);

			if (lazy_check_wraparound_failsafe(vacrel))
			{
				/* Wraparound emergency -- end current index scan */
				allindexes = false;
				break;
			}
		}
	}
	else
	{
		/* Outsource everything to parallel variant */
		parallel_vacuum_bulkdel_all_indexes(vacrel->pvs, old_live_tuples,
											vacrel->num_index_scans,
											&(vacrel->worker_usage.vacuum));

		/*
		 * Do a postcheck to consider applying wraparound failsafe now.  Note
		 * that parallel VACUUM only gets the precheck and this postcheck.
		 */
		if (lazy_check_wraparound_failsafe(vacrel))
			allindexes = false;
	}

	/*
	 * We delete all LP_DEAD items from the first heap pass in all indexes on
	 * each call here (except calls where we choose to do the failsafe). This
	 * makes the next call to lazy_vacuum_heap_rel() safe (except in the event
	 * of the failsafe triggering, which prevents the next call from taking
	 * place).
	 */
	Assert(vacrel->num_index_scans > 0 ||
		   vacrel->dead_items_info->num_items == vacrel->scan_data->lpdead_items);
	Assert(allindexes || VacuumFailsafeActive);

	/*
	 * Increase and report the number of index scans.  Also, we reset
	 * PROGRESS_VACUUM_INDEXES_TOTAL and PROGRESS_VACUUM_INDEXES_PROCESSED.
	 *
	 * We deliberately include the case where we started a round of bulk
	 * deletes that we weren't able to finish due to the failsafe triggering.
	 */
	vacrel->num_index_scans++;
	progress_end_val[0] = 0;
	progress_end_val[1] = 0;
	progress_end_val[2] = vacrel->num_index_scans;
	pgstat_progress_update_multi_param(3, progress_end_index, progress_end_val);

	return allindexes;
}

/*
 * Read stream callback for vacuum's third phase (second pass over the heap).
 * Gets the next block from the TID store and returns it or InvalidBlockNumber
 * if there are no further blocks to vacuum.
 *
 * NB: Assumed to be safe to use with READ_STREAM_USE_BATCHING.
 */
static BlockNumber
vacuum_reap_lp_read_stream_next(ReadStream *stream,
								void *callback_private_data,
								void *per_buffer_data)
{
	TidStoreIter *iter = callback_private_data;
	TidStoreIterResult *iter_result;

	iter_result = TidStoreIterateNext(iter);
	if (iter_result == NULL)
		return InvalidBlockNumber;

	/*
	 * Save the TidStoreIterResult for later, so we can extract the offsets.
	 * It is safe to copy the result, according to TidStoreIterateNext().
	 */
	memcpy(per_buffer_data, iter_result, sizeof(*iter_result));

	return iter_result->blkno;
}

/*
 *	lazy_vacuum_heap_rel() -- second pass over the heap for two pass strategy
 *
 * This routine marks LP_DEAD items in vacrel->dead_items as LP_UNUSED. Pages
 * that never had lazy_scan_prune record LP_DEAD items are not visited at all.
 *
 * We may also be able to truncate the line pointer array of the heap pages we
 * visit.  If there is a contiguous group of LP_UNUSED items at the end of the
 * array, it can be reclaimed as free space.  These LP_UNUSED items usually
 * start out as LP_DEAD items recorded by lazy_scan_prune (we set items from
 * each page to LP_UNUSED, and then consider if it's possible to truncate the
 * page's line pointer array).
 *
 * Note: the reason for doing this as a second pass is we cannot remove the
 * tuples until we've removed their index entries, and we want to process
 * index entry removal in batches as large as possible.
 */
static void
lazy_vacuum_heap_rel(LVRelState *vacrel)
{
	ReadStream *stream;
	BlockNumber vacuumed_pages = 0;
	Buffer		vmbuffer = InvalidBuffer;
	LVSavedErrInfo saved_err_info;
	TidStoreIter *iter;

	Assert(vacrel->do_index_vacuuming);
	Assert(vacrel->do_index_cleanup);
	Assert(vacrel->num_index_scans > 0);

	/* Report that we are now vacuuming the heap */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_HEAP);

	/* Update error traceback information */
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_HEAP,
							 InvalidBlockNumber, InvalidOffsetNumber);

	iter = TidStoreBeginIterate(vacrel->dead_items);

	/*
	 * Set up the read stream for vacuum's second pass through the heap.
	 *
	 * It is safe to use batchmode, as vacuum_reap_lp_read_stream_next() does
	 * not need to wait for IO and does not perform locking. Once we support
	 * parallelism it should still be fine, as presumably the holder of locks
	 * would never be blocked by IO while holding the lock.
	 */
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
										READ_STREAM_USE_BATCHING,
										vacrel->bstrategy,
										vacrel->rel,
										MAIN_FORKNUM,
										vacuum_reap_lp_read_stream_next,
										iter,
										sizeof(TidStoreIterResult));

	while (true)
	{
		BlockNumber blkno;
		Buffer		buf;
		Page		page;
		TidStoreIterResult *iter_result;
		Size		freespace;
		OffsetNumber offsets[MaxOffsetNumber];
		int			num_offsets;

		vacuum_delay_point(false);

		buf = read_stream_next_buffer(stream, (void **) &iter_result);

		/* The relation is exhausted */
		if (!BufferIsValid(buf))
			break;

		vacrel->blkno = blkno = BufferGetBlockNumber(buf);

		Assert(iter_result);
		num_offsets = TidStoreGetBlockOffsets(iter_result, offsets, lengthof(offsets));
		Assert(num_offsets <= lengthof(offsets));

		/*
		 * Pin the visibility map page in case we need to mark the page
		 * all-visible.  In most cases this will be very cheap, because we'll
		 * already have the correct page pinned anyway.
		 */
		visibilitymap_pin(vacrel->rel, blkno, &vmbuffer);

		/* We need a non-cleanup exclusive lock to mark dead_items unused */
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		lazy_vacuum_heap_page(vacrel, blkno, buf, offsets,
							  num_offsets, vmbuffer);

		/* Now that we've vacuumed the page, record its available space */
		page = BufferGetPage(buf);
		freespace = PageGetHeapFreeSpace(page);

		UnlockReleaseBuffer(buf);
		RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
		vacuumed_pages++;
	}

	read_stream_end(stream);
	TidStoreEndIterate(iter);

	vacrel->blkno = InvalidBlockNumber;
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * We set all LP_DEAD items from the first heap pass to LP_UNUSED during
	 * the second heap pass.  No more, no less.
	 */
	Assert(vacrel->num_index_scans > 1 ||
		   (vacrel->dead_items_info->num_items == vacrel->scan_data->lpdead_items &&
			vacuumed_pages == vacrel->scan_data->lpdead_item_pages));

	ereport(DEBUG2,
			(errmsg("table \"%s\": removed %" PRId64 " dead item identifiers in %u pages",
					vacrel->relname, vacrel->dead_items_info->num_items,
					vacuumed_pages)));

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
}

/*
 *	lazy_vacuum_heap_page() -- free page's LP_DEAD items listed in the
 *						  vacrel->dead_items store.
 *
 * Caller must have an exclusive buffer lock on the buffer (though a full
 * cleanup lock is also acceptable).  vmbuffer must be valid and already have
 * a pin on blkno's visibility map page.
 */
static void
lazy_vacuum_heap_page(LVRelState *vacrel, BlockNumber blkno, Buffer buffer,
					  OffsetNumber *deadoffsets, int num_offsets,
					  Buffer vmbuffer)
{
	Page		page = BufferGetPage(buffer);
	OffsetNumber unused[MaxHeapTuplesPerPage];
	int			nunused = 0;
	TransactionId newest_live_xid;
	TransactionId conflict_xid = InvalidTransactionId;
	bool		all_frozen;
	LVSavedErrInfo saved_err_info;
	uint8		vmflags = 0;

	Assert(vacrel->do_index_vacuuming);

	pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED, blkno);

	/* Update error traceback information */
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_HEAP, blkno,
							 InvalidOffsetNumber);

	/*
	 * Before marking dead items unused, check whether the page will become
	 * all-visible once that change is applied. This lets us reap the tuples
	 * and mark the page all-visible within the same critical section,
	 * enabling both changes to be emitted in a single WAL record. Since the
	 * visibility checks may perform I/O and allocate memory, they must be
	 * done outside the critical section.
	 */
	if (heap_page_would_be_all_visible(vacrel->rel, buffer,
									   vacrel->vistest, true,
									   deadoffsets, num_offsets,
									   &all_frozen, &newest_live_xid,
									   &vacrel->offnum))
	{
		vmflags |= VISIBILITYMAP_ALL_VISIBLE;
		if (all_frozen)
		{
			vmflags |= VISIBILITYMAP_ALL_FROZEN;
			Assert(!TransactionIdIsValid(newest_live_xid));
		}

		/*
		 * Take the lock on the vmbuffer before entering a critical section.
		 * The heap page lock must also be held while updating the VM to
		 * ensure consistency.
		 */
		LockBuffer(vmbuffer, BUFFER_LOCK_EXCLUSIVE);
	}

	START_CRIT_SECTION();

	for (int i = 0; i < num_offsets; i++)
	{
		ItemId		itemid;
		OffsetNumber toff = deadoffsets[i];

		itemid = PageGetItemId(page, toff);

		Assert(ItemIdIsDead(itemid) && !ItemIdHasStorage(itemid));
		ItemIdSetUnused(itemid);
		unused[nunused++] = toff;
	}

	Assert(nunused > 0);

	/* Attempt to truncate line pointer array now */
	PageTruncateLinePointerArray(page);

	if ((vmflags & VISIBILITYMAP_VALID_BITS) != 0)
	{
		/*
		 * The page is guaranteed to have had dead line pointers, so we always
		 * set PD_ALL_VISIBLE.
		 */
		PageSetAllVisible(page);
		PageClearPrunable(page);
		(void) visibilitymap_set(blkno,
								 vmbuffer, vmflags,
								 vacrel->rel->rd_locator);
		conflict_xid = newest_live_xid;
	}

	/*
	 * Mark buffer dirty before we write WAL.
	 */
	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (RelationNeedsWAL(vacrel->rel))
	{
		log_heap_prune_and_freeze(vacrel->rel, buffer,
								  vmflags != 0 ? vmbuffer : InvalidBuffer,
								  vmflags,
								  conflict_xid,
								  false,	/* no cleanup lock required */
								  PRUNE_VACUUM_CLEANUP,
								  NULL, 0,	/* frozen */
								  NULL, 0,	/* redirected */
								  NULL, 0,	/* dead */
								  unused, nunused);
	}

	END_CRIT_SECTION();

	if ((vmflags & VISIBILITYMAP_ALL_VISIBLE) != 0)
	{
		/* Count the newly set VM page for logging */
		LockBuffer(vmbuffer, BUFFER_LOCK_UNLOCK);
		vacrel->scan_data->new_all_visible_pages++;
		if (all_frozen)
			vacrel->scan_data->new_all_visible_all_frozen_pages++;
	}

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
}

/*
 * Trigger the failsafe to avoid wraparound failure when vacrel table has a
 * relfrozenxid and/or relminmxid that is dangerously far in the past.
 * Triggering the failsafe makes the ongoing VACUUM bypass any further index
 * vacuuming and heap vacuuming.  Truncating the heap is also bypassed.
 *
 * Any remaining work (work that VACUUM cannot just bypass) is typically sped
 * up when the failsafe triggers.  VACUUM stops applying any cost-based delay
 * that it started out with.
 *
 * Returns true when failsafe has been triggered.
 */
static bool
lazy_check_wraparound_failsafe(LVRelState *vacrel)
{
	/* Don't warn more than once per VACUUM */
	if (VacuumFailsafeActive)
		return true;

	if (unlikely(vacuum_xid_failsafe_check(&vacrel->cutoffs)))
	{
		const int	progress_index[] = {
			PROGRESS_VACUUM_INDEXES_TOTAL,
			PROGRESS_VACUUM_INDEXES_PROCESSED,
			PROGRESS_VACUUM_MODE
		};
		int64		progress_val[3] = {0, 0, PROGRESS_VACUUM_MODE_FAILSAFE};

		VacuumFailsafeActive = true;

		/*
		 * Let the processes of a parallel heap scan in on it, so that they
		 * too stop applying the cost-based delay and stop using the buffer
		 * access strategy. Only the leader gets here.
		 */
		if (ParallelLVIsActive(vacrel))
		{
			Assert(!IsParallelWorker());
			pg_atomic_write_u32(&vacrel->plvstate->shared->failsafe_active, 1);
		}

		/*
		 * We abandon use of the strategy in failsafe mode to allow use of all
		 * of shared buffers. vacrel->bstrategy is not the source of truth for
		 * an ongoing heap scan, but clear it just for tidiness. Any ongoing
		 * phase I heap scan has its own references to the strategy and clears
		 * them separately (see lazy_scan_heap()). And none of the other
		 * vacuum phases will read from vacrel->bstrategy once failsafe mode
		 * is engaged. The phase I read stream clears the strategy references
		 * held by the ReadBuffersOperations outside of this function because
		 * lazy_check_wraparound_failsafe() may be called from any phase of
		 * vacuum, including when the phase I stream is inactive.
		 *
		 * We assume the caller who allocated the memory for the
		 * BufferAccessStrategy will free it.
		 */
		vacrel->bstrategy = NULL;

		/* Disable index vacuuming, index cleanup, and heap rel truncation */
		vacrel->do_index_vacuuming = false;
		vacrel->do_index_cleanup = false;
		vacrel->do_rel_truncate = false;

		/* Reset the progress counters and set the failsafe mode */
		pgstat_progress_update_multi_param(3, progress_index, progress_val);

		ereport(WARNING,
				(errmsg("bypassing nonessential maintenance of table \"%s.%s.%s\" as a failsafe after %d index scans",
						vacrel->dbname, vacrel->relnamespace, vacrel->relname,
						vacrel->num_index_scans),
				 errdetail("The table's relfrozenxid or relminmxid is too far in the past."),
				 errhint("Consider increasing configuration parameter \"maintenance_work_mem\" or \"autovacuum_work_mem\".\n"
						 "You might also need to consider other ways for VACUUM to keep up with the allocation of transaction IDs.")));

		/* Stop applying cost limits from this point on */
		VacuumCostActive = false;
		VacuumCostBalance = 0;

		return true;
	}

	return false;
}

/*
 *	lazy_cleanup_all_indexes() -- cleanup all indexes of relation.
 */
static void
lazy_cleanup_all_indexes(LVRelState *vacrel)
{
	double		reltuples = vacrel->new_rel_tuples;
	bool		estimated_count = vacrel->scan_data->scanned_pages < vacrel->rel_pages;
	const int	progress_start_index[] = {
		PROGRESS_VACUUM_PHASE,
		PROGRESS_VACUUM_INDEXES_TOTAL
	};
	const int	progress_end_index[] = {
		PROGRESS_VACUUM_INDEXES_TOTAL,
		PROGRESS_VACUUM_INDEXES_PROCESSED
	};
	int64		progress_start_val[2];
	int64		progress_end_val[2] = {0, 0};

	Assert(vacrel->do_index_cleanup);
	Assert(vacrel->nindexes > 0);

	/*
	 * Report that we are now cleaning up indexes and the number of indexes to
	 * cleanup.
	 */
	progress_start_val[0] = PROGRESS_VACUUM_PHASE_INDEX_CLEANUP;
	progress_start_val[1] = vacrel->nindexes;
	pgstat_progress_update_multi_param(2, progress_start_index, progress_start_val);

	if (!ParallelVacuumIsActive(vacrel))
	{
		for (int idx = 0; idx < vacrel->nindexes; idx++)
		{
			Relation	indrel = vacrel->indrels[idx];
			IndexBulkDeleteResult *istat = vacrel->indstats[idx];

			vacrel->indstats[idx] =
				lazy_cleanup_one_index(indrel, istat, reltuples,
									   estimated_count, vacrel);

			/* Report the number of indexes cleaned up */
			pgstat_progress_update_param(PROGRESS_VACUUM_INDEXES_PROCESSED,
										 idx + 1);
		}
	}
	else
	{
		/* Outsource everything to parallel variant */
		parallel_vacuum_cleanup_all_indexes(vacrel->pvs, reltuples,
											vacrel->num_index_scans,
											estimated_count,
											&(vacrel->worker_usage.cleanup));
	}

	/* Reset the progress counters */
	pgstat_progress_update_multi_param(2, progress_end_index, progress_end_val);
}

/*
 *	lazy_vacuum_one_index() -- vacuum index relation.
 *
 *		Delete all the index tuples containing a TID collected in
 *		vacrel->dead_items.  Also update running statistics. Exact
 *		details depend on index AM's ambulkdelete routine.
 *
 *		reltuples is the number of heap tuples to be passed to the
 *		bulkdelete callback.  It's always assumed to be estimated.
 *		See indexam.sgml for more info.
 *
 * Returns bulk delete stats derived from input stats
 */
static IndexBulkDeleteResult *
lazy_vacuum_one_index(Relation indrel, IndexBulkDeleteResult *istat,
					  double reltuples, LVRelState *vacrel)
{
	IndexVacuumInfo ivinfo;
	LVSavedErrInfo saved_err_info;
	const int	reset_index[] = {
		PROGRESS_VACUUM_CURRENT_INDEX_RELID,
		PROGRESS_SCAN_BLOCKS_TOTAL,
		PROGRESS_SCAN_BLOCKS_DONE
	};
	const int64 reset_val[] = {(int64) InvalidOid, 0, 0};

	ivinfo.index = indrel;
	ivinfo.heaprel = vacrel->rel;
	ivinfo.analyze_only = false;
	ivinfo.is_autovacuum = AmAutoVacuumWorkerProcess();
	ivinfo.report_progress = true;
	ivinfo.estimated_count = true;
	ivinfo.message_level = DEBUG2;
	ivinfo.num_heap_tuples = reltuples;
	ivinfo.strategy = vacrel->bstrategy;

	/* Report which index we're currently processing */
	pgstat_progress_update_param(PROGRESS_VACUUM_CURRENT_INDEX_RELID,
								 (int64) RelationGetRelid(indrel));

	/*
	 * Update error traceback information.
	 *
	 * The index name is saved during this phase and restored immediately
	 * after this phase.  See vacuum_error_callback.
	 */
	Assert(vacrel->indname == NULL);
	vacrel->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_VACUUM_INDEX,
							 InvalidBlockNumber, InvalidOffsetNumber);

	/* Do bulk deletion */
	istat = vac_bulkdel_one_index(&ivinfo, istat, vacrel->dead_items,
								  vacrel->dead_items_info);

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	pfree(vacrel->indname);
	vacrel->indname = NULL;

	pgstat_progress_update_multi_param(3, reset_index, reset_val);

	return istat;
}

/*
 *	lazy_cleanup_one_index() -- do post-vacuum cleanup for index relation.
 *
 *		Calls index AM's amvacuumcleanup routine.  reltuples is the number
 *		of heap tuples and estimated_count is true if reltuples is an
 *		estimated value.  See indexam.sgml for more info.
 *
 * Returns bulk delete stats derived from input stats
 */
static IndexBulkDeleteResult *
lazy_cleanup_one_index(Relation indrel, IndexBulkDeleteResult *istat,
					   double reltuples, bool estimated_count,
					   LVRelState *vacrel)
{
	IndexVacuumInfo ivinfo;
	LVSavedErrInfo saved_err_info;
	const int	reset_index[] = {
		PROGRESS_VACUUM_CURRENT_INDEX_RELID,
		PROGRESS_SCAN_BLOCKS_TOTAL,
		PROGRESS_SCAN_BLOCKS_DONE
	};
	const int64 reset_val[] = {(int64) InvalidOid, 0, 0};

	ivinfo.index = indrel;
	ivinfo.heaprel = vacrel->rel;
	ivinfo.analyze_only = false;
	ivinfo.is_autovacuum = AmAutoVacuumWorkerProcess();
	ivinfo.report_progress = true;
	ivinfo.estimated_count = estimated_count;
	ivinfo.message_level = DEBUG2;

	ivinfo.num_heap_tuples = reltuples;
	ivinfo.strategy = vacrel->bstrategy;

	/* Report which index we're currently processing */
	pgstat_progress_update_param(PROGRESS_VACUUM_CURRENT_INDEX_RELID,
								 (int64) RelationGetRelid(indrel));

	/*
	 * Update error traceback information.
	 *
	 * The index name is saved during this phase and restored immediately
	 * after this phase.  See vacuum_error_callback.
	 */
	Assert(vacrel->indname == NULL);
	vacrel->indname = pstrdup(RelationGetRelationName(indrel));
	update_vacuum_error_info(vacrel, &saved_err_info,
							 VACUUM_ERRCB_PHASE_INDEX_CLEANUP,
							 InvalidBlockNumber, InvalidOffsetNumber);

	istat = vac_cleanup_one_index(&ivinfo, istat);

	/* Revert to the previous phase information for error traceback */
	restore_vacuum_error_info(vacrel, &saved_err_info);
	pfree(vacrel->indname);
	vacrel->indname = NULL;

	pgstat_progress_update_multi_param(3, reset_index, reset_val);

	return istat;
}

/*
 * should_attempt_truncation - should we attempt to truncate the heap?
 *
 * Don't even think about it unless we have a shot at releasing a goodly
 * number of pages.  Otherwise, the time taken isn't worth it, mainly because
 * an AccessExclusive lock must be replayed on any hot standby, where it can
 * be particularly disruptive.
 *
 * Also don't attempt it if wraparound failsafe is in effect.  The entire
 * system might be refusing to allocate new XIDs at this point.  The system
 * definitely won't return to normal unless and until VACUUM actually advances
 * the oldest relfrozenxid -- which hasn't happened for target rel just yet.
 * If lazy_truncate_heap attempted to acquire an AccessExclusiveLock to
 * truncate the table under these circumstances, an XID exhaustion error might
 * make it impossible for VACUUM to fix the underlying XID exhaustion problem.
 * There is very little chance of truncation working out when the failsafe is
 * in effect in any case.  lazy_scan_prune makes the optimistic assumption
 * that any LP_DEAD items it encounters will always be LP_UNUSED by the time
 * we're called.
 */
static bool
should_attempt_truncation(LVRelState *vacrel)
{
	BlockNumber possibly_freeable;

	if (!vacrel->do_rel_truncate || VacuumFailsafeActive)
		return false;

	possibly_freeable = vacrel->rel_pages - vacrel->scan_data->nonempty_pages;
	if (possibly_freeable > 0 &&
		(possibly_freeable >= REL_TRUNCATE_MINIMUM ||
		 possibly_freeable >= vacrel->rel_pages / REL_TRUNCATE_FRACTION))
		return true;

	return false;
}

/*
 * lazy_truncate_heap - try to truncate off any empty pages at the end
 */
static void
lazy_truncate_heap(LVRelState *vacrel)
{
	BlockNumber orig_rel_pages = vacrel->rel_pages;
	BlockNumber new_rel_pages;
	bool		lock_waiter_detected;
	int			lock_retry;

	/* Report that we are now truncating */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_TRUNCATE);

	/* Update error traceback information one last time */
	update_vacuum_error_info(vacrel, NULL, VACUUM_ERRCB_PHASE_TRUNCATE,
							 vacrel->scan_data->nonempty_pages, InvalidOffsetNumber);

	/*
	 * Loop until no more truncating can be done.
	 */
	do
	{
		/*
		 * We need full exclusive lock on the relation in order to do
		 * truncation. If we can't get it, give up rather than waiting --- we
		 * don't want to block other backends, and we don't want to deadlock
		 * (which is quite possible considering we already hold a lower-grade
		 * lock).
		 */
		lock_waiter_detected = false;
		lock_retry = 0;
		while (true)
		{
			if (ConditionalLockRelation(vacrel->rel, AccessExclusiveLock))
				break;

			/*
			 * Check for interrupts while trying to (re-)acquire the exclusive
			 * lock.
			 */
			CHECK_FOR_INTERRUPTS();

			if (++lock_retry > (VACUUM_TRUNCATE_LOCK_TIMEOUT /
								VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL))
			{
				/*
				 * We failed to establish the lock in the specified number of
				 * retries. This means we give up truncating.
				 */
				ereport(vacrel->verbose ? INFO : DEBUG2,
						(errmsg("\"%s\": stopping truncate due to conflicting lock request",
								vacrel->relname)));
				return;
			}

			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL,
							 WAIT_EVENT_VACUUM_TRUNCATE);
			ResetLatch(MyLatch);
		}

		/*
		 * Now that we have exclusive lock, look to see if the rel has grown
		 * whilst we were vacuuming with non-exclusive lock.  If so, give up;
		 * the newly added pages presumably contain non-deletable tuples.
		 */
		new_rel_pages = RelationGetNumberOfBlocks(vacrel->rel);
		if (new_rel_pages != orig_rel_pages)
		{
			/*
			 * Note: we intentionally don't update vacrel->rel_pages with the
			 * new rel size here.  If we did, it would amount to assuming that
			 * the new pages are empty, which is unlikely. Leaving the numbers
			 * alone amounts to assuming that the new pages have the same
			 * tuple density as existing ones, which is less unlikely.
			 */
			UnlockRelation(vacrel->rel, AccessExclusiveLock);
			return;
		}

		/*
		 * Scan backwards from the end to verify that the end pages actually
		 * contain no tuples.  This is *necessary*, not optional, because
		 * other backends could have added tuples to these pages whilst we
		 * were vacuuming.
		 */
		new_rel_pages = count_nondeletable_pages(vacrel, &lock_waiter_detected);
		vacrel->blkno = new_rel_pages;

		if (new_rel_pages >= orig_rel_pages)
		{
			/* can't do anything after all */
			UnlockRelation(vacrel->rel, AccessExclusiveLock);
			return;
		}

		/*
		 * Okay to truncate.
		 */
		RelationTruncate(vacrel->rel, new_rel_pages);

		/*
		 * We can release the exclusive lock as soon as we have truncated.
		 * Other backends can't safely access the relation until they have
		 * processed the smgr invalidation that smgrtruncate sent out ... but
		 * that should happen as part of standard invalidation processing once
		 * they acquire lock on the relation.
		 */
		UnlockRelation(vacrel->rel, AccessExclusiveLock);

		/*
		 * Update statistics.  Here, it *is* correct to adjust rel_pages
		 * without also touching reltuples, since the tuple count wasn't
		 * changed by the truncation.
		 */
		vacrel->scan_data->removed_pages += orig_rel_pages - new_rel_pages;
		vacrel->rel_pages = new_rel_pages;

		ereport(vacrel->verbose ? INFO : DEBUG2,
				(errmsg("table \"%s\": truncated %u to %u pages",
						vacrel->relname,
						orig_rel_pages, new_rel_pages)));
		orig_rel_pages = new_rel_pages;
	} while (new_rel_pages > vacrel->scan_data->nonempty_pages &&
			 lock_waiter_detected);
}

/*
 * Rescan end pages to verify that they are (still) empty of tuples.
 *
 * Returns number of nondeletable pages (last nonempty page + 1).
 */
static BlockNumber
count_nondeletable_pages(LVRelState *vacrel, bool *lock_waiter_detected)
{
	StaticAssertDecl((PREFETCH_SIZE & (PREFETCH_SIZE - 1)) == 0,
					 "prefetch size must be power of 2");

	BlockNumber blkno;
	BlockNumber prefetchedUntil;
	instr_time	starttime;

	/* Initialize the starttime if we check for conflicting lock requests */
	INSTR_TIME_SET_CURRENT(starttime);

	/*
	 * Start checking blocks at what we believe relation end to be and move
	 * backwards.  (Strange coding of loop control is needed because blkno is
	 * unsigned.)  To make the scan faster, we prefetch a few blocks at a time
	 * in forward direction, so that OS-level readahead can kick in.
	 */
	blkno = vacrel->rel_pages;
	prefetchedUntil = InvalidBlockNumber;
	while (blkno > vacrel->scan_data->nonempty_pages)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		bool		hastup;

		/*
		 * Check if another process requests a lock on our relation. We are
		 * holding an AccessExclusiveLock here, so they will be waiting. We
		 * only do this once per VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL, and we
		 * only check if that interval has elapsed once every 32 blocks to
		 * keep the number of system calls and actual shared lock table
		 * lookups to a minimum.
		 */
		if ((blkno % 32) == 0)
		{
			instr_time	currenttime;
			instr_time	elapsed;

			INSTR_TIME_SET_CURRENT(currenttime);
			elapsed = currenttime;
			INSTR_TIME_SUBTRACT(elapsed, starttime);
			if ((INSTR_TIME_GET_MICROSEC(elapsed) / 1000)
				>= VACUUM_TRUNCATE_LOCK_CHECK_INTERVAL)
			{
				if (LockHasWaitersRelation(vacrel->rel, AccessExclusiveLock))
				{
					ereport(vacrel->verbose ? INFO : DEBUG2,
							(errmsg("table \"%s\": suspending truncate due to conflicting lock request",
									vacrel->relname)));

					*lock_waiter_detected = true;
					return blkno;
				}
				starttime = currenttime;
			}
		}

		/*
		 * We don't insert a vacuum delay point here, because we have an
		 * exclusive lock on the table which we want to hold for as short a
		 * time as possible.  We still need to check for interrupts however.
		 */
		CHECK_FOR_INTERRUPTS();

		blkno--;

		/* If we haven't prefetched this lot yet, do so now. */
		if (prefetchedUntil > blkno)
		{
			BlockNumber prefetchStart;
			BlockNumber pblkno;

			prefetchStart = blkno & ~(PREFETCH_SIZE - 1);
			for (pblkno = prefetchStart; pblkno <= blkno; pblkno++)
			{
				PrefetchBuffer(vacrel->rel, MAIN_FORKNUM, pblkno);
				CHECK_FOR_INTERRUPTS();
			}
			prefetchedUntil = prefetchStart;
		}

		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								 vacrel->bstrategy);

		/* In this phase we only need shared access to the buffer */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageIsEmpty(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		hastup = false;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/*
			 * Note: any non-unused item should be taken as a reason to keep
			 * this page.  Even an LP_DEAD item makes truncation unsafe, since
			 * we must not have cleaned out its index entries.
			 */
			if (ItemIdIsUsed(itemid))
			{
				hastup = true;
				break;			/* can stop scanning */
			}
		}						/* scan along page */

		UnlockReleaseBuffer(buf);

		/* Done scanning if we found a tuple here */
		if (hastup)
			return blkno + 1;
	}

	/*
	 * If we fall out of the loop, all the previously-thought-to-be-empty
	 * pages still are; we need not bother to look at the last known-nonempty
	 * page.
	 */
	return vacrel->scan_data->nonempty_pages;
}

/*
 * Allocate dead_items and dead_items_info (either using palloc, or in dynamic
 * shared memory). Sets both in vacrel for caller.
 *
 * Also handles parallel initialization as part of allocating dead_items in
 * DSM when required.
 */
static void
dead_items_alloc(LVRelState *vacrel, int nworkers)
{
	VacDeadItemsInfo *dead_items_info;
	int			vac_work_mem = AmAutoVacuumWorkerProcess() &&
		autovacuum_work_mem != -1 ?
		autovacuum_work_mem : maintenance_work_mem;
	bool		try_parallel = false;

	if (nworkers >= 0)
	{
		/*
		 * Since parallel workers cannot access data in temporary tables, we
		 * can't perform parallel vacuum on them.
		 */
		if (RelationUsesLocalBuffers(vacrel->rel))
		{
			/*
			 * Give warning only if the user explicitly tries to perform a
			 * parallel vacuum on the temporary table.
			 */
			if (nworkers > 0)
				ereport(WARNING,
						(errmsg("disabling parallel option of vacuum on \"%s\" --- cannot vacuum temporary tables in parallel",
								vacrel->relname)));
		}
		else
			try_parallel = true;
	}

	/*
	 * Initialize state for parallel index vacuuming. As of now, only one
	 * worker can be used for an index, so we invoke parallelism only if there
	 * are at least two indexes on a table.
	 */
	if (try_parallel && vacrel->nindexes > 1 && vacrel->do_index_vacuuming)
		vacrel->pvs = parallel_vacuum_init(vacrel->rel, vacrel->indrels,
										   vacrel->nindexes, nworkers,
										   vac_work_mem,
										   vacrel->verbose ? INFO : DEBUG2,
										   vacrel->bstrategy);

	if (ParallelVacuumIsActive(vacrel))
	{
		/* dead_items and dead_items_info spaces are allocated in DSM */
		vacrel->dead_items = parallel_vacuum_get_dead_items(vacrel->pvs,
															&vacrel->dead_items_info);
	}
	else
	{
		/* Allocate both dead_items and dead_items_info locally */
		dead_items_info = palloc_object(VacDeadItemsInfo);
		dead_items_info->max_bytes = vac_work_mem * (Size) 1024;
		dead_items_info->num_items = 0;
		vacrel->dead_items_info = dead_items_info;

		vacrel->dead_items = TidStoreCreateLocal(dead_items_info->max_bytes, true);
	}

	/* Initialize state for parallel heap vacuuming, sharing dead_items */
	if (try_parallel)
		parallel_lazy_scan_init(vacrel, nworkers, vac_work_mem);
}

/*
 * Add the given block number and offset numbers to dead_items.
 */
static void
dead_items_add(LVRelState *vacrel, BlockNumber blkno, OffsetNumber *offsets,
			   int num_offsets)
{
	/* The lock matters only for the shared store of a parallel heap scan */
	TidStoreLockExclusive(vacrel->dead_items);
	TidStoreSetBlockOffsets(vacrel->dead_items, blkno, offsets, num_offsets);
	TidStoreUnlock(vacrel->dead_items);

	/* Each process counts its own; the leader adds the workers' at the end */
	vacrel->dead_items_info->num_items += num_offsets;

	if (ParallelLVIsActive(vacrel))
	{
		if (IsParallelWorker())
		{
			/* Added to the leader's row now and then, not here */
			vacrel->plvstate->unreported_dead_items += num_offsets;
		}
		else
			pgstat_progress_incr_param(PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS,
									   num_offsets);
	}
	else
	{
		size_t		mem_used = TidStoreMemoryUsage(vacrel->dead_items);

		/*
		 * Note it when the dead items outgrow the memory limit, so that the
		 * scan pauses for a round of vacuuming. The usage only grows here, so
		 * this is the only place that has to look at it.
		 */
		if (mem_used > vacrel->dead_items_info->max_bytes)
			vacrel->dead_items_full = true;

		pgstat_progress_incr_param(PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS,
								   num_offsets);
		pgstat_progress_update_param(PROGRESS_VACUUM_DEAD_TUPLE_BYTES, mem_used);
	}
}

/*
 * Have the dead items outgrown the memory limit? In a parallel heap scan the
 * answer is the same for every process, and the one that notices publishes it
 * for the rest. Asking the shared store for its usage takes a lock, so only
 * call this where the scan can act on the answer: at the end of a chunk, and
 * at the end of a pass.
 */
static bool
dead_items_check_memory_limit(LVRelState *vacrel)
{
	pg_atomic_uint32 *full;

	if (!ParallelLVIsActive(vacrel))
		return vacrel->dead_items_full;

	full = &vacrel->plvstate->shared->dead_items_full;
	if (pg_atomic_read_u32(full) != 0)
		return true;

	/*
	 * Our own count, so that whoever stored the items is the one to say so.
	 * Let's force at least one page-worth of tuples to be stored as to ensure
	 * we do at least some work when the memory configured is so low that we
	 * run out before storing anything.
	 */
	if (vacrel->dead_items_info->num_items > 0 &&
		TidStoreMemoryUsage(vacrel->dead_items) >
		vacrel->dead_items_info->max_bytes)
	{
		pg_atomic_write_u32(full, 1);
		return true;
	}

	return false;
}

/*
 * Forget all collected dead items.
 */
static void
dead_items_reset(LVRelState *vacrel)
{
	const int	prog_index[2] = {
		PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS,
		PROGRESS_VACUUM_DEAD_TUPLE_BYTES
	};
	const int64 prog_val[2] = {0, 0};

	/* Update statistics for dead items */
	vacrel->num_dead_items_resets++;
	vacrel->total_dead_items_bytes += TidStoreMemoryUsage(vacrel->dead_items);

	if (ParallelVacuumIsActive(vacrel))
	{
		parallel_vacuum_reset_dead_items(vacrel->pvs);
		vacrel->dead_items = parallel_vacuum_get_dead_items(vacrel->pvs,
															&vacrel->dead_items_info);
	}
	else
	{
		size_t		max_bytes = vacrel->dead_items_info->max_bytes;

		/* Recreate the tidstore with the same max_bytes limitation */
		if (ParallelLVIsActive(vacrel))
		{
			PVShared   *shared = &vacrel->plvstate->shared->base;

			parallel_vacuum_destroy_dead_items(shared, vacrel->dead_items);
			vacrel->dead_items = parallel_vacuum_create_dead_items(shared);
		}
		else
		{
			TidStoreDestroy(vacrel->dead_items);
			vacrel->dead_items = TidStoreCreateLocal(max_bytes, true);
		}

		/* Reset the counter */
		vacrel->dead_items_info->num_items = 0;
	}

	/* The store has room again */
	vacrel->dead_items_full = false;
	if (ParallelLVIsActive(vacrel))
		pg_atomic_write_u32(&vacrel->plvstate->shared->dead_items_full, 0);

	/* The progress counters are kept by increments, so start them over */
	pgstat_progress_update_multi_param(2, prog_index, prog_val);
}

/*
 * Perform cleanup for resources allocated in dead_items_alloc
 */
static void
dead_items_cleanup(LVRelState *vacrel)
{
	/* End parallel mode */
	if (ParallelLVIsActive(vacrel))
		parallel_lazy_scan_shutdown(vacrel);

	if (ParallelVacuumIsActive(vacrel))
	{
		parallel_vacuum_end(vacrel->pvs, vacrel->indstats);
		vacrel->pvs = NULL;
	}

	/* Don't bother with pfree here */
}

/*
 * Compute the number of parallel workers to request for the heap scan. With
 * nothing requested, it comes from the number of blocks to scan.
 */
static int
parallel_lazy_scan_compute_workers(LVRelState *vacrel, int nrequested)
{
	BlockNumber rel_pages = vacrel->rel_pages;
	int			nchunks = rel_pages / PARALLEL_LV_CHUNK_SIZE;
	int			parallel_workers;
	int			max_workers;

	max_workers = AmAutoVacuumWorkerProcess() ?
		autovacuum_max_parallel_workers :
		max_parallel_maintenance_workers;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_workers == 0)
		return 0;

	/* Small relations aren't worth it, and the leader needs a chunk too */
	if (rel_pages < min_parallel_table_scan_size || nchunks < 2)
		return 0;

	if (nrequested > 0)
		parallel_workers = nrequested;
	else
	{
		BlockNumber scan_pages = rel_pages;
		int			threshold;

		/*
		 * Estimate the blocks the scan will read. DISABLE_PAGE_SKIPPING means
		 * all of them; otherwise the VM says which ones get skipped, and an
		 * aggressive vacuum only skips the all-frozen ones. The counts can be
		 * stale and eager scanning reads some all-visible blocks anyway, so
		 * this is only an estimate.
		 */
		if (vacrel->skipwithvm)
		{
			BlockNumber allvisible,
						allfrozen;

			visibilitymap_count(vacrel->rel, &allvisible, &allfrozen);
			allvisible = Min(allvisible, rel_pages);
			allfrozen = Min(allfrozen, allvisible);
			scan_pages -= vacrel->aggressive ? allfrozen : allvisible;
		}

		if (scan_pages < min_parallel_table_scan_size)
			return 0;

		/*
		 * Select the number of workers based on the log of the number of
		 * blocks to scan, as compute_parallel_worker() does for a parallel
		 * sequential scan.
		 */
		parallel_workers = 1;
		threshold = Max(min_parallel_table_scan_size, 1);
		while (scan_pages >= (BlockNumber) (threshold * 3))
		{
			parallel_workers++;
			threshold *= 3;
			if (threshold > INT_MAX / 3)
				break;			/* avoid overflow */
		}
	}

	/* Each worker needs a chunk of its own, after the leader's */
	parallel_workers = Min(parallel_workers, nchunks - 1);

	return Min(parallel_workers, max_workers);
}

/*
 * Set up the parallel heap scan, sharing the dead items if they aren't shared
 * already. Does nothing if the scan is not worth parallelizing.
 */
static void
parallel_lazy_scan_init(LVRelState *vacrel, int nworkers,
						int vac_work_mem)
{
	ParallelContext *pcxt;
	ParallelLVShared *shared;
	ParallelLVState *plvstate;
	Size		est_scan_data_len;
	int			querylen;
	BlockNumber first_chunk_end = 0;

	Assert(!IsParallelWorker());
	Assert(!ParallelLVIsActive(vacrel));

	/* Not when the failsafe has triggered, like for the index phases */
	if (VacuumFailsafeActive)
		return;

	nworkers = parallel_lazy_scan_compute_workers(vacrel, nworkers);
	if (nworkers <= 0)
		return;

	EnterParallelMode();
	pcxt = CreateParallelContext("postgres", "heap_parallel_vacuum_main",
								 nworkers);

	/* Estimate the space for everything, one key each */
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(ParallelLVShared));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* BufferUsage and WalUsage, as nobody can tell if anyone's looking */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	est_scan_data_len = mul_size(sizeof(LVScanData), nworkers);
	shm_toc_estimate_chunk(&pcxt->estimator, est_scan_data_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* keep compiler quiet */

	InitializeParallelDSM(pcxt);

	/* No DSM segment available, so no workers; scan serially instead */
	if (pcxt->nworkers == 0)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}

	plvstate = palloc0_object(ParallelLVState);
	plvstate->pcxt = pcxt;
	plvstate->next_failsafe_check = FAILSAFE_EVERY_PAGES;

	/*
	 * If eager scanning is on, the first chunk is the first eager scan
	 * region, so that the chunks line up with the regions.
	 */
	if (BlockNumberIsValid(vacrel->next_eager_scan_region_start))
	{
		first_chunk_end = vacrel->next_eager_scan_region_start;
		Assert(first_chunk_end < vacrel->rel_pages);
	}

	/* Shared information */
	shared = shm_toc_allocate(pcxt->toc, sizeof(ParallelLVShared));
	MemSet(shared, 0, sizeof(ParallelLVShared));
	parallel_vacuum_init_shared(&shared->base, pcxt, vacrel->rel,
								vacrel->bstrategy, vac_work_mem,
								vacrel->verbose ? INFO : DEBUG2);
	shared->nindexes = vacrel->nindexes;
	shared->aggressive = vacrel->aggressive;
	shared->skipwithvm = vacrel->skipwithvm;
	shared->rel_pages = vacrel->rel_pages;
	shared->cutoffs = vacrel->cutoffs;
	shared->NewRelfrozenXid = vacrel->scan_data->NewRelfrozenXid;
	shared->NewRelminMxid = vacrel->scan_data->NewRelminMxid;
	shared->eager_scan_success_limit = vacrel->eager_scan_success_limit;
	if (vacrel->eager_scan_max_fails_per_region > 0)
		shared->eager_scan_max_fails_per_chunk =
			Max(1, (BlockNumber) (vacrel->eager_scan_max_fails_per_region *
								  ((double) PARALLEL_LV_CHUNK_SIZE /
								   EAGER_SCAN_REGION_SIZE)));
	pg_atomic_init_u32(&shared->eager_scan_remaining_successes,
					   vacrel->eager_scan_remaining_successes);
	pg_atomic_init_u32(&shared->failsafe_active, 0);
	pg_atomic_init_u32(&shared->dead_items_full, 0);
	pg_atomic_init_u64(&shared->nallocated, first_chunk_end);
	shm_toc_insert(pcxt->toc, PARALLEL_LV_KEY_SHARED, shared);
	plvstate->shared = shared;

	/* Each worker's BufferUsage and WalUsage; no need to initialize */
	plvstate->buffer_usage = shm_toc_allocate(pcxt->toc,
											  mul_size(sizeof(BufferUsage), nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_LV_KEY_BUFFER_USAGE, plvstate->buffer_usage);
	plvstate->wal_usage = shm_toc_allocate(pcxt->toc,
										   mul_size(sizeof(WalUsage), nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_LV_KEY_WAL_USAGE, plvstate->wal_usage);

	/* Each worker's LVScanData; the workers initialize them */
	plvstate->scan_data_array = shm_toc_allocate(pcxt->toc, est_scan_data_len);
	shm_toc_insert(pcxt->toc, PARALLEL_LV_KEY_SCAN_DATA, plvstate->scan_data_array);

	/* Store query string for workers */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		shm_toc_insert(pcxt->toc, PARALLEL_LV_KEY_QUERY_TEXT, sharedquery);
	}

	/*
	 * The workers need dead_items in shared memory. parallel_vacuum_init()
	 * has put them there if it ran. Tables without indexes collect none, so
	 * the local store stays.
	 */
	if (!ParallelVacuumIsActive(vacrel) && vacrel->nindexes > 0)
	{
		TidStoreDestroy(vacrel->dead_items);
		vacrel->dead_items = parallel_vacuum_create_dead_items(&shared->base);
	}

	vacrel->plvstate = plvstate;

	/* The leader takes the first chunk, which the counter starts after */
	vacrel->scan_end = first_chunk_end;
	parallel_lazy_scan_eager_region(vacrel, 0);
}

/*
 * Destroy the parallel context, and end parallel mode.
 */
static void
parallel_lazy_scan_shutdown(LVRelState *vacrel)
{
	ParallelLVState *plvstate = vacrel->plvstate;

	Assert(!IsParallelWorker());

	/* The shared dead_items created by parallel_lazy_scan_init() */
	if (!ParallelVacuumIsActive(vacrel) && vacrel->nindexes > 0)
		parallel_vacuum_destroy_dead_items(&vacrel->plvstate->shared->base,
										   vacrel->dead_items);

	DestroyParallelContext(plvstate->pcxt);
	ExitParallelMode();

	pfree(vacrel->plvstate);
	vacrel->plvstate = NULL;
}

/*
 * The parallel heap scan, as seen by the leader. Launches the workers and
 * scans along with them, repeating with a round of index and heap vacuuming in
 * between whenever the dead items exceed the memory limit.
 */
static void
parallel_lazy_scan_heap_leader(LVRelState *vacrel)
{
	BlockNumber blkno;

	Assert(!IsParallelWorker());

	for (;;)
	{
		parallel_lazy_scan_heap_begin(vacrel);

		/* The leader scans too, and alone when no worker got launched */
		parallel_lazy_scan_chunks(vacrel);

		parallel_lazy_scan_heap_end(vacrel);

		/* A process only stops short at the memory limit, so we're done */
		if (!dead_items_check_memory_limit(vacrel))
			break;

		/* Perform a round of index and heap vacuuming, then scan on */
		vacrel->consider_bypass_optimization = false;
		lazy_vacuum(vacrel);

		/* Vacuum the FSM, every block handed out so far has been scanned */
		blkno = Min(pg_atomic_read_u64(&vacrel->plvstate->shared->nallocated),
					vacrel->rel_pages);
		if (blkno > vacrel->next_fsm_block_to_vacuum)
		{
			FreeSpaceMapVacuumRange(vacrel->rel,
									vacrel->next_fsm_block_to_vacuum, blkno);
			vacrel->next_fsm_block_to_vacuum = blkno;
		}

		pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
									 PROGRESS_VACUUM_PHASE_SCAN_HEAP);
	}

	Assert(pg_atomic_read_u64(&vacrel->plvstate->shared->nallocated) >=
		   vacrel->rel_pages);
}

/*
 * Launch the workers for one pass over the heap.
 */
static void
parallel_lazy_scan_heap_begin(LVRelState *vacrel)
{
	ParallelLVState *plvstate = vacrel->plvstate;
	ParallelLVShared *shared = vacrel->plvstate->shared;
	uint64		nallocated = pg_atomic_read_u64(&shared->nallocated);
	int			nchunks_left = 0;
	int			nworkers;

	Assert(!IsParallelWorker());

	/* No more workers than the chunks left, one of which the leader takes */
	if (nallocated < vacrel->rel_pages)
		nchunks_left = (vacrel->rel_pages - nallocated +
						PARALLEL_LV_CHUNK_SIZE - 1) / PARALLEL_LV_CHUNK_SIZE;
	if (nchunks_left > 0)
		nchunks_left--;
	nworkers = Min(plvstate->pcxt->nworkers, nchunks_left);

#ifdef USE_INJECTION_POINTS
	/* Launch fewer workers each time, to test finishing the chunks left */
	if (IS_INJECTION_POINT_ATTACHED("parallel-heap-vacuum-ramp-down-workers"))
		nworkers = Max(nworkers - plvstate->nlaunches, 0);
#endif

	plvstate->nworkers_launched = 0;
	if (nworkers == 0)
		return;

	/* Reinitialize parallel context to relaunch parallel workers */
	if (plvstate->nlaunches > 0)
		ReinitializeParallelDSM(plvstate->pcxt);
	ReinitializeParallelWorkers(plvstate->pcxt, nworkers);

	/* This can change between passes */
	shared->do_index_vacuuming = vacrel->do_index_vacuuming;

	/*
	 * Where the dead items are, if there are any to share. A table without
	 * indexes collects none, so its workers each make a store of their own.
	 */
	parallel_vacuum_set_dead_items(&shared->base,
								   vacrel->nindexes > 0 ? vacrel->dead_items : NULL);

	parallel_vacuum_launch_workers(plvstate->pcxt, &shared->base);
	plvstate->nlaunches++;
	plvstate->nworkers_launched = plvstate->pcxt->nworkers_launched;

	vacrel->worker_usage.heap_scan.nplanned += nworkers;
	vacrel->worker_usage.heap_scan.nlaunched += plvstate->nworkers_launched;

	ereport(vacrel->verbose ? INFO : DEBUG2,
			(errmsg(ngettext("launched %d parallel vacuum worker for heap scanning (planned: %d)",
							 "launched %d parallel vacuum workers for heap scanning (planned: %d)",
							 plvstate->nworkers_launched),
					plvstate->nworkers_launched, nworkers)));
}

/*
 * Wait for the workers of one pass to finish, and gather what they did.
 */
static void
parallel_lazy_scan_heap_end(LVRelState *vacrel)
{
	ParallelLVState *plvstate = vacrel->plvstate;
	LVScanData *scan_data = vacrel->scan_data;
	const int	prog_index[2] = {
		PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS,
		PROGRESS_VACUUM_DEAD_TUPLE_BYTES
	};
	int64		prog_val[2];

	Assert(!IsParallelWorker());

	if (plvstate->nworkers_launched > 0)
	{
		parallel_vacuum_wait_for_workers(plvstate->pcxt,
										 &vacrel->plvstate->shared->base);

		/* Take the buffer and WAL usage of the workers that just finished */
		for (int i = 0; i < plvstate->nworkers_launched; i++)
			InstrAccumParallelQuery(&plvstate->buffer_usage[i],
									&plvstate->wal_usage[i]);
	}

	for (int i = 0; i < plvstate->nworkers_launched; i++)
	{
		LVScanData *data = &plvstate->scan_data_array[i];

		/* Add the worker's counters to ours (only the leader removes pages) */
#define ACCUM_COUNT(item) scan_data->item += data->item
		ACCUM_COUNT(scanned_pages);
		ACCUM_COUNT(eager_scanned_pages);
		ACCUM_COUNT(new_frozen_tuple_pages);
		ACCUM_COUNT(new_all_visible_pages);
		ACCUM_COUNT(new_all_visible_all_frozen_pages);
		ACCUM_COUNT(new_all_frozen_pages);
		ACCUM_COUNT(lpdead_item_pages);
		ACCUM_COUNT(missed_dead_pages);
		ACCUM_COUNT(tuples_deleted);
		ACCUM_COUNT(tuples_frozen);
		ACCUM_COUNT(lpdead_items);
		ACCUM_COUNT(live_tuples);
		ACCUM_COUNT(recently_dead_tuples);
		ACCUM_COUNT(missed_dead_tuples);
#undef ACCUM_COUNT

		/* The worker added this many items to dead_items */
		vacrel->dead_items_info->num_items += data->lpdead_items;

		scan_data->nonempty_pages = Max(scan_data->nonempty_pages,
										data->nonempty_pages);

		/* The oldest XID and MXID seen by any process is what counts */
		Assert(TransactionIdIsValid(data->NewRelfrozenXid));
		Assert(MultiXactIdIsValid(data->NewRelminMxid));
		if (TransactionIdPrecedes(data->NewRelfrozenXid, scan_data->NewRelfrozenXid))
			scan_data->NewRelfrozenXid = data->NewRelfrozenXid;
		if (MultiXactIdPrecedes(data->NewRelminMxid, scan_data->NewRelminMxid))
			scan_data->NewRelminMxid = data->NewRelminMxid;

		scan_data->skippedallvis |= data->skippedallvis;
	}

	/* Now that the counts are complete, report them */
	prog_val[0] = vacrel->dead_items_info->num_items;
	prog_val[1] = TidStoreMemoryUsage(vacrel->dead_items);
	pgstat_progress_update_multi_param(2, prog_index, prog_val);
}

/*
 * Scan the chunks this process can get, until none are left or the dead items
 * are over the memory limit. Both the leader and the workers run this.
 */
static void
parallel_lazy_scan_chunks(LVRelState *vacrel)
{
	/* Count as active for the cost-based delay while scanning */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	do_lazy_scan_heap(vacrel);

	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * Claim the next chunk for this process. Returns false if none are left,
 * otherwise sets 'next_block' to its first block.
 */
static bool
parallel_lazy_scan_next_chunk(LVRelState *vacrel, BlockNumber *next_block)
{
	ParallelLVShared *shared = vacrel->plvstate->shared;
	uint64		start;

	if (IsParallelWorker())
	{
		/* A good time to report our dead items to the leader */
		parallel_lazy_scan_report_dead_items(vacrel);

		/*
		 * And to pick up the wraparound failsafe, which only the leader
		 * checks. Do as it does and stop applying the cost-based delay;
		 * do_lazy_scan_heap() takes it from here and tells the read stream to
		 * stop using the buffer access strategy.
		 */
		if (unlikely(!VacuumFailsafeActive) &&
			pg_atomic_read_u32(&shared->failsafe_active) != 0)
		{
			VacuumFailsafeActive = true;
			VacuumCostActive = false;
		}
	}

	start = pg_atomic_fetch_add_u64(&shared->nallocated, PARALLEL_LV_CHUNK_SIZE);
	if (start >= vacrel->rel_pages)
		return false;

	vacrel->scan_end = Min(start + PARALLEL_LV_CHUNK_SIZE, vacrel->rel_pages);
	*next_block = start;

	/* A new chunk is a new eager scan region */
	parallel_lazy_scan_eager_region(vacrel, start);

	return true;
}

/*
 * Start an eager scan region at 'start', unless the shared success cap has
 * been hit. Each chunk of a parallel heap scan is a region, with a failure cap
 * of its own.
 */
static void
parallel_lazy_scan_eager_region(LVRelState *vacrel, BlockNumber start)
{
	ParallelLVShared *shared = vacrel->plvstate->shared;

	if (pg_atomic_read_u32(&shared->eager_scan_remaining_successes) > 0)
	{
		vacrel->eager_scan_max_fails_per_region =
			shared->eager_scan_max_fails_per_chunk;
		vacrel->next_eager_scan_region_start = start;
	}
	else
	{
		vacrel->eager_scan_max_fails_per_region = 0;
		vacrel->next_eager_scan_region_start = InvalidBlockNumber;
	}
	vacrel->eager_scan_remaining_fails = 0;
}

/*
 * Send the dead items a worker has added since the last time to the leader,
 * for its progress row.
 */
static void
parallel_lazy_scan_report_dead_items(LVRelState *vacrel)
{
	ParallelLVState *plvstate = vacrel->plvstate;

	Assert(IsParallelWorker());

	if (plvstate->unreported_dead_items > 0)
	{
		pgstat_progress_parallel_incr_param(PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS,
											plvstate->unreported_dead_items);
		plvstate->unreported_dead_items = 0;
	}
}

/*
 * The heap scan a worker of a parallel heap vacuum does.
 */
static void
parallel_lazy_scan_heap_worker(LVRelState *vacrel, shm_toc *toc)
{
	ParallelLVState *plvstate = vacrel->plvstate;
	ParallelLVShared *shared = plvstate->shared;

	Assert(IsParallelWorker());

	vacrel->eager_scan_success_limit = shared->eager_scan_success_limit;

	/* No chunk yet, so the read stream claims one on its first call */
	vacrel->current_block = InvalidBlockNumber;
	vacrel->next_unskippable_block = InvalidBlockNumber;

	update_vacuum_error_info(vacrel, NULL, VACUUM_ERRCB_PHASE_SCAN_HEAP,
							 InvalidBlockNumber, InvalidOffsetNumber);
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_SCAN_HEAP);

	parallel_lazy_scan_chunks(vacrel);
	parallel_lazy_scan_report_dead_items(vacrel);
}

/*
 * Perform work within a launched parallel process. The setup and the teardown
 * here are the same whatever phase of the vacuum the leader launched us for.
 */
void
heap_parallel_vacuum_main(dsm_segment *seg, shm_toc *toc)
{
	LVRelState *vacrel;
	ParallelLVShared *shared;
	ParallelLVState *plvstate;
	TidStore   *dead_items;
	BufferAccessStrategy bstrategy;
	LVScanData *scan_data_array;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	char	   *sharedquery;
	Relation	rel;
	ErrorContextCallback errcallback;

	elog(DEBUG1, "starting parallel heap vacuum worker");

	shared = shm_toc_lookup(toc, PARALLEL_LV_KEY_SHARED, false);
	sharedquery = shm_toc_lookup(toc, PARALLEL_LV_KEY_QUERY_TEXT, true);

	/*
	 * The setup every parallel vacuum worker does. The leader holds the locks
	 * on the indexes for us. dead_items comes back NULL for a table without
	 * indexes, which collects none to share.
	 */
	rel = parallel_vacuum_begin_worker(&shared->base, sharedquery,
									   &dead_items, &bstrategy);

	/*
	 * If the failsafe has triggered in the leader, do as it does and stop
	 * applying the cost-based delay. This has to come after the setup above,
	 * which turns the delay on. do_lazy_scan_heap() tells the read stream to
	 * drop the buffer access strategy.
	 */
	if (pg_atomic_read_u32(&shared->failsafe_active) != 0)
	{
		VacuumFailsafeActive = true;
		VacuumCostActive = false;
	}

	/* Set up what the heap scan reads from LVRelState */
	vacrel = palloc0_object(LVRelState);
	vacrel->rel = rel;
	vacrel->nindexes = shared->nindexes;
	vacrel->aggressive = shared->aggressive;
	vacrel->skipwithvm = shared->skipwithvm;
	vacrel->do_index_vacuuming = shared->do_index_vacuuming;
	vacrel->verbose = shared->base.elevel == INFO;
	vacrel->cutoffs = shared->cutoffs;
	vacrel->vistest = GlobalVisTestFor(rel);
	vacrel->rel_pages = shared->rel_pages;
	vacrel->dbname = get_database_name(MyDatabaseId);
	vacrel->relnamespace = get_namespace_name(RelationGetNamespace(rel));
	vacrel->relname = pstrdup(RelationGetRelationName(rel));

	vacrel->bstrategy = bstrategy;

	/* The shared dead items, or one of our own if there are none to share */
	vacrel->dead_items_info = palloc_object(VacDeadItemsInfo);
	vacrel->dead_items_info->max_bytes = shared->base.dead_items_info.max_bytes;
	vacrel->dead_items_info->num_items = 0;
	if (dead_items != NULL)
		vacrel->dead_items = dead_items;
	else
		vacrel->dead_items = TidStoreCreateLocal(shared->base.dead_items_info.max_bytes,
												 true);

	/* Our scan data lives in DSM for the leader to gather */
	scan_data_array = shm_toc_lookup(toc, PARALLEL_LV_KEY_SCAN_DATA, false);
	vacrel->scan_data = &scan_data_array[ParallelWorkerNumber];
	MemSet(vacrel->scan_data, 0, sizeof(LVScanData));
	vacrel->scan_data->NewRelfrozenXid = shared->NewRelfrozenXid;
	vacrel->scan_data->NewRelminMxid = shared->NewRelminMxid;

	plvstate = palloc0_object(ParallelLVState);
	plvstate->shared = shared;
	vacrel->plvstate = plvstate;

	/* Setup error traceback support for ereport() */
	errcallback.callback = vacuum_error_callback;
	errcallback.arg = vacrel;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	parallel_lazy_scan_heap_worker(vacrel, toc);

	/* Report buffer/WAL usage during parallel execution */
	buffer_usage = shm_toc_lookup(toc, PARALLEL_LV_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_LV_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/* A store we made for ourselves is ours to destroy */
	if (dead_items == NULL)
		TidStoreDestroy(vacrel->dead_items);

	parallel_vacuum_end_worker(&shared->base, rel, dead_items, bstrategy);
}

#ifdef USE_ASSERT_CHECKING

/*
 * Wrapper for heap_page_would_be_all_visible() which can be used for callers
 * that expect no LP_DEAD on the page. Currently assert-only, but there is no
 * reason not to use it outside of asserts.
 */
bool
heap_page_is_all_visible(Relation rel, Buffer buf,
						 GlobalVisState *vistest,
						 bool *all_frozen,
						 TransactionId *newest_live_xid,
						 OffsetNumber *logging_offnum)
{
	/*
	 * Pass allow_update_vistest as false so that the GlobalVisState
	 * boundaries used here match those used by the pruning code we are
	 * cross-checking. Allowing an update could move the boundaries between
	 * the two calls, causing a spurious assertion failure.
	 */
	return heap_page_would_be_all_visible(rel, buf,
										  vistest, false,
										  NULL, 0,
										  all_frozen,
										  newest_live_xid,
										  logging_offnum);
}
#endif

/*
 * Check whether the heap page in buf is all-visible except for the dead
 * tuples referenced in the deadoffsets array.
 *
 * Vacuum uses this to check if a page would become all-visible after reaping
 * known dead tuples. This function does not remove the dead items.
 *
 * This cannot be called in a critical section, as the visibility checks may
 * perform IO and allocate memory.
 *
 * Returns true if the page is all-visible other than the provided
 * deadoffsets and false otherwise.
 *
 * vistest is used to determine visibility. If allow_update_vistest is true,
 * the boundaries of the GlobalVisState may be updated when checking the
 * visibility of the newest live XID on the page.
 *
 * Output parameters:
 *
 *  - *all_frozen: true if every tuple on the page is frozen
 *  - *newest_live_xid: newest xmin of live tuples on the page
 *  - *logging_offnum: OffsetNumber of current tuple being processed;
 *     used by vacuum's error callback system.
 *
 * Callers looking to verify that the page is already all-visible can call
 * heap_page_is_all_visible().
 *
 * This logic is closely related to heap_prune_record_unchanged_lp_normal().
 * If you modify this function, ensure consistency with that code. An
 * assertion cross-checks that both remain in agreement. Do not introduce new
 * side-effects.
 */
static bool
heap_page_would_be_all_visible(Relation rel, Buffer buf,
							   GlobalVisState *vistest,
							   bool allow_update_vistest,
							   OffsetNumber *deadoffsets,
							   int ndeadoffsets,
							   bool *all_frozen,
							   TransactionId *newest_live_xid,
							   OffsetNumber *logging_offnum)
{
	Page		page = BufferGetPage(buf);
	BlockNumber blockno = BufferGetBlockNumber(buf);
	OffsetNumber offnum,
				maxoff;
	bool		all_visible = true;
	int			matched_dead_count = 0;

	*newest_live_xid = InvalidTransactionId;
	*all_frozen = true;

	Assert(ndeadoffsets == 0 || deadoffsets);

#ifdef USE_ASSERT_CHECKING
	/* Confirm input deadoffsets[] is strictly sorted */
	if (ndeadoffsets > 1)
	{
		for (int i = 1; i < ndeadoffsets; i++)
			Assert(deadoffsets[i - 1] < deadoffsets[i]);
	}
#endif

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff && all_visible;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;
		HeapTupleData tuple;
		TransactionId dead_after;

		/*
		 * Set the offset number so that we can display it along with any
		 * error that occurred while processing this tuple.
		 */
		*logging_offnum = offnum;
		itemid = PageGetItemId(page, offnum);

		/* Unused or redirect line pointers are of no interest */
		if (!ItemIdIsUsed(itemid) || ItemIdIsRedirected(itemid))
			continue;

		ItemPointerSet(&(tuple.t_self), blockno, offnum);

		/*
		 * Dead line pointers can have index pointers pointing to them. So
		 * they can't be treated as visible
		 */
		if (ItemIdIsDead(itemid))
		{
			if (!deadoffsets ||
				matched_dead_count >= ndeadoffsets ||
				deadoffsets[matched_dead_count] != offnum)
			{
				*all_frozen = all_visible = false;
				break;
			}
			matched_dead_count++;
			continue;
		}

		Assert(ItemIdIsNormal(itemid));

		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationGetRelid(rel);

		/* Visibility checks may do IO or allocate memory */
		Assert(CritSectionCount == 0);
		switch (HeapTupleSatisfiesVacuumHorizon(&tuple, buf, &dead_after))
		{
			case HEAPTUPLE_LIVE:
				{
					TransactionId xmin;

					/* Check heap_prune_record_unchanged_lp_normal comments */
					if (!HeapTupleHeaderXminCommitted(tuple.t_data))
					{
						all_visible = false;
						*all_frozen = false;
						break;
					}

					/*
					 * The inserter definitely committed. But we don't know if
					 * it is old enough that everyone sees it as committed.
					 * Don't check that now.
					 *
					 * If we scan all tuples without finding one that prevents
					 * the page from being all-visible, we then check whether
					 * any snapshot still considers the newest XID on the page
					 * to be running. In that case, the page is not considered
					 * all-visible.
					 */
					xmin = HeapTupleHeaderGetXmin(tuple.t_data);

					/* Track newest xmin on page. */
					if (TransactionIdFollows(xmin, *newest_live_xid) &&
						TransactionIdIsNormal(xmin))
						*newest_live_xid = xmin;

					/* Check whether this tuple is already frozen or not */
					if (all_visible && *all_frozen &&
						heap_tuple_needs_eventual_freeze(tuple.t_data))
						*all_frozen = false;
				}
				break;

			case HEAPTUPLE_DEAD:
			case HEAPTUPLE_RECENTLY_DEAD:
			case HEAPTUPLE_INSERT_IN_PROGRESS:
			case HEAPTUPLE_DELETE_IN_PROGRESS:
				{
					all_visible = false;
					*all_frozen = false;
					break;
				}
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				break;
		}
	}							/* scan along page */

	/*
	 * After processing all the live tuples on the page, if the newest xmin
	 * among them may still be considered running by any snapshot, the page
	 * cannot be all-visible.
	 */
	if (all_visible &&
		TransactionIdIsNormal(*newest_live_xid) &&
		GlobalVisTestXidConsideredRunning(vistest, *newest_live_xid,
										  allow_update_vistest))
	{
		all_visible = false;
		*all_frozen = false;
	}

	/* Clear the offset information once we have processed the given page. */
	*logging_offnum = InvalidOffsetNumber;

	return all_visible;
}

/*
 * Update index statistics in pg_class if the statistics are accurate.
 */
static void
update_relstats_all_indexes(LVRelState *vacrel)
{
	Relation   *indrels = vacrel->indrels;
	int			nindexes = vacrel->nindexes;
	IndexBulkDeleteResult **indstats = vacrel->indstats;

	Assert(vacrel->do_index_cleanup);

	for (int idx = 0; idx < nindexes; idx++)
	{
		Relation	indrel = indrels[idx];
		IndexBulkDeleteResult *istat = indstats[idx];

		if (istat == NULL || istat->estimated_count)
			continue;

		/* Update index statistics */
		vac_update_relstats(indrel,
							istat->num_pages,
							istat->num_index_tuples,
							0, 0,
							false,
							InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL, false);
	}
}

/*
 * Error context callback for errors occurring during vacuum.  The error
 * context messages for index phases should match the messages set in parallel
 * vacuum.  If you change this function for those phases, change
 * parallel_vacuum_error_callback() as well.
 */
static void
vacuum_error_callback(void *arg)
{
	LVRelState *errinfo = arg;

	switch (errinfo->phase)
	{
		case VACUUM_ERRCB_PHASE_SCAN_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
			{
				if (OffsetNumberIsValid(errinfo->offnum))
					errcontext("while scanning block %u offset %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->offnum, errinfo->relnamespace, errinfo->relname);
				else
					errcontext("while scanning block %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
			}
			else
				errcontext("while scanning relation \"%s.%s\"",
						   errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_VACUUM_HEAP:
			if (BlockNumberIsValid(errinfo->blkno))
			{
				if (OffsetNumberIsValid(errinfo->offnum))
					errcontext("while vacuuming block %u offset %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->offnum, errinfo->relnamespace, errinfo->relname);
				else
					errcontext("while vacuuming block %u of relation \"%s.%s\"",
							   errinfo->blkno, errinfo->relnamespace, errinfo->relname);
			}
			else
				errcontext("while vacuuming relation \"%s.%s\"",
						   errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_VACUUM_INDEX:
			errcontext("while vacuuming index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname, errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_INDEX_CLEANUP:
			errcontext("while cleaning up index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname, errinfo->relnamespace, errinfo->relname);
			break;

		case VACUUM_ERRCB_PHASE_TRUNCATE:
			if (BlockNumberIsValid(errinfo->blkno))
				errcontext("while truncating relation \"%s.%s\" to %u blocks",
						   errinfo->relnamespace, errinfo->relname, errinfo->blkno);
			break;

		case VACUUM_ERRCB_PHASE_UNKNOWN:
		default:
			return;				/* do nothing; the errinfo may not be
								 * initialized */
	}
}

/*
 * Updates the information required for vacuum error callback.  This also saves
 * the current information which can be later restored via restore_vacuum_error_info.
 */
static void
update_vacuum_error_info(LVRelState *vacrel, LVSavedErrInfo *saved_vacrel,
						 int phase, BlockNumber blkno, OffsetNumber offnum)
{
	if (saved_vacrel)
	{
		saved_vacrel->offnum = vacrel->offnum;
		saved_vacrel->blkno = vacrel->blkno;
		saved_vacrel->phase = vacrel->phase;
	}

	vacrel->blkno = blkno;
	vacrel->offnum = offnum;
	vacrel->phase = phase;
}

/*
 * Restores the vacuum information saved via a prior call to update_vacuum_error_info.
 */
static void
restore_vacuum_error_info(LVRelState *vacrel,
						  const LVSavedErrInfo *saved_vacrel)
{
	vacrel->blkno = saved_vacrel->blkno;
	vacrel->offnum = saved_vacrel->offnum;
	vacrel->phase = saved_vacrel->phase;
}
