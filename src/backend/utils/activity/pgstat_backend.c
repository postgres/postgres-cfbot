/* -------------------------------------------------------------------------
 *
 * pgstat_backend.c
 *	  Implementation of backend statistics.
 *
 * This file contains the implementation of backend statistics.  It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * This statistics kind uses a proc number as object ID for the hash table
 * of pgstats.  Entries are created each time a process is spawned, and are
 * dropped when the process exits.  These are not written to the pgstats file
 * on disk.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_backend.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/pgstat_internal.h"

/*
 * Returns statistics of a backend by proc number.
 */
PgStat_Backend *
pgstat_fetch_stat_backend(ProcNumber procNumber)
{
	PgStat_Backend *backend_entry;

	backend_entry = (PgStat_Backend *) pgstat_fetch_entry(PGSTAT_KIND_BACKEND,
														  InvalidOid, procNumber,
														  NULL);

	return backend_entry;
}

/*
 * Returns statistics of a backend by pid.
 *
 * This routine includes sanity checks to ensure that the backend exists and
 * is running.  "bktype" can be optionally defined to return the BackendType
 * of the backend whose statistics are returned.
 */
PgStat_Backend *
pgstat_fetch_stat_backend_by_pid(int pid, BackendType *bktype)
{
	PGPROC	   *proc;
	PgBackendStatus *beentry;
	ProcNumber	procNumber;
	PgStat_Backend *backend_stats;

	proc = BackendPidGetProc(pid);
	if (bktype)
		*bktype = B_INVALID;

	/* this could be an auxiliary process */
	if (!proc)
		proc = AuxiliaryPidGetProc(pid);

	if (!proc)
		return NULL;

	procNumber = GetNumberFromPGProc(proc);

	beentry = pgstat_get_beentry_by_proc_number(procNumber);
	if (!beentry)
		return NULL;

	/* check if the backend type tracks statistics */
	if (!pgstat_tracks_backend_bktype(beentry->st_backendType))
		return NULL;

	/* if PID does not match, leave */
	if (beentry->st_procpid != pid)
		return NULL;

	if (bktype)
		*bktype = beentry->st_backendType;

	/*
	 * Retrieve the entry.  Note that "beentry" may be freed depending on the
	 * value of stats_fetch_consistency, so do not access it from this point.
	 */
	backend_stats = pgstat_fetch_stat_backend(procNumber);
	if (!backend_stats)
	{
		if (bktype)
			*bktype = B_INVALID;
		return NULL;
	}

	return backend_stats;
}

/*
 * Create backend statistics entry for proc number.
 */
void
pgstat_create_backend(ProcNumber procnum)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Backend *shstatent;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_BACKEND, InvalidOid,
											procnum, false);
	shstatent = (PgStatShared_Backend *) entry_ref->shared_stats;

	/*
	 * NB: need to accept that there might be stats from an older backend,
	 * e.g. if we previously used this proc number.
	 */
	memset(&shstatent->stats, 0, sizeof(shstatent->stats));
	pgstat_unlock_entry(entry_ref);
}

/*
 * Backend statistics are not collected for all BackendTypes.
 *
 * The following BackendTypes do not participate in the backend stats
 * subsystem:
 * - The same and for the same reasons as in pgstat_tracks_io_bktype().
 * - B_BG_WRITER, B_CHECKPOINTER, B_STARTUP and B_AUTOVAC_LAUNCHER because their
 * I/O stats are already visible in pg_stat_io and there is only one of those.
 *
 * Function returns true if BackendType participates in the backend stats
 * subsystem and false if it does not.
 *
 * When adding a new BackendType, also consider adding relevant restrictions to
 * pgstat_tracks_io_object() and pgstat_tracks_io_op().
 */
bool
pgstat_tracks_backend_bktype(BackendType bktype)
{
	/*
	 * List every type so that new backend types trigger a warning about
	 * needing to adjust this switch.
	 */
	switch (bktype)
	{
		case B_INVALID:
		case B_AUTOVAC_LAUNCHER:
		case B_DEAD_END_BACKEND:
		case B_ARCHIVER:
		case B_LOGGER:
		case B_BG_WRITER:
		case B_CHECKPOINTER:
		case B_IO_WORKER:
		case B_STARTUP:
		case B_DATACHECKSUMSWORKER_LAUNCHER:
		case B_DATACHECKSUMSWORKER_WORKER:
			return false;

		case B_AUTOVAC_WORKER:
		case B_BACKEND:
		case B_BG_WORKER:
		case B_STANDALONE_BACKEND:
		case B_SLOTSYNC_WORKER:
		case B_WAL_RECEIVER:
		case B_WAL_SENDER:
		case B_WAL_SUMMARIZER:
		case B_WAL_WRITER:
			return true;
	}

	return false;
}

void
pgstat_backend_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Backend *) header)->stats.stat_reset_timestamp = ts;
}
