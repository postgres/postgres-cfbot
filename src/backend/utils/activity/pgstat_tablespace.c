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
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_tablespace.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_tablespace_d.h"
#include "common/relpath.h"
#include "storage/fd.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"


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
 * Count time spent reading blocks in a tablespace.
 *
 * "io_start" is the value returned by pgstat_prepare_io_time(); it is zero
 * when I/O timing is disabled, in which case we must not read the clock at
 * all -- doing so would both cost time on a hot path and, worse, produce a
 * bogus duration measured from the epoch.
 */
void
pgstat_count_tablespace_blk_read_time(Oid spcoid, instr_time io_start)
{
	PgStat_StatTabspaceEntry *tsent;
	instr_time	io_time;

	if (INSTR_TIME_IS_ZERO(io_start) || !OidIsValid(spcoid))
		return;

	INSTR_TIME_SET_CURRENT(io_time);
	INSTR_TIME_SUBTRACT(io_time, io_start);

	tsent = pgstat_prep_tablespace_pending(spcoid);
	tsent->blk_read_time += INSTR_TIME_GET_MICROSEC(io_time);
}

/*
 * Count time spent writing or extending blocks in a tablespace.
 *
 * See pgstat_count_tablespace_blk_read_time() for the io_start convention.
 */
void
pgstat_count_tablespace_blk_write_time(Oid spcoid, instr_time io_start)
{
	PgStat_StatTabspaceEntry *tsent;
	instr_time	io_time;

	if (INSTR_TIME_IS_ZERO(io_start) || !OidIsValid(spcoid))
		return;

	INSTR_TIME_SET_CURRENT(io_time);
	INSTR_TIME_SUBTRACT(io_time, io_start);

	tsent = pgstat_prep_tablespace_pending(spcoid);
	tsent->blk_write_time += INSTR_TIME_GET_MICROSEC(io_time);
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
