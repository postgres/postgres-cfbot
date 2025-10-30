/*-------------------------------------------------------------------------
 *
 * pg_job_object.c
 *	  Windows Job Object support for preventing orphaned backends
 *
 * On Unix, backends can detect when the postmaster dies via getppid().
 * Windows has no equivalent mechanism. We solve this by using Job Objects,
 * a Windows kernel feature that groups processes and can automatically
 * terminate all members when the job handle closes.
 *
 * By configuring JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, we ensure that if
 * the postmaster exits (cleanly or via crash), Windows immediately kills
 * all backends. This prevents orphaned processes that hold locks and
 * prevent clean restart.
 *
 * The job object handle is stored in a static variable and never explicitly
 * closed. This is intentional - we rely on Windows closing it automatically
 * when the postmaster process exits, which triggers the child termination.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/pg_job_object.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef WIN32

#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/pg_job_object.h"

static HANDLE pg_job_object = NULL;


/*
 * pg_create_job_object
 *
 * Create job object for this PostgreSQL instance and configure it to
 * kill all children when the postmaster exits.
 *
 * Failure is not fatal - we log a warning and continue. PostgreSQL will
 * run without orphan protection, which is no worse than current behavior.
 */
void
pg_create_job_object(void)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit_info;
	char		job_name[128];
	DWORD		error;

	snprintf(job_name, sizeof(job_name), "PostgreSQL_Port_%d_PID_%lu",
			 PostPortNumber, GetCurrentProcessId());

	pg_job_object = CreateJobObjectA(NULL, job_name);

	if (pg_job_object == NULL)
	{
		error = GetLastError();
		ereport(LOG,
				(errmsg("could not create job object \"%s\": error code %lu",
						job_name, error),
				 errdetail("Orphaned process cleanup will not be available.")));
		return;
	}

	elog(DEBUG1, "created job object \"%s\"", job_name);

	/*
	 * Set KILL_ON_JOB_CLOSE. When the job handle closes (either explicit
	 * close or process termination), all processes in the job are terminated.
	 *
	 * This is the critical flag that prevents orphaned backends.
	 */
	memset(&limit_info, 0, sizeof(limit_info));
	limit_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

	if (!SetInformationJobObject(pg_job_object,
								  JobObjectExtendedLimitInformation,
								  &limit_info,
								  sizeof(limit_info)))
	{
		error = GetLastError();
		ereport(WARNING,
				(errmsg("could not configure job object: error code %lu", error),
				 errdetail("Job object created but KILL_ON_JOB_CLOSE not set."),
				 errhint("Orphaned processes may occur if postmaster crashes.")));
		CloseHandle(pg_job_object);
		pg_job_object = NULL;
		return;
	}

	if (!AssignProcessToJobObject(pg_job_object, GetCurrentProcess()))
	{
		error = GetLastError();

		/*
		 * ERROR_ACCESS_DENIED means we're already in a job. This can happen
		 * when PostgreSQL runs under a job-aware supervisor (Windows service
		 * on older Windows, or any process manager using nested jobs).
		 *
		 * On Windows 8+, we could use nested jobs, but for simplicity we
		 * just skip job creation. The parent job should handle cleanup.
		 */
		if (error == ERROR_ACCESS_DENIED)
		{
			ereport(LOG,
					(errmsg("postmaster is already in a job object"),
					 errdetail("This can occur when PostgreSQL is run under a job-aware supervisor."),
					 errhint("Automatic orphan cleanup will not be available.")));
		}
		else
		{
			ereport(WARNING,
					(errmsg("could not assign postmaster to job object: error code %lu", error)));
		}

		CloseHandle(pg_job_object);
		pg_job_object = NULL;
		return;
	}

	elog(LOG, "PostgreSQL job object configured successfully - orphaned process prevention enabled");
}


/*
 * pg_destroy_job_object
 *
 * Explicitly close the job object handle. This will trigger KILL_ON_JOB_CLOSE,
 * terminating all backends.
 *
 * Note: In most cases we don't call this - we rely on Windows closing the
 * handle automatically when the postmaster exits. Explicit close is only
 * needed if we want to control the exact timing of backend termination.
 */
void
pg_destroy_job_object(void)
{
	if (pg_job_object != NULL)
	{
		elog(DEBUG1, "closing job object - all child processes will terminate");
		CloseHandle(pg_job_object);
		pg_job_object = NULL;
	}
}


/*
 * pg_is_in_job_object
 *
 * Check if current process is in the PostgreSQL job object.
 * Used primarily for testing and verification.
 */
bool
pg_is_in_job_object(void)
{
	BOOL		in_job = FALSE;

	if (pg_job_object == NULL)
		return false;

	if (!IsProcessInJob(GetCurrentProcess(), pg_job_object, &in_job))
	{
		elog(DEBUG1, "IsProcessInJob failed: error code %lu", GetLastError());
		return false;
	}

	return (bool) in_job;
}

#endif							/* WIN32 */
