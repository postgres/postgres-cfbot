/*-------------------------------------------------------------------------
 *
 * explain_running.c
 *	  Explain query plans during execution
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain_running.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_running.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/backend_status.h"
#include "utils/injection_point.h"

/* Is plan node wrapping for query plan logging currently in progress? */
static bool WrapNodesInProgress = false;

static bool wrapExecProcNode(PlanState *ps, void *context);

/*
 * Wrap all the underlying ExecProcNode with ExecProcNodeFirst.
 */
static bool
wrapExecProcNode(PlanState *ps, void *context)
{
	if (ps == NULL)
		return false;

	ExecSetExecProcNode(ps, ps->ExecProcNodeReal);

	return planstate_tree_walker(ps, wrapExecProcNode, context);
}

/*
 * Handle receipt of an interrupt indicating logging the plan of the currently
 * running query.
 *
 * All the actual work is deferred to ProcessLogQueryPlanInterrupt(),
 * because we cannot safely emit a log message inside the signal handler.
 */
void
HandleLogQueryPlanInterrupt(void)
{
	InterruptPending = true;
	LogQueryPlanPending = true;
	/* latch will be set by procsignal_sigusr1_handler */
}

/*
 * Actual plan logging function.
 */
void
LogQueryPlan(void)
{
	ExplainState *es;
	MemoryContext cxt;
	MemoryContext old_cxt;
	QueryDesc  *queryDesc;

	/*
	 * Current QueryDesc is valid only during standard_ExecutorRun. However,
	 * ExecProcNode can be called afterward (i.e., ExecPostprocessPlan). To
	 * handle the case, check whether we have QueryDesc now.
	 */
	queryDesc = GetCurrentQueryDesc();

	if (queryDesc == NULL)
	{
		ereport(LOG_SERVER_ONLY,
				errmsg("query plan logging was requested but there was no opportunity to do it"));
		LogQueryPlanPending = false;
		return;
	}

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"log_query_plan temporary context",
								ALLOCSET_DEFAULT_SIZES);
	old_cxt = MemoryContextSwitchTo(cxt);

	es = NewExplainState();
	es->running = true;

	PG_TRY();
	{
		ExplainStringAssemble(es, queryDesc, es->format, false, -1);

		ereport(LOG_SERVER_ONLY,
				errmsg("query and its plan for queryid " INT64_FORMAT " running on backend with PID %d are:\n%s",
					   queryDesc->plannedstmt->queryId, MyProcPid,
					   es->str->data));
	}
	PG_FINALLY();
	{
		MemoryContextSwitchTo(old_cxt);
		MemoryContextDelete(cxt);
		LogQueryPlanPending = false;
	}
	PG_END_TRY();
}

/*
 * Process the request for logging query plan at CHECK_FOR_INTERRUPTS().
 *
 * Since executing EXPLAIN-related code at an arbitrary CHECK_FOR_INTERRUPTS()
 * point is potentially unsafe, this function just wraps the nodes of
 * ExecProcNode with ExecProcNodeFirst, which logs query plan if requested.
 * This way ensures that EXPLAIN-related code is executed only during
 * ExecProcNodeFirst, where it is considered safe.
 */
void
ProcessLogQueryPlanInterrupt(void)
{
	QueryDesc  *querydesc = GetCurrentQueryDesc();

	/* If current query has already finished, we can do nothing but exit */
	if (querydesc == NULL)
	{
		ereport(LOG_SERVER_ONLY,
				errmsg("query plan logging was requested but there was no opportunity to do it"));
		LogQueryPlanPending = false;
		return;
	}

	/*
	 * Exit immediately if wrapping plan is already in progress. This prevents
	 * recursive calls, which could occur if logging is requested repeatedly
	 * and rapidly, potentially leading to infinite recursion and crash.
	 */
	if (WrapNodesInProgress)
		return;

	WrapNodesInProgress = true;

	PG_TRY();
	{
		INJECTION_POINT("log-query-interrupt", NULL);

		/*
		 * Wrap ExecProcNodes with ExecProcNodeFirst, which logs query plan
		 * when LogQueryPlanPending is true.
		 */
		(void) wrapExecProcNode(querydesc->planstate, NULL);
	}
	PG_FINALLY();
	{
		WrapNodesInProgress = false;
	}
	PG_END_TRY();
}

/*
 * Signal a backend process to log the query plan of the running query.
 *
 * By default, only superusers are allowed to signal a backend to log its
 * plan because the output is written to the server log, which in many cases
 * can only be read by superusers.
 * Additional roles can be permitted with GRANT.
 */
Datum
pg_log_query_plan(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	PGPROC	   *proc;
	PgBackendStatus *be_status = NULL;

	proc = BackendPidGetProc(pid);

	if (proc != NULL)
		be_status = pgstat_get_beentry_by_proc_number(proc->vxid.procNumber);

	if (proc == NULL || be_status == NULL || be_status->st_procpid != pid)
	{
		/*
		 * This is just a warning so a loop-through-resultset will not abort
		 * if one backend terminated on its own during the run.
		 */
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL backend process", pid)));
		PG_RETURN_BOOL(false);
	}

	if (be_status->st_backendType != B_BACKEND)
	{
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL client backend process", pid)));
		PG_RETURN_BOOL(false);
	}

	if (SendProcSignal(pid, PROCSIG_LOG_QUERY_PLAN, proc->vxid.procNumber) < 0)
	{
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}
