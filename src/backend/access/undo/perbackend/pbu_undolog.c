/*-------------------------------------------------------------------------
 *
 * undolog.c
 *	  management of undo logs
 *
 * PostgreSQL undo log manager.  This module is responsible for lifecycle
 * management of undo logs and backing files, associating undo logs with
 * backends, allocating and managing space within undo logs.
 *
 * For the code that reads and writes blocks of data, see undofile.c.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undolog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/perbackend/pbu_compat.h"

#include "access/transam.h"
#include "access/perbackend/pbu_undolog.h"
#include "access/perbackend/pbu_undolog_xlog.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/standby.h"
#include "access/perbackend/pbu_undofile.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

#include <sys/stat.h>
#include <unistd.h>

/*
 * Number of bits of an undo log number used to identify a bank of
 * UndoLogControl objects.  This allows us to break up our array of
 * UndoLogControl objects into many smaller arrays, called banks, and find our
 * way to an UndoLogControl object in O(1) complexity in two steps.
 */
#define UndoLogBankBits 14
#define UndoLogBanks (1 << UndoLogBankBits)

/* Number of UndoLogControl objects in one bank (the remaining low bits). */
#define UndoLogsPerBank (1 << (UndoLogNumberBits - UndoLogBankBits))

/* Extract the undo bank number from an undo log number (upper bits). */
#define UndoLogNoGetBankNo(logno)				\
	((logno) >> (UndoLogNumberBits - UndoLogBankBits))

/* Extract the slot within a bank from an undo log number (lower bits). */
#define UndoLogNoGetSlotNo(logno)				\
	((logno) & (UndoLogsPerBank - 1))

/*
 * During recovery we maintain a mapping of transaction ID to undo logs
 * numbers.  We do this with another two-level array, so that we use memory
 * only for chunks of the array that overlap with the range of active xids.
 */
#define UndoLogXidLowBits 16

/*
 * Number of high bits.
 */
#define UndoLogXidHighBits \
	(sizeof(TransactionId) * CHAR_BIT - UndoLogXidLowBits)

/* Extract the upper bits of an xid, for undo log mapping purposes. */
#define UndoLogGetXidHigh(xid) ((xid) >> UndoLogXidLowBits)

/* Extract the lower bits of an xid, for undo log mapping purposes. */
#define UndoLogGetXidLow(xid) ((xid) & ((1 << UndoLogXidLowBits) - 1))

/*
 * Main control structure for undo log management in shared memory.
 */
typedef struct UndoLogSharedData
{
	UndoLogNumber free_lists[UndoPersistenceLevels];
	int			low_bankno;		/* the lowest bank */
	int			high_bankno;	/* one past the highest bank */
	UndoLogNumber low_logno;	/* the lowest logno */
	UndoLogNumber high_logno;	/* one past the highest logno */

	/*
	 * Array of DSM handles pointing to the arrays of UndoLogControl objects.
	 * We don't expect there to be many banks active at a time -- usually 1 or
	 * 2, but we need random access by log number so we arrange them into
	 * 'banks'.
	 */
	dsm_handle	banks[UndoLogBanks];
} UndoLogSharedData;

/*
 * Per-backend state for the undo log module.
 * Backend-local pointers to undo subsystem state in shared memory.
 */
static struct
{
	UndoLogSharedData *shared;

	/*
	 * The control object for the undo logs that this backend is currently
	 * attached to at each persistence level.
	 */
	UndoLogControl *logs[UndoPersistenceLevels];

	/* The DSM segments used to hold banks of control objects. */
	dsm_segment *bank_segments[UndoLogBanks];

	/*
	 * The address where each bank of control objects is mapped into memory in
	 * this backend.  We map banks into memory on demand, and they stay mapped
	 * in until every backend that mapped them exits.
	 */
	UndoLogControl *banks[UndoLogBanks];

	/*
	 * The lowest log number that might currently be mapped into this backend.
	 */
	int			low_logno;

	/*
	 * If the undo_tablespaces GUC changes we'll remember to examine it and
	 * attach to a new undo log using this flag.
	 */
	bool		need_to_choose_tablespace;

	/*
	 * During recovery, the startup process maintains a mapping of xid to undo
	 * log number, instead of using 'log' above.  This is not used in regular
	 * backends and can be in backend-private memory so long as recovery is
	 * single-process.  This map references UNDO_PERMANENT logs only, since
	 * temporary and unlogged relations don't have WAL to replay.
	 */
	UndoLogNumber **xid_map;

	/*
	 * The slot for the oldest xids still running.  We advance this during
	 * checkpoints to free up chunks of the map.
	 */
	uint16		xid_map_oldest_chunk;

	/* Current dbid.  Used during recovery. */
	Oid			dbid;

	/*
	 * Transaction's start header undo record pointer in the previous undo log
	 * when transaction spills across multiple undo log.  This is used for
	 * identifying the log switch during recovery and updating the transaction
	 * header in the previous log.
	 */
	UndoRecPtr	prevlogurp;
} MyUndoLogState;

/* GUC variables */
static char *undo_tablespaces = NULL;

static UndoLogControl *get_undo_log_by_number(UndoLogNumber logno);
static void ensure_undo_log_number(UndoLogNumber logno);
static void attach_undo_log(UndoPersistence level, Oid tablespace);
static void detach_current_undo_log(UndoPersistence level, bool exhausted);
static void extend_undo_log(UndoLogNumber logno, UndoLogOffset new_end);
static void undo_log_before_exit(int code, Datum value);
static void forget_undo_buffers(int logno, UndoLogOffset old_discard,
								UndoLogOffset new_discard,
								bool drop_tail);
static bool choose_undo_tablespace(bool force_detach, Oid *oid);
static void undolog_xid_map_gc(void);
static UndoLogControl *undolog_redo_get_log(UndoLogNumber logno);
static void undolog_bank_gc(void);

/*
 * pbu: singleton lock guarding the undo-log freelist / bank arrays.  zheap
 * used a built-in named LWLock (UndoLogLock); to avoid touching the shared
 * lwlocklist.h we allocate our own here and register a tranche.
 */
LWLock	   *UndoLogLock_pbu = NULL;
LWLock	   *RollbackRequestLock_pbu = NULL;
LWLock	   *UndoWorkerLock_pbu = NULL;
pg_atomic_uint64 *UndoOldestXidHavingUndo_pbu = NULL;

PG_FUNCTION_INFO_V1(pbu_pg_stat_get_undo_logs);

/*
 * Return the amount of traditional smhem required for undo log management.
 * Extra shared memory will be managed using DSM segments.
 */
Size
PbuUndoLogShmemSize(void)
{
	/*
	 * Layout: UndoLogSharedData, then the 8-byte atomic
	 * oldestXidWithEpochHavingUndo, then the three singleton LWLocks.  The
	 * atomic must be 8-byte aligned (pg_atomic_init_u64 asserts this), so it
	 * is placed at an explicitly 8-byte-aligned offset.  MAXALIGN is only 4
	 * bytes on some platforms.  The allocation base is cache-line aligned.
	 */
	return add_size(TYPEALIGN(8, sizeof(UndoLogSharedData)),
					add_size(MAXALIGN(sizeof(pg_atomic_uint64)),
							 mul_size(3, sizeof(LWLock))));
}

/*
 * Initialize the undo log subsystem.  Called in each backend.
 */
void
PbuUndoLogShmemRequest(void)
{
	/*
	 * Reserve the shared struct in the request phase so the framework sizes
	 * the segment for it and populates MyUndoLogState.shared before
	 * PbuUndoLogShmemInit() runs (mirroring the sLog subsystem's pattern).
	 * The layout after the struct is: the 8-byte oldest-xid atomic (placed
	 * first, at an 8-byte-aligned offset, because pg_atomic_init_u64 requires
	 * 8-byte alignment) followed by the three singleton LWLocks; see
	 * PbuUndoLogShmemSize().
	 */
	ShmemRequestStruct(.name = "UndoLogShared",
					   .size = PbuUndoLogShmemSize(),
					   .alignment = 8,
					   .ptr = (void **) &MyUndoLogState.shared);
}

void
PbuUndoLogShmemInit(void)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;

	/*
	 * The framework has already allocated the "UndoLogShared" region and set
	 * MyUndoLogState.shared (see PbuUndoLogShmemRequest()).  Carve the
	 * singleton locks + oldest-xid atomic out of the region after the shared
	 * struct.
	 */
	UndoOldestXidHavingUndo_pbu = (pg_atomic_uint64 *)
		(((char *) shared) + TYPEALIGN(8, sizeof(UndoLogSharedData)));
	Assert(TYPEALIGN(8, (uintptr_t) UndoOldestXidHavingUndo_pbu) ==
		   (uintptr_t) UndoOldestXidHavingUndo_pbu);
	UndoLogLock_pbu = (LWLock *)
		(((char *) UndoOldestXidHavingUndo_pbu) +
		 MAXALIGN(sizeof(pg_atomic_uint64)));
	RollbackRequestLock_pbu = UndoLogLock_pbu + 1;
	UndoWorkerLock_pbu = UndoLogLock_pbu + 2;

	if (!IsUnderPostmaster)
	{
		int			i;

		LWLockInitialize(UndoLogLock_pbu, LWTRANCHE_UNDO_LOG_PBU);
		LWLockInitialize(RollbackRequestLock_pbu, LWTRANCHE_UNDO_LOG_PBU);
		LWLockInitialize(UndoWorkerLock_pbu, LWTRANCHE_UNDO_LOG_PBU);
		pg_atomic_init_u64(UndoOldestXidHavingUndo_pbu, 0);

		/*
		 * We start with no undo logs.  StartUpUndoLogs() will recreate undo
		 * logs that were known at last checkpoint.
		 */
		memset(shared, 0, sizeof(*shared));
		for (i = 0; i < UndoPersistenceLevels; ++i)
			shared->free_lists[i] = InvalidUndoLogNumber;
		shared->low_bankno = 0;
		shared->high_bankno = 0;

		/*
		 * Undo log numbers double as the RelFileNumber of the undo log's
		 * pseudo-relation in the undo SMGR (UndoRecPtrGetRelNode == logno,
		 * and the on-disk segment path is derived from that same relNumber).
		 * A RelFileNumber of 0 is InvalidRelFileNumber and trips
		 * RelFileNumberIsValid() in smgropen(), so undo log 0 can never be
		 * used.  Start numbering at 1.  (zheap addressed undo logs in a
		 * private space where 0 was fine; on master they are ordinary SMGR
		 * relations, so the first valid number is 1.)  FILEOPS is the first
		 * producer to actually allocate an undo log, which is why this only
		 * bites now.
		 */
		shared->low_logno = 1;
		shared->high_logno = 1;
	}
}

void
UndoLogInit(void)
{
	before_shmem_exit(undo_log_before_exit, 0);
}

/*
 * Figure out which directory holds an undo log based on tablespace.
 */
static void
UndoLogDirectory(Oid tablespace, char *dir)
{
	if (tablespace == DEFAULTTABLESPACE_OID ||
		tablespace == InvalidOid)
		snprintf(dir, MAXPGPATH, "base/undo");
	else
		snprintf(dir, MAXPGPATH, "pg_tblspc/%u/%s/undo",
				 tablespace, TABLESPACE_VERSION_DIRECTORY);

	/* Note: Should we use UndoLogDatabaseOid (9) instead of "undo"? */

	/*
	 * Note: Should we add an extra directory between log number and segment
	 * files?  If all undo logs are in the same directory then
	 * fsync(directory) may create contention in the OS between unrelated
	 * backends that as they rotate segment files.
	 */
}

/*
 * Compute the pathname to use for an undo log segment file.
 */
void
UndoLogSegmentPath(UndoLogNumber logno, int segno, Oid tablespace, char *path)
{
	char		dir[MAXPGPATH];

	/* Figure out which directory holds the segment, based on tablespace. */
	UndoLogDirectory(tablespace, dir);

	/*
	 * Build the path from log number and offset.  The pathname is the
	 * UndoRecPtr of the first byte in the segment in hexadecimal, with a
	 * period inserted between the components.
	 */
	snprintf(path, MAXPGPATH, "%s/%06X.%010zX", dir, logno,
			 segno * UndoLogSegmentSize);
}

/*
 * Iterate through the set of currently active logs.
 *
 * This scans every log in [low_logno, high_logno); the cost is linear in
 * the range of log numbers, including inactive logs.
 */
UndoLogControl *
UndoLogNext(UndoLogControl *log)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;

	if (log == NULL)
	{
		UndoLogNumber low_logno;

		LWLockAcquire(UndoLogLock, LW_SHARED);
		low_logno = shared->low_logno;
		LWLockRelease(UndoLogLock);

		return get_undo_log_by_number(low_logno);
	}
	else
	{
		UndoLogNumber high_logno;

		LWLockAcquire(UndoLogLock, LW_SHARED);
		high_logno = shared->high_logno;
		LWLockRelease(UndoLogLock);

		if (log->logno + 1 == high_logno)
			return NULL;

		return get_undo_log_by_number(log->logno + 1);
	}
}

/*
 * Check if an undo log position has been discarded.  'point' must be an undo
 * log pointer that was allocated at some point in the past, otherwise the
 * result is undefined.
 */
bool
UndoLogIsDiscarded(UndoRecPtr point)
{
	UndoLogControl *log = get_undo_log_by_number(UndoRecPtrGetLogNo(point));
	bool		result;

	/*
	 * If we don't recognize the log number, it's either entirely discarded or
	 * it's never been allocated (ie from the future) and our result is
	 * undefined.
	 */
	if (log == NULL)
		return true;

	/*
	 * Note: For a super cheap locked operation, it's better to use
	 * LW_EXLUSIVE even though we don't need exclusivity, right?
	 */
	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	result = UndoRecPtrGetOffset(point) < log->meta.discard;
	LWLockRelease(&log->mutex);

	return result;
}

/*
 * Store latest transaction's start undo record point in undo meta data.  It
 * will fetched by the backend when it's reusing the undo log and preparing
 * its first undo.
 */
void
UndoLogSetLastXactStartPoint(UndoRecPtr point)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(point);
	UndoLogControl *log = get_undo_log_by_number(logno);

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->meta.last_xact_start = UndoRecPtrGetOffset(point);
	LWLockRelease(&log->mutex);
}

/*
 * Fetch the previous transaction's start undo record point.  Return Invalid
 * undo pointer if backend is not attached to any log.
 */
UndoRecPtr
UndoLogGetLastXactStartPoint(UndoLogNumber logno)
{
	UndoLogControl *log = get_undo_log_by_number(logno);
	uint64		last_xact_start = 0;

	if (unlikely(log == NULL))
		return InvalidUndoRecPtr;

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	last_xact_start = log->meta.last_xact_start;
	LWLockRelease(&log->mutex);

	if (last_xact_start == 0)
		return InvalidUndoRecPtr;

	return MakeUndoRecPtr(logno, last_xact_start);
}

/*
 * Is this record is the first record for any transaction.
 */
bool
IsTransactionFirstRec(TransactionId xid)
{
	uint16		high_bits = UndoLogGetXidHigh(xid);
	uint16		low_bits = UndoLogGetXidLow(xid);
	UndoLogNumber logno;
	UndoLogControl *log;

	Assert(InRecovery);

	if (MyUndoLogState.xid_map == NULL)
		elog(ERROR, "xid to undo log number map not initialized");
	if (MyUndoLogState.xid_map[high_bits] == NULL)
		elog(ERROR, "cannot find undo log number for xid %u", xid);

	logno = MyUndoLogState.xid_map[high_bits][low_bits];

	log = get_undo_log_by_number(logno);
	if (log == NULL)
		elog(ERROR, "cannot find undo log number %d for xid %u", logno, xid);

	return log->meta.is_first_rec;
}

/*
 * Detach from the undo log we are currently attached to, returning it to the
 * free list if it still has space.
 */
static void
detach_current_undo_log(UndoPersistence persistence, bool exhausted)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	UndoLogControl *log = MyUndoLogState.logs[persistence];

	Assert(log != NULL);

	MyUndoLogState.logs[persistence] = NULL;

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->procno = INVALID_PROC_NUMBER;
	log->pid = InvalidPid;
	log->xid = InvalidTransactionId;
	if (exhausted)
		log->meta.status = UNDO_LOG_STATUS_EXHAUSTED;
	LWLockRelease(&log->mutex);

	/* Push back onto the appropriate freelist. */
	if (!exhausted)
	{
		LWLockAcquire(UndoLogLock, LW_EXCLUSIVE);
		log->next_free = shared->free_lists[persistence];
		shared->free_lists[persistence] = log->logno;
		LWLockRelease(UndoLogLock);
	}
}

static void
undo_log_before_exit(int code, Datum arg)
{
	int			i;

	for (i = 0; i < UndoPersistenceLevels; ++i)
	{
		if (MyUndoLogState.logs[i] != NULL)
			detach_current_undo_log(i, false);
	}
}

/*
 * Create a fully allocated empty segment file on disk for the byte starting
 * at 'end'.
 */
static void
allocate_empty_undo_segment(UndoLogNumber logno, Oid tablespace,
							UndoLogOffset end)
{
	struct stat stat_buffer;
	off_t		size;
	char		path[MAXPGPATH];
	void	   *zeroes;
	size_t		nzeroes = 8192;
	int			fd;

	UndoLogSegmentPath(logno, end / UndoLogSegmentSize, tablespace, path);

	/*
	 * Create and fully allocate a new file.  If we crashed and recovered then
	 * the file might already exist, so use flags that tolerate that. It's
	 * also possible that it exists but is too short, in which case we'll
	 * write the rest.  We don't really care what's in the file, we just want
	 * to make sure that the filesystem has allocated physical blocks for it,
	 * so that non-COW filesystems will report ENOSPC now rather than later
	 * when the space is needed and we'll avoid creating files with holes.
	 */
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0 && tablespace != 0)
	{
		char		undo_path[MAXPGPATH];

		/* Try creating the undo directory for this tablespace. */
		UndoLogDirectory(tablespace, undo_path);
		if (mkdir(undo_path, S_IRWXU) != 0 && errno != EEXIST)
		{
			char	   *parentdir;

			if (errno != ENOENT || !InRecovery)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create directory \"%s\": %m",
								undo_path)));

			/*
			 * In recovery, it's possible that the tablespace directory
			 * doesn't exist because a later WAL record removed the whole
			 * tablespace.  In that case we create a regular directory to
			 * stand in for it.  This is similar to the logic in
			 * TablespaceCreateDbspace().
			 */

			/* create two parents up if not exist */
			parentdir = pstrdup(undo_path);
			get_parent_directory(parentdir);
			get_parent_directory(parentdir);
			/* Can't create parent and it doesn't already exist? */
			if (mkdir(parentdir, S_IRWXU) < 0 && errno != EEXIST)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create directory \"%s\": %m",
								parentdir)));
			pfree(parentdir);

			/* create one parent up if not exist */
			parentdir = pstrdup(undo_path);
			get_parent_directory(parentdir);
			/* Can't create parent and it doesn't already exist? */
			if (mkdir(parentdir, S_IRWXU) < 0 && errno != EEXIST)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create directory \"%s\": %m",
								parentdir)));
			pfree(parentdir);

			if (mkdir(undo_path, S_IRWXU) != 0 && errno != EEXIST)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create directory \"%s\": %m",
								undo_path)));
		}

		fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	}
	if (fd < 0)
		elog(ERROR, "could not create new file \"%s\": %m", path);
	if (fstat(fd, &stat_buffer) < 0)
		elog(ERROR, "could not stat \"%s\": %m", path);
	size = stat_buffer.st_size;

	/* A buffer full of zeroes we'll use to fill up new segment files. */
	zeroes = palloc0(nzeroes);

	while (size < UndoLogSegmentSize)
	{
		ssize_t		written;

		written = write(fd, zeroes, Min(nzeroes, UndoLogSegmentSize - size));
		if (written < 0)
			elog(ERROR, "cannot initialize undo log segment file \"%s\": %m",
				 path);
		size += written;
	}

	/* Flush the contents of the file to disk. */
	if (pg_fsync(fd) != 0)
		elog(ERROR, "cannot fsync file \"%s\": %m", path);
	CloseTransientFile(fd);

	pfree(zeroes);

}

/*
 * Create a new undo segment, when it is unexpectedly not present.
 */
void
UndoLogNewSegment(UndoLogNumber logno, Oid tablespace, int segno)
{
	Assert(InRecovery);
	allocate_empty_undo_segment(logno, tablespace, segno * UndoLogSegmentSize);
}

/*
 * Create and zero-fill a new segment for the undo log we are currently
 * attached to.
 */
static void
extend_undo_log(UndoLogNumber logno, UndoLogOffset new_end)
{
	UndoLogControl *log;
	char		dir[MAXPGPATH];
	size_t		end;

	log = get_undo_log_by_number(logno);

	Assert(log != NULL);
	Assert(log->meta.end % UndoLogSegmentSize == 0);
	Assert(new_end % UndoLogSegmentSize == 0);
	Assert(MyUndoLogState.logs[log->meta.persistence] == log || InRecovery);

	/*
	 * Create all the segments needed to increase 'end' to the requested size.
	 * This is quite expensive, so we will try to avoid it completely by
	 * renaming files into place in PbuUndoLogDiscard instead.
	 */
	end = log->meta.end;
	while (end < new_end)
	{
		allocate_empty_undo_segment(logno, log->meta.tablespace, end);
		end += UndoLogSegmentSize;
	}

	/*
	 * Flush the parent dir so that the directory metadata survives a crash
	 * after this point.
	 */
	UndoLogDirectory(log->meta.tablespace, dir);
	fsync_fname(dir, true);

	/*
	 * If we're not in recovery, we need to WAL-log the creation of the new
	 * file(s).  We do that after the above filesystem modifications, in
	 * violation of the data-before-WAL rule as exempted by
	 * src/backend/access/transam/README.  This means that it's possible for
	 * us to crash having made some or all of the filesystem changes but
	 * before WAL logging, but in that case we'll eventually try to create the
	 * same segment(s) again which is tolerated.
	 *
	 * Unlogged and temporary undo logs are not WAL-logged: their contents
	 * never reach the WAL, crash recovery empties them
	 * (ResetNonPermanentUndoLogs), and a standby has no use for their
	 * segments.  The same goes for their discards; see PbuUndoLogDiscard().
	 */
	if (!InRecovery && log->meta.persistence == UNDO_PERMANENT)
	{
		xl_undolog_extend xlrec;
		XLogRecPtr	ptr;

		xlrec.logno = logno;
		xlrec.end = end;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		ptr = XLogInsert(RM_UNDOLOG_ID, XLOG_UNDOLOG_EXTEND);
		XLogFlush(ptr);
	}

	/*
	 * We didn't need to acquire the mutex to read 'end' above because only we
	 * write to it.  But we need the mutex to update it, because the
	 * checkpointer might read it concurrently.
	 *
	 * Note: It's possible for meta.end to be higher already during recovery,
	 * because of the timing of a checkpoint; in that case we did nothing
	 * above and we shouldn't update shmem here.  That interaction needs more
	 * analysis.
	 */
	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	if (log->meta.end < end)
		log->meta.end = end;
	LWLockRelease(&log->mutex);
}

/*
 * Get an insertion point that is guaranteed to be backed by enough space to
 * hold 'size' bytes of data.  To actually write into the undo log, client
 * code should call this first and then use bufmgr routines to access buffers
 * and provide WAL logs and redo handlers.  In other words, while this module
 * looks after making sure the undo log has sufficient space and the undo meta
 * data is crash safe, the *contents* of the undo log and (indirectly) the
 * insertion point are the responsibility of client code.
 *
 * Note: As an optimization, we could take a third argument 'discard_last'.  If
 * the caller knows that the last transaction it committed is all visible and
 * has its undo pointer, it could supply that value.  Then while we hold
 * log->mutex we could check if log->meta.discard == discard_last, and if it's
 * in the same undo log segment as the current insert then it could cheaply
 * update it in shmem and include the value in the existing
 * XLOG_UNDOLOG_ATTACH WAL record.  We'd be leaving the heavier lifting of
 * dealing with segment roll-over to undo workers, but avoiding work for undo
 * workers by folding a super cheap common case into the next foreground xact.
 * (Not sure how we actually avoid waking up the undo work though...)
 *
 * Note: Problem: if foreground processes can move the discard pointer as well
 * as background processes (undo workers), then how is the undo worker
 * supposed to access the undo data pointed to by the discard pointer so that
 * it can read the xid?  We certainly don't want to hold the undo log lock
 * while doing stuff like that, because it would interfere with read-only
 * sessions that need to check the discard pointer.  Possible solution: we may
 * need a way to 'pin' the discard pointer while the undo worker is
 * considering what to do.  If we add 'discard_last' as described in the
 * previous paragraph, that optimisation would need to be skipped if the
 * foreground process running UndoLogAllocate sees that the discard pointer is
 * currently pinned by a background worker.  Going to sit on this thought for
 * a little while before writing any code... need to contemplate undo workers
 * some more.
 *
 * Returns an undo log insertion point that can be converted to a buffer tag
 * and an insertion point within a buffer page using the macros above.
 */
UndoRecPtr
UndoLogAllocate(size_t size, UndoPersistence persistence)
{
	UndoLogControl *log = MyUndoLogState.logs[persistence];
	UndoLogOffset new_insert;
	TransactionId logxid;

	/*
	 * We may need to attach to an undo log, either because this is the first
	 * time this backend as needed to write to an undo log at all or because
	 * the undo_tablespaces GUC was changed.  When doing that, we'll need
	 * interlocking against tablespaces being concurrently dropped.
	 */

retry:
	/* See if we need to check the undo_tablespaces GUC. */
	if (unlikely(MyUndoLogState.need_to_choose_tablespace || log == NULL))
	{
		Oid			tablespace;
		bool		need_to_unlock;

		need_to_unlock =
			choose_undo_tablespace(MyUndoLogState.need_to_choose_tablespace,
								   &tablespace);
		attach_undo_log(persistence, tablespace);
		if (need_to_unlock)
			LWLockRelease(TablespaceCreateLock);
		log = MyUndoLogState.logs[persistence];
		MyUndoLogState.need_to_choose_tablespace = false;
	}

	/*
	 * If this is the first time we've allocated undo log space in this
	 * transaction, we'll record the xid->undo log association so that it can
	 * be replayed correctly. Before that, we set the first record flag to
	 * false.
	 */
	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->meta.is_first_rec = false;
	logxid = log->xid;

	if (logxid != GetTopTransactionId())
	{
		xl_undolog_attach xlrec;

		/*
		 * While we have the lock, check if we have been forcibly detached by
		 * DROP TABLESPACE.  That can only happen between transactions (see
		 * DetachUndoLogsInsTablespace()) so we only have to check for it in
		 * this branch.
		 */
		if (log->procno == INVALID_PROC_NUMBER)
		{
			LWLockRelease(&log->mutex);
			log = NULL;
			goto retry;
		}
		log->xid = GetTopTransactionId();
		log->meta.is_first_rec = true;
		LWLockRelease(&log->mutex);

		/*
		 * Temporary undo is needed only until the transaction that wrote it
		 * has ended: only the backend that owns a temporary table can read
		 * it, and that backend has applied or no longer needs the undo of its
		 * earlier transactions.  A log taken over from a backend that exited
		 * holds nothing anyone needs either.  So before the first temporary
		 * undo record of a transaction, discard everything that came before.
		 * (The discard worker never processes temporary logs.)  Not while a
		 * rollback of this backend is still to be finished, though: see
		 * PbuApplyPendingUndo().
		 */
		if (persistence == UNDO_TEMP && log->meta.discard < log->meta.insert &&
			!PbuTempUndoPending())
		{
			LWLockAcquire(&log->discard_update_lock, LW_EXCLUSIVE);
			PbuUndoLogDiscard(MakeUndoRecPtr(log->logno, log->meta.insert),
							  InvalidTransactionId);
			LWLockRelease(&log->discard_update_lock);
		}

		/* Skip the attach record for unlogged and temporary tables. */
		if (persistence == UNDO_PERMANENT)
		{
			xlrec.xid = GetTopTransactionId();
			xlrec.logno = log->logno;
			xlrec.dbid = MyDatabaseId;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xlrec));
			XLogInsert(RM_UNDOLOG_ID, XLOG_UNDOLOG_ATTACH);
		}
	}
	else
	{
		LWLockRelease(&log->mutex);
	}

	/*
	 * 'size' is expressed in usable non-header bytes.  Figure out how far we
	 * have to move insert to create space for 'size' usable bytes (stepping
	 * over any intervening headers).
	 */
	Assert(log->meta.insert % BLCKSZ >= UndoLogBlockHeaderSize);
	new_insert = UndoLogOffsetPlusUsableBytes(log->meta.insert, size);
	Assert(new_insert % BLCKSZ >= UndoLogBlockHeaderSize);

	/*
	 * We don't need to acquire log->mutex to read log->meta.insert and
	 * log->meta.end, because this backend is the only one that can modify
	 * them.
	 */
	if (unlikely(new_insert > log->meta.end))
	{
		if (new_insert > UndoLogMaxSize)
		{
			log = NULL;
			detach_current_undo_log(persistence, true);
			goto retry;
		}

		/*
		 * Extend the end of this undo log to cover new_insert (in other words
		 * round up to the segment size).
		 */
		extend_undo_log(log->logno,
						new_insert + UndoLogSegmentSize -
						new_insert % UndoLogSegmentSize);
		Assert(new_insert <= log->meta.end);
	}

	return MakeUndoRecPtr(log->logno, log->meta.insert);
}

/*
 * In recovery, we expect the xid to map to a known log which already has
 * enough space in it.
 */
UndoRecPtr
UndoLogAllocateInRecovery(TransactionId xid, size_t size,
						  UndoPersistence level)
{
	uint16		high_bits = UndoLogGetXidHigh(xid);
	uint16		low_bits = UndoLogGetXidLow(xid);
	UndoLogNumber logno;
	UndoLogControl *log;

	/*
	 * The sequence of calls to UndoLogAllocateRecovery during REDO (recovery)
	 * must match the sequence of calls to UndoLogAllocate during DO, for any
	 * given session.  The XXX_redo code for any UNDO-generating operation
	 * must use UndoLogAllocateRecovery rather than UndoLogAllocate, because
	 * it must supply the extra 'xid' argument so that we can find out which
	 * undo log number to use.  During DO, that's tracked per-backend, but
	 * during REDO the original backends/sessions are lost and we have only
	 * the Xids.
	 */
	Assert(InRecovery);

	/*
	 * Look up the undo log number for this xid.  The mapping must already
	 * have been created by an XLOG_UNDOLOG_ATTACH record emitted during the
	 * first call to UndoLogAllocate for this xid after the most recent
	 * checkpoint.
	 */
	if (MyUndoLogState.xid_map == NULL)
		elog(ERROR, "xid to undo log number map not initialized");
	if (MyUndoLogState.xid_map[high_bits] == NULL)
		elog(ERROR, "cannot find undo log number for xid %u", xid);
	logno = MyUndoLogState.xid_map[high_bits][low_bits];

	if (logno == InvalidUndoLogNumber)
		elog(ERROR, "cannot find undo log number for xid %u", xid);

	/*
	 * This log must already have been created by XLOG_UNDOLOG_CREATE records
	 * emitted by UndoLogAllocate.
	 */
	log = get_undo_log_by_number(logno);
	if (log == NULL)
		elog(ERROR, "cannot find undo log number %d for xid %u", logno, xid);

	/*
	 * This log must already have been extended to cover the requested size by
	 * XLOG_UNDOLOG_EXTEND records emitted by UndoLogAllocate, or by
	 * XLOG_UNDLOG_DISCARD records recycling segments.
	 */
	if (log->meta.end < UndoLogOffsetPlusUsableBytes(log->meta.insert, size))
		elog(ERROR,
			 "unexpectedly couldn't allocate %zu bytes in undo log number %d",
			 size, logno);

	/*
	 * By this time we have allocated a undo log in transaction so after this
	 * it will not be first undo record for the transaction.
	 */
	log->meta.is_first_rec = false;

	return MakeUndoRecPtr(logno, log->meta.insert);
}

/*
 * Advance the insertion pointer by 'size' usable (non-header) bytes.
 *
 * Caller must WAL-log this operation first, and must replay it during
 * recovery.
 */
void
UndoLogAdvance(UndoRecPtr insertion_point, size_t size, UndoPersistence persistence)
{
	UndoLogControl *log = NULL;
	UndoLogNumber logno = UndoRecPtrGetLogNo(insertion_point);

	/*
	 * During recovery, MyUndoLogState is uninitialized. Hence, we need to
	 * work more.
	 */
	log = (InRecovery) ? get_undo_log_by_number(logno)
		: MyUndoLogState.logs[persistence];

	Assert(log != NULL);
	Assert(InRecovery || logno == log->logno);
	Assert(UndoRecPtrGetOffset(insertion_point) == log->meta.insert);

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->meta.insert = UndoLogOffsetPlusUsableBytes(log->meta.insert, size);
	LWLockRelease(&log->mutex);
}

/*
 * Advance the discard pointer in one undo log, discarding all undo data
 * relating to one or more whole transactions.  The passed in undo pointer is
 * the address of the oldest data that the called would like to keep, and the
 * affected undo log is implied by this pointer, ie
 * UndoRecPtrGetLogNo(discard_pointer).
 *
 * The caller asserts that there will be no attempts to access the undo log
 * region being discarded after this moment.  This operation will cause the
 * relevant buffers to be dropped immediately, without writing any data out to
 * disk.  Any attempt to read the buffers (except a partial buffer at the end
 * of this range which will remain) may result in IO errors, because the
 * underlying segment file may physically removed.
 *
 * Only one backend should call this for a given undo log concurrently, or
 * data structures will become corrupted.  It is expected that the caller will
 * be an undo worker; only one undo worker should be working on a given undo
 * log at a time.
 *
 * Note: Special case for when we wrapped past the end of an undo log, spilling
 * into a new one.  How do we discard that?  Essentially we'll be discarding
 * the whole undo log, but not sure how the caller should know that or deal
 * with it and how this code should handle it.
 */
void
PbuUndoLogDiscard(UndoRecPtr discard_point, TransactionId xid)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(discard_point);
	UndoLogControl *log = get_undo_log_by_number(logno);
	UndoLogOffset old_discard;
	UndoLogOffset discard = UndoRecPtrGetOffset(discard_point);
	UndoLogOffset end;
	int			segno;
	int			new_segno;
	bool		need_to_flush_wal = false;

	if (log == NULL)
		elog(ERROR, "cannot advance discard pointer for unknown undo log %d",
			 logno);

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	if (discard > log->meta.insert)
		elog(ERROR, "cannot move discard point past insert point");
	old_discard = log->meta.discard;
	if (discard < old_discard)
		elog(ERROR, "cannot move discard pointer backwards");
	end = log->meta.end;
	LWLockRelease(&log->mutex);

	/*
	 * Drop all buffers holding this undo data out of the buffer pool (except
	 * the last one, if the new location is in the middle of it somewhere), so
	 * that the contained data doesn't ever touch the disk.  The caller
	 * promises that this data will not be needed again.  We have to drop the
	 * buffers from the buffer pool before removing files, otherwise a
	 * concurrent session might try to write the block to evict the buffer.
	 */
	forget_undo_buffers(logno, old_discard, discard, false);

	/*
	 * Check if we crossed a segment boundary and need to do some synchronous
	 * filesystem operations.
	 */
	segno = old_discard / UndoLogSegmentSize;
	new_segno = discard / UndoLogSegmentSize;
	if (segno < new_segno)
	{
		int			recycle;
		UndoLogOffset pointer;

		/*
		 * We WAL-log the discards of permanent undo logs, but we only need to
		 * flush the WAL if we have performed a filesystem operation.
		 */
		need_to_flush_wal = true;

		/*
		 * Note: When we rename or unlink a file, it's possible that some
		 * backend still has it open because it has recently read a page from
		 * it.  smgr/undofile.c in any such backend will eventually close it,
		 * because it considers that fd to belong to the file with the name
		 * that we're unlinking or renaming and it doesn't like to keep more
		 * than one open at a time.  No backend should ever try to read from
		 * such a file descriptor; that is what it means when we say that the
		 * caller of PbuUndoLogDiscard() asserts that there will be no
		 * attempts to access the discarded range of undo log!  In the case of
		 * a rename, if a backend were to attempt to read undo data in the
		 * range being discarded, it would read entirely the wrong data.
		 *
		 * Note: What defenses could we build against that happening due to
		 * bugs/corruption?  One way would be for undofile.c to refuse to read
		 * buffers from before the current discard point, but currently
		 * undofile.c doesn't need to deal with shmem/locks.  That may be
		 * false economy, but we really don't want reader to have to wait to
		 * acquire the undo log lock just to read undo data while we are doing
		 * filesystem stuff in here.
		 */

		/* Replace one segment unless there is already a spare one. */
		if (log->meta.end - log->meta.insert < UndoLogSegmentSize)
			recycle = 1;
		else
			recycle = 0;

		/* Rewind to the start of the segment. */
		pointer = segno * UndoLogSegmentSize;

		while (pointer < new_segno * UndoLogSegmentSize)
		{
			char		discard_path[MAXPGPATH];

			/* Tell the checkpointer that the file is going away. */
			undofile_forget_sync(log->logno, pointer / UndoLogSegmentSize,
								 log->meta.tablespace);

			UndoLogSegmentPath(logno, pointer / UndoLogSegmentSize,
							   log->meta.tablespace, discard_path);

			/*
			 * Never rename pages whose checksums include their old block
			 * numbers.
			 */
			if (recycle > 0)
			{
				/*
				 * End points one byte past the end of the current undo space,
				 * ie to the first byte of the segment file we want to create.
				 */
				if (unlink(discard_path) == 0)
				{
					allocate_empty_undo_segment(logno, log->meta.tablespace, end);
					end += UndoLogSegmentSize;
					--recycle;
				}
				else
				{
					elog(ERROR, "could not remove \"%s\": %m",
						 discard_path);
				}
			}
			else
			{
				if (unlink(discard_path) != 0)
					elog(ERROR, "could not unlink \"%s\": %m", discard_path);
			}
			pointer += UndoLogSegmentSize;
		}
	}

	/*
	 * WAL-log the discard of a permanent undo log.  Unlogged and temporary
	 * undo logs are not WAL-logged at all (see extend_undo_log()): a temp log
	 * is discarded at the first temp write of each transaction, and
	 * WAL-logging that would cost every such transaction a WAL record, and at
	 * a segment boundary a WAL flush, for nothing.
	 */
	if (log->meta.persistence == UNDO_PERMANENT)
	{
		xl_undolog_discard xlrec;
		XLogRecPtr	ptr;

		xlrec.logno = logno;
		xlrec.discard = discard;
		xlrec.end = end;
		xlrec.latestxid = xid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		ptr = XLogInsert(RM_UNDOLOG_ID, XLOG_UNDOLOG_DISCARD);

		if (need_to_flush_wal)
			XLogFlush(ptr);
	}

	/* Update shmem to show the new discard and end pointers. */
	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->meta.discard = discard;
	log->meta.end = end;
	LWLockRelease(&log->mutex);
}

Oid
UndoRecPtrGetTablespace(UndoRecPtr ptr)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(ptr);
	UndoLogControl *log = get_undo_log_by_number(logno);

	/*
	 * Note: What should the behaviour of this function be if you ask for the
	 * tablespace of a discarded log, where even the shmem bank is gone?
	 */

	/*
	 * No need to acquire log->mutex, because log->meta.tablespace is constant
	 * for the lifetime of the log.
	 */
	if (log != NULL)
		return log->meta.tablespace;
	else
		return InvalidOid;
}

/*
 * Return first valid UndoRecPtr for a given undo logno.  If logno is invalid
 * then return InvalidUndoRecPtr.
 */
UndoRecPtr
UndoLogGetFirstValidRecord(UndoLogNumber logno)
{
	UndoLogControl *log = get_undo_log_by_number(logno);

	if (log == NULL || log->meta.discard == log->meta.insert)
		return InvalidUndoRecPtr;

	return MakeUndoRecPtr(logno, log->meta.discard);
}

/*
 * Return the ext insert location.  This will also validate the input xid
 * if latest insert point is not for the same transaction id then this will
 * return Invalid Undo pointer.
 */
UndoRecPtr
UndoLogGetNextInsertPtr(UndoLogNumber logno, TransactionId xid)
{
	UndoLogControl *log = get_undo_log_by_number(logno);
	TransactionId logxid;
	UndoRecPtr	insert;

	LWLockAcquire(&log->mutex, LW_SHARED);
	insert = log->meta.insert;
	logxid = log->xid;
	LWLockRelease(&log->mutex);

	if (TransactionIdIsValid(logxid) && !TransactionIdEquals(logxid, xid))
		return InvalidUndoRecPtr;

	return MakeUndoRecPtr(logno, insert);
}

/*
 * Rewind the undo log insert position also set the prevlen in the mata
 */
void
UndoLogRewind(UndoRecPtr insert_urp)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(insert_urp);
	UndoLogControl *log = get_undo_log_by_number(logno);
	UndoLogOffset insert = UndoRecPtrGetOffset(insert_urp);

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->meta.insert = insert;

	/*
	 * Force the wal log on next undo allocation. So that during recovery undo
	 * insert location is consistent with normal allocation.
	 */
	log->need_attach_wal_record = true;
	LWLockRelease(&log->mutex);

	/* WAL log the rewind. */
	{
		xl_undolog_rewind xlrec;

		xlrec.logno = logno;
		xlrec.insert = insert;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		XLogInsert(RM_UNDOLOG_ID, XLOG_UNDOLOG_REWIND);
	}
}

/*
 * Delete unreachable files under pg_undo.  Any files corresponding to LSN
 * positions before the previous checkpoint are no longer needed.
 */
static void
CleanUpUndoCheckPointFiles(XLogRecPtr checkPointRedo)
{
	DIR		   *dir;
	struct dirent *de;
	char		path[MAXPGPATH];
	char		oldest_path[MAXPGPATH];

	/*
	 * Keep only those >= the previous checkpoint's redo point.  A file-level
	 * copy (base backup, pg_rewind) can therefore lack the snapshot for the
	 * redo point it restarts from; StartupUndoLogs() copes with that.
	 */
	snprintf(oldest_path, MAXPGPATH, "%016llX",
			 (unsigned long long) checkPointRedo);
	dir = AllocateDir("pg_undo");
	while ((de = ReadDir(dir, "pg_undo")) != NULL)
	{
		/*
		 * Assume that fixed width uppercase hex strings sort the same way as
		 * the values they represent, so we can use strcmp to identify undo
		 * log snapshot files corresponding to checkpoints that we don't need
		 * anymore.  This assumption holds for ASCII.
		 */
		if (!(strlen(de->d_name) == UNDO_CHECKPOINT_FILENAME_LENGTH))
			continue;

		if (UndoCheckPointFilenamePrecedes(de->d_name, oldest_path))
		{
			snprintf(path, MAXPGPATH, "pg_undo/%s", de->d_name);
			if (unlink(path) != 0)
				elog(ERROR, "could not unlink file \"%s\": %m", path);
		}
	}
	FreeDir(dir);
}

/*
 * Write out the undo log meta data to the pg_undo directory.  The actual
 * contents of undo logs is in shared buffers and therefore handled by
 * CheckPointBuffers(), but here we record the table of undo logs and their
 * properties.
 */
void
CheckPointUndoLogs(XLogRecPtr checkPointRedo, XLogRecPtr priorCheckPointRedo)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	UndoLogMetaData *serialized = NULL;
	UndoLogNumber low_logno;
	UndoLogNumber high_logno;
	UndoLogNumber logno;
	size_t		serialized_size = 0;
	char	   *data;
	char		path[MAXPGPATH];
	int			num_logs;
	int			fd;
	pg_crc32c	crc;

	/*
	 * Take this opportunity to check if we can free up any DSM segments and
	 * also some entries in the checkpoint file by forgetting about entirely
	 * discarded undo logs.  Otherwise both would eventually grow large.
	 */
	LWLockAcquire(UndoLogLock, LW_EXCLUSIVE);
	while (shared->low_logno < shared->high_logno)
	{
		UndoLogControl *log;

		log = get_undo_log_by_number(shared->low_logno);
		if (log->meta.status != UNDO_LOG_STATUS_DISCARDED)
			break;

		/*
		 * If this was the last slot in a bank, the bank is no longer needed.
		 * The shared memory will be given back to the operating system once
		 * every attached backend runs undolog_bank_gc().
		 */
		if (UndoLogNoGetSlotNo(shared->low_logno + 1) == 0)
			shared->banks[UndoLogNoGetBankNo(shared->low_logno)] =
				DSM_HANDLE_INVALID;

		++shared->low_logno;
	}
	LWLockRelease(UndoLogLock);

	/* Detach from any banks that we don't need if low_logno advanced. */
	undolog_bank_gc();

	/*
	 * We acquire UndoLogLock to prevent any undo logs from being created or
	 * discarded while we build a snapshot of them.  This isn't expected to
	 * take long on a healthy system because the number of active logs should
	 * be around the number of backends.  Holding this lock won't prevent
	 * concurrent access to the undo log, except when segments need to be
	 * added or removed.
	 */
	LWLockAcquire(UndoLogLock, LW_SHARED);

	low_logno = shared->low_logno;
	high_logno = shared->high_logno;
	num_logs = high_logno - low_logno;

	/*
	 * Rather than doing the file IO while we hold the lock, we'll copy it
	 * into a palloc'd buffer.
	 */
	if (num_logs > 0)
	{
		serialized_size = sizeof(UndoLogMetaData) * num_logs;
		serialized = (UndoLogMetaData *) palloc0(serialized_size);

		for (logno = low_logno; logno != high_logno; ++logno)
		{
			UndoLogControl *log;

			log = get_undo_log_by_number(logno);
			if (log == NULL)	/* can this happen? */
				continue;

			/* Capture snapshot while holding the mutex. */
			LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
			log->need_attach_wal_record = true;
			memcpy(&serialized[logno - low_logno], &log->meta,
				   sizeof(UndoLogMetaData));
			LWLockRelease(&log->mutex);
		}
	}

	LWLockRelease(UndoLogLock);

	/* Dump into a file under pg_undo. */

	/*
	 * Ensure the pg_undo directory exists.  initdb does not create it (the
	 * per-backend engine is optional), so create it lazily and idempotently
	 * the first time a checkpoint persists undo-log metadata.
	 */
	(void) MakePGDirectory("pg_undo");

	snprintf(path, MAXPGPATH, "pg_undo/%016" INT64_MODIFIER "X",
			 (unsigned long long) checkPointRedo);
	pgstat_report_wait_start(WAIT_EVENT_UNDO_CHECKPOINT_WRITE);
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	/* Compute header checksum. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &low_logno, sizeof(low_logno));
	COMP_CRC32C(crc, &high_logno, sizeof(high_logno));
	FIN_CRC32C(crc);

	/* Write out range of active log numbers + crc. */
	if ((write(fd, &low_logno, sizeof(low_logno)) != sizeof(low_logno)) ||
		(write(fd, &high_logno, sizeof(high_logno)) != sizeof(high_logno)) ||
		(write(fd, &crc, sizeof(crc)) != sizeof(crc)))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", path)));

	/* Write out the meta data for all undo logs in that range. */
	data = (char *) serialized;
	INIT_CRC32C(crc);
	while (serialized_size > 0)
	{
		ssize_t		written;

		written = write(fd, data, serialized_size);
		if (written < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", path)));
		COMP_CRC32C(crc, data, written);
		serialized_size -= written;
		data += written;
	}
	FIN_CRC32C(crc);

	if (write(fd, &crc, sizeof(crc)) != sizeof(crc))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", path)));


	/* Flush file and directory entry. */
	pgstat_report_wait_start(WAIT_EVENT_UNDO_CHECKPOINT_SYNC);
	pg_fsync(fd);
	CloseTransientFile(fd);
	fsync_fname("pg_undo", true);
	pgstat_report_wait_end();

	if (serialized)
		pfree(serialized);

	CleanUpUndoCheckPointFiles(priorCheckPointRedo);
	undolog_xid_map_gc();
}

/*
 * Does directory 'path' hold any file?  A missing directory holds none.
 */
static bool
undo_dir_has_files(const char *path)
{
	DIR		   *dir = AllocateDir(path);
	struct dirent *de;
	bool		found = false;

	if (dir == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m", path)));
		return false;
	}
	while (!found && (de = ReadDir(dir, path)) != NULL)
		found = strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0;
	FreeDir(dir);
	return found;
}

/*
 * Has this cluster ever written an undo log segment, in any tablespace?
 */
static bool
undo_segments_exist(void)
{
	DIR		   *dir;
	struct dirent *de;
	char		path[MAXPGPATH];
	bool		found = undo_dir_has_files("base/undo");

	dir = AllocateDir(PG_TBLSPC_DIR);
	while (!found && (de = ReadDir(dir, PG_TBLSPC_DIR)) != NULL)
	{
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, MAXPGPATH, "%s/%s/%s/undo", PG_TBLSPC_DIR, de->d_name,
				 TABLESPACE_VERSION_DIRECTORY);
		found = undo_dir_has_files(path);
	}
	FreeDir(dir);
	return found;
}

/*
 * pg_undo/<redo> is missing: pick the snapshot to start from instead, and
 * return false if pg_undo holds none.
 *
 * File-level copies lose the exact snapshot.  pg_rewind copies pg_undo/ from
 * a source whose later checkpoints have already pruned the snapshot for the
 * common checkpoint the target restarts from; a base backup misses its start
 * checkpoint's snapshot if two checkpoints complete before pg_undo/ is
 * copied.  The copy then holds a snapshot named after a LATER redo point of
 * the same history, which the WAL to be replayed passes through.  Start from
 * the oldest such snapshot, as CLOG copied "from the future" is used: undo
 * log redo is idempotent against a newer state (see undolog_redo_get_log()).
 *
 * Failing that, use the newest older snapshot.  That is only right when the
 * undo logs did not change after it, which is what pg_resetwal leaves behind:
 * a new redo point past the clean-shutdown checkpoint that wrote the
 * snapshot.
 */
static bool
choose_undo_snapshot(XLogRecPtr redo, char *path)
{
	DIR		   *dir;
	struct dirent *de;
	uint64		newer = 0;
	uint64		older = 0;

	dir = AllocateDir("pg_undo");
	if (dir == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m", "pg_undo")));
		return false;
	}
	while ((de = ReadDir(dir, "pg_undo")) != NULL)
	{
		uint64		lsn;

		if (strlen(de->d_name) != UNDO_CHECKPOINT_FILENAME_LENGTH ||
			strspn(de->d_name, "0123456789ABCDEF") != UNDO_CHECKPOINT_FILENAME_LENGTH)
			continue;
		lsn = strtou64(de->d_name, NULL, 16);
		if (lsn > redo && (newer == 0 || lsn < newer))
			newer = lsn;
		else if (lsn < redo && lsn > older)
			older = lsn;
	}
	FreeDir(dir);

	if (newer == 0 && older == 0)
		return false;
	snprintf(path, MAXPGPATH, "pg_undo/%016llX",
			 (unsigned long long) (newer != 0 ? newer : older));
	ereport(LOG,
			(errmsg("undo log checkpoint snapshot for redo point %X/%08X is missing, using \"%s\"",
					LSN_FORMAT_ARGS(redo), path)));
	return true;
}

void
StartupUndoLogs(XLogRecPtr checkPointRedo)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	char		path[MAXPGPATH];
	int			logno;
	int			fd;
	pg_crc32c	crc;
	pg_crc32c	new_crc;

	/* If initdb is calling, there is no file to read yet. */
	if (IsBootstrapProcessingMode())
		return;

	/* Open the pg_undo file corresponding to the given checkpoint. */
	snprintf(path, MAXPGPATH, "pg_undo/%016" INT64_MODIFIER "X",
			 (unsigned long long) checkPointRedo);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0 && errno == ENOENT)
	{
		if (!choose_undo_snapshot(checkPointRedo, path))
		{
			/*
			 * No snapshot at all.  That is only valid for a cluster that has
			 * no undo logs, and those leave no segment files.  Otherwise
			 * starting with an empty inventory would lose the undo logs, and
			 * with them the history of every version chain pointing into
			 * them.
			 */
			if (undo_segments_exist())
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("undo log checkpoint snapshot \"%s\" is missing", path),
						 errdetail("Undo log segment files exist, but directory \"%s\" holds no snapshot of the undo log inventory.",
								   "pg_undo"),
						 errhint("Restore directory \"%s\" from the backup or server this data directory was copied from.",
								 "pg_undo")));
			return;
		}
		fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	}
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	pgstat_report_wait_start(WAIT_EVENT_UNDO_CHECKPOINT_READ);

	/* Read the active log number range. */
	if ((read(fd, &shared->low_logno, sizeof(shared->low_logno))
		 != sizeof(shared->low_logno)) ||
		(read(fd, &shared->high_logno, sizeof(shared->high_logno))
		 != sizeof(shared->high_logno)) ||
		(read(fd, &crc, sizeof(crc)) != sizeof(crc)))
		elog(ERROR, "pg_undo file \"%s\" is corrupted", path);

	/* Verify the header checksum. */
	INIT_CRC32C(new_crc);
	COMP_CRC32C(new_crc, &shared->low_logno, sizeof(shared->low_logno));
	COMP_CRC32C(new_crc, &shared->high_logno, sizeof(shared->high_logno));
	FIN_CRC32C(new_crc);

	if (crc != new_crc)
		elog(ERROR,
			 "pg_undo file \"%s\" has incorrect checksum", path);

	/* Initialize all the logs and set up the freelist. */
	INIT_CRC32C(new_crc);
	for (logno = shared->low_logno; logno < shared->high_logno; ++logno)
	{
		UndoLogControl *log;

		/* Get a zero-initialized control objects. */
		ensure_undo_log_number(logno);
		log = get_undo_log_by_number(logno);

		/* Read in the meta data for this undo log. */
		if (read(fd, &log->meta, sizeof(log->meta)) != sizeof(log->meta))
			elog(ERROR, "corrupted pg_undo meta data in file \"%s\": %m",
				 path);
		COMP_CRC32C(new_crc, &log->meta, sizeof(log->meta));

		/*
		 * At normal start-up, or during recovery, all active undo logs start
		 * out on the appropriate free list.
		 */
		log->procno = INVALID_PROC_NUMBER;
		log->pid = InvalidPid;
		log->xid = InvalidTransactionId;
		if (log->meta.status == UNDO_LOG_STATUS_ACTIVE)
		{
			log->next_free = shared->free_lists[log->meta.persistence];
			shared->free_lists[log->meta.persistence] = logno;
		}
	}
	FIN_CRC32C(new_crc);

	/* Verify body checksum. */
	if (read(fd, &crc, sizeof(crc)) != sizeof(crc))
		elog(ERROR, "pg_undo file \"%s\" is corrupted", path);
	if (crc != new_crc)
		elog(ERROR,
			 "pg_undo file \"%s\" has incorrect checksum", path);

	CloseTransientFile(fd);
	pgstat_report_wait_end();
}

/*
 * WAL-LOG undo log meta data information before inserting the first WAL after
 * the checkpoint for any undo log.
 *
 * Only the permanent undo log is replayed.  The meta of an unlogged or
 * temporary undo log must never reach the WAL: replay would map the
 * transaction to that log, and the replay of the transaction's next permanent
 * undo record would then allocate in the wrong log.  It would also stamp the
 * permanent log's LSN (NeedUndoMetaLog), suppressing the permanent log's own
 * first-after-checkpoint record.
 */
void
LogUndoMetaData(xl_undolog_meta *xlrec)
{
	XLogRecPtr	RedoRecPtr;
	XLogRecPtr	recptr;

	if (xlrec->meta.persistence != UNDO_PERMANENT)
		return;

	/*
	 * Use the current redo point, not GetFullPageWriteInfo()'s backend-local
	 * cached copy.  That cached copy is refreshed only inside
	 * XLogInsertRecord, so between a backend's WAL inserts it can lag an
	 * intervening checkpoint. NeedUndoMetaLog() would then compare log->lsn
	 * against a stale (older) redo point, wrongly conclude the meta was
	 * already logged this checkpoint cycle, and skip it -- leaving the undo
	 * record that follows as the first undo record after the new redo point
	 * with no XLOG_UNDOLOG_ATTACH/META before it.  Recovery replaying from
	 * that redo point would then meet an undo record for an xid its
	 * xid->logno map was never told about and die with "xid to undo log
	 * number map not initialized".  GetRedoRecPtr() reads the shared redo
	 * point and refreshes the local copy, so the decision is made against the
	 * checkpoint actually in effect.
	 */
	RedoRecPtr = GetRedoRecPtr();

	if (NeedUndoMetaLog(RedoRecPtr))
	{
		XLogBeginInsert();
		XLogRegisterData((char *) xlrec, sizeof(xl_undolog_meta));
		recptr = XLogInsert(RM_UNDOLOG_ID, XLOG_UNDOLOG_META);
		UndoLogSetLSN(recptr);
	}
}

/*
 * CaptureCurrentUndoLogMeta - snapshot the CURRENT (post-insert-advance) meta
 * of the permanent undo log this backend is attached to.
 *
 * LogUndoMetaData()/PrepareUndoInsert capture the meta BEFORE the insert
 * pointer is advanced (undometa->meta = log->meta then UndoLogAdvance).  A
 * producer that makes its undo crash-safe via a full-page image of the undo
 * page (rather than by re-running the undo insert during its own redo) must
 * also replay the advanced insert pointer, or the recovered log looks empty
 * (insert == discard) even though the page holds data.  Call this AFTER
 * InsertPreparedUndo() to capture the advanced insert, then LogUndoMetaDataNow()
 * to WAL-log it so undolog_xlog_meta() restores the correct insert pointer.
 *
 * Returns false (leaving *xlrec untouched) if this backend is not attached to
 * a permanent undo log.
 */
bool
CaptureCurrentUndoLogMeta(xl_undolog_meta *xlrec)
{
	UndoLogControl *log = MyUndoLogState.logs[UNDO_PERMANENT];

	if (log == NULL)
		return false;

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	xlrec->meta = log->meta;
	xlrec->logno = log->logno;
	xlrec->xid = log->xid;
	LWLockRelease(&log->mutex);

	return true;
}

/*
 * LogUndoMetaDataNow - unconditionally WAL-log the given undo-log meta.
 *
 * Unlike LogUndoMetaData(), this is not gated on NeedUndoMetaLog(): it always
 * emits the XLOG_UNDOLOG_META record.  Used by FPI-based producers to record
 * the post-advance insert pointer for EVERY undo record they write (see
 * CaptureCurrentUndoLogMeta), so recovery restores the correct insert pointer
 * regardless of checkpoint boundaries.
 */
void
LogUndoMetaDataNow(xl_undolog_meta *xlrec)
{
	XLogRecPtr	recptr;

	XLogBeginInsert();
	XLogRegisterData((char *) xlrec, sizeof(xl_undolog_meta));
	recptr = XLogInsert(RM_UNDOLOG_ID, XLOG_UNDOLOG_META);
	UndoLogSetLSN(recptr);
}

/*
 * Check whether we need to log undolog meta or not.
 */
bool
NeedUndoMetaLog(XLogRecPtr redo_point)
{
	UndoLogControl *log = MyUndoLogState.logs[UNDO_PERMANENT];

	/*
	 * If the current session is not attached to any undo log then we don't
	 * need to log meta.  It is quite possible that some operations skip
	 * writing undo, so those won't be attached to any undo log.
	 */
	if (log == NULL)
		return false;

	Assert(AmAttachedToUndoLog(log));

	if (log->lsn <= redo_point)
		return true;

	return false;
}

/*
 * Update the WAL lsn in the undo.  This is to test whether we need to include
 * the xid to logno mapping information in the next WAL or not.
 */
void
UndoLogSetLSN(XLogRecPtr lsn)
{
	UndoLogControl *log = MyUndoLogState.logs[UNDO_PERMANENT];

	Assert(AmAttachedToUndoLog(log));
	log->lsn = lsn;
}

/*
 * Get an UndoLogControl pointer for a given logno.  This may require
 * attaching to a DSM segment if it isn't already attached in this backend.
 * Return NULL if there is no such logno because it has been entirely
 * discarded.
 */
static UndoLogControl *
get_undo_log_by_number(UndoLogNumber logno)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	int			bankno;
	int			slotno;

	/*
	 * Undo log numbers start at 1 (shared->low_logno is initialised to 1);
	 * logno 0 and InvalidUndoLogNumber (-1) are never allocated.  Without
	 * this guard, a stale UndoRecPtr whose log number decodes to 0 (e.g. a
	 * slot's urec_ptr that a concurrent transaction reset while we held only
	 * a shared content lock) would index banks[0][0] and hand back an
	 * uninitialised UndoLogControl.  Reading undo from it derives
	 * RelFileNumber 0 and trips the RelFileNumberIsValid() assert in
	 * smgropen() (seen from GetLockerTransInfo -> UndoFetchRecord under a
	 * 16-way concurrent load).  Return NULL so every caller's existing "log
	 * == NULL -> record is gone" path handles it safely.
	 */
	if (logno < 1)
		return NULL;

	bankno = UndoLogNoGetBankNo(logno);
	slotno = UndoLogNoGetSlotNo(logno);

	/* See if we need to attach to the bank that holds logno. */
	if (unlikely(MyUndoLogState.banks[bankno] == NULL))
	{
		dsm_segment *segment;

		if (shared->banks[bankno] != DSM_HANDLE_INVALID)
		{
			segment = dsm_attach(shared->banks[bankno]);
			if (segment != NULL)
			{
				MyUndoLogState.bank_segments[bankno] = segment;
				MyUndoLogState.banks[bankno] = dsm_segment_address(segment);

				dsm_pin_mapping(segment);
			}
		}

		if (unlikely(MyUndoLogState.banks[bankno] == NULL))
			return NULL;
	}

	return &MyUndoLogState.banks[bankno][slotno];
}

UndoLogControl *
UndoLogGet(UndoLogNumber logno)
{
	return get_undo_log_by_number(logno);
}

/*
 * We write the undo log number into each UndoLogControl object.
 */
static void
initialize_undo_log_bank(int bankno, UndoLogControl *bank)
{
	int			i;

	for (i = 0; i < UndoLogsPerBank; ++i)
	{
		bank[i].logno = UndoLogsPerBank * bankno + i;

		/*
		 * The bank arrives zeroed, and 0 is a valid ProcNumber, so procno
		 * must be set to the unattached sentinel explicitly.  Without this an
		 * untouched slot would claim to be owned by the executor in PGPROC
		 * slot 0.
		 */
		bank[i].procno = INVALID_PROC_NUMBER;
		bank[i].pid = InvalidPid;

		LWLockInitialize(&bank[i].mutex, LWTRANCHE_UNDO_LOG_PBU);
		LWLockInitialize(&bank[i].discard_lock, LWTRANCHE_UNDO_LOG_PBU);
		LWLockInitialize(&bank[i].discard_update_lock, LWTRANCHE_UNDO_LOG_PBU);
	}
}

/*
 * Create shared memory space for a given undo log number, if it doesn't exist
 * already.
 */
static void
ensure_undo_log_number(UndoLogNumber logno)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	int			bankno = UndoLogNoGetBankNo(logno);

	/* In single-user mode, we have to use backend-private memory. */
	if (!IsUnderPostmaster)
	{
		if (MyUndoLogState.banks[bankno] == NULL)
		{
			size_t		size;

			size = sizeof(UndoLogControl) * UndoLogsPerBank;
			MyUndoLogState.banks[bankno] =
				MemoryContextAllocZero(TopMemoryContext, size);

			initialize_undo_log_bank(bankno, MyUndoLogState.banks[bankno]);
		}
		return;
	}

	/* Do we need to create a bank in shared memory for this undo log number? */
	if (shared->banks[bankno] == DSM_HANDLE_INVALID)
	{
		dsm_segment *segment;
		size_t		size;

		size = sizeof(UndoLogControl) * UndoLogsPerBank;
		segment = dsm_create(size, 0);
		dsm_pin_mapping(segment);
		dsm_pin_segment(segment);
		memset(dsm_segment_address(segment), 0, size);
		shared->banks[bankno] = dsm_segment_handle(segment);
		MyUndoLogState.banks[bankno] = dsm_segment_address(segment);

		initialize_undo_log_bank(bankno, MyUndoLogState.banks[bankno]);
	}
}

/*
 * Attach to an undo log, possibly creating or recycling one.
 */
static void
attach_undo_log(UndoPersistence persistence, Oid tablespace)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	UndoLogControl *log = NULL;
	UndoLogNumber logno;
	UndoLogNumber *place;

	Assert(!InRecovery);
	Assert(MyUndoLogState.logs[persistence] == NULL);

	LWLockAcquire(UndoLogLock, LW_EXCLUSIVE);

	/*
	 * Unattached logs form a linked list per persistence level.  Finding a
	 * matching tablespace requires a linear scan; with one tablespace the
	 * first entry suffices.
	 */
	place = &shared->free_lists[persistence];
	while (*place != InvalidUndoLogNumber)
	{
		UndoLogControl *candidate = get_undo_log_by_number(*place);

		if (candidate == NULL)
			elog(ERROR, "corrupted undo log freelist");
		if (candidate->meta.tablespace == tablespace)
		{
			logno = *place;
			log = candidate;
			*place = candidate->next_free;
			break;
		}
		place = &candidate->next_free;
	}

	/*
	 * All existing undo logs for this tablespace and persistence level are
	 * busy, so we'll have to create a new one.
	 */
	if (log == NULL)
	{
		if (shared->high_logno > (1 << UndoLogNumberBits))
		{
			/*
			 * You've used up all 16 exabytes of undo log addressing space.
			 * This is a difficult state to reach using only 16 exabytes of
			 * WAL.
			 */
			elog(ERROR, "cannot create new undo log");
		}

		logno = shared->high_logno;
		ensure_undo_log_number(logno);

		/* Get new zero-filled UndoLogControl object. */
		log = get_undo_log_by_number(logno);

		Assert(log->meta.persistence == 0);
		Assert(log->meta.tablespace == InvalidOid);
		Assert(log->meta.discard == 0);
		Assert(log->meta.insert == 0);
		Assert(log->meta.end == 0);
		Assert(log->procno == INVALID_PROC_NUMBER);
		Assert(log->pid == InvalidPid);
		Assert(log->xid == 0);

		/*
		 * The insert and discard pointers start after the first block's
		 * header.  That means that insert is > end for a short time in a
		 * newly created undo log.  Is there any problem with that?
		 */
		log->meta.insert = UndoLogBlockHeaderSize;
		log->meta.discard = UndoLogBlockHeaderSize;

		log->meta.tablespace = tablespace;
		log->meta.persistence = persistence;
		log->meta.status = UNDO_LOG_STATUS_ACTIVE;

		/* Move the high log number pointer past this one. */
		++shared->high_logno;

		/* WAL-log the creation of this new undo log. */
		{
			xl_undolog_create xlrec;

			xlrec.logno = logno;
			xlrec.tablespace = log->meta.tablespace;
			xlrec.persistence = log->meta.persistence;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xlrec));
			XLogInsert(RM_UNDOLOG_ID, XLOG_UNDOLOG_CREATE);
		}

		/*
		 * This undo log has no segments.  UndoLogAllocate will create the
		 * first one on demand.
		 */
	}
	LWLockRelease(UndoLogLock);

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->procno = MyProcNumber;
	log->pid = MyProcPid;
	log->xid = InvalidTransactionId;
	log->need_attach_wal_record = true;
	LWLockRelease(&log->mutex);

	MyUndoLogState.logs[persistence] = log;
}

/*
 * Free chunks of the xid/undo log map that relate to transactions that are no
 * longer running.  This is run at each checkpoint.
 */
static void
undolog_xid_map_gc(void)
{
	UndoLogNumber **xid_map = MyUndoLogState.xid_map;
	TransactionId oldest_xid;
	uint16		new_oldest_chunk;
	uint16		oldest_chunk;

	if (xid_map == NULL)
		return;

	/*
	 * During crash recovery, it may not be possible to call GetOldestXmin()
	 * yet because latestCompletedXid is invalid.
	 */
	if (!FullTransactionIdIsNormal(ShmemVariableCache->latestCompletedXid))
		return;

	oldest_xid = GetOldestXmin(NULL, PROCARRAY_FLAGS_DEFAULT);
	new_oldest_chunk = UndoLogGetXidHigh(oldest_xid);
	oldest_chunk = MyUndoLogState.xid_map_oldest_chunk;

	while (oldest_chunk != new_oldest_chunk)
	{
		if (xid_map[oldest_chunk])
		{
			pfree(xid_map[oldest_chunk]);
			xid_map[oldest_chunk] = NULL;
		}
		oldest_chunk = (oldest_chunk + 1) % (1 << UndoLogXidHighBits);
	}
	MyUndoLogState.xid_map_oldest_chunk = new_oldest_chunk;
}

/*
 * Detach from shared memory banks that are no longer needed because they hold
 * undo logs that are entirely discarded.  This should ideally be called
 * periodically in any backend that accesses undo data, so that they have a
 * chance to detach from DSM segments that hold banks of entirely discarded
 * undo log control objects.
 */
static void
undolog_bank_gc(void)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	UndoLogNumber low_logno = shared->low_logno;

	if (unlikely(MyUndoLogState.low_logno < low_logno))
	{
		int			low_bank = UndoLogNoGetBankNo(low_logno);
		int			bank = UndoLogNoGetBankNo(MyUndoLogState.low_logno);

		while (bank < low_bank)
		{
			Assert(shared->banks[bank] == DSM_HANDLE_INVALID);
			if (MyUndoLogState.banks[bank] != NULL)
			{
				dsm_detach(MyUndoLogState.bank_segments[bank]);
				MyUndoLogState.bank_segments[bank] = NULL;
				MyUndoLogState.banks[bank] = NULL;
			}
			++bank;
		}
	}

	MyUndoLogState.low_logno = low_logno;
}

/*
 * Associate a xid with an undo log, during recovery.  In a primary server,
 * this isn't necessary because backends know which undo log they're attached
 * to.  During recovery, the natural association between backends and xids is
 * lost, so we need to manage that explicitly.
 */
static void
undolog_xid_map_add(TransactionId xid, UndoLogNumber logno)
{
	uint16		high_bits;
	uint16		low_bits;

	high_bits = UndoLogGetXidHigh(xid);
	low_bits = UndoLogGetXidLow(xid);

	if (unlikely(MyUndoLogState.xid_map == NULL))
	{
		/* First time through.  Create mapping array. */
		MyUndoLogState.xid_map =
			MemoryContextAllocZero(TopMemoryContext,
								   sizeof(UndoLogNumber *) *
								   (1 << (32 - UndoLogXidLowBits)));
		MyUndoLogState.xid_map_oldest_chunk = high_bits;
	}

	if (unlikely(MyUndoLogState.xid_map[high_bits] == NULL))
	{
		/* This bank of mappings doesn't exist yet.  Create it. */
		MyUndoLogState.xid_map[high_bits] =
			MemoryContextAllocZero(TopMemoryContext,
								   sizeof(UndoLogNumber) *
								   (1 << UndoLogXidLowBits));
	}

	/* Associate this xid with this undo log number. */
	MyUndoLogState.xid_map[high_bits][low_bits] = logno;
}

/*
 * Seed the recovery xid->logno map for an undo record whose own WAL carries
 * the log number.
 *
 * The mapping is normally created by replaying an XLOG_UNDOLOG_ATTACH (first
 * undo record of a transaction) or XLOG_UNDOLOG_META (first undo record of a
 * transaction after a checkpoint).  The META is emitted on the primary only
 * when LogUndoMetaData() judges, from the redo point then in effect, that it
 * has not been logged since the last checkpoint.  That judgement races a
 * concurrent checkpoint: the forward undo record can be assigned an LSN just
 * after a new redo point while its META was skipped, so a crash that replays
 * from that redo point meets the forward record before any ATTACH/META for its
 * xid and dies with "xid to undo log number map not initialized".
 *
 * A producer whose forward record carries both the top-level xid and the undo
 * record pointer (hence the log number) resolves this unambiguously: the record
 * names the exact log DO wrote into.  Call this from such a producer's redo,
 * before PrepareUndoInsert(), so IsTransactionFirstRec() and
 * UndoLogAllocateInRecovery() find the log regardless of whether the META was
 * emitted.  Writing the value an ATTACH/META would also write is harmless, and
 * when the transaction's undo spans logs the current record's log number is the
 * right one for this record.  log->meta (including is_first_rec and the insert
 * pointer) still comes from the checkpoint snapshot and the replayed records,
 * exactly as without this call, so replayed record sizes and offsets are
 * unchanged.
 */
void
UndoLogRecoverySetXidMap(TransactionId xid, UndoLogNumber logno)
{
	Assert(InRecovery);
	undolog_xid_map_add(xid, logno);
}

/*
 * Restore, for the redo of one undo record, the undo log state DO placed it
 * with: the record lands at urp; if first_rec it starts its transaction in
 * the log, carrying the transaction header that the previous transaction's,
 * at last_xact_start, is linked to; and the transaction belongs to database
 * dbid.  A record after a transaction's first leaves last_xact_start alone:
 * that is the transaction's own start, which the redo of its first record,
 * or a snapshot taken after that record was placed, already holds.
 *
 * The WAL alone does not otherwise determine this.  DO reserves an undo
 * record's space -- advancing the log's insert pointer, and for the first
 * record of a transaction also its last_xact_start -- before the critical
 * section, and decides there whether the log's state needs an
 * XLOG_UNDOLOG_META.  The forward WAL record that carries the undo comes
 * after.  A checkpoint can choose its redo point between the two and write
 * its undo log snapshot (CheckPointUndoLogs) after the reservation.  Recovery
 * from that redo point then starts from a snapshot that already includes the
 * record, finds no META after the redo point to put the log back, and
 * replays the record further on ("undo record pointer mismatch"), or without
 * its transaction header.  Whether a META is emitted depends on the redo
 * point DO saw, so no META-based rule closes this.
 *
 * A producer whose forward record carries these values (FLUX does) calls this
 * before PrepareUndoInsert() in its redo, so that each replayed record is
 * placed from its own WAL whatever snapshot recovery started from.  This
 * subsumes UndoLogRecoverySetXidMap().  The end of the log is not set here: it
 * only grows, and is WAL-logged (XLOG_UNDOLOG_EXTEND) before any record that
 * uses it.  The discard pointer moves back with the insert pointer if redo
 * started from a newer snapshot (see undolog_xlog_meta()); DO's own was at or
 * before the record.  A log switch in the middle of a transaction is still
 * taken from its own XLOG_UNDOLOG_SWITCH.
 */
void
UndoLogRecoverySetPlacement(TransactionId xid, Oid dbid, UndoRecPtr urp,
							bool first_rec, UndoLogOffset last_xact_start)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(urp);
	UndoLogControl *log;

	Assert(InRecovery);
	undolog_xid_map_add(xid, logno);

	log = undolog_redo_get_log(logno);
	if (log == NULL)
		return;

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->meta.insert = UndoRecPtrGetOffset(urp);
	log->meta.discard = Min(log->meta.discard, log->meta.insert);
	if (first_rec)
		log->meta.last_xact_start = last_xact_start;
	log->meta.is_first_rec = first_rec;
	log->xid = xid;
	LWLockRelease(&log->mutex);

	MyUndoLogState.dbid = dbid;
}

/* assign_hook: do extra actions as needed */
void
assign_undo_tablespaces(const char *newval, void *extra)
{
	/*
	 * This is normally called only when GetTopTransactionIdIfAny() ==
	 * InvalidTransactionId (because you can't change undo_tablespaces in the
	 * middle of a transaction that's been asigned an xid), but we can't
	 * assert that because it's also called at the end of a transaction that's
	 * rolling back, to reset the GUC if it was set inside the transaction.
	 */

	/* Tell UndoLogAllocate() to reexamine undo_tablespaces. */
	MyUndoLogState.need_to_choose_tablespace = true;
}

static bool
choose_undo_tablespace(bool force_detach, Oid *tablespace)
{
	char	   *rawname;
	List	   *namelist;
	bool		need_to_unlock;
	int			length;
	int			i;

	/*
	 * We need a modifiable copy of string.
	 *
	 * undo_tablespaces is never registered as a GUC in this build (the
	 * per-backend engine's GUCs are not wired up until FLUX migrates to it),
	 * so it stays at its C initializer NULL.  A NULL here means "no
	 * tablespace configured", which is exactly the empty-string case handled
	 * below (DEFAULTTABLESPACE_OID); treat it as such rather than
	 * pstrdup(NULL). FILEOPS is the first producer to actually allocate an
	 * undo log, so it is the first to reach this path.
	 */
	rawname = pstrdup(undo_tablespaces != NULL ? undo_tablespaces : "");

	/* Break string into list of identifiers. */
	if (!SplitIdentifierString(rawname, ',', &namelist))
		elog(ERROR, "undo_tablespaces is unexpectedly malformed");

	length = list_length(namelist);
	if (length == 0 ||
		(length == 1 && ((char *) linitial(namelist))[0] == '\0'))
	{
		/*
		 * If it's an empty string, then we'll use the default tablespace.  No
		 * locking is required because it can't be dropped.
		 */
		*tablespace = DEFAULTTABLESPACE_OID;
		need_to_unlock = false;
	}
	else
	{
		/*
		 * Choose an OID using our pid, so that if several backends have the
		 * same multi-tablespace setting they'll spread out.  We could easily
		 * do better than this if more serious load balancing is judged
		 * useful.
		 */
		int			index = MyProcNumber % length;
		int			first_index = index;
		Oid			oid = InvalidOid;

		/*
		 * Take the tablespace create/drop lock while we look the name up.
		 * This prevents the tablespace from being dropped while we're trying
		 * to resolve the name, or while the called is trying to create an
		 * undo log in it.  The caller will have to release this lock.
		 */
		LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);
		for (;;)
		{
			const char *name = list_nth(namelist, index);

			oid = get_tablespace_oid(name, true);
			if (oid == InvalidOid)
			{
				/* Unknown tablespace, try the next one. */
				index = (index + 1) % length;

				/*
				 * But if we've tried them all, it's time to complain.  We'll
				 * arbitrarily complain about the last one we tried in the
				 * error message.
				 */
				if (index == first_index)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("tablespace \"%s\" does not exist", name),
							 errhint("Create the tablespace or set undo_tablespaces to a valid or empty list.")));
				continue;
			}
			if (oid == GLOBALTABLESPACE_OID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("undo logs cannot be placed in pg_global tablespace")));
			/* If we got here we succeeded in finding one. */
			break;
		}

		Assert(oid != InvalidOid);
		*tablespace = oid;
		need_to_unlock = true;
	}

	/*
	 * If we came here because the user changed undo_tablesaces, then detach
	 * from any undo logs we happen to be attached to.
	 */
	if (force_detach)
	{
		for (i = 0; i < UndoPersistenceLevels; ++i)
		{
			UndoLogControl *log = MyUndoLogState.logs[i];
			UndoLogSharedData *shared = MyUndoLogState.shared;

			if (log != NULL)
			{
				LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
				log->procno = INVALID_PROC_NUMBER;
				log->pid = InvalidPid;
				log->xid = InvalidTransactionId;
				LWLockRelease(&log->mutex);

				LWLockAcquire(UndoLogLock, LW_EXCLUSIVE);
				log->next_free = shared->free_lists[i];
				shared->free_lists[i] = log->logno;
				LWLockRelease(UndoLogLock);

				MyUndoLogState.logs[i] = NULL;
			}
		}
	}

	return need_to_unlock;
}

bool
DropUndoLogsInTablespace(Oid tablespace)
{
	DIR		   *dir;
	char		undo_path[MAXPGPATH];
	UndoLogNumber low_logno;
	UndoLogNumber high_logno;
	UndoLogNumber logno;
	UndoLogSharedData *shared = MyUndoLogState.shared;
	int			i;

	Assert(LWLockHeldByMe(TablespaceCreateLock));
	Assert(tablespace != DEFAULTTABLESPACE_OID);

	LWLockAcquire(UndoLogLock, LW_SHARED);
	low_logno = shared->low_logno;
	high_logno = shared->high_logno;
	LWLockRelease(UndoLogLock);

	/* First, try to kick everyone off any undo logs in this tablespace. */
	for (logno = low_logno; logno < high_logno; ++logno)
	{
		UndoLogControl *log = get_undo_log_by_number(logno);
		bool		ok;
		bool		return_to_freelist = false;

		/* Skip undo logs in other tablespaces. */
		if (log->meta.tablespace != tablespace)
			continue;

		/* Check if this undo log can be forcibly detached. */
		LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
		if (log->meta.discard == log->meta.insert &&
			(log->xid == InvalidTransactionId ||
			 !TransactionIdIsInProgress(log->xid)))
		{
			log->xid = InvalidTransactionId;
			if (log->procno != INVALID_PROC_NUMBER)
			{
				log->procno = INVALID_PROC_NUMBER;
				log->pid = InvalidPid;
				return_to_freelist = true;
			}
			ok = true;
		}
		else
		{
			/*
			 * There is data we need in this undo log.  We can't force it to
			 * be detached.
			 */
			ok = false;
		}
		LWLockRelease(&log->mutex);

		/* If we failed, then give up now and report failure. */
		if (!ok)
			return false;

		/*
		 * Put this undo log back on the appropriate free-list.  No one can
		 * attach to it while we hold TablespaceCreateLock, but if we return
		 * earlier in a future go around this loop, we need the undo log to
		 * remain usable.  We'll remove all appropriate logs from the
		 * free-lists in a separate step below.
		 */
		if (return_to_freelist)
		{
			LWLockAcquire(UndoLogLock, LW_EXCLUSIVE);
			log->next_free = shared->free_lists[log->meta.persistence];
			shared->free_lists[log->meta.persistence] = logno;
			LWLockRelease(UndoLogLock);
		}
	}

	/*
	 * We detached all backends from undo logs in this tablespace, and no one
	 * can attach to any non-default-tablespace undo logs while we hold
	 * TablespaceCreateLock.  We can now drop the undo logs.
	 */
	for (logno = low_logno; logno < high_logno; ++logno)
	{
		UndoLogControl *log = get_undo_log_by_number(logno);

		/* Skip undo logs in other tablespaces. */
		if (log->meta.tablespace != tablespace)
			continue;

		/*
		 * Make sure no buffers remain.  When that is done by UndoDiscard(),
		 * the final page is left in shared_buffers because it may contain
		 * data, or at least be needed again very soon.  Here we need to drop
		 * even that page from the buffer pool.
		 */
		forget_undo_buffers(logno, log->meta.discard, log->meta.discard, true);

		/*
		 * Dropping the log permanently discards its remaining address space;
		 * log numbers are not reused in another tablespace.
		 */

		LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
		log->meta.status = UNDO_LOG_STATUS_DISCARDED;
		LWLockRelease(&log->mutex);
	}

	/* Unlink all undo segment files in this tablespace. */
	UndoLogDirectory(tablespace, undo_path);

	dir = AllocateDir(undo_path);
	if (dir != NULL)
	{
		struct dirent *de;

		while ((de = ReadDirExtended(dir, undo_path, LOG)) != NULL)
		{
			char		segment_path[MAXPGPATH];

			if (strcmp(de->d_name, ".") == 0 ||
				strcmp(de->d_name, "..") == 0)
				continue;
			snprintf(segment_path, sizeof(segment_path), "%s/%s",
					 undo_path, de->d_name);
			if (unlink(segment_path) < 0)
				elog(LOG, "couldn't unlink file \"%s\": %m", segment_path);
		}
		FreeDir(dir);
	}

	/* Remove all dropped undo logs from the free-lists. */
	LWLockAcquire(UndoLogLock, LW_EXCLUSIVE);
	for (i = 0; i < UndoPersistenceLevels; ++i)
	{
		UndoLogControl *log;
		UndoLogNumber *place;

		place = &shared->free_lists[i];
		while (*place != InvalidUndoLogNumber)
		{
			log = get_undo_log_by_number(*place);
			if (log->meta.status == UNDO_LOG_STATUS_DISCARDED)
				*place = log->next_free;
			else
				place = &log->next_free;
		}
	}
	LWLockRelease(UndoLogLock);

	return true;
}

/*
 * Rewrite all allocated undo pages after the checksum-enable barrier.
 * Unlike relations, undo logs are shared by all databases and have no catalog
 * entries.  Include discarded prefixes of partial segments, since offline
 * verification sees those too.  New segments are zero-filled and all writers
 * already generate checksums, so a snapshot of each log's extent suffices.
 */
void
UndoLogEnableChecksums(void)
{
	UndoLogControl *log = NULL;

	/* Keep DROP TABLESPACE from removing logs during the scan. */
	LWLockAcquire(TablespaceCreateLock, LW_SHARED);
	while ((log = UndoLogNext(log)) != NULL)
	{
		UndoLogMetaData meta;
		RelFileLocator rlocator;
		BlockNumber first;
		BlockNumber end;

		/* Discard can remove files, or invalidate buffers we just dirtied. */
		LWLockAcquire(&log->discard_update_lock, LW_SHARED);
		LWLockAcquire(&log->mutex, LW_SHARED);
		meta = log->meta;
		LWLockRelease(&log->mutex);
		if (meta.status != UNDO_LOG_STATUS_ACTIVE &&
			meta.status != UNDO_LOG_STATUS_EXHAUSTED)
		{
			LWLockRelease(&log->discard_update_lock);
			continue;
		}

		rlocator.spcOid = meta.tablespace;
		rlocator.dbOid = UndoLogDatabaseOid;
		rlocator.relNumber = log->logno;
		first = (meta.discard / UndoLogSegmentSize) * UNDOSEG_SIZE;
		end = meta.end / BLCKSZ;
		for (BlockNumber block = first; block < end; block++)
		{
			Buffer		buffer;

			CHECK_FOR_INTERRUPTS();
			buffer = ReadBufferWithoutRelcache(rlocator, UndoLogForkNum,
											   block, RBM_NORMAL, NULL,
											   UndoPersistenceIsPermanent(meta.persistence));
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			if (!PageIsNew(BufferGetPage(buffer)))
			{
				START_CRIT_SECTION();
				MarkBufferDirty(buffer);
				if (meta.persistence == UNDO_PERMANENT)
					log_newpage_buffer(buffer, false);
				END_CRIT_SECTION();
			}
			UnlockReleaseBuffer(buffer);
		}
		LWLockRelease(&log->discard_update_lock);
	}
	LWLockRelease(TablespaceCreateLock);
}

void
ResetUndoLogs(UndoPersistence persistence)
{
	UndoLogControl *log = NULL;

	while ((log = UndoLogNext(log)) != NULL)
	{
		DIR		   *dir;
		struct dirent *de;
		char		undo_path[MAXPGPATH];
		char		segment_prefix[MAXPGPATH];
		size_t		segment_prefix_size;
		UndoLogOffset seg;

		if (log->meta.status != UNDO_LOG_STATUS_ACTIVE ||
			log->meta.persistence != persistence)
			continue;

		/*
		 * Nothing in the buffer pool may be written back to the files we are
		 * about to remove, and the checkpointer must not try to fsync them.
		 */
		forget_undo_buffers(log->logno, 0, 0, true);
		for (seg = log->meta.discard - log->meta.discard % UndoLogSegmentSize;
			 seg < log->meta.end; seg += UndoLogSegmentSize)
			undofile_forget_sync(log->logno, seg / UndoLogSegmentSize,
								 log->meta.tablespace);

		/* Scan the directory for files belonging to this undo log. */
		snprintf(segment_prefix, sizeof(segment_prefix), "%06X.", log->logno);
		segment_prefix_size = strlen(segment_prefix);
		UndoLogDirectory(log->meta.tablespace, undo_path);
		dir = AllocateDir(undo_path);
		if (dir != NULL)
		{
			while ((de = ReadDirExtended(dir, undo_path, LOG)) != NULL)
			{
				char		segment_path[MAXPGPATH];

				if (strncmp(de->d_name, segment_prefix, segment_prefix_size) != 0)
					continue;
				snprintf(segment_path, sizeof(segment_path), "%s/%s",
						 undo_path, de->d_name);
				if (unlink(segment_path) < 0)
					elog(LOG, "could not unlink file \"%s\": %m", segment_path);
			}
			FreeDir(dir);
		}

		/*
		 * The log is now empty.  Place its insert and discard pointers at the
		 * first usable byte of the segment the next allocation will create at
		 * 'end', exactly as for a newly created log, so that no later discard
		 * can see them move backwards.  No transaction header precedes the
		 * insert point any more.
		 */
		LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
		log->meta.insert = log->meta.discard = log->meta.end +
			UndoLogBlockHeaderSize;
		log->meta.last_xact_start = log->meta.insert;
		log->meta.is_first_rec = false;
		log->xid = InvalidTransactionId;
		log->oldest_xid = InvalidTransactionId;
		log->oldest_data = InvalidUndoRecPtr;
		LWLockRelease(&log->mutex);
	}
}

/*
 * ResetNonPermanentUndoLogs
 *		Empty every unlogged and temporary undo log after a crash.
 *
 * The pages of those logs are neither WAL-logged nor written at a regular
 * checkpoint, so after a crash their files hold whatever happened to reach
 * disk, while the logs' insert and discard pointers come from the checkpoint
 * snapshot and from replayed WAL.  The tables they belong to are being reset
 * as well (unlogged tables to their init forks; temporary tables are gone), so
 * none of that undo is needed.  Called at the end of crash recovery, next to
 * ResetUnloggedRelations(UNLOGGED_RELATION_INIT).
 */
void
ResetNonPermanentUndoLogs(void)
{
	ResetUndoLogs(UNDO_UNLOGGED);
	ResetUndoLogs(UNDO_TEMP);
}

Datum
pbu_pg_stat_get_undo_logs(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_UNDO_LOGS_COLS 9
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	UndoLogNumber low_logno;
	UndoLogNumber high_logno;
	UndoLogNumber logno;
	UndoLogSharedData *shared = MyUndoLogState.shared;
	char	   *tablespace_name = NULL;
	Oid			last_tablespace = InvalidOid;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* Find the range of active log numbers. */
	LWLockAcquire(UndoLogLock, LW_SHARED);
	low_logno = shared->low_logno;
	high_logno = shared->high_logno;
	LWLockRelease(UndoLogLock);

	/* Scan all undo logs to build the results. */
	for (logno = low_logno; logno < high_logno; ++logno)
	{
		UndoLogControl *log = get_undo_log_by_number(logno);
		char		buffer[17];
		Datum		values[PG_STAT_GET_UNDO_LOGS_COLS];
		bool		nulls[PG_STAT_GET_UNDO_LOGS_COLS] = {false};
		Oid			tablespace;

		if (log == NULL)
			continue;

		/*
		 * This won't be a consistent result overall, but the values for each
		 * log will be consistent because we'll take the per-log lock while
		 * copying them.
		 */
		LWLockAcquire(&log->mutex, LW_SHARED);

		if (log->meta.status == UNDO_LOG_STATUS_DISCARDED)
		{
			LWLockRelease(&log->mutex);
			continue;
		}

		values[0] = ObjectIdGetDatum((Oid) logno);
		values[1] = CStringGetTextDatum(
										log->meta.persistence == UNDO_PERMANENT ? "permanent" :
										log->meta.persistence == UNDO_UNLOGGED ? "unlogged" :
										log->meta.persistence == UNDO_TEMP ? "temporary" : "<uknown>");
		tablespace = log->meta.tablespace;

		snprintf(buffer, sizeof(buffer), UndoRecPtrFormat,
				 (unsigned long long) MakeUndoRecPtr(logno, log->meta.discard));
		values[3] = CStringGetTextDatum(buffer);
		snprintf(buffer, sizeof(buffer), UndoRecPtrFormat,
				 (unsigned long long) MakeUndoRecPtr(logno, log->meta.insert));
		values[4] = CStringGetTextDatum(buffer);
		snprintf(buffer, sizeof(buffer), UndoRecPtrFormat,
				 (unsigned long long) MakeUndoRecPtr(logno, log->meta.end));
		values[5] = CStringGetTextDatum(buffer);
		if (log->xid == InvalidTransactionId)
			nulls[6] = true;
		else
			values[6] = TransactionIdGetDatum(log->xid);
		if (log->procno == INVALID_PROC_NUMBER)
			nulls[7] = true;
		else
			values[7] = Int32GetDatum((int64) log->pid);
		LWLockRelease(&log->mutex);

		/*
		 * Deal with potentially slow tablespace name lookup without the lock.
		 * Avoid making multiple calls to that expensive function for the
		 * common case of repeating tablespace.
		 */
		if (tablespace != last_tablespace)
		{
			if (tablespace_name)
				pfree(tablespace_name);
			tablespace_name = get_tablespace_name(tablespace);
			last_tablespace = tablespace;
		}
		if (tablespace_name)
		{
			values[2] = CStringGetTextDatum(tablespace_name);
			nulls[2] = false;
		}
		else
			nulls[2] = true;

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	if (tablespace_name)
		pfree(tablespace_name);

	return (Datum) 0;
}

/*
 * Find the undo log an UndoLog record refers to during redo.
 *
 * Redo can start from a snapshot NEWER than its redo point (see
 * choose_undo_snapshot()), so records before the snapshot meet a later state
 * of the same history.  A log below low_logno has since been dropped:
 * return NULL and the caller skips the record.  A log at or above high_logno
 * was never created as far as this inventory knows, which means the
 * inventory is wrong: fail rather than lose the log.
 */
static UndoLogControl *
undolog_redo_get_log(UndoLogNumber logno)
{
	UndoLogSharedData *shared = MyUndoLogState.shared;
	UndoLogControl *log;

	if (logno < shared->low_logno)
		return NULL;
	log = logno < shared->high_logno ? get_undo_log_by_number(logno) : NULL;
	if (log == NULL)
		elog(ERROR, "unknown undo log %d", logno);
	return log;
}

/*
 * replay the creation of a new undo log
 */
static void
undolog_xlog_create(XLogReaderState *record)
{
	xl_undolog_create *xlrec = (xl_undolog_create *) XLogRecGetData(record);
	UndoLogControl *log;
	UndoLogSharedData *shared = MyUndoLogState.shared;

	/*
	 * The log already exists, or is already gone, in a starting state newer
	 * than this record: keep that state.  Its persistence and tablespace are
	 * the ones recorded here, and the first undo insert into a new log is
	 * always preceded by an XLOG_UNDOLOG_META that sets its pointers.
	 */
	if (xlrec->logno < shared->low_logno)
		return;
	if (xlrec->logno < shared->high_logno &&
		(log = get_undo_log_by_number(xlrec->logno)) != NULL &&
		log->meta.status != UNDO_LOG_STATUS_UNUSED)
		return;

	/* Create meta-data space in shared memory. */
	ensure_undo_log_number(xlrec->logno);

	log = get_undo_log_by_number(xlrec->logno);
	log->meta.status = UNDO_LOG_STATUS_ACTIVE;
	log->meta.persistence = xlrec->persistence;
	log->meta.tablespace = xlrec->tablespace;
	log->meta.insert = UndoLogBlockHeaderSize;
	log->meta.discard = UndoLogBlockHeaderSize;

	LWLockAcquire(UndoLogLock, LW_SHARED);
	shared->high_logno = Max(xlrec->logno + 1, shared->high_logno);
	LWLockRelease(UndoLogLock);
}

/*
 * replay the addition of a new segment to an undo log
 */
static void
undolog_xlog_extend(XLogReaderState *record)
{
	xl_undolog_extend *xlrec = (xl_undolog_extend *) XLogRecGetData(record);

	if (undolog_redo_get_log(xlrec->logno) == NULL)
		return;

	/*
	 * Extend exactly as we would during DO phase.  This never shrinks the
	 * log: an end at or below the current one creates nothing.
	 */
	extend_undo_log(xlrec->logno, xlrec->end);
}

/*
 * replay the association of an xid with a specific undo log
 */
static void
undolog_xlog_attach(XLogReaderState *record)
{
	xl_undolog_attach *xlrec = (xl_undolog_attach *) XLogRecGetData(record);
	UndoLogControl *log;

	undolog_xid_map_add(xlrec->xid, xlrec->logno);

	/* Restore current dbid */
	MyUndoLogState.dbid = xlrec->dbid;

	/*
	 * Whatever follows is the first record for this transaction, so
	 * PrepareUndoInsert() adds the transaction header (UREC_INFO_TRANSACTION)
	 * to it.
	 */
	log = undolog_redo_get_log(xlrec->logno);
	if (log == NULL)
		return;
	log->meta.is_first_rec = true;
	log->xid = xlrec->xid;
}

/*
 * replay the undo-log switch wal.  Store the transaction's undo record
 * pointer of the previous log in MyUndoLogState temporarily, which will
 * be reset after reading first time.
 */
static void
undolog_xlog_switch(XLogReaderState *record)
{
	UndoRecPtr	prevlogurp = *((UndoRecPtr *) XLogRecGetData(record));

	MyUndoLogState.prevlogurp = prevlogurp;
}

/*
 * replay undo log meta-data image
 */
static void
undolog_xlog_meta(XLogReaderState *record)
{
	xl_undolog_meta *xlrec = (xl_undolog_meta *) XLogRecGetData(record);
	UndoLogControl *log;
	UndoLogOffset end;
	UndoLogStatus status;

	undolog_xid_map_add(xlrec->xid, xlrec->logno);

	log = undolog_redo_get_log(xlrec->logno);
	if (log == NULL)
		return;

	/*
	 * Install the log's meta-data as of this record.  While this races
	 * against a checkpoint, XLOG_UNDOLOG_META always wins because it must be
	 * correct for any subsequent data appended by this transaction.  That
	 * holds even when redo started from a newer snapshot: every later change
	 * to the log is itself in the WAL replayed after this record, so the log
	 * converges on the state the snapshot recorded.  Field by field:
	 *
	 * insert, is_first_rec, last_xact_start: describe the undo insert that
	 * follows, whose redo must land exactly at 'insert' (FluxPbuRedoUndo
	 * checks).  insert also legitimately moves back (UndoLogRewind), so these
	 * are never kept from a newer state.
	 *
	 * discard: the value DO had here.  It may move back from a newer starting
	 * state, together with insert, so that the undo inserts replayed next are
	 * not taken as discarded.  The DISCARD records after this one then remove
	 * again any segment their replay recreated.
	 *
	 * end: only grows.  Keep a larger one: its segments exist, or recovery
	 * creates them on first access.
	 *
	 * persistence, tablespace: fixed when the log is created.
	 *
	 * status: only ever advances (active, exhausted, discarded) and an
	 * exhaustion is not WAL-logged, so keep a later status.
	 */
	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	end = log->meta.end;
	status = log->meta.status;
	log->meta = xlrec->meta;
	log->meta.end = Max(end, xlrec->meta.end);
	log->meta.status = Max(status, xlrec->meta.status);
	log->xid = xlrec->xid;
	log->procno = MyProcNumber;
	log->pid = MyProcPid;		/* show as recovery process */
	LWLockRelease(&log->mutex);
}

/*
 * Drop from shared buffers the pages of an undo log that are being discarded,
 * dirty or not, so that none is later written to a segment file that no
 * longer exists.
 *
 * Normally only [old_discard, new_discard) is dropped: the page holding
 * new_discard, and everything above it, may still hold live undo.  If
 * drop_tail is true the whole log is being removed (DROP TABLESPACE) and
 * every page from old_discard onward is dropped.
 */
static void
forget_undo_buffers(int logno, UndoLogOffset old_discard,
					UndoLogOffset new_discard, bool drop_tail)
{
	BlockNumber old_blockno;
	BlockNumber new_blockno;
	RelFileNode rnode;
	SMgrRelation smgr;
	ForkNumber	fork = UndoLogForkNum;

	UndoRecPtrAssignRelFileNode(rnode, MakeUndoRecPtr(logno, old_discard));

	/*
	 * Keep dirty pages in the retained partial segment, even below its
	 * discard point.  Online checksum enablement may have rewritten them;
	 * dropping them would leave stale checksums on disk.  This also applies
	 * during replay of the checksum full-page images followed by discard.
	 */
	old_blockno = (old_discard / UndoLogSegmentSize) * UNDOSEG_SIZE;
	new_blockno = (new_discard / UndoLogSegmentSize) * UNDOSEG_SIZE;

	/*
	 * A discarded page may be dirty: undo pages are never modified after they
	 * are written, but one that was dirty when discarded stays dirty.  Left
	 * in shared buffers, it would later be written by the checkpointer or by
	 * eviction -- into a segment file the caller is about to rename or
	 * unlink, failing the checkpoint.  So it must be dropped here, before any
	 * file is.
	 *
	 * For a partial discard the range needs an upper bound: the live tail at
	 * and above new_blockno may also be dirty, and dropping it would lose
	 * undo. DropRelationBuffers() drops everything from its start block
	 * onward, so it is only safe when the whole log is going away.
	 */
	smgr = smgropen(rnode, INVALID_PROC_NUMBER);
	if (drop_tail)
	{
		/*
		 * The whole log is going away (DROP TABLESPACE) and every segment
		 * file will be unlinked, so everything at or beyond old_blockno must
		 * go -- including any pre-extended pages above the insert point.
		 */
		DropRelationBuffers(smgr, &fork, 1, &old_blockno);
	}
	else if (old_blockno < new_blockno)
		DropRelationBufferRange(smgr, fork, old_blockno, new_blockno);
}

/*
 * replay an undo segment discard record
 */
static void
undolog_xlog_discard(XLogReaderState *record)
{
	xl_undolog_discard *xlrec = (xl_undolog_discard *) XLogRecGetData(record);
	UndoLogControl *log;
	UndoLogOffset discard;
	UndoLogOffset end;
	UndoLogOffset old_segment_begin;
	UndoLogOffset new_segment_begin;
	RelFileNode rnode = {0};
	char		dir[MAXPGPATH];

	/*
	 * A log below low_logno has been dropped since; see
	 * undolog_redo_get_log().
	 */
	log = undolog_redo_get_log(xlrec->logno);
	if (log == NULL)
		return;

	/*
	 * We're about to discard undologs. In Hot Standby mode, ensure that
	 * there's no queries running which need to get tuple from discarded undo.
	 *
	 * Note: we are passing empty rnode to the conflict function so that it
	 * can check conflict in all the backend regardless of which database the
	 * backend is connected.
	 */
	if (InHotStandby && TransactionIdIsValid(xlrec->latestxid))
		ResolveRecoveryConflictWithSnapshot(xlrec->latestxid, false, rnode);

	/*
	 * See if we need to unlink or rename any files, but don't consider it an
	 * error if we find that files are missing.  Since PbuUndoLogDiscard()
	 * performs filesystem operations before WAL logging or updating shmem
	 * which could be checkpointed, a crash could have left files already
	 * deleted, but we could replay WAL that expects the files to be there.
	 */

	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	discard = log->meta.discard;
	end = log->meta.end;
	LWLockRelease(&log->mutex);

	/*
	 * Both pointers only advance.  If neither would here, the starting state
	 * is newer than this record (or the record is a repeat): the segments it
	 * removes are gone already, and the ones it adds exist.
	 */
	if (xlrec->discard <= discard && xlrec->end <= end)
		return;

	/* Drop buffers before we remove/recycle any files. */
	forget_undo_buffers(xlrec->logno, discard, xlrec->discard, false);

	/* Rewind to the start of the segment. */
	old_segment_begin = discard - discard % UndoLogSegmentSize;
	new_segment_begin = xlrec->discard - xlrec->discard % UndoLogSegmentSize;

	/* Unlink or rename segments that are no longer in range. */
	while (old_segment_begin < new_segment_begin)
	{
		char		discard_path[MAXPGPATH];

		/* Tell the checkpointer that the file is going away. */
		undofile_forget_sync(log->logno, old_segment_begin / UndoLogSegmentSize,
							 log->meta.tablespace);

		UndoLogSegmentPath(xlrec->logno, old_segment_begin / UndoLogSegmentSize,
						   log->meta.tablespace, discard_path);

		/* Never rename pages whose checksums include their old block numbers. */
		if (end < xlrec->end)
		{
			if (unlink(discard_path) == 0)
			{
				allocate_empty_undo_segment(xlrec->logno, log->meta.tablespace, end);
				end += UndoLogSegmentSize;
			}
			else
			{
				elog(LOG, "could not remove \"%s\": %m",
					 discard_path);
			}
		}
		else
		{
			if (unlink(discard_path) != 0)
				elog(LOG, "could not unlink \"%s\": %m", discard_path);
		}
		old_segment_begin += UndoLogSegmentSize;
	}

	/* Create any further new segments that are needed the slow way. */
	while (end < xlrec->end)
	{
		allocate_empty_undo_segment(xlrec->logno, log->meta.tablespace, end);
		end += UndoLogSegmentSize;
	}

	/* Flush the directory entries. */
	UndoLogDirectory(log->meta.tablespace, dir);
	fsync_fname(dir, true);

	/* Update shmem.  The discard pointer never moves back here. */
	LWLockAcquire(&log->mutex, LW_EXCLUSIVE);
	log->meta.discard = Max(discard, xlrec->discard);
	log->meta.end = end;
	LWLockRelease(&log->mutex);
}

/*
 * replay the rewind of a undo log
 */
static void
undolog_xlog_rewind(XLogReaderState *record)
{
	xl_undolog_rewind *xlrec = (xl_undolog_rewind *) XLogRecGetData(record);
	UndoLogControl *log;

	log = undolog_redo_get_log(xlrec->logno);
	if (log != NULL)
		log->meta.insert = xlrec->insert;
}

void
undolog_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_UNDOLOG_CREATE:
			undolog_xlog_create(record);
			break;
		case XLOG_UNDOLOG_EXTEND:
			undolog_xlog_extend(record);
			break;
		case XLOG_UNDOLOG_ATTACH:
			undolog_xlog_attach(record);
			break;
		case XLOG_UNDOLOG_DISCARD:
			undolog_xlog_discard(record);
			break;
		case XLOG_UNDOLOG_REWIND:
			undolog_xlog_rewind(record);
			break;
		case XLOG_UNDOLOG_META:
			undolog_xlog_meta(record);
			break;
		case XLOG_UNDOLOG_SWITCH:
			undolog_xlog_switch(record);
			break;
		default:
			elog(PANIC, "undo_redo: unknown op code %u", info);
	}
}

/*
 * For assertions only.
 */
bool
AmAttachedToUndoLog(UndoLogControl *log)
{
	int			i;

	for (i = 0; i < UndoPersistenceLevels; ++i)
	{
		if (MyUndoLogState.logs[i] == log)
			return true;
	}
	return false;
}

/*
 * Fetch database id from the undo log state
 */
Oid
UndoLogStateGetDatabaseId(void)
{
	Assert(InRecovery);
	return MyUndoLogState.dbid;
}

/*
 * Get transaction start header in the previous log
 *
 * This should be only called during recovery.  The value of prevlogurp
 * is restored in MyUndoLogState while replying the UNDOLOG_XLOG_SWITCH
 * wal and it will be cleared in this function.
 */
UndoRecPtr
UndoLogStateGetAndClearPrevLogXactUrp(void)
{
	UndoRecPtr	prevlogurp;

	Assert(InRecovery);
	prevlogurp = MyUndoLogState.prevlogurp;
	MyUndoLogState.prevlogurp = InvalidUndoRecPtr;

	return prevlogurp;
}

/*
 * Get the undo log number my backend is attached to
 */
UndoLogNumber
UndoLogAmAttachedTo(UndoPersistence persistence)
{
	if (MyUndoLogState.logs[persistence] == NULL)
		return InvalidUndoLogNumber;
	return MyUndoLogState.logs[persistence]->logno;
}
