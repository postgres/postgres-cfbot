/* -------------------------------------------------------------------------
 *
 * pgstat_logmsg.c
 *	  Implementation of log message statistics.
 *
 * This file contains the implementation of log message statistics.  It is
 * kept separate from pgstat.c to enforce the line between the statistics
 * access / storage implementation and the details about individual types of
 * statistics.
 *
 * Counts of messages emitted to the server log are kept grouped by
 * (backend type, database, user, severity level, SQLSTATE), capped at
 * PGSTAT_LOGMSG_MAX_ENTRIES distinct combinations.  Once the table is full,
 * already-tracked combinations keep counting while messages for new
 * combinations are accounted for in n_dropped.
 *
 * The entries are kept in an index-based separate-chaining hash table laid
 * out entirely inside PgStat_LogMsgStats (see pgstat.h).  Because chain
 * links are array indices rather than pointers, the struct stays valid when
 * the fixed-amount stats machinery copies it around: snapshots are taken
 * with a raw memcpy under the changecount protocol and the stats file is
 * written/restored verbatim.  This is also why none of the standard hash
 * table implementations (dynahash, simplehash, dshash) are used here: they
 * all rely on process-local pointers or external allocations.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_logmsg.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 * Expose the errcodes_names[] lookup table when errcodes_names.h is pulled
 * in via pgstat.h below; other files only see ERRCODES_NAMES_COUNT.
 */
#define ERRCODES_NAMES_INCLUDE_TABLE

#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"

/* the table and the count come from the same generated header */
StaticAssertDecl(lengthof(errcodes_names) == ERRCODES_NAMES_COUNT + 1,
				 "errcodes_names table does not match ERRCODES_NAMES_COUNT");

/* Minimum message severity level to track; see also guc_parameters.dat */
int			pgstat_track_logmsg = PGSTAT_LOGMSG_TRACK_NONE;

/*
 * Prevent recursion should updating the stats itself cause a message to be
 * logged.
 */
static bool pgstat_logmsg_counting = false;


/*
 * Hash a log message signature into a uint32, used to pick a chain bucket.
 */
static inline uint32
pgstat_logmsg_hash(BackendType backend_type, Oid dboid, Oid userid,
				   int elevel, int sqlerrcode)
{
	uint32		h;

	h = murmurhash32((uint32) backend_type);
	h = hash_combine(h, murmurhash32((uint32) dboid));
	h = hash_combine(h, murmurhash32((uint32) userid));
	h = hash_combine(h, murmurhash32((uint32) elevel));
	h = hash_combine(h, murmurhash32((uint32) sqlerrcode));
	return h;
}

/*
 * Count one message emitted to the server log.
 *
 * Called by EmitErrorReport() for messages with output_to_server set.  This
 * runs in any process type, possibly very early or very late in its
 * lifetime, so be careful about what infrastructure is relied upon here.
 *
 * The chain walk is O(1) expected (average chain length equals the load
 * factor) for lookup, insert, and drop alike, even when the table is full.
 */
void
pgstat_count_logmsg(ErrorData *edata)
{
	PgStatShared_LogMsg *stats_shmem;
	PgStat_LogMsgStats *stats;
	Oid			dboid;
	Oid			userid;
	int			sec_context;
	uint32		hash;
	uint32		bucket;
	int32		idx;
	bool		found;

	if (pgstat_track_logmsg == PGSTAT_LOGMSG_TRACK_NONE ||
		edata->elevel < pgstat_track_logmsg)
		return;

	/* stats shared memory might not be set up yet, or already torn down */
	if (pgStatLocal.shmem == NULL || pgStatLocal.shmem->is_shutdown)
		return;

	/* cannot take LWLocks without a PGPROC (e.g. in the postmaster) */
	if (!MyProc)
		return;

	if (pgstat_logmsg_counting)
		return;
	pgstat_logmsg_counting = true;

	stats_shmem = &pgStatLocal.shmem->logmsg;
	stats = &stats_shmem->stats;

	dboid = MyDatabaseId;

	/*
	 * Use GetUserIdAndSecContext() rather than GetUserId(): the latter
	 * asserts that a user ID has been set, which is not the case for messages
	 * emitted before authentication completes or in auxiliary processes.
	 * Here InvalidOid is fine and simply means "no user".
	 */
	GetUserIdAndSecContext(&userid, &sec_context);

	hash = pgstat_logmsg_hash(MyBackendType, dboid, userid,
							  edata->elevel, edata->sqlerrcode);
	bucket = hash % PGSTAT_LOGMSG_MAX_ENTRIES;

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);

	found = false;
	for (idx = stats->heads[bucket]; idx != -1; idx = stats->entries[idx].next)
	{
		PgStat_LogMsgEntry *entry = &stats->entries[idx];

		if (entry->backend_type == MyBackendType &&
			entry->dboid == dboid &&
			entry->userid == userid &&
			entry->elevel == edata->elevel &&
			entry->sqlerrcode == edata->sqlerrcode)
		{
			pgstat_begin_changecount_write(&stats_shmem->changecount);
			entry->count++;
			pgstat_end_changecount_write(&stats_shmem->changecount);
			found = true;
			break;
		}
	}

	if (!found)
	{
		if (stats->num_entries < PGSTAT_LOGMSG_MAX_ENTRIES)
		{
			int32		newidx = stats->num_entries;
			PgStat_LogMsgEntry *entry = &stats->entries[newidx];

			pgstat_begin_changecount_write(&stats_shmem->changecount);
			entry->backend_type = MyBackendType;
			entry->dboid = dboid;
			entry->userid = userid;
			entry->elevel = edata->elevel;
			entry->sqlerrcode = edata->sqlerrcode;
			entry->count = 1;
			entry->next = stats->heads[bucket];
			stats->heads[bucket] = newidx;
			stats->num_entries++;
			pgstat_end_changecount_write(&stats_shmem->changecount);
		}
		else
		{
			pgstat_begin_changecount_write(&stats_shmem->changecount);
			stats->n_dropped++;
			pgstat_end_changecount_write(&stats_shmem->changecount);
		}
	}

	LWLockRelease(&stats_shmem->lock);

	pgstat_logmsg_counting = false;
}

/*
 * Return the condition name for a SQLSTATE error code as listed in
 * errcodes.txt, or NULL if the code has no name (e.g. custom codes raised
 * from user code).
 */
const char *
pgstat_get_logmsg_errcode_name(int sqlerrcode)
{
	for (int i = 0; errcodes_names[i].name != NULL; i++)
	{
		if (errcodes_names[i].sqlerrcode == sqlerrcode)
			return errcodes_names[i].name;
	}
	return NULL;
}

/*
 * Support function for the SQL-callable pgstat* functions.  Returns a
 * pointer to the log message statistics struct.
 */
PgStat_LogMsgStats *
pgstat_fetch_stat_logmsg(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_LOGMSG);

	return &pgStatLocal.snapshot.logmsg;
}

void
pgstat_logmsg_init_shmem_cb(void *stats)
{
	PgStatShared_LogMsg *stats_shmem = (PgStatShared_LogMsg *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);

	/* mark all bucket chains as empty */
	memset(stats_shmem->stats.heads, 0xFF,
		   sizeof(stats_shmem->stats.heads));
}

void
pgstat_logmsg_reset_all_cb(TimestampTz ts)
{
	PgStatShared_LogMsg *stats_shmem = &pgStatLocal.shmem->logmsg;
	PgStat_LogMsgStats *stats = &stats_shmem->stats;

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);

	/*
	 * Empty all bucket chains and reset num_entries so that entries are
	 * reclaimed for reuse; otherwise, once the table fills up, a reset would
	 * not free capacity for new distinct combinations.
	 */
	pgstat_begin_changecount_write(&stats_shmem->changecount);
	stats->num_entries = 0;
	stats->n_dropped = 0;
	memset(stats->heads, 0xFF, sizeof(stats->heads));
	stats->stat_reset_timestamp = ts;
	pgstat_end_changecount_write(&stats_shmem->changecount);

	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_logmsg_snapshot_cb(void)
{
	PgStatShared_LogMsg *stats_shmem = &pgStatLocal.shmem->logmsg;

	pgstat_copy_changecounted_stats(&pgStatLocal.snapshot.logmsg,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
}
