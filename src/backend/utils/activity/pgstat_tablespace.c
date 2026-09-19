/* -------------------------------------------------------------------------
 *
 * pgstat_tablespace.c
 *	  Implementation of tablespace statistics.
 *
 * This file contains the implementation of tablespace statistics.  It is kept
 * separate from other statistics implementations for the sake of readability.
 *
 * Tablespace statistics are aggregated from several sources: block and tuple
 * counts are folded in when relation and index statistics are flushed, I/O
 * timings are reported by the buffer manager, and temporary file usage is
 * reported by fd.c.
 *
 * Relation, index and temporary file reports all happen in processes that call
 * pgstat_report_stat(), so they use the ordinary pending entry mechanism.
 * Block I/O timings cannot: the buffer manager also reports them from the
 * checkpointer and the background writer, which never call that function.  A
 * PgStat_EntryRef->pending entry created there would never be flushed, and
 * would pin the shared entry for the life of the process, so a later DROP
 * TABLESPACE could not free it (see the "cannot gc shared ref that has pending
 * data" case in pgstat_gc_entry_refs()).  They are therefore accumulated in
 * process-local memory and flushed through flush_static_cb, as
 * PGSTAT_KIND_BACKEND does with its own per-process data.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_tablespace.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_tablespace_d.h"
#include "common/relpath.h"
#include "storage/fd.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/*
 * Block I/O timings not yet flushed to shared memory, keyed by tablespace.
 */
typedef struct PgStat_PendingTabspaceTime
{
	Oid			spcoid;			/* hash key, must be first */
	PgStat_Counter blk_read_time;	/* times in microseconds */
	PgStat_Counter blk_write_time;
} PgStat_PendingTabspaceTime;

static HTAB *pending_tabspace_times = NULL;


static PgStat_PendingTabspaceTime *pgstat_prep_tablespace_time(Oid spcoid);


/*
 * Register the creation of a tablespace with the cumulative stats system.
 *
 * This does not materialize the entry -- that happens the first time something
 * is reported for the tablespace.  What it does do is arrange for the entry to
 * be removed if the transaction aborts, and reset any statistics left over
 * from an earlier tablespace that happened to have the same OID.  The latter
 * matters because pgstat_flush_tablespace_times() can recreate an entry for a
 * tablespace that has just been dropped; see the comment there.
 */
void
pgstat_create_tablespace(Oid spcoid)
{
	pgstat_create_transactional(PGSTAT_KIND_TABLESPACE, InvalidOid, spcoid);
}

/*
 * Remove entry for the tablespace being dropped.
 */
void
pgstat_drop_tablespace(Oid spcoid)
{
	pgstat_drop_transactional(PGSTAT_KIND_TABLESPACE, InvalidOid, spcoid);
}

/*
 * Fetch tablespace statistics.
 */
PgStat_StatTabspaceEntry *
pgstat_fetch_stat_tabspaceentry(Oid spcoid)
{
	return (PgStat_StatTabspaceEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_TABLESPACE, InvalidOid, spcoid, NULL);
}

/*
 * Prepare for reporting tablespace stats.
 */
PgStat_StatTabspaceEntry *
pgstat_prep_tablespace_pending(Oid spcoid)
{
	PgStat_EntryRef *entry_ref;

	Assert(OidIsValid(spcoid));

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_TABLESPACE,
										  InvalidOid, spcoid, NULL);

	return (PgStat_StatTabspaceEntry *) entry_ref->pending;
}

/*
 * Determine which tablespace a temporary file belongs to, based on its path.
 *
 * fd.c does not remember the tablespace a temporary file was created in -- by
 * the time the file is deleted and its usage reported, only the path is still
 * available.  Rather than widen Vfd, we recover the OID from the path.
 *
 * TempTablespacePath() builds these paths, and produces just two shapes: one
 * rooted at PG_TBLSPC_DIR for a real tablespace, and one rooted in the data
 * directory for the default tablespace.  Rather than hard-code the latter, we
 * ask TempTablespacePath() itself what it looks like, so this stays correct if
 * the layout ever changes.  Note that it also maps the global tablespace onto
 * the default one, so temporary files never belong to pg_global.
 *
 * Returns InvalidOid if the path is not recognized, in which case the caller
 * simply does not attribute the file to any tablespace.
 */
Oid
pgstat_tablespace_from_tempfile_path(const char *path)
{
	char		defaultpath[MAXPGPATH];

	if (path == NULL)
		return InvalidOid;

	if (strncmp(path, PG_TBLSPC_DIR_SLASH, strlen(PG_TBLSPC_DIR_SLASH)) == 0)
		return atooid(path + strlen(PG_TBLSPC_DIR_SLASH));

	TempTablespacePath(defaultpath, DEFAULTTABLESPACE_OID);
	if (strncmp(path, defaultpath, strlen(defaultpath)) == 0)
		return DEFAULTTABLESPACE_OID;

	return InvalidOid;
}

/*
 * Find or create the process-local pending block I/O timings for a tablespace.
 */
static PgStat_PendingTabspaceTime *
pgstat_prep_tablespace_time(Oid spcoid)
{
	PgStat_PendingTabspaceTime *pending;
	bool		found;

	Assert(OidIsValid(spcoid));

	if (pending_tabspace_times == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(PgStat_PendingTabspaceTime);
		ctl.hcxt = TopMemoryContext;
		pending_tabspace_times = hash_create("Pending tablespace I/O timings",
											 8, &ctl,
											 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	pending = hash_search(pending_tabspace_times, &spcoid, HASH_ENTER, &found);
	if (!found)
	{
		pending->blk_read_time = 0;
		pending->blk_write_time = 0;
	}

	return pending;
}

/*
 * Count time spent reading blocks in a tablespace.
 *
 * "io_time" is the elapsed time returned by pgstat_count_io_op_time(), so
 * that the clock is read only once per I/O and this agrees exactly with what
 * was reported for the same I/O elsewhere.  It is zero when I/O timing is
 * disabled, in which case there is nothing to do.
 */
void
pgstat_count_tablespace_blk_read_time(Oid spcoid, instr_time io_time)
{
	PgStat_PendingTabspaceTime *pending;

	if (INSTR_TIME_IS_ZERO(io_time) || !OidIsValid(spcoid))
		return;

	pending = pgstat_prep_tablespace_time(spcoid);
	pending->blk_read_time += INSTR_TIME_GET_MICROSEC(io_time);

	pgstat_report_fixed = true;
}

/*
 * Count time spent writing or extending blocks in a tablespace.
 *
 * See pgstat_count_tablespace_blk_read_time() for the io_time convention.
 */
void
pgstat_count_tablespace_blk_write_time(Oid spcoid, instr_time io_time)
{
	PgStat_PendingTabspaceTime *pending;

	if (INSTR_TIME_IS_ZERO(io_time) || !OidIsValid(spcoid))
		return;

	pending = pgstat_prep_tablespace_time(spcoid);
	pending->blk_write_time += INSTR_TIME_GET_MICROSEC(io_time);

	pgstat_report_fixed = true;
}

/*
 * Flush out process-local block I/O timings.
 *
 * Returns true if some of them could not be flushed due to lock contention;
 * those are kept and retried on the next call.
 *
 * The shared entry is created if it does not exist.  Not creating it is not an
 * option: this also runs in the checkpointer and the background writer, and on
 * a standby -- or on a tablespace whose relations have not reported anything
 * yet -- nothing else would have created it, so the timings would be dropped
 * on the floor.  Note that pgstat_create_tablespace() does not help here, as
 * it only arranges for the entry to be removed should the transaction abort.
 *
 * The cost is that a tablespace dropped between the last I/O and this flush
 * gets an entry created for an OID that no longer exists.  Such an entry holds
 * nothing but the in-flight timings and is invisible in pg_stat_tablespace,
 * which joins against pg_tablespace, but it does persist in the stats file.
 * Should the OID ever be reused, pgstat_create_tablespace() resets it.
 */
bool
pgstat_flush_tablespace_times(bool nowait)
{
	HASH_SEQ_STATUS hstat;
	PgStat_PendingTabspaceTime *pending;
	bool		partial_flush = false;

	if (pending_tabspace_times == NULL ||
		hash_get_num_entries(pending_tabspace_times) == 0)
		return false;

	hash_seq_init(&hstat, pending_tabspace_times);
	while ((pending = hash_seq_search(&hstat)) != NULL)
	{
		PgStat_EntryRef *entry_ref;
		PgStatShared_Tablespace *shent;
		Oid			spcoid = pending->spcoid;

		entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_TABLESPACE,
												InvalidOid, spcoid, nowait);
		if (entry_ref == NULL)
		{
			partial_flush = true;
			continue;
		}

		shent = (PgStatShared_Tablespace *) entry_ref->shared_stats;
		shent->stats.blk_read_time += pending->blk_read_time;
		shent->stats.blk_write_time += pending->blk_write_time;

		pgstat_unlock_entry(entry_ref);

		/* deleting the entry the scan is sitting on is allowed */
		(void) hash_search(pending_tabspace_times, &spcoid, HASH_REMOVE, NULL);
	}

	return partial_flush;
}

/*
 * Flush out pending stats for the entry.
 */
bool
pgstat_tablespace_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStatShared_Tablespace *sharedent;
	PgStat_StatTabspaceEntry *pendingent;

	pendingent = (PgStat_StatTabspaceEntry *) entry_ref->pending;
	sharedent = (PgStatShared_Tablespace *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

#define PGSTAT_ACCUM_TABSPACECOUNT(item)		\
	(sharedent)->stats.item += (pendingent)->item

	/* blk_read_time and blk_write_time are flushed separately, see above */
	PGSTAT_ACCUM_TABSPACECOUNT(blocks_fetched);
	PGSTAT_ACCUM_TABSPACECOUNT(blocks_hit);
	PGSTAT_ACCUM_TABSPACECOUNT(temp_files);
	PGSTAT_ACCUM_TABSPACECOUNT(temp_bytes);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_returned);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_fetched);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_inserted);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_updated);
	PGSTAT_ACCUM_TABSPACECOUNT(tuples_deleted);

#undef PGSTAT_ACCUM_TABSPACECOUNT

	pgstat_unlock_entry(entry_ref);

	memset(pendingent, 0, sizeof(*pendingent));

	return true;
}

/*
 * Reset stats reset timestamp.
 */
void
pgstat_tablespace_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Tablespace *) header)->stats.stat_reset_timestamp = ts;
}
