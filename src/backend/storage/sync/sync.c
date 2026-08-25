/*-------------------------------------------------------------------------
 *
 * sync.c
 *	  File synchronization management code.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/sync/sync.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/multixact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/bgwriter.h"
#include "storage/aio.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/md.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/*
 * In some contexts (currently, standalone backends and the checkpointer)
 * we keep track of pending fsync operations: we need to remember all relation
 * segments that have been written since the last checkpoint, so that we can
 * fsync them down to disk before completing the next checkpoint.  This hash
 * table remembers the pending operations.  We use a hash table mostly as
 * a convenient way of merging duplicate requests.
 *
 * We use a similar mechanism to remember no-longer-needed files that can
 * be deleted after the next checkpoint, but we use a linked list instead of
 * a hash table, because we don't expect there to be any duplicate requests.
 *
 * These mechanisms are only used for non-temp relations; we never fsync
 * temp rels, nor do we need to postpone their deletion (see comments in
 * mdunlink).
 *
 * (Regular backends do not track pending operations locally, but forward
 * them to the checkpointer.)
 */
typedef uint16 CycleCtr;		/* can be any convenient integer size */

typedef struct PendingFsyncEntry
{
	FileTag		tag;			/* identifies handler and file */
	CycleCtr	cycle_ctr;		/* sync_cycle_ctr of oldest request */
	bool		canceled;		/* canceled is true if we canceled "recently" */

	/*
	 * Set when a request arrives for a tag that already has an entry, and
	 * cleared whenever an fsync for it is started.  If it is set once that
	 * fsync completes, the request came in while the fsync was in flight, so
	 * the fsync cannot be assumed to have covered it.
	 */
	bool		re_requested;

	/* fsync is done, pending hash-table bookkeeping */
	bool		sync_completed;
} PendingFsyncEntry;

typedef struct
{
	FileTag		tag;			/* identifies handler and file */
	CycleCtr	cycle_ctr;		/* checkpoint_cycle_ctr when request was made */
	bool		canceled;		/* true if request has been canceled */
} PendingUnlinkEntry;

/*
 * Transient state used while processing a batch of fsync requests.  A single
 * SyncState instance lives on the stack of ProcessSyncRequests() so that no
 * partial state survives across calls.
 */
typedef struct SyncState
{
	dlist_head	inflight;		/* InflightSyncEntry being fsync'd right now */
	dlist_head	retry;			/* InflightSyncEntry to be retried */
	int			inflight_count; /* number of entries in "inflight" */
	int			max_inflight;	/* max number of concurrent fsyncs */
	int			absorb_counter;

	/* stats */
	int			processed;
	instr_time	longest;
	instr_time	total_elapsed;
} SyncState;

static HTAB *pendingOps = NULL;
static List *pendingUnlinks = NIL;
static MemoryContext pendingOpsCxt; /* context for the above  */

/*
 * Context for the InflightSyncEntry structs allocated while a batch of fsync
 * requests is being processed.  It is kept separate from pendingOpsCxt (which
 * must survive for the lifetime of the process, as it holds pendingOps
 * itself), so that it can be reset between batches.
 */
static MemoryContext inflightSyncCxt;

/*
 * All InflightSyncEntry structs that have not yet been freed.  Unlike the
 * lists in SyncState, this survives an error so that handler-owned files can
 * be closed before their entries are discarded.
 */
static dlist_head activeSyncEntries = DLIST_STATIC_INIT(activeSyncEntries);

static CycleCtr sync_cycle_ctr = 0;
static CycleCtr checkpoint_cycle_ctr = 0;

/* Intervals for calling AbsorbSyncRequests */
#define FSYNCS_PER_ABSORB		10
#define UNLINKS_PER_ABSORB		10

/*
 * Function pointers for handling sync and unlink requests.
 */
typedef struct SyncOps
{
	void		(*sync_syncfiletag) (PgAioHandle *ioh, InflightSyncEntry *entry);
	int			(*sync_unlinkfiletag) (const FileTag *ftag, char *path);
	bool		(*sync_filetagmatches) (const FileTag *ftag,
										const FileTag *candidate);
} SyncOps;

/*
 * These indexes must correspond to the values of the SyncRequestHandler enum.
 */
static const SyncOps syncsw[] = {
	/* magnetic disk */
	[SYNC_HANDLER_MD] = {
		.sync_syncfiletag = mdsyncfiletag,
		.sync_unlinkfiletag = mdunlinkfiletag,
		.sync_filetagmatches = mdfiletagmatches
	},
	/* pg_xact */
	[SYNC_HANDLER_CLOG] = {
		.sync_syncfiletag = clogsyncfiletag
	},
	/* pg_commit_ts */
	[SYNC_HANDLER_COMMIT_TS] = {
		.sync_syncfiletag = committssyncfiletag
	},
	/* pg_multixact/offsets */
	[SYNC_HANDLER_MULTIXACT_OFFSET] = {
		.sync_syncfiletag = multixactoffsetssyncfiletag
	},
	/* pg_multixact/members */
	[SYNC_HANDLER_MULTIXACT_MEMBER] = {
		.sync_syncfiletag = multixactmemberssyncfiletag
	}
};

/*
 * Initialize data structures for the file sync tracking.
 */
void
InitSync(void)
{
	/*
	 * Create pending-operations hashtable if we need it.  Currently, we need
	 * it if we are standalone (not under a postmaster) or if we are a
	 * checkpointer auxiliary process.
	 */
	if (!IsUnderPostmaster || AmCheckpointerProcess())
	{
		HASHCTL		hash_ctl;

		/*
		 * XXX: The checkpointer needs to add entries to the pending ops table
		 * when absorbing fsync requests.  That is done within a critical
		 * section, which isn't usually allowed, but we make an exception. It
		 * means that there's a theoretical possibility that you run out of
		 * memory while absorbing fsync requests, which leads to a PANIC.
		 * Fortunately the hash table is small so that's unlikely to happen in
		 * practice.
		 */
		pendingOpsCxt = AllocSetContextCreate(TopMemoryContext,
											  "Pending ops context",
											  ALLOCSET_DEFAULT_SIZES);
		MemoryContextAllowInCriticalSection(pendingOpsCxt, true);

		hash_ctl.keysize = sizeof(FileTag);
		hash_ctl.entrysize = sizeof(PendingFsyncEntry);
		hash_ctl.hcxt = pendingOpsCxt;
		pendingOps = hash_create("Pending Ops Table",
								 100L,
								 &hash_ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		pendingUnlinks = NIL;

		inflightSyncCxt = AllocSetContextCreate(TopMemoryContext,
												"Inflight sync context",
												ALLOCSET_DEFAULT_SIZES);
	}
}

/*
 * SyncPreCheckpoint() -- Do pre-checkpoint work
 *
 * To distinguish unlink requests that arrived before this checkpoint
 * started from those that arrived during the checkpoint, we use a cycle
 * counter similar to the one we use for fsync requests. That cycle
 * counter is incremented here.
 *
 * This must be called *before* the checkpoint REDO point is determined.
 * That ensures that we won't delete files too soon.  Since this calls
 * AbsorbSyncRequests(), which performs memory allocations, it cannot be
 * called within a critical section.
 *
 * Note that we can't do anything here that depends on the assumption
 * that the checkpoint will be completed.
 */
void
SyncPreCheckpoint(void)
{
	/*
	 * Operations such as DROP TABLESPACE assume that the next checkpoint will
	 * process all recently forwarded unlink requests, but if they aren't
	 * absorbed prior to advancing the cycle counter, they won't be processed
	 * until a future checkpoint.  The following absorb ensures that any
	 * unlink requests forwarded before the checkpoint began will be processed
	 * in the current checkpoint.
	 */
	AbsorbSyncRequests();

	/*
	 * Any unlink requests arriving after this point will be assigned the next
	 * cycle counter, and won't be unlinked until next checkpoint.
	 */
	checkpoint_cycle_ctr++;
}

/*
 * SyncPostCheckpoint() -- Do post-checkpoint work
 *
 * Remove any lingering files that can now be safely removed.
 */
void
SyncPostCheckpoint(void)
{
	int			absorb_counter;
	ListCell   *lc;

	absorb_counter = UNLINKS_PER_ABSORB;
	foreach(lc, pendingUnlinks)
	{
		PendingUnlinkEntry *entry = (PendingUnlinkEntry *) lfirst(lc);
		char		path[MAXPGPATH];

		/* Skip over any canceled entries */
		if (entry->canceled)
			continue;

		/*
		 * New entries are appended to the end, so if the entry is new we've
		 * reached the end of old entries.
		 *
		 * Note: if just the right number of consecutive checkpoints fail, we
		 * could be fooled here by cycle_ctr wraparound.  However, the only
		 * consequence is that we'd delay unlinking for one more checkpoint,
		 * which is perfectly tolerable.
		 */
		if (entry->cycle_ctr == checkpoint_cycle_ctr)
			break;

		/* Unlink the file */
		if (syncsw[entry->tag.handler].sync_unlinkfiletag(&entry->tag,
														  path) < 0)
		{
			/*
			 * There's a race condition, when the database is dropped at the
			 * same time that we process the pending unlink requests. If the
			 * DROP DATABASE deletes the file before we do, we will get ENOENT
			 * here. rmtree() also has to ignore ENOENT errors, to deal with
			 * the possibility that we delete the file first.
			 */
			if (errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}

		/* Mark the list entry as canceled, just in case */
		entry->canceled = true;

		/*
		 * As in ProcessSyncRequests, we don't want to stop absorbing fsync
		 * requests for a long time when there are many deletions to be done.
		 * We can safely call AbsorbSyncRequests() at this point in the loop.
		 */
		if (--absorb_counter <= 0)
		{
			AbsorbSyncRequests();
			absorb_counter = UNLINKS_PER_ABSORB;
		}
	}

	/*
	 * If we reached the end of the list, we can just remove the whole list
	 * (remembering to pfree all the PendingUnlinkEntry objects).  Otherwise,
	 * we must keep the entries at or after "lc".
	 */
	if (lc == NULL)
	{
		list_free_deep(pendingUnlinks);
		pendingUnlinks = NIL;
	}
	else
	{
		int			ntodelete = list_cell_number(pendingUnlinks, lc);

		for (int i = 0; i < ntodelete; i++)
			pfree(list_nth(pendingUnlinks, i));

		pendingUnlinks = list_delete_first_n(pendingUnlinks, ntodelete);
	}
}

/*
 * Close the file that a sync handler opened for an in-flight fsync.
 */
static void
sync_close_file(InflightSyncEntry *entry)
{
	switch (entry->close_method)
	{
		case SYNC_CLOSE_NONE:
			break;
		case SYNC_CLOSE_TRANSIENT:
			CloseTransientFile(entry->close_file);
			break;
		case SYNC_CLOSE_VFD:
			FileClose((File) entry->close_file);
			break;
		default:
			pg_unreachable();
	}

	entry->close_method = SYNC_CLOSE_NONE;
}

static void
sync_free_entry(InflightSyncEntry *entry)
{
	dlist_delete_from(&activeSyncEntries, &entry->cleanup_node);
	pfree(entry);
}

/*
 * Error cleanup callback for ProcessSyncRequests().
 */
static void
sync_cleanup_inflight(int code, Datum arg)
{
	while (!dlist_is_empty(&activeSyncEntries))
	{
		dlist_node *node = dlist_pop_head_node(&activeSyncEntries);
		InflightSyncEntry *entry;

		entry = dlist_container(InflightSyncEntry, cleanup_node, node);

		if (entry->started)
			pgaio_wref_wait(&entry->iow);

		sync_close_file(entry);
		pfree(entry);
	}
}

static void
sync_start_one(SyncState *sync_state, InflightSyncEntry *entry)
{
	struct PgAioHandle *ioh;
	instr_time	io_start;

	INSTR_TIME_SET_CURRENT(io_start);
	entry->start_time = io_start;

	entry->started = false;
	entry->open_errno = 0;
	entry->close_method = SYNC_CLOSE_NONE;
	pgaio_wref_clear(&entry->iow);

	/*
	 * Any request that arrives from here on may cover data that the fsync
	 * started below does not, so start out with a clean slate.  This has to
	 * happen before the IO is submitted; requests absorbed in between are
	 * covered by the fsync, so treating them as newer is merely conservative.
	 */
	entry->hash_entry->re_requested = false;

	ioh = pgaio_io_acquire(CurrentResourceOwner, &entry->ioret);
	pgaio_io_get_wref(ioh, &entry->iow);

	/*
	 * The handler opens the file, assigns the target and stages the fsync.
	 * Hold interrupts so that the referenced descriptor cannot be closed
	 * during submission.
	 */
	HOLD_INTERRUPTS();
	syncsw[entry->tag.handler].sync_syncfiletag(ioh, entry);
	RESUME_INTERRUPTS();

	if (!entry->started)
		pgaio_io_release(ioh);

	dlist_push_tail(&sync_state->inflight, &entry->node);
	sync_state->inflight_count++;
}

static void
sync_drain_one(SyncState *sync_state)
{
	dlist_node *node;
	InflightSyncEntry *entry;
	int			result;

	Assert(sync_state->inflight_count > 0);

	node = dlist_pop_head_node(&sync_state->inflight);
	entry = dlist_container(InflightSyncEntry, node, node);
	sync_state->inflight_count--;

	if (entry->started)
	{
		pgaio_wref_wait(&entry->iow);

		/*
		 * We did not register a completion callback, so the distilled status
		 * is always PGAIO_RS_OK and the raw fsync() return value (0 on
		 * success, -errno on failure) is available in ->result.result.
		 */
		result = -entry->ioret.result.result;
	}
	else
		result = entry->open_errno;

	sync_close_file(entry);

	if (!result)
	{
		instr_time	io_time;

		/*
		 * These values measure submission-to-reap time, not necessarily fsync
		 * duration.  Submission-order reaping can overstate individual
		 * durations, and the aggregate can exceed wall-clock time because
		 * fsyncs overlap.
		 */
		INSTR_TIME_SET_CURRENT(io_time);
		INSTR_TIME_SUBTRACT(io_time, entry->start_time);

		if (INSTR_TIME_GT(io_time, sync_state->longest))
			sync_state->longest = io_time;
		INSTR_TIME_ADD(sync_state->total_elapsed, io_time);
		sync_state->processed++;

		if (log_checkpoints)
			elog(DEBUG1, "checkpoint sync: number=%d file=%s time=%.3f ms",
				 sync_state->processed,
				 entry->path,
				 INSTR_TIME_GET_MILLISEC(io_time));

		entry->hash_entry->sync_completed = true;
		sync_free_entry(entry);
	}
	else
	{
		/*
		 * The request may have been canceled after we started the fsync, e.g.
		 * because the relation was dropped in the meantime and an intervening
		 * AbsorbSyncRequests() picked up the cancel message.  Since
		 * mdunlink() queues the "cancel" before actually unlinking, a
		 * cancellation means the failure is expected and the entry can simply
		 * be dropped.
		 *
		 * The upstream, synchronous code checked this at the top of its retry
		 * loop; because the fsync is now in flight while requests are being
		 * absorbed, we have to re-check it here.
		 */
		if (entry->hash_entry->canceled)
		{
			entry->hash_entry->sync_completed = true;
			sync_free_entry(entry);
			return;
		}

		/*
		 * It is possible that the relation has been dropped or truncated
		 * since the fsync request was entered. Therefore, allow ENOENT, but
		 * only if we didn't fail already on this file.
		 */
		errno = result;
		if (!FILE_POSSIBLY_DELETED(errno) || entry->retry_count > 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							entry->path)));
		else
			ereport(DEBUG1,
					(errcode_for_file_access(),
					 errmsg_internal("could not fsync file \"%s\" but retrying: %m",
									 entry->path)));

		entry->retry_count++;
		dlist_push_tail(&sync_state->retry, &entry->node);
	}
}

static void
sync_drain_all(SyncState *sync_state)
{
	while (sync_state->inflight_count)
		sync_drain_one(sync_state);
}

/*
 * Finish requests whose fsyncs have completed.
 *
 * The main hash scan may only remove the entry it most recently returned, so
 * completion processing is deferred until it ends.  This second scan can then
 * remove each completed entry as the current entry.  Recheck the hash entry
 * now because requests absorbed since the fsync completed may require it to
 * remain for the next checkpoint cycle.
 */
static void
sync_process_completed(void)
{
	HASH_SEQ_STATUS hstat;
	PendingFsyncEntry *entry;

	hash_seq_init(&hstat, pendingOps);
	while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
	{
		if (!entry->sync_completed)
			continue;

		/*
		 * We are done with this entry, unless a request for it arrived while
		 * the fsync was in flight.  A cancel supersedes any such request, as
		 * RememberSyncRequest() clears "canceled" when it records a new one.
		 */
		if (!entry->re_requested || entry->canceled)
		{
			if (hash_search(pendingOps, &entry->tag, HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "pendingOps corrupted");
		}
		else
			entry->sync_completed = false;
	}
}

/*
 * Reissue any fsync requests that previously failed with an ignorable error.
 *
 * The fsync table could contain requests to fsync segments that have been
 * deleted (unlinked) by the time we get to them. Rather than just hoping an
 * ENOENT (or EACCES on Windows) error can be ignored, what we do on error is
 * absorb pending requests and then retry. Since mdunlink() queues a "cancel"
 * message before actually unlinking, the fsync request is guaranteed to be
 * marked canceled after the absorb if it really was this case.
 */
static void
sync_process_retries(SyncState *sync_state)
{
	if (dlist_is_empty(&sync_state->retry))
		return;

	AbsorbSyncRequests();

	while (!dlist_is_empty(&sync_state->retry))
	{
		dlist_node *node = dlist_pop_head_node(&sync_state->retry);
		InflightSyncEntry *entry = dlist_container(InflightSyncEntry, node, node);

		if (entry->hash_entry->canceled)
		{
			/* Safe to remove here, the scan has already finished. */
			if (hash_search(pendingOps, &entry->hash_entry->tag,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "pendingOps corrupted");
			sync_free_entry(entry);
			continue;
		}

		Assert(sync_state->inflight_count <= sync_state->max_inflight);
		if (sync_state->inflight_count == sync_state->max_inflight)
			sync_drain_one(sync_state);

		sync_start_one(sync_state, entry);
	}

	sync_drain_all(sync_state);
	sync_process_completed();
}

/*
 * Process queued fsync requests.  The public wrapper ensures that any error
 * closes files owned by in-flight entries.
 */
static void
ProcessSyncRequestsInternal(void)
{
	static bool sync_in_progress = false;

	HASH_SEQ_STATUS hstat;
	PendingFsyncEntry *entry;
	SyncState	sync_state;

	/*
	 * This is only called during checkpoints, and checkpoints should only
	 * occur in processes that have created a pendingOps.
	 */
	if (!pendingOps)
		elog(ERROR, "cannot sync without a pendingOps table");

	/*
	 * If we are in the checkpointer, the sync had better include all fsync
	 * requests that were queued by backends up to this point.  The tightest
	 * race condition that could occur is that a buffer that must be written
	 * and fsync'd for the checkpoint could have been dumped by a backend just
	 * before it was visited by BufferSync().  We know the backend will have
	 * queued an fsync request before clearing the buffer's dirtybit, so we
	 * are safe as long as we do an Absorb after completing BufferSync().
	 */
	AbsorbSyncRequests();

	/*
	 * To avoid excess fsync'ing (in the worst case, maybe a never-terminating
	 * checkpoint), we want to ignore fsync requests that are entered into the
	 * hashtable after this point --- they should be processed next time,
	 * instead.  We use sync_cycle_ctr to tell old entries apart from new
	 * ones: new ones will have cycle_ctr equal to the incremented value of
	 * sync_cycle_ctr.
	 *
	 * In normal circumstances, all entries present in the table at this point
	 * will have cycle_ctr exactly equal to the current (about to be old)
	 * value of sync_cycle_ctr.  However, if we fail partway through the
	 * fsync'ing loop, then older values of cycle_ctr might remain when we
	 * come back here to try again.  Repeated checkpoint failures would
	 * eventually wrap the counter around to the point where an old entry
	 * might appear new, causing us to skip it, possibly allowing a checkpoint
	 * to succeed that should not have.  To forestall wraparound, any time the
	 * previous ProcessSyncRequests() failed to complete, run through the
	 * table and forcibly set cycle_ctr = sync_cycle_ctr.
	 *
	 * Think not to merge this loop with the main loop, as the problem is
	 * exactly that that loop may fail before having visited all the entries.
	 * From a performance point of view it doesn't matter anyway, as this path
	 * will never be taken in a system that's functioning normally.
	 */
	if (sync_in_progress)
	{
		/* prior try failed, so update any stale cycle_ctr values */
		hash_seq_init(&hstat, pendingOps);
		while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			entry->cycle_ctr = sync_cycle_ctr;
			entry->sync_completed = false;
		}
	}

	/* Advance counter so that new hashtable entries are distinguishable */
	sync_cycle_ctr++;

	/* Set flag to detect failure if we don't reach the end of the loop */
	sync_in_progress = true;

	/*
	 * Bound concurrent fsyncs by both the AIO handle and transient descriptor
	 * budgets.
	 */
	dlist_init(&sync_state.inflight);
	dlist_init(&sync_state.retry);
	sync_state.inflight_count = 0;
	sync_state.max_inflight = GetFsyncConcurrencyLimit();
	sync_state.processed = 0;
	INSTR_TIME_SET_ZERO(sync_state.longest);
	INSTR_TIME_SET_ZERO(sync_state.total_elapsed);

	Assert(dlist_is_empty(&activeSyncEntries));
	MemoryContextReset(inflightSyncCxt);

	/* Now scan the hashtable for fsync requests to process */
	sync_state.absorb_counter = FSYNCS_PER_ABSORB;
	hash_seq_init(&hstat, pendingOps);
	while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * If the entry is new then don't process it this time; it is new.
		 * Note "continue" bypasses the hash-remove call at the bottom of the
		 * loop.
		 */
		if (entry->cycle_ctr == sync_cycle_ctr)
			continue;

		/* Else assert we haven't missed it */
		Assert((CycleCtr) (entry->cycle_ctr + 1) == sync_cycle_ctr);

		/*
		 * If in checkpointer, we want to absorb pending requests every so
		 * often to prevent overflow of the fsync request queue.  It is
		 * unspecified whether newly-added entries will be visited by
		 * hash_seq_search, but we don't care since we don't need to process
		 * them anyway.
		 */
		if (enableFsync && --sync_state.absorb_counter <= 0)
		{
			AbsorbSyncRequests();
			sync_state.absorb_counter = FSYNCS_PER_ABSORB;
		}

		if (!enableFsync || entry->canceled)
		{
			/* We are done with this entry, remove it */
			if (hash_search(pendingOps, &entry->tag, HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "pendingOps corrupted");
		}
		else
		{
			InflightSyncEntry *inflight_entry;

			Assert(sync_state.inflight_count <= sync_state.max_inflight);
			if (sync_state.inflight_count == sync_state.max_inflight)
				sync_drain_one(&sync_state);

			/*
			 * Mark the entry as already dealt with in this cycle.  It must
			 * remain in the hash table until its fsync completes and the scan
			 * ends.  If a new request arrives meanwhile, this cycle counter
			 * leaves the entry to be processed by the next checkpoint.
			 */
			entry->cycle_ctr = sync_cycle_ctr;

			inflight_entry = MemoryContextAllocZero(inflightSyncCxt,
													sizeof(InflightSyncEntry));
			inflight_entry->tag = entry->tag;
			inflight_entry->hash_entry = entry;
			dlist_push_tail(&activeSyncEntries,
							&inflight_entry->cleanup_node);

			sync_start_one(&sync_state, inflight_entry);
		}
	}

	sync_drain_all(&sync_state);
	sync_process_completed();

	/*
	 * A second failure raises an error, so normally one retry pass is enough.
	 * Keep an explicit bound in case that changes.
	 */
	for (int failures = 0; failures < 5; failures++)
	{
		if (dlist_is_empty(&sync_state.retry))
			break;

		sync_process_retries(&sync_state);
	}

	if (!dlist_is_empty(&sync_state.inflight) ||
		!dlist_is_empty(&sync_state.retry))
		elog(PANIC, "in-flight sync requests remain after ProcessSyncRequests");

	/* Return sync performance metrics for report at checkpoint end */
	CheckpointStats.ckpt_sync_rels = sync_state.processed;
	CheckpointStats.ckpt_longest_sync = INSTR_TIME_GET_MICROSEC(sync_state.longest);
	CheckpointStats.ckpt_agg_sync_time = INSTR_TIME_GET_MICROSEC(sync_state.total_elapsed);

	/* Flag successful completion of ProcessSyncRequests */
	sync_in_progress = false;
}

/*
 *	ProcessSyncRequests() -- Process queued fsync requests.
 */
void
ProcessSyncRequests(void)
{
	PG_ENSURE_ERROR_CLEANUP(sync_cleanup_inflight, (Datum) 0);
	{
		ProcessSyncRequestsInternal();
	}
	PG_END_ENSURE_ERROR_CLEANUP(sync_cleanup_inflight, (Datum) 0);

	Assert(dlist_is_empty(&activeSyncEntries));
}

/*
 * RememberSyncRequest() -- callback from checkpointer side of sync request
 *
 * We stuff fsync requests into the local hash table for execution
 * during the checkpointer's next checkpoint.  UNLINK requests go into a
 * separate linked list, however, because they get processed separately.
 *
 * See sync.h for more information on the types of sync requests supported.
 */
void
RememberSyncRequest(const FileTag *ftag, SyncRequestType type)
{
	Assert(pendingOps);

	if (type == SYNC_FORGET_REQUEST)
	{
		PendingFsyncEntry *entry;

		/* Cancel previously entered request */
		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  ftag,
												  HASH_FIND,
												  NULL);
		if (entry != NULL)
			entry->canceled = true;
	}
	else if (type == SYNC_FILTER_REQUEST)
	{
		HASH_SEQ_STATUS hstat;
		PendingFsyncEntry *pfe;
		ListCell   *cell;

		/* Cancel matching fsync requests */
		hash_seq_init(&hstat, pendingOps);
		while ((pfe = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			if (pfe->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &pfe->tag))
				pfe->canceled = true;
		}

		/* Cancel matching unlink requests */
		foreach(cell, pendingUnlinks)
		{
			PendingUnlinkEntry *pue = (PendingUnlinkEntry *) lfirst(cell);

			if (pue->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &pue->tag))
				pue->canceled = true;
		}
	}
	else if (type == SYNC_UNLINK_REQUEST)
	{
		/* Unlink request: put it in the linked list */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingUnlinkEntry *entry;

		entry = palloc_object(PendingUnlinkEntry);
		entry->tag = *ftag;
		entry->cycle_ctr = checkpoint_cycle_ctr;
		entry->canceled = false;

		pendingUnlinks = lappend(pendingUnlinks, entry);

		MemoryContextSwitchTo(oldcxt);
	}
	else
	{
		/* Normal case: enter a request to fsync this segment */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingFsyncEntry *entry;
		bool		found;

		Assert(type == SYNC_REQUEST);

		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  ftag,
												  HASH_ENTER,
												  &found);

		/*
		 * If an entry already existed, an fsync for it may be in flight right
		 * now, in which case it cannot be assumed to cover this request; see
		 * sync_drain_one().
		 */
		entry->re_requested = found;

		/* if new entry, or was previously canceled, initialize it */
		if (!found || entry->canceled)
		{
			entry->cycle_ctr = sync_cycle_ctr;
			entry->canceled = false;
			entry->sync_completed = false;
		}

		/*
		 * NB: it's intentional that we don't change cycle_ctr if the entry
		 * already exists.  The cycle_ctr must represent the oldest fsync
		 * request that could be in the entry.
		 */

		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * Register the sync request locally, or forward it to the checkpointer.
 *
 * If retryOnError is true, we'll keep trying if there is no space in the
 * queue.  Return true if we succeeded, or false if there wasn't space.
 */
bool
RegisterSyncRequest(const FileTag *ftag, SyncRequestType type,
					bool retryOnError)
{
	bool		ret;

	if (pendingOps != NULL)
	{
		/* standalone backend or startup process: fsync state is local */
		RememberSyncRequest(ftag, type);
		return true;
	}

	for (;;)
	{
		/*
		 * Notify the checkpointer about it.  If we fail to queue a message in
		 * retryOnError mode, we have to sleep and try again ... ugly, but
		 * hopefully won't happen often.
		 *
		 * XXX should we CHECK_FOR_INTERRUPTS in this loop?  Escaping with an
		 * error in the case of SYNC_UNLINK_REQUEST would leave the
		 * no-longer-used file still present on disk, which would be bad, so
		 * I'm inclined to assume that the checkpointer will always empty the
		 * queue soon.
		 */
		ret = ForwardSyncRequest(ftag, type);

		/*
		 * If we are successful in queueing the request, or we failed and were
		 * instructed not to retry on error, break.
		 */
		if (ret || (!ret && !retryOnError))
			break;

		WaitLatch(NULL, WL_EXIT_ON_PM_DEATH | WL_TIMEOUT, 10,
				  WAIT_EVENT_REGISTER_SYNC_REQUEST);
	}

	return ret;
}
