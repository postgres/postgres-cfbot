/*-------------------------------------------------------------------------
 *
 * pgarch.c
 *
 *	PostgreSQL WAL archiver
 *
 *	All functions relating to archiver are included here
 *
 *	- All functions executed by archiver process
 *
 *	- archiver is forked from postmaster, and the two
 *	processes then communicate using signals. All functions
 *	executed by postmaster are included in this file.
 *
 *	Initial author: Simon Riggs		simon@2ndquadrant.com
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pgarch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "lib/binaryheap.h"
#include "libpq/pqsignal.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "postmaster/pgarch.h"
#include "storage/condition_variable.h"
#include "storage/aio_subsys.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timeout.h"
#include "utils/wait_event.h"


/* ----------
 * Timer definitions.
 * ----------
 */
#define PGARCH_AUTOWAKE_INTERVAL 60 /* How often to force a poll of the
									 * archive status directory; in seconds. */
#define PGARCH_RESTART_INTERVAL 10	/* How often to attempt to restart a
									 * failed archiver; in seconds. */
#define PGARCH_ASYNC_POLL_MS	1000L
#define PGARCH_ASYNC_STOP_GRACE_S	60

/*
 * Maximum number of retries allowed when attempting to archive a WAL
 * file.
 */
#define NUM_ARCHIVE_RETRIES 3

/*
 * Maximum number of retries allowed when attempting to remove an
 * orphan archive status file.
 */
#define NUM_ORPHAN_CLEANUP_RETRIES 3

/*
 * Maximum number of .ready files to gather per directory scan.
 */
#define NUM_FILES_PER_DIRECTORY_SCAN 64

/* Shared memory area for archiver process */
typedef struct PgArchData
{
	int			pgprocno;		/* proc number of archiver process */

	/*
	 * Forces a directory scan in pgarch_readyXlog().
	 */
	pg_atomic_uint32 force_dir_scan;
} PgArchData;

char	   *XLogArchiveLibrary = "";
char	   *arch_module_check_errdetail_string;


/* ----------
 * Local data
 * ----------
 */
static time_t last_sigterm_time = 0;
static PgArchData *PgArch = NULL;
static const ArchiveModuleCallbacks *ArchiveCallbacks;
static ArchiveModuleState *archive_module_state;
static MemoryContext archive_context;


/*
 * Stuff for tracking multiple files to archive from each scan of
 * archive_status.  Minimizing the number of directory scans when there are
 * many files to archive can significantly improve archival rate.
 *
 * arch_heap is a max-heap that is used during the directory scan to track
 * the highest-priority files to archive.  After the directory scan
 * completes, the file names are stored in ascending order of priority in
 * arch_files.  pgarch_readyXlog() returns files from arch_files until it
 * is empty, at which point another directory scan must be performed.
 *
 * We only need this data in the archiver process, so make it a palloc'd
 * struct rather than a bunch of static arrays.
 */
struct arch_files_state
{
	binaryheap *arch_heap;
	int			arch_files_size;	/* number of live entries in arch_files[] */
	char	   *arch_files[NUM_FILES_PER_DIRECTORY_SCAN];
	/* buffers underlying heap, and later arch_files[], entries: */
	char		arch_filenames[NUM_FILES_PER_DIRECTORY_SCAN][MAX_XFN_CHARS + 1];
};

static struct arch_files_state *arch_files = NULL;

typedef struct ArchInFlight
{
	char		xlog[MAX_XFN_CHARS + 1];	/* empty = free */
	int			failures;
	bool		waiting;
	time_t		not_before;
} ArchInFlight;

static ArchInFlight arch_inflight[NUM_FILES_PER_DIRECTORY_SCAN];
static int	arch_inflight_count = 0;
static bool archiver_use_async = false;
static int	archiver_shell_jobs = 1;

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t ready_to_stop = false;

/* ----------
 * Local function forward declarations
 * ----------
 */
static void pgarch_waken_stop(SIGNAL_ARGS);
static void pgarch_MainLoop(void);
static void pgarch_ArchiverCopyLoop(void);
static void pgarch_ArchiverAsyncLoop(void);
static bool pgarch_archiveXlog(char *xlog);
static bool pgarch_readyXlog(char *xlog);
static void pgarch_archiveDone(char *xlog);
static void pgarch_die(int code, Datum arg);
static void ProcessPgArchInterrupts(void);
static int	ready_file_comparator(Datum a, Datum b, void *arg);
static void LoadArchiveLibrary(void);
static void pgarch_call_module_shutdown_cb(int code, Datum arg);
static void pgarch_module_error_cleanup(MemoryContext oldcontext);
static bool pgarch_archive_configured(void);
static int	pgarch_wal_file_present(const char *xlog);
static ArchInFlight *pgarch_inflight_find(const char *xlog);
static ArchInFlight *pgarch_inflight_acquire(const char *xlog);
static bool pgarch_inflight_blocked(const ArchInFlight *slot);
static void pgarch_inflight_clear(void);
static void pgarch_abort_async_inflight(void);
static void pgarch_set_activity(const char *fmt, const char *xlog);
static long pgarch_async_retry_sleep(bool have_deferred);
static bool pgarch_async_finish(ArchInFlight *slot, ArchiveResultStatus status);
static int	pgarch_submitXlog(char *xlog);
static int	pgarch_pollXlog(char *file, ArchiveResultStatus *status,
							 long timeout_ms);

static void PgArchShmemRequest(void *arg);
static void PgArchShmemInit(void *arg);

const ShmemCallbacks PgArchShmemCallbacks = {
	.request_fn = PgArchShmemRequest,
	.init_fn = PgArchShmemInit,
};

/* Register shared memory space needed by the archiver */
static void
PgArchShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "Archiver Data",
					   .size = sizeof(PgArchData),
					   .ptr = (void **) &PgArch,
		);
}

/* Initialize archiver-related shared memory */
static void
PgArchShmemInit(void *arg)
{
	MemSet(PgArch, 0, sizeof(PgArchData));
	PgArch->pgprocno = INVALID_PROC_NUMBER;
	pg_atomic_init_u32(&PgArch->force_dir_scan, 0);
}

/*
 * PgArchCanRestart
 *
 * Return true, indicating archiver is allowed to restart, if enough time has
 * passed since it was last launched to reach PGARCH_RESTART_INTERVAL.
 * Otherwise return false.
 *
 * This is a safety valve to protect against continuous respawn attempts if the
 * archiver is dying immediately at launch. Note that since we will retry to
 * launch the archiver from the postmaster main loop, we will get another
 * chance later.
 */
bool
PgArchCanRestart(void)
{
	static time_t last_pgarch_start_time = 0;
	time_t		curtime = time(NULL);

	/*
	 * If first time through, or time somehow went backwards, always update
	 * last_pgarch_start_time to match the current clock and allow archiver
	 * start.  Otherwise allow it only once enough time has elapsed.
	 */
	if (last_pgarch_start_time == 0 ||
		curtime < last_pgarch_start_time ||
		curtime - last_pgarch_start_time >= PGARCH_RESTART_INTERVAL)
	{
		last_pgarch_start_time = curtime;
		return true;
	}
	return false;
}


/* Main entry point for archiver process */
void
PgArchiverMain(const void *startup_data, size_t startup_data_len)
{
	Assert(startup_data_len == 0);

	AuxiliaryProcessMainCommon();

	/*
	 * Ignore all signals usually bound to some action in the postmaster,
	 * except for SIGHUP, SIGTERM, SIGUSR1, SIGUSR2, and SIGQUIT.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, PG_SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, PG_SIG_IGN);
	pqsignal(SIGPIPE, PG_SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, pgarch_waken_stop);

	/* Unblock signals (they were blocked when the postmaster forked us) */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/* We shouldn't be launched unnecessarily. */
	Assert(XLogArchivingActive());

	/* Arrange to clean up at archiver exit */
	on_shmem_exit(pgarch_die, 0);

	/*
	 * Advertise our proc number so that backends can use our latch to wake us
	 * up while we're sleeping.
	 */
	PgArch->pgprocno = MyProcNumber;

	/* Create workspace for pgarch_readyXlog() */
	arch_files = palloc_object(struct arch_files_state);
	arch_files->arch_files_size = 0;

	/* Initialize our max-heap for prioritizing files to archive. */
	arch_files->arch_heap = binaryheap_allocate(NUM_FILES_PER_DIRECTORY_SCAN,
												ready_file_comparator, NULL);

	/* Initialize our memory context. */
	archive_context = AllocSetContextCreate(TopMemoryContext,
											"archiver",
											ALLOCSET_DEFAULT_SIZES);

	/* Load the archive_library. */
	LoadArchiveLibrary();

	pgarch_MainLoop();

	proc_exit(0);
}

/*
 * Wake up the archiver
 */
void
PgArchWakeup(void)
{
	int			arch_pgprocno = PgArch->pgprocno;

	/*
	 * We don't acquire ProcArrayLock here.  It's actually fine because
	 * procLatch isn't ever freed, so we just can potentially set the wrong
	 * process' (or no process') latch.  Even in that case the archiver will
	 * be relaunched shortly and will start archiving.
	 */
	if (arch_pgprocno != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(arch_pgprocno)->procLatch);
}


/* SIGUSR2 signal handler for archiver process */
static void
pgarch_waken_stop(SIGNAL_ARGS)
{
	/* set flag to do a final cycle and shut down afterwards */
	ready_to_stop = true;
	SetLatch(MyLatch);
}

/*
 * pgarch_MainLoop
 *
 * Main loop for archiver
 */
static void
pgarch_MainLoop(void)
{
	bool		time_to_stop;

	/*
	 * There shouldn't be anything for the archiver to do except to wait for a
	 * signal ... however, the archiver exists to protect our data, so it
	 * wakes up occasionally to allow itself to be proactive.
	 */
	do
	{
		ResetLatch(MyLatch);

		/* When we get SIGUSR2, we do one more archive cycle, then exit */
		time_to_stop = ready_to_stop;

		/* Check for barrier events and config update */
		ProcessPgArchInterrupts();

		/*
		 * If we've gotten SIGTERM, we normally just sit and do nothing until
		 * SIGUSR2 arrives.  However, that means a random SIGTERM would
		 * disable archiving indefinitely, which doesn't seem like a good
		 * idea.  If more than 60 seconds pass since SIGTERM, exit anyway, so
		 * that the postmaster can start a new archiver if needed.  Also exit
		 * if time unexpectedly goes backward.
		 */
		if (ShutdownRequestPending)
		{
			time_t		curtime = time(NULL);

			if (last_sigterm_time == 0)
				last_sigterm_time = curtime;
			else if (curtime < last_sigterm_time ||
					 curtime - last_sigterm_time >= 60)
				break;
		}

		/* Do what we're here for */
		pgarch_ArchiverCopyLoop();

		time_to_stop = ready_to_stop;

		/*
		 * Sleep until a signal is received, or until a poll is forced by
		 * PGARCH_AUTOWAKE_INTERVAL, or until postmaster dies.
		 */
		if (!time_to_stop)		/* Don't wait during last iteration */
		{
			int			rc;

			rc = WaitLatch(MyLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   PGARCH_AUTOWAKE_INTERVAL * 1000L,
						   WAIT_EVENT_ARCHIVER_MAIN);
			if (rc & WL_POSTMASTER_DEATH)
				time_to_stop = true;
		}

		/*
		 * The archiver quits either when the postmaster dies (not expected)
		 * or after completing one more archiving cycle after receiving
		 * SIGUSR2.
		 */
	} while (!time_to_stop);
}

/*
 * pgarch_ArchiverCopyLoop
 *
 * Archives all outstanding xlogs then returns
 */
static void
pgarch_ArchiverCopyLoop(void)
{
	char		xlog[MAX_XFN_CHARS + 1];

	if (archiver_use_async)
	{
		pgarch_ArchiverAsyncLoop();
		return;
	}

	/* force directory scan in the first call to pgarch_readyXlog() */
	arch_files->arch_files_size = 0;

	/*
	 * loop through all xlogs with archive_status of .ready and archive
	 * them...mostly we expect this to be a single file, though it is possible
	 * some backend will add files onto the list of those that need archiving
	 * while we are still copying earlier archives
	 */
	while (pgarch_readyXlog(xlog))
	{
		int			failures = 0;

		for (;;)
		{
			int			wal_present;

			/*
			 * Do not initiate any more archive commands after receiving
			 * SIGTERM, nor after the postmaster has died unexpectedly. The
			 * first condition is to try to keep from having init SIGKILL the
			 * command, and the second is to avoid conflicts with another
			 * archiver spawned by a newer postmaster.
			 */
			if (ShutdownRequestPending || !PostmasterIsAlive())
				return;

			/*
			 * Check for barrier events and config update.  This is so that
			 * we'll adopt a new setting for archive_command as soon as
			 * possible, even if there is a backlog of files to be archived.
			 */
			ProcessPgArchInterrupts();

			if (!pgarch_archive_configured())
				return;

			wal_present = pgarch_wal_file_present(xlog);
			if (wal_present < 0)
				return;
			if (wal_present == 0)
				break;

			if (pgarch_archiveXlog(xlog))
			{
				/* successful */
				pgarch_archiveDone(xlog);

				/*
				 * Tell the cumulative stats system about the WAL file that we
				 * successfully archived
				 */
				pgstat_report_archiver(xlog, false);

				break;			/* out of inner retry loop */
			}
			else
			{
				/*
				 * Tell the cumulative stats system about the WAL file that we
				 * failed to archive
				 */
				pgstat_report_archiver(xlog, true);

				if (++failures >= NUM_ARCHIVE_RETRIES)
				{
					ereport(WARNING,
							(errmsg("archiving write-ahead log file \"%s\" failed too many times, will try again later",
									xlog)));
					return;		/* give up archiving for now */
				}
				pg_usleep(1000000L);	/* wait a bit before retrying */
			}
		}
	}
}

static void
pgarch_module_error_cleanup(MemoryContext oldcontext)
{
	error_context_stack = NULL;

	HOLD_INTERRUPTS();

	EmitErrorReport();

	disable_all_timeouts(false);
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
	pgaio_error_cleanup();
	ReleaseAuxProcessResources(false);
	AtEOXact_Files(false);
	AtEOXact_HashTables(false);

	MemoryContextSwitchTo(oldcontext);
	FlushErrorState();
	MemoryContextReset(archive_context);
	PG_exception_stack = NULL;

	RESUME_INTERRUPTS();
}

static bool
pgarch_archive_configured(void)
{
	arch_module_check_errdetail_string = NULL;
	if (ArchiveCallbacks->check_configured_cb != NULL &&
		!ArchiveCallbacks->check_configured_cb(archive_module_state))
	{
		ereport(WARNING,
				(errmsg("\"archive_mode\" enabled, yet archiving is not configured"),
				 arch_module_check_errdetail_string ?
				 errdetail_internal("%s", arch_module_check_errdetail_string) : 0));
		return false;
	}
	return true;
}

/*
 * Return 1 if present, 0 if an orphan .ready was removed, -1 after too
 * many cleanup failures.
 *
 * Since archive status files are not removed in a durable manner, a system
 * crash could leave behind .ready files for WAL segments that have already
 * been recycled or removed.  In this case, simply remove the orphan status
 * file and move on.  unlink() is used here as even on subsequent crashes
 * the same orphan files would get removed, so there is no need to worry
 * about durability.
 */
static int
pgarch_wal_file_present(const char *xlog)
{
	struct stat stat_buf;
	char		pathname[MAXPGPATH];
	char		xlogready[MAXPGPATH];
	int			failures_orphan = 0;

	snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);
	if (stat(pathname, &stat_buf) == 0 || errno != ENOENT)
		return 1;

	StatusFilePath(xlogready, xlog, ".ready");
	for (;;)
	{
		if (unlink(xlogready) == 0)
		{
			ereport(WARNING,
					(errmsg("removed orphan archive status file \"%s\"",
							xlogready)));
			return 0;
		}

		if (++failures_orphan >= NUM_ORPHAN_CLEANUP_RETRIES)
		{
			ereport(WARNING,
					(errmsg("removal of orphan archive status file \"%s\" failed too many times, will try again later",
							xlogready)));
			return -1;
		}

		pg_usleep(1000000L);
	}
}

static ArchInFlight *
pgarch_inflight_find(const char *xlog)
{
	for (int i = 0; i < NUM_FILES_PER_DIRECTORY_SCAN; i++)
	{
		if (arch_inflight[i].xlog[0] != '\0' &&
			strcmp(arch_inflight[i].xlog, xlog) == 0)
			return &arch_inflight[i];
	}
	return NULL;
}

static ArchInFlight *
pgarch_inflight_acquire(const char *xlog)
{
	ArchInFlight *slot = pgarch_inflight_find(xlog);

	if (slot != NULL)
		return slot;

	for (int i = 0; i < NUM_FILES_PER_DIRECTORY_SCAN; i++)
	{
		if (arch_inflight[i].xlog[0] == '\0')
		{
			slot = &arch_inflight[i];
			strlcpy(slot->xlog, xlog, sizeof(slot->xlog));
			slot->failures = 0;
			slot->waiting = false;
			slot->not_before = 0;
			return slot;
		}
	}
	return NULL;
}

static bool
pgarch_inflight_blocked(const ArchInFlight *slot)
{
	return slot->waiting ||
		slot->failures >= NUM_ARCHIVE_RETRIES ||
		(slot->not_before != 0 && time(NULL) < slot->not_before);
}

static void
pgarch_inflight_clear(void)
{
	MemSet(arch_inflight, 0, sizeof(arch_inflight));
	arch_inflight_count = 0;
}

static void
pgarch_abort_async_inflight(void)
{
	if (ArchiveCallbacks->cancel_cb != NULL)
		ArchiveCallbacks->cancel_cb(archive_module_state);
	pgarch_inflight_clear();
}

static void
pgarch_abort_async_inflight_msg(const char *msg)
{
	if (arch_inflight_count > 0)
		ereport(LOG, (errmsg("%s", msg)));
	pgarch_abort_async_inflight();
}

static void
pgarch_set_activity(const char *fmt, const char *xlog)
{
	char		activitymsg[MAXFNAMELEN + 16];

	snprintf(activitymsg, sizeof(activitymsg), fmt, xlog);
	set_ps_display(activitymsg);
}

static long
pgarch_async_retry_sleep(bool have_deferred)
{
	time_t		now = time(NULL);
	time_t		wait_until = 0;
	bool		pending = have_deferred;
	bool		have_wait = false;

	for (int i = 0; i < NUM_FILES_PER_DIRECTORY_SCAN; i++)
	{
		if (arch_inflight[i].xlog[0] == '\0' ||
			arch_inflight[i].waiting ||
			arch_inflight[i].failures >= NUM_ARCHIVE_RETRIES)
			continue;
		pending = true;
		if (arch_inflight[i].not_before == 0 ||
			arch_inflight[i].not_before <= now)
			return PGARCH_ASYNC_POLL_MS;
		if (!have_wait || arch_inflight[i].not_before < wait_until)
		{
			wait_until = arch_inflight[i].not_before;
			have_wait = true;
		}
	}

	if (!pending)
		return -1;
	if (have_wait && wait_until > now)
		return (wait_until - now) * 1000L;
	return PGARCH_ASYNC_POLL_MS;
}

static bool
pgarch_async_finish(ArchInFlight *slot, ArchiveResultStatus status)
{
	if (slot->waiting)
	{
		arch_inflight_count--;
		slot->waiting = false;
	}

	if (status == ARCHIVE_RESULT_SUCCESS)
	{
		pgarch_archiveDone(slot->xlog);
		pgstat_report_archiver(slot->xlog, false);
		pgarch_set_activity("last was %s", slot->xlog);
		MemSet(slot, 0, sizeof(*slot));
		return false;
	}

	pgstat_report_archiver(slot->xlog, true);
	pgarch_set_activity("failed on %s", slot->xlog);

	if (status == ARCHIVE_RESULT_FAIL)
		slot->failures = NUM_ARCHIVE_RETRIES;
	else
	{
		slot->failures++;
		slot->not_before = time(NULL) + 1;
	}

	if (slot->failures < NUM_ARCHIVE_RETRIES)
		return false;

	ereport(WARNING,
			(errmsg("archiving write-ahead log file \"%s\" failed too many times, will try again later",
					slot->xlog)));
	return true;
}

static int
pgarch_submitXlog(char *xlog)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext oldcontext;
	char		pathname[MAXPGPATH];
	int			result;

	snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);
	oldcontext = MemoryContextSwitchTo(archive_context);

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		pgarch_module_error_cleanup(oldcontext);
		result = -1;
	}
	else
	{
		bool		ok;

		PG_exception_stack = &local_sigjmp_buf;
		ok = ArchiveCallbacks->submit_cb(archive_module_state, xlog, pathname);
		PG_exception_stack = NULL;
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(archive_context);
		result = ok ? 1 : 0;
	}

	return result;
}

static int
pgarch_pollXlog(char *file, ArchiveResultStatus *status, long timeout_ms)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext oldcontext;
	const char *modulefile = NULL;
	int			result;

	oldcontext = MemoryContextSwitchTo(archive_context);

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		pgarch_module_error_cleanup(oldcontext);
		result = -1;
	}
	else
	{
		bool		got;

		PG_exception_stack = &local_sigjmp_buf;
		got = ArchiveCallbacks->poll_cb(archive_module_state, &modulefile,
										status, timeout_ms);
		if (got)
		{
			if (modulefile == NULL || modulefile[0] == '\0')
				file[0] = '\0';
			else
				strlcpy(file, modulefile, MAX_XFN_CHARS + 1);
			result = 1;
		}
		else
			result = 0;
		PG_exception_stack = NULL;
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(archive_context);
	}

	return result;
}

static void
pgarch_ArchiverAsyncLoop(void)
{
	bool		give_up = false;
	char		deferred[MAX_XFN_CHARS + 1];
	time_t		stop_deadline = 0;

	arch_files->arch_files_size = 0;
	pgarch_inflight_clear();
	deferred[0] = '\0';

	for (;;)
	{
		int			skips = 0;
		char		xlog[MAX_XFN_CHARS + 1];
		char		done_file[MAX_XFN_CHARS + 1];
		ArchiveResultStatus status;
		int			cbres;
		ArchInFlight *slot;

		if (ShutdownRequestPending || !PostmasterIsAlive())
		{
			pgarch_abort_async_inflight_msg("canceling in flight archive commands due to shutdown");
			return;
		}

		ProcessPgArchInterrupts();
		if (!pgarch_archive_configured())
		{
			pgarch_abort_async_inflight_msg("canceling in flight archive commands because archiving is no longer configured");
			return;
		}

		if (ready_to_stop)
		{
			time_t		now = time(NULL);

			if (stop_deadline == 0)
				stop_deadline = now + PGARCH_ASYNC_STOP_GRACE_S;
			else if (now >= stop_deadline ||
					 now < stop_deadline - PGARCH_ASYNC_STOP_GRACE_S)
			{
				pgarch_abort_async_inflight_msg("canceling in flight archive commands due to shutdown");
				return;
			}
		}

		while (!give_up &&
			   arch_inflight_count < NUM_FILES_PER_DIRECTORY_SCAN &&
			   skips < NUM_FILES_PER_DIRECTORY_SCAN)
		{
			int			wal_present;

			if (deferred[0] != '\0')
			{
				strlcpy(xlog, deferred, sizeof(xlog));
				deferred[0] = '\0';
			}
			else if (!pgarch_readyXlog(xlog))
				break;

			slot = pgarch_inflight_find(xlog);
			if (slot != NULL && pgarch_inflight_blocked(slot))
			{
				skips++;
				continue;
			}

			wal_present = pgarch_wal_file_present(xlog);
			if (wal_present < 0)
			{
				give_up = true;
				break;
			}
			if (wal_present == 0)
			{
				if (slot != NULL)
					MemSet(slot, 0, sizeof(*slot));
				continue;
			}

			slot = pgarch_inflight_acquire(xlog);
			if (slot == NULL)
			{
				if (arch_inflight_count == 0)
					give_up = true;
				else
					strlcpy(deferred, xlog, sizeof(deferred));
				break;
			}

			skips = 0;
			pgarch_set_activity("submitting %s", xlog);

			cbres = pgarch_submitXlog(xlog);
			if (cbres < 0)
			{
				if (pgarch_async_finish(slot, ARCHIVE_RESULT_RETRY))
					give_up = true;
				continue;
			}
			if (cbres == 0)
			{
				strlcpy(deferred, xlog, sizeof(deferred));
				break;
			}

			slot->waiting = true;
			arch_inflight_count++;
		}

		if (arch_inflight_count == 0)
		{
			long		sleep_ms;

			if (give_up || ShutdownRequestPending)
				return;

			sleep_ms = pgarch_async_retry_sleep(deferred[0] != '\0');
			if (sleep_ms < 0)
				return;
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 sleep_ms,
							 WAIT_EVENT_ARCHIVER_MAIN);
			ResetLatch(MyLatch);
			continue;
		}

		cbres = pgarch_pollXlog(done_file, &status, PGARCH_ASYNC_POLL_MS);
		if (cbres < 0)
		{
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 PGARCH_ASYNC_POLL_MS,
							 WAIT_EVENT_ARCHIVER_MAIN);
			ResetLatch(MyLatch);
			continue;
		}
		if (cbres == 0)
			continue;

		slot = (done_file[0] != '\0') ? pgarch_inflight_find(done_file) : NULL;
		if (slot == NULL || !slot->waiting)
		{
			ereport(WARNING,
					(errmsg("asynchronous archive module returned unexpected file \"%s\"",
							done_file)));
			pgarch_abort_async_inflight();
			return;
		}

		if (pgarch_async_finish(slot, status))
			give_up = true;
		if (give_up && arch_inflight_count == 0)
			return;
	}
}

/*
 * pgarch_archiveXlog
 *
 * Invokes archive_file_cb to copy one archive file to wherever it should go
 *
 * Returns true if successful
 */
static bool
pgarch_archiveXlog(char *xlog)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext oldcontext;
	char		pathname[MAXPGPATH];
	bool		ret;

	snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);
	pgarch_set_activity("archiving %s", xlog);
	oldcontext = MemoryContextSwitchTo(archive_context);

	/*
	 * Since the archiver operates at the bottom of the exception stack,
	 * ERRORs turn into FATALs and cause the archiver process to restart.
	 * However, using ereport(ERROR, ...) when there are problems is easy to
	 * code and maintain.  Therefore, we create our own exception handler to
	 * catch ERRORs and return false instead of restarting the archiver
	 * whenever there is a failure.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		pgarch_module_error_cleanup(oldcontext);
		ret = false;
	}
	else
	{
		PG_exception_stack = &local_sigjmp_buf;
		ret = ArchiveCallbacks->archive_file_cb(archive_module_state,
												xlog, pathname);
		PG_exception_stack = NULL;
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(archive_context);
	}

	pgarch_set_activity(ret ? "last was %s" : "failed on %s", xlog);
	return ret;
}

/*
 * pgarch_readyXlog
 *
 * Return name of the oldest xlog file that has not yet been archived.
 * Concurrent callers must skip in-flight names themselves. If a failure
 * occurs, we will completely re-copy the file at the next available
 * opportunity.
 *
 * It is important that we return the oldest, so that we archive xlogs
 * in order that they were written, for two reasons:
 * 1) to maintain the sequential chain of xlogs required for recovery
 * 2) because the oldest ones will sooner become candidates for
 * recycling at time of checkpoint
 *
 * NOTE: the "oldest" comparison will consider any .history file to be older
 * than any other file except another .history file.  Segments on a timeline
 * with a smaller ID will be older than all segments on a timeline with a
 * larger ID; the net result being that past timelines are given higher
 * priority for archiving.  This seems okay, or at least not obviously worth
 * changing.
 */
static bool
pgarch_readyXlog(char *xlog)
{
	char		XLogArchiveStatusDir[MAXPGPATH];
	DIR		   *rldir;
	struct dirent *rlde;

	/*
	 * If a directory scan was requested, clear the stored file names and
	 * proceed.
	 */
	if (pg_atomic_exchange_u32(&PgArch->force_dir_scan, 0) == 1)
		arch_files->arch_files_size = 0;

	/*
	 * If we still have stored file names from the previous directory scan,
	 * try to return one of those.  We check to make sure the status file is
	 * still present, as the archive_command for a previous file may have
	 * already marked it done.
	 */
	while (arch_files->arch_files_size > 0)
	{
		struct stat st;
		char		status_file[MAXPGPATH];
		char	   *arch_file;

		arch_files->arch_files_size--;
		arch_file = arch_files->arch_files[arch_files->arch_files_size];
		StatusFilePath(status_file, arch_file, ".ready");

		if (stat(status_file, &st) == 0)
		{
			strcpy(xlog, arch_file);
			return true;
		}
		else if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", status_file)));
	}

	/* arch_heap is probably empty, but let's make sure */
	binaryheap_reset(arch_files->arch_heap);

	/*
	 * Open the archive status directory and read through the list of files
	 * with the .ready suffix, looking for the earliest files.
	 */
	snprintf(XLogArchiveStatusDir, MAXPGPATH, XLOGDIR "/archive_status");
	rldir = AllocateDir(XLogArchiveStatusDir);

	while ((rlde = ReadDir(rldir, XLogArchiveStatusDir)) != NULL)
	{
		int			basenamelen = (int) strlen(rlde->d_name) - 6;
		char		basename[MAX_XFN_CHARS + 1];
		char	   *arch_file;

		/* Ignore entries with unexpected number of characters */
		if (basenamelen < MIN_XFN_CHARS ||
			basenamelen > MAX_XFN_CHARS)
			continue;

		/* Ignore entries with unexpected characters */
		if (strspn(rlde->d_name, VALID_XFN_CHARS) < basenamelen)
			continue;

		/* Ignore anything not suffixed with .ready */
		if (strcmp(rlde->d_name + basenamelen, ".ready") != 0)
			continue;

		/* Truncate off the .ready */
		memcpy(basename, rlde->d_name, basenamelen);
		basename[basenamelen] = '\0';

		/*
		 * Store the file in our max-heap if it has a high enough priority.
		 */
		if (binaryheap_size(arch_files->arch_heap) < NUM_FILES_PER_DIRECTORY_SCAN)
		{
			/* If the heap isn't full yet, quickly add it. */
			arch_file = arch_files->arch_filenames[binaryheap_size(arch_files->arch_heap)];
			strcpy(arch_file, basename);
			binaryheap_add_unordered(arch_files->arch_heap, CStringGetDatum(arch_file));

			/* If we just filled the heap, make it a valid one. */
			if (binaryheap_size(arch_files->arch_heap) == NUM_FILES_PER_DIRECTORY_SCAN)
				binaryheap_build(arch_files->arch_heap);
		}
		else if (ready_file_comparator(binaryheap_first(arch_files->arch_heap),
									   CStringGetDatum(basename), NULL) > 0)
		{
			/*
			 * Remove the lowest priority file and add the current one to the
			 * heap.
			 */
			arch_file = DatumGetCString(binaryheap_remove_first(arch_files->arch_heap));
			strcpy(arch_file, basename);
			binaryheap_add(arch_files->arch_heap, CStringGetDatum(arch_file));
		}
	}
	FreeDir(rldir);

	/* If no files were found, simply return. */
	if (binaryheap_empty(arch_files->arch_heap))
		return false;

	/*
	 * If we didn't fill the heap, we didn't make it a valid one.  Do that
	 * now.
	 */
	if (binaryheap_size(arch_files->arch_heap) < NUM_FILES_PER_DIRECTORY_SCAN)
		binaryheap_build(arch_files->arch_heap);

	/*
	 * Fill arch_files array with the files to archive in ascending order of
	 * priority.
	 */
	arch_files->arch_files_size = binaryheap_size(arch_files->arch_heap);
	for (int i = 0; i < arch_files->arch_files_size; i++)
		arch_files->arch_files[i] = DatumGetCString(binaryheap_remove_first(arch_files->arch_heap));

	/* Return the highest priority file. */
	arch_files->arch_files_size--;
	strcpy(xlog, arch_files->arch_files[arch_files->arch_files_size]);

	return true;
}

/*
 * ready_file_comparator
 *
 * Compares the archival priority of the given files to archive.  If "a"
 * has a higher priority than "b", a negative value will be returned.  If
 * "b" has a higher priority than "a", a positive value will be returned.
 * If "a" and "b" have equivalent values, 0 will be returned.
 */
static int
ready_file_comparator(Datum a, Datum b, void *arg)
{
	char	   *a_str = DatumGetCString(a);
	char	   *b_str = DatumGetCString(b);
	bool		a_history = IsTLHistoryFileName(a_str);
	bool		b_history = IsTLHistoryFileName(b_str);

	/* Timeline history files always have the highest priority. */
	if (a_history != b_history)
		return a_history ? -1 : 1;

	/* Priority is given to older files. */
	return strcmp(a_str, b_str);
}

/*
 * PgArchForceDirScan
 *
 * When called, the next call to pgarch_readyXlog() will perform a
 * directory scan.  This is useful for ensuring that important files such
 * as timeline history files are archived as quickly as possible.
 */
void
PgArchForceDirScan(void)
{
	pg_atomic_write_membarrier_u32(&PgArch->force_dir_scan, 1);
}

/*
 * pgarch_archiveDone
 *
 * Emit notification that an xlog file has been successfully archived.
 * We do this by renaming the status file from NNN.ready to NNN.done.
 * Eventually, a checkpoint process will notice this and delete both the
 * NNN.done file and the xlog file itself.
 */
static void
pgarch_archiveDone(char *xlog)
{
	char		rlogready[MAXPGPATH];
	char		rlogdone[MAXPGPATH];

	StatusFilePath(rlogready, xlog, ".ready");
	StatusFilePath(rlogdone, xlog, ".done");

	/*
	 * To avoid extra overhead, we don't durably rename the .ready file to
	 * .done.  Archive commands and libraries must gracefully handle attempts
	 * to re-archive files (e.g., if the server crashes just before this
	 * function is called), so it should be okay if the .ready file reappears
	 * after a crash.
	 */
	if (rename(rlogready, rlogdone) < 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						rlogready, rlogdone)));
}


/*
 * pgarch_die
 *
 * Exit-time cleanup handler
 */
static void
pgarch_die(int code, Datum arg)
{
	PgArch->pgprocno = INVALID_PROC_NUMBER;
}

/*
 * Interrupt handler for WAL archiver process.
 *
 * This is called in the loops pgarch_MainLoop and pgarch_ArchiverCopyLoop.
 * It checks for barrier events, config update and request for logging of
 * memory contexts, but not shutdown request because how to handle
 * shutdown request is different between those loops.
 */
static void
ProcessPgArchInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* Perform logging of memory contexts of this process */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	if (ConfigReloadPending)
	{
		char	   *archiveLib = pstrdup(XLogArchiveLibrary);
		bool		archiveLibChanged;

		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		if (XLogArchiveLibrary[0] != '\0' && XLogArchiveCommand[0] != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("both \"archive_command\" and \"archive_library\" set"),
					 errdetail("Only one of \"archive_command\", \"archive_library\" may be set.")));

		archiveLibChanged = strcmp(XLogArchiveLibrary, archiveLib) != 0;
		pfree(archiveLib);

		if (archiveLibChanged)
		{
			/*
			 * Ideally, we would simply unload the previous archive module and
			 * load the new one, but there is presently no mechanism for
			 * unloading a library (see the comment above
			 * internal_load_library()).  To deal with this, we simply restart
			 * the archiver.  The new archive module will be loaded when the
			 * new archiver process starts up.  Note that this triggers the
			 * module's shutdown callback, if defined.
			 */
			ereport(LOG,
					(errmsg("restarting archiver process because value of "
							"\"archive_library\" was changed")));

			proc_exit(0);
		}

		if (XLogArchiveLibrary[0] == '\0' &&
			XLogArchiveCommandJobs != archiver_shell_jobs)
		{
			ereport(LOG,
					(errmsg("restarting archiver process because value of "
							"\"archive_command_jobs\" was changed")));
			proc_exit(0);
		}
	}
}

/*
 * LoadArchiveLibrary
 *
 * Loads the archiving callbacks into our local ArchiveCallbacks.
 */
static void
LoadArchiveLibrary(void)
{
	ArchiveModuleInit archive_init;

	if (XLogArchiveLibrary[0] != '\0' && XLogArchiveCommand[0] != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("both \"archive_command\" and \"archive_library\" set"),
				 errdetail("Only one of \"archive_command\", \"archive_library\" may be set.")));

	/*
	 * If shell archiving is enabled, use our special initialization function.
	 * Otherwise, load the library and call its _PG_archive_module_init().
	 */
	if (XLogArchiveLibrary[0] == '\0')
		archive_init = shell_archive_init;
	else
		archive_init = (ArchiveModuleInit)
			load_external_function(XLogArchiveLibrary,
								   "_PG_archive_module_init", false, NULL);

	if (archive_init == NULL)
		ereport(ERROR,
				(errmsg("archive modules have to define the symbol %s", "_PG_archive_module_init")));

	ArchiveCallbacks = (*archive_init) ();

	if ((ArchiveCallbacks->submit_cb == NULL) !=
		(ArchiveCallbacks->poll_cb == NULL))
		ereport(ERROR,
				(errmsg("archive modules must provide both submit and poll callbacks for asynchronous archiving")));

	archiver_use_async = (ArchiveCallbacks->submit_cb != NULL);
	archiver_shell_jobs = XLogArchiveCommandJobs;
	if (archiver_use_async)
	{
		if (ArchiveCallbacks->archive_file_cb != NULL)
			ereport(ERROR,
					(errmsg("archive modules must not register both synchronous and asynchronous archive callbacks")));
		if (ArchiveCallbacks->cancel_cb == NULL)
			ereport(ERROR,
					(errmsg("asynchronous archive modules must provide a cancel callback")));
	}
	else if (ArchiveCallbacks->archive_file_cb == NULL)
		ereport(ERROR,
				(errmsg("archive modules must register an archive callback")));

	archive_module_state = palloc0_object(ArchiveModuleState);
	if (ArchiveCallbacks->startup_cb != NULL)
		ArchiveCallbacks->startup_cb(archive_module_state);

	before_shmem_exit(pgarch_call_module_shutdown_cb, 0);
}

/*
 * Call the shutdown callback of the loaded archive module, if defined.
 */
static void
pgarch_call_module_shutdown_cb(int code, Datum arg)
{
	if (ArchiveCallbacks->shutdown_cb != NULL)
		ArchiveCallbacks->shutdown_cb(archive_module_state);
}
