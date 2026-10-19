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
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/subsystems.h"
#include "utils/backend_status.h"
#include "utils/injection_point.h"

typedef int64 LogQueryPlanRequestId;

typedef struct LogQueryPlanRequestIdControl
{
	slock_t		mutex;
	LogQueryPlanRequestId next_request_id;
} LogQueryPlanRequestIdControl;

typedef enum LogQueryPlanRequestState
{
	LOG_QUERY_PLAN_REQUEST_IDLE = 0,
	LOG_QUERY_PLAN_REQUEST_PENDING,	/* Request registered but not yet taken by target backend */
	LOG_QUERY_PLAN_REQUEST_PROCESSING,	/* Request taken by target backend; processing in progress */
} LogQueryPlanRequestState;

typedef struct LogQueryPlanRequest
{
	LogQueryPlanRequestId request_id;
	pid_t		target_pid;
	/* Future versions may add fields here to carry EXPLAIN options. */
} LogQueryPlanRequest;

typedef struct LogQueryPlanRequestSlot
{
	slock_t		mutex;
	LogQueryPlanRequestState state;
	LogQueryPlanRequest request;
} LogQueryPlanRequestSlot;

static LogQueryPlanRequestSlot *LogQueryPlanRequestSlots;
static LogQueryPlanRequestIdControl *LogQueryPlanRequestIdCtl;

static void LogQueryPlanShmemRequest(void *arg);
static void LogQueryPlanShmemInit(void *arg);
static void LogQueryPlanRequestSlotCleanup(int code, Datum arg);

const ShmemCallbacks LogQueryPlanShmemCallbacks = {
	.request_fn = LogQueryPlanShmemRequest,
	.init_fn = LogQueryPlanShmemInit,
};

static bool wrapExecProcNode(PlanState *ps, void *context);
static LogQueryPlanRequestId GenerateLogQueryPlanRequestId(void);
static bool ReserveLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
									   LogQueryPlanRequestId request_id);
static void RollbackLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
										LogQueryPlanRequestId request_id);
static LogQueryPlanRequest *TakePendingLogQueryPlanRequest(void);
static LogQueryPlanRequest *GetProcessingLogQueryPlanRequest(void);
static void FinishLogQueryPlanRequest(void);

static void
LogQueryPlanShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "Log Query Plan Request ID",
					   .size = sizeof(LogQueryPlanRequestIdControl),
					   .ptr = (void **) &LogQueryPlanRequestIdCtl,
		);

	ShmemRequestStruct(.name = "Log Query Plan Request Slots",
					   .size = mul_size(sizeof(LogQueryPlanRequestSlot), MaxBackends),
					   .ptr = (void **) &LogQueryPlanRequestSlots,
		);
}

static void
LogQueryPlanShmemInit(void *arg)
{
	SpinLockInit(&LogQueryPlanRequestIdCtl->mutex);
	LogQueryPlanRequestIdCtl->next_request_id = 1;

	for (int i = 0; i < MaxBackends; i++)
	{
		LogQueryPlanRequestSlot *slot = &LogQueryPlanRequestSlots[i];

		SpinLockInit(&slot->mutex);
		slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	}
}

/*
 * Initialize the request slot assigned to a newly-started process.
 */
void
LogQueryPlanRequestSlotInit(void)
{
	LogQueryPlanRequestSlot *slot;

	Assert(MyBackendType == B_BACKEND);
	slot = &LogQueryPlanRequestSlots[MyProcNumber];

	SpinLockAcquire(&slot->mutex);
	slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	SpinLockRelease(&slot->mutex);

	on_shmem_exit(LogQueryPlanRequestSlotCleanup, Int32GetDatum(MyProcNumber));
}

/*
 * Release a request slot when its process exits.
 */
static void
LogQueryPlanRequestSlotCleanup(int code, Datum arg)
{
	ProcNumber	proc_number = DatumGetInt32(arg);
	LogQueryPlanRequestSlot *slot;

	Assert(proc_number >= 0 && proc_number < MaxBackends);
	slot = &LogQueryPlanRequestSlots[proc_number];

	SpinLockAcquire(&slot->mutex);
	slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	SpinLockRelease(&slot->mutex);
}

/*
 * Generate an opaque request identifier from a shared counter.
 *
 * Keep identifiers positive and representable as SQL bigint, wrapping to 1
 * after PG_INT64_MAX.  Failed requests consume identifiers, too.
 */
static LogQueryPlanRequestId
GenerateLogQueryPlanRequestId(void)
{
	LogQueryPlanRequestId id;

	SpinLockAcquire(&LogQueryPlanRequestIdCtl->mutex);
	id = LogQueryPlanRequestIdCtl->next_request_id;
	LogQueryPlanRequestIdCtl->next_request_id =
		(id == PG_INT64_MAX) ? 1 : id + 1;
	SpinLockRelease(&LogQueryPlanRequestIdCtl->mutex);

	return id;
}

/*
 * Register a pending request in the target process's slot if it is idle.
 * Return true on success, or false if a request is already pending or being
 * processed.
 */
static bool
ReserveLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
						   LogQueryPlanRequestId request_id)
{
	LogQueryPlanRequestSlot *slot;
	bool		reserved = false;

	Assert(proc_number >= 0 && proc_number < MaxBackends);
	slot = &LogQueryPlanRequestSlots[proc_number];

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_IDLE)
	{
		slot->request.request_id = request_id;
		slot->request.target_pid = pid;
		slot->state = LOG_QUERY_PLAN_REQUEST_PENDING;
		reserved = true;
	}
	SpinLockRelease(&slot->mutex);

	return reserved;
}

/*
 * Undo a reservation after signal delivery fails, but only if the slot still
 * contains the same pending request.  Leave requests already taken by the
 * target process or registered by another caller untouched.
 */
static void
RollbackLogQueryPlanRequest(ProcNumber proc_number, pid_t pid,
							LogQueryPlanRequestId request_id)
{
	LogQueryPlanRequestSlot *slot;

	Assert(proc_number >= 0 && proc_number < MaxBackends);
	slot = &LogQueryPlanRequestSlots[proc_number];

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_PENDING &&
		slot->request.target_pid == pid &&
		slot->request.request_id == request_id)
		slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	SpinLockRelease(&slot->mutex);
}

/*
 * Take a pending request and set its state to PROCESSING, or return NULL if
 * none exists.
 */
static LogQueryPlanRequest *
TakePendingLogQueryPlanRequest(void)
{
	LogQueryPlanRequestSlot *slot;
	LogQueryPlanRequest *request = NULL;

	slot = &LogQueryPlanRequestSlots[MyProcNumber];

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_PENDING &&
		slot->request.target_pid == MyProcPid)
	{
		slot->state = LOG_QUERY_PLAN_REQUEST_PROCESSING;
		request = &slot->request;
	}
	else
		/* Clear under the lock so a new request's notification is not lost. */
		LogQueryPlanPending = false;
	SpinLockRelease(&slot->mutex);

	return request;
}

/*
 * Return the processing request, or return NULL if none exists.
 */
static LogQueryPlanRequest *
GetProcessingLogQueryPlanRequest(void)
{
	LogQueryPlanRequestSlot *slot = &LogQueryPlanRequestSlots[MyProcNumber];
	LogQueryPlanRequest *request = NULL;

	SpinLockAcquire(&slot->mutex);
	if (slot->state == LOG_QUERY_PLAN_REQUEST_PROCESSING)
		request = &slot->request;
	SpinLockRelease(&slot->mutex);

	return request;
}

static void
FinishLogQueryPlanRequest(void)
{
	LogQueryPlanRequestSlot *slot = &LogQueryPlanRequestSlots[MyProcNumber];

	SpinLockAcquire(&slot->mutex);
	/* Clear the notification before allowing another request to be registered. */
	LogQueryPlanPending = false;
	slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	SpinLockRelease(&slot->mutex);
}

/*
 * Discard any unfinished request when a transaction or subtransaction aborts.
 * Do not allocate or report errors here: the original error must be preserved.
 */
void
AbortLogQueryPlanRequest(void)
{
	/* Only client backends use query plan logging request slots. */
	if (MyBackendType != B_BACKEND)
		return;

	FinishLogQueryPlanRequest();
}

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
	LogQueryPlanRequest *request;

	/*
	 * Current QueryDesc is valid only during standard_ExecutorRun. However,
	 * ExecProcNode can be called afterward (i.e., ExecPostprocessPlan). To
	 * handle the case, check whether we have QueryDesc now.
	 */
	queryDesc = GetCurrentQueryDesc();

	if (queryDesc == NULL)
	{
		FinishLogQueryPlanRequestWithoutPlan(0);
		return;
	}

	request = GetProcessingLogQueryPlanRequest();

	if (request == NULL)
		return;

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
				errmsg("query and its plan for queryid " INT64_FORMAT " requested by request ID " INT64_FORMAT " running on backend with PID %d are:\n%s",
					   queryDesc->plannedstmt->queryId,
					   request->request_id, MyProcPid,
					   es->str->data));
	}
	PG_FINALLY();
	{
		MemoryContextSwitchTo(old_cxt);
		MemoryContextDelete(cxt);
		FinishLogQueryPlanRequest();
	}
	PG_END_TRY();
}

/*
 * Finish a request without logging its plan, and log the queryid and requestid if possible.
 */
void
FinishLogQueryPlanRequestWithoutPlan(uint64 query_id)
{
	LogQueryPlanRequestSlot *slot = &LogQueryPlanRequestSlots[MyProcNumber];
	LogQueryPlanRequestId request_id = 0;

	SpinLockAcquire(&slot->mutex);
	request_id = slot->request.request_id;
	/* Clear the notification before allowing another request to be registered. */
	LogQueryPlanPending = false;
	slot->state = LOG_QUERY_PLAN_REQUEST_IDLE;
	SpinLockRelease(&slot->mutex);

	// request_id should have been set when the request was made.
	Assert(request_id != 0);

	if (query_id != 0)
		ereport(LOG_SERVER_ONLY,
				errmsg("query plan logging request " INT64_FORMAT " could not be completed because there was no opportunity to log the plan for queryid " INT64_FORMAT,
					   request_id, query_id));
	else
		ereport(LOG_SERVER_ONLY,
				errmsg("query plan logging request " INT64_FORMAT " could not be completed because there was no opportunity to log the plan",
					   request_id));
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
	QueryDesc  *querydesc;

	/* Exit immediately if processing a request is already in progress. */
	if (GetProcessingLogQueryPlanRequest() != NULL)
		return;

	/* Exit immediately when there is no pending request for this backend. */
	if (TakePendingLogQueryPlanRequest() == NULL)
		return;

	querydesc = GetCurrentQueryDesc();

	/* If current query has already finished, we can do nothing but exit */
	if (querydesc == NULL)
	{
		FinishLogQueryPlanRequestWithoutPlan(0);
		return;
	}

	PG_TRY();
	{
		INJECTION_POINT("log-query-interrupt", NULL);

		/*
		 * Wrap ExecProcNodes with ExecProcNodeFirst, which logs query plan
		 * when LogQueryPlanPending is true.
		 */
		(void) wrapExecProcNode(querydesc->planstate, NULL);
	}
	PG_CATCH();
	{
		FinishLogQueryPlanRequest();
		PG_RE_THROW();
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
	ProcNumber	procNumber;
	PgBackendStatus *be_status = NULL;
	LogQueryPlanRequestId request_id;

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
		PG_RETURN_NULL();
	}

	if (be_status->st_backendType != B_BACKEND)
	{
		ereport(WARNING,
				(errmsg("PID %d is not a PostgreSQL client backend process", pid)));
		PG_RETURN_NULL();
	}

	procNumber = GetNumberFromPGProc(proc);
	request_id = GenerateLogQueryPlanRequestId();

	if (!ReserveLogQueryPlanRequest(procNumber, pid, request_id))
	{
		ereport(WARNING,
				(errmsg("query plan request for PID %d is already pending", pid)));
		PG_RETURN_NULL();
	}

	if (SendProcSignal(pid, PROCSIG_LOG_QUERY_PLAN, procNumber) < 0)
	{
		/* The signal was not delivered, so undo our reservation if possible. */
		RollbackLogQueryPlanRequest(procNumber, pid, request_id);
		ereport(WARNING,
				(errmsg("could not send signal to process %d: %m", pid)));
		PG_RETURN_NULL();
	}

	PG_RETURN_INT64(request_id);
}
