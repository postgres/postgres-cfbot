/* -------------------------------------------------------------------------
 *
 * pgstat_io.c
 *	  Implementation of IO statistics.
 *
 * This file contains the implementation of IO statistics. It is kept separate
 * from pgstat.c to enforce the line between the statistics access / storage
 * implementation and the details about individual types of statistics.
 *
 * IO statistics use a per-backend dshash to avoid double-counting. Each
 * process flushes IO stats to its own entry in the dshash (keyed by
 * ProcNumber). The global pg_stat_io view aggregates the global stats
 * (which holds stats from exited processes) plus all live per-backend entries.
 *
 * Copyright (c) 2021-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_io.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/instrument.h"
#include "storage/bufmgr.h"
#include "utils/pgstat_internal.h"

static PgStat_PendingIO PendingIOStats;
static bool have_iostats = false;

/*
 * Check that stats have not been counted for any combination of IOObject,
 * IOContext, and IOOp which are not tracked for the passed-in BackendType.
 * Non-zero time with a zero operation count is allowed as there are cases
 * where this may be appropriate -- like when a backend is waiting on IO
 * initiated by another backend.
 *
 * The passed-in PgStat_BktypeIO must contain stats from the BackendType
 * specified by the second parameter. Caller is responsible for locking the
 * passed-in PgStat_BktypeIO, if needed.
 */
bool
pgstat_bktype_io_stats_valid(PgStat_BktypeIO *backend_io,
							 BackendType bktype)
{
	for (int io_object = 0; io_object < IOOBJECT_NUM_TYPES; io_object++)
	{
		for (int io_context = 0; io_context < IOCONTEXT_NUM_TYPES; io_context++)
		{
			for (int io_op = 0; io_op < IOOP_NUM_TYPES; io_op++)
			{
				/* we don't track it, and it is not 0 */
				if (!pgstat_tracks_io_op(bktype, io_object, io_context, io_op) &&
					(backend_io->counts[io_object][io_context][io_op] != 0 ||
					 backend_io->times[io_object][io_context][io_op] != 0))
					return false;
			}
		}
	}

	return true;
}

void
pgstat_count_io_op(IOObject io_object, IOContext io_context, IOOp io_op,
				   uint32 cnt, uint64 bytes)
{
	Assert((unsigned int) io_object < IOOBJECT_NUM_TYPES);
	Assert((unsigned int) io_context < IOCONTEXT_NUM_TYPES);
	Assert(pgstat_is_ioop_tracked_in_bytes(io_op) || bytes == 0);
	Assert(pgstat_tracks_io_op(MyBackendType, io_object, io_context, io_op));

	PendingIOStats.counts[io_object][io_context][io_op] += cnt;
	PendingIOStats.bytes[io_object][io_context][io_op] += bytes;

	have_iostats = true;
	pgstat_report_fixed = true;
}

/*
 * Initialize the internal timing for an IO operation, depending on an
 * IO timing GUC.
 */
instr_time
pgstat_prepare_io_time(bool track_io_guc)
{
	instr_time	io_start;

	if (track_io_guc)
		INSTR_TIME_SET_CURRENT(io_start);
	else
	{
		/*
		 * There is no need to set io_start when an IO timing GUC is disabled.
		 * Initialize it to zero to avoid compiler warnings and to let
		 * pgstat_count_io_op_time() know that timings should be ignored.
		 */
		INSTR_TIME_SET_ZERO(io_start);
	}

	return io_start;
}

/*
 * Like pgstat_count_io_op() except it also accumulates time.
 *
 * The calls related to pgstat_count_buffer_*() are for pgstat_database.  As
 * pg_stat_database only counts block read and write times, these are done for
 * IOOP_READ, IOOP_WRITE and IOOP_EXTEND.
 *
 * pgBufferUsage is used for EXPLAIN.  pgBufferUsage has write and read stats
 * for shared, local and temporary blocks.  pg_stat_io does not track the
 * activity of temporary blocks, so these are ignored here.
 */
void
pgstat_count_io_op_time(IOObject io_object, IOContext io_context, IOOp io_op,
						instr_time start_time, uint32 cnt, uint64 bytes)
{
	if (!INSTR_TIME_IS_ZERO(start_time))
	{
		instr_time	io_time;

		INSTR_TIME_SET_CURRENT(io_time);
		INSTR_TIME_SUBTRACT(io_time, start_time);

		if (io_object != IOOBJECT_WAL)
		{
			if (io_op == IOOP_WRITE || io_op == IOOP_EXTEND)
			{
				pgstat_count_buffer_write_time(INSTR_TIME_GET_MICROSEC(io_time));
				if (io_object == IOOBJECT_RELATION)
					INSTR_TIME_ADD(pgBufferUsage.shared_blk_write_time, io_time);
				else if (io_object == IOOBJECT_TEMP_RELATION)
					INSTR_TIME_ADD(pgBufferUsage.local_blk_write_time, io_time);
			}
			else if (io_op == IOOP_READ)
			{
				pgstat_count_buffer_read_time(INSTR_TIME_GET_MICROSEC(io_time));
				if (io_object == IOOBJECT_RELATION)
					INSTR_TIME_ADD(pgBufferUsage.shared_blk_read_time, io_time);
				else if (io_object == IOOBJECT_TEMP_RELATION)
					INSTR_TIME_ADD(pgBufferUsage.local_blk_read_time, io_time);
			}
		}

		INSTR_TIME_ADD(PendingIOStats.pending_times[io_object][io_context][io_op],
					   io_time);
	}

	pgstat_count_io_op(io_object, io_context, io_op, cnt, bytes);
}

PgStat_IO *
pgstat_fetch_stat_io(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_IO);

	return &pgStatLocal.snapshot.io;
}

/*
 * Simpler wrapper of pgstat_io_flush_cb()
 */
void
pgstat_flush_io(bool nowait)
{
	(void) pgstat_io_flush_cb(nowait);
}

/*
 * Flush out locally pending IO statistics to the per-backend dshash entry.
 *
 * If no stats have been recorded, this function returns false.
 *
 * If nowait is true, this function returns true if the lock could not be
 * acquired. Otherwise, return false.
 */
bool
pgstat_io_flush_cb(bool nowait)
{
	PgStatShared_IOBackendEntry *entry;
	PgStat_BktypeIO *bktype_shstats;

	if (!have_iostats)
		return false;

	entry = pgstat_lock_my_per_backend_entry(PGSTAT_KIND_IO, nowait);

	if (entry == NULL)
		return nowait;

	bktype_shstats = &entry->stats.stats;

	for (int io_object = 0; io_object < IOOBJECT_NUM_TYPES; io_object++)
	{
		for (int io_context = 0; io_context < IOCONTEXT_NUM_TYPES; io_context++)
		{
			for (int io_op = 0; io_op < IOOP_NUM_TYPES; io_op++)
			{
				instr_time	time;

				bktype_shstats->counts[io_object][io_context][io_op] +=
					PendingIOStats.counts[io_object][io_context][io_op];

				bktype_shstats->bytes[io_object][io_context][io_op] +=
					PendingIOStats.bytes[io_object][io_context][io_op];

				time = PendingIOStats.pending_times[io_object][io_context][io_op];

				bktype_shstats->times[io_object][io_context][io_op] +=
					INSTR_TIME_GET_MICROSEC(time);
			}
		}
	}

	LWLockRelease(&entry->header.lock);

	memset(&PendingIOStats, 0, sizeof(PendingIOStats));
	have_iostats = false;

	return false;
}

const char *
pgstat_get_io_context_name(IOContext io_context)
{
	switch (io_context)
	{
		case IOCONTEXT_BULKREAD:
			return "bulkread";
		case IOCONTEXT_BULKWRITE:
			return "bulkwrite";
		case IOCONTEXT_INIT:
			return "init";
		case IOCONTEXT_NORMAL:
			return "normal";
		case IOCONTEXT_VACUUM:
			return "vacuum";
	}

	elog(ERROR, "unrecognized IOContext value: %d", io_context);
	pg_unreachable();
}

const char *
pgstat_get_io_object_name(IOObject io_object)
{
	switch (io_object)
	{
		case IOOBJECT_RELATION:
			return "relation";
		case IOOBJECT_TEMP_RELATION:
			return "temp relation";
		case IOOBJECT_WAL:
			return "wal";
	}

	elog(ERROR, "unrecognized IOObject value: %d", io_object);
	pg_unreachable();
}

void
pgstat_io_init_shmem_cb(void *stats)
{
	PgStatShared_IO *stat_shmem = (PgStatShared_IO *) stats;

	LWLockInitialize(&stat_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_io_reset_all_cb(TimestampTz ts)
{
	PgStatShared_IO *shmem = &pgStatLocal.shmem->io;
	dshash_seq_status hstat;
	PgStatShared_IOBackendEntry *entry;
	dshash_table *hash;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_IO);

	/*
	 * Hold the kind lock while resetting both the global stats and live
	 * entries. Transfers hold the same lock, so pre-reset counters cannot be
	 * moved into the global stats after it is reset.
	 */
	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);
	memset(&shmem->stats, 0, sizeof(shmem->stats));
	shmem->stats.stat_reset_timestamp = ts;

	/* Reset all per-backend entries */
	if (hash != NULL)
	{
		dshash_seq_init(&hstat, hash, true);
		while ((entry = dshash_seq_next(&hstat)) != NULL)
		{
			LWLockAcquire(&entry->header.lock, LW_EXCLUSIVE);
			memset(&entry->stats.stats, 0, sizeof(PgStat_BktypeIO));
			entry->stats.stat_reset_timestamp = ts;
			LWLockRelease(&entry->header.lock);
		}
		dshash_seq_term(&hstat);
	}

	LWLockRelease(&shmem->lock);
}

/*
 * Build IO stats snapshot by aggregating global stats and all live
 * per-backend entries.
 */
void
pgstat_io_snapshot_cb(void)
{
	PgStatShared_IO *shmem = &pgStatLocal.shmem->io;
	PgStat_IO  *snap = &pgStatLocal.snapshot.io;
	dshash_table *hash;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_IO);

	/*
	 * Prevent entries from moving to the global stats between copying it and
	 * scanning the per-backend hash.
	 */
	LWLockAcquire(&shmem->lock, LW_SHARED);
	memcpy(snap, &shmem->stats, sizeof(PgStat_IO));

	/* Add in all live per-backend entries */
	if (hash != NULL)
		pgstat_per_backend_snapshot(PGSTAT_KIND_IO, hash, snap);

	LWLockRelease(&shmem->lock);
}

/*
 * IO statistics are not collected for all BackendTypes.
 *
 * The following BackendTypes do not participate in the cumulative stats
 * subsystem or do not perform IO on which we currently track:
 * - Dead-end backend because it is not connected to shared memory and
 *   doesn't do any IO
 * - Syslogger because it is not connected to shared memory
 * - Archiver because most relevant archiving IO is delegated to a
 *   specialized command or module
 *
 * Function returns true if BackendType participates in the cumulative stats
 * subsystem for IO and false if it does not.
 *
 * When adding a new BackendType, also consider adding relevant restrictions to
 * pgstat_tracks_io_object() and pgstat_tracks_io_op().
 */
bool
pgstat_tracks_io_bktype(BackendType bktype)
{
	/*
	 * List every type so that new backend types trigger a warning about
	 * needing to adjust this switch.
	 */
	switch (bktype)
	{
		case B_INVALID:
		case B_DEAD_END_BACKEND:
		case B_ARCHIVER:
		case B_LOGGER:
			return false;

		case B_DATACHECKSUMSWORKER_LAUNCHER:
		case B_DATACHECKSUMSWORKER_WORKER:
		case B_AUTOVAC_LAUNCHER:
		case B_AUTOVAC_WORKER:
		case B_BACKEND:
		case B_BG_WORKER:
		case B_BG_WRITER:
		case B_CHECKPOINTER:
		case B_IO_WORKER:
		case B_SLOTSYNC_WORKER:
		case B_STANDALONE_BACKEND:
		case B_STARTUP:
		case B_WAL_RECEIVER:
		case B_WAL_SENDER:
		case B_WAL_SUMMARIZER:
		case B_WAL_WRITER:
			return true;
	}

	return false;
}

/*
 * Some BackendTypes do not perform IO on certain IOObjects or in certain
 * IOContexts. Some IOObjects are never operated on in some IOContexts. Check
 * that the given BackendType is expected to do IO in the given IOContext and
 * on the given IOObject and that the given IOObject is expected to be operated
 * on in the given IOContext.
 */
bool
pgstat_tracks_io_object(BackendType bktype, IOObject io_object,
						IOContext io_context)
{
	bool		no_temp_rel;

	/*
	 * Some BackendTypes should never track IO statistics.
	 */
	if (!pgstat_tracks_io_bktype(bktype))
		return false;

	/*
	 * Currently, IO on IOOBJECT_WAL objects can only occur in the
	 * IOCONTEXT_NORMAL and IOCONTEXT_INIT IOContexts.
	 */
	if (io_object == IOOBJECT_WAL &&
		(io_context != IOCONTEXT_NORMAL &&
		 io_context != IOCONTEXT_INIT))
		return false;

	/*
	 * Currently, IO on temporary relations can only occur in the
	 * IOCONTEXT_NORMAL IOContext.
	 */
	if (io_context != IOCONTEXT_NORMAL &&
		io_object == IOOBJECT_TEMP_RELATION)
		return false;

	/*
	 * In core Postgres, only regular backends and WAL Sender processes
	 * executing queries will use local buffers and operate on temporary
	 * relations. Parallel workers will not use local buffers (see
	 * InitLocalBuffers()); however, extensions leveraging background workers
	 * have no such limitation, so track IO on IOOBJECT_TEMP_RELATION for
	 * BackendType B_BG_WORKER.
	 */
	no_temp_rel = bktype == B_AUTOVAC_LAUNCHER || bktype == B_BG_WRITER ||
		bktype == B_CHECKPOINTER || bktype == B_AUTOVAC_WORKER ||
		bktype == B_STANDALONE_BACKEND || bktype == B_STARTUP ||
		bktype == B_WAL_SUMMARIZER || bktype == B_WAL_WRITER ||
		bktype == B_WAL_RECEIVER;

	if (no_temp_rel && io_context == IOCONTEXT_NORMAL &&
		io_object == IOOBJECT_TEMP_RELATION)
		return false;

	/*
	 * Some BackendTypes only perform IO under IOOBJECT_WAL, hence exclude all
	 * rows for all the other objects for these.
	 */
	if ((bktype == B_WAL_SUMMARIZER || bktype == B_WAL_RECEIVER ||
		 bktype == B_WAL_WRITER) && io_object != IOOBJECT_WAL)
		return false;

	/*
	 * Some BackendTypes do not currently perform any IO in certain
	 * IOContexts, and, while it may not be inherently incorrect for them to
	 * do so, excluding those rows from the view makes the view easier to use.
	 */
	if ((bktype == B_CHECKPOINTER || bktype == B_BG_WRITER) &&
		(io_context == IOCONTEXT_BULKREAD ||
		 io_context == IOCONTEXT_BULKWRITE ||
		 io_context == IOCONTEXT_VACUUM))
		return false;

	if (bktype == B_AUTOVAC_LAUNCHER && io_context == IOCONTEXT_VACUUM)
		return false;

	if ((bktype == B_AUTOVAC_WORKER || bktype == B_AUTOVAC_LAUNCHER) &&
		io_context == IOCONTEXT_BULKWRITE)
		return false;

	/*
	 * The data checksums launcher scans catalogs and emits WAL records for
	 * checksum state changes. Catalog scans can use a bulkread strategy.
	 */
	if (bktype == B_DATACHECKSUMSWORKER_LAUNCHER)
	{
		if (io_object == IOOBJECT_WAL ||
			(io_object == IOOBJECT_RELATION &&
			 (io_context == IOCONTEXT_BULKREAD ||
			  io_context == IOCONTEXT_NORMAL)))
			return true;

		return false;
	}

	/*
	 * The worker also scans catalogs, then processes relations using a vacuum
	 * access strategy. Catalog scans can use a bulkread strategy.
	 */
	if (bktype == B_DATACHECKSUMSWORKER_WORKER)
	{
		if (io_object == IOOBJECT_WAL ||
			(io_object == IOOBJECT_RELATION &&
			 (io_context == IOCONTEXT_BULKREAD ||
			  io_context == IOCONTEXT_NORMAL ||
			  io_context == IOCONTEXT_VACUUM)))
			return true;

		return false;
	}

	return true;
}

/*
 * Some BackendTypes will never do certain IOOps and some IOOps should not
 * occur in certain IOContexts or on certain IOObjects. Check that the given
 * IOOp is valid for the given BackendType in the given IOContext and on the
 * given IOObject. Note that there are currently no cases of an IOOp being
 * invalid for a particular BackendType only within a certain IOContext and/or
 * only on a certain IOObject.
 */
bool
pgstat_tracks_io_op(BackendType bktype, IOObject io_object,
					IOContext io_context, IOOp io_op)
{
	bool		strategy_io_context;

	/* if (io_context, io_object) will never collect stats, we're done */
	if (!pgstat_tracks_io_object(bktype, io_object, io_context))
		return false;

	/*
	 * Some BackendTypes will not do certain IOOps.
	 */
	if (bktype == B_BG_WRITER &&
		(io_op == IOOP_READ || io_op == IOOP_EVICT || io_op == IOOP_HIT))
		return false;

	if (bktype == B_CHECKPOINTER &&
		((io_object != IOOBJECT_WAL && io_op == IOOP_READ) ||
		 (io_op == IOOP_EVICT || io_op == IOOP_HIT)))
		return false;

	if ((bktype == B_BG_WRITER || bktype == B_CHECKPOINTER) &&
		io_op == IOOP_EXTEND)
		return false;

	/*
	 * Some BackendTypes do not perform reads with IOOBJECT_WAL.
	 */
	if (io_object == IOOBJECT_WAL && io_op == IOOP_READ &&
		(bktype == B_WAL_RECEIVER || bktype == B_BG_WRITER ||
		 bktype == B_AUTOVAC_LAUNCHER || bktype == B_AUTOVAC_WORKER ||
		 bktype == B_DATACHECKSUMSWORKER_LAUNCHER ||
		 bktype == B_DATACHECKSUMSWORKER_WORKER ||
		 bktype == B_WAL_WRITER))
		return false;

	/*
	 * Temporary tables are not logged and thus do not require fsync'ing.
	 * Writeback is not requested for temporary tables.
	 */
	if (io_object == IOOBJECT_TEMP_RELATION &&
		(io_op == IOOP_FSYNC || io_op == IOOP_WRITEBACK))
		return false;

	/*
	 * Some IOOps are not valid in certain IOContexts and some IOOps are only
	 * valid in certain contexts.
	 */
	if (io_context == IOCONTEXT_BULKREAD && io_op == IOOP_EXTEND)
		return false;

	strategy_io_context = io_context == IOCONTEXT_BULKREAD ||
		io_context == IOCONTEXT_BULKWRITE || io_context == IOCONTEXT_VACUUM;

	/*
	 * IOOP_REUSE is only relevant when a BufferAccessStrategy is in use.
	 */
	if (!strategy_io_context && io_op == IOOP_REUSE)
		return false;

	/*
	 * IOOBJECT_WAL IOObject will not do certain IOOps depending on IOContext.
	 */
	if (io_object == IOOBJECT_WAL && io_context == IOCONTEXT_INIT &&
		!(io_op == IOOP_WRITE || io_op == IOOP_FSYNC))
		return false;

	if (io_object == IOOBJECT_WAL && io_context == IOCONTEXT_NORMAL &&
		!(io_op == IOOP_WRITE || io_op == IOOP_READ || io_op == IOOP_FSYNC))
		return false;

	/*
	 * IOOP_FSYNC IOOps done by a backend using a BufferAccessStrategy are
	 * counted in the IOCONTEXT_NORMAL IOContext. See comment in
	 * register_dirty_segment() for more details.
	 */
	if (strategy_io_context && io_op == IOOP_FSYNC)
		return false;


	return true;
}

/*
 * Accumulate IO counters from src into dst.
 */
static inline void
pgstat_io_accumulate_counters(PgStat_BktypeIO *dst, const PgStat_BktypeIO *src)
{
	for (int io_object = 0; io_object < IOOBJECT_NUM_TYPES; io_object++)
	{
		for (int io_context = 0; io_context < IOCONTEXT_NUM_TYPES; io_context++)
		{
			for (int io_op = 0; io_op < IOOP_NUM_TYPES; io_op++)
			{
				dst->counts[io_object][io_context][io_op] +=
					src->counts[io_object][io_context][io_op];
				dst->bytes[io_object][io_context][io_op] +=
					src->bytes[io_object][io_context][io_op];
				dst->times[io_object][io_context][io_op] +=
					src->times[io_object][io_context][io_op];
			}
		}
	}
}

/*
 * Accumulate one per-backend IO entry into a snapshot or the global stats.
 */
void
pgstat_io_per_backend_acc_cb(void *dst, void *entry)
{
	PgStat_IO  *stats = dst;
	PgStatShared_IOBackendEntry *e = (PgStatShared_IOBackendEntry *) entry;
	BackendType bktype = e->header.backend_type;

	if (bktype == B_INVALID)
		return;

	pgstat_io_accumulate_counters(&stats->stats[bktype], &e->stats.stats);
}

/*
 * Accumulate a backend's IO stats into the global stats, then remove the
 * entry from the dshash.
 *
 * Called at backend exit after the final flush, or when a ProcNumber is
 * being reused.
 */
void
pgstat_io_acc_backend_cb(void)
{
	pgstat_acc_my_per_backend(PGSTAT_KIND_IO, &pgStatLocal.shmem->io.lock);
}

/*
 * Accumulate all remaining per-backend IO stats entries into the global stats
 * and remove them. Called at clean server shutdown to ensure all flushed data
 * is preserved in the stats file.
 */
void
pgstat_io_acc_all_backends(void)
{
	pgstat_acc_all_per_backend(PGSTAT_KIND_IO, &pgStatLocal.shmem->io.lock);
}

/*
 * Returns per-backend IO statistics for the given ProcNumber.
 */
PgStat_BackendIO *
pgstat_fetch_stat_backend_io(ProcNumber procnum)
{
	return (PgStat_BackendIO *) pgstat_fetch_per_backend(PGSTAT_KIND_IO, procnum);
}

/*
 * Reset a backend's IO stats. Accumulate the entry's counters into the
 * global stats, then zero the stats and set the reset timestamp.
 */
void
pgstat_io_reset_backend_cb(ProcNumber procnum, TimestampTz ts)
{
	PgStatShared_IO *shmem = &pgStatLocal.shmem->io;
	dshash_table *hash;
	PgStatShared_IOBackendEntry *entry;

	hash = pgstat_per_backend_attach(PGSTAT_KIND_IO);

	if (hash == NULL)
		return;

	LWLockAcquire(&shmem->lock, LW_EXCLUSIVE);

	entry = dshash_find(hash, &procnum, true);

	if (entry == NULL)
	{
		LWLockRelease(&shmem->lock);
		return;
	}

	LWLockAcquire(&entry->header.lock, LW_EXCLUSIVE);

	/* Accumulate current stats into global before zeroing */
	pgstat_io_accumulate_counters(&shmem->stats.stats[entry->header.backend_type],
								  &entry->stats.stats);

	/* Zero stats and set reset timestamp */
	memset(&entry->stats, 0, sizeof(entry->stats));
	entry->stats.stat_reset_timestamp = ts;

	LWLockRelease(&entry->header.lock);
	dshash_release_lock(hash, entry);
	LWLockRelease(&shmem->lock);
}
