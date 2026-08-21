/*-------------------------------------------------------------------------
 *
 * parallel.c
 *
 *	Parallel support for pg_dump and pg_restore
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Parallel operation works like this:
 *
 * The original, leader thread calls ParallelBackupStart(), which starts
 * the desired number of worker threads, which each enter WaitForCommands().
 *
 * The leader thread dispatches an individual work item to one of the worker
 * threads in DispatchJobForTocEntry().  We send a command string such as
 * "DUMP 1234" or "RESTORE 1234", where 1234 is the TocEntry ID.
 * The worker process receives and decodes the command and passes it to the
 * routine pointed to by AH->WorkerJobDumpPtr or AH->WorkerJobRestorePtr,
 * which are routines of the current archive format.  That routine performs
 * the required action (dump or restore) and returns an integer status code.
 * This is passed back to the leader where we pass it to the
 * ParallelCompletionPtr callback function that was passed to
 * DispatchJobForTocEntry().  The callback function does state updating
 * for the leader control logic in pg_backup_archiver.c.
 *
 * In principle additional archive-format-specific information might be needed
 * in commands or worker status responses, but so far that hasn't proved
 * necessary, since workers have full copies of the ArchiveHandle/TocEntry
 * data structures.  Remember that we have started the workers only after we
 * have read in the catalog.  That's why our worker threads can also
 * access the catalog information.
 *
 * The workerStatus field for each worker is only accessed by the leader, and
 * has one of the following values:
 *		WRKR_NOT_STARTED: we've not yet started this worker
 *		WRKR_IDLE: it's waiting for a command
 *		WRKR_WORKING: it's working on a command
 *		WRKR_TERMINATED: process ended
 * The pstate->te[] entry for each worker is valid when it's in WRKR_WORKING
 * state, and must be NULL in other states.
 */

#include "postgres_fe.h"

#include "fe_utils/string_utils.h"
#include "parallel.h"
#include "pg_backup_utils.h"
#ifdef WIN32
#include "port/pg_bswap.h"
#endif
#include "port/pg_threads_ext.h"

/* Mnemonic macros for indexing the fd array returned by pipe(2) */
#define PIPE_READ							0
#define PIPE_WRITE							1

#define NO_SLOT (-1)			/* Failure result for GetIdleWorker() */

/* Worker thread statuses */
typedef enum
{
	WRKR_NOT_STARTED = 0,
	WRKR_IDLE,
	WRKR_WORKING,
	WRKR_TERMINATED,
} T_WorkerStatus;

#define WORKER_IS_RUNNING(workerStatus) \
	((workerStatus) == WRKR_IDLE || (workerStatus) == WRKR_WORKING)

/*
 * Private per-parallel-worker state (typedef for this is in parallel.h).
 *
 * Much of this is valid only in the leader thread.  But the AH field should
 * be touched only by worker threads.  The pipe descriptors are valid
 * everywhere.
 */
struct ParallelSlot
{
	T_WorkerStatus workerStatus;	/* see enum above */

	/* These fields are valid if workerStatus == WRKR_WORKING: */
	ParallelCompletionPtr callback; /* function to call on completion */
	void	   *callback_data;	/* passthrough data for it */

	ArchiveHandle *AH;			/* Archive data worker is using */

	int			pipeRead;		/* leader's end of the pipes */
	int			pipeWrite;
	int			pipeRevRead;	/* child's end of the pipes */
	int			pipeRevWrite;

	PQExpBuffer buffer;
	pg_thrd_t	thread;
	int			result;
};

#ifdef WIN32

/* Windows implementation of pipe access */
static int	pgpipe(int handles[2]);
#define piperead(a,b,c)		recv(a,b,c,0)
#define pipewrite(a,b,c)	send(a,b,c,0)

#else							/* !WIN32 */

/* Non-Windows implementation of pipe access */
#define pgpipe(a)			pipe(a)
#define piperead(a,b,c)		read(a,b,c)
#define pipewrite(a,b,c)	write(a,b,c)

#endif							/* WIN32 */

/*
 * State info for archive_close_connection() shutdown callback.
 */
typedef struct ShutdownInformation
{
	ParallelState *pstate;
	Archive    *AHX;
} ShutdownInformation;

static ShutdownInformation shutdown_info;

/*
 * Write a simple string to stderr --- must be safe in a signal handler.
 * We ignore the write() result since there's not much we could do about it.
 * Certain compilers make that harder than it ought to be.
 */
#define write_stderr(str) \
	do { \
		const char *str_ = (str); \
		ssize_t	rc_; \
		rc_ = write(fileno(stderr), str_, strlen(str_)); \
		(void) rc_; \
	} while (0)

/* Pointer to each worker thread's ParallelSlot.  NULL in main thread. */
static thread_local ParallelSlot *parallel_slot_thread_local;

/* Local function prototypes */
static void archive_close_connection(int code, void *arg);
static void ShutdownWorkersHard(ParallelState *pstate);
static void WaitForTerminatingWorkers(ParallelState *pstate);
static void RunWorker(ParallelSlot *slot);
static int	GetIdleWorker(ParallelState *pstate);
static void lockTableForWorker(ArchiveHandle *AH, TocEntry *te);
static void WaitForCommands(ArchiveHandle *AH, int pipefd[2]);
static bool ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate,
							bool do_wait);
static char *getMessageFromLeader(int pipefd[2]);
static void sendMessageToLeader(int pipefd[2], const char *str);
static int	select_loop(int maxFd, fd_set *workerset);
static char *getMessageFromWorker(ParallelState *pstate,
								  bool do_wait, int *worker);
static void sendMessageToWorker(ParallelState *pstate,
								int worker, const char *str);
static char *readMessageFromPipe(int fd);

#define messageStartsWith(msg, prefix) \
	(strncmp(msg, prefix, strlen(prefix)) == 0)


/*
 * Initialize parallel dump support --- should be called early in process
 * startup.  (Currently, this is called whether or not we intend parallel
 * activity.)
 */
void
init_parallel_dump_utils(void)
{
	static bool parallel_init_done = false;

	if (!parallel_init_done)
	{
#ifdef WIN32
		WSADATA		wsaData;
		int			err;

		/* Initialize socket access */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
			pg_fatal("%s() failed: error code %d", "WSAStartup", err);
#endif
		parallel_init_done = true;
	}
}

/*
 * Exported for use by exit_nicely().
 */
bool
am_parallel_worker_thread(void)
{
	return parallel_slot_thread_local != NULL;
}

/*
 * A thread-local version of getLocalPQExpBuffer().
 */
static PQExpBuffer
getThreadLocalPQExpBuffer(void)
{
	PQExpBuffer id_return;

	/* Each worker has its own pre-allocated buffer to reuse repeatedly. */
	id_return = parallel_slot_thread_local->buffer;

	resetPQExpBuffer(id_return);

	return id_return;
}

/*
 * pg_dump and pg_restore call this to register the cleanup handler
 * as soon as they've created the ArchiveHandle.
 */
void
on_exit_close_archive(Archive *AHX)
{
	shutdown_info.AHX = AHX;
	on_exit_nicely(archive_close_connection, &shutdown_info);
}

/*
 * on_exit_nicely handler for shutting down database connections and
 * worker processes cleanly.
 */
static void
archive_close_connection(int code, void *arg)
{
	ShutdownInformation *si = (ShutdownInformation *) arg;

	if (si->pstate)
	{
		/* In parallel mode, must figure out who we are */
		ParallelSlot *slot = parallel_slot_thread_local;

		if (!slot)
		{
			/*
			 * We're the leader.  Forcibly shut down workers, then close our
			 * own database connection, if any.
			 */
			ShutdownWorkersHard(si->pstate);

			if (si->AHX)
				DisconnectDatabase(si->AHX);

			/* Close worker database connections. */
			for (int i = 0; i < si->pstate->numWorkers; ++i)
				if (si->pstate->parallelSlot[i].AH)
					DisconnectDatabase(&si->pstate->parallelSlot[i].AH->public);
		}
		else
		{
			/*
			 * We're a worker.  Close our communication sockets.
			 *
			 * (Without this, if this is a premature exit, the leader would
			 * fail to detect it because there would be no EOF condition on
			 * the other end of the pipe.)
			 */
			closesocket(slot->pipeRevRead);
			closesocket(slot->pipeRevWrite);
		}
	}
	else
	{
		/* Non-parallel operation: just kill the leader DB connection */
		if (si->AHX)
			DisconnectDatabase(si->AHX);
	}
}

/*
 * Forcibly shut down any remaining workers, waiting for them to finish.
 *
 * Note that we don't expect to come here during normal exit (the workers
 * should be long gone, and the ParallelState too).  We're only here in a
 * pg_fatal() situation, so intervening to cancel active commands is
 * appropriate.
 */
static void
ShutdownWorkersHard(ParallelState *pstate)
{
	int			i;

	Assert(!am_parallel_worker_thread());

	/*
	 * Close our write end of the sockets so that any workers waiting for
	 * commands know they can exit.  (Note: some of the pipeWrite fields might
	 * still be zero, if we failed to initialize all the workers.  Hence, just
	 * ignore errors here.)
	 */
	for (i = 0; i < pstate->numWorkers; i++)
		closesocket(pstate->parallelSlot[i].pipeWrite);

	/* Force early termination of any commands currently in progress. */
	quit_handler_cancel_and_remove_all_connections();

	/* Now wait for them to terminate. */
	WaitForTerminatingWorkers(pstate);
}

/*
 * Wait for all workers to terminate.
 */
static void
WaitForTerminatingWorkers(ParallelState *pstate)
{
	Assert(!am_parallel_worker_thread());

	for (int i = 0; i < pstate->numWorkers; ++i)
	{
		ParallelSlot *slot = &pstate->parallelSlot[i];

		if (slot->workerStatus != WRKR_TERMINATED)
		{
			int			error;

			if ((error = pg_thrd_join(slot->thread,
									  &slot->result)) != pg_thrd_success)
				pg_fatal("could not join thread %d: %s", i,
						 pg_thrd_error_string_with_detail(error));
			slot->workerStatus = WRKR_TERMINATED;
			pstate->te[i] = NULL;
		}
	}
}

/*
 * This function is called to set up and run a worker process.  Caller should
 * exit the thread upon return.
 */
static void
RunWorker(ParallelSlot *slot)
{
	ArchiveHandle *AH = slot->AH;
	int			pipefd[2];

	/* fetch child ends of pipes */
	pipefd[PIPE_READ] = slot->pipeRevRead;
	pipefd[PIPE_WRITE] = slot->pipeRevWrite;

	/*
	 * Call the setup worker function that's defined in the ArchiveHandle.
	 */
	(AH->SetupWorkerPtr) ((Archive *) AH);

	/*
	 * Execute commands until done.
	 */
	WaitForCommands(AH, pipefd);
}

static int
worker_thread_main(void *argument)
{
	ParallelSlot *slot = argument;

	/* For getThreadLocalPQExpBuffer() and am_parallel_worker_thread(). */
	parallel_slot_thread_local = slot;

	/* Run the worker ... */
	RunWorker(slot);

	/* Exit the thread */
	return 0;
}

/*
 * This function starts a parallel dump or restore by spawning off the worker
 * threads.
 */
ParallelState *
ParallelBackupStart(ArchiveHandle *AH)
{
	ParallelState *pstate;

	Assert(AH->public.numWorkers > 0);

	pstate = pg_malloc_object(ParallelState);

	pstate->numWorkers = AH->public.numWorkers;
	pstate->te = NULL;
	pstate->parallelSlot = NULL;

	if (AH->public.numWorkers == 1)
		return pstate;

	/* Create status arrays, being sure to initialize all fields to 0 */
	pstate->te =
		pg_malloc0_array(TocEntry *, pstate->numWorkers);
	pstate->parallelSlot =
		pg_malloc0_array(ParallelSlot, pstate->numWorkers);

	for (int i = 0; i < pstate->numWorkers; ++i)
	{
		ArchiveHandle *worker_ah;

		/* Make per-thread PQExpBuffer. */
		if (!(pstate->parallelSlot[i].buffer = createPQExpBuffer()))
			pg_fatal("could not allocate PQExpBuffer");

		/* Make per-thread archive clone with its own connection. */
		worker_ah = CloneArchive(AH);
		worker_ah->connCancel = PQgetCancel(worker_ah->connection);
		if (!worker_ah->connCancel)
			pg_fatal("could not create cancel connection");
		pstate->parallelSlot[i].AH = worker_ah;
	}

	/* Make fmtId() and fmtQualifiedId() use per-thread buffer. */
	getLocalPQExpBuffer = getThreadLocalPQExpBuffer;

	/*
	 * Set the pstate in shutdown_info, to tell the exit handler that it must
	 * clean up workers as well as the main database connection.
	 */
	shutdown_info.pstate = pstate;

	/* Create desired number of workers */
	for (int i = 0; i < pstate->numWorkers; i++)
	{
		ParallelSlot *slot = &pstate->parallelSlot[i];
		int			pipeMW[2],
					pipeWM[2];
		int			error;

		/* Create communication pipes for this worker */
		if (pgpipe(pipeMW) < 0 || pgpipe(pipeWM) < 0)
			pg_fatal("could not create communication channels: %m");

		/* leader's ends of the pipes */
		slot->pipeRead = pipeWM[PIPE_READ];
		slot->pipeWrite = pipeMW[PIPE_WRITE];
		/* child's ends of the pipes */
		slot->pipeRevRead = pipeMW[PIPE_READ];
		slot->pipeRevWrite = pipeWM[PIPE_WRITE];

		if ((error = pg_thrd_create(&slot->thread,
									worker_thread_main,
									slot)) != pg_thrd_success)
			pg_fatal("could not start worker thread: %s",
					 pg_thrd_error_string_with_detail(error));

		slot->workerStatus = WRKR_IDLE;
	}

	return pstate;
}

/*
 * Close down a parallel dump or restore.
 */
void
ParallelBackupEnd(ArchiveHandle *AH, ParallelState *pstate)
{
	/* No work if non-parallel */
	if (pstate->numWorkers == 1)
		return;

	/* There should not be any unfinished jobs */
	Assert(IsEveryWorkerIdle(pstate));

	/* Close the sockets so that the workers know they can exit */
	for (int i = 0; i < pstate->numWorkers; i++)
	{
		closesocket(pstate->parallelSlot[i].pipeRead);
		closesocket(pstate->parallelSlot[i].pipeWrite);
	}

	/* Wait for them to exit */
	WaitForTerminatingWorkers(pstate);

	/*
	 * Unlink pstate from shutdown_info, so the exit handler will not try to
	 * use it; and likewise unlink from signal_info.
	 */
	shutdown_info.pstate = NULL;

	/* Release per-thread resources. */
	for (int i = 0; i < pstate->numWorkers; ++i)
	{
		ParallelSlot *slot = &pstate->parallelSlot[i];

		destroyPQExpBuffer(slot->buffer);
		PQfreeCancel(slot->AH->connCancel);
		DisconnectDatabase(&slot->AH->public);
		DeCloneArchive(slot->AH);
	}

	/* Release state (mere neatnik-ism, since we're about to terminate) */
	free(pstate->te);
	free(pstate->parallelSlot);
	free(pstate);
}

/*
 * These next four functions handle construction and parsing of the command
 * strings and response strings for parallel workers.
 *
 * Currently, these can be the same regardless of which archive format we are
 * processing.  In future, we might want to let format modules override these
 * functions to add format-specific data to a command or response.
 */

/*
 * buildWorkerCommand: format a command string to send to a worker.
 *
 * The string is built in the caller-supplied buffer of size buflen.
 */
static void
buildWorkerCommand(ArchiveHandle *AH, TocEntry *te, T_Action act,
				   char *buf, int buflen)
{
	if (act == ACT_DUMP)
		snprintf(buf, buflen, "DUMP %d", te->dumpId);
	else if (act == ACT_RESTORE)
		snprintf(buf, buflen, "RESTORE %d", te->dumpId);
	else
		Assert(false);
}

/*
 * parseWorkerCommand: interpret a command string in a worker.
 */
static void
parseWorkerCommand(ArchiveHandle *AH, TocEntry **te, T_Action *act,
				   const char *msg)
{
	DumpId		dumpId;
	int			nBytes;

	if (messageStartsWith(msg, "DUMP "))
	{
		*act = ACT_DUMP;
		sscanf(msg, "DUMP %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else if (messageStartsWith(msg, "RESTORE "))
	{
		*act = ACT_RESTORE;
		sscanf(msg, "RESTORE %d%n", &dumpId, &nBytes);
		Assert(nBytes == strlen(msg));
		*te = getTocEntryByDumpId(AH, dumpId);
		Assert(*te != NULL);
	}
	else
		pg_fatal("unrecognized command received from leader: \"%s\"",
				 msg);
}

/*
 * buildWorkerResponse: format a response string to send to the leader.
 *
 * The string is built in the caller-supplied buffer of size buflen.
 */
static void
buildWorkerResponse(ArchiveHandle *AH, TocEntry *te, T_Action act, int status,
					char *buf, int buflen)
{
	snprintf(buf, buflen, "OK %d %d %d",
			 te->dumpId,
			 status,
			 status == WORKER_IGNORED_ERRORS ? AH->public.n_errors : 0);
}

/*
 * parseWorkerResponse: parse the status message returned by a worker.
 *
 * Returns the integer status code, and may update fields of AH and/or te.
 */
static int
parseWorkerResponse(ArchiveHandle *AH, TocEntry *te,
					const char *msg)
{
	DumpId		dumpId;
	int			nBytes,
				n_errors;
	int			status = 0;

	if (messageStartsWith(msg, "OK "))
	{
		sscanf(msg, "OK %d %d %d%n", &dumpId, &status, &n_errors, &nBytes);

		Assert(dumpId == te->dumpId);
		Assert(nBytes == strlen(msg));

		AH->public.n_errors += n_errors;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	return status;
}

/*
 * Dispatch a job to some free worker.
 *
 * te is the TocEntry to be processed, act is the action to be taken on it.
 * callback is the function to call on completion of the job.
 *
 * If no worker is currently available, this will block, and previously
 * registered callback functions may be called.
 */
void
DispatchJobForTocEntry(ArchiveHandle *AH,
					   ParallelState *pstate,
					   TocEntry *te,
					   T_Action act,
					   ParallelCompletionPtr callback,
					   void *callback_data)
{
	int			worker;
	char		buf[256];

	/* Get a worker, waiting if none are idle */
	while ((worker = GetIdleWorker(pstate)) == NO_SLOT)
		WaitForWorkers(AH, pstate, WFW_ONE_IDLE);

	/* Construct and send command string */
	buildWorkerCommand(AH, te, act, buf, sizeof(buf));

	sendMessageToWorker(pstate, worker, buf);

	/* Remember worker is busy, and which TocEntry it's working on */
	pstate->parallelSlot[worker].workerStatus = WRKR_WORKING;
	pstate->parallelSlot[worker].callback = callback;
	pstate->parallelSlot[worker].callback_data = callback_data;
	pstate->te[worker] = te;
}

/*
 * Find an idle worker and return its slot number.
 * Return NO_SLOT if none are idle.
 */
static int
GetIdleWorker(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus == WRKR_IDLE)
			return i;
	}
	return NO_SLOT;
}

/*
 * Return true iff every worker is in the WRKR_IDLE state.
 */
bool
IsEveryWorkerIdle(ParallelState *pstate)
{
	int			i;

	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (pstate->parallelSlot[i].workerStatus != WRKR_IDLE)
			return false;
	}
	return true;
}

/*
 * Acquire lock on a table to be dumped by a worker process.
 *
 * The leader process is already holding an ACCESS SHARE lock.  Ordinarily
 * it's no problem for a worker to get one too, but if anything else besides
 * pg_dump is running, there's a possible deadlock:
 *
 * 1) Leader dumps the schema and locks all tables in ACCESS SHARE mode.
 * 2) Another process requests an ACCESS EXCLUSIVE lock (which is not granted
 *	  because the leader holds a conflicting ACCESS SHARE lock).
 * 3) A worker process also requests an ACCESS SHARE lock to read the table.
 *	  The worker is enqueued behind the ACCESS EXCLUSIVE lock request.
 * 4) Now we have a deadlock, since the leader is effectively waiting for
 *	  the worker.  The server cannot detect that, however.
 *
 * To prevent an infinite wait, prior to touching a table in a worker, request
 * a lock in ACCESS SHARE mode but with NOWAIT.  If we don't get the lock,
 * then we know that somebody else has requested an ACCESS EXCLUSIVE lock and
 * so we have a deadlock.  We must fail the backup in that case.
 */
static void
lockTableForWorker(ArchiveHandle *AH, TocEntry *te)
{
	const char *qualId;
	PQExpBuffer query;
	PGresult   *res;

	/* Nothing to do for BLOBS */
	if (strcmp(te->desc, "BLOBS") == 0)
		return;

	query = createPQExpBuffer();

	qualId = fmtQualifiedId(te->namespace, te->tag);

	appendPQExpBuffer(query, "LOCK TABLE %s IN ACCESS SHARE MODE NOWAIT",
					  qualId);

	res = PQexec(AH->connection, query->data);

	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("could not obtain lock on relation \"%s\"\n"
				 "This usually means that someone requested an ACCESS EXCLUSIVE lock "
				 "on the table after the pg_dump parent process had gotten the "
				 "initial ACCESS SHARE lock on the table.", qualId);

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * WaitForCommands: main routine for a worker process.
 *
 * Read and execute commands from the leader until we see EOF on the pipe.
 */
static void
WaitForCommands(ArchiveHandle *AH, int pipefd[2])
{
	char	   *command;
	TocEntry   *te;
	T_Action	act;
	int			status = 0;
	char		buf[256];

	for (;;)
	{
		if (!(command = getMessageFromLeader(pipefd)))
		{
			/* EOF, so done */
			return;
		}

		/* Decode the command */
		parseWorkerCommand(AH, &te, &act, command);

		if (act == ACT_DUMP)
		{
			/* Acquire lock on this table within the worker's session */
			lockTableForWorker(AH, te);

			/* Perform the dump command */
			status = (AH->WorkerJobDumpPtr) (AH, te);
		}
		else if (act == ACT_RESTORE)
		{
			/* Perform the restore command */
			status = (AH->WorkerJobRestorePtr) (AH, te);
		}
		else
			Assert(false);

		/* Return status to leader */
		buildWorkerResponse(AH, te, act, status, buf, sizeof(buf));

		sendMessageToLeader(pipefd, buf);

		/* command was pg_malloc'd and we are responsible for free()ing it. */
		free(command);
	}
}

/*
 * Check for status messages from workers.
 *
 * If do_wait is true, wait to get a status message; otherwise, just return
 * immediately if there is none available.
 *
 * When we get a status message, we pass the status code to the callback
 * function that was specified to DispatchJobForTocEntry, then reset the
 * worker status to IDLE.
 *
 * Returns true if we collected a status message, else false.
 *
 * XXX is it worth checking for more than one status message per call?
 * It seems somewhat unlikely that multiple workers would finish at exactly
 * the same time.
 */
static bool
ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate, bool do_wait)
{
	int			worker;
	char	   *msg;

	/* Try to collect a status message */
	msg = getMessageFromWorker(pstate, do_wait, &worker);

	if (!msg)
	{
		/* If do_wait is true, we must have detected EOF on some socket */
		if (do_wait)
			pg_fatal("a worker process died unexpectedly");
		return false;
	}

	/* Process it and update our idea of the worker's status */
	if (messageStartsWith(msg, "OK "))
	{
		ParallelSlot *slot = &pstate->parallelSlot[worker];
		TocEntry   *te = pstate->te[worker];
		int			status;

		status = parseWorkerResponse(AH, te, msg);
		slot->callback(AH, te, status, slot->callback_data);
		slot->workerStatus = WRKR_IDLE;
		pstate->te[worker] = NULL;
	}
	else
		pg_fatal("invalid message received from worker: \"%s\"",
				 msg);

	/* Free the string returned from getMessageFromWorker */
	free(msg);

	return true;
}

/*
 * Check for status results from workers, waiting if necessary.
 *
 * Available wait modes are:
 * WFW_NO_WAIT: reap any available status, but don't block
 * WFW_GOT_STATUS: wait for at least one more worker to finish
 * WFW_ONE_IDLE: wait for at least one worker to be idle
 * WFW_ALL_IDLE: wait for all workers to be idle
 *
 * Any received results are passed to the callback specified to
 * DispatchJobForTocEntry.
 *
 * This function is executed in the leader process.
 */
void
WaitForWorkers(ArchiveHandle *AH, ParallelState *pstate, WFW_WaitOption mode)
{
	bool		do_wait = false;

	/*
	 * In GOT_STATUS mode, always block waiting for a message, since we can't
	 * return till we get something.  In other modes, we don't block the first
	 * time through the loop.
	 */
	if (mode == WFW_GOT_STATUS)
	{
		/* Assert that caller knows what it's doing */
		Assert(!IsEveryWorkerIdle(pstate));
		do_wait = true;
	}

	for (;;)
	{
		/*
		 * Check for status messages, even if we don't need to block.  We do
		 * not try very hard to reap all available messages, though, since
		 * there's unlikely to be more than one.
		 */
		if (ListenToWorkers(AH, pstate, do_wait))
		{
			/*
			 * If we got a message, we are done by definition for GOT_STATUS
			 * mode, and we can also be certain that there's at least one idle
			 * worker.  So we're done in all but ALL_IDLE mode.
			 */
			if (mode != WFW_ALL_IDLE)
				return;
		}

		/* Check whether we must wait for new status messages */
		switch (mode)
		{
			case WFW_NO_WAIT:
				return;			/* never wait */
			case WFW_GOT_STATUS:
				Assert(false);	/* can't get here, because we waited */
				break;
			case WFW_ONE_IDLE:
				if (GetIdleWorker(pstate) != NO_SLOT)
					return;
				break;
			case WFW_ALL_IDLE:
				if (IsEveryWorkerIdle(pstate))
					return;
				break;
		}

		/* Loop back, and this time wait for something to happen */
		do_wait = true;
	}
}

/*
 * Read one command message from the leader, blocking if necessary
 * until one is available, and return it as a malloc'd string.
 * On EOF, return NULL.
 *
 * This function is executed in worker processes.
 */
static char *
getMessageFromLeader(int pipefd[2])
{
	return readMessageFromPipe(pipefd[PIPE_READ]);
}

/*
 * Send a status message to the leader.
 *
 * This function is executed in worker processes.
 */
static void
sendMessageToLeader(int pipefd[2], const char *str)
{
	size_t		len = strlen(str) + 1;

	if (pipewrite(pipefd[PIPE_WRITE], str, len) != len)
		pg_fatal("could not write to the communication channel: %m");
}

/*
 * Wait until some descriptor in "workerset" becomes readable.
 * Returns -1 on error, else the number of readable descriptors.
 */
static int
select_loop(int maxFd, fd_set *workerset)
{
	int			i;
	fd_set		saveSet = *workerset;

	for (;;)
	{
		*workerset = saveSet;
		i = select(maxFd + 1, workerset, NULL, NULL, NULL);

#ifndef WIN32
		if (i < 0 && errno == EINTR)
			continue;
#else
		if (i == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
			continue;
#endif
		break;
	}

	return i;
}


/*
 * Check for messages from worker processes.
 *
 * If a message is available, return it as a malloc'd string, and put the
 * index of the sending worker in *worker.
 *
 * If nothing is available, wait if "do_wait" is true, else return NULL.
 *
 * If we detect EOF on any socket, we'll return NULL.  It's not great that
 * that's hard to distinguish from the no-data-available case, but for now
 * our one caller is okay with that.
 *
 * This function is executed in the leader process.
 */
static char *
getMessageFromWorker(ParallelState *pstate, bool do_wait, int *worker)
{
	int			i;
	fd_set		workerset;
	int			maxFd = -1;
	struct timeval nowait = {0, 0};

	/* construct bitmap of socket descriptors for select() */
	FD_ZERO(&workerset);
	for (i = 0; i < pstate->numWorkers; i++)
	{
		if (!WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			continue;
		FD_SET(pstate->parallelSlot[i].pipeRead, &workerset);
		if (pstate->parallelSlot[i].pipeRead > maxFd)
			maxFd = pstate->parallelSlot[i].pipeRead;
	}

	if (do_wait)
	{
		i = select_loop(maxFd, &workerset);
		Assert(i != 0);
	}
	else
	{
		if ((i = select(maxFd + 1, &workerset, NULL, NULL, &nowait)) == 0)
			return NULL;
	}

	if (i < 0)
		pg_fatal("%s() failed: %m", "select");

	for (i = 0; i < pstate->numWorkers; i++)
	{
		char	   *msg;

		if (!WORKER_IS_RUNNING(pstate->parallelSlot[i].workerStatus))
			continue;
		if (!FD_ISSET(pstate->parallelSlot[i].pipeRead, &workerset))
			continue;

		/*
		 * Read the message if any.  If the socket is ready because of EOF,
		 * we'll return NULL instead (and the socket will stay ready, so the
		 * condition will persist).
		 *
		 * Note: because this is a blocking read, we'll wait if only part of
		 * the message is available.  Waiting a long time would be bad, but
		 * since worker status messages are short and are always sent in one
		 * operation, it shouldn't be a problem in practice.
		 */
		msg = readMessageFromPipe(pstate->parallelSlot[i].pipeRead);
		*worker = i;
		return msg;
	}
	Assert(false);
	return NULL;
}

/*
 * Send a command message to the specified worker process.
 *
 * This function is executed in the leader process.
 */
static void
sendMessageToWorker(ParallelState *pstate, int worker, const char *str)
{
	size_t		len = strlen(str) + 1;

	if (pipewrite(pstate->parallelSlot[worker].pipeWrite, str, len) != len)
	{
		pg_fatal("could not write to the communication channel: %m");
	}
}

/*
 * Read one message from the specified pipe (fd), blocking if necessary
 * until one is available, and return it as a malloc'd string.
 * On EOF, return NULL.
 *
 * A "message" on the channel is just a null-terminated string.
 */
static char *
readMessageFromPipe(int fd)
{
	char	   *msg;
	int			msgsize,
				bufsize;
	int			ret;

	/*
	 * In theory, if we let piperead() read multiple bytes, it might give us
	 * back fragments of multiple messages.  (That can't actually occur, since
	 * neither leader nor workers send more than one message without waiting
	 * for a reply, but we don't wish to assume that here.)  For simplicity,
	 * read a byte at a time until we get the terminating '\0'.  This method
	 * is a bit inefficient, but since this is only used for relatively short
	 * command and status strings, it shouldn't matter.
	 */
	bufsize = 64;				/* could be any number */
	msg = (char *) pg_malloc(bufsize);
	msgsize = 0;
	for (;;)
	{
		Assert(msgsize < bufsize);
		ret = piperead(fd, msg + msgsize, 1);
		if (ret <= 0)
			break;				/* error or connection closure */

		Assert(ret == 1);

		if (msg[msgsize] == '\0')
			return msg;			/* collected whole message */

		msgsize++;
		if (msgsize == bufsize) /* enlarge buffer if needed */
		{
			bufsize += 16;		/* could be any number */
			msg = (char *) pg_realloc(msg, bufsize);
		}
	}

	/* Other end has closed the connection */
	pg_free(msg);
	return NULL;
}

#ifdef WIN32

/*
 * This is a replacement version of pipe(2) for Windows which allows the pipe
 * handles to be used in select().
 *
 * Reads and writes on the pipe must go through piperead()/pipewrite().
 *
 * For consistency with Unix we declare the returned handles as "int".
 * This is okay even on WIN64 because system handles are not more than
 * 32 bits wide, but we do have to do some casting.
 */
static int
pgpipe(int handles[2])
{
	pgsocket	s,
				tmp_sock;
	struct sockaddr_in serv_addr;
	int			len = sizeof(serv_addr);

	/* We have to use the Unix socket invalid file descriptor value here. */
	handles[0] = handles[1] = -1;

	/*
	 * setup listen socket
	 */
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not create socket: error code %d",
					 WSAGetLastError());
		return -1;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = pg_hton16(0);
	serv_addr.sin_addr.s_addr = pg_hton32(INADDR_LOOPBACK);
	if (bind(s, (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not bind: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (listen(s, 1) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not listen: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	if (getsockname(s, (SOCKADDR *) &serv_addr, &len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: %s() failed: error code %d", "getsockname",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}

	/*
	 * setup pipe handles
	 */
	if ((tmp_sock = socket(AF_INET, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not create second socket: error code %d",
					 WSAGetLastError());
		closesocket(s);
		return -1;
	}
	handles[1] = (int) tmp_sock;

	if (connect(handles[1], (SOCKADDR *) &serv_addr, len) == SOCKET_ERROR)
	{
		pg_log_error("pgpipe: could not connect socket: error code %d",
					 WSAGetLastError());
		closesocket(handles[1]);
		handles[1] = -1;
		closesocket(s);
		return -1;
	}
	if ((tmp_sock = accept(s, (SOCKADDR *) &serv_addr, &len)) == PGINVALID_SOCKET)
	{
		pg_log_error("pgpipe: could not accept connection: error code %d",
					 WSAGetLastError());
		closesocket(handles[1]);
		handles[1] = -1;
		closesocket(s);
		return -1;
	}
	handles[0] = (int) tmp_sock;

	closesocket(s);
	return 0;
}

#endif							/* WIN32 */
