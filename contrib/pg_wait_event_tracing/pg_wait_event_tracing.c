/*-------------------------------------------------------------------------
 *
 * pg_wait_event_tracing.c
 *	  Statistics-level wait-event collector.
 *
 * The recorder is the peer-review package's collector, ported onto the
 * begin/end wait-event hooks and renamed.  Each collecting backend owns one
 * sparse DSA slot, addressed through a small, always-resident control
 * table; the roughly 200 KiB-per-backend timing payload itself lives in a
 * DSA area (GetNamedDSA()) and is allocated only for a backend that
 * actually enables capture.  Hook callbacks only touch preallocated
 * backend-local pointers: allocation, locking, and error-capable work
 * happen from parse/executor safe points, never from the begin/end hooks
 * themselves.
 *
 * The control table lives in fixed shared memory (shmem_request_hook /
 * shmem_startup_hook), not the DSM registry: a server-side process (the
 * checkpointer, an I/O worker, ...) must reach its slot from inside the
 * begin hook, where it cannot attach anything (see the "server processes"
 * block below), so the table has to already be mapped by the time any
 * hook can fire.  That is true of fixed shmem in every process from
 * postmaster startup on -- the module requires shared_preload_libraries,
 * so shmem_startup_hook always runs before user code does -- but is not
 * true of a DSM-registry segment, which is created/attached lazily on
 * first reference.
 *
 * Server-side processes never reach post_parse_analyze_hook or
 * ExecutorStart_hook (they don't parse queries or run the executor
 * through those entry points), so without help they would only ever
 * attach at the next configuration reload after capture is turned on --
 * missing everything from process start until then, including
 * crash-recovery waits in the startup process.  When capture is already
 * on in the configuration at postmaster start, this module additionally
 * reserves a second, fixed-size region -- one payload-sized slot per
 * possible server-side ProcNumber -- and each such process claims its own
 * slot from inside the begin hook itself (see the claim protocol below),
 * without allocating, locking, waiting, or erroring.
 *
 * This file carries the statistics level only.  The trace level (per-backend
 * ring buffer, query markers, trace SRFs) is a separate patch; the slot
 * layout below reserves the fields that level will need (trace_ptr,
 * trace_state) so that addition does not reshape the control segment.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_authid.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "parser/analyze.h"
#include "port/pg_bitutils.h"
#include "port/atomics.h"
#include "portability/instr_time.h"
#include "postmaster/autovacuum.h"
#include "replication/walsender.h"
#include "storage/dsm_registry.h"
#include "storage/io_worker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/tuplestore.h"
#include "utils/wait_classes.h"
#include "utils/wait_event.h"

#include "pg_wait_event_tracing_data.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_wait_event_tracing",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(pg_stat_get_wait_event_timing);
PG_FUNCTION_INFO_V1(pg_stat_get_wait_event_timing_overflow);
PG_FUNCTION_INFO_V1(pg_stat_reset_wait_event_timing);
PG_FUNCTION_INFO_V1(pg_stat_reset_wait_event_timing_all);
PG_FUNCTION_INFO_V1(pg_wait_event_tracing_capacity);
PG_FUNCTION_INFO_V1(pg_wait_event_tracing_hooks_installed);

PGDLLEXPORT void _PG_init(void);

#define PWET_CONTROL_NAME "pg_wait_event_tracing"
#define PWET_CONTROL_STRUCT_NAME "pg_wait_event_tracing control"
#define PWET_REGION_HEADER_NAME "pg_wait_event_tracing header"
#define PWET_SERVER_REGION_NAME "pg_wait_event_tracing server processes"
#define PWET_STATS_DSA_NAME "pg_wait_event_tracing_stats"
#define PWET_NUM_SLOTS (MaxBackends + NUM_AUXILIARY_PROCS)

/*
 * "6" in plan section 4.2a's R = [MaxConnections, MaxBackends + 6 +
 * io_max_workers): the auxiliary process types other than I/O workers
 * (checkpointer, background writer, WAL writer, WAL summarizer, archiver,
 * startup process, WAL receiver -- proc.h's own comment on
 * NUM_AUXILIARY_PROCS explains why 6 of these overlapping-lifetime slots
 * suffice).  Expressed from proc.h's constants, not as a literal, so it
 * tracks NUM_AUXILIARY_PROCS/MAX_IO_WORKERS if they ever change.
 */
#define PWET_NON_IO_AUX_PROCS (NUM_AUXILIARY_PROCS - MAX_IO_WORKERS)
#define PWET_HISTOGRAM_BUCKETS 32
#define PWET_IDX_LWLOCK (-2)
#define PWET_LWLOCK_EMPTY ((uint16) 0xFFFF)
#define PWET_WAIT_EVENT_CLASS_MASK 0xFF000000U
#define PWET_WAIT_EVENT_ID_MASK 0x0000FFFFU
#define PWET_LWLOCK_PROBE_LIMIT 8
#define PWET_HAS_STATS_PRIVS(role) \
	(has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS) || \
	 has_privs_of_role(GetUserId(), role))

/*
 * Reserved trace_state values.  The trace-level patch adds ACTIVE and
 * ORPHANED; this module only ever produces FREE.
 */
#define PWET_TRACE_FREE 0

typedef enum PwetCaptureLevel
{
	PWET_CAPTURE_OFF = 0,
	PWET_CAPTURE_STATS,
} PwetCaptureLevel;

typedef struct PwetTimingEntry
{
	int64		count;
	int64		total_ns;
	int64		max_ns;
	int64		histogram[PWET_HISTOGRAM_BUCKETS];
} PwetTimingEntry;

typedef struct PwetLWLockHashEntry
{
	uint16		tranche_id;
	uint16		dense_idx;
} PwetLWLockHashEntry;

typedef struct PwetLWLockHash
{
	int			num_used;
	int			hash_size;
	int			max_entries;
} PwetLWLockHash;

/*
 * Per-backend statistics payload, allocated in the stats DSA area only for
 * a backend that has enabled capture.  Fixed-class entries are followed by
 * the runtime-sized LWLock hash and its entry array.
 *
 * The in-flight wait (which event, and when it started) is NOT part of
 * this payload: it lives in the process-local statics pwet_wait_start/
 * pwet_current_event instead, because it is written by the owning backend
 * only and read by nobody else -- no cross-backend reader ever looked at
 * it (pg_stat_get_wait_event_timing() and friends only ever copy
 * events/lwlock_hash/the overflow counters/reset_count out of a payload;
 * see pwet_emit_timing_row() and pg_stat_get_wait_event_timing_overflow()).
 * Keeping it here would just be extra bytes in a struct that is, for the
 * fixed-region case, memcpy'd whole on every lock-free read.
 */
typedef struct PwetStats
{
	int64		reset_count;
	PwetTimingEntry events[PWET_NUM_EVENTS];
	PwetLWLockHash lwlock_hash;
	int64		lwlock_overflow_count;
	int64		flat_overflow_count;
} PwetStats;

/*
 * One entry per possible ProcNumber, always resident in the control
 * segment.  trace_ptr/trace_state are unused placeholders reserved for the
 * trace-level patch.
 *
 * owner_pid/owner_start identify the process that currently owns the
 * payload (stats_ptr for a client backend; the matching slice of the fixed
 * server-process region -- see below -- for a server-side process): every
 * reader compares them against the live PgBackendStatus entry for this
 * ProcNumber and ignores the slot on a mismatch, so a successor that has
 * not (yet) claimed its own payload never gets attributed a predecessor's
 * counters.
 *
 * A client backend publishes owner_pid/owner_start under pwet_lock,
 * alongside stats_ptr; a server-side process instead publishes them
 * lock-free from its begin hook (see pwet_claim_fixed_slot()), since the
 * hook may not take a lock.  reset_generation lives here, not in the
 * payload itself, so that a reset request (always published under
 * pwet_lock, regardless of which kind of slot it targets -- see
 * pwet_request_reset()) can be tied atomically to the owner token that
 * authorizes it, and so the owning process can later notice it with a
 * lock-free read (see pwet_wait_end()).
 */
typedef struct PwetSlot
{
	dsa_pointer stats_ptr;		/* InvalidDsaPointer when not collecting */
	dsa_pointer trace_ptr;		/* reserved for the trace level */
	uint8		trace_state;	/* reserved for the trace level */
	int			owner_pid;		/* 0 when unowned */
	TimestampTz owner_start;	/* MyStartTimestamp of the owner */
	pg_atomic_uint32 generation;	/* bumped on every ownership change */
	pg_atomic_uint32 reset_generation; /* bumped by a reset request */
} PwetSlot;

/*
 * A small, always-allocated (regardless of capture) record of the one
 * decision that can only be made once, by whichever process creates
 * shared memory: whether the server-process region was requested, and
 * if so, its bounds.  shmem_request_hook decides this from pwet_capture
 * at postmaster start, but shmem_startup_hook -- which is what actually
 * opens the region -- also runs in every EXEC_BACKEND child, potentially
 * long after a reload has changed pwet_capture to something else.  A
 * child must never re-derive the decision from its own (possibly
 * reloaded) pwet_capture; it has to read what the postmaster actually
 * decided and reserved, from here.
 */
typedef struct PwetRegionHeader
{
	bool		server_region_present;
	int			server_region_start;
	int			server_region_end;
} PwetRegionHeader;

static const struct config_enum_entry pwet_capture_options[] = {
	{"off", PWET_CAPTURE_OFF, false},
	{"stats", PWET_CAPTURE_STATS, false},
	{NULL, 0, false}
};

static int	pwet_capture = PWET_CAPTURE_OFF;
static int	pwet_max_tranches = 192;

/*
 * guc.c's set_config_with_handle() calls a PGC_ENUM variable's assign_hook
 * BEFORE storing the new value (assign_hook(newval, newextra) precedes
 * *conf->variable = newval), so pwet_capture is still the *old* value for
 * the whole duration of pwet_assign_capture().  A client backend hides
 * this: pwet_maybe_attach() also runs from post_parse_analyze_hook /
 * ExecutorStart_hook on the next statement, by which point pwet_capture
 * has long since been updated.  A server-side process has no "next
 * statement": with the reserved region absent it depends entirely on the
 * assign hook's own synchronous pwet_maybe_attach() call to attach via
 * DSA, and with the region present a released fixed slot depends on
 * pwet_wait_begin() re-claiming -- which does not go through
 * pwet_can_attach() at all, but the DSA fallback does, and a stale
 * pwet_capture there would make pwet_can_attach() see the old (often OFF)
 * value and refuse forever, since nothing else ever retries it for such a
 * process.  pwet_capture_effective mirrors pwet_capture except that
 * pwet_assign_capture() updates it first, so pwet_can_attach() -- the
 * only place this matters -- always sees the value capture is *becoming*.
 * The begin/end hooks deliberately keep testing pwet_capture itself (see
 * pwet_wait_begin()/pwet_wait_end()), so recording never starts or stops
 * based on a value that has not actually taken effect yet.
 */
static int	pwet_capture_effective = PWET_CAPTURE_OFF;

/* The control table: an array of PWET_NUM_SLOTS PwetSlots, nothing else. */
static PwetSlot *pwet_ctl;
static LWLock *pwet_lock;
static dsa_area *pwet_stats_dsa;

/*
 * Set by pwet_shmem_request() from pwet_capture, in the postmaster only,
 * immediately before conditionally requesting the region's bytes; read
 * back by pwet_shmem_startup() in that same process, immediately after,
 * to decide whether to record the region as present in the header (see
 * PwetRegionHeader).  Meaningless in any other process: an EXEC_BACKEND
 * child never calls shmem_request_hook at all (only the postmaster does,
 * once, before shared memory exists), so this stays at its unused
 * default there -- which is fine, since a child reads presence/bounds
 * from the header, never from this.
 */
static bool pwet_region_requested;

/*
 * The reserved server-process region (plan section 4.2a).  NULL unless
 * the header (see PwetRegionHeader) records it as present -- which the
 * header can only ever say if capture was already non-off in the
 * configuration at postmaster start (see pwet_shmem_request()/
 * pwet_shmem_startup()).  [pwet_server_region_start,
 * pwet_server_region_end) is R, a sub-range of ProcNumbers, read from the
 * header the same way; pwet_server_stride is the byte size of one
 * process's slice, computed once (from pwet_max_tranches, a
 * PGC_POSTMASTER GUC) at the same time as the region itself and never
 * recomputed, so every process addresses the region the same way it was
 * originally sized.
 */
static char *pwet_server_region;
static int	pwet_server_region_start;
static int	pwet_server_region_end;
static Size pwet_server_stride;

static PwetStats *pwet_my_stats;
static ProcNumber pwet_my_procno = INVALID_PROC_NUMBER;
static Size pwet_stats_stride;
static uint32 pwet_last_reset_generation;

/*
 * The owning backend's in-flight wait: written only by that backend's own
 * pwet_wait_begin_impl()/pwet_wait_end_impl(), and read by nobody else --
 * confirmed by grepping every ->wait_start/->current_event site before
 * this pair replaced PwetStats.wait_start/PwetStats.current_event (the
 * cross-backend readers, pg_stat_get_wait_event_timing() and friends,
 * copy only the events/lwlock_hash/overflow/reset_count parts of the
 * payload -- see pwet_emit_timing_row() and the overflow SRF -- never
 * these two).  That made them safe to move out of the DSA/fixed-region
 * payload into ordinary process-local statics: there was never a second
 * reader to keep in sync with the payload's memory, and moving them
 * shrinks every stats payload by sizeof(instr_time) + sizeof(uint32)
 * without changing any observable memory bound (t/001_memory.pl's checks
 * are all footprint *inequalities*, not exact sizes).
 */
static instr_time pwet_wait_start;
static uint32 pwet_current_event;

/*
 * The one-slot, backend-local pending-accounting buffer (v11 patch 0004
 * fixup; see DECISION-deferred-accounting.md).  pwet_wait_end_impl() no
 * longer does the count/total/max/histogram update and trace append
 * itself: doing that inline made every wait_end() call, including one
 * returning from inside LWLockAcquire()'s critical section, pay a
 * shared-payload cache miss and (in trace mode) a seqlock append before
 * the caller could proceed -- for a contended lock, every queued waiter
 * pays that too, which is what made W3's collector overhead 40x its
 * additive estimate. Instead, wait_end_impl() only reads the clock,
 * computes the duration, and stashes (event, duration, timestamp) here;
 * pwet_flush_pending() later runs the exact same accounting the old
 * inline code did, against the STORED values, from a point where the
 * cost no longer extends any lock's hold time -- see its own comment for
 * the complete list of call sites and why each is safe/required.
 *
 * Single-slot, not a queue: at most one wait is ever in flight per
 * backend (pwet_wait_start/pwet_current_event above), so at most one
 * record is ever pending, and pwet_wait_begin_impl() flushes it before
 * timing the next wait -- see pwet_flush_pending()'s comment.
 *
 * trace records pwet_rec_trace's answer at wait_end time (see
 * pwet_wait_end_impl()): whether to append this wait to the trace ring
 * is a RECORDING decision, made once, at wait_end, exactly like it was
 * before this deferral existed -- pwet_flush_pending() later obeys that
 * decision, together with a defensive check that the ring itself is
 * still there to append to; it never re-derives the decision from
 * whatever pwet_rec_trace happens to answer at the later flush.
 */
typedef struct PwetPendingWait
{
	uint32		event;
	int64		duration_ns;
	int64		timestamp_ns;	/* the wait_end clock read, stored verbatim */
	bool		trace;			/* pwet_rec_trace != NULL, at wait_end time */
} PwetPendingWait;

static PwetPendingWait pwet_pending;
static bool pwet_pending_valid;

static bool pwet_active;
static bool pwet_attach_needed;
static bool pwet_exit_started;
static bool pwet_stats_writes_disabled;
static bool pwet_exit_callback_registered;

/*
 * The recording gate for the stats level, maintained by
 * pwet_update_rec_pointers() from the inputs above (plus
 * pwet_capture_effective): non-NULL if and only if this backend should
 * actually record right now.  pwet_wait_begin_impl()/pwet_wait_end_impl()
 * test only this, instead of re-deriving the same three-way test (capture
 * level, the writes-disabled flag, and the payload pointer) on every
 * wait; see pwet_update_rec_pointers()'s own comment for the exact
 * equivalence and why it is safe.
 */
static PwetStats *pwet_rec_stats;

/*
 * Per-process, computed at most once per process (see pwet_wait_begin()):
 * does MyProcNumber fall inside the reserved server-process region?
 * Cached because the answer can't change over a process's lifetime, and
 * the begin/end hooks run on every wait event.
 *
 * Keyed by MyProcPid, not a bare "have we ever checked" bool, because a
 * bare bool would survive fork() into every child with whatever value it
 * had in the parent -- and the *postmaster* also calls the begin hook
 * (its ServerLoop waits through the same WaitEventSetWait() timed pair),
 * with MyProcNumber == INVALID_PROC_NUMBER, so it would cache
 * eligible=false once, permanently, for itself; every child forked
 * afterwards -- checkpointer, background writer, WAL writer, every
 * ordinary backend -- inherits that exact memory image via fork() and
 * would see the cache already "checked", never re-deriving its own real
 * answer from its own MyProcNumber.  (An EXEC_BACKEND child does not
 * have this problem: it starts from a fresh, zeroed image, not a forked
 * copy, which is why this bug was invisible on Windows.)  Keying to
 * MyProcPid makes every process -- forked or exec'd -- recompute on its
 * own first call, since no live process shares another live process's
 * pid.
 */
static int	pwet_fixed_slot_checked_pid;
static bool pwet_fixed_slot_eligible;

static wait_event_hook_type prev_wait_event_begin_hook;
static wait_event_hook_type prev_wait_event_end_hook;

/*
 * Whether this process has installed the wait-event hooks for itself yet
 * (see pwet_install_wait_hooks()).  Every process in the cluster runs
 * _PG_init(), but the hooks are process-local globals, so a process that
 * never turns capture on never has to pay their indirect-call overhead on
 * every timed wait -- see pwet_install_wait_hooks()'s own comment for the
 * installation rule and why the hooks are never removed again.
 */
static bool pwet_wait_hooks_installed;

static post_parse_analyze_hook_type prev_post_parse_analyze_hook;
static ExecutorStart_hook_type prev_ExecutorStart_hook;
static shmem_request_hook_type prev_shmem_request_hook;
static shmem_startup_hook_type prev_shmem_startup_hook;

static void pwet_wait_begin(uint32 wait_event_info);
static void pwet_wait_begin_nochain(uint32 wait_event_info);
static void pwet_wait_end(uint32 wait_event_info);
static void pwet_wait_end_nochain(uint32 wait_event_info);
static void pwet_flush_pending(void);
static void pwet_install_wait_hooks(void);
static void pwet_update_rec_pointers(void);
static pg_always_inline void pwet_maybe_attach(void);
static void pwet_maybe_attach_slow(void);
static bool pwet_ensure_stats_dsa(void);
static void pwet_release_stats(void);
static void pwet_before_shmem_exit(int code, Datum arg);
static void pwet_request_reset(int procnumber, int target_pid,
							   TimestampTz target_start);
static void pwet_check_reset_privileges(Oid target_role);
static bool pwet_is_fixed_procnumber(int procnumber);
static PwetStats *pwet_fixed_payload(int procnumber);
static void pwet_claim_fixed_slot(void);
static void pwet_release_fixed_slot(void);

static Size
pwet_control_size(int nslots)
{
	return mul_size(nslots, sizeof(PwetSlot));
}

static int
pwet_hash_size_for(int max_entries)
{
	int			size = 32;

	while (size < max_entries * 2)
		size <<= 1;
	return size;
}

static Size
pwet_stats_payload_size(int max_entries)
{
	int			hash_size = pwet_hash_size_for(max_entries);

	return add_size(sizeof(PwetStats),
					add_size(mul_size(hash_size,
									  sizeof(PwetLWLockHashEntry)),
							 mul_size(max_entries,
									  sizeof(PwetTimingEntry))));
}

static inline PwetLWLockHashEntry *
pwet_lwlock_hash_entries(PwetStats *state)
{
	return (PwetLWLockHashEntry *)
		((char *) state + sizeof(PwetStats));
}

static inline PwetTimingEntry *
pwet_lwlock_hash_events(PwetStats *state)
{
	return (PwetTimingEntry *)
		((char *) state + sizeof(PwetStats) +
		 (Size) state->lwlock_hash.hash_size *
		 sizeof(PwetLWLockHashEntry));
}

static void
pwet_lwlock_hash_clear(PwetStats *state)
{
	PwetLWLockHash *hash = &state->lwlock_hash;
	PwetLWLockHashEntry *entries = pwet_lwlock_hash_entries(state);
	PwetTimingEntry *events = pwet_lwlock_hash_events(state);
	int			i;

	hash->num_used = 0;
	memset(events, 0,
		   (Size) hash->max_entries * sizeof(PwetTimingEntry));
	for (i = 0; i < hash->hash_size; i++)
	{
		entries[i].tranche_id = PWET_LWLOCK_EMPTY;
		entries[i].dense_idx = 0;
	}
}

static PwetTimingEntry *
pwet_lwlock_lookup(PwetStats *state, uint16 tranche_id)
{
	PwetLWLockHash *hash = &state->lwlock_hash;
	PwetLWLockHashEntry *entries = pwet_lwlock_hash_entries(state);
	PwetTimingEntry *events = pwet_lwlock_hash_events(state);
	uint32		hash_value = (uint32) tranche_id * 2654435761U;
	int			slot = hash_value & (hash->hash_size - 1);
	int			limit;
	int			i;

	limit = hash->num_used >= hash->max_entries
		? PWET_LWLOCK_PROBE_LIMIT : hash->hash_size;

	for (i = 0; i < limit; i++)
	{
		PwetLWLockHashEntry *entry = &entries[slot];

		if (entry->tranche_id == tranche_id)
			return &events[entry->dense_idx];

		if (entry->tranche_id == PWET_LWLOCK_EMPTY)
		{
			if (hash->num_used >= hash->max_entries)
				return NULL;

			entry->tranche_id = tranche_id;
			entry->dense_idx = hash->num_used++;
			return &events[entry->dense_idx];
		}

		slot = (slot + 1) & (hash->hash_size - 1);
	}

	return NULL;
}

static int
pwet_timing_index(uint32 wait_event_info)
{
	uint32		class_id = wait_event_info & PWET_WAIT_EVENT_CLASS_MASK;
	int			event_id = wait_event_info & PWET_WAIT_EVENT_ID_MASK;
	int			class_byte;
	int			dense;

	if (class_id == PG_WAIT_LWLOCK)
		return PWET_IDX_LWLOCK;

	class_byte = class_id >> 24;
	if (class_byte >= PWET_RAW_CLASSES)
		return -1;

	dense = pwet_class_dense[class_byte];
	if (dense < 0 || event_id >= pwet_class_nevents[dense])
		return -1;

	return pwet_class_offset[dense] + event_id;
}

static int
pwet_timing_bucket(int64 duration_ns)
{
	int			bucket;

	if (duration_ns < 1024)
		return 0;

	bucket = pg_leftmost_one_pos64((uint64) duration_ns) - 9;
	if (bucket >= PWET_HISTOGRAM_BUCKETS)
		bucket = PWET_HISTOGRAM_BUCKETS - 1;
	return bucket;
}

/* Initialize a freshly created control table: mark every slot empty. */
static void
pwet_control_init(PwetSlot *slots)
{
	int			i;

	for (i = 0; i < PWET_NUM_SLOTS; i++)
	{
		slots[i].stats_ptr = InvalidDsaPointer;
		slots[i].trace_ptr = InvalidDsaPointer;
		slots[i].trace_state = PWET_TRACE_FREE;
		slots[i].owner_pid = 0;
		slots[i].owner_start = 0;
		pg_atomic_init_u32(&slots[i].generation, 0);
		pg_atomic_init_u32(&slots[i].reset_generation, 0);
	}
}

/*
 * Compute R = [start, end), the sub-range of ProcNumbers server-side
 * processes can occupy (plan section 4.2a).  Layout, verified on this
 * master's proc.c (ProcGlobalShmemInit()): ProcNumbers are handed out in
 * one array, [0, MaxConnections) client backends first, then autovacuum
 * launcher/workers and the special workers
 * (autovacuum_worker_slots + NUM_SPECIAL_WORKER_PROCS), then background
 * workers -- which include parallel query workers and logical replication
 * workers -- (max_worker_processes), then WAL senders (max_wal_senders),
 * ending at MaxBackends; then auxiliary processes fill
 * [MaxBackends, MaxBackends + NUM_AUXILIARY_PROCS) on a first-free linear
 * search (InitAuxiliaryProcess()), not by type, so with at most
 * PWET_NON_IO_AUX_PROCS + io_max_workers of them concurrently alive their
 * ProcNumbers never reach MaxBackends + PWET_NON_IO_AUX_PROCS +
 * io_max_workers.  io_max_workers is PGC_SIGHUP: if it is raised by a
 * reload after postmaster start, workers beyond the region reserved here
 * fall back to the DSA path once they reach a safe point (see
 * pwet_can_attach()) -- this only shrinks the fixed-slot coverage, it does
 * not let any process write outside the reserved bytes, since eligibility
 * is decided against this stored range, not against "is this any kind of
 * server-side process".  The clamp to MaxBackends + NUM_AUXILIARY_PROCS
 * is therefore just defense in depth (io_max_workers's own GUC bound
 * already keeps it <= MAX_IO_WORKERS).
 */
static void
pwet_compute_server_region(int *start, int *end)
{
	int			raw_end = MaxBackends + PWET_NON_IO_AUX_PROCS + io_max_workers;
	int			hard_max = MaxBackends + NUM_AUXILIARY_PROCS;

	*start = MaxConnections;
	*end = Min(raw_end, hard_max);
}

/*
 * shmem_request_hook: request the always-resident control table, its
 * LWLock tranche, the (also always-resident) region header, and -- only
 * if capture is already configured on -- the reserved server-process
 * region.
 *
 * MaxBackends and every GUC referenced by pwet_compute_server_region() are
 * final by the time this runs, and pwet_capture already reflects
 * postgresql.conf: postmaster.c calls, in order, SelectConfigFiles()
 * (loads the config file), process_shared_preload_libraries() (runs every
 * library's _PG_init(), including this one -- DefineCustomEnumVariable()
 * applies any config-file value for pg_wait_event_tracing.capture right
 * then), InitializeMaxBackends(), and only then process_shmem_requests()
 * (which calls this hook).  Verified by reading postmaster.c directly,
 * not inferred.
 *
 * R is never actually empty (MaxConnections is always < MaxBackends +
 * PWET_NON_IO_AUX_PROCS + io_max_workers), so whether the region's bytes
 * get requested here depends entirely on pwet_region_requested, i.e. on
 * pwet_capture -- never on R's size.
 */
static void
pwet_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pwet_control_size(PWET_NUM_SLOTS));
	RequestNamedLWLockTranche(PWET_CONTROL_NAME, 1);
	RequestAddinShmemSpace(sizeof(PwetRegionHeader));

	pwet_region_requested = (pwet_capture != PWET_CAPTURE_OFF);
	if (pwet_region_requested)
	{
		int			start,
					end;

		pwet_compute_server_region(&start, &end);
		RequestAddinShmemSpace(mul_size(end - start,
										pwet_stats_payload_size(pwet_max_tranches)));
	}
}

/*
 * shmem_startup_hook: create or attach the control table, the region
 * header, and, if the header says so, the server-process region.
 *
 * Runs once in the postmaster (CreateSharedMemoryAndSemaphores()) and,
 * under EXEC_BACKEND, again in every child (AttachSharedMemoryStructs()) --
 * verified in ipci.c, which calls shmem_startup_hook from both places, the
 * same way pg_stat_statements relies on it to re-derive its own statics in
 * every child.  pwet_ctl/pwet_lock/pwet_server_region are plain
 * process-local pointers into shared memory, not stored in shared memory
 * themselves, so each EXEC_BACKEND child must (and does) recompute them
 * here; a fork()-based child instead simply inherits them from the
 * postmaster.
 *
 * Whether the region exists, and its bounds, are decided exactly once,
 * by whichever process creates the header (necessarily the postmaster,
 * since EXEC_BACKEND children only ever attach to already-created shared
 * memory): !found below is true only then, and only there do we consult
 * pwet_region_requested/pwet_compute_server_region() at all.  Every other
 * call -- an EXEC_BACKEND child, or a later re-entry -- finds the header
 * already populated and just reads it.  This is required, not just
 * simpler: a child re-running _PG_init() (and so redefining pwet_capture
 * from whatever the config currently says, which can differ from its
 * value at postmaster start if a reload happened in between) must not be
 * able to change whether the region is treated as present -- the region
 * itself was only actually allocated if the *original* decision, recorded
 * here, was to request it.
 */
static void
pwet_shmem_startup(void)
{
	bool		found;
	PwetRegionHeader *hdr;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	pwet_ctl = NULL;
	pwet_server_region = NULL;

	pwet_lock = &(GetNamedLWLockTranche(PWET_CONTROL_NAME))->lock;

	pwet_ctl = (PwetSlot *) ShmemInitStruct(PWET_CONTROL_STRUCT_NAME,
											pwet_control_size(PWET_NUM_SLOTS),
											&found);
	if (!found)
		pwet_control_init(pwet_ctl);

	hdr = (PwetRegionHeader *) ShmemInitStruct(PWET_REGION_HEADER_NAME,
											   sizeof(PwetRegionHeader),
											   &found);
	if (!found)
	{
		/* We are the postmaster, creating this for the first time. */
		hdr->server_region_present = pwet_region_requested;
		if (pwet_region_requested)
			pwet_compute_server_region(&hdr->server_region_start,
									   &hdr->server_region_end);
		else
		{
			hdr->server_region_start = 0;
			hdr->server_region_end = 0;
		}
	}

	pwet_server_region_start = hdr->server_region_start;
	pwet_server_region_end = hdr->server_region_end;

	if (hdr->server_region_present)
	{
		pwet_server_stride = pwet_stats_payload_size(pwet_max_tranches);
		pwet_server_region = (char *) ShmemInitStruct(PWET_SERVER_REGION_NAME,
													  mul_size(pwet_server_region_end -
															   pwet_server_region_start,
															   pwet_server_stride),
													  &found);
		/* ShmemInitStruct()'s underlying allocation is zeroed on creation. */
	}
}

/*
 * Lazily attach this backend to the stats DSA area.  GetNamedDSA() manages
 * its own tranche and creation lock; we only need to remember the result.
 */
static bool
pwet_ensure_stats_dsa(void)
{
	bool		found;

	if (pwet_stats_dsa != NULL)
		return true;

	pwet_stats_dsa = GetNamedDSA(PWET_STATS_DSA_NAME, &found);
	return pwet_stats_dsa != NULL;
}

/*
 * Is procnumber inside the reserved server-process region, with the
 * region actually present?  (It exists only when capture was already
 * configured on at postmaster start; see pwet_shmem_request().)  A
 * ProcNumber failing this check always means "not eligible for the fixed
 * path right now", never "out of bounds": every caller already knows
 * procnumber < PWET_NUM_SLOTS from other bounds checks.
 */
static bool
pwet_is_fixed_procnumber(int procnumber)
{
	return pwet_server_region != NULL &&
		procnumber >= pwet_server_region_start &&
		procnumber < pwet_server_region_end;
}

/* Address of procnumber's slice of the server-process region. */
static PwetStats *
pwet_fixed_payload(int procnumber)
{
	Assert(pwet_is_fixed_procnumber(procnumber));
	return (PwetStats *) (pwet_server_region +
						  (Size) (procnumber - pwet_server_region_start) *
						  pwet_server_stride);
}

static bool
pwet_can_attach(void)
{
	/* See pwet_capture_effective's comment for why this, not pwet_capture. */
	if (pwet_exit_started || !pwet_active ||
		pwet_capture_effective == PWET_CAPTURE_OFF)
		return false;
	if (MyProc == NULL || MyProcNumber == INVALID_PROC_NUMBER)
		return false;
	if (MyProcNumber < 0 || MyProcNumber >= PWET_NUM_SLOTS)
		return false;

	/*
	 * A ProcNumber inside the reserved region never takes the DSA path
	 * while that region exists, even before this process has claimed its
	 * fixed slot: pwet_attach_stats() would publish stats_ptr under
	 * pwet_lock and point pwet_my_stats at the DSA payload, but readers
	 * for a ProcNumber in R always consult the fixed region instead (see
	 * pwet_is_fixed_procnumber() call sites), so anything recorded there
	 * would silently never be shown.  When the region does not exist
	 * (capture was off at postmaster start), this is unreachable and
	 * today's DSA-at-next-reload behaviour is unchanged.
	 */
	if (pwet_is_fixed_procnumber(MyProcNumber))
		return false;

	if (!IsNormalProcessingMode() || CritSectionCount > 0)
		return false;
	if (MyProc->lwWaiting != LW_WS_NOT_WAITING)
		return false;
	return true;
}

/*
 * Recompute pwet_rec_stats, the single recording-gate pointer for the
 * stats level, from its three inputs.  Called at every site that assigns
 * any of pwet_capture_effective, pwet_stats_writes_disabled, or
 * pwet_my_stats (find them all with:
 *   grep -n 'pwet_stats_writes_disabled =\|pwet_my_stats =\|pwet_capture_effective ='
 * ), so the pointer is never stale by the time pwet_wait_begin_impl()/
 * pwet_wait_end_impl() next read it.
 *
 * pwet_rec_stats is non-NULL exactly when a test against the STORED
 * pwet_capture -- (pwet_capture != OFF, !pwet_stats_writes_disabled,
 * pwet_my_stats != NULL) -- would answer yes.  This function computes it
 * from pwet_capture_effective, the "becoming" value, not from
 * pwet_capture, the stored one -- deliberately, so the assign hook
 * itself, which sets pwet_capture_effective before guc.c stores the new
 * value, can call this function and get the right in-progress answer,
 * exactly like every other attach decision (see pwet_capture_effective's
 * own comment).
 *
 * Outside the assign hook's own synchronous call chain,
 * pwet_capture_effective == pwet_capture always (guc.c has, by then,
 * finished storing the value), so substituting one for the other changes
 * nothing there.  INSIDE that chain, the two genuinely differ, and this
 * is NOT merely academic: pwet_wait_begin_impl()/pwet_wait_end_impl() can
 * run synchronously from inside it, because pwet_maybe_attach() (called
 * from pwet_assign_capture() when capture is becoming non-off) reaches
 * pwet_attach_stats(), which takes an LWLock to publish the new payload --
 * itself a timed wait.  Naively deriving the gate from
 * pwet_capture_effective there would start recording mid-assign-hook,
 * before the SET has actually taken effect (e.g. "off -> stats" would
 * count the attach's own LWLock wait).  pwet_assign_capture() is what
 * keeps this function's answer correct in that window too: it masks
 * pwet_stats_writes_disabled, for its own duration only, to reproduce
 * exactly what testing the STORED value would have answered (see its own
 * comment for the precise rule and why).  This function does not need to
 * know, from its inputs alone, whether it is being called from inside
 * that masked window -- the masking is what makes the answer right
 * either way.
 *
 * The set of recorded waits is therefore unchanged, byte for byte, in
 * both cases: outside the chain by the pwet_capture_effective ==
 * pwet_capture identity, and inside it by the assign hook's masking.
 */
static void
pwet_update_rec_pointers(void)
{
	pwet_rec_stats = (pwet_capture_effective != PWET_CAPTURE_OFF &&
					  !pwet_stats_writes_disabled &&
					  pwet_my_stats != NULL)
		? pwet_my_stats : NULL;
}

/*
 * Claim this process's slot in the reserved server-process region, from
 * inside the begin hook (plan section 4.2a's claim protocol).  Called at
 * most once per process (see pwet_wait_begin()'s cached eligibility
 * check), so there is never a second live process contending for the same
 * slot concurrently -- the previous occupant, if any, is long gone by the
 * time a ProcNumber is reused.  The only concurrent observers are the
 * lock-free readers (pg_stat_get_wait_event_timing() and friends), which
 * is why ownership is published in the exact order below rather than in
 * one step.
 *
 * Obeys the hook rules: no allocation, no lock, no wait, no ereport --
 * only plain loads/stores, one memset on already-mapped fixed shared
 * memory, and write barriers.
 */
static void
pwet_claim_fixed_slot(void)
{
	PwetSlot   *slot = &pwet_ctl[MyProcNumber];
	PwetStats  *payload = pwet_fixed_payload(MyProcNumber);
	bool		same_owner;

	same_owner = (slot->owner_pid == MyProcPid &&
				  slot->owner_start == MyStartTimestamp);

	/* (1) Unpublish before touching anything a reader might be copying. */
	slot->owner_pid = 0;
	pg_write_barrier();

	/* (2) Fresh owner: reset the payload exactly as a new DSA slot starts. */
	if (!same_owner)
	{
		int			hash_size = pwet_hash_size_for(pwet_max_tranches);
		PwetLWLockHashEntry *entries;
		int			i;

		memset(payload, 0, pwet_server_stride);
		payload->lwlock_hash.num_used = 0;
		payload->lwlock_hash.hash_size = hash_size;
		payload->lwlock_hash.max_entries = pwet_max_tranches;
		entries = pwet_lwlock_hash_entries(payload);
		for (i = 0; i < hash_size; i++)
			entries[i].tranche_id = PWET_LWLOCK_EMPTY;
	}

	/* (3) Publish the new owner: start timestamp first, pid last. */
	slot->owner_start = MyStartTimestamp;
	pg_write_barrier();
	slot->owner_pid = MyProcPid;

	/*
	 * Bumped on every ownership change (section 4.1), same as the DSA
	 * attach path; nothing reads this yet, but pg_atomic_fetch_add_u32()
	 * is a plain atomic op, allowed in the hook.
	 */
	pg_atomic_fetch_add_u32(&slot->generation, 1);

	/* (4) Cache the pointer the hooks use. */
	pwet_my_stats = payload;
	pwet_my_procno = MyProcNumber;
	pwet_last_reset_generation = pg_atomic_read_u32(&slot->reset_generation);
	pwet_update_rec_pointers();
}

/*
 * Stop writing to a claimed fixed slot (assign hook, capture -> off).
 * The region is never freed -- it is reserved for the process's entire
 * lifetime -- so this only withdraws ownership; the reader's beentry
 * check then makes the row disappear, matching the DSA release path's
 * user-visible effect.  Re-enabling capture re-claims at the next begin
 * hook (pwet_can_attach() already refuses the DSA path for this
 * ProcNumber, so pwet_maybe_attach() is a no-op here and
 * pwet_claim_fixed_slot() is what picks it back up).
 *
 * No lock: owner_pid/owner_start for a slot in the server region are only
 * ever written by the process that owns MyProcNumber, whether from the
 * begin hook or from here -- never by another backend, which only ever
 * touches reset_generation (under pwet_lock; see pwet_request_reset()).
 *
 * Flushes the pending wait, if any, before withdrawing ownership: once
 * pwet_my_stats is cleared below, pwet_flush_pending() would have nowhere
 * left to account it (see its own comment on the payload-gone case), so
 * this is the last chance to record it.
 */
static void
pwet_release_fixed_slot(void)
{
	pwet_flush_pending();
	pwet_my_stats = NULL;
	pwet_update_rec_pointers();
	if (pwet_my_procno != INVALID_PROC_NUMBER)
	{
		PwetSlot   *slot = &pwet_ctl[pwet_my_procno];

		slot->owner_pid = 0;
		/* Bumped on every ownership change (section 4.1), as on attach. */
		pg_atomic_fetch_add_u32(&slot->generation, 1);
	}
}

static bool
pwet_attach_stats(void)
{
	static bool in_attach;
	PwetSlot   *slot;
	PwetStats  *state = NULL;
	dsa_pointer stats_ptr = InvalidDsaPointer;

	if (pwet_my_stats != NULL)
		return true;
	if (in_attach || !pwet_can_attach())
		return false;

	/*
	 * pwet_ctl/pwet_lock are set up by pwet_shmem_startup() before any
	 * user code can run (the module requires shared_preload_libraries, so
	 * that hook always fires first); only the DSA payload area is created
	 * lazily, on demand, here.
	 */
	Assert(pwet_ctl != NULL && pwet_lock != NULL);

	in_attach = true;
	PG_TRY();
	{
		if (pwet_ensure_stats_dsa())
		{
			PwetLWLockHashEntry *entries;
			int			hash_size;
			int			i;

			pwet_stats_stride = pwet_stats_payload_size(pwet_max_tranches);
			hash_size = pwet_hash_size_for(pwet_max_tranches);
			stats_ptr = dsa_allocate_extended(pwet_stats_dsa,
											  pwet_stats_stride,
											  DSA_ALLOC_ZERO |
											  DSA_ALLOC_NO_OOM);
			if (DsaPointerIsValid(stats_ptr))
			{
				state = dsa_get_address(pwet_stats_dsa, stats_ptr);
				state->lwlock_hash.num_used = 0;
				state->lwlock_hash.hash_size = hash_size;
				state->lwlock_hash.max_entries = pwet_max_tranches;
				entries = pwet_lwlock_hash_entries(state);
				for (i = 0; i < hash_size; i++)
					entries[i].tranche_id = PWET_LWLOCK_EMPTY;

				slot = &pwet_ctl[MyProcNumber];
				LWLockAcquire(pwet_lock, LW_EXCLUSIVE);
				if (DsaPointerIsValid(slot->stats_ptr))
					dsa_free(pwet_stats_dsa, slot->stats_ptr);
				slot->stats_ptr = stats_ptr;
				slot->owner_pid = MyProcPid;
				slot->owner_start = MyStartTimestamp;
				pg_atomic_fetch_add_u32(&slot->generation, 1);
				pwet_last_reset_generation =
					pg_atomic_read_u32(&slot->reset_generation);
				LWLockRelease(pwet_lock);

				pwet_my_stats = state;
				pwet_my_procno = MyProcNumber;
				pwet_update_rec_pointers();
			}
		}
	}
	PG_FINALLY();
	{
		in_attach = false;
	}
	PG_END_TRY();

	return pwet_my_stats != NULL;
}

/*
 * The steady-state path reaches this from every parsed and executed query.
 * Keep its no-attachment-needed case to one backend-local flag test,
 * avoiding an out-of-line call into the attachment machinery.
 */
static pg_always_inline void
pwet_maybe_attach(void)
{
	if (pwet_attach_needed)
		pwet_maybe_attach_slow();
}

static void
pwet_maybe_attach_slow(void)
{
	if (!pwet_can_attach())
		return;

	if (!pwet_attach_stats())
		return;

	/*
	 * Registered lazily, per backend, the first time that backend actually
	 * attaches: on_exit_reset() (called early in every forked/exec'd
	 * backend, well before shared_preload_libraries processing happens
	 * again on EXEC_BACKEND, and inherited as a no-op on fork otherwise)
	 * would discard a registration made from _PG_init() in the postmaster,
	 * so this is the only place this can usefully happen.
	 */
	if (!pwet_exit_callback_registered)
	{
		before_shmem_exit(pwet_before_shmem_exit, (Datum) 0);
		pwet_exit_callback_registered = true;
	}

	pwet_attach_needed = false;
}

/*
 * Flushes the pending wait, if any, before the payload is freed: once
 * pwet_my_stats is cleared below, there is nowhere left to account it
 * (see pwet_flush_pending()'s comment on the payload-gone case).
 */
static void
pwet_release_stats(void)
{
	PwetSlot   *slot;
	ProcNumber	procno = pwet_my_procno;
	bool		was_disabled = pwet_stats_writes_disabled;

	pwet_flush_pending();

	if (pwet_my_stats == NULL || pwet_stats_dsa == NULL || pwet_ctl == NULL ||
		procno == INVALID_PROC_NUMBER)
	{
		pwet_my_stats = NULL;
		pwet_update_rec_pointers();
		return;
	}

	pwet_stats_writes_disabled = true;
	pwet_my_stats = NULL;
	pwet_update_rec_pointers();
	slot = &pwet_ctl[procno];

	LWLockAcquire(pwet_lock, LW_EXCLUSIVE);
	if (DsaPointerIsValid(slot->stats_ptr))
	{
		dsa_free(pwet_stats_dsa, slot->stats_ptr);
		slot->stats_ptr = InvalidDsaPointer;
		slot->owner_pid = 0;
		slot->owner_start = 0;
		pg_atomic_fetch_add_u32(&slot->generation, 1);
	}
	LWLockRelease(pwet_lock);

	if (!pwet_exit_started)
	{
		pwet_stats_writes_disabled = was_disabled;
		pwet_update_rec_pointers();
	}
}

static void
pwet_before_shmem_exit(int code, Datum arg)
{
	/*
	 * First thing, while pwet_my_stats/pwet_my_trace are both still
	 * attached and nothing below has touched either of them yet: the last
	 * pending wait, if any, gets both halves (stats and, if trace is
	 * active, the trace record) accounted here, exactly once, before exit
	 * cleanup begins.  pwet_orphan_trace()/pwet_release_stats() below each
	 * flush again on their own (every release/orphan site does; see their
	 * comments), which is harmless -- pwet_flush_pending() is a no-op once
	 * there is nothing pending -- but this call is what makes that true
	 * for both of them here, rather than leaving it to whichever runs
	 * first.
	 */
	pwet_flush_pending();
	pwet_exit_started = true;
	pwet_stats_writes_disabled = true;
	pwet_update_rec_pointers();
	pwet_release_stats();
	pwet_my_procno = INVALID_PROC_NUMBER;
}

static void
pwet_assign_capture(int newval, void *extra)
{
	int			old_stored = pwet_capture;
	bool		saved_stats_disabled = pwet_stats_writes_disabled;

	/*
	 * Flush the pending wait, if any, before anything below masks
	 * recording or releases/reattaches a payload: pwet_my_stats/
	 * pwet_my_trace are both still exactly what they were when the wait
	 * was stashed, so this is the last point before this function can
	 * change either of them out from under a still-pending record.
	 */
	pwet_flush_pending();

	/*
	 * Lazily install this process's own wait hooks the moment capture
	 * becomes non-off in it, before any attach logic below runs (that
	 * logic can itself run synchronously from here -- see
	 * pwet_maybe_attach() further down -- and the hooks must already be in
	 * place by the time any wait completes).  See
	 * pwet_install_wait_hooks() for why this never happens in _PG_init()
	 * instead, and why the hooks, once installed, are never removed.
	 */
	if (newval != PWET_CAPTURE_OFF)
		pwet_install_wait_hooks();

	/*
	 * Mask recording, for the rest of this function only, to exactly what
	 * old_stored (the value guc.c has NOT yet overwritten pwet_capture
	 * with) would have permitted -- even though pwet_rec_stats is computed
	 * from pwet_capture_effective, the "becoming" value set below, not
	 * old_stored.
	 *
	 * This matters because pwet_maybe_attach() below can run synchronously
	 * from inside this very function (see its own call further down), and
	 * pwet_attach_stats() takes an LWLock to publish the new payload --
	 * itself a timed wait, i.e. something pwet_wait_begin()/
	 * pwet_wait_end() can observe before this function returns.  Before
	 * pwet_rec_stats existed, the hot path tested pwet_capture directly,
	 * which guc.c does not store until AFTER this function returns (see
	 * the RULE comment on pwet_capture_effective): so for the whole
	 * duration of this function, the old code's recording gate saw
	 * old_stored, never newval, no matter what got attached in the
	 * meantime.  Deriving the gate from pwet_capture_effective instead
	 * (which THIS function sets to newval, below) would, without the mask
	 * here, start recording mid-function the instant an attach triggered
	 * by the incoming value completes -- e.g. off -> stats would count
	 * the attach's own LWLock wait in stats.  Masking reproduces the old
	 * gate's answer exactly: stats is masked only when old_stored == OFF,
	 * the one case where the old gate would have refused to record no
	 * matter what (pwet_capture == OFF outright); when old_stored is
	 * already STATS, a payload already exists and the old gate already
	 * counted this function's own waits under it.
	 *
	 * The window between this function returning and guc.c actually
	 * storing newval into pwet_capture contains no wait sites (nothing in
	 * set_config_with_handle() between the assign_hook call and the store
	 * waits on anything), so masking exactly the inside of this function,
	 * and restoring on every exit (below), is sufficient: the set of
	 * recorded waits is byte-for-byte identical to testing the stored
	 * pwet_capture throughout, exactly as before pwet_rec_stats existed.
	 * Deferred accounting (v11 patch 0004 fixup; see
	 * DECISION-deferred-accounting.md) does not change this: the recording
	 * decision for a wait is still taken here and in
	 * pwet_wait_end_impl(), at wait_end time, exactly as before -- only
	 * the bookkeeping itself, writing the counters, is deferred, never
	 * the decision of whether to.
	 */
	if (old_stored == PWET_CAPTURE_OFF)
		pwet_stats_writes_disabled = true;

	/*
	 * Update the "becoming" value first, before anything below can call
	 * pwet_can_attach() (see pwet_capture_effective's comment): guc.c has
	 * not yet stored newval into pwet_capture itself at this point.
	 */
	pwet_capture_effective = newval;
	pwet_update_rec_pointers();

	if (pwet_my_stats != NULL)
	{
		INSTR_TIME_SET_ZERO(pwet_wait_start);
		pwet_current_event = 0;
	}

	if (pwet_active && !pwet_exit_started)
	{
		if (newval == PWET_CAPTURE_OFF)
		{
			/*
			 * pwet_release_stats() only knows how to release a DSA payload
			 * (it checks slot->stats_ptr, which a fixed-slot owner never
			 * sets); a process holding a claimed fixed slot instead has
			 * pwet_fixed_slot_eligible set (see pwet_wait_begin()), and
			 * needs pwet_release_fixed_slot() to withdraw ownership from
			 * the control table.
			 */
			if (pwet_fixed_slot_eligible)
				pwet_release_fixed_slot();
			else
				pwet_release_stats();
			pwet_attach_needed = false;
		}
		else
		{
			pwet_attach_needed = true;
			/*
			 * Attach right away if this is a safe point; otherwise the
			 * post_parse_analyze/ExecutorStart hooks pick it up for a
			 * client backend, or the next begin hook re-claims for a
			 * server-side process (pwet_can_attach() refuses the DSA path
			 * for a ProcNumber in the reserved region, so
			 * pwet_maybe_attach() below is a no-op for those; see
			 * pwet_claim_fixed_slot()).
			 */
			if (IsNormalProcessingMode())
				pwet_maybe_attach();
		}
	}

	/*
	 * Unmask: restore pwet_stats_writes_disabled to what it was on entry,
	 * so the mask above is scoped to exactly this function's own duration,
	 * on every exit path (there is only this one).  pwet_release_stats()
	 * above may itself have already toggled this same flag true and back
	 * as part of its own release protocol; that nesting composes
	 * correctly because it restores to "whatever it saw on entry to
	 * itself", which by then is our masked value -- so after it returns,
	 * the flag is back to our masked value, and this restores it one
	 * level further out, to the value from before we masked it.  If this
	 * process is exiting, leave it true instead, matching
	 * pwet_before_shmem_exit() (which sets pwet_exit_started before this
	 * hook could even be reached again, but a defensive match costs
	 * nothing).
	 */
	if (pwet_exit_started)
		pwet_stats_writes_disabled = true;
	else
		pwet_stats_writes_disabled = saved_stats_disabled;
	pwet_update_rec_pointers();
}

/*
 * Flush the one pending completed-wait record, if any, running exactly the
 * accounting pwet_wait_end_impl() used to run inline before the deferred-
 * accounting change (v11 patch 0004 fixup; see
 * DECISION-deferred-accounting.md and pwet_pending's own comment for why).
 * Called at every point where ordering or a payload's/ring's lifetime
 * matters -- enumerated in full in the commit message, in outline here:
 *
 *   - pwet_wait_begin_impl(), before timing the next wait;
 *   - before every pwet_trace_write_marker() call site: post_parse_analyze,
 *     ExecutorStart/End, both of ProcessUtility's markers, the xact
 *     callback, and wait_begin's own Idle marker synthesis (already
 *     covered by wait_begin's own flush above, since nothing in between
 *     can create a new pending record, but restated there for the reader
 *     rather than relied on implicitly);
 *   - pwet_assign_capture(), before it masks recording or releases/
 *     reattaches anything;
 *   - pwet_release_stats(), pwet_release_fixed_slot(), pwet_release_trace(),
 *     pwet_orphan_trace(), before each does its work;
 *   - pwet_before_shmem_exit(), first thing;
 *   - pwet_reset_own(), before zeroing;
 *   - the SQL readers of the CALLING backend's own data:
 *     pg_get_backend_wait_event_trace(), the calling backend's own row in
 *     pg_stat_get_wait_event_timing()'s and
 *     pg_stat_get_wait_event_timing_overflow()'s sweeps, and
 *     pg_get_wait_event_trace() when given the caller's own procnumber
 *     (which covers pg_wait_event_trace_by_statement() too, since it is
 *     built on that function in the extension script) -- a cross-backend
 *     reader instead sees whatever the owning backend's own next flush
 *     point produced, the bounded latency the decision document accepts.
 *
 * Must obey the hook rules (no allocation, no lock, no wait, no ereport):
 * wait_begin_impl calls this from inside the hook itself, so the rule is
 * upheld unconditionally here, even though most other call sites above are
 * ordinary safe points that would not themselves require it.
 *
 * Both recording decisions for this record -- whether to count it in
 * stats at all, and whether to append it to the trace ring -- were
 * already made at wait_end time, exactly as they were before this
 * deferral existed: the record is only ever stashed when pwet_rec_stats
 * was non-NULL then (see pwet_wait_end_impl()), and pwet_pending.trace
 * records what pwet_rec_trace answered at that same instant.  This
 * function only carries those decisions out later; it never re-derives
 * either one from whatever pwet_rec_stats/pwet_rec_trace happen to
 * answer now, at flush time, which can differ from their wait_end-time
 * answer during pwet_assign_capture()'s own masked window (see its
 * comment) -- re-deriving there would let this later moment's masking,
 * meant for a different question (should a NEW wait be recorded right
 * now), silently override a decision already made for this one.
 *
 * The one exception is pwet_my_stats/pwet_my_trace themselves: a pending
 * record's payload or ring can have been genuinely released between
 * wait_end and this flush (which cannot happen on the normal path, since
 * every release/orphan site above flushes first) -- pwet_my_stats == NULL
 * drops the stats half, and pwet_my_trace == NULL drops the trace half
 * even when pwet_pending.trace is true, there being nowhere left to put
 * either one.
 */
static void
pwet_flush_pending(void)
{
	uint32		event;
	int64		duration_ns;
	int64		timestamp_ns;
	PwetStats  *state;

	if (!pwet_pending_valid)
		return;

	event = pwet_pending.event;
	duration_ns = pwet_pending.duration_ns;
	timestamp_ns = pwet_pending.timestamp_ns;
	pwet_pending_valid = false;

	/*
	 * Stats eligibility needs no separate field to check here: this
	 * record was only ever stashed by pwet_wait_end_impl() while
	 * pwet_rec_stats was non-NULL, so the recording decision is already
	 * implied by the record's mere existence -- only whether the payload
	 * is still there to write into (pwet_my_stats) remains to be checked.
	 */
	state = pwet_my_stats;
	if (state != NULL)
	{
		uint32		reset_generation;
		int			idx;
		PwetTimingEntry *entry = NULL;

		/*
		 * reset_generation lives in the always-mapped control slot, not the
		 * DSA payload (see pwet_request_reset()), so this is a plain
		 * lock-free atomic read: the owner is the only reader, and the
		 * requester only ever increments it under the control lock.  Same
		 * position in the sequence as the pre-deferral code: immediately
		 * before this record's own values are applied to state->events,
		 * i.e. the reset always lands before the record it would otherwise
		 * have been clobbered by is accounted.
		 */
		reset_generation =
			pg_atomic_read_u32(&pwet_ctl[pwet_my_procno].reset_generation);
		if (reset_generation != pwet_last_reset_generation)
		{
			memset(state->events, 0, sizeof(state->events));
			pwet_lwlock_hash_clear(state);
			state->reset_count++;
			state->lwlock_overflow_count = 0;
			state->flat_overflow_count = 0;
			pwet_last_reset_generation = reset_generation;
		}

		idx = pwet_timing_index(event);
		if (idx == PWET_IDX_LWLOCK)
			entry = pwet_lwlock_lookup(state, event & PWET_WAIT_EVENT_ID_MASK);
		else if (idx >= 0)
			entry = &state->events[idx];

		if (entry != NULL)
		{
			entry->count++;
			entry->total_ns += duration_ns;
			if (duration_ns > entry->max_ns)
				entry->max_ns = duration_ns;
			entry->histogram[pwet_timing_bucket(duration_ns)]++;
		}
		else if (idx == PWET_IDX_LWLOCK)
			state->lwlock_overflow_count++;
		else
			state->flat_overflow_count++;
	}

	/* No trace level yet at this point in the series. */
	(void) timestamp_ns;
}

/*
 * Body of the wait_event_begin_hook, shared by the chaining and
 * non-chaining wrappers below (pwet_install_wait_hooks() picks whichever
 * one applies to this process, once, at install time).  chain is always a
 * compile-time constant at each call site, so the inlined body has no
 * previous-hook NULL test on either path: pwet_install_wait_hooks() only
 * ever wires up the chain=true wrapper when prev_wait_event_begin_hook is
 * known non-NULL, so there is nothing to test here.
 */
static pg_always_inline void
pwet_wait_begin_impl(uint32 wait_event_info, bool chain)
{
	if (chain)
		prev_wait_event_begin_hook(wait_event_info);

	/*
	 * Flush the previous wait's pending record, if any, before this wait's
	 * own clock read below (see pwet_flush_pending()'s comment for the
	 * complete list of flush points): this is the primary one, what keeps
	 * the deferred accounting's cost out of the interval this wait itself
	 * measures, and, when the PREVIOUS wait ended inside a critical
	 * section (e.g. LWLockAcquire()'s), out of the caller's lock hold time
	 * too -- that caller has already returned from LWLockAcquire() by the
	 * time any NEW wait begins.  Unconditional: must run even when
	 * pwet_rec_stats is NULL right now, since the pending record's own
	 * recording decisions were already made, for both halves, back when
	 * it was stashed at wait_end time -- see pwet_flush_pending()'s
	 * comment for why it carries those decisions out from pwet_my_stats/
	 * pwet_pending.trace/pwet_my_trace, never re-deriving either one from
	 * pwet_rec_stats/pwet_rec_trace as they stand now.  Cheap when
	 * nothing is pending: one boolean test.
	 */
	pwet_flush_pending();

	/*
	 * The attached, capturing case (the overwhelming majority of calls once
	 * any backend anywhere is capturing) is one pointer test.  Everything
	 * below, up to and including the not-yet-attached slow path, is
	 * unchanged in what it decides -- only the top-level test changed, from
	 * three separate conditions to the one pwet_rec_stats pointer that
	 * pwet_update_rec_pointers() keeps in sync with them (see its comment).
	 */
	if (pwet_rec_stats == NULL)
	{
		/* The original three conditions, tested individually. */
		if (pwet_capture == PWET_CAPTURE_OFF || pwet_stats_writes_disabled)
			return;

		if (pwet_my_stats == NULL)
		{
			/*
			 * A server-side process never reaches post_parse_analyze_hook or
			 * ExecutorStart_hook, so this is the only place it can attach; a
			 * client backend attaches through those hooks (or the assign
			 * hook) instead, since pwet_is_fixed_procnumber() is never true
			 * for a ProcNumber below MaxConnections.  Computed at most once
			 * per process (see pwet_fixed_slot_checked_pid's comment for why
			 * this is keyed by pid rather than a bare "already checked"
			 * flag): the answer cannot change over a process's lifetime once
			 * it has one, and this hook runs on every wait event.
			 *
			 * MyProcNumber can itself still be INVALID_PROC_NUMBER here: the
			 * postmaster never has one (its own ServerLoop reaches this hook
			 * too), and any process, right after fork/exec, technically could
			 * call this before InitProcess()/InitAuxiliaryProcess() has run.
			 * Neither claims nor caches in that case, so a later call -- once
			 * (if ever) MyProcNumber becomes valid -- retries; this costs a
			 * few extra branches per wait in the postmaster for its entire
			 * lifetime (it never gets a ProcNumber), which is fine since the
			 * postmaster's own waits are not a hot path.
			 */
			if (pwet_fixed_slot_checked_pid != MyProcPid)
			{
				if (MyProcNumber == INVALID_PROC_NUMBER)
					return;

				pwet_fixed_slot_checked_pid = MyProcPid;
				pwet_fixed_slot_eligible = pwet_is_fixed_procnumber(MyProcNumber);
			}

			if (pwet_fixed_slot_eligible)
				pwet_claim_fixed_slot();

			if (pwet_my_stats == NULL)
				return;
		}

		/*
		 * Recompute unconditionally, whichever of the three paths above was
		 * taken (already attached; just claimed a fixed slot; still not
		 * attached at all), so pwet_rec_stats is guaranteed fresh before
		 * the hot path below reads it -- pwet_claim_fixed_slot() already
		 * calls this itself too (it is one of the sites
		 * pwet_update_rec_pointers()'s own comment enumerates), but calling
		 * it again here is cheap and idempotent, and means this block does
		 * not have to know which path it took.  Return, not fall through,
		 * if the three conditions still do not all hold.
		 */
		pwet_update_rec_pointers();
		if (pwet_rec_stats == NULL)
			return;
	}

	INSTR_TIME_SET_CURRENT(pwet_wait_start);
	pwet_current_event = wait_event_info;
}

static void
pwet_wait_begin(uint32 wait_event_info)
{
	pwet_wait_begin_impl(wait_event_info, true);
}

static void
pwet_wait_begin_nochain(uint32 wait_event_info)
{
	pwet_wait_begin_impl(wait_event_info, false);
}

/*
 * Body of the wait_event_end_hook; see pwet_wait_begin_impl()'s comment.
 *
 * Before the deferred-accounting change (v11 patch 0004 fixup; see
 * DECISION-deferred-accounting.md), this function did the reset-generation
 * check, the count/total/max/histogram update, and the trace append
 * itself, all before returning to the caller -- for an LWLock wait, that
 * caller is still inside LWLockAcquire()'s critical section, so every one
 * of those nanoseconds, including a cache miss on the shared payload,
 * directly extended the lock's hold time and was paid by every queued
 * waiter (the v11 finding this fixup addresses).  Now it does only the
 * cheap, unavoidable part -- read the clock, compute the duration -- and
 * stashes (event, duration, timestamp, and whether to trace) in the
 * one-slot pending buffer pwet_pending (see its own comment) for
 * pwet_flush_pending() to account, unchanged, from a point that no
 * longer extends any lock's hold time.  Durations are still measured
 * between the same two instants as before, and the stored timestamp is
 * still this same wait_end clock read, so every value that eventually
 * reaches the stats payload or a trace record is identical to what
 * immediate accounting would have produced -- only when it gets there
 * is deferred.  Both recording decisions -- whether to count this wait
 * in stats at all (the pwet_rec_stats test below, unchanged from
 * before), and whether to append it to the trace ring
 * (pwet_pending.trace, capturing pwet_rec_trace's answer right here) --
 * are still made at this exact instant, exactly as they were before
 * deferral; pwet_flush_pending() only carries them out later.
 */
static pg_always_inline void
pwet_wait_end_impl(uint32 wait_event_info, bool chain)
{
	/*
	 * pwet_rec_stats is non-NULL exactly when the old three-way test
	 * (pwet_capture != OFF, !pwet_stats_writes_disabled, pwet_my_stats !=
	 * NULL) held -- see pwet_update_rec_pointers()'s comment -- so this one
	 * pointer read replaces that test without changing which waits get
	 * recorded.
	 */
	if (pwet_rec_stats != NULL)
	{
		uint32		event = pwet_current_event;

		if (event != 0 && !INSTR_TIME_IS_ZERO(pwet_wait_start))
		{
			instr_time	now;
			int64		duration_ns;

			INSTR_TIME_SET_CURRENT(now);
			duration_ns = INSTR_TIME_GET_NANOSEC(now) -
				INSTR_TIME_GET_NANOSEC(pwet_wait_start);
			if (duration_ns < 0)
				duration_ns = 0;

			/*
			 * Defend against nesting: pwet_wait_start/pwet_current_event
			 * form a single in-flight-wait slot that pwet_wait_begin_impl()
			 * always (re)writes before a new wait can start, and that same
			 * function unconditionally flushes pwet_pending before doing
			 * so (see its comment) -- so a record should never already be
			 * pending here.  If it somehow were anyway, flush the old one
			 * now rather than silently overwriting (losing) it.
			 */
			if (pwet_pending_valid)
				pwet_flush_pending();

			pwet_pending.event = event;
			pwet_pending.duration_ns = duration_ns;
			pwet_pending.timestamp_ns = INSTR_TIME_GET_NANOSEC(now);
			/* No trace level yet at this point in the series. */
			pwet_pending.trace = false;
			pwet_pending_valid = true;

			INSTR_TIME_SET_ZERO(pwet_wait_start);
		}
	}

	if (chain)
		prev_wait_event_end_hook(wait_event_info);
}

static void
pwet_wait_end(uint32 wait_event_info)
{
	pwet_wait_end_impl(wait_event_info, true);
}

static void
pwet_wait_end_nochain(uint32 wait_event_info)
{
	pwet_wait_end_impl(wait_event_info, false);
}

/*
 * Lazily install this process's wait_event_begin_hook/wait_event_end_hook,
 * the first time (in this process) capture becomes non-off -- called from
 * pwet_assign_capture(), never from _PG_init() (see its own comment).
 * Idempotent (pwet_wait_hooks_installed guards it) and, deliberately,
 * never undone: a later consumer that chains onto us (prev_*_hook here)
 * may itself have saved our function pointer as ITS previous hook by the
 * time capture drops back to off in this process; uninstalling ourselves
 * here would silently cut that consumer out of the chain for the rest of
 * the process's life, with no way for it to notice.  Leaving the hooks
 * installed costs nothing extra: pwet_wait_begin()/pwet_wait_end() are
 * already no-ops whenever pwet_capture is off.
 *
 * Which of the two wrapper pairs (chaining vs. non-chaining) gets wired up
 * is decided once, right here, from whether this process already had
 * another module's hook installed: a NULL previous pointer means there is
 * nothing to chain to, ever, for the rest of this process's life (nothing
 * later sets wait_event_begin_hook/wait_event_end_hook back to NULL), so
 * the non-chaining variant drops the previous-hook test from the hot path
 * entirely instead of testing a pointer that will forever be NULL.
 */
static void
pwet_install_wait_hooks(void)
{
	if (pwet_wait_hooks_installed)
		return;

	prev_wait_event_begin_hook = wait_event_begin_hook;
	prev_wait_event_end_hook = wait_event_end_hook;

	wait_event_begin_hook = (prev_wait_event_begin_hook != NULL)
		? pwet_wait_begin : pwet_wait_begin_nochain;
	wait_event_end_hook = (prev_wait_event_end_hook != NULL)
		? pwet_wait_end : pwet_wait_end_nochain;

	pwet_wait_hooks_installed = true;
}

static void
pwet_post_parse_analyze(ParseState *pstate, Query *query,
						const JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook != NULL)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	pwet_maybe_attach();
}

static void
pwet_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	pwet_maybe_attach();

	if (prev_ExecutorStart_hook != NULL)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * Resolve the optional pid SRF argument to a ProcNumber range
 * [out_start, out_end).  Returns false if the SRF should emit zero rows
 * (unknown pid -- silent no-op).  Auxiliary processes are included here:
 * unlike the reset functions, reading their stats is not a control action.
 */
static bool
pwet_pid_range(FunctionCallInfo fcinfo, int argnum,
			  int *out_start, int *out_end)
{
	if (PG_ARGISNULL(argnum))
	{
		*out_start = 0;
		*out_end = PWET_NUM_SLOTS;
		return true;
	}
	else
	{
		int			target_pid = PG_GETARG_INT32(argnum);
		PGPROC	   *proc;
		int			procnumber;

		proc = BackendPidGetProc(target_pid);
		if (proc == NULL)
			proc = AuxiliaryPidGetProc(target_pid);
		if (proc == NULL)
			return false;

		procnumber = GetNumberFromPGProc(proc);
		if (procnumber < 0 || procnumber >= PWET_NUM_SLOTS)
			return false;

		*out_start = procnumber;
		*out_end = procnumber + 1;
		return true;
	}
}

static void
pwet_emit_timing_row(ReturnSetInfo *rsinfo, PgBackendStatus *beentry,
					 int procnumber, uint32 wait_event_info,
					 PwetTimingEntry *entry, ArrayType *histogram,
					 int64 *histogram_data)
{
	Datum		values[10];
	bool		nulls[10] = {0};
	const char *event_type;
	const char *event_name;
	int			i;

	event_type = pgstat_get_wait_event_type(wait_event_info);
	event_name = pgstat_get_wait_event(wait_event_info);
	if (event_type == NULL || event_name == NULL)
		return;

	values[0] = Int32GetDatum(beentry->st_procpid);
	values[1] = CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));
	values[2] = Int32GetDatum(procnumber);
	values[3] = CStringGetTextDatum(event_type);
	values[4] = CStringGetTextDatum(event_name);
	values[5] = Int64GetDatum(entry->count);
	values[6] = Float8GetDatum((double) entry->total_ns / 1000000.0);
	values[7] = Float8GetDatum(entry->count > 0
							   ? (double) entry->total_ns /
							   entry->count / 1000.0 : 0.0);
	values[8] = Float8GetDatum((double) entry->max_ns / 1000.0);
	for (i = 0; i < PWET_HISTOGRAM_BUCKETS; i++)
		histogram_data[i] = entry->histogram[i];
	values[9] = PointerGetDatum(histogram);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * Lock-free read of a fixed slot's owner token (plan section 4.2a): read
 * both fields, then a read barrier before the caller looks at the
 * payload, so a concurrent pwet_claim_fixed_slot() -- whose step (1)
 * clears owner_pid before touching the payload -- is guaranteed visible
 * first.  Returns false immediately (without touching the payload at all)
 * if the slot does not currently belong to beentry.
 */
static bool
pwet_fixed_owner_matches(PwetSlot *slot, PgBackendStatus *beentry,
						 int *out_pid, TimestampTz *out_start)
{
	int			pid = slot->owner_pid;
	TimestampTz start = slot->owner_start;

	pg_read_barrier();

	if (pid != beentry->st_procpid || start != beentry->st_proc_start_timestamp)
		return false;

	*out_pid = pid;
	*out_start = start;
	return true;
}

/*
 * Second half of the double read: re-read the owner token after copying
 * the payload (with a read barrier first, pairing with
 * pwet_claim_fixed_slot()'s step (3) write barrier) and confirm it still
 * matches what pwet_fixed_owner_matches() saw.  A mismatch means a claim
 * raced with the copy and the payload may be torn or already belong to a
 * new owner; the caller must discard it.
 */
static bool
pwet_fixed_owner_unchanged(PwetSlot *slot, int pid, TimestampTz start)
{
	pg_read_barrier();
	return slot->owner_pid == pid && slot->owner_start == start;
}

/*
 * Lock-free read of a claimed fixed slot's full payload into *snapshot.
 * See pwet_fixed_owner_matches()/pwet_fixed_owner_unchanged() for the
 * double-read protocol this brackets the copy with.
 */
static bool
pwet_read_fixed_slot(int procnumber, PgBackendStatus *beentry,
					 PwetStats *snapshot)
{
	PwetSlot   *slot = &pwet_ctl[procnumber];
	int			pid;
	TimestampTz start;

	if (!pwet_fixed_owner_matches(slot, beentry, &pid, &start))
		return false;

	memcpy(snapshot, pwet_fixed_payload(procnumber), pwet_server_stride);

	return pwet_fixed_owner_unchanged(slot, pid, start);
}

/*
 * SQL function: pg_stat_get_wait_event_timing(pid int4, OUT ...)
 *
 * One row per (backend, wait_event) with a non-zero count.  pid is
 * optional: NULL means every backend; a non-NULL value restricts the sweep
 * to that backend (silently empty for an unknown pid).
 *
 * The sweep below flushes the CALLING backend's own pending wait, if the
 * sweep reaches its own procnumber (pid was NULL, or exactly this
 * backend's pid), so a session reading its own row always sees its own
 * just-completed waits -- see the module comment / commit message for why
 * this is one of the required flush points.  A cross-backend row is read
 * from whatever the owner last flushed itself, unaffected by this.
 */
Datum
pg_stat_get_wait_event_timing(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ArrayType  *histogram;
	int64	   *histogram_data;
	PwetStats  *snapshot;
	int			start_idx;
	int			end_idx;
	int			procnumber;

	InitMaterializedSRF(fcinfo, 0);

	if (!pwet_pid_range(fcinfo, 0, &start_idx, &end_idx))
		PG_RETURN_VOID();
	if (!pwet_ensure_stats_dsa())
		PG_RETURN_VOID();

	pwet_stats_stride = pwet_stats_payload_size(pwet_max_tranches);
	snapshot = palloc(pwet_stats_stride);

	{
		Datum		zeros[PWET_HISTOGRAM_BUCKETS];

		memset(zeros, 0, sizeof(zeros));
		histogram = construct_array_builtin(zeros,
											PWET_HISTOGRAM_BUCKETS,
											INT8OID);
		histogram_data = (int64 *) ARR_DATA_PTR(histogram);
	}

	for (procnumber = start_idx; procnumber < end_idx; procnumber++)
	{
		PgBackendStatus *beentry;
		bool		matched;
		int			i;

		beentry = pgstat_get_beentry_by_proc_number(procnumber);
		if (beentry == NULL || beentry->st_procpid == 0 ||
			!PWET_HAS_STATS_PRIVS(beentry->st_userid))
			continue;

		/* Own row: flush before reading it (see the function comment). */
		if (procnumber == MyProcNumber)
			pwet_flush_pending();

		if (pwet_is_fixed_procnumber(procnumber))
		{
			/* Lock-free: the region is never freed, so this never races
			 * with anything but the owner's own claim. */
			matched = pwet_read_fixed_slot(procnumber, beentry, snapshot);
		}
		else
		{
			PwetSlot   *slot = &pwet_ctl[procnumber];
			dsa_pointer stats_ptr;

			LWLockAcquire(pwet_lock, LW_SHARED);
			stats_ptr = slot->stats_ptr;
			matched = DsaPointerIsValid(stats_ptr) &&
				slot->owner_pid == beentry->st_procpid &&
				slot->owner_start == beentry->st_proc_start_timestamp;
			if (matched)
				memcpy(snapshot, dsa_get_address(pwet_stats_dsa, stats_ptr),
					   pwet_stats_stride);
			LWLockRelease(pwet_lock);
		}

		if (!matched)
			continue;

		for (i = 0; i < PWET_DENSE_CLASSES; i++)
		{
			int			base = pwet_class_offset[i];
			int			nevents = pwet_class_nevents[i];
			uint32		class_id = pwet_dense_to_classid[i];
			int			j;

			for (j = 0; j < nevents; j++)
			{
				PwetTimingEntry *entry = &snapshot->events[base + j];

				if (entry->count == 0)
					continue;
				pwet_emit_timing_row(rsinfo, beentry, procnumber,
									 ((uint32) class_id << 24) | (uint32) j,
									 entry, histogram, histogram_data);
			}
		}

		{
			PwetLWLockHashEntry *entries =
				pwet_lwlock_hash_entries(snapshot);
			PwetTimingEntry *events =
				pwet_lwlock_hash_events(snapshot);

			for (i = 0; i < snapshot->lwlock_hash.hash_size; i++)
			{
				PwetLWLockHashEntry *hash_entry = &entries[i];
				PwetTimingEntry *entry;

				if (hash_entry->tranche_id == PWET_LWLOCK_EMPTY)
					continue;
				entry = &events[hash_entry->dense_idx];
				if (entry->count == 0)
					continue;
				pwet_emit_timing_row(rsinfo, beentry, procnumber,
									 PG_WAIT_LWLOCK |
									 hash_entry->tranche_id,
									 entry, histogram, histogram_data);
			}
		}
	}

	pfree(snapshot);
	PG_RETURN_VOID();
}

/*
 * SQL function: pg_stat_get_wait_event_timing_overflow(pid int4, OUT ...)
 *
 * One row per backend that has an attached stats payload, exposing the
 * truncation counters the recording path maintains.  pid has the same
 * optional semantics as pg_stat_get_wait_event_timing().
 *
 * Flushes the calling backend's own pending record before emitting its
 * row, same rule and same reason as pg_stat_get_wait_event_timing()'s own
 * sweep (see pwet_flush_pending()'s comment): reset_count in particular
 * is only ever advanced by a flush noticing reset_generation has moved,
 * so a caller reading its own row here must not be left looking at a
 * stale count merely because nothing else has flushed for it yet.
 */
Datum
pg_stat_get_wait_event_timing_overflow(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			start_idx;
	int			end_idx;
	int			procnumber;

	InitMaterializedSRF(fcinfo, 0);

	if (!pwet_pid_range(fcinfo, 0, &start_idx, &end_idx))
		PG_RETURN_VOID();
	if (!pwet_ensure_stats_dsa())
		PG_RETURN_VOID();

	for (procnumber = start_idx; procnumber < end_idx; procnumber++)
	{
		PgBackendStatus *beentry;
		Datum		values[6];
		bool		nulls[6] = {0};
		int64		lwlock_overflow = 0;
		int64		flat_overflow = 0;
		int64		reset_count = 0;
		bool		matched = false;

		beentry = pgstat_get_beentry_by_proc_number(procnumber);
		if (beentry == NULL || beentry->st_procpid == 0 ||
			!PWET_HAS_STATS_PRIVS(beentry->st_userid))
			continue;

		/* Own row: flush before reading it (see the function comment). */
		if (procnumber == MyProcNumber)
			pwet_flush_pending();

		if (pwet_is_fixed_procnumber(procnumber))
		{
			PwetSlot   *slot = &pwet_ctl[procnumber];
			int			pid;
			TimestampTz start;

			if (pwet_fixed_owner_matches(slot, beentry, &pid, &start))
			{
				PwetStats  *state = pwet_fixed_payload(procnumber);

				lwlock_overflow = state->lwlock_overflow_count;
				flat_overflow = state->flat_overflow_count;
				reset_count = state->reset_count;
				matched = pwet_fixed_owner_unchanged(slot, pid, start);
			}
		}
		else
		{
			PwetSlot   *slot = &pwet_ctl[procnumber];

			LWLockAcquire(pwet_lock, LW_SHARED);
			if (DsaPointerIsValid(slot->stats_ptr) &&
				slot->owner_pid == beentry->st_procpid &&
				slot->owner_start == beentry->st_proc_start_timestamp)
			{
				PwetStats  *state = dsa_get_address(pwet_stats_dsa,
													slot->stats_ptr);

				lwlock_overflow = state->lwlock_overflow_count;
				flat_overflow = state->flat_overflow_count;
				reset_count = state->reset_count;
				matched = true;
			}
			LWLockRelease(pwet_lock);
		}

		if (!matched)
			continue;

		values[0] = Int32GetDatum(beentry->st_procpid);
		values[1] = CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));
		values[2] = Int32GetDatum(procnumber);
		values[3] = Int64GetDatum(lwlock_overflow);
		values[4] = Int64GetDatum(flat_overflow);
		values[5] = Int64GetDatum(reset_count);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	PG_RETURN_VOID();
}

/*
 * Flushes the pending wait, if any, before zeroing: the wait already
 * happened and, per the decision document, its recorded values must not
 * be lost, so it is accounted first and then immediately wiped by the
 * reset below, exactly as it would have been (recorded, then reset) had
 * pg_stat_reset_wait_event_timing() run in a later statement instead of
 * landing between the wait and its flush.  Its trace record, if any, is
 * unaffected by a stats reset and is left standing.
 */
static void
pwet_reset_own(void)
{
	pwet_flush_pending();

	if (pwet_my_stats != NULL)
	{
		memset(pwet_my_stats->events, 0, sizeof(pwet_my_stats->events));
		pwet_lwlock_hash_clear(pwet_my_stats);
		pwet_my_stats->reset_count++;
		pwet_my_stats->lwlock_overflow_count = 0;
		pwet_my_stats->flat_overflow_count = 0;
		pwet_current_event = 0;
		INSTR_TIME_SET_ZERO(pwet_wait_start);
	}
}

/*
 * Replicate the target-authorization checks of pg_signal_backend() in
 * src/backend/storage/ipc/signalfuncs.c: a non-superuser cannot touch a
 * superuser-owned or role-less target, and otherwise needs privileges of
 * the target role or of pg_signal_backend.  Unlike pg_signal_backend(),
 * there is no separate carve-out for autovacuum workers: they are
 * role-less, so they already require superuser here, which is the more
 * conservative choice for a function that erases diagnostic state.
 */
static void
pwet_check_reset_privileges(Oid target_role)
{
	if (!OidIsValid(target_role) || superuser_arg(target_role))
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to reset another backend's wait event timing statistics"),
					 errdetail("Only roles with the %s attribute may reset statistics of a superuser-owned or role-less backend.",
							   "SUPERUSER")));
	}
	else if (!has_privs_of_role(GetUserId(), target_role) &&
			 !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reset another backend's wait event timing statistics"),
				 errdetail("Only roles with privileges of the target role or the \"%s\" role may reset another backend's wait event timing statistics.",
						   "pg_signal_backend")));
}

/*
 * Request an asynchronous reset on the given slot, if it is still owned by
 * target_pid/target_start.  The owning backend notices at its next
 * wait_end() (see pwet_wait_end()) and clears its own counters.
 *
 * target_pid/target_start were captured by the caller when it resolved the
 * pid to a ProcNumber, which can be arbitrarily far in the past by the time
 * we get the lock (ProcArrayLock was already released by then).  Re-checking
 * the owner token under the same lock that publishes the request is what
 * prevents the request from landing on a successor that has since reused
 * this ProcNumber (a bare "does this slot have a payload" check is not
 * enough: the successor could be capturing too).
 */
static void
pwet_request_reset(int procnumber, int target_pid, TimestampTz target_start)
{
	PwetSlot   *slot = &pwet_ctl[procnumber];

	INJECTION_POINT("pg-wait-event-tracing-reset-before-publish", NULL);

	LWLockAcquire(pwet_lock, LW_EXCLUSIVE);
	if (slot->owner_pid == target_pid && slot->owner_start == target_start)
		pg_atomic_fetch_add_u32(&slot->reset_generation, 1);
	LWLockRelease(pwet_lock);
}

/*
 * SQL function: pg_stat_reset_wait_event_timing(pid int4)
 *
 *   NULL or own pid : reset the caller's own counters synchronously.
 *   another pid     : request a cross-backend reset, subject to the same
 *                      target authorization as pg_signal_backend().
 *   unknown pid     : silent no-op (matching pg_signal_backend()'s WARNING).
 *   auxiliary pid   : rejected -- BackendPidGetProc() only resolves normal
 *                      backends, so this falls out of the same check.
 */
Datum
pg_stat_reset_wait_event_timing(PG_FUNCTION_ARGS)
{
	int			target_pid;
	PGPROC	   *proc;
	int			procnumber;
	PgBackendStatus *beentry;

	if (PG_ARGISNULL(0) || PG_GETARG_INT32(0) == MyProcPid)
	{
		pwet_reset_own();
		PG_RETURN_VOID();
	}

	target_pid = PG_GETARG_INT32(0);

	proc = BackendPidGetProc(target_pid);
	if (proc == NULL)
	{
		/* Matches pg_signal_backend(): unknown pid or auxiliary process. */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL backend process",
						target_pid)));
		PG_RETURN_VOID();
	}

	procnumber = GetNumberFromPGProc(proc);
	if (procnumber < 0 || procnumber >= PWET_NUM_SLOTS)
		PG_RETURN_VOID();

	pwet_check_reset_privileges(proc->roleId);

	beentry = pgstat_get_beentry_by_proc_number(procnumber);
	if (beentry == NULL || beentry->st_procpid != target_pid)
		PG_RETURN_VOID();		/* gone by the time we got here */

	pwet_request_reset(procnumber, target_pid,
					   beentry->st_proc_start_timestamp);

	PG_RETURN_VOID();
}

/*
 * SQL function: pg_stat_reset_wait_event_timing_all()
 *
 * Request a reset on every slot.  Superuser-only: unlike the single-pid
 * form, this is not delegable by granting EXECUTE, matching the "_all()
 * superuser-only" policy regardless of what the extension script's default
 * REVOKE/GRANT state happens to be.
 */
Datum
pg_stat_reset_wait_event_timing_all(PG_FUNCTION_ARGS)
{
	int			i;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reset wait event timing statistics for all backends"),
				 errdetail("Only roles with the %s attribute may reset statistics for all backends.",
						   "SUPERUSER")));

	/*
	 * Unlike the single-pid form, there is no specific owner to re-check:
	 * bumping an unowned slot's reset_generation is harmless (nothing
	 * consumes it), and a slot that gets a new owner concurrently either
	 * sees this generation already accounted for at attach time or picks up
	 * the bump at its first wait_end, which is a fine outcome either way for
	 * an operation whose contract is "every backend", not "this backend".
	 */
	LWLockAcquire(pwet_lock, LW_EXCLUSIVE);
	for (i = 0; i < PWET_NUM_SLOTS; i++)
		pg_atomic_fetch_add_u32(&pwet_ctl[i].reset_generation, 1);
	LWLockRelease(pwet_lock);

	PG_RETURN_VOID();
}

/*
 * SQL function: pg_wait_event_tracing_capacity()
 *
 * One row per dense class plus one for LWLock (whose effective capacity is
 * the max_tranches GUC, not a table entry, since LWLock waits go through a
 * per-backend hash rather than the flat per-event array).  Meant to be
 * compared against "SELECT type, count(*) FROM pg_wait_events GROUP BY
 * type" by the module's regression test, which fails when any class is
 * within 4 of its capacity.
 */
Datum
pg_wait_event_tracing_capacity(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		values[2];
	bool		nulls[2] = {0};
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	for (i = 0; i < PWET_DENSE_CLASSES; i++)
	{
		values[0] = CStringGetTextDatum(pwet_class_names[i]);
		values[1] = Int32GetDatum(pwet_class_nevents[i]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	values[0] = CStringGetTextDatum("LWLock");
	values[1] = Int32GetDatum(pwet_max_tranches);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	PG_RETURN_VOID();
}

/*
 * SQL function: pg_wait_event_tracing_hooks_installed()
 *
 * Diagnostic for the lazy, per-process hook installation (see
 * pwet_install_wait_hooks()): true if the calling backend has installed
 * its own wait_event_begin_hook/wait_event_end_hook, false if it never has
 * (capture has been off in this process since it started).  Reveals
 * nothing about any other backend or about what is being recorded, so it
 * is granted to PUBLIC, like pg_wait_event_tracing_capacity().
 */
Datum
pg_wait_event_tracing_hooks_installed(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pwet_wait_hooks_installed);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_wait_event_tracing must be loaded via \"shared_preload_libraries\"")));

	DefineCustomEnumVariable("pg_wait_event_tracing.capture",
							 "Controls wait event collection.",
							 NULL,
							 &pwet_capture,
							 PWET_CAPTURE_OFF,
							 pwet_capture_options,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 pwet_assign_capture,
							 NULL);
	DefineCustomIntVariable("pg_wait_event_tracing.max_tranches",
							"Maximum distinct LWLock tranches tracked per backend.",
							NULL,
							&pwet_max_tranches,
							192,
							16,
							65534,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE,
							NULL,
							NULL,
							NULL);
	MarkGUCPrefixReserved("pg_wait_event_tracing");

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pwet_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pwet_shmem_startup;

	/*
	 * The wait-event begin/end hooks are NOT installed here: they are
	 * installed lazily, per process, the first time this process's own
	 * pwet_assign_capture() sees capture become non-off -- see
	 * pwet_install_wait_hooks().  A process that never enables capture
	 * never pays the indirect-call overhead on its timed waits.
	 */
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pwet_post_parse_analyze;
	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = pwet_ExecutorStart;

	pwet_active = true;
	pwet_attach_needed = (pwet_capture != PWET_CAPTURE_OFF);
}
