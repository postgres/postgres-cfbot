/*-------------------------------------------------------------------------
 *
 * shell_archive.c
 *
 * This archiving function uses a user-specified shell command (the
 * archive_command GUC) to copy write-ahead log files.  It is used as the
 * default, but other modules may define their own custom archiving logic.
 *
 * Copyright (c) 2022-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/archive/shell_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/wait.h>
#ifndef WIN32
#include <unistd.h>
#endif

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "common/percentrepl.h"
#include "libpq/pqsignal.h"
#include "pgstat.h"
#ifndef WIN32
#include "postmaster/fork_process.h"
#endif
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/wait_event.h"

/* Keep in sync with archive_command_jobs GUC maximum. */
#define ARCHIVE_COMMAND_JOBS_MAX	8

#ifndef WIN32
static struct
{
	pid_t		pid;
	char		file[MAX_XFN_CHARS + 1];
}				shell_jobs[ARCHIVE_COMMAND_JOBS_MAX];
static int	shell_nactive = 0;
static char shell_done_file[MAX_XFN_CHARS + 1];
static bool shell_sigchld_armed = false;

static void shell_sigchld_handler(SIGNAL_ARGS);
static void shell_prepare_child(void);
static void shell_kill_job(pid_t pid);
#endif

static bool shell_archive_configured(ArchiveModuleState *state);
static bool shell_archive_file(ArchiveModuleState *state,
							   const char *file,
							   const char *path);
static void shell_archive_shutdown(ArchiveModuleState *state);
#ifndef WIN32
static bool shell_archive_submit(ArchiveModuleState *state,
								 const char *file,
								 const char *path);
static bool shell_archive_poll(ArchiveModuleState *state,
							   const char **file,
							   ArchiveResultStatus *status,
							   long timeout_ms);
static void shell_archive_cancel(ArchiveModuleState *state);
#endif

static const ArchiveModuleCallbacks shell_archive_sync_callbacks = {
	.check_configured_cb = shell_archive_configured,
	.archive_file_cb = shell_archive_file,
	.shutdown_cb = shell_archive_shutdown
};

#ifndef WIN32
static const ArchiveModuleCallbacks shell_archive_async_callbacks = {
	.check_configured_cb = shell_archive_configured,
	.shutdown_cb = shell_archive_shutdown,
	.submit_cb = shell_archive_submit,
	.poll_cb = shell_archive_poll,
	.cancel_cb = shell_archive_cancel
};
#endif

const ArchiveModuleCallbacks *
shell_archive_init(void)
{
#ifndef WIN32
	if (XLogArchiveCommandJobs > 1)
		return &shell_archive_async_callbacks;
#endif
	return &shell_archive_sync_callbacks;
}

static bool
shell_archive_configured(ArchiveModuleState *state)
{
	if (XLogArchiveCommand[0] != '\0')
		return true;

	arch_module_check_errdetail("\"%s\" is not set.",
								"archive_command");
	return false;
}

static char *
shell_build_command(const char *file, const char *path)
{
	char	   *nativePath = NULL;
	char	   *cmd;

	if (path)
	{
		nativePath = pstrdup(path);
		make_native_path(nativePath);
	}

	cmd = replace_percent_placeholders(XLogArchiveCommand,
									   "archive_command", "fp",
									   file, nativePath);
	if (nativePath)
		pfree(nativePath);
	return cmd;
}

static void
shell_report_failure(const char *cmd, int rc)
{
	int			lev = wait_result_is_any_signal(rc, true) ? FATAL : LOG;

	if (WIFEXITED(rc))
	{
		ereport(lev,
				(errmsg("archive command failed with exit code %d",
						WEXITSTATUS(rc)),
				 errdetail("The failed archive command was: %s",
						   cmd)));
	}
	else if (WIFSIGNALED(rc))
	{
#if defined(WIN32)
		ereport(lev,
				(errmsg("archive command was terminated by exception 0x%X",
						WTERMSIG(rc)),
				 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
				 errdetail("The failed archive command was: %s",
						   cmd)));
#else
		ereport(lev,
				(errmsg("archive command was terminated by signal %d: %s",
						WTERMSIG(rc), pg_strsignal(WTERMSIG(rc))),
				 errdetail("The failed archive command was: %s",
						   cmd)));
#endif
	}
	else
	{
		ereport(lev,
				(errmsg("archive command exited with unrecognized status %d",
						rc),
				 errdetail("The failed archive command was: %s",
						   cmd)));
	}
}

static bool
shell_archive_file(ArchiveModuleState *state, const char *file,
				   const char *path)
{
	char	   *cmd = shell_build_command(file, path);
	int			rc;

	ereport(DEBUG3,
			(errmsg_internal("executing archive command \"%s\"",
							 cmd)));

	fflush(NULL);
	pgstat_report_wait_start(WAIT_EVENT_ARCHIVE_COMMAND);
	rc = system(cmd);
	pgstat_report_wait_end();

	if (rc != 0)
		shell_report_failure(cmd, rc);
	else
		elog(DEBUG1, "archived write-ahead log file \"%s\"", file);

	pfree(cmd);
	return (rc == 0);
}

#ifndef WIN32
static void
shell_report_file_failure(const char *file, int rc)
{
	char		pathname[MAXPGPATH];
	char	   *cmd;

	snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", file);
	cmd = shell_build_command(file, pathname);
	shell_report_failure(cmd, rc);
	pfree(cmd);
}

static bool
shell_archive_submit(ArchiveModuleState *state, const char *file,
					 const char *path)
{
	char	   *cmd;
	pid_t		pid;

	if (shell_nactive >= XLogArchiveCommandJobs)
		return false;

	cmd = shell_build_command(file, path);
	ereport(DEBUG3,
			(errmsg_internal("submitting archive command \"%s\"",
							 cmd)));

	pid = fork_process();
	if (pid == 0)
	{
		if (setpgid(0, 0) < 0)
			_exit(1);
		shell_prepare_child();
		execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
		_exit(127);
	}
	if (pid < 0)
	{
		ereport(LOG,
				(errmsg("could not fork archive command: %m"),
				 errdetail("The failed archive command was: %s",
						   cmd)));
		pfree(cmd);
		return false;
	}

	/* EACCES means the child already set its process group. */
	if (setpgid(pid, pid) < 0 && errno != EACCES)
		ereport(LOG,
				(errmsg("could not set process group for archive command: %m")));

	shell_jobs[shell_nactive].pid = pid;
	strlcpy(shell_jobs[shell_nactive].file, file,
			sizeof(shell_jobs[shell_nactive].file));
	shell_nactive++;
	pfree(cmd);
	return true;
}

static bool
shell_try_reap(const char **file, ArchiveResultStatus *status)
{
	int			i;

	for (i = 0; i < shell_nactive; i++)
	{
		int			waitstatus;
		pid_t		done;

		for (;;)
		{
			done = waitpid(shell_jobs[i].pid, &waitstatus, WNOHANG);
			if (!(done < 0 && errno == EINTR))
				break;
		}

		if (done == 0)
			continue;

		strlcpy(shell_done_file, shell_jobs[i].file, sizeof(shell_done_file));
		*file = shell_done_file;
		shell_jobs[i] = shell_jobs[--shell_nactive];

		if (done < 0)
		{
			ereport(LOG,
					(errmsg("could not wait for archive command: %m"),
					 errdetail("While waiting for write-ahead log file \"%s\"",
							   shell_done_file)));
			*status = ARCHIVE_RESULT_RETRY;
		}
		else if (waitstatus == 0)
		{
			*status = ARCHIVE_RESULT_SUCCESS;
			elog(DEBUG1, "archived write-ahead log file \"%s\"",
				 shell_done_file);
		}
		else
		{
			if (wait_result_is_any_signal(waitstatus, true))
				shell_archive_cancel(NULL);
			shell_report_file_failure(shell_done_file, waitstatus);
			*status = ARCHIVE_RESULT_RETRY;
		}
		return true;
	}

	return false;
}

static void
shell_sigchld_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	SetLatch(MyLatch);
	errno = save_errno;
}

static void
shell_prepare_child(void)
{
	pqsignal(SIGHUP, PG_SIG_DFL);
	pqsignal(SIGINT, PG_SIG_DFL);
	pqsignal(SIGTERM, PG_SIG_DFL);
	pqsignal(SIGQUIT, PG_SIG_DFL);
	pqsignal(SIGALRM, PG_SIG_DFL);
	pqsignal(SIGPIPE, PG_SIG_DFL);
	pqsignal(SIGUSR1, PG_SIG_DFL);
	pqsignal(SIGUSR2, PG_SIG_DFL);
	pqsignal(SIGCHLD, PG_SIG_DFL);
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
}

static void
shell_kill_job(pid_t pid)
{
	int			status;

	if (kill(-pid, SIGKILL) < 0 && errno != ESRCH)
		kill(pid, SIGKILL);

	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
}

static bool
shell_archive_poll(ArchiveModuleState *state, const char **file,
				   ArchiveResultStatus *status, long timeout_ms)
{
	if (shell_try_reap(file, status))
		return true;
	if (timeout_ms <= 0 || shell_nactive <= 0)
		return false;

	if (!shell_sigchld_armed)
	{
		pqsignal(SIGCHLD, shell_sigchld_handler);
		shell_sigchld_armed = true;
	}
	if (shell_try_reap(file, status))
		return true;

	(void) WaitLatch(MyLatch,
					 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					 timeout_ms,
					 WAIT_EVENT_ARCHIVE_COMMAND);
	ResetLatch(MyLatch);

	return shell_try_reap(file, status);
}

static void
shell_archive_cancel(ArchiveModuleState *state)
{
	while (shell_nactive > 0)
	{
		shell_nactive--;
		shell_kill_job(shell_jobs[shell_nactive].pid);
	}
}
#endif															/* !WIN32 */

static void
shell_archive_shutdown(ArchiveModuleState *state)
{
#ifndef WIN32
	shell_archive_cancel(state);
#endif
	elog(DEBUG1, "archiver process shutting down");
}
