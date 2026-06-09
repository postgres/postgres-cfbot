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
 * The per-backend slot array lives in the main shared memory segment,
 * sized at postmaster start from wait_event_timing_max_tranches, so it is
 * valid for the entire life of every backend -- no lazy attach and no
 * teardown gating are required.
 *
 * Controlled by the wait_event_capture GUC (off | stats, default off) and
 * the compile-time option --enable-wait-event-timing.  In builds without
 * that option the file still compiles (the GUC backing variable, the enum
 * table, the rejecting check hook, and empty-result SQL stubs), so the GUC
 * and the catalog functions exist uniformly.
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

/*
 * Enum value table consumed by guc.c.  Order matches the
 * WaitEventCaptureLevel enum and the documented "off < stats" ordering.
 */
const struct config_enum_entry wait_event_capture_options[] = {
	{"off", WAIT_EVENT_CAPTURE_OFF, false},
	{"stats", WAIT_EVENT_CAPTURE_STATS, false},
	{NULL, 0, false}
};

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

/* Defined so every extern in wait_event_timing.h resolves in stub builds. */
WaitEventTimingState *my_wait_event_timing = NULL;

void
pgstat_set_wait_event_timing_storage(int procNumber)
{
}

void
pgstat_reset_wait_event_timing_storage(void)
{
}

#else							/* USE_WAIT_EVENT_TIMING */

#include "catalog/pg_authid.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procnumber.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
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
 * Backend-local cached pointer to the start of the shared slot array, set
 * at shmem init (postmaster) and, in EXEC_BACKEND mode, at attach.  Slots
 * are NOT a simple C array: each has a runtime-determined stride (header +
 * variable-size hash arrays); use wet_slot() to index.
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

/* Cache the backend-local layout dimensions from the GUC (idempotent). */
static void
wait_event_timing_init_local_dims(void)
{
	if (wait_event_timing_per_backend_stride != 0)
		return;
	wait_event_timing_max_entries = wait_event_timing_max_tranches;
	wait_event_timing_hash_size =
		wait_event_timing_hash_size_for(wait_event_timing_max_entries);
	wait_event_timing_per_backend_stride =
		wait_event_timing_slot_size(wait_event_timing_max_entries);
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
 * ShmemRequest: reserve the per-backend slot array.  Sized from
 * wait_event_timing_max_tranches; the framework stores the allocated
 * address in WaitEventTimingArray before WaitEventTimingShmemInit runs.
 */
static void
WaitEventTimingShmemRequest(void *arg)
{
	Size		stride;

	wait_event_timing_init_local_dims();
	stride = wait_event_timing_per_backend_stride;

	ShmemRequestStruct(.name = "WaitEventTimingArray",
					   .size = mul_size(NUM_WAIT_EVENT_TIMING_SLOTS, stride),
					   .ptr = (void **) &WaitEventTimingArray);
}

/* ShmemInit: zero the array and initialise each slot's hash header. */
static void
WaitEventTimingShmemInit(void *arg)
{
	int			i;

	wait_event_timing_init_local_dims();

	memset(WaitEventTimingArray, 0,
		   mul_size(NUM_WAIT_EVENT_TIMING_SLOTS,
					wait_event_timing_per_backend_stride));

	for (i = 0; i < NUM_WAIT_EVENT_TIMING_SLOTS; i++)
	{
		WaitEventTimingState *slot = wet_slot(i);
		LWLockTimingHashEntry *entries;
		int			j;

		pg_atomic_init_u32(&slot->reset_generation, 0);
		slot->lwlock_hash.num_used = 0;
		slot->lwlock_hash.hash_size = wait_event_timing_hash_size;
		slot->lwlock_hash.max_entries = wait_event_timing_max_entries;

		/* The array was zeroed above, but the empty sentinel is 0xFFFF. */
		entries = wet_lwlock_hash_entries(slot);
		for (j = 0; j < wait_event_timing_hash_size; j++)
			entries[j].tranche_id = LWLOCK_TIMING_EMPTY_SLOT;
	}
}

const ShmemCallbacks WaitEventTimingShmemCallbacks = {
	.request_fn = WaitEventTimingShmemRequest,
	.init_fn = WaitEventTimingShmemInit,
};

/*
 * Point my_wait_event_timing at this backend's slot.  Called from
 * InitProcess()/InitAuxiliaryProcess() once the backend has a procNumber.
 * The slot is cleared here so stats do not leak across slot reuse when a
 * new backend inherits a procNumber previously held by an exited one.
 */
void
pgstat_set_wait_event_timing_storage(int procNumber)
{
	WaitEventTimingState *slot;

	if (procNumber < 0 || procNumber >= NUM_WAIT_EVENT_TIMING_SLOTS ||
		WaitEventTimingArray == NULL)
	{
		my_wait_event_timing = NULL;
		return;
	}

	wait_event_timing_init_local_dims();

	slot = wet_slot(procNumber);

	memset(slot->events, 0, sizeof(slot->events));
	lwlock_timing_hash_clear(slot);
	slot->lwlock_overflow_count = 0;
	slot->flat_overflow_count = 0;
	slot->reset_count = 0;
	slot->current_event = 0;
	INSTR_TIME_SET_ZERO(slot->wait_start);

	/*
	 * Adopt the current shared reset generation as our baseline; the
	 * reset_generation counter persists across slot reuse, so a new backend
	 * must not treat the prior occupant's resets as its own.
	 */
	my_last_reset_generation = pg_atomic_read_u32(&slot->reset_generation);

	/* Publish only after the slot is fully initialised. */
	my_wait_event_timing = slot;
}

/*
 * Detach from the timing slot on backend exit.  The slot itself stays in
 * shared memory; clearing the pointer keeps the late-shutdown wait-event
 * hot path from touching it.
 */
void
pgstat_reset_wait_event_timing_storage(void)
{
	my_wait_event_timing = NULL;
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
}

/*
 * Out-of-line body for pgstat_report_wait_start()'s timing path.  Records
 * the start timestamp and the event being waited on.  Reached only when
 * wait_event_capture != OFF.
 */
void
pgstat_report_wait_start_timing(uint32 wait_event_info)
{
	if (my_wait_event_timing == NULL)
		return;

	INSTR_TIME_SET_CURRENT(my_wait_event_timing->wait_start);
	my_wait_event_timing->current_event = wait_event_info;
}

/*
 * Out-of-line body for pgstat_report_wait_end()'s timing path.  Computes
 * the wait duration and accumulates per-event statistics.
 *
 * capture_level is the value of wait_event_capture observed at the inline
 * gate; in this commit only STATS exists, so it is not branched on, but it
 * is threaded through to keep the gate ABI stable for the trace level
 * added later in the series.
 */
void
pgstat_report_wait_end_timing(int capture_level)
{
	uint32		event;
	uint32		cur_reset_gen;

	(void) capture_level;

	if (my_wait_event_timing == NULL)
		return;

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

	if (WaitEventTimingArray == NULL)
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

	if (WaitEventTimingArray == NULL)
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

	if (WaitEventTimingArray == NULL)
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
	if (WaitEventTimingArray == NULL)
		PG_RETURN_VOID();

	for (i = 0; i < NUM_WAIT_EVENT_TIMING_SLOTS; i++)
		wait_event_timing_request_reset(i);

	PG_RETURN_VOID();
}

#endif							/* USE_WAIT_EVENT_TIMING */
