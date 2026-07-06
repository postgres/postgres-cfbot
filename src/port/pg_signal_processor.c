/*-------------------------------------------------------------------------
 *
 * pg_signal_processor.c
 *    Mechanism for handling OS signals serially in a thread.
 *
 * This is primarily intended to handle SIGINT (^C) and SIGQUIT in frontend
 * programs, to perform cleanup work before exiting.  Handlers must be
 * thread-safe, but do not need to be reentrant or async-signal-safe.
 *
 * On Windows, only SIGINT and SIGBREAK are supported, and they are mapped to
 * CTRL_C_EVENT and CTRL_BREAK_EVENT console events.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    src/port/pg_signal_processor.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <signal.h>
#include <stdio.h>

#include "port/pg_signal_processor.h"
#include "port/pg_threads_ext.h"

#ifndef WIN32
/* Steal one signal to use for stopping the helper thread. */
#define PG_SIGNAL_PROCESSOR_EOF_SIGNAL SIGXCPU
#endif

#ifndef NSIG
#define NSIG 64
#endif

static pg_signal_processor_handler serial_handlers[NSIG];

#ifdef WIN32
static pg_mtx_t serial_handler_mutex = PG_MTX_PLAIN_INIT;
#else
static pg_thrd_t serial_handler_thread;
#endif

#ifdef WIN32
/* Windows: serialized execution using Windows' event thread pool. */
static BOOL WINAPI
pg_signal_processor_console_ctrl_handler(DWORD control_type)
{
	pg_signal_processor_handler handler;
	int			signo = -1;

	/* CTRL_XXX_EVENT -> SIGXXX mapping matches Windows' own signal(). */
	if (control_type == CTRL_C_EVENT)
		signo = SIGINT;
	else if (control_type == CTRL_BREAK_EVENT)
		signo = SIGBREAK;

	/* If we have a handler, serialize execution. */
	if (signo != -1 && (handler = serial_handlers[signo]))
	{
		pg_mtx_lock(&serial_handler_mutex);
		handler(signo);
		pg_mtx_unlock(&serial_handler_mutex);

		/*
		 * If we made it out of the handler (ie it didn't choose to call
		 * _exit()), then don't allow further processing of this event, to
		 * avoid reaching Windows' default process-exit behavior.
		 */
		return true;
	}

	return false;
}
#endif

#ifndef WIN32
/* Unix: consume signals synchronously in a dispatch loop. */
static int
pg_signal_processor_loop(void *data)
{
	sigset_t	wait_signals;

	sigemptyset(&wait_signals);
	sigaddset(&wait_signals, PG_SIGNAL_PROCESSOR_EOF_SIGNAL);
	for (int i = 0; i < lengthof(serial_handlers); ++i)
		if (serial_handlers[i])
			sigaddset(&wait_signals, i);

	for (;;)
	{
		int			signo;

		errno = sigwait(&wait_signals, &signo);
		if (errno != 0)
		{
			fprintf(stderr,
					"pg_signal_processor_loop: sigwait() failed: %s\n",
					strerror(errno));
			break;
		}

		if (signo == PG_SIGNAL_PROCESSOR_EOF_SIGNAL)
			break;

		serial_handlers[signo] (signo);
	}

	return 0;
}

static void
dummy_handler(int signo)
{
	pg_unreachable();
}
#endif

/*
 * Install handler for a given signal.  This must be called to set up the
 * handler table *before* the program becomes multithreaded on Unix, at must
 * be called at least once before pg_signal_processor_start().
 */
void
pg_signal_processor_set_serial_handler(int signo,
									   pg_signal_processor_handler handler)
{
#ifndef WIN32
	sigset_t	block_mask;

	/* Can't use our reserved EOF signal number. */
	Assert(signo != PG_SIGNAL_PROCESSOR_EOF_SIGNAL);

	/* Block from normal delivery, and the same for our EOF signal. */
	sigemptyset(&block_mask);
	sigaddset(&block_mask, PG_SIGNAL_PROCESSOR_EOF_SIGNAL);
	sigaddset(&block_mask, signo);
	sigprocmask(SIG_BLOCK, &block_mask, NULL);

	/*
	 * Install a dummy handler.  This prevents some OSes from discarding
	 * blocked SIG_IGN signals that we plan to read with sigwait().
	 */
	signal(PG_SIGNAL_PROCESSOR_EOF_SIGNAL, dummy_handler);
	signal(signo, dummy_handler);
#endif

	Assert(signo >= 0 && signo < NSIG);
	Assert(handler);

	serial_handlers[signo] = handler;
}

/* Start serial signal processing. */
int
pg_signal_processor_start(void)
{
#ifdef WIN32
	/* Windows: install control handler to be called by Windows' threads. */
	if (!SetConsoleCtrlHandler(pg_signal_processor_console_ctrl_handler, true))
	{
		fprintf(stderr, "SetConsoleCtrlHandler() failed, Windows error %u\n",
				(unsigned) GetLastError());
		return -1;
	}
#else
	int error;

	/* Unix: start serial execution thread. */
	if ((error = pg_thrd_create(&serial_handler_thread,
								pg_signal_processor_loop,
								NULL)) != pg_thrd_success)
	{
		fprintf(stderr, "could not start thread: %s\n",
				pg_thrd_error_string_with_detail(error));
		return -1;
	}
#endif

	return 0;
}

/* Stop serial signal processing. */
void
pg_signal_processor_stop(void)
{
#ifdef WIN32
	/* Windows: uninstall control handler function. */
	SetConsoleCtrlHandler(pg_signal_processor_console_ctrl_handler, false);
#else
	int error;

	/* Unix: tell helper thread to exit, and wait for it to return. */
	raise(PG_SIGNAL_PROCESSOR_EOF_SIGNAL);
	if ((error = pg_thrd_join(serial_handler_thread, NULL)) != pg_thrd_success)
		fprintf(stderr, "pg_signal_processor_stop: could not join thread: %s\n",
				pg_thrd_error_string_with_detail(error));
#endif
}
