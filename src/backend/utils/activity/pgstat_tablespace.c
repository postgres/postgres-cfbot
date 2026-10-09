/* -------------------------------------------------------------------------
 *
 * pgstat_tablespace.c
 *	  Implementation of tablespace statistics.
 *
 * This file contains the implementation of tablespace statistics.  It is kept
 * separate from other statistics implementations for the sake of readability.
 *
 * Tablespace statistics are aggregated from several sources: block and tuple
 * counts are folded in when relation and index statistics are flushed, block
 * I/O times are counted along with the I/O statistics, and temporary file
 * usage is reported by fd.c.
 *
 * All of them are accumulated in process-local memory first, and are added to
 * the shared entries by pgstat_flush_tablespace().  The ordinary pending entry
 * mechanism does not fit: pgstat_count_io_op_time_ext() also runs in the
 * checkpointer and the background writer, which never call
 * pgstat_report_stat(), and a pending entry created there would pin the
 * shared entry for the life of the process.
 *
 * The flush never creates a shared entry.  By then the tablespace may have
 * been dropped, and an entry created for it would never be removed.  Entries
 * are created by pgstat_ensure_tablespace_entry() instead, at points where
 * the tablespace cannot be dropped: in CREATE TABLESPACE, when a backend
 * starts counting for a relation it has open, when a temporary file is
 * created, and when a process counts its first block I/O time for the
 * tablespace since its last flush.  Counts for a tablespace without an entry
 * are discarded, which is what happens to a tablespace dropped in the
 * meantime.
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
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


/*
 * Counts not yet flushed to shared memory, one element per tablespace.
 *
 * This is a plain array, searched linearly.  A process works with few
 * tablespaces between two flushes, and pgstat_count_tablespace_io_op_time()
 * needs its element for every timed block I/O.  I/O often alternates between
 * tablespaces, for instance between a table and its index, or when the
 * checkpointer balances its writes, so remembering only the tablespace used
 * last in front of a hash table would not do.
 */
typedef struct PgStat_PendingTabspace
{
	Oid			spcoid;
	PgStat_StatTabspaceEntry counts;
} PgStat_PendingTabspace;

static PgStat_PendingTabspace *pending_tabspaces = NULL;
static int	npending_tabspaces = 0;
static int	maxpending_tabspaces = 0;


/*
 * Register the creation of a tablespace with the cumulative stats system.
 *
 * The entry is created right away, and is removed again if the transaction
 * aborts.  Any statistics left over from an earlier tablespace that happened
 * to have the same OID are reset.
 */
void
pgstat_create_tablespace(Oid spcoid)
{
	pgstat_create_transactional(PGSTAT_KIND_TABLESPACE, InvalidOid, spcoid);
	(void) pgstat_get_entry_ref(PGSTAT_KIND_TABLESPACE, InvalidOid, spcoid,
								true, NULL);
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
 * Make sure the shared entry for a tablespace exists.
 *
 * This must only be called while something keeps the tablespace from being
 * dropped, such as a file in it that is in use.  A call made after the drop
 * would create an entry that nothing removes.
 */
void
pgstat_ensure_tablespace_entry(Oid spcoid)
{
	static Oid	last_spcoid = InvalidOid;

	Assert(OidIsValid(spcoid));

	/*
	 * This is called for every relation a backend starts counting for.  An
	 * entry only goes away together with its tablespace, so there is no need
	 * to look again for the tablespace seen last.
	 */
	if (spcoid == last_spcoid)
		return;

	(void) pgstat_get_entry_ref(PGSTAT_KIND_TABLESPACE, InvalidOid, spcoid,
								true, NULL);
	last_spcoid = spcoid;
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
 * Find or create the process-local counts for a tablespace.
 *
 * If "ensure_entry" is set, the shared entry is created along with the local
 * counts.  See pgstat_ensure_tablespace_entry() for when that is allowed.
 */
static PgStat_PendingTabspace *
pgstat_get_pending_tablespace(Oid spcoid, bool ensure_entry)
{
	PgStat_PendingTabspace *pending;

	Assert(OidIsValid(spcoid));

	for (int i = 0; i < npending_tabspaces; i++)
	{
		if (pending_tabspaces[i].spcoid == spcoid)
			return &pending_tabspaces[i];
	}

	if (ensure_entry)
		pgstat_ensure_tablespace_entry(spcoid);

	if (npending_tabspaces == maxpending_tabspaces)
	{
		int			newmax = Max(8, maxpending_tabspaces * 2);

		if (pending_tabspaces == NULL)
			pending_tabspaces = MemoryContextAlloc(TopMemoryContext,
												   newmax * sizeof(PgStat_PendingTabspace));
		else
			pending_tabspaces = repalloc(pending_tabspaces,
										 newmax * sizeof(PgStat_PendingTabspace));
		maxpending_tabspaces = newmax;
	}

	pending = &pending_tabspaces[npending_tabspaces++];
	pending->spcoid = spcoid;
	memset(&pending->counts, 0, sizeof(pending->counts));

	return pending;
}

/*
 * Prepare for reporting tablespace stats.
 *
 * The returned pointer is only good until the next call or flush.  This does
 * not create the shared entry, see pgstat_ensure_tablespace_entry().
 */
PgStat_StatTabspaceEntry *
pgstat_prep_tablespace_pending(Oid spcoid)
{
	/* make sure pgstat_report_stat() gets to pgstat_flush_tablespace() */
	pgstat_report_fixed = true;

	return &pgstat_get_pending_tablespace(spcoid, false)->counts;
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
 * Count the time of a block I/O against a tablespace.
 *
 * Called from pgstat_count_io_op_time_ext(), only when I/O timing is enabled.
 * As in pg_stat_database, reads count as read time, and writes and extends
 * count as write time.
 *
 * The caller has just done I/O on a relation in the tablespace, which thus
 * cannot be dropped, so the shared entry can be created from here.  This may
 * be all that is ever counted for the tablespace, for instance in the
 * checkpointer of a standby that runs no queries.
 */
void
pgstat_count_tablespace_io_op_time(Oid spcoid, IOOp io_op, instr_time io_time)
{
	PgStat_PendingTabspace *pending = pgstat_get_pending_tablespace(spcoid, true);

	if (io_op == IOOP_READ)
		pending->counts.blk_read_time += INSTR_TIME_GET_MICROSEC(io_time);
	else if (io_op == IOOP_WRITE || io_op == IOOP_EXTEND)
		pending->counts.blk_write_time += INSTR_TIME_GET_MICROSEC(io_time);
}

/*
 * Flush out process-local counts.
 *
 * Returns true if some of them could not be flushed due to lock contention;
 * those are kept and retried on the next call.
 *
 * Shared entries are not created here, see the file header.  Counts for a
 * tablespace that has no entry are dropped.
 */
bool
pgstat_flush_tablespace(bool nowait)
{
	int			nkept = 0;

	/*
	 * Let go of references to entries dropped since the last call.  The
	 * lookups in this file keep a reference to each entry they find, but
	 * dropped entries are only noticed in pgstat_get_entry_ref().  If the
	 * checkpointer still held such a reference at shutdown, the dropped entry
	 * would be there when the stats file is written.
	 */
	if (pgstat_need_entry_refs_gc())
		pgstat_gc_entry_refs();

	for (int i = 0; i < npending_tabspaces; i++)
	{
		PgStat_PendingTabspace *pending = &pending_tabspaces[i];
		PgStat_EntryRef *entry_ref;
		PgStat_StatTabspaceEntry *shent;

		entry_ref = pgstat_get_entry_ref(PGSTAT_KIND_TABLESPACE, InvalidOid,
										 pending->spcoid, false, NULL);
		if (entry_ref == NULL)
			continue;

		if (!pgstat_lock_entry(entry_ref, nowait))
		{
			/* keep it for the next attempt */
			pending_tabspaces[nkept++] = *pending;
			continue;
		}

		shent = &((PgStatShared_Tablespace *) entry_ref->shared_stats)->stats;

#define PGSTAT_ACCUM_TABSPACECOUNT(item) \
	(shent)->item += (pending->counts).item

		PGSTAT_ACCUM_TABSPACECOUNT(blocks_fetched);
		PGSTAT_ACCUM_TABSPACECOUNT(blocks_hit);
		PGSTAT_ACCUM_TABSPACECOUNT(blk_read_time);
		PGSTAT_ACCUM_TABSPACECOUNT(blk_write_time);
		PGSTAT_ACCUM_TABSPACECOUNT(temp_files);
		PGSTAT_ACCUM_TABSPACECOUNT(temp_bytes);
		PGSTAT_ACCUM_TABSPACECOUNT(tuples_returned);
		PGSTAT_ACCUM_TABSPACECOUNT(tuples_fetched);
		PGSTAT_ACCUM_TABSPACECOUNT(tuples_inserted);
		PGSTAT_ACCUM_TABSPACECOUNT(tuples_updated);
		PGSTAT_ACCUM_TABSPACECOUNT(tuples_deleted);

#undef PGSTAT_ACCUM_TABSPACECOUNT

		pgstat_unlock_entry(entry_ref);
	}

	npending_tabspaces = nkept;

	return nkept > 0;
}

/*
 * Reset stats reset timestamp.
 */
void
pgstat_tablespace_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Tablespace *) header)->stats.stat_reset_timestamp = ts;
}
