/*-------------------------------------------------------------------------
 *
 * sync.h
 *	  File synchronization management code.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sync.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYNC_H
#define SYNC_H

#include "lib/ilist.h"
#include "portability/instr_time.h"
#include "storage/aio_types.h"
#include "storage/relfilelocator.h"

/*
 * Type of sync request.  These are used to manage the set of pending
 * requests to call a sync handler's sync or unlink functions at the next
 * checkpoint.
 */
typedef enum SyncRequestType
{
	SYNC_REQUEST,				/* schedule a call of sync function */
	SYNC_UNLINK_REQUEST,		/* schedule a call of unlink function */
	SYNC_FORGET_REQUEST,		/* forget all calls for a tag */
	SYNC_FILTER_REQUEST,		/* forget all calls satisfying match fn */
} SyncRequestType;

/*
 * Which set of functions to use to handle a given request.  The values of
 * the enumerators must match the indexes of the function table in sync.c.
 */
typedef enum SyncRequestHandler
{
	SYNC_HANDLER_MD = 0,
	SYNC_HANDLER_CLOG,
	SYNC_HANDLER_COMMIT_TS,
	SYNC_HANDLER_MULTIXACT_OFFSET,
	SYNC_HANDLER_MULTIXACT_MEMBER,
	SYNC_HANDLER_NONE,
} SyncRequestHandler;

/*
 * A tag identifying a file.  Its representation is shared with AIO target
 * data, so changes are automatically visible to processes that reopen files
 * on behalf of sync.c.
 */
typedef PgAioSyncFileTag FileTag;

struct PendingFsyncEntry;
struct PgAioHandle;

/*
 * How the file opened by a sync handler must be closed once its asynchronous
 * fsync has completed.
 */
typedef enum SyncFileCloseMethod
{
	SYNC_CLOSE_NONE = 0,		/* nothing to close */
	SYNC_CLOSE_TRANSIENT,		/* CloseTransientFile(close_file) */
	SYNC_CLOSE_VFD,				/* FileClose((File) close_file) */
} SyncFileCloseMethod;

/*
 * State for a single in-flight asynchronous fsync request.  A sync handler
 * opens the file to be synced, fills in the fields it is responsible for, and
 * starts an asynchronous fsync on the AIO handle it is given.
 */
typedef struct InflightSyncEntry
{
	FileTag		tag;			/* identifies handler and file */

	char		path[MAXPGPATH];

	/*
	 * Set by the handler: whether it started an asynchronous fsync on the
	 * passed-in AIO handle.  If the file could not be opened, the handler
	 * sets started = false and open_errno to the errno of the failed open.
	 */
	bool		started;
	int			open_errno;

	/* set by the handler: how to close the opened file after completion */
	SyncFileCloseMethod close_method;
	int			close_file;		/* fd, or File, depending on close_method */

	struct PendingFsyncEntry *hash_entry;

	int			retry_count;

	instr_time	start_time;

	PgAioReturn ioret;
	PgAioWaitRef iow;

	/* membership in the inflight / retry lists */
	dlist_node	node;

	/* membership in the error-cleanup list */
	dlist_node	cleanup_node;
} InflightSyncEntry;

extern void InitSync(void);
extern void SyncPreCheckpoint(void);
extern void SyncPostCheckpoint(void);
extern void ProcessSyncRequests(void);
extern void RememberSyncRequest(const FileTag *ftag, SyncRequestType type);
extern bool RegisterSyncRequest(const FileTag *ftag, SyncRequestType type,
								bool retryOnError);

/* AIO support */
extern PGDLLIMPORT const PgAioTargetInfo aio_sync_filetag_target_info;
extern void pgaio_io_set_target_sync_filetag(PgAioHandle *ioh,
											 const FileTag *ftag);

#endif							/* SYNC_H */
