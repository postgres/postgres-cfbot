/*-------------------------------------------------------------------------
 *
 *	parallel_slot.c
 *		Parallel support for front-end parallel database connections
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/parallel_slot.c
 *
 *-------------------------------------------------------------------------
 */

#if defined(WIN32) && FD_SETSIZE < 1024
#error FD_SETSIZE needs to have been increased
#endif

#include "postgres_fe.h"

#include <sys/select.h>
#ifndef WIN32
#include <pthread.h>
#include <signal.h>
#endif

#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "fe_utils/parallel_slot.h"
#include "fe_utils/query_utils.h"

#define ERRCODE_UNDEFINED_TABLE  "42P01"

static int	select_loop(int maxFd, fd_set *workerset);
static bool processQueryResult(ParallelSlot *slot, PGresult *result);

/*
 * Process (and delete) a query result.  Returns true if there's no problem,
 * false otherwise. It's up to the handler to decide what constitutes a
 * problem.
 */
static bool
processQueryResult(ParallelSlot *slot, PGresult *result)
{
	Assert(slot->handler != NULL);

	/* On failure, the handler should return NULL after freeing the result */
	if (!slot->handler(result, slot->connection, slot->handler_context))
		return false;

	/* Ok, we have to free it ourself */
	PQclear(result);
	return true;
}

/*
 * Wait until a file descriptor from the given set becomes readable.
 *
 * Returns the number of ready descriptors, or -1 on failure (including
 * getting a cancel request).
 */
static int
select_loop(int maxFd, fd_set *workerset)
{
	int			i;
	fd_set		saveSet = *workerset;

#ifndef WIN32

	/*
	 * Keep SIGINT blocked except while waiting in pselect().  Otherwise a
	 * SIGINT arriving between the CancelRequested check and the start of the
	 * wait would go unnoticed until a socket becomes readable, which for a
	 * stuck server might be never.
	 */
	sigset_t	origmask;
	sigset_t	blockmask;

	sigemptyset(&blockmask);
	sigaddset(&blockmask, SIGINT);
	pthread_sigmask(SIG_BLOCK, &blockmask, &origmask);
#endif

	for (;;)
	{
		/*
		 * On Windows, we need to check once in a while for cancel requests;
		 * on other platforms we rely on pselect() returning when interrupted.
		 */
#ifdef WIN32
		struct timeval tv = {1, 0};
#endif

		if (CancelRequested)
		{
			i = -1;
			break;
		}

		*workerset = saveSet;
#ifdef WIN32
		i = select(maxFd + 1, workerset, NULL, NULL, &tv);
#else
		i = pselect(maxFd + 1, workerset, NULL, NULL, NULL, &origmask);
#endif

#ifdef WIN32
		if (i == SOCKET_ERROR)
		{
			i = -1;

			if (WSAGetLastError() == WSAEINTR)
				errno = EINTR;
		}
#endif

		if (i < 0 && errno == EINTR)
			continue;			/* ignore this */
		if (i == 0)
			continue;			/* timeout (Win32 only) */
		break;
	}

#ifndef WIN32
	pthread_sigmask(SIG_SETMASK, &origmask, NULL);
#endif

	return i;
}

/*
 * Return the offset of a suitable idle slot, or -1 if none are available.  If
 * the given dbname is not null, only idle slots connected to the given
 * database are considered suitable, otherwise all idle connected slots are
 * considered suitable.
 */
static int
find_matching_idle_slot(const ParallelSlotArray *sa, const char *dbname)
{
	int			i;

	for (i = 0; i < sa->numslots; i++)
	{
		if (sa->slots[i].inUse)
			continue;

		if (sa->slots[i].connection == NULL)
			continue;

		if (dbname == NULL ||
			strcmp(PQdb(sa->slots[i].connection), dbname) == 0)
			return i;
	}
	return -1;
}

/*
 * Return the offset of the first slot without a database connection, or -1 if
 * all slots are connected.
 */
static int
find_unconnected_slot(const ParallelSlotArray *sa)
{
	int			i;

	for (i = 0; i < sa->numslots; i++)
	{
		if (sa->slots[i].inUse)
			continue;

		if (sa->slots[i].connection == NULL)
			return i;
	}

	return -1;
}

/*
 * Return the offset of the first idle slot, or -1 if all slots are busy.
 */
static int
find_any_idle_slot(const ParallelSlotArray *sa)
{
	int			i;

	for (i = 0; i < sa->numslots; i++)
		if (!sa->slots[i].inUse)
			return i;

	return -1;
}

/*
 * Wait for any slot's connection to have query results, consume the results,
 * and update the slot's status as appropriate.  Returns true on success,
 * false on cancellation, on error, or if no slots are connected.  Slots
 * without a connection are ignored.
 */
static bool
wait_on_slots(ParallelSlotArray *sa)
{
	int			i;
	fd_set		slotset;
	int			maxFd = -1;

	/* We must reconstruct the fd_set for each call to select_loop */
	FD_ZERO(&slotset);

	for (i = 0; i < sa->numslots; i++)
	{
		int			sock;

		if (sa->slots[i].connection == NULL)
			continue;

		sock = PQsocket(sa->slots[i].connection);

		/*
		 * We don't really expect any connections to lose their sockets after
		 * startup, but just in case, cope by ignoring them.
		 */
		if (sock < 0)
			continue;

		FD_SET(sock, &slotset);
		if (sock > maxFd)
			maxFd = sock;
	}

	/*
	 * If we get this far with no valid connections, processing cannot
	 * continue.
	 */
	if (maxFd < 0)
		return false;

	i = select_loop(maxFd, &slotset);

	/* failure? */
	if (i < 0)
		return false;

	for (i = 0; i < sa->numslots; i++)
	{
		int			sock;

		if (sa->slots[i].connection == NULL)
			continue;

		sock = PQsocket(sa->slots[i].connection);

		if (sock >= 0 && FD_ISSET(sock, &slotset))
		{
			/* select() says input is available, so consume it */
			PQconsumeInput(sa->slots[i].connection);
		}

		/* Collect result(s) as long as any are available */
		while (!PQisBusy(sa->slots[i].connection))
		{
			PGresult   *result = PQgetResult(sa->slots[i].connection);

			if (result != NULL)
			{
				/* Handle and discard the command result */
				if (!processQueryResult(&sa->slots[i], result))
					return false;
			}
			else
			{
				/* This connection has become idle */
				ParallelSlotSetIdle(&sa->slots[i]);
				break;
			}
		}
	}
	return true;
}

/*
 * Open a new database connection using the stored connection parameters and
 * optionally a given dbname if not null, execute the stored initial command if
 * any, and associate the new connection with the given slot.
 */
static void
connect_slot(ParallelSlotArray *sa, int slotno, const char *dbname)
{
	const char *old_override;
	ParallelSlot *slot = &sa->slots[slotno];

	old_override = sa->cparams->override_dbname;
	if (dbname)
		sa->cparams->override_dbname = dbname;
	slot->connection = connectDatabase(sa->cparams, sa->progname, sa->echo, false, true);
	sa->cparams->override_dbname = old_override;

	/*
	 * POSIX defines FD_SETSIZE as the highest file descriptor acceptable to
	 * FD_SET() and allied macros.  Windows defines it as a ceiling on the
	 * count of file descriptors in the set, not a ceiling on the value of
	 * each file descriptor; see
	 * https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select
	 * and
	 * https://learn.microsoft.com/en-us/windows/win32/api/winsock/ns-winsock-fd_set.
	 * We can't ignore that, because Windows starts file descriptors at a
	 * higher value, delays reuse, and skips values.  With less than ten
	 * concurrent file descriptors, opened and closed rapidly, one can reach
	 * file descriptor 1024.
	 *
	 * Doing a hard exit here is a bit grotty, but it doesn't seem worth
	 * complicating the API to make it less grotty.
	 */
#ifdef WIN32
	if (slotno >= FD_SETSIZE)
	{
		pg_log_error("too many jobs for this platform: %d", slotno);
		exit(1);
	}
#else
	{
		int			fd = PQsocket(slot->connection);

		if (fd >= FD_SETSIZE)
		{
			pg_log_error("socket file descriptor out of range for select(): %d",
						 fd);
			pg_log_error_hint("Try fewer jobs.");
			exit(1);
		}
	}
#endif

	/* Setup the connection using the supplied command, if any. */
	if (sa->initcmd)
		executeCommand(slot->connection, sa->initcmd, sa->echo);
}

/*
 * ParallelSlotsGetIdle
 *		Return a connection slot that is ready to execute a command.
 *
 * The slot returned is chosen as follows:
 *
 * If any idle slot already has an open connection, and if either dbname is
 * null or the existing connection is to the given database, that slot will be
 * returned allowing the connection to be reused.
 *
 * Otherwise, if any idle slot is not yet connected to any database, the slot
 * will be returned with its connection opened using the stored cparams and
 * optionally the given dbname if not null.
 *
 * Otherwise, if any idle slot exists, an idle slot will be chosen and returned
 * after having its connection disconnected and reconnected using the stored
 * cparams and optionally the given dbname if not null.
 *
 * Otherwise, if any slots have connections that are busy, we loop on select()
 * until one socket becomes available.  When this happens, we read the whole
 * set and mark as free all sockets that become available.  We then select a
 * slot using the same rules as above.
 *
 * Otherwise, we cannot return a slot, which is an error, and NULL is returned.
 *
 * For any connection created, if the stored initcmd is not null, it will be
 * executed as a command on the newly formed connection before the slot is
 * returned.
 *
 * If an error occurs, NULL is returned.
 */
ParallelSlot *
ParallelSlotsGetIdle(ParallelSlotArray *sa, const char *dbname)
{
	int			offset;

	Assert(sa);
	Assert(sa->numslots > 0);

	while (1)
	{
		/* First choice: a slot already connected to the desired database. */
		offset = find_matching_idle_slot(sa, dbname);
		if (offset >= 0)
		{
			sa->slots[offset].inUse = true;
			return &sa->slots[offset];
		}

		/* Second choice: a slot not connected to any database. */
		offset = find_unconnected_slot(sa);
		if (offset >= 0)
		{
			connect_slot(sa, offset, dbname);
			sa->slots[offset].inUse = true;
			return &sa->slots[offset];
		}

		/* Third choice: a slot connected to the wrong database. */
		offset = find_any_idle_slot(sa);
		if (offset >= 0)
		{
			disconnectDatabase(sa->slots[offset].connection);
			sa->slots[offset].connection = NULL;
			connect_slot(sa, offset, dbname);
			sa->slots[offset].inUse = true;
			return &sa->slots[offset];
		}

		/*
		 * Fourth choice: block until one or more slots become available. If
		 * any slots hit a fatal error, we'll find out about that here and
		 * return NULL.
		 */
		if (!wait_on_slots(sa))
			return NULL;
	}
}

/*
 * ParallelSlotsSetup
 *		Prepare a set of parallel slots but do not connect to any database.
 *
 * This creates and initializes a set of slots, marking all parallel slots as
 * free and ready to use.  Establishing connections is delayed until requesting
 * a free slot.  The cparams, progname, echo, and initcmd are stored for later
 * use and must remain valid for the lifetime of the returned array.
 */
ParallelSlotArray *
ParallelSlotsSetup(int numslots, ConnParams *cparams, const char *progname,
				   bool echo, const char *initcmd)
{
	ParallelSlotArray *sa;

	Assert(numslots > 0);
	Assert(cparams != NULL);
	Assert(progname != NULL);

	sa = (ParallelSlotArray *) palloc0(offsetof(ParallelSlotArray, slots) +
									   numslots * sizeof(ParallelSlot));

	sa->numslots = numslots;
	sa->cparams = cparams;
	sa->progname = progname;
	sa->echo = echo;
	sa->initcmd = initcmd;

	return sa;
}

/*
 * ParallelSlotsAdoptConn
 *		Assign an open connection to the slots array for reuse.
 *
 * This turns over ownership of an open connection to a slots array.  The
 * caller should not further use or close the connection.  All the connection's
 * parameters (user, host, port, etc.) except possibly dbname should match
 * those of the slots array's cparams, as given in ParallelSlotsSetup.  If
 * these parameters differ, subsequent behavior is undefined.
 */
void
ParallelSlotsAdoptConn(ParallelSlotArray *sa, PGconn *conn)
{
	int			offset;

	offset = find_unconnected_slot(sa);
	if (offset >= 0)
		sa->slots[offset].connection = conn;
	else
		disconnectDatabase(conn);
}

/*
 * ParallelSlotsTerminate
 *		Clean up a set of parallel slots
 *
 * Iterate through all connections in a given set of ParallelSlots and
 * terminate all connections.
 */
void
ParallelSlotsTerminate(ParallelSlotArray *sa)
{
	int			i;
	int			ncanceled = 0;

	for (i = 0; i < sa->numslots; i++)
	{
		PGconn	   *conn = sa->slots[i].connection;

		if (conn == NULL)
			continue;

		if (PQtransactionStatus(conn) == PQTRANS_ACTIVE)
			ncanceled++;

		disconnectDatabase(conn);
	}

	/*
	 * Only report this if the user asked for the cancel, not when we cancel
	 * the remaining queries because one of them failed.
	 */
	if (ncanceled > 0 && CancelRequested)
		fprintf(stderr, ngettext("Cancel request sent\n",
								 "Cancel requests sent\n",
								 ncanceled));
}

/*
 * Return whether any slot is still running a query.
 */
static bool
any_slot_busy(const ParallelSlotArray *sa)
{
	for (int i = 0; i < sa->numslots; i++)
	{
		if (PQtransactionStatus(sa->slots[i].connection) == PQTRANS_ACTIVE)
			return true;
	}

	return false;
}

/*
 * ParallelSlotsWaitCompletion
 *
 * Wait for all connections to finish, returning false if at least one
 * error has been found on the way.
 */
bool
ParallelSlotsWaitCompletion(ParallelSlotArray *sa)
{
	while (any_slot_busy(sa))
	{
		if (!wait_on_slots(sa))
			return false;
	}

	/*
	 * wait_on_slots only marks a slot as idle once its query has finished.
	 * That never happens for a slot that was handed out but on which sending
	 * the query failed, so mark all of them as idle here.
	 */
	for (int i = 0; i < sa->numslots; i++)
		ParallelSlotSetIdle(&sa->slots[i]);

	return true;
}

/*
 * TableCommandResultHandler
 *
 * ParallelSlotResultHandler for results of commands (not queries) against
 * tables.
 *
 * Requires that the result status is either PGRES_COMMAND_OK or an error about
 * a missing table.  This is useful for utilities that compile a list of tables
 * to process and then run commands (vacuum, reindex, or whatever) against
 * those tables, as there is a race condition between the time the list is
 * compiled and the time the command attempts to open the table.
 *
 * For missing tables, logs an error but allows processing to continue.
 *
 * For all other errors, logs an error and terminates further processing.
 *
 * res: PGresult from the query executed on the slot's connection
 * conn: connection belonging to the slot
 * context: unused
 */
bool
TableCommandResultHandler(PGresult *res, PGconn *conn, void *context)
{
	Assert(res != NULL);
	Assert(conn != NULL);

	/*
	 * If it's an error, report it.  Errors about a missing table are harmless
	 * so we continue processing; but die for other errors.
	 */
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		char	   *sqlState = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		pg_log_error("processing of database \"%s\" failed: %s",
					 PQdb(conn), PQerrorMessage(conn));

		if (sqlState && strcmp(sqlState, ERRCODE_UNDEFINED_TABLE) != 0)
		{
			PQclear(res);
			return false;
		}
	}

	return true;
}
