/*-------------------------------------------------------------------------
 *
 * wait_event_timing.h
 *	  Per-backend wait event timing instrumentation.
 *
 * When enabled via the wait_event_capture GUC, every transition through
 * pgstat_report_wait_start()/pgstat_report_wait_end() records the wait
 * duration and accumulates per-(backend, event) statistics -- count,
 * total/maximum duration, and a log2 duration histogram -- in shared
 * memory.  Each backend writes only to its own slot, so the hot path
 * needs no locking; cross-backend readers (the pg_stat_wait_event_timing
 * SRF) read lock-free and tolerate torn reads of 64-bit fields on 32-bit
 * platforms, which is acceptable for statistics.
 *
 * The per-backend slot array is allocated lazily in a DSA the first time
 * any backend in the cluster sets wait_event_capture != off, so a build
 * that compiles the feature in but never enables it pays no per-backend
 * memory.  Backends attach to the array on their first wait event under
 * capture (pgstat_wait_event_timing_lazy_attach), and a before_shmem_exit
 * gate stops the hot path from touching DSA mappings that proc_exit has
 * already torn down.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/utils/wait_event_timing.h
 *-------------------------------------------------------------------------
 */
#ifndef WAIT_EVENT_TIMING_H
#define WAIT_EVENT_TIMING_H

#include "port/atomics.h"
#include "portability/instr_time.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"
#include "utils/wait_event_types.h"

/*
 * Capture levels for the wait_event_capture GUC.  Order is significant:
 * higher values are strict supersets of lower ones, so code paths can
 * test for activation with "level >= WAIT_EVENT_CAPTURE_STATS".
 *
 *   OFF   - No instrumentation, no hot-path cost.
 *   STATS - Aggregated per-event statistics (counts, durations, histogram)
 *           exposed via pg_stat_wait_event_timing.
 *   TRACE - Everything in STATS plus a per-session ring buffer of individual
 *           wait events and query-attribution markers, exposed via
 *           pg_backend_wait_event_trace.
 */
typedef enum WaitEventCaptureLevel
{
	WAIT_EVENT_CAPTURE_OFF = 0,
	WAIT_EVENT_CAPTURE_STATS,
	WAIT_EVENT_CAPTURE_TRACE,
} WaitEventCaptureLevel;

/*
 * Pin the enum ordering at compile time so code that compares with >= keeps
 * working, and so reordering is caught at build time rather than via
 * mysterious runtime mode switches.
 */
StaticAssertDecl(WAIT_EVENT_CAPTURE_OFF == 0 &&
				 WAIT_EVENT_CAPTURE_STATS == 1 &&
				 WAIT_EVENT_CAPTURE_TRACE == 2,
				 "WaitEventCaptureLevel values must be 0=OFF < 1=STATS < 2=TRACE");

/*
 * Number of log2 histogram buckets.  Bin edges are powers of two on the
 * nanosecond axis: bucket 0 covers [0, 1024) ns, bucket i covers
 * [2^(i+9), 2^(i+10)) ns, and the last bucket is open-ended at
 * [2^(NBUCKETS+8), inf) ns.  These boundaries approximate the
 * decimal-microsecond grid (1024 ~ 1 us, 2048 ~ 2 us, ...), which lets
 * wait_event_timing_bucket() avoid a divide on the hot path.
 *
 * 32 buckets cover from <1us through the open-ended overflow at 2^40 ns
 * (~18 minutes), so the long tail (lock contention, vacuum, replication
 * apply, noisy-neighbour I/O spikes) lands in a real bucket rather than a
 * single overflow bin -- which is exactly where tail/P99 analysis pays
 * off.
 */
#define WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS	32

/* Sentinel returned by wait_event_timing_index() for LWLock events. */
#define WAIT_EVENT_TIMING_IDX_LWLOCK	(-2)

/*
 * Per-event accumulated statistics.  One entry per distinct wait event per
 * backend, written only by the owning backend.
 */
typedef struct WaitEventTimingEntry
{
	int64		count;			/* number of occurrences */
	int64		total_ns;		/* total wait duration in nanoseconds */
	int64		max_ns;			/* longest single wait in nanoseconds */
	int64		histogram[WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS];
} WaitEventTimingEntry;

/*
 * Sentinel marking an empty LWLock-hash slot.  We reserve the top of the
 * uint16 range (0xFFFF) rather than 0 so that any legal tranche id --
 * including the currently-unused tranche 0 -- can be stored and matched.
 */
#define LWLOCK_TIMING_EMPTY_SLOT	((uint16) 0xFFFF)

/*
 * Open-addressing hash slot mapping an LWLock tranche id to a dense index
 * into the per-backend lwlock_events[] array.  Per-backend, single-writer.
 */
typedef struct LWLockTimingHashEntry
{
	uint16		tranche_id;		/* LWLOCK_TIMING_EMPTY_SLOT marks empty */
	uint16		dense_idx;		/* index into lwlock_events[] */
} LWLockTimingHashEntry;

/*
 * Header for the per-backend LWLock-timing hash.  The slot table and the
 * dense events array follow the WaitEventTimingState in memory (their
 * lengths are runtime-determined by wait_event_timing_max_tranches), so
 * they are not struct members; resolve them via the helpers in
 * wait_event_timing.c.
 */
typedef struct LWLockTimingHash
{
	int			num_used;		/* count of occupied entries */
	int			hash_size;		/* slot-table size (power of two); immutable */
	int			max_entries;	/* cap on distinct tranches; immutable */
} LWLockTimingHash;

/* Declaration of the GUC (see guc_parameters.dat). */
extern PGDLLIMPORT int wait_event_timing_max_tranches;

/*
 * Per-backend wait event timing state.  One slot per
 * MaxBackends + NUM_AUXILIARY_PROCS, written exclusively by the owning
 * backend.  Shared-memory layout of one slot:
 *
 *     [ WaitEventTimingState header ]
 *     [ LWLockTimingHashEntry[hash_size] ]
 *     [ WaitEventTimingEntry[max_entries]      <- lwlock_events[] ]
 *
 * where hash_size and max_entries are runtime-derived from the GUC
 * wait_event_timing_max_tranches and recorded in lwlock_hash.  Slots are
 * laid out contiguously in the lazily-allocated DSA region using a runtime
 * stride rather than C array indexing, since the per-backend size is
 * determined at first enable.
 */
typedef struct WaitEventTimingState
{
	/*
	 * Generation counter for cross-backend reset requests.  Bumped atomically
	 * by pg_stat_reset_wait_event_timing(target); the owning backend notices
	 * the change at its next wait_end and clears its own counters.  This
	 * keeps the hot path lock-free: only the owning backend ever writes its
	 * statistics, so there is no writer/resetter race.
	 */
	pg_atomic_uint32 reset_generation;

	/* Current wait start timestamp (set by pgstat_report_wait_start). */
	instr_time	wait_start;

	/* Current wait_event_info (cached for use in wait_end). */
	uint32		current_event;

	/*
	 * Number of resets this backend has observed and acted on.  Own-backend
	 * resets are synchronous (one bump per call); cross-backend resets
	 * coalesce (multiple requests between two wait_ends count as one).
	 */
	int64		reset_count;

	/* Per-event statistics: flat array for bounded classes. */
	WaitEventTimingEntry events[WAIT_EVENT_TIMING_NUM_EVENTS];

	/* Per-event statistics: hash for the LWLock class (unbounded ids). */
	LWLockTimingHash lwlock_hash;

	/*
	 * Count of LWLock events dropped because the LWLock-timing hash reached
	 * its cap (wait_event_timing_max_tranches).  Exposed via
	 * pg_stat_wait_event_timing_overflow.
	 */
	int64		lwlock_overflow_count;

	/* Count of events dropped because the class index was out of range. */
	int64		flat_overflow_count;
} WaitEventTimingState;

/* GUC variables (see guc_parameters.dat). */
extern PGDLLIMPORT int wait_event_capture;

/* Pointer to this backend's timing state in shared memory. */
extern PGDLLIMPORT WaitEventTimingState *my_wait_event_timing;

/*
 * Called from InitProcess()/InitAuxiliaryProcess() to set up this backend's
 * timing/trace bookkeeping, and from ProcKill() to clear it.
 */
extern void pgstat_set_wait_event_timing_storage(int procNumber);
extern void pgstat_reset_wait_event_timing_storage(void);


/* ----------------------------------------------------------------------
 * Trace level (wait_event_capture = trace)
 *
 * In addition to the STATS aggregates, every completed wait (and a set of
 * query-attribution markers) is pushed into a per-session ring buffer --
 * one record per completed wait.  The ring is allocated lazily in DSA on
 * first use,
 * so only sessions that enable trace pay the per-ring memory cost.  External
 * tools read a session's ring via pg_get_backend_wait_event_trace() (own
 * session) or pg_get_wait_event_trace(procnumber) (cross-backend).
 *
 * Query attribution is by scanning the ring at read time: QUERY/EXEC
 * START/END markers delimit which wait events belong to which query_id.
 *
 * The ring size is set cluster-wide at server start by the
 * wait_event_trace_ring_size GUC (PGC_POSTMASTER, default 4 MB).  It
 * MUST be a power of two: the writer indexes the ring as (pos & ring_mask).
 * ----------------------------------------------------------------------
 */

/* Trace record types */
#define TRACE_WAIT_EVENT	0
#define TRACE_QUERY_START	1
#define TRACE_QUERY_END		2
#define TRACE_EXEC_START	3
#define TRACE_EXEC_END		4

typedef struct WaitEventTraceRecord
{
	/*
	 * Seqlock for torn-read detection.  Writers set seq odd before filling
	 * fields, then even after; readers check seq before and after and skip
	 * the record if either is odd or they differ.  uint32 wrap is irrelevant
	 * over the ~10-20 ns reader access window.
	 */
	uint32		seq;
	uint8		record_type;	/* TRACE_WAIT_EVENT / QUERY_* / EXEC_* */
	uint8		pad[3];
	int64		timestamp_ns;	/* monotonic clock */
	union
	{
		struct					/* record_type = TRACE_WAIT_EVENT */
		{
			uint32		event;	/* wait_event_info */
			uint32		pad2;
			int64		duration_ns;
		}			wait;
		struct					/* QUERY_START/END or EXEC_START/END */
		{
			int64		query_id;
			int64		pad2;
		}			query;
	}			data;
} WaitEventTraceRecord;			/* 32 bytes */

/*
 * The seqlock wrap-safety argument and the mask-index math both rely on a
 * fixed 32-byte record stride; make a stray field addition a build failure.
 */
StaticAssertDecl(sizeof(WaitEventTraceRecord) == 32,
				 "WaitEventTraceRecord must be exactly 32 bytes");

/*
 * Per-backend trace ring header followed by the records array.  records[]
 * is variably sized at allocation time (wait_event_trace_ring_size
 * decides the row count).  write_pos and ring_mask share a cache line so
 * the hot-path index calculation touches one line.
 */
typedef struct WaitEventTraceState
{
	pg_atomic_uint64 write_pos; /* monotonically increasing, wraps via mask */
	uint32		ring_mask;		/* ring_size - 1; ring_size is a power of two */
	uint32		ring_size_pad;	/* keep the records[] slab 16-byte aligned */
	WaitEventTraceRecord records[FLEXIBLE_ARRAY_MEMBER];
} WaitEventTraceState;

/*
 * Per-procNumber trace-ring slot lifecycle.  Decoupled from backend
 * lifecycle on purpose: when a backend exits we transition its slot to
 * ORPHANED and leave the ring in DSA so cross-backend consumers can still
 * read the dying backend's final waits.  An orphan is reclaimed when a new
 * backend takes the same procNumber, or by
 * pg_stat_clear_orphaned_wait_event_rings().
 *
 *   FREE      no ring allocated (ring_ptr invalid).
 *   OWNED     a live backend at this procNumber is writing to the ring.
 *   ORPHANED  the owner exited; the ring is post-mortem and immutable.
 */
typedef enum WaitEventTraceSlotState
{
	WAIT_EVENT_TRACE_SLOT_FREE = 0,
	WAIT_EVENT_TRACE_SLOT_OWNED,
	WAIT_EVENT_TRACE_SLOT_ORPHANED,
} WaitEventTraceSlotState;

/*
 * Per-procNumber slot.  generation is bumped on every owner transition;
 * cross-backend readers snapshot it before+after their read and retry if it
 * changed (the BackendStatusArray st_changecount idiom).  state is atomic
 * only for cheap unlocked "worth visiting" probes; authoritative reads of
 * (state, ring_ptr) are done under WaitEventTraceCtl->lock in LW_SHARED,
 * while every transition holds it LW_EXCLUSIVE.
 */
typedef struct WaitEventTraceSlot
{
	pg_atomic_uint64 generation;	/* bumped on every owner transition */
	pg_atomic_uint32 state;		/* WaitEventTraceSlotState */
	uint32		pad;			/* keep ring_ptr 8-aligned */
	dsa_pointer ring_ptr;		/* InvalidDsaPointer when FREE; else the
								 * WaitEventTraceState chunk */
} WaitEventTraceSlot;

/*
 * Control struct in fixed shared memory.  trace_slots[] is indexed by
 * procNumber.
 *
 * External cross-backend reader protocol (pg_get_wait_event_trace is the
 * reference implementation):
 *   1. read trace_slots[procNumber].state unlocked as a "worth visiting"
 *      probe; FREE -> nothing to read.
 *   2. acquire lock LW_SHARED (all transitions take LW_EXCLUSIVE, so the
 *      slot's state/ring_ptr/ring memory are stable for the iteration).
 *   3. re-check state under the lock; resolve ring_ptr via dsa_get_address;
 *      read write_pos.
 *   4. iterate [read_start, write_pos): for each record do the per-record
 *      POSITION-ENCODED IDENTITY seqlock check against shared memory --
 *      expected_seq = (uint32)(i*2 + 2); read seq, barrier, copy record,
 *      barrier, re-read seq; accept only if both equal expected_seq.  This
 *      rejects stale previous-cycle reads (parity alone would not).
 *   5. release the lock; emit the buffered records afterwards.
 *   6. optional: snapshot generation before/after if releasing the lock
 *      between batches.
 */
typedef struct WaitEventTraceControl
{
	dsa_handle	trace_dsa_handle;	/* DSA_HANDLE_INVALID until first use */
	LWLock		lock;			/* protects DSA creation and slot transitions */
	WaitEventTraceSlot trace_slots[FLEXIBLE_ARRAY_MEMBER];	/* per procNumber */
} WaitEventTraceControl;

/* Trace GUC and the records-per-ring value derived from it at startup. */
extern PGDLLIMPORT int wait_event_trace_ring_size;
extern PGDLLIMPORT uint32 WaitEventTraceRingSize;

/* This backend's procNumber for the trace ring, or -1 if not set. */
extern PGDLLIMPORT int my_trace_proc_number;

/*
 * Lazy DSA-based trace ring allocation -- called on first trace write and
 * at backend startup when capture = trace was set via configuration.
 */
extern void wait_event_trace_attach(int procNumber);

/* Query-attribution markers (defined in wait_event_timing.c). */
extern void wait_event_trace_query_start(int64 query_id);
extern void wait_event_trace_query_end(int64 query_id);
extern void wait_event_trace_exec_start(int64 query_id);
extern void wait_event_trace_exec_end(int64 query_id);

#endif							/* WAIT_EVENT_TIMING_H */
