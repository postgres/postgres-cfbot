/*-------------------------------------------------------------------------
 *
 * wait_event_timing.c
 *	  Per-backend wait event timing and histogram accumulation.
 *
 * Every transition through pgstat_report_wait_start()/_end() records the
 * wait duration with INSTR_TIME (a clock_gettime()-grade timestamp) and
 * accumulates per-(backend, event) statistics -- count, total/max
 * nanoseconds, and a log2 duration histogram -- in shared memory.  Each
 * backend writes only to its own slot, so the hot path needs no locking.
 *
 * The per-backend slot array is allocated lazily in a DSA the first time
 * any backend in the cluster enables capture, so a build that compiles the
 * feature in but never enables it pays no per-backend memory.  Backends
 * attach on their first wait event under capture; a before_shmem_exit gate
 * keeps the hot path away from DSA mappings that proc_exit has torn down.
 *
 * At the trace level, every completed wait (and a set of query-attribution
 * markers) is additionally pushed into a per-session DSA ring buffer that
 * survives backend exit for post-mortem reading; see the trace section
 * below and the lifecycle discussion on WaitEventTraceControl.
 *
 * Controlled by the wait_event_capture GUC (off | stats | trace, default
 * off) and the compile-time option --enable-wait-event-timing.  In builds
 * without that option the file still compiles (the GUC backing variables,
 * the enum table, the rejecting check hook, and empty-result SQL stubs),
 * so the GUC and the catalog functions exist uniformly.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/wait_event_timing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/subsystems.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/wait_event_timing.h"

/*
 * GUC variables -- always defined so the GUC system has backing variables
 * even when compiled without --enable-wait-event-timing.
 */
int			wait_event_capture = WAIT_EVENT_CAPTURE_OFF;
int			wait_event_timing_max_tranches = 192;
int			wait_event_trace_ring_size = 4096;

/*
 * Records-per-ring derived from wait_event_trace_ring_size at server
 * start.  Read by the writer (via the per-ring cached mask) and by the ring
 * allocator.  Zero until the GUC framework commits the boot value.
 */
uint32		WaitEventTraceRingSize = 0;

/*
 * Enum value table consumed by guc.c.  Order matches the
 * WaitEventCaptureLevel enum and the documented "off < stats < trace".
 */
const struct config_enum_entry wait_event_capture_options[] = {
	{"off", WAIT_EVENT_CAPTURE_OFF, false},
	{"stats", WAIT_EVENT_CAPTURE_STATS, false},
	{"trace", WAIT_EVENT_CAPTURE_TRACE, false},
	{NULL, 0, false}
};

/*
 * GUC check hook for wait_event_trace_ring_size.  The ring size in records
 * must be a power of two for the writer's mask-indexing (pos & ring_mask).
 * Each record is 32 bytes, so kb is a power of two iff the record count is.
 * Defined for both build configurations so the GUC framework validates it
 * uniformly.
 */
bool
check_wait_event_trace_ring_size(int *newval, void **extra, GucSource source)
{
	int			v = *newval;

	if (v <= 0 || (v & (v - 1)) != 0)
	{
		GUC_check_errdetail("wait_event_trace_ring_size must be a positive power of two.");
		return false;
	}
	return true;
}

#ifndef USE_WAIT_EVENT_TIMING

/*
 * Stub build: no instrumentation.  Provide the symbols referenced by
 * pg_proc.dat, the GUC machinery, and the shmem subsystem registry.
 */
#include "fmgr.h"
#include "funcapi.h"

Datum		pg_stat_get_wait_event_timing(PG_FUNCTION_ARGS);
Datum		pg_stat_get_wait_event_timing_overflow(PG_FUNCTION_ARGS);
Datum		pg_stat_reset_wait_event_timing(PG_FUNCTION_ARGS);
Datum		pg_stat_reset_wait_event_timing_all(PG_FUNCTION_ARGS);
Datum		pg_get_backend_wait_event_trace(PG_FUNCTION_ARGS);
Datum		pg_get_wait_event_trace(PG_FUNCTION_ARGS);
Datum		pg_stat_clear_orphaned_wait_event_rings(PG_FUNCTION_ARGS);

Datum
pg_stat_get_wait_event_timing(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	PG_RETURN_VOID();
}

Datum
pg_stat_get_wait_event_timing_overflow(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	PG_RETURN_VOID();
}

Datum
pg_get_backend_wait_event_trace(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	PG_RETURN_VOID();
}

Datum
pg_get_wait_event_trace(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	PG_RETURN_VOID();
}

Datum
pg_stat_clear_orphaned_wait_event_rings(PG_FUNCTION_ARGS)
{
	/* No trace infrastructure in this build, so never any orphans. */
	PG_RETURN_INT64(0);
}

Datum
pg_stat_reset_wait_event_timing(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("wait event capture is not supported by this build"),
			 errhint("Compile PostgreSQL with --enable-wait-event-timing.")));
	PG_RETURN_VOID();
}

Datum
pg_stat_reset_wait_event_timing_all(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("wait event capture is not supported by this build"),
			 errhint("Compile PostgreSQL with --enable-wait-event-timing.")));
	PG_RETURN_VOID();
}

/*
 * GUC check hook for the stub build.  Any value other than 'off' is
 * meaningless without --enable-wait-event-timing, so reject it -- or
 * downgrade to 'off' with a warning when the value comes from a
 * non-interactive source, so a leftover setting does not block startup.
 */
bool
check_wait_event_capture(int *newval, void **extra, GucSource source)
{
	if (*newval != WAIT_EVENT_CAPTURE_OFF)
	{
		if (source < PGC_S_INTERACTIVE)
		{
			ereport(WARNING,
					(errmsg("wait_event_capture is not supported by this build, "
							"forcing to \"off\""),
					 errhint("Compile PostgreSQL with "
							 "--enable-wait-event-timing.")));
			*newval = WAIT_EVENT_CAPTURE_OFF;
			return true;
		}
		GUC_check_errdetail("This build does not support wait event capture.");
		GUC_check_errhint("Compile PostgreSQL with --enable-wait-event-timing.");
		return false;
	}
	return true;
}

void
assign_wait_event_capture(int newval, void *extra)
{
}

/* No shared memory is reserved in the stub build. */
const ShmemCallbacks WaitEventTimingShmemCallbacks = {0};
const ShmemCallbacks WaitEventTraceControlShmemCallbacks = {0};

/* Defined so every extern in wait_event_timing.h resolves in stub builds. */
WaitEventTimingState *my_wait_event_timing = NULL;
int			my_trace_proc_number = -1;

void
pgstat_set_wait_event_timing_storage(int procNumber)
{
}

void
pgstat_reset_wait_event_timing_storage(void)
{
}

void
wait_event_trace_attach(int procNumber)
{
}

/*
 * Stub trace-marker entry points -- declared unconditionally in the header
 * so call sites in execMain.c/postgres.c/backend_status.c need no #ifdef.
 */
void
wait_event_trace_query_start(int64 query_id)
{
}

void
wait_event_trace_query_end(int64 query_id)
{
}

void
wait_event_trace_exec_start(int64 query_id)
{
}

void
wait_event_trace_exec_end(int64 query_id)
{
}

#else							/* USE_WAIT_EVENT_TIMING */

#include "catalog/pg_authid.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procnumber.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/injection_point.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#define NUM_WAIT_EVENT_TIMING_SLOTS  (MaxBackends + NUM_AUXILIARY_PROCS)

#define HAS_PGSTAT_PERMISSIONS(role) \
	(has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS) || \
	 has_privs_of_role(GetUserId(), role))

/* Pointer to this backend's timing state in shared memory. */
WaitEventTimingState *my_wait_event_timing = NULL;

/*
 * Backend-local copy of the last reset generation this backend acted on.
 * Compared against the shared reset_generation at every wait_end; when they
 * differ, the owning backend performs the reset of its own counters on
 * behalf of whoever called pg_stat_reset_wait_event_timing(target).
 */
static uint32 my_last_reset_generation = 0;

/*
 * DSA-based control struct in fixed shared memory.  The large per-backend
 * WaitEventTimingState array is allocated lazily in DSA the first time any
 * backend in the cluster sets wait_event_capture != off, so a build that
 * compiles the feature in but never enables it pays no per-backend memory.
 */
typedef struct WaitEventTimingControl
{
	LWLock		lock;			/* protects first-time DSA create + array
								 * alloc */
	dsa_handle	timing_dsa_handle;	/* DSA_HANDLE_INVALID until first enable */
	dsa_pointer timing_array;	/* InvalidDsaPointer until first enable */
} WaitEventTimingControl;

static WaitEventTimingControl *WaitEventTimingCtl = NULL;
static dsa_area *timing_dsa = NULL;

/*
 * Per-backend gate raised by the before_shmem_exit callback once proc_exit
 * begins tearing down DSA mappings (dsm_backend_shutdown runs as a later
 * on_shmem_exit callback).  Once set, the wait-event hot path skips all
 * timing work -- including the lazy re-attach branch -- so it cannot
 * dereference my_wait_event_timing or run DSA primitives on already-detached
 * memory.  Per-backend (process-local), so the hot-path check is a single
 * cache-warm load.
 */
static bool wait_event_timing_writes_disabled = false;

/*
 * Backend-local cached pointer to the start of the shared slot array, set on
 * first lazy-attach.  Slots are NOT a simple C array: each has a
 * runtime-determined stride (header + variable-size hash arrays); use
 * wet_slot() to index.
 */
static char *WaitEventTimingArray = NULL;

/*
 * Per-backend slot stride and the hash dimensions, all derived from the
 * GUC wait_event_timing_max_tranches.  Because the GUC is PGC_POSTMASTER,
 * every backend in the cluster derives identical values, so the shared
 * layout is consistent across fork() and EXEC_BACKEND.
 */
static Size wait_event_timing_per_backend_stride = 0;
static int	wait_event_timing_hash_size = 0;
static int	wait_event_timing_max_entries = 0;

/* ---- Trace-level (wait_event_capture = trace) state ---- */
/* Pointer to this backend's trace ring buffer */
static WaitEventTraceState *my_wait_event_trace = NULL;

/* DSA-based trace ring buffer control */
static WaitEventTraceControl *WaitEventTraceCtl = NULL;
static dsa_area *trace_dsa = NULL;
int			my_trace_proc_number = -1;

/*
 * Same-backend coordination between pg_get_backend_wait_event_trace (the
 * own-session SRF reader) and wait_event_trace_release_slot (the GUC
 * step-down path that frees this backend's ring).  Both paths run in this
 * same backend, single-threaded, so a plain bool is sufficient -- no
 * atomics needed.
 *
 *   srf_in_progress   set true while the SRF is iterating the ring; the
 *                     release path observes this and defers the dsa_free
 *                     instead of yanking the chunk out from under us.
 *
 *   release_pending   set by the release path when it had to defer; the
 *                     SRF's PG_FINALLY checks it and performs the deferred
 *                     dsa_free after the iteration completes.
 *
 * Cross-backend readers (extensions, bgworkers reading another backend's
 * ring) cannot use this mechanism -- they coordinate with the release
 * path via WaitEventTraceCtl->lock instead.  See the header for the
 * recommended snapshot-under-lock pattern for those consumers.
 */
static bool wait_event_trace_srf_in_progress = false;
static bool wait_event_trace_release_pending = false;

/*
 * Per-backend gate that disables the trace-ring writer in the wait-
 * event hot path while a slot-state transition is in progress.
 *
 * Set true around code paths that either free the local trace ring
 * (wait_event_trace_release_slot's dsa_free) or transition the slot
 * out of OWNED (wait_event_trace_before_shmem_exit's OWNED ->
 * ORPHANED publish).  In both cases an internal LWLock inside
 * dsa_free / dsa_attach / dsa_pin_mapping / dsa_pin can in
 * principle contend long enough to dispatch a wait event; that
 * wait event's pgstat_report_wait_end_timing inline path runs in
 * the SAME backend, sees capture_level == TRACE (the GUC hasn't
 * been committed yet by the time the assign hook runs), and would:
 *
 *   * during release_slot's dsa_free: write into a ring that has
 *     already been returned to the DSA freelist -- if another
 *     allocator has since reused the chunk, this is a stray write
 *     into someone else's allocation.
 *
 *   * during release_slot's dsa_free, alternative timing: see
 *     my_wait_event_trace == NULL on a naive "clear before free"
 *     fix and recurse into wait_event_trace_attach, which would
 *     either deadlock on the WaitEventTraceCtl->lock the outer
 *     release_slot already holds, or (on a lock-free moment)
 *     allocate a fresh ring that the outer release_slot would
 *     then free again as part of its post-acquire DsaPointerIsValid
 *     check -- a different use-after-free of a freshly-allocated
 *     chunk.
 *
 *   * during before_shmem_exit: write into the ring after the slot
 *     has been published as ORPHANED, violating the post-mortem
 *     read-only contract that cross-backend readers rely on.
 *
 * The flag is per-backend (static at file scope means per-process
 * in PG's process-per-backend model), so the hot path's check is a
 * single cache-warm load and a branch; no atomic, no fence.  The
 * trace branch is already gated by capture_level == TRACE so the
 * additional check costs nothing in the common case where capture
 * is off or stats-only.  The flag is set on the very same backend
 * that may later read it from the hot path, so there is no
 * cross-process visibility concern.
 *
 * See the release_slot and before_shmem_exit doc comments for the
 * specific transition each uses this flag around.
 */
static bool wait_event_trace_writes_disabled = false;

/*
 * Re-entrancy guard for the trace-record writer in
 * pgstat_report_wait_end_timing(): wait events emitted while a trace
 * record is mid-write are not themselves ring-recorded.  In production the
 * record write is a handful of plain stores and cannot wait, so this never
 * fires; it exists for the injection point inside the writer, whose wait
 * machinery emits nested wait events that would otherwise recurse into the
 * writer (and, by re-running the injection point, self-deadlock on locks
 * the outer invocation holds).  Per-backend, single-threaded -- a plain
 * bool suffices.
 */
static bool wait_event_trace_in_write = false;

/*
 * Forward declarations for trace functions referenced before their
 * definitions (the storage/assign hooks call them; the bodies are in the
 * trace machinery section below).
 */
static void wait_event_trace_detach(int procNumber);
static void wait_event_trace_release_slot(int procNumber);
static void wait_event_trace_clear_orphan_at_init(int procNumber);

/*
 * Mapping arrays for the flat events[] array, generated from
 * wait_event_names.txt by generate-wait_event_types.pl.  Defines
 * wait_event_class_dense / _nevents / _offset / _dense_to_classid.
 */
#include "utils/wait_event_timing_data.h"

/*
 * Round up to the next power of two, with a minimum of 32.  The hash slot
 * count must be a power of two for the mask-based modulo in the lookup hot
 * path; we target >= 2x the entry cap so the load factor stays <= 50%.
 */
static int
wait_event_timing_hash_size_for(int max_entries)
{
	int			size = 32;

	while (size < max_entries * 2)
		size <<= 1;
	return size;
}

/*
 * Compute the per-backend slot size for the given max_entries.  Layout:
 *
 *     [ WaitEventTimingState header ]
 *     [ LWLockTimingHashEntry[hash_size] ]
 *     [ WaitEventTimingEntry[max_entries]    <- lwlock_events[] ]
 */
static Size
wait_event_timing_slot_size(int max_entries)
{
	int			hash_size = wait_event_timing_hash_size_for(max_entries);

	return add_size(sizeof(WaitEventTimingState),
					add_size(mul_size(hash_size, sizeof(LWLockTimingHashEntry)),
							 mul_size(max_entries, sizeof(WaitEventTimingEntry))));
}

/*
 * Ensure this backend is attached to the timing DSA.  The DSA is created by
 * whichever backend first reaches here with an empty control struct;
 * subsequent callers attach to the existing handle.  The backend-local
 * dsa_area pointer is cached in timing_dsa for the backend's lifetime.
 */
static void
wait_event_timing_ensure_dsa(void)
{
	MemoryContext oldcontext;

	if (timing_dsa != NULL)
		return;

	if (WaitEventTimingCtl == NULL)
		return;					/* pre-ShmemInit; nothing to attach to */

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	LWLockAcquire(&WaitEventTimingCtl->lock, LW_EXCLUSIVE);

	if (WaitEventTimingCtl->timing_dsa_handle == DSA_HANDLE_INVALID)
	{
		timing_dsa = dsa_create(LWTRANCHE_WAIT_EVENT_TIMING_DSA);
		dsa_pin(timing_dsa);
		dsa_pin_mapping(timing_dsa);
		WaitEventTimingCtl->timing_dsa_handle = dsa_get_handle(timing_dsa);
	}
	else
	{
		timing_dsa = dsa_attach(WaitEventTimingCtl->timing_dsa_handle);
		dsa_pin_mapping(timing_dsa);
	}

	LWLockRelease(&WaitEventTimingCtl->lock);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Attach this backend to the shared per-backend slot array, allocating it in
 * DSA on first use when allocate_if_missing is true.  Returns true if the
 * array is now available (WaitEventTimingArray non-NULL).  Readers pass
 * false so a SELECT against an empty view does not force a big allocation;
 * the hot path passes true so the first wait event under capture != off
 * creates the storage.
 *
 * Re-entrancy guard: dsa_create / dsa_allocate / the LWLockAcquire inside
 * ensure_dsa can themselves emit LWLock wait events, which feed back into
 * the wait-end timing hot path, which lazy-attaches by calling this
 * function.  The in_attach guard prevents deadlock on the control lock and
 * recursion with a half-initialised pointer.
 */
static bool
wait_event_timing_attach_array(bool allocate_if_missing)
{
	static bool in_attach = false;
	bool		attached = false;

	if (WaitEventTimingArray != NULL)
		return true;

	if (WaitEventTimingCtl == NULL)
		return false;

	/*
	 * Reader fast path: if no backend has ever enabled capture, the DSA was
	 * never created.  Return without calling ensure_dsa() so a plain SELECT
	 * against an empty view does not create (and pin for the postmaster's
	 * lifetime) an otherwise-unused DSA segment.  Only the first enabler
	 * (allocate_if_missing) brings the DSA into existence.
	 */
	if (!allocate_if_missing &&
		WaitEventTimingCtl->timing_dsa_handle == DSA_HANDLE_INVALID)
		return false;

	if (in_attach)
		return false;

	in_attach = true;
	PG_TRY();
	{
		wait_event_timing_ensure_dsa();

		if (WaitEventTimingCtl->timing_array == InvalidDsaPointer)
		{
			if (!allocate_if_missing)
			{
				attached = false;
			}
			else
			{
				int			max_entries;
				int			hash_size;
				Size		stride;
				Size		total;

				/*
				 * Snapshot the GUC once for the cluster-wide first-enable
				 * allocation; every slot shares these dimensions for the
				 * cluster's lifetime (the GUC is PGC_POSTMASTER).
				 */
				max_entries = wait_event_timing_max_tranches;
				hash_size = wait_event_timing_hash_size_for(max_entries);
				stride = wait_event_timing_slot_size(max_entries);
				total = mul_size(NUM_WAIT_EVENT_TIMING_SLOTS, stride);

				LWLockAcquire(&WaitEventTimingCtl->lock, LW_EXCLUSIVE);

				if (WaitEventTimingCtl->timing_array == InvalidDsaPointer)
				{
					dsa_pointer p;
					char	   *region;
					int			i;

					p = dsa_allocate_extended(timing_dsa, total, DSA_ALLOC_ZERO);
					region = (char *) dsa_get_address(timing_dsa, p);

					for (i = 0; i < NUM_WAIT_EVENT_TIMING_SLOTS; i++)
					{
						WaitEventTimingState *slot;
						LWLockTimingHashEntry *slot_entries;
						int			j;

						slot = (WaitEventTimingState *) (region + (Size) i * stride);

						pg_atomic_init_u32(&slot->reset_generation, 0);
						slot->lwlock_hash.num_used = 0;
						slot->lwlock_hash.hash_size = hash_size;
						slot->lwlock_hash.max_entries = max_entries;

						/*
						 * DSA_ALLOC_ZERO zeroed the region, but the empty
						 * sentinel is 0xFFFF, not 0.
						 */
						slot_entries = (LWLockTimingHashEntry *)
							((char *) slot + sizeof(WaitEventTimingState));
						for (j = 0; j < hash_size; j++)
							slot_entries[j].tranche_id = LWLOCK_TIMING_EMPTY_SLOT;
					}

					WaitEventTimingCtl->timing_array = p;
				}

				LWLockRelease(&WaitEventTimingCtl->lock);
				attached = true;
			}
		}
		else
			attached = true;

		if (attached)
		{
			WaitEventTimingState *first;

			WaitEventTimingArray = (char *)
				dsa_get_address(timing_dsa, WaitEventTimingCtl->timing_array);

			/*
			 * Recover the dimensions from the first slot's header (all slots
			 * share them) and cache the stride so wet_slot() is a single
			 * multiply-and-add.
			 */
			first = (WaitEventTimingState *) WaitEventTimingArray;
			wait_event_timing_max_entries = first->lwlock_hash.max_entries;
			wait_event_timing_hash_size = first->lwlock_hash.hash_size;
			wait_event_timing_per_backend_stride =
				wait_event_timing_slot_size(wait_event_timing_max_entries);
		}
	}
	PG_FINALLY();
	{
		in_attach = false;
	}
	PG_END_TRY();

	return WaitEventTimingArray != NULL;
}

/* Resolve the address of slot `idx` within WaitEventTimingArray. */
static inline WaitEventTimingState *
wet_slot(int idx)
{
	return (WaitEventTimingState *)
		(WaitEventTimingArray + (Size) idx * wait_event_timing_per_backend_stride);
}

/*
 * Address of the LWLock hash slot table for a slot (immediately follows
 * the WaitEventTimingState header).
 */
static inline LWLockTimingHashEntry *
wet_lwlock_hash_entries(WaitEventTimingState *state)
{
	return (LWLockTimingHashEntry *) ((char *) state + sizeof(WaitEventTimingState));
}

/*
 * Address of the dense LWLock events array for a slot (immediately follows
 * the slot table).
 */
static inline WaitEventTimingEntry *
wet_lwlock_hash_events(WaitEventTimingState *state)
{
	return (WaitEventTimingEntry *)
		((char *) state + sizeof(WaitEventTimingState)
		 + (Size) state->lwlock_hash.hash_size * sizeof(LWLockTimingHashEntry));
}

/*
 * Convert wait_event_info to a flat index for the events[] array.  Returns
 * WAIT_EVENT_TIMING_IDX_LWLOCK for LWLock events (which use the hash) and
 * -1 for events outside the mapped classes.
 */
static int
wait_event_timing_index(uint32 wait_event_info)
{
	uint32		classId = wait_event_info & WAIT_EVENT_CLASS_MASK;
	int			eventId = wait_event_info & WAIT_EVENT_ID_MASK;
	int			class_byte;
	int			dense;

	if (classId == PG_WAIT_LWLOCK)
		return WAIT_EVENT_TIMING_IDX_LWLOCK;

	class_byte = classId >> 24;
	if (class_byte >= WAIT_EVENT_TIMING_RAW_CLASSES)
		return -1;

	dense = wait_event_class_dense[class_byte];
	if (dense < 0)
		return -1;

	if (eventId >= wait_event_class_nevents[dense])
		return -1;

	return wait_event_class_offset[dense] + eventId;
}

/*
 * Reset a slot's LWLockTimingHash to its empty initial state.  The hash
 * header's hash_size and max_entries are immutable and not reset here.
 */
static void
lwlock_timing_hash_clear(WaitEventTimingState *state)
{
	LWLockTimingHash *ht = &state->lwlock_hash;
	LWLockTimingHashEntry *entries = wet_lwlock_hash_entries(state);
	WaitEventTimingEntry *events = wet_lwlock_hash_events(state);
	int			i;

	ht->num_used = 0;
	memset(events, 0, (Size) ht->max_entries * sizeof(WaitEventTimingEntry));
	for (i = 0; i < ht->hash_size; i++)
	{
		entries[i].tranche_id = LWLOCK_TIMING_EMPTY_SLOT;
		entries[i].dense_idx = 0;
	}
}

/*
 * Maximum probes attempted on the lookup hot path once the table is at
 * capacity.  At cap an unknown tranche cannot be inserted, so bounding the
 * scan caps the per-event cost instead of walking clustered occupied slots
 * on every unknown-tranche wait_end.  8 is well above the expected probe
 * distance at the target load factor.
 */
#define LWLOCK_TIMING_LOOKUP_AT_CAP_PROBE_LIMIT 8

/*
 * Look up (or insert) the timing entry for an LWLock tranche id.  Returns
 * NULL when the table is at capacity and the tranche is not already
 * present.
 */
static WaitEventTimingEntry *
lwlock_timing_lookup(WaitEventTimingState *state, uint16 tranche_id)
{
	LWLockTimingHash *ht = &state->lwlock_hash;
	LWLockTimingHashEntry *entries = wet_lwlock_hash_entries(state);
	WaitEventTimingEntry *events = wet_lwlock_hash_events(state);
	uint32		hash = (uint32) tranche_id * 2654435761U;
	int			slot = hash & (ht->hash_size - 1);
	int			limit;
	int			i;

	limit = (ht->num_used >= ht->max_entries)
		? LWLOCK_TIMING_LOOKUP_AT_CAP_PROBE_LIMIT
		: ht->hash_size;

	for (i = 0; i < limit; i++)
	{
		LWLockTimingHashEntry *e = &entries[slot];

		if (e->tranche_id == tranche_id)
			return &events[e->dense_idx];

		if (e->tranche_id == LWLOCK_TIMING_EMPTY_SLOT)
		{
			if (ht->num_used >= ht->max_entries)
				return NULL;

			e->tranche_id = tranche_id;
			e->dense_idx = ht->num_used++;
			return &events[e->dense_idx];
		}

		slot = (slot + 1) & (ht->hash_size - 1);
	}

	return NULL;
}

/*
 * Compute the histogram bucket index for a duration in nanoseconds.  See
 * the rationale on WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS in the header.
 */
static int
wait_event_timing_bucket(int64 duration_ns)
{
	int			bucket;

	/*
	 * Everything under ~1us (and 0, undefined for pg_leftmost_one_pos64)
	 * lands in bucket 0.
	 */
	if (duration_ns < 1024)
		return 0;

	bucket = pg_leftmost_one_pos64((uint64) duration_ns) - 9;

	if (bucket >= WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS)
		bucket = WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS - 1;

	return bucket;
}

/*
 * ShmemRequest/Init: reserve only the small control struct in fixed shmem.
 * The large per-backend array is allocated lazily in DSA on first enable.
 */
static void
WaitEventTimingShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "WaitEventTimingControl",
					   .size = sizeof(WaitEventTimingControl),
					   .ptr = (void **) &WaitEventTimingCtl);
}

static void
WaitEventTimingShmemInit(void *arg)
{
	LWLockInitialize(&WaitEventTimingCtl->lock,
					 LWTRANCHE_WAIT_EVENT_TIMING_DSA);
	WaitEventTimingCtl->timing_dsa_handle = DSA_HANDLE_INVALID;
	WaitEventTimingCtl->timing_array = InvalidDsaPointer;
	WaitEventTimingArray = NULL;
}

const ShmemCallbacks WaitEventTimingShmemCallbacks = {
	.request_fn = WaitEventTimingShmemRequest,
	.init_fn = WaitEventTimingShmemInit,
};

/*
 * before_shmem_exit callback: disable the inline hot path for the rest of
 * proc_exit, so it does not dereference my_wait_event_timing or attempt a
 * fresh lazy-attach after dsm_backend_shutdown has unmapped the DSA segment
 * behind the slot.  We do NOT null my_wait_event_timing here: a NULL pointer
 * would route the hot path through the lazy-attach branch, which then
 * re-attaches using DSA primitives that operate on already-detached memory.
 */
static void
pgstat_wait_event_timing_before_shmem_exit(int code, Datum arg)
{
	wait_event_timing_writes_disabled = true;
}

/*
 * Point my_wait_event_timing at this backend's slot, allocating the DSA
 * array on first call.  Reached from the hot path the first time this
 * backend observes wait_event_capture != off; after the first successful
 * attach the cached pointer stays valid for the backend's lifetime, so this
 * is a cold branch.
 */
static void
pgstat_wait_event_timing_lazy_attach(void)
{
	int			procNumber;
	WaitEventTimingState *slot;

	if (my_wait_event_timing != NULL)
		return;

	if (MyProc == NULL)
		return;

	/*
	 * Skip during backend startup.  InitPostgres runs in InitProcessing mode
	 * (PostgresMain switches to NormalProcessing only after it returns), and a
	 * wait event can fire from InitPostgres's own transaction cleanup --
	 * CommitTransactionCommand -> LockReleaseAll -> LWLockAcquire -- while the
	 * current resource owner is already mid-release.  The attach path's
	 * dsm_create_descriptor() would then call ResourceOwnerEnlarge() after
	 * release has started and FATAL.  Skipping here keeps the backend alive;
	 * the first wait once it reaches normal processing attaches successfully.
	 */
	if (!IsNormalProcessingMode())
		return;

	/*
	 * Lazy attach allocates memory (dsa_attach -> dsm_attach ->
	 * MemoryContextAlloc), which Assert-fails inside a critical section.  A
	 * backend's very first wait event after capture is enabled can land in
	 * one (e.g. a parallel worker in XLogInsert).  Skipping silently drops
	 * that in-flight event but keeps the backend alive; the next wait event
	 * outside any critical section attaches successfully.
	 */
	if (CritSectionCount > 0)
		return;

	/*
	 * Skip if MyProc is already on an LWLock wait queue: we run inside
	 * LWLockAcquire after LWLockQueueSelf set MyProc->lwWaiting, and our
	 * attach path's own LWLockAcquire would hit the "queueing while waiting
	 * on another lock" PANIC.  The next wait outside an LWLock-wait context
	 * retries successfully.
	 */
	if (MyProc->lwWaiting != LW_WS_NOT_WAITING)
		return;

	procNumber = GetNumberFromPGProc(MyProc);
	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
		return;

	if (!wait_event_timing_attach_array(true))
		return;

	slot = wet_slot(procNumber);

	/*
	 * Clear the slot before publishing it: the DSA region is zeroed at
	 * creation, but a later backend may inherit a slot from an exited one.
	 * Zero through the local `slot` first, THEN publish to
	 * my_wait_event_timing, so a non-NULL pointer always means the slot is
	 * ready for the very next store.
	 */
	memset(slot->events, 0, sizeof(slot->events));
	lwlock_timing_hash_clear(slot);
	slot->reset_count = 0;
	slot->lwlock_overflow_count = 0;
	slot->flat_overflow_count = 0;
	slot->current_event = 0;
	INSTR_TIME_SET_ZERO(slot->wait_start);

	my_last_reset_generation = pg_atomic_read_u32(&slot->reset_generation);

	/* Publish only after the slot is fully initialised. */
	my_wait_event_timing = slot;

	/*
	 * Register a before_shmem_exit callback (once) to raise the
	 * writes-disabled gate before the DSA mappings go away.  shmem_exit()
	 * runs all before_shmem_exit callbacks (which sets the gate) and only
	 * then calls dsm_backend_shutdown(), so the gate is guaranteed up before
	 * any DSA segment is unmapped -- independent of callback ordering.
	 */
	{
		static bool registered = false;

		if (!registered)
		{
			before_shmem_exit(pgstat_wait_event_timing_before_shmem_exit,
							  (Datum) 0);
			registered = true;
		}
	}
}

/*
 * Called from InitProcess()/InitAuxiliaryProcess().  In the lazy-DSA design
 * there is no slot to point at yet -- the slot attaches on the first wait
 * event under capture != off (pgstat_wait_event_timing_lazy_attach) -- so a
 * backend that never enables capture pays zero per-backend memory.  Just
 * make sure the pointer starts NULL.
 */
void
pgstat_set_wait_event_timing_storage(int procNumber)
{
	my_wait_event_timing = NULL;
	my_wait_event_trace = NULL;

	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
	{
		my_trace_proc_number = -1;
		return;
	}

	my_trace_proc_number = procNumber;

	/*
	 * If the previous occupant of this procNumber left an ORPHANED trace ring
	 * behind (we do not free trace rings at backend exit -- see
	 * WaitEventTraceControl), free it now so this backend starts with a clean
	 * FREE slot.
	 */
	wait_event_trace_clear_orphan_at_init(procNumber);
}

/*
 * Detach from the timing slot on backend exit.  The slots stay in DSA;
 * clearing the local pointers keeps the late-shutdown hot path from touching
 * already-detached memory.
 */
void
pgstat_reset_wait_event_timing_storage(void)
{
	if (my_trace_proc_number >= 0)
		wait_event_trace_detach(my_trace_proc_number);

	my_wait_event_timing = NULL;
	my_wait_event_trace = NULL;
	my_trace_proc_number = -1;
}

/*
 * GUC check hook for wait_event_capture (timing build).  All enum values
 * are accepted; there is nothing to validate beyond the enum mapping.
 */
bool
check_wait_event_capture(int *newval, void **extra, GucSource source)
{
	return true;
}

/*
 * GUC assign hook for wait_event_capture (timing build).
 *
 * Drop any in-flight wait state: after the capture level changes, the
 * existing wait_start / current_event can no longer be trusted (a wait
 * that started under one level and ends under another would be miscredited
 * or use a stale start time).  Forfeiting at most one in-flight sample per
 * GUC change is negligible and eliminates all such miscredits.
 */
void
assign_wait_event_capture(int newval, void *extra)
{
	if (my_wait_event_timing != NULL)
	{
		INSTR_TIME_SET_ZERO(my_wait_event_timing->wait_start);
		my_wait_event_timing->current_event = 0;
	}

	/*
	 * Stepping down from TRACE: release the ~4 MB DSA ring now rather than
	 * holding it pinned for the rest of the session.  Only fires when a ring
	 * is actually attached, so OFF -> TRACE -> OFF without ever emitting a
	 * trace record stays a no-op.  Re-enable re-allocates a fresh ring on the
	 * first wait event via wait_event_trace_attach.  dsa_free is non-raising
	 * LWLock bookkeeping, so it is safe from an assign hook.
	 */
	if (newval != WAIT_EVENT_CAPTURE_TRACE && my_wait_event_trace != NULL)
		wait_event_trace_release_slot(my_trace_proc_number);
}

/* ================= Trace-level ring machinery ================= */


/*
 * Write a trace ring marker record.  Shared helper for all marker types.
 */
static void
wait_event_trace_write_marker(uint8 record_type, int64 query_id)
{
	uint64		pos;
	WaitEventTraceRecord *rec;
	uint32		seq;
	instr_time	now;

	/*
	 * Single capture-level gate: markers only land in the ring when
	 * wait_event_capture is at TRACE.  This guarantees consistency with the
	 * wait-event hot path (also gated on the same level) -- there is no
	 * configuration in which one half of the trace fires and the other
	 * doesn't.  query_id == 0 means "no query ID available" (utility command
	 * or compute_query_id = off), which we skip.
	 *
	 * wait_event_trace_writes_disabled is the same per-backend gate the
	 * wait-event hot path uses; it is raised by release_slot and
	 * before_shmem_exit around slot-state transitions to keep both writers
	 * consistent.  Markers cannot fire during those transitions today
	 * (single-threaded execution, no nested executor), but checking here
	 * keeps the contract uniform across all trace-ring writers and is robust
	 * to future code paths that might invoke a marker from a nested context.
	 *
	 * No likely()/unlikely() annotation: this function is called at
	 * query/exec boundaries (a handful per query, not per wait event), so
	 * neither side of the branch dominates often enough for static layout to
	 * matter, and the meaningful production configuration (wait_event_capture
	 * = trace) is exactly when the body is hot -- an annotation on the
	 * early-return would point the wrong way.
	 */
	if (wait_event_capture != WAIT_EVENT_CAPTURE_TRACE ||
		wait_event_trace_writes_disabled ||
		query_id == 0)
		return;

	/*
	 * Lazy attach on first use.  Allocation lives here (not in the assign
	 * hook) because dsa_allocate_extended() can ereport(ERROR) on OOM, which
	 * is forbidden in assign-hook context but legitimate here.  Idempotent:
	 * wait_event_trace_attach() short-circuits on subsequent calls.
	 */
	if (my_wait_event_trace == NULL)
	{
		if (my_trace_proc_number < 0)
			return;
		wait_event_trace_attach(my_trace_proc_number);
		if (my_wait_event_trace == NULL)
			return;				/* attach path unable to allocate */
	}

	/*
	 * Claim the next slot.  Single-writer counter (only the owning backend
	 * writes its own ring), so a plain read+write is sufficient and avoids
	 * the LOCK XADD that pg_atomic_fetch_add_u64 would emit -- a wasted
	 * cache-coherence trip on an unshared cache line at this rate (one per
	 * wait event).  Cross-backend readers use pg_atomic_read_u64, which
	 * compiles to a plain MOV on x86 and tolerates concurrent writes here
	 * (their actual safety against the records[] window is the per-record
	 * seqlock below).  Same idiom as injection_point.c's per-entry generation
	 * counter (single writer + multiple lock-free readers).
	 */
	pos = pg_atomic_read_u64(&my_wait_event_trace->write_pos);
	pg_atomic_write_u64(&my_wait_event_trace->write_pos, pos + 1);
	rec = &my_wait_event_trace->records[pos & my_wait_event_trace->ring_mask];
	seq = (uint32) (pos * 2 + 1);

	rec->seq = seq;
	pg_write_barrier();			/* release: payload stores must not rise above
								 * seq=odd */

	INSTR_TIME_SET_CURRENT(now);
	rec->record_type = record_type;
	rec->timestamp_ns = INSTR_TIME_GET_NANOSEC(now);
	rec->data.query.query_id = query_id;
	rec->data.query.pad2 = 0;

	pg_write_barrier();			/* release: payload stores must land before
								 * seq=even */
	rec->seq = seq + 1;
}

void
wait_event_trace_query_start(int64 query_id)
{
	wait_event_trace_write_marker(TRACE_QUERY_START, query_id);
}

void
wait_event_trace_query_end(int64 query_id)
{
	wait_event_trace_write_marker(TRACE_QUERY_END, query_id);
}

void
wait_event_trace_exec_start(int64 query_id)
{
	wait_event_trace_write_marker(TRACE_EXEC_START, query_id);
}

void
wait_event_trace_exec_end(int64 query_id)
{
	wait_event_trace_write_marker(TRACE_EXEC_END, query_id);
}

/*
 * Report the shared memory space needed for trace ring buffer control.
 * Only a small control struct is in fixed shmem; the actual ring buffers
 * are allocated lazily via DSA.  At ~24 bytes/slot, the slot array adds
 * ~26 KB at a default MaxBackends, negligible compared to the ring
 * memory itself.
 */
static Size
WaitEventTraceControlShmemSize(void)
{
	return add_size(offsetof(WaitEventTraceControl, trace_slots),
					mul_size(NUM_WAIT_EVENT_TIMING_SLOTS,
							 sizeof(WaitEventTraceSlot)));
}

static void
WaitEventTraceControlShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "WaitEventTraceControl",
					   .size = WaitEventTraceControlShmemSize(),
					   .ptr = (void **) &WaitEventTraceCtl);
}

/*
 * Initialize shared memory for trace ring buffer control.
 */
static void
WaitEventTraceControlShmemInit(void *arg)
{
	int			i;

	WaitEventTraceCtl->trace_dsa_handle = DSA_HANDLE_INVALID;
	LWLockInitialize(&WaitEventTraceCtl->lock,
					 LWTRANCHE_WAIT_EVENT_TRACE_DSA);
	for (i = 0; i < NUM_WAIT_EVENT_TIMING_SLOTS; i++)
	{
		WaitEventTraceSlot *s = &WaitEventTraceCtl->trace_slots[i];

		pg_atomic_init_u64(&s->generation, 0);
		pg_atomic_init_u32(&s->state, WAIT_EVENT_TRACE_SLOT_FREE);
		s->pad = 0;
		s->ring_ptr = InvalidDsaPointer;
	}
}

const ShmemCallbacks WaitEventTraceControlShmemCallbacks = {
	.request_fn = WaitEventTraceControlShmemRequest,
	.init_fn = WaitEventTraceControlShmemInit,
};

/*
 * Ensure the shared DSA for trace ring buffers exists and is attached.
 * Creates it on first call (any backend), attaches on subsequent calls.
 * Must be called from a backend context (not postmaster).
 */
static void
wait_event_trace_ensure_dsa(void)
{
	MemoryContext oldcontext;

	if (trace_dsa != NULL)
		return;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	LWLockAcquire(&WaitEventTraceCtl->lock, LW_EXCLUSIVE);

	if (WaitEventTraceCtl->trace_dsa_handle == DSA_HANDLE_INVALID)
	{
		trace_dsa = dsa_create(LWTRANCHE_WAIT_EVENT_TRACE_DSA);
		dsa_pin(trace_dsa);
		dsa_pin_mapping(trace_dsa);
		WaitEventTraceCtl->trace_dsa_handle = dsa_get_handle(trace_dsa);
	}
	else
	{
		trace_dsa = dsa_attach(WaitEventTraceCtl->trace_dsa_handle);
		dsa_pin_mapping(trace_dsa);
	}

	LWLockRelease(&WaitEventTraceCtl->lock);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Transition our trace ring slot to ORPHANED on backend exit.
 *
 * Registered as a before_shmem_exit callback.  Runs BEFORE
 * dsm_backend_shutdown() detaches the DSA.
 *
 * Crucially, we do NOT free the ring here.  The ring stays allocated in
 * DSA so that cross-backend consumers -- the in-tree
 * pg_get_wait_event_trace SRF and any extension following the
 * snapshot pattern documented on WaitEventTraceControl -- can read
 * the dying backend's final waits.  The original "free at exit"
 * design lost data the instant a worker terminated, which was
 * particularly bad for parallel workers exiting in milliseconds at
 * end-of-parallel-query.  See the lifecycle comment on
 * WaitEventTraceControl for the full design
 * rationale and the bounded-memory cost we accept in exchange.
 *
 * The ORPHANED slot is reclaimed in one of two ways:
 *   (a) a new backend at this procNumber calls
 *       wait_event_trace_clear_orphan_at_init() at backend init, or
 *   (b) the DBA calls pg_stat_clear_orphaned_wait_event_rings().
 *
 * State transition order matters: bump generation BEFORE storing the
 * new state, so cross-backend readers that snapshot
 * (generation_before, state, ring_ptr, generation_after) under the
 * lock see a consistent (state, ring_ptr) pair iff generation didn't
 * change.  We hold the lock for the whole transition, but readers do
 * not have to (they just take it briefly to snapshot the ring
 * contents); the generation check is what makes the unlocked-read
 * path safe.
 */
static void
wait_event_trace_before_shmem_exit(int code, Datum arg)
{
	int			procNumber = DatumGetInt32(arg);
	WaitEventTraceSlot *slot;

	if (WaitEventTraceCtl == NULL)
		return;

	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
		return;

	slot = &WaitEventTraceCtl->trace_slots[procNumber];

	/*
	 * If this backend never ended up with an OWNED slot (e.g. capture was off
	 * the whole session, or the trace was released back to FREE via
	 * assign_wait_event_capture going trace -> off), there is nothing to
	 * transition.  Read state without the lock first as a fast-path check;
	 * the authoritative re-check happens under the lock below.
	 */
	if (pg_atomic_read_u32(&slot->state) != WAIT_EVENT_TRACE_SLOT_OWNED)
	{
		wait_event_trace_writes_disabled = true;
		my_wait_event_trace = NULL;
		return;
	}

	/*
	 * Disable trace-ring writes on this backend before we touch the lock.
	 * Writes after this point would race with the OWNED -> ORPHANED state
	 * publish below: a wait event whose end-timing path runs after the state
	 * has been published as ORPHANED would write into a ring that the patch
	 * contract declares read-only post-mortem.  Cross-backend readers
	 * snapshot ORPHANED rings without expecting concurrent writes from the
	 * dying owner.  See wait_event_trace_writes_disabled for the full UAF /
	 * contract-violation analysis.
	 *
	 * The flag stays true for the remainder of this backend's life (we are in
	 * proc_exit; there is no subsequent capture re-enable to handle), so we
	 * do not reset it.
	 */
	wait_event_trace_writes_disabled = true;

	LWLockAcquire(&WaitEventTraceCtl->lock, LW_EXCLUSIVE);

	/*
	 * Drop the local pointer inside the lock-held region as a second line of
	 * defense; the writes-disabled flag above is the primary gate.
	 */
	my_wait_event_trace = NULL;

	if (pg_atomic_read_u32(&slot->state) == WAIT_EVENT_TRACE_SLOT_OWNED &&
		DsaPointerIsValid(slot->ring_ptr))
	{
		/*
		 * Bump generation first so any reader that snapped the old generation
		 * will detect the change on its post-read recheck and discard its
		 * read.  Then publish the ORPHANED state. Keep ring_ptr valid -- the
		 * data is what we want to preserve.
		 */
		pg_atomic_fetch_add_u64(&slot->generation, 1);
		pg_atomic_write_u32(&slot->state, WAIT_EVENT_TRACE_SLOT_ORPHANED);
	}

	LWLockRelease(&WaitEventTraceCtl->lock);
}

/*
 * Allocate (or re-acquire) a trace ring buffer for this backend via DSA.
 * Called when wait_event_capture is set to 'trace'.
 *
 * Slot state at entry will be one of:
 *
 *   FREE     fresh slot (or one cleared on this backend's init by
 *            wait_event_trace_clear_orphan_at_init): allocate a new
 *            ring, transition slot to OWNED, bump generation.
 *
 *   OWNED    we already attached earlier in this same backend's life
 *            (e.g. user toggled capture trace->stats->trace; the
 *            stats step calls wait_event_trace_release_slot which
 *            transitions back to FREE, but our cached
 *            my_wait_event_trace was cleared on the way down -- so
 *            seeing OWNED here at attach time means a different
 *            backend somehow ended up with this procNumber, which
 *            cannot happen because procNumber is per-backend and a
 *            single backend can only run one attach at a time.  We
 *            still tolerate this state defensively by re-mapping the
 *            existing ring rather than leaking a second allocation.
 *
 *   ORPHANED can never be observed here: a new backend's
 *            pgstat_set_wait_event_timing_storage() called
 *            wait_event_trace_clear_orphan_at_init() before any
 *            wait-event capture path can run, so any prior orphan has
 *            already been demoted to FREE.  Treated as a safety check
 *            (Assert in debug builds).
 */
void
wait_event_trace_attach(int procNumber)
{
	/*
	 * Re-entrancy guard.  dsa_create / dsa_allocate_extended below can emit
	 * wait events internally; those reach the lazy-attach hot path which
	 * calls back into this function while we still hold
	 * WaitEventTraceCtl->lock or are mid-allocation.  See the
	 * function-local-static-bool pattern explainer on
	 * wait_event_timing_attach_array.
	 */
	static bool in_attach = false;
	static bool shmem_exit_registered = false;
	WaitEventTraceSlot *slot;
	dsa_pointer p;
	WaitEventTraceState *ts;
	uint32		state_now;

	if (in_attach)
		return;

	if (WaitEventTraceCtl == NULL)
		return;

	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
		return;

	/*
	 * Skip the attach if we are inside a critical section.  Below this point
	 * we call dsa_create / dsa_attach / dsa_allocate_extended, all of which
	 * can allocate memory via MemoryContextAlloc and Assert-fail on
	 * "CritSectionCount == 0 || allowInCritSection". The very-first wait
	 * event after wait_event_capture = trace can land inside a critical
	 * section (e.g. a parallel worker scanning a heap page hits
	 * BufferSetHintBits16 -> XLogSaveBufferForHint -> XLogInsert ->
	 * LWLockAcquire, with the XLogInsert critical section open).
	 *
	 * Skipping here silently drops the in-flight wait event (it is not
	 * traced) but keeps the backend alive.  The next wait event outside any
	 * critical section will hit this function again and attach successfully.
	 * See the matching guard in pgstat_wait_event_timing_lazy_attach.
	 */
	if (CritSectionCount > 0)
		return;

	/*
	 * Skip the attach if MyProc is already on an LWLock wait queue.  We are
	 * called from the wait-event hot path which fires AFTER LWLockQueueSelf
	 * has set MyProc->lwWaiting; a nested LWLockAcquire on our internal lock
	 * (via wait_event_trace_ensure_dsa) would PANIC at lwlock.c:1029. See the
	 * matching guard in pgstat_wait_event_timing_lazy_ attach for the full
	 * rationale.
	 */
	if (MyProc != NULL && MyProc->lwWaiting != LW_WS_NOT_WAITING)
		return;

	slot = &WaitEventTraceCtl->trace_slots[procNumber];

	in_attach = true;
	PG_TRY();
	{
		state_now = pg_atomic_read_u32(&slot->state);

		/*
		 * ORPHANED is normally impossible at attach time --
		 * pgstat_set_wait_event_timing_storage() at backend init calls
		 * wait_event_trace_clear_orphan_at_init() which demotes any inherited
		 * orphan to FREE.  But there is one case where this backend can
		 * legitimately observe its own slot in the ORPHANED state: after we
		 * have already run wait_event_trace_before_shmem_exit()
		 * (transitioning the slot to ORPHANED on exit), a later
		 * before_shmem_exit callback (e.g. pgstat_io_flush_cb during
		 * proc_exit shutdown) can contend on an LWLock that emits a wait
		 * event, which calls pgstat_report_wait_end_timing() ->
		 * wait_event_trace_attach() after my_wait_event_trace has been
		 * cleared.  We must not re-attach in that case: we are dying, the
		 * ring is now post-mortem data for cross-backend readers, and the
		 * writer invariant must hold.  Skip the trace for any wait events
		 * emitted after our own exit transition.
		 */
		if (state_now == WAIT_EVENT_TRACE_SLOT_ORPHANED)
		{
			/* PG_FINALLY below clears in_attach. */
		}
		else if (state_now == WAIT_EVENT_TRACE_SLOT_OWNED &&
				 DsaPointerIsValid(slot->ring_ptr))
		{
			/* Already have a ring buffer; re-map to it. */
			wait_event_trace_ensure_dsa();
			my_wait_event_trace = dsa_get_address(trace_dsa, slot->ring_ptr);
			my_trace_proc_number = procNumber;
		}
		else
		{
			Size		alloc_size;

			wait_event_trace_ensure_dsa();

			/*
			 * Cache the cluster-wide ring size on first allocation in this
			 * backend.  wait_event_trace_ring_size is PGC_POSTMASTER, so by
			 * the time any backend reaches here, its boot value has been
			 * committed by the GUC framework.  All rings in the postmaster
			 * run share the same dimensions.
			 */
			if (WaitEventTraceRingSize == 0)
				WaitEventTraceRingSize =
					(uint32) wait_event_trace_ring_size * 1024U /
					(uint32) sizeof(WaitEventTraceRecord);

			alloc_size = offsetof(WaitEventTraceState, records) +
				(Size) WaitEventTraceRingSize * sizeof(WaitEventTraceRecord);

			p = dsa_allocate_extended(trace_dsa, alloc_size, DSA_ALLOC_ZERO);
			ts = dsa_get_address(trace_dsa, p);
			pg_atomic_init_u64(&ts->write_pos, 0);
			ts->ring_mask = WaitEventTraceRingSize - 1;

			LWLockAcquire(&WaitEventTraceCtl->lock, LW_EXCLUSIVE);

			/*
			 * Publish ring_ptr BEFORE transitioning state to OWNED.
			 * Cross-backend readers that observe state==OWNED outside the
			 * lock then see a valid ring_ptr.  Bump generation last so any
			 * reader that snapped the prior generation will detect the
			 * change.
			 */
			slot->ring_ptr = p;
			pg_atomic_write_u32(&slot->state, WAIT_EVENT_TRACE_SLOT_OWNED);
			pg_atomic_fetch_add_u64(&slot->generation, 1);
			LWLockRelease(&WaitEventTraceCtl->lock);

			my_wait_event_trace = ts;
			my_trace_proc_number = procNumber;

			/*
			 * Register cleanup to run BEFORE dsm_backend_shutdown() detaches
			 * the DSA.  The before_shmem_exit callbacks run in LIFO order
			 * before DSM detach, so the ORPHANED transition (which does not
			 * actually free the ring) is safe at that point.
			 *
			 * Guarded by shmem_exit_registered because under the
			 * release-on-disable policy (see wait_event_trace_release_slot
			 * and assign_wait_event_capture) the allocate branch can run
			 * multiple times per backend lifetime -- once per off/stats ->
			 * trace re-enable cycle.  The cleanup itself is idempotent (it
			 * short-circuits when state is not OWNED), so it is safe to
			 * invoke after a release-then-reattach cycle, but we still avoid
			 * growing the before_shmem_exit list.
			 */
			if (!shmem_exit_registered)
			{
				before_shmem_exit(wait_event_trace_before_shmem_exit,
								  Int32GetDatum(procNumber));
				shmem_exit_registered = true;
			}
		}
	}
	PG_FINALLY();
	{
		in_attach = false;
	}
	PG_END_TRY();
}

/*
 * Free trace ring buffer for this backend on exit.
 */
static void
wait_event_trace_detach(int procNumber)
{
	/*
	 * Only clear local pointers here.  The actual DSA free happens in
	 * wait_event_trace_before_shmem_exit(), which runs before
	 * dsm_backend_shutdown() detaches the DSA segments.
	 */
	my_wait_event_trace = NULL;
	my_trace_proc_number = -1;
}

/*
 * Release this backend's trace ring buffer back to DSA immediately.
 *
 * Called from assign_wait_event_capture when the user steps down from
 * TRACE to STATS or OFF.  Without this, a ~4 MB ring allocated by a
 * brief investigation would remain pinned for the rest of the session's
 * lifetime, which can leak gigabytes across large connection pools.
 *
 * Important contrast with wait_event_trace_before_shmem_exit: backend
 * exit transitions the slot to ORPHANED (preserving data for
 * cross-backend readers); release_slot fully frees and returns to FREE
 * because the operator has explicitly disabled trace -- they have
 * affirmatively decided not to keep the data, so we honour that and
 * reclaim the memory immediately.  Subsequent re-enable allocates a
 * fresh ring via wait_event_trace_attach's allocate branch.
 *
 * The operation is LWLock-safe and does not raise -- dsa_free is pure
 * bookkeeping on the DSA freelist, no allocation and no ereport paths.
 * Safe to call from a GUC assign hook.
 *
 * If pg_get_backend_wait_event_trace is currently iterating our own ring
 * (wait_event_trace_srf_in_progress), we must NOT free the chunk out
 * from under it: that would be a use-after-free on the records[] the SRF
 * is still reading.  Set wait_event_trace_release_pending instead and
 * return; the SRF's PG_FINALLY block will perform the deferred free
 * after iteration completes.  In practice this branch is unreachable in
 * current PG (assign hooks fire only at command boundaries and the SRF
 * is a single command), but it makes the invariant explicit and the
 * future-proofing free.
 */
static void
wait_event_trace_release_slot(int procNumber)
{
	/*
	 * Re-entrancy guard.  dsa_free takes a DSA-internal LWLock which can in
	 * principle emit a wait event; if a nested assign hook re-enters we must
	 * not recurse.  See the function-local-static-bool pattern explainer on
	 * wait_event_timing_attach_array.
	 */
	static bool in_release = false;
	WaitEventTraceSlot *slot;

	if (in_release)
		return;

	if (WaitEventTraceCtl == NULL || trace_dsa == NULL)
		return;

	/*
	 * Same-backend SRF is iterating our own ring.  Defer the free until the
	 * SRF's PG_FINALLY runs.
	 */
	if (wait_event_trace_srf_in_progress)
	{
		wait_event_trace_release_pending = true;
		return;
	}

	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
		return;

	slot = &WaitEventTraceCtl->trace_slots[procNumber];

	in_release = true;

	/*
	 * Disable trace-ring writes on this backend before we touch the lock or
	 * call dsa_free.  An internal LWLock inside dsa_free can dispatch a wait
	 * event whose end-timing path would otherwise see capture_level == TRACE
	 * (the GUC assign hook is in flight; the variable has not been committed
	 * by the framework yet) and write into the very chunk we are returning to
	 * the DSA freelist.  See the comment on wait_event_trace_writes_disabled
	 * for the full UAF analysis.
	 */
	wait_event_trace_writes_disabled = true;

	PG_TRY();
	{
		LWLockAcquire(&WaitEventTraceCtl->lock, LW_EXCLUSIVE);

		/*
		 * Drop the local pointer BEFORE the dsa_free as a second line of
		 * defense (the writes-disabled flag above is the primary gate).  Any
		 * wait event whose hot path slips past the gate check via a compiler
		 * or memory-ordering surprise would at least see my_wait_event_trace
		 * == NULL and skip the write.
		 */
		my_wait_event_trace = NULL;

		if (DsaPointerIsValid(slot->ring_ptr))
		{
			/*
			 * Bump generation first to invalidate any concurrent
			 * cross-backend snapshot, then free, then publish the FREE state
			 * with a NULL ring_ptr.  Order matters for unlocked readers that
			 * have already passed the state check.
			 */
			pg_atomic_fetch_add_u64(&slot->generation, 1);
			dsa_free(trace_dsa, slot->ring_ptr);
			slot->ring_ptr = InvalidDsaPointer;
			pg_atomic_write_u32(&slot->state, WAIT_EVENT_TRACE_SLOT_FREE);
		}
		LWLockRelease(&WaitEventTraceCtl->lock);
	}
	PG_FINALLY();
	{
		wait_event_trace_writes_disabled = false;
		in_release = false;
	}
	PG_END_TRY();
}

/*
 * Clear an orphaned trace ring at backend init time.
 *
 * Called from pgstat_set_wait_event_timing_storage() once the new
 * backend has its procNumber.  If the slot we're inheriting was left
 * ORPHANED by a previous backend (because we deliberately do not free
 * trace rings on backend exit -- see the lifecycle discussion on
 * WaitEventTraceControl), free the ring now so the new backend starts
 * with a clean FREE slot.  Subsequent wait_event_trace_attach() calls
 * (when this backend itself enables trace) will then take the
 * allocate branch.
 *
 * No-op when the slot is already FREE or OWNED: FREE means there's
 * nothing to clear; OWNED is impossible at backend init (only a
 * not-yet-exited backend can leave a slot OWNED, and procNumbers are
 * assigned exclusively).  We assert OWNED is not observed in debug
 * builds and conservatively skip the free in production.
 *
 * Robustness: this runs during InitProcess() (before the backend can
 * accept any work), and the work it performs -- dsa_attach() and
 * dsa_free() -- can raise ERROR on rare runtime failures (corrupted
 * DSA segment headers, descriptor exhaustion, mmap ENOMEM, etc.).
 * An uncaught ERROR here would propagate out of InitProcess() and
 * abort backend startup entirely, even for sessions that never
 * intended to use wait_event_capture.  To prevent the trace
 * feature's housekeeping from gating connection establishment, the
 * body is wrapped in PG_TRY()/PG_CATCH(): any error from dsa_attach
 * or dsa_free is captured, downgraded to a WARNING with a hint
 * pointing at the admin sweep function, and execution continues.
 * The orphan stays in place; it can be reclaimed by the next
 * backend that inherits the same procNumber (if the underlying
 * problem was transient), by pg_stat_clear_orphaned_wait_event_rings(),
 * or at next cluster restart.
 */
static void
wait_event_trace_clear_orphan_at_init(int procNumber)
{
	WaitEventTraceSlot *slot;
	uint32		state_now;
	MemoryContext caller_cxt;

	if (WaitEventTraceCtl == NULL)
		return;

	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
		return;

	slot = &WaitEventTraceCtl->trace_slots[procNumber];

	state_now = pg_atomic_read_u32(&slot->state);
	if (state_now != WAIT_EVENT_TRACE_SLOT_ORPHANED)
	{
		Assert(state_now != WAIT_EVENT_TRACE_SLOT_OWNED);
		return;
	}

	/*
	 * Save CurrentMemoryContext so the PG_CATCH path can copy the error data
	 * into a context that survives FlushErrorState(). FlushErrorState() calls
	 * MemoryContextReset(ErrorContext), so CopyErrorData() must run in a
	 * different context or the returned ErrorData becomes a dangling pointer.
	 */
	caller_cxt = CurrentMemoryContext;

	PG_TRY();
	{
		/*
		 * The trace DSA is shared across the cluster.  We must attach to it
		 * before calling dsa_free (which needs the dsa_area pointer).  The
		 * DSA was created by some earlier backend that wrote a trace record
		 * (otherwise the slot couldn't have ended up ORPHANED), so the handle
		 * in WaitEventTraceCtl is valid; ensure_dsa() will attach.  Both
		 * ensure_dsa() and dsa_free() can raise ERROR; the PG_CATCH below
		 * downgrades any such error to a WARNING so backend startup is not
		 * blocked.
		 */
		wait_event_trace_ensure_dsa();

		LWLockAcquire(&WaitEventTraceCtl->lock, LW_EXCLUSIVE);
		if (pg_atomic_read_u32(&slot->state) == WAIT_EVENT_TRACE_SLOT_ORPHANED &&
			DsaPointerIsValid(slot->ring_ptr))
		{
			pg_atomic_fetch_add_u64(&slot->generation, 1);
			dsa_free(trace_dsa, slot->ring_ptr);
			slot->ring_ptr = InvalidDsaPointer;
			pg_atomic_write_u32(&slot->state, WAIT_EVENT_TRACE_SLOT_FREE);
		}
		LWLockRelease(&WaitEventTraceCtl->lock);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/*
		 * Release any LWLocks we (or anything we called) might still hold.
		 * Two paths can leave WaitEventTraceCtl->lock held when control
		 * reaches here:
		 *
		 * 1. The outer LWLockAcquire above succeeded and dsa_free raised
		 * before we reached LWLockRelease. 2. wait_event_trace_ensure_dsa()
		 * raised inside its own LWLockAcquire/dsa_attach/LWLockRelease
		 * region.
		 *
		 * We are running during InitProcess(), BEFORE any transaction or
		 * PostgresMain sigsetjmp has been set up, so PG's standard
		 * "AbortTransaction -> LWLockReleaseAll" cleanup does NOT fire on the
		 * longjmp into PG_CATCH. Without an explicit release here the lock
		 * would stay held for the lifetime of this backend, blocking every
		 * future LW_EXCLUSIVE acquirer (the orphan-clear sweep, release_slot,
		 * before_shmem_exit transitions, and subsequent backends'
		 * clear_orphan_at_init).  That would be strictly worse than the
		 * original failure-startup behavior this commit set out to fix.
		 *
		 * LWLockReleaseAll() is the idiomatic catch-path lock cleanup used by
		 * the standalone aux-process error handlers (walwriter.c,
		 * checkpointer.c, pgarch.c).  It is safe to call broadly here because
		 * pgstat_set_wait_ event_timing_storage runs at a fixed point in
		 * InitProcess where the caller frame holds no other LWLocks across
		 * our return: the earlier InitProcess steps that touch LWLocks
		 * (ProcArrayAdd, etc.) release them before returning, and the
		 * subsequent steps that acquire LWLocks have not yet run.
		 */
		LWLockReleaseAll();

		/*
		 * Switch BACK to the caller's context before CopyErrorData so that
		 * edata is allocated in a context that survives FlushErrorState().
		 * FlushErrorState() calls MemoryContextReset(ErrorContext);
		 * allocating edata in ErrorContext (the default at PG_CATCH entry on
		 * the error path) would make it a dangling pointer the moment we
		 * flush.  See the matching pattern in spi.c PG_CATCH branches.
		 */
		MemoryContextSwitchTo(caller_cxt);
		edata = CopyErrorData();
		FlushErrorState();

		ereport(WARNING,
				(errcode(edata->sqlerrcode),
				 errmsg("could not clear orphaned wait-event trace ring "
						"at backend init: %s", edata->message),
				 errdetail("Backend startup proceeds with the orphan "
						   "still allocated for procnumber %d.",
						   procNumber),
				 errhint("Run pg_stat_clear_orphaned_wait_event_rings() "
						 "to release the orphan when the underlying "
						 "condition is resolved.")));

		FreeErrorData(edata);
	}
	PG_END_TRY();
}

/*
 * Out-of-line body for pgstat_report_wait_start()'s timing path.  Records
 * the start timestamp and the event being waited on.  Reached only when
 * wait_event_capture != OFF.
 */
void
pgstat_report_wait_start_timing(uint32 wait_event_info)
{
	/*
	 * Stay out of the timing path once proc_exit has begun tearing down DSA
	 * mappings (see the before_shmem_exit callback).
	 */
	if (wait_event_timing_writes_disabled)
		return;

	if (my_wait_event_timing == NULL)
	{
		pgstat_wait_event_timing_lazy_attach();

		/*
		 * lazy_attach can dispatch nested wait events while it sets up DSA
		 * (dsa_attach takes an internal LWLock); those nested wait_end calls
		 * clear my_wait_event_info to 0.  Re-publish so the outer wait stays
		 * visible in pg_stat_activity.  Only needed on the first-attach path.
		 */
		*(volatile uint32 *) my_wait_event_info = wait_event_info;
	}

	if (my_wait_event_timing != NULL)
	{
		INSTR_TIME_SET_CURRENT(my_wait_event_timing->wait_start);
		my_wait_event_timing->current_event = wait_event_info;
	}
}

/*
 * Out-of-line body for pgstat_report_wait_end()'s timing path.  Computes
 * the wait duration, accumulates per-event statistics, and at the trace
 * level pushes the completed wait into the per-session ring.
 *
 * capture_level is the value of wait_event_capture observed at the inline
 * gate.  Passing it through (rather than re-loading the global here) avoids
 * a redundant load on the trace branch below -- the function-call boundary
 * defeats CSE -- and means a concurrent GUC change cannot half-apply to
 * this call: we run entirely at the gate's view of the level.
 */
void
pgstat_report_wait_end_timing(int capture_level)
{
	uint32		event;
	uint32		cur_reset_gen;

	if (wait_event_timing_writes_disabled)
		return;

	if (my_wait_event_timing == NULL)
	{
		pgstat_wait_event_timing_lazy_attach();
		if (my_wait_event_timing == NULL)
			return;
	}

	event = my_wait_event_timing->current_event;

	/*
	 * Service a pending cross-backend reset request.  A single relaxed atomic
	 * load; when the shared generation has advanced past the value we last
	 * acted on, clear our own counters on behalf of the requester and record
	 * the reset.  wait_start is left untouched so the in-flight measurement
	 * still lands (in the freshly-zeroed counters), and current_event is
	 * zeroed so external readers do not see stale state.
	 */
	cur_reset_gen = pg_atomic_read_u32(&my_wait_event_timing->reset_generation);
	if (cur_reset_gen != my_last_reset_generation)
	{
		memset(my_wait_event_timing->events, 0,
			   sizeof(my_wait_event_timing->events));
		lwlock_timing_hash_clear(my_wait_event_timing);
		my_wait_event_timing->reset_count++;
		my_wait_event_timing->lwlock_overflow_count = 0;
		my_wait_event_timing->flat_overflow_count = 0;
		my_wait_event_timing->current_event = 0;
		my_last_reset_generation = cur_reset_gen;
	}

	if (event != 0 && !INSTR_TIME_IS_ZERO(my_wait_event_timing->wait_start))
	{
		instr_time	now;
		int64		duration_ns;
		int			idx;
		WaitEventTimingEntry *entry = NULL;

		INSTR_TIME_SET_CURRENT(now);
		duration_ns = INSTR_TIME_GET_NANOSEC(now) -
			INSTR_TIME_GET_NANOSEC(my_wait_event_timing->wait_start);

		if (duration_ns < 0)
			duration_ns = 0;

		idx = wait_event_timing_index(event);

		/*
		 * Single-writer hot path: each slot has exactly one writer (the
		 * owning backend), and the SRF reader is lock-free, so no locking is
		 * needed here.  Events that do not map to a slot -- an LWLock tranche
		 * beyond the per-backend cap, or a class unknown to the timing tables
		 * -- bump a per-backend overflow counter.  We deliberately do not log
		 * here: this runs inline in every wait_end, potentially deep in the
		 * backend stack, so the overflow counters are surfaced through a
		 * statistics view rather than through ereport().
		 */
		if (idx == WAIT_EVENT_TIMING_IDX_LWLOCK)
			entry = lwlock_timing_lookup(my_wait_event_timing, event & 0xFFFF);
		else if (idx >= 0)
			entry = &my_wait_event_timing->events[idx];

		if (entry != NULL)
		{
			entry->count++;
			entry->total_ns += duration_ns;
			if (duration_ns > entry->max_ns)
				entry->max_ns = duration_ns;
			entry->histogram[wait_event_timing_bucket(duration_ns)]++;
		}
		else if (idx == WAIT_EVENT_TIMING_IDX_LWLOCK)
			my_wait_event_timing->lwlock_overflow_count++;
		else if (idx == -1)
			my_wait_event_timing->flat_overflow_count++;

		/* Trace level: push the completed wait into the per-session ring. */
		if (capture_level == WAIT_EVENT_CAPTURE_TRACE &&
			!wait_event_trace_writes_disabled &&
			!wait_event_trace_in_write)
		{
			/*
			 * Lazy-attach the ring on first use here (not in the assign hook,
			 * which must not ereport on OOM).  The writes-disabled gate also
			 * blocks this re-attach during slot-state transitions
			 * (release_slot / before_shmem_exit): without it a nested wait
			 * event mid-transition could recurse into a fresh attach that
			 * deadlocks on the lock the transition already holds.
			 */
			if (my_wait_event_trace == NULL && my_trace_proc_number >= 0)
				wait_event_trace_attach(my_trace_proc_number);

			if (my_wait_event_trace != NULL)
			{
				/*
				 * Single-writer claim: a plain read+write avoids the LOCK
				 * XADD that pg_atomic_fetch_add_u64 would emit on every wait
				 * event.  Cross-backend readers use pg_atomic_read_u64 and
				 * rely on the per-record seqlock below for safety.
				 */
				uint64		pos = pg_atomic_read_u64(&my_wait_event_trace->write_pos);
				WaitEventTraceRecord *rec;
				uint32		seq;

				/*
				 * Wait events emitted while a trace record is being written
				 * must not themselves be ring-recorded (the gate above): the
				 * record write is plain stores and cannot wait, but the
				 * injection point below can, and the wait machinery it runs
				 * (DSM registry lookup, condition-variable sleep) emits wait
				 * events whose wait_end would recurse into this block --
				 * re-running the injection point and self-deadlocking on
				 * locks the outer invocation still holds.  Costs one
				 * process-local store each way; nested waits still
				 * accumulate in the stats above.
				 */
				wait_event_trace_in_write = true;

				pg_atomic_write_u64(&my_wait_event_trace->write_pos, pos + 1);

				/*
				 * Injection point for the regression test of the
				 * position-encoded identity seqlock: stalling here widens the
				 * window between the write_pos store and the rec->seq store,
				 * simulating weak-memory visibility that would otherwise be
				 * unreachable on x86.  Compiled out unless
				 * --enable-injection-points.
				 */
				INJECTION_POINT("wait-event-trace-after-write-pos", NULL);

				rec = &my_wait_event_trace->records[pos & my_wait_event_trace->ring_mask];
				seq = (uint32) (pos * 2 + 1);

				rec->seq = seq;
				pg_write_barrier(); /* payload stores must not rise above
									 * seq=odd */

				rec->record_type = TRACE_WAIT_EVENT;
				rec->timestamp_ns = INSTR_TIME_GET_NANOSEC(now);
				rec->data.wait.event = event;
				rec->data.wait.pad2 = 0;
				rec->data.wait.duration_ns = duration_ns;

				pg_write_barrier(); /* payload stores must land before
									 * seq=even */
				rec->seq = seq + 1;

				wait_event_trace_in_write = false;
			}
		}

		INSTR_TIME_SET_ZERO(my_wait_event_timing->wait_start);
	}
}

/*
 * Resolve the optional pid SRF argument to a procNumber range
 * [out_start, out_end).  Returns false if the SRF should emit zero rows
 * (unknown pid -- silent no-op).
 */
static bool
wait_event_timing_pid_range(FunctionCallInfo fcinfo,
							int *out_start, int *out_end)
{
	if (PG_ARGISNULL(0))
	{
		*out_start = 0;
		*out_end = NUM_WAIT_EVENT_TIMING_SLOTS;
		return true;
	}
	else
	{
		int			target_pid = PG_GETARG_INT32(0);
		PGPROC	   *proc;
		int			procNumber;

		proc = BackendPidGetProc(target_pid);
		if (proc == NULL)
			proc = AuxiliaryPidGetProc(target_pid);
		if (proc == NULL)
			return false;

		procNumber = GetNumberFromPGProc(proc);
		if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
			return false;

		*out_start = procNumber;
		*out_end = procNumber + 1;
		return true;
	}
}

/* Emit one SRF row for a populated timing entry. */
static void
wait_event_timing_emit_row(ReturnSetInfo *rsinfo, PgBackendStatus *beentry,
						   int procnumber, uint32 wait_event_info,
						   WaitEventTimingEntry *entry,
						   ArrayType *hist_array, int64 *hist_payload)
{
	Datum		values[10];
	bool		nulls[10];
	const char *event_type;
	const char *event_name;
	int			bucket;

	event_type = pgstat_get_wait_event_type(wait_event_info);
	event_name = pgstat_get_wait_event(wait_event_info);
	if (event_type == NULL || event_name == NULL)
		return;

	memset(nulls, 0, sizeof(nulls));

	values[0] = Int32GetDatum(beentry->st_procpid);
	values[1] = CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));
	values[2] = Int32GetDatum(procnumber);
	values[3] = CStringGetTextDatum(event_type);
	values[4] = CStringGetTextDatum(event_name);
	values[5] = Int64GetDatum(entry->count);
	values[6] = Float8GetDatum((double) entry->total_ns / 1000000.0);
	values[7] = Float8GetDatum(entry->count > 0
							   ? (double) entry->total_ns / entry->count / 1000.0
							   : 0.0);
	values[8] = Float8GetDatum((double) entry->max_ns / 1000.0);

	for (bucket = 0; bucket < WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS; bucket++)
		hist_payload[bucket] = entry->histogram[bucket];
	values[9] = PointerGetDatum(hist_array);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * SQL function: pg_stat_get_wait_event_timing(pid int4, OUT ...)
 *
 * Returns one row per (backend, wait_event) with a non-zero count.  pid is
 * optional: NULL means all backends; a non-NULL value restricts the sweep
 * to that backend (silently empty for unknown pids).
 */
Datum
pg_stat_get_wait_event_timing(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			start_idx;
	int			end_idx;
	int			backend_idx;
	ArrayType  *hist_array;
	int64	   *hist_payload;

	InitMaterializedSRF(fcinfo, 0);

	if (!wait_event_timing_attach_array(false))
		PG_RETURN_VOID();

	if (!wait_event_timing_pid_range(fcinfo, &start_idx, &end_idx))
		PG_RETURN_VOID();

	/*
	 * Allocate the histogram ArrayType once and reuse it across every row;
	 * tuplestore_putvalues flattens the varlena into its stored tuple, so
	 * rewriting the payload cannot corrupt previously emitted rows.
	 */
	{
		Datum		zero_elems[WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS];

		memset(zero_elems, 0, sizeof(zero_elems));
		hist_array = construct_array_builtin(zero_elems,
											 WAIT_EVENT_TIMING_HISTOGRAM_BUCKETS,
											 INT8OID);
		hist_payload = (int64 *) ARR_DATA_PTR(hist_array);
	}

	for (backend_idx = start_idx; backend_idx < end_idx; backend_idx++)
	{
		WaitEventTimingState *state = wet_slot(backend_idx);
		PgBackendStatus *beentry;
		int			i;

		/* Skip dead backend slots and enforce stats permissions. */
		beentry = pgstat_get_beentry_by_proc_number(backend_idx);
		if (beentry == NULL)
			continue;
		if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
			continue;

		/* Flat array rows (all classes except LWLock). */
		for (i = 0; i < WAIT_EVENT_TIMING_DENSE_CLASSES; i++)
		{
			int			base = wait_event_class_offset[i];
			int			nevents = wait_event_class_nevents[i];
			uint32		classId = wait_event_dense_to_classid[i];
			int			j;

			for (j = 0; j < nevents; j++)
			{
				WaitEventTimingEntry *entry = &state->events[base + j];

				if (entry->count == 0)
					continue;

				wait_event_timing_emit_row(rsinfo, beentry, backend_idx,
										   ((uint32) classId << 24) | j,
										   entry, hist_array, hist_payload);
			}
		}

		/* LWLock hash rows. */
		{
			LWLockTimingHashEntry *entries = wet_lwlock_hash_entries(state);
			WaitEventTimingEntry *events = wet_lwlock_hash_events(state);
			int			hash_size = state->lwlock_hash.hash_size;

			for (i = 0; i < hash_size; i++)
			{
				LWLockTimingHashEntry *he = &entries[i];
				WaitEventTimingEntry *entry;

				if (he->tranche_id == LWLOCK_TIMING_EMPTY_SLOT)
					continue;

				entry = &events[he->dense_idx];
				if (entry->count == 0)
					continue;

				wait_event_timing_emit_row(rsinfo, beentry, backend_idx,
										   PG_WAIT_LWLOCK | he->tranche_id,
										   entry, hist_array, hist_payload);
			}
		}
	}

	PG_RETURN_VOID();
}

/*
 * SQL function: pg_stat_get_wait_event_timing_overflow(pid int4, OUT ...)
 *
 * Exposes the per-backend truncation counters that the recording path
 * maintains: lwlock_overflow_count (LWLock waits dropped because the
 * per-backend tranche hash was full), flat_overflow_count (events whose
 * class index was out of range), and reset_count (resets the backend has
 * observed and acted on).  pid has the same optional semantics as
 * pg_stat_get_wait_event_timing().
 */
Datum
pg_stat_get_wait_event_timing_overflow(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			start_idx;
	int			end_idx;
	int			backend_idx;

	InitMaterializedSRF(fcinfo, 0);

	if (!wait_event_timing_attach_array(false))
		PG_RETURN_VOID();

	if (!wait_event_timing_pid_range(fcinfo, &start_idx, &end_idx))
		PG_RETURN_VOID();

	for (backend_idx = start_idx; backend_idx < end_idx; backend_idx++)
	{
		WaitEventTimingState *state = wet_slot(backend_idx);
		PgBackendStatus *beentry;
		Datum		values[6];
		bool		nulls[6];

		beentry = pgstat_get_beentry_by_proc_number(backend_idx);
		if (beentry == NULL)
			continue;
		if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
			continue;

		memset(nulls, 0, sizeof(nulls));

		values[0] = Int32GetDatum(beentry->st_procpid);
		values[1] = CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));
		values[2] = Int32GetDatum(backend_idx);
		values[3] = Int64GetDatum(state->lwlock_overflow_count);
		values[4] = Int64GetDatum(state->flat_overflow_count);
		values[5] = Int64GetDatum(state->reset_count);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	PG_RETURN_VOID();
}

/*
 * Request a cross-backend self-reset on the given slot: bump the slot's
 * reset_generation and wake the target so it promptly observes the change
 * and clears its own counters at its next wait_end.  Lock-free: only the
 * owning backend ever writes its statistics.
 */
static void
wait_event_timing_request_reset(int slot_idx)
{
	Assert(slot_idx >= 0 && slot_idx < NUM_WAIT_EVENT_TIMING_SLOTS);

	if (!wait_event_timing_attach_array(false))
		return;

	pg_atomic_fetch_add_u32(&wet_slot(slot_idx)->reset_generation, 1);

	/*
	 * The slot index is also the PGPROC array index.  Waking the target
	 * shortens the time before it completes its current wait and notices the
	 * request; setting a latch on a slot with no live owner is harmless.
	 */
	if (ProcGlobal != NULL && ProcGlobal->allProcs != NULL)
		SetLatch(&ProcGlobal->allProcs[slot_idx].procLatch);
}

/*
 * SQL function: pg_stat_reset_wait_event_timing(pid int4)
 *
 *   NULL or own pid : reset the caller's own counters synchronously.
 *   another pid     : request a cross-backend reset (pg_signal_backend).
 *   unknown pid     : silent no-op.
 *
 * Cross-backend resets are asynchronous: the target clears its counters at
 * its next wait_end.  Callers needing read-after-reset semantics should
 * target their own backend, or poll reset_count in
 * pg_stat_wait_event_timing_overflow until it increments.
 */
Datum
pg_stat_reset_wait_event_timing(PG_FUNCTION_ARGS)
{
	int			target_pid;
	PGPROC	   *proc;
	int			procNumber;

	if (PG_ARGISNULL(0) || PG_GETARG_INT32(0) == MyProcPid)
	{
		/*
		 * Own backend: synchronous, no lock needed (single writer).
		 * wait_start is already zero (every wait_end zeroes it and we cannot
		 * be mid-wait while running this function), so there is no in-flight
		 * measurement to preserve.
		 */
		if (my_wait_event_timing != NULL)
		{
			memset(my_wait_event_timing->events, 0,
				   sizeof(my_wait_event_timing->events));
			lwlock_timing_hash_clear(my_wait_event_timing);
			my_wait_event_timing->reset_count++;
			my_wait_event_timing->lwlock_overflow_count = 0;
			my_wait_event_timing->flat_overflow_count = 0;
			my_wait_event_timing->current_event = 0;
		}
		PG_RETURN_VOID();
	}

	/*
	 * Cross-backend reset requires pg_signal_backend, matching
	 * pg_stat_reset_backend_stats(pid): anyone who can terminate the target
	 * backend can already destroy more forensic state than a counter wipe.
	 */
	if (!has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reset another backend's wait event timing"),
				 errdetail("Only roles with privileges of the \"pg_signal_backend\" role may reset another backend's wait event timing.")));

	target_pid = PG_GETARG_INT32(0);

	proc = BackendPidGetProc(target_pid);
	if (proc == NULL)
		proc = AuxiliaryPidGetProc(target_pid);
	if (proc == NULL)
		PG_RETURN_VOID();		/* unknown/dead pid: silent no-op */

	procNumber = GetNumberFromPGProc(proc);
	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
		PG_RETURN_VOID();

	wait_event_timing_request_reset(procNumber);

	PG_RETURN_VOID();
}

/*
 * SQL function: pg_stat_reset_wait_event_timing_all()
 *
 * Request a reset on every backend.  Execution is revoked from PUBLIC by
 * default (the blast radius is the whole cluster, a different decision
 * from the per-backend variant); administrators can delegate with GRANT.
 */
Datum
pg_stat_reset_wait_event_timing_all(PG_FUNCTION_ARGS)
{
	int			i;

	/*
	 * Execution is revoked from PUBLIC in system_views.sql; administrators
	 * can delegate with GRANT EXECUTE.
	 */
	if (!wait_event_timing_attach_array(false))
		PG_RETURN_VOID();

	for (i = 0; i < NUM_WAIT_EVENT_TIMING_SLOTS; i++)
		wait_event_timing_request_reset(i);

	PG_RETURN_VOID();
}

/* ================= Trace-level readers ================= */

/*
 * SQL function: pg_get_backend_wait_event_trace()
 *
 * Returns trace records from the current backend's own ring buffer.  This
 * function is deliberately session-local; cross-backend reading goes
 * through pg_get_wait_event_trace(procnumber) below, which implements the
 * snapshot-under-lock protocol documented on WaitEventTraceControl in
 * wait_event_timing.h.  The name mirrors pg_get_backend_memory_contexts()
 * to make the session-local scope explicit at the API level.
 *
 * Same-backend coordination with wait_event_trace_release_slot uses the
 * wait_event_trace_srf_in_progress / _release_pending flags rather than
 * an LWLock: same-backend serialization is implicit, so a per-backend
 * bool plus a deferred-free path is sufficient and avoids any of the
 * cross-backend lock-hold latency that the cross-backend reader pattern
 * has to manage.  PG_TRY/PG_FINALLY guarantees the flag is cleared and
 * any deferred dsa_free is performed even on ereport(ERROR).
 *
 * Uses InitMaterializedSRF (materialize-all).  The ring holds up to
 * WaitEventTraceRingSize records (set at server start from the
 * wait_event_trace_ring_size GUC; default 4 MB = 131072 records);
 * full materialization caps the per-call cost at the ring size of
 * tuplestore memory, which is acceptable for the use case this SRF
 * is designed for: interactive own-session diagnostics from psql.
 *
 * This SRF is NOT the path for cross-backend monitoring tools: it is
 * hard-coded to the calling backend's own ring, so a bgworker selecting
 * from pg_backend_wait_event_trace via SPI would see only its own
 * (typically empty) ring.  Cross-backend consumers use
 * pg_get_wait_event_trace(procnumber) for SQL access, or implement the
 * snapshot-under-lock protocol on WaitEventTraceControl directly.
 *
 * value-per-call (deferred) SRF mode would let an interactive
 * "SELECT ... FROM pg_backend_wait_event_trace LIMIT N" short-circuit
 * the materialisation, but converting this function would require
 * spanning the wait_event_trace_srf_in_progress flag (and its
 * deferred-free coordination with assign_wait_event_capture) across
 * multiple SRF callbacks plus a transaction-cleanup registration to
 * handle LIMIT abandonment.  The complexity is not
 * justified for the diagnostic use case, especially since cross-
 * backend monitoring (the consumer that would actually benefit from
 * streaming) goes through the snapshot pattern above instead.
 * Interactive callers who want only recent records should use
 * "ORDER BY seq DESC LIMIT N" -- the LIMIT is applied after
 * materialisation but the cost stays bounded by the ring size.
 */
Datum
pg_get_backend_wait_event_trace(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	WaitEventTraceState *ts;
	uint64		write_pos;
	uint64		read_start;
	uint64		i;

	InitMaterializedSRF(fcinfo, 0);

	if (my_wait_event_trace == NULL)
		PG_RETURN_VOID();

	ts = my_wait_event_trace;

	write_pos = pg_atomic_read_u64(&ts->write_pos);

	if (write_pos == 0)
		PG_RETURN_VOID();

	/* Read from oldest available to newest */
	{
		uint64		ring_size = (uint64) ts->ring_mask + 1;

		read_start = (write_pos > ring_size)
			? write_pos - ring_size : 0;
	}

	/*
	 * Mark the iteration in progress so wait_event_trace_release_slot defers
	 * any concurrent dsa_free of our own ring (see the comment on that
	 * function for the deferral protocol).  PG_FINALLY clears the flag and
	 * performs any deferred free, even on ereport(ERROR).
	 */
	wait_event_trace_srf_in_progress = true;
	PG_TRY();
	{
		for (i = read_start; i < write_pos; i++)
		{
			WaitEventTraceRecord *rec =
				&ts->records[i & ts->ring_mask];
			Datum		values[6];
			bool		nulls[6];
			const char *event_type;
			const char *event_name;
			uint32		seq_before;
			uint32		seq_after;
			uint8		rtype;
			int64		timestamp_ns;
			uint32		event_info;
			int64		duration_ns;
			int64		query_id;

			/* Seqlock read */
			seq_before = rec->seq;
			pg_read_barrier();	/* acquire: payload loads below must not rise
								 * above this */

			if (seq_before & 1)
				continue;

			rtype = rec->record_type;
			timestamp_ns = rec->timestamp_ns;

			if (rtype == TRACE_WAIT_EVENT)
			{
				event_info = rec->data.wait.event;
				duration_ns = rec->data.wait.duration_ns;
				query_id = 0;
			}
			else if (rtype == TRACE_QUERY_START || rtype == TRACE_QUERY_END ||
					 rtype == TRACE_EXEC_START || rtype == TRACE_EXEC_END)
			{
				event_info = 0;
				duration_ns = 0;
				query_id = rec->data.query.query_id;
			}
			else
			{
				pg_read_barrier();	/* acquire: pair with seq_before read
									 * above before skipping */
				continue;
			}

			pg_read_barrier();	/* acquire: payload loads must have landed
								 * before seq_after */
			seq_after = rec->seq;

			if (seq_before != seq_after)
				continue;

			/* Skip empty wait events */
			if (rtype == TRACE_WAIT_EVENT && event_info == 0)
				continue;

			if (rtype == TRACE_WAIT_EVENT)
			{
				event_type = pgstat_get_wait_event_type(event_info);
				event_name = pgstat_get_wait_event(event_info);
			}
			else if (rtype == TRACE_QUERY_START)
			{
				event_type = "Query";
				event_name = "QueryStart";
			}
			else if (rtype == TRACE_EXEC_START)
			{
				event_type = "Query";
				event_name = "ExecStart";
			}
			else if (rtype == TRACE_EXEC_END)
			{
				event_type = "Query";
				event_name = "ExecEnd";
			}
			else
			{
				event_type = "Query";
				event_name = "QueryEnd";
			}

			if (event_type == NULL || event_name == NULL)
				continue;

			memset(nulls, 0, sizeof(nulls));

			values[0] = Int64GetDatum((int64) i);
			values[1] = Int64GetDatum(timestamp_ns);
			values[2] = CStringGetTextDatum(event_type);
			values[3] = CStringGetTextDatum(event_name);
			values[4] = Float8GetDatum((double) duration_ns / 1000.0);
			values[5] = Int64GetDatum(query_id);

			tuplestore_putvalues(rsinfo->setResult,
								 rsinfo->setDesc,
								 values, nulls);
		}
	}
	PG_FINALLY();
	{
		wait_event_trace_srf_in_progress = false;

		/*
		 * If a GUC step-down fired during iteration, it deferred the
		 * dsa_free.  Process it now that we're safely past the loop. Re-check
		 * release_pending under the same flag to handle the
		 * (impossible-today, possible-tomorrow) case of a nested SRF.
		 */
		if (wait_event_trace_release_pending)
		{
			wait_event_trace_release_pending = false;
			if (my_trace_proc_number >= 0)
				wait_event_trace_release_slot(my_trace_proc_number);
		}
	}
	PG_END_TRY();

	PG_RETURN_VOID();
}

/*
 * One element of the local result buffer.  Pairs a per-record copy
 * with the original ring index (used as the seq output column).
 */
typedef struct WetValidRecord
{
	uint64		ring_index;		/* original index in the writer's ring */
	WaitEventTraceRecord rec;
} WetValidRecord;

/*
 * Snapshot the trace ring for a given procNumber and emit records into
 * the SRF's tuplestore.  Returns silently for FREE slots, out-of-range
 * procnumbers, slots whose ring was never allocated, and slots whose
 * write_pos is zero.
 *
 * Cross-backend reader protocol implemented here:
 *
 *   1. Read slot->state without the lock as a cheap "worth visiting"
 *      check; FREE -> nothing to emit.
 *   2. Allocate the worst-case result buffer BEFORE taking the lock,
 *      so the palloc -- which can bottom out in a glibc mmap syscall
 *      for the worst-case (full-ring) size -- runs without holding the
 *      WaitEventTraceCtl lock.
 *   3. Acquire WaitEventTraceCtl->lock in LW_SHARED.  All slot
 *      transitions take LW_EXCLUSIVE, so the slot's identity, state,
 *      and ring_ptr are stable for the duration of the iteration.
 *   4. Re-check state under the lock and resolve ring_ptr via
 *      dsa_get_address.  Read write_pos.
 *   5. Iterate every live ring index [read_start, write_pos).  For
 *      each record do the per-record POSITION-ENCODED IDENTITY
 *      seqlock check ON SHARED MEMORY (see the comment on the loop
 *      below).
 *   6. Release the lock.
 *   7. Walk the local result array and emit rows into the tuplestore.
 *      This is the expensive part (potential disk spill); doing it
 *      after release minimises lock-hold time.
 *
 * Why per-record seqlock against shared memory, not against a local
 * memcpy of the full ring: the protocol requires the two seq reads
 * to go to the SAME shared-memory location at DIFFERENT TIMES, with
 * the payload read between them.  A bulk memcpy then seqlock-on-
 * local-copy reads the same frozen byte twice, the check degenerates
 * to a no-op, and torn / stale-cycle reads slip through.
 *
 * Why position-encoded identity, not just parity: the writer encodes
 * the ring position into the seq value (mid-write = pos*2+1, complete
 * = pos*2+2).  After RING_SIZE writes the slot wraps and is rewritten
 * with a new numerically-distinct seq.  A parity-only check accepts
 * any stable even seq -- including the PREVIOUS cycle's seq if cross-
 * process visibility puts the new write_pos ahead of the new seq
 * update.  See the loop body for the four failure modes the identity
 * check rejects.
 *
 * Holding LW_SHARED throughout the iteration also makes the
 * generation-counter retry unnecessary for this caller: slot
 * transitions take LW_EXCLUSIVE and therefore cannot happen while we
 * hold LW_SHARED.  The generation counter is still part of the
 * cross-backend reader contract on WaitEventTraceControl for external
 * readers that follow a different lock-release pattern (e.g. an
 * extension that wants to release the lock between batches of records
 * and re-acquire), but this in-tree implementation does not release
 * the lock mid-iteration.
 *
 * Both OWNED and ORPHANED slots are read uniformly.  For OWNED the
 * live owner is concurrently writing; the seqlock catches torn reads.
 * For ORPHANED the records are immutable post-mortem so the check is
 * essentially a pass-through (it still correctly skips at most one
 * trailing odd-seq record if the owner died mid-write).
 *
 * Lock-hold is O(write_pos - read_start) shared-memory loads, at
 * roughly the same wall-clock cost as a single 4 MB memcpy of the
 * full ring (~1 ms on modern hardware), with no I/O and no syscalls.
 */
static void
emit_wait_event_trace_for_procnumber(int procNumber, ReturnSetInfo *rsinfo)
{
	WaitEventTraceSlot *slot;
	WaitEventTraceState *ts;
	WetValidRecord *valid_records = NULL;
	uint64		valid_count = 0;
	uint64		write_pos;
	uint64		read_start;
	uint64		i;
	uint32		state_now;

	if (WaitEventTraceCtl == NULL)
		return;

	/*
	 * Range check.  Negative or out-of-range procnumbers return an empty
	 * result rather than ERRORing because the most natural use pattern for
	 * cross-backend readers is to iterate every possible slot index (a
	 * monitoring background worker doesn't know the exact
	 * NUM_WAIT_EVENT_TIMING_SLOTS at SQL level), and silent- empty for
	 * out-of-range matches the behaviour of sister functions like
	 * pg_stat_get_wait_event_timing(NULL) which iterate the shared array
	 * internally.  FREE-but-in-range slots also return empty (see the state
	 * check below); the caller cannot distinguish out-of-range from FREE,
	 * which is fine.
	 */
	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS)
		return;

	slot = &WaitEventTraceCtl->trace_slots[procNumber];

	/*
	 * If the trace DSA was never created (no backend in the cluster has ever
	 * set wait_event_capture = trace), every slot is still in its initial
	 * FREE state.  Skip without taking the lock.
	 */
	if (WaitEventTraceCtl->trace_dsa_handle == DSA_HANDLE_INVALID)
		return;

	/*
	 * Unlocked fast-path check; the authoritative check is under the lock
	 * below.
	 */
	if (pg_atomic_read_u32(&slot->state) == WAIT_EVENT_TRACE_SLOT_FREE)
		return;

	wait_event_trace_ensure_dsa();
	if (trace_dsa == NULL)
		return;

	/*
	 * Allocate the worst-case result buffer BEFORE taking the lock. The
	 * buffer is sized for the full ring (sizeof(WetValidRecord) *
	 * WaitEventTraceRingSize, e.g. ~5 MB at the 4 MB default ring and up to
	 * ~42 MB at the 32 MB maximum); on a near-empty ring most goes unused,
	 * but that is preferable to holding the WaitEventTraceCtl lock during a
	 * palloc that may bottom out in a glibc mmap() syscall (allocations above
	 * the malloc-mmap threshold).  Glibc's arena-internal mutex around the
	 * syscall would serialise every concurrent reader of this lock through
	 * one VMA-modifying kernel operation; sizing the alloc outside the lock
	 * keeps the lock-hold time bounded by the per-record loop alone.
	 *
	 * After we acquire the lock we will either consume this buffer (writing
	 * up to (write_pos - read_start) entries) or release it unused on an
	 * early return.
	 */

	/*
	 * Worst-case size = ring size.  Derive it from the GUC on first use in
	 * this backend; subsequent calls see the cached value. The GUC is
	 * PGC_POSTMASTER so the value is the same across every backend in this
	 * postmaster run and never changes.
	 */
	if (WaitEventTraceRingSize == 0)
		WaitEventTraceRingSize =
			(uint32) wait_event_trace_ring_size * 1024U /
			(uint32) sizeof(WaitEventTraceRecord);
	valid_records = palloc(sizeof(WetValidRecord) * WaitEventTraceRingSize);

	LWLockAcquire(&WaitEventTraceCtl->lock, LW_SHARED);

	state_now = pg_atomic_read_u32(&slot->state);
	if (state_now == WAIT_EVENT_TRACE_SLOT_FREE ||
		!DsaPointerIsValid(slot->ring_ptr))
	{
		LWLockRelease(&WaitEventTraceCtl->lock);
		pfree(valid_records);
		return;
	}

	ts = (WaitEventTraceState *) dsa_get_address(trace_dsa, slot->ring_ptr);
	write_pos = pg_atomic_read_u64(&ts->write_pos);

	if (write_pos == 0)
	{
		LWLockRelease(&WaitEventTraceCtl->lock);
		pfree(valid_records);
		return;
	}

	/* Live range: oldest available to newest. */
	{
		uint64		ring_size = (uint64) ts->ring_mask + 1;

		read_start = (write_pos > ring_size)
			? write_pos - ring_size : 0;
	}

	for (i = read_start; i < write_pos; i++)
	{
		WaitEventTraceRecord *rec_shared =
			&ts->records[i & ts->ring_mask];
		WetValidRecord *out = &valid_records[valid_count];
		uint32		expected_seq;
		uint32		seq_before;
		uint32		seq_after;

		/*
		 * Position-encoded seqlock identity check (NOT just parity).
		 *
		 * The writer encodes the ring position into the seq value: mid-write
		 * -> (uint32)(pos * 2 + 1), complete -> + 2.  After RING_SIZE writes
		 * the slot wraps and the same memory location gets a new seq value
		 * (next_pos * 2 + 2) that is numerically distinct from the previous
		 * cycle's seq.
		 *
		 * A parity-only check (skip on odd seq, accept on stable even) is
		 * INSUFFICIENT for this layout in the cross-backend case: if the
		 * writer just incremented write_pos to pos+1 but cross-process cache
		 * coherence has not yet propagated the subsequent rec->seq =
		 * (pos*2+1) store, this reader at i = pos would see the previous
		 * cycle's complete-even seq (from logical position pos - RING_SIZE).
		 * Both seq_before and seq_after would read that stale even value,
		 * parity passes, identity-against-itself passes, and a record
		 * belonging to the PREVIOUS cycle gets emitted with the new
		 * ring_index = pos.  Silent data corruption (wrong attribution, not
		 * torn bytes).
		 *
		 * The fix is identity against EXPECTED: a record is valid for
		 * iterator position i if and only if its seq equals (uint32)(i * 2 +
		 * 2) -- the writer's encoded "complete" value for that exact ring
		 * position.  This rejects:
		 *
		 * * Stale prior cycle (seq <  expected): writer hasn't yet advanced
		 * rec->seq for the current cycle. * Mid-write current cycle (seq ==
		 * expected - 1, odd): writer is in the payload write window. * Ring
		 * wrapped past us (seq >  expected): the writer completed a later
		 * cycle on this slot during our read.
		 *
		 * The uint32 wraparound at 2^31 cycles is safe: we use exact
		 * equality, and the writer's existing wrap-safety argument
		 * (sizeof(seq) > worst-case in-flight window by 11 orders of
		 * magnitude) covers the seq value.
		 */
		expected_seq = (uint32) (i * 2 + 2);

		seq_before = rec_shared->seq;
		pg_read_barrier();

		if (seq_before != expected_seq)
			continue;

		out->rec = *rec_shared; /* one 32-byte structure copy */

		pg_read_barrier();
		seq_after = rec_shared->seq;

		if (seq_after != expected_seq)
			continue;

		out->ring_index = i;
		valid_count++;
	}

	LWLockRelease(&WaitEventTraceCtl->lock);

	/*
	 * Walk the local result array and emit rows.  No shared-memory access
	 * from here on, so spills to disk by the tuplestore (if the result is
	 * large) do not hold any wait-event-timing lock.
	 */
	for (i = 0; i < valid_count; i++)
	{
		WetValidRecord *vr = &valid_records[i];
		WaitEventTraceRecord *rec = &vr->rec;
		Datum		values[6];
		bool		nulls[6];
		const char *event_type;
		const char *event_name;
		uint8		rtype = rec->record_type;
		uint32		event_info;
		int64		duration_ns;
		int64		query_id;

		if (rtype == TRACE_WAIT_EVENT)
		{
			event_info = rec->data.wait.event;
			duration_ns = rec->data.wait.duration_ns;
			query_id = 0;

			/* Skip empty wait events. */
			if (event_info == 0)
				continue;

			event_type = pgstat_get_wait_event_type(event_info);
			event_name = pgstat_get_wait_event(event_info);
		}
		else if (rtype == TRACE_QUERY_START)
		{
			event_info = 0;
			duration_ns = 0;
			query_id = rec->data.query.query_id;
			event_type = "Query";
			event_name = "QueryStart";
		}
		else if (rtype == TRACE_QUERY_END)
		{
			event_info = 0;
			duration_ns = 0;
			query_id = rec->data.query.query_id;
			event_type = "Query";
			event_name = "QueryEnd";
		}
		else if (rtype == TRACE_EXEC_START)
		{
			event_info = 0;
			duration_ns = 0;
			query_id = rec->data.query.query_id;
			event_type = "Query";
			event_name = "ExecStart";
		}
		else if (rtype == TRACE_EXEC_END)
		{
			event_info = 0;
			duration_ns = 0;
			query_id = rec->data.query.query_id;
			event_type = "Query";
			event_name = "ExecEnd";
		}
		else
		{
			/* Unrecognised record_type -- skip defensively. */
			continue;
		}

		if (event_type == NULL || event_name == NULL)
			continue;

		memset(nulls, 0, sizeof(nulls));

		values[0] = Int64GetDatum((int64) vr->ring_index);
		values[1] = Int64GetDatum(rec->timestamp_ns);
		values[2] = CStringGetTextDatum(event_type);
		values[3] = CStringGetTextDatum(event_name);
		values[4] = Float8GetDatum((double) duration_ns / 1000.0);
		values[5] = Int64GetDatum(query_id);

		tuplestore_putvalues(rsinfo->setResult,
							 rsinfo->setDesc,
							 values, nulls);
	}

	pfree(valid_records);
}

/*
 * SQL function: pg_get_wait_event_trace(procnumber int4)
 *
 * Cross-backend trace ring reader.  Returns the records from the trace
 * ring belonging to the backend that currently or previously occupied
 * the given procNumber slot.  Reads OWNED and ORPHANED slots uniformly;
 * FREE slots return an empty result.
 *
 * This SRF is the in-tree consumer of the orphan-preserved trace data:
 * a backend that exited while wait_event_capture = trace leaves its
 * ring allocated in DSA in ORPHANED state, and this function reads it
 * until either a new backend takes over the same procNumber or the
 * DBA calls pg_stat_clear_orphaned_wait_event_rings().  External
 * extensions that need cross-backend access follow the same
 * snapshot pattern documented on WaitEventTraceControl in
 * wait_event_timing.h; this function serves as both the reference
 * implementation and a DBA-facing diagnostic tool.
 *
 * Privileges: REVOKE'd from PUBLIC and GRANT'ed to pg_read_all_stats
 * in system_views.sql, matching the privilege model of the session-
 * local view pg_backend_wait_event_trace.
 *
 * The procnumber argument can be obtained from the procnumber column
 * of pg_stat_get_wait_event_timing or pg_stat_get_wait_event_timing_
 * overflow.  For pid-keyed access against live backends, callers can
 * do:
 *
 *   SELECT * FROM pg_get_wait_event_trace(
 *       (SELECT procnumber FROM pg_stat_get_wait_event_timing(<pid>)
 *        WHERE pid = <pid> LIMIT 1));
 *
 * Note that pid-keyed access cannot read ORPHANED slots because a
 * dying backend's pid is removed from procArray on exit; for
 * post-mortem reading of short-lived backends (parallel workers,
 * autovacuum, walsender) the procNumber must be captured before the
 * backend exits, or discovered by iterating procnumbers in a
 * monitoring background worker.
 */
Datum
pg_get_wait_event_trace(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int32		procNumber = PG_GETARG_INT32(0);

	InitMaterializedSRF(fcinfo, 0);

	emit_wait_event_trace_for_procnumber((int) procNumber, rsinfo);

	PG_RETURN_VOID();
}

/*
 * SQL function: pg_stat_clear_orphaned_wait_event_rings()
 *
 * Free every trace ring whose owner has exited (slot state ORPHANED).
 * Returns the number of rings released.
 *
 * Why this exists.  When a backend that had wait_event_capture = trace
 * exits, we deliberately do NOT free its ~4 MB trace ring (see the
 * lifecycle discussion on WaitEventTraceControl): the data must remain
 * readable by cross-backend consumers -- the in-tree
 * pg_get_wait_event_trace SRF and any extension following the
 * snapshot pattern on WaitEventTraceControl -- and an exit-time
 * dsa_free would defeat that.
 * The reclaim instead happens lazily in two places:
 *
 *   (a) wait_event_trace_clear_orphan_at_init(): when a new backend
 *       inherits the same procNumber slot at init, it frees the prior
 *       orphan as part of starting clean.  This handles the common
 *       case (busy clusters with connection churn) automatically.
 *
 *   (b) THIS FUNCTION: an explicit DBA-driven sweep that releases
 *       every currently orphaned ring at once.
 *
 * The pathological case (a) does not handle is "capture briefly
 * enabled, then disabled, on a cluster with long-lived pooled
 * connections that never exit".  In that scenario procNumbers do not
 * recycle, so prior orphans persist until cluster restart unless the
 * DBA calls this function.  Worst-case bound is
 * NUM_WAIT_EVENT_TIMING_SLOTS * sizeof(WaitEventTraceState) which is
 * ~400 MB at MaxBackends=100, ~4 GB at MaxBackends=1000 -- bounded
 * but worth a kill switch.
 *
 * Permissions: execution is revoked from PUBLIC by default, matching the
 * cluster-wide reset (pg_stat_reset_wait_event_timing_all).  This is a
 * cluster-scope memory-reclamation operation: it can disrupt any
 * concurrent cross-backend reader on any orphaned slot.  The
 * disruption is bounded (readers retry via the generation counter
 * and at worst skip one read) but the operation is still
 * cluster-wide, so the default privilege matches the reset variant
 * with the same blast radius; administrators can delegate with GRANT.
 *
 * The function is safe to call even when no orphans exist (returns
 * 0) and even when capture is currently OFF (the slot array exists
 * unconditionally; only the rings are lazy).
 */
Datum
pg_stat_clear_orphaned_wait_event_rings(PG_FUNCTION_ARGS)
{
	int64		freed = 0;
	int			i;

	/*
	 * Execution is revoked from PUBLIC in system_views.sql; administrators
	 * can delegate with GRANT EXECUTE.
	 */
	if (WaitEventTraceCtl == NULL)
		PG_RETURN_INT64(0);

	/*
	 * If no backend has ever enabled trace, the trace DSA was never created
	 * and there cannot be any ORPHANED slots: every slot is still in its
	 * initial FREE state.  Nothing to do.
	 */
	if (WaitEventTraceCtl->trace_dsa_handle == DSA_HANDLE_INVALID)
		PG_RETURN_INT64(0);

	/* Attach to the trace DSA so dsa_free() can be called. */
	wait_event_trace_ensure_dsa();
	if (trace_dsa == NULL)
		PG_RETURN_INT64(0);

	/*
	 * Walk every slot, taking and releasing WaitEventTraceCtl->lock per slot
	 * rather than holding it across the entire sweep.
	 *
	 * Rationale: at MaxBackends = 1000 with a fully-orphaned cluster the
	 * per-slot work (atomic state read + dsa_free + ring_ptr clear + atomic
	 * state write) totals a few microseconds; holding the lock across all
	 * slots would yield a millisecond-scale lock-hold window during which
	 * every concurrent backend startup (the lazy
	 * wait_event_trace_clear_orphan_at_init path), every cross-backend reader
	 * (pg_get_wait_event_trace and the external snapshot pattern), and every
	 * capture step-down or restore would stall.  PG's general convention is
	 * to keep LWLock-held windows in paths that compete with regular activity
	 * well under 100 microseconds; per-slot release/reacquire gives us a
	 * worst- case lock-hold of one slot's worth of work regardless of how
	 * many orphans exist cluster-wide.
	 *
	 * An unlocked fast-path read of slot->state skips non-ORPHANED slots
	 * without an LWLockAcquire/Release pair.  This is safe: if a slot races
	 * from non-ORPHANED to ORPHANED after we read it, we miss that orphan --
	 * but the function is documented as a snapshot sweep, the missed orphan
	 * can be cleared by a subsequent call, and the same race exists for
	 * orphans that appear after the loop ends.  The authoritative re-check
	 * under the lock prevents racing on the dsa_free direction (we never free
	 * a slot whose owner became OWNED again).
	 *
	 * CHECK_FOR_INTERRUPTS at the top of the loop body lets the caller cancel
	 * a long sweep; with the previous single-lock structure the
	 * InterruptHoldoffCount elevation from LWLockAcquire deferred all
	 * cancellation until release.
	 */
	for (i = 0; i < NUM_WAIT_EVENT_TIMING_SLOTS; i++)
	{
		WaitEventTraceSlot *slot = &WaitEventTraceCtl->trace_slots[i];

		CHECK_FOR_INTERRUPTS();

		/* Unlocked fast-path: skip non-ORPHANED slots cheaply. */
		if (pg_atomic_read_u32(&slot->state) != WAIT_EVENT_TRACE_SLOT_ORPHANED)
			continue;

		LWLockAcquire(&WaitEventTraceCtl->lock, LW_EXCLUSIVE);

		/*
		 * Authoritative re-check under the lock.  A concurrent
		 * clear_orphan_at_init may have already freed this slot.
		 */
		if (pg_atomic_read_u32(&slot->state) == WAIT_EVENT_TRACE_SLOT_ORPHANED &&
			DsaPointerIsValid(slot->ring_ptr))
		{
			pg_atomic_fetch_add_u64(&slot->generation, 1);
			dsa_free(trace_dsa, slot->ring_ptr);
			slot->ring_ptr = InvalidDsaPointer;
			pg_atomic_write_u32(&slot->state, WAIT_EVENT_TRACE_SLOT_FREE);
			freed++;
		}

		LWLockRelease(&WaitEventTraceCtl->lock);
	}

	PG_RETURN_INT64(freed);
}

#endif							/* USE_WAIT_EVENT_TIMING */
