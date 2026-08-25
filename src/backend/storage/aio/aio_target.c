/*-------------------------------------------------------------------------
 *
 * aio_target.c
 *	  AIO - Functionality related to executing IO for different targets
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_target.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/smgr.h"
#include "storage/sync.h"

static char *pgaio_sync_describe_identity(const PgAioTargetData *sd);

/*
 * Target info for generic file syncs (PGAIO_TID_SYNC). The file being synced
 * is identified by a path that is not stored in shared memory, therefore no
 * reopen callback is provided.
 */
static const PgAioTargetInfo aio_sync_target_info = {
	.name = "sync",
	.describe_identity = pgaio_sync_describe_identity,
};

/*
 * Registry for entities that can be the target of AIO.
 */
static const PgAioTargetInfo *pgaio_target_info[] = {
	[PGAIO_TID_INVALID] = &(PgAioTargetInfo) {
		.name = "invalid",
	},
	[PGAIO_TID_SMGR] = &aio_smgr_target_info,
	[PGAIO_TID_SYNC] = &aio_sync_target_info,
	[PGAIO_TID_SYNC_FILETAG] = &aio_sync_filetag_target_info,
};

/*
 * The IO whose descriptor this process reopened and whose target requires the
 * descriptor to be released after execution.  Only set between
 * pgaio_io_reopen() and pgaio_io_close_reopened().
 */
static PgAioHandle *pgaio_reopened_ioh = NULL;


/*
 * describe_identity callback for PGAIO_TID_SYNC. As we do not store the path
 * of the file being synced in shared memory, only a generic description can
 * be provided.
 */
static char *
pgaio_sync_describe_identity(const PgAioTargetData *sd)
{
	return pstrdup("generic file sync");
}



/* --------------------------------------------------------------------------------
 * Public target related functions operating on IO Handles
 * --------------------------------------------------------------------------------
 */

bool
pgaio_io_has_target(PgAioHandle *ioh)
{
	return ioh->target != PGAIO_TID_INVALID;
}

/*
 * Return the name for the target associated with the IO. Mostly useful for
 * debugging/logging.
 */
const char *
pgaio_io_get_target_name(PgAioHandle *ioh)
{
	/* explicitly allow INVALID here, function used by debug messages */
	Assert(ioh->target >= PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);

	return pgaio_target_info[ioh->target]->name;
}

/*
 * Assign a target to the IO.
 *
 * This has to be called exactly once before pgaio_io_start_*() is called.
 */
void
pgaio_io_set_target(PgAioHandle *ioh, PgAioTargetID targetid)
{
	Assert(ioh->state == PGAIO_HS_HANDED_OUT);
	Assert(ioh->target == PGAIO_TID_INVALID);

	ioh->target = targetid;
}

PgAioTargetData *
pgaio_io_get_target_data(PgAioHandle *ioh)
{
	return &ioh->target_data;
}

/*
 * Return a stringified description of the IO's target.
 *
 * The string is localized and allocated in the current memory context.
 */
char *
pgaio_io_get_target_description(PgAioHandle *ioh)
{
	/* disallow INVALID, there wouldn't be a description */
	Assert(ioh->target > PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);

	return pgaio_target_info[ioh->target]->describe_identity(&ioh->target_data);
}



/* --------------------------------------------------------------------------------
 * Internal target related functions operating on IO Handles
 * --------------------------------------------------------------------------------
 */

/*
 * Internal: Check if pgaio_io_reopen() is available for the IO.
 */
bool
pgaio_io_can_reopen(PgAioHandle *ioh)
{
	Assert(ioh->target > PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);

	return pgaio_target_info[ioh->target]->reopen != NULL;
}

/*
 * Internal: Before executing an IO outside of the context of the process the
 * IO has been staged in, the file descriptor has to be reopened - any FD
 * referenced in the IO itself, won't be valid in the separate process.
 */
void
pgaio_io_reopen(PgAioHandle *ioh)
{
	Assert(ioh->target > PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);
	Assert(ioh->op > PGAIO_OP_INVALID && ioh->op < PGAIO_OP_COUNT);
	Assert(pgaio_reopened_ioh == NULL);

	pgaio_target_info[ioh->target]->reopen(ioh);

	/*
	 * Remember that this process, rather than the one that staged the IO,
	 * owns the file descriptor now, so that pgaio_io_close_reopened() can
	 * release it once the IO has been executed.
	 */
	if (pgaio_target_info[ioh->target]->close != NULL)
		pgaio_reopened_ioh = ioh;
}

/*
 * Internal: Counterpart to pgaio_io_reopen(), releasing the file descriptor it
 * acquired.  Does nothing unless this process reopened this very IO and its
 * target needs the descriptor to be released.
 *
 * This has to be called before the IO's completion is processed, as that can
 * make the handle be reused for an unrelated IO.
 */
void
pgaio_io_close_reopened(PgAioHandle *ioh)
{
	if (pgaio_reopened_ioh != ioh)
		return;

	pgaio_reopened_ioh = NULL;
	pgaio_target_info[ioh->target]->close(ioh);
}
