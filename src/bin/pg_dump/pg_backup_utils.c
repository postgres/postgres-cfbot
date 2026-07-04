/*-------------------------------------------------------------------------
 *
 * pg_backup_utils.c
 *	Utility routines shared by pg_dump and pg_restore
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_backup_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "parallel.h"
#include "pg_backup_utils.h"
#include "port/pg_threads.h"
#include "port/pg_signal_processor.h"
#include "fe_utils/cancel_set.h"

/* Globals exported by this file */
const char *progname = NULL;

/*
 * Flag telling worker threads to stay quiet about query failures because
 * we're cancelling their queries as part of tearing down the process.  See
 * the comment in pg_backup_utils.h.
 *
 * The cancel thread writes it while worker threads read it, so it's marked
 * volatile to keep the compiler from caching the value.  A plain volatile
 * bool isn't a memory barrier, but it's good enough here.  A lot of things
 * happen between set_cancel_in_progress() in the cancel thread and the other
 * threads calling is_cancel_in_progress(), including network operations,
 * which implicitly act as memory barriers.  Furthermore, the flag is only
 * ever flipped one way (false to true) and a worker briefly observing the
 * stale false just means it would print one error before the process dies.
 * The only goal of this flag is to make sure workers don't log "query
 * cancelled" errors during the shutdown process.
 *
 * XXX: This should be swapped out for a proper atomic when we have those in
 * the frontend code, so that we wouldn't need to rationalizee all of the
 * above.
 */
static volatile bool cancelInProgress = false;

void
set_cancel_in_progress(void)
{
	cancelInProgress = true;
}

bool
is_cancel_in_progress(void)
{
	return cancelInProgress;
}

#define MAX_ON_EXIT_NICELY				20

static struct
{
	on_exit_nicely_callback function;
	void	   *arg;
}			on_exit_nicely_list[MAX_ON_EXIT_NICELY];

static int	on_exit_nicely_index;

static cancel_set *quit_handler_connections;

/*
 * Parse a --section=foo command line argument.
 *
 * Set or update the bitmask in *dumpSections according to arg.
 * dumpSections is initialised as DUMP_UNSECTIONED by pg_dump and
 * pg_restore so they can know if this has even been called.
 */
void
set_dump_section(const char *arg, int *dumpSections)
{
	/* if this is the first call, clear all the bits */
	if (*dumpSections == DUMP_UNSECTIONED)
		*dumpSections = 0;

	if (strcmp(arg, "pre-data") == 0)
		*dumpSections |= DUMP_PRE_DATA;
	else if (strcmp(arg, "data") == 0)
		*dumpSections |= DUMP_DATA;
	else if (strcmp(arg, "post-data") == 0)
		*dumpSections |= DUMP_POST_DATA;
	else
	{
		pg_log_error("unrecognized section name: \"%s\"", arg);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}
}

/*
 * Code for responding to cancel interrupts (SIGINT, control-C, etc)
 *
 * When we get a cancel interrupt, we could just die, but in pg_restore that
 * could leave a SQL command (e.g., CREATE INDEX on a large table) running
 * for a long time.  Instead, we try to send a cancel request and then die.
 * pg_dump probably doesn't really need this, but we might as well use it
 * there too.
 *
 * This runs synchronously and serially on a thread, so it is not subject to
 * the usual async-signal-safety requirements.  The cancel_set API is
 * thread-safe.
 */
static void
quit_handler(int signo)
{
	fprintf(stderr, "%s: terminated by user\n", progname);

	/* Prevent cancellation errors from being reported by worker threads. */
	set_cancel_in_progress();

	/* Send cancel on all connections. */
	cancel_set_cancel_all(quit_handler_connections);

	_exit(1);
}

/*
 * Initialize SIGINT, SIGQUIT/SIGBREAK, SIGTERM handlers.
 */
void
quit_handler_setup(void)
{
	if (!(quit_handler_connections = cancel_set_alloc()))
		pg_fatal("out of memory");

	/* Handle ^C etc in serial signal processing thread. */
	pg_signal_processor_set_serial_handler(SIGINT, quit_handler);
	pg_signal_processor_set_serial_handler(SIGTERM, quit_handler);
#ifndef WIN32
	pg_signal_processor_set_serial_handler(SIGQUIT, quit_handler);
#else
	pg_signal_processor_set_serial_handler(SIGBREAK, quit_handler);
#endif

	if (pg_signal_processor_start() != 0)
		pg_fatal("%s() failed: %m", "pg_signal_processor_start");
}

void
quit_handler_add_connection(PGconn *connection)
{
	if (!cancel_set_add(quit_handler_connections, connection))
		pg_fatal("out of memory");
}

void
quit_handler_remove_connection(PGconn *connection)
{
	cancel_set_remove(quit_handler_connections, connection);
}

void
quit_handler_cancel_and_remove_connection(PGconn *connection)
{
	cancel_set_cancel_and_remove(quit_handler_connections, connection);
}

void
quit_handler_cancel_and_remove_all_connections(void)
{
	cancel_set_cancel_and_remove_all(quit_handler_connections);
}

/* Register a callback to be run when exit_nicely is invoked. */
void
on_exit_nicely(on_exit_nicely_callback function, void *arg)
{
	if (on_exit_nicely_index >= MAX_ON_EXIT_NICELY)
		pg_fatal("out of on_exit_nicely slots");
	on_exit_nicely_list[on_exit_nicely_index].function = function;
	on_exit_nicely_list[on_exit_nicely_index].arg = arg;
	on_exit_nicely_index++;
}

/*
 * Run accumulated on_exit_nicely callbacks in reverse order and then exit
 * without printing any message.
 *
 * If running in a parallel worker thread, we only exit the thread, not the
 * whole process.
 *
 * Note that in parallel operation, the callback(s) will be run by each thread
 * since the list state is necessarily shared by all threads; each callback
 * must contain logic to ensure it does only what's appropriate for its
 * thread.
 */
void
exit_nicely(int code)
{
	int			i;

	for (i = on_exit_nicely_index - 1; i >= 0; i--)
		on_exit_nicely_list[i].function(code,
										on_exit_nicely_list[i].arg);

	if (am_parallel_worker_thread())
		pg_thrd_exit(code);

	exit(code);
}
