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
 * The per-backend slot array lives in the main shared memory segment,
 * sized at postmaster start (see WaitEventTimingShmemCallbacks).  It is
 * therefore valid for the entire life of every backend, including the
 * proc_exit cascade -- no lazy attach and no teardown gating are needed.
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
#include "utils/wait_event_types.h"

/*
 * Capture levels for the wait_event_capture GUC.  Order is significant:
 * higher values are strict supersets of lower ones, so code paths can
 * test for activation with "level >= WAIT_EVENT_CAPTURE_STATS".
 *
 *   OFF   - No instrumentation, no hot-path cost.
 *   STATS - Aggregated per-event statistics (counts, durations, histogram)
 *           exposed via pg_stat_wait_event_timing.
 *
 * A further TRACE level is added later in the series.
 */
typedef enum WaitEventCaptureLevel
{
	WAIT_EVENT_CAPTURE_OFF = 0,
	WAIT_EVENT_CAPTURE_STATS,
} WaitEventCaptureLevel;

/*
 * Pin the enum ordering at compile time so future code that compares with
 * >= against WAIT_EVENT_CAPTURE_STATS keeps working, and so reordering is
 * caught at build time rather than via mysterious runtime mode switches.
 */
StaticAssertDecl(WAIT_EVENT_CAPTURE_OFF == 0 &&
				 WAIT_EVENT_CAPTURE_STATS == 1,
				 "WaitEventCaptureLevel values must be 0=OFF < 1=STATS");

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
 * laid out contiguously in the main shared memory segment using a runtime
 * stride rather than C array indexing, since the per-backend size is
 * determined at server start.
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
 * Called from InitProcess()/InitAuxiliaryProcess() to point
 * my_wait_event_timing at this backend's slot, and from ProcKill() to
 * clear it.
 */
extern void pgstat_set_wait_event_timing_storage(int procNumber);
extern void pgstat_reset_wait_event_timing_storage(void);

#endif							/* WAIT_EVENT_TIMING_H */
