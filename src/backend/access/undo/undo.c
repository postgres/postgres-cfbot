/*-------------------------------------------------------------------------
 *
 * undo.c
 *	  Common undo layer coordination
 *
 * The undo subsystem consists of several logically separate subsystems
 * that work together to achieve a common goal. The code in this file
 * provides a limited amount of common infrastructure that can be used
 * by all of those various subsystems, and helps coordinate activities
 * such as shared memory initialization and startup/shutdown.
 *
 * This file has no compile-time or link-time dependency on any specific
 * table or index access method.  Shared memory for AM-specific tracking
 * structures (such as an optional per-tuple tracking hash an AM may register)
 * is registered as its own sibling entry in storage/subsystemlist.h, not
 * sized or initialized here.  Likewise, the
 * set of UNDO resource managers to register comes from access/undormgrlist.h
 * (mirroring the subsystemlist.h idiom): a new consumer adds itself to that
 * list, and never edits this file.
 *
 * Shared memory initialization uses the PG_SHMEM_SUBSYSTEM pattern:
 * UndoShmemCallbacks is registered in subsystemlist.h, and the framework
 * calls UndoShmemRequest() and UndoShmemInit() at the appropriate times
 * during postmaster startup.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/undo.h"
#include "access/undolog.h"
#include "access/undormgr.h"
#include "access/undormgrs.h"
#include "access/undoworker.h"
#include "access/xactundo.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "utils/memutils.h"

/*
 * UndoContext is a child of TopMemoryContext which is never reset. The only
 * reason for having a separate context is to make it easier to spot leaks or
 * excessive memory utilization related to undo operations.
 */
MemoryContext UndoContext = NULL;

static void AtProcExit_Undo(int code, Datum arg);
static void UndoShmemRequest_internal(void *arg);
static void UndoShmemInit_internal(void *arg);
static void UndoShmemAttach_internal(void *arg);
static void RegisterUndoRmgrs(void);

/*
 * ShmemCallbacks for the UNDO subsystem.
 *
 * Registered via PG_SHMEM_SUBSYSTEM(UndoShmemCallbacks) in subsystemlist.h.
 *
 * init_fn initializes the contents of all UNDO shared memory structures.
 */
const ShmemCallbacks UndoShmemCallbacks = {
	.request_fn = UndoShmemRequest_internal,
	.init_fn = UndoShmemInit_internal,
	.attach_fn = UndoShmemAttach_internal,
};

/*
 * UndoShmemSize
 *		Figure out how much shared memory will be needed for undo.
 *
 * Each subsystem separately computes the space it requires, and we
 * carefully add up those values here.  AM-specific tracking structures
 * (such as an optional per-tuple tracking hash) are sized by their own
 * PG_SHMEM_SUBSYSTEM entry and are not included here.
 */
Size
UndoShmemSize(void)
{
	Size		size;

	size = UndoLogShmemSize();
	size = add_size(size, XactUndoShmemSize());
	size = add_size(size, UndoWorkerShmemSize());

	return size;
}

/*
 * UndoShmemRequest_internal
 *		Register shared memory needs for UNDO subsystems.
 *
 * Called during the request_fn phase of postmaster startup, before shared
 * memory is allocated.
 */
static void
UndoShmemRequest_internal(void *arg)
{
	/*
	 * Register the UNDO background worker.  This must happen during the
	 * request_fn phase (before BackgroundWorkerShmemInit runs in the init_fn
	 * phase), because RegisterBackgroundWorker() cannot be called after
	 * BackgroundWorkerShmemInit().
	 *
	 * Use a static flag to ensure we only register once.  The request_fn
	 * callback is called again during postmaster reinitialize (after a child
	 * crash), and RegisterBackgroundWorker() would fail if called after the
	 * first shmem init.
	 */
	{
		static bool undo_worker_registered = false;

		/*
		 * Only the postmaster can register a static background worker.  In
		 * bootstrap and single-user mode (initdb) there is no postmaster, so
		 * skip registration; RegisterBackgroundWorker() would otherwise just
		 * emit a LOG and return without registering.
		 */
		if (!undo_worker_registered &&
			!IsUnderPostmaster && IsPostmasterEnvironment)
		{
			UndoWorkerRegister();

			undo_worker_registered = true;
		}
	}

	/*
	 * Initialize the UNDO resource manager dispatch table and register the
	 * built-in resource managers listed in access/undormgrlist.h.
	 */
}

/*
 * UndoShmemInit / UndoShmemInit_internal
 *		Initialize undo-related shared memory.
 *
 * Also, perform other initialization steps that need to be done very early.
 * This is called once during postmaster startup via the ShmemCallbacks
 * framework.
 */
static void
UndoShmemInit_internal(void *arg)
{
	UndoShmemInit();
}

void
UndoShmemInit(void)
{
	/*
	 * Initialize the undo memory context. If it already exists (crash restart
	 * via reset_shared()), reset it instead.
	 */
	if (UndoContext)
		MemoryContextReset(UndoContext);
	else
		UndoContext = AllocSetContextCreate(TopMemoryContext, "Undo",
											ALLOCSET_DEFAULT_SIZES);

	/* Now give various undo subsystems a chance to initialize. */
	UndoLogShmemInit();
	XactUndoShmemInit();
	UndoWorkerShmemInit();

	/*
	 * Initialize the UNDO resource manager dispatch table and register the
	 * built-in resource managers listed in access/undormgrlist.h.
	 */
	RegisterUndoRmgrs();
}

/*
 * UndoShmemAttach_internal
 *		Re-establish per-process UNDO state in EXEC_BACKEND children.
 *
 * Under EXEC_BACKEND (Windows, or --enable-exec-backend builds) a child does
 * not inherit the postmaster's address space, so module-scope state populated
 * by UndoShmemInit() is not present and must be rebuilt here.  Two distinct
 * kinds of state need re-establishing:
 *
 * 1. The pointers into shared memory held by the legacy ShmemInitStruct-based
 *    sub-modules (UndoLogShared, UndoWorkerShmem, WorkQueue, RevertState).
 *    Re-calling each *ShmemInit() in a child is safe:
 *    ShmemInitStruct() self-attaches via AttachShmemIndexEntry() when
 *    IsUnderPostmaster, re-assigns the module global, and reports found=true so
 *    the one-time "if (!found)" initialization block is correctly skipped.
 *
 * 2. The UndoRmgrs[] dispatch table and each registered AM's undo hook
 *    function pointers, both rebuilt by RegisterUndoRmgrs() (idempotent:
 *    InitUndoRmgrs() zeroes the table first).
 *
 * XactUndoShmemInit() is a no-op and so omitted here.  Any
 * AM-specific shared structure with the same "no found-guard" hazard
 * handles its own EXEC_BACKEND re-attach via its own PG_SHMEM_SUBSYSTEM entry
 * (see storage/subsystemlist.h); this function is only responsible for the
 * generic UNDO sub-modules listed above.
 */
static void
UndoShmemAttach_internal(void *arg)
{
	UndoLogShmemInit();
	UndoWorkerShmemInit();

	RegisterUndoRmgrs();
}

/*
 * RegisterUndoRmgrs
 *		Initialize the UNDO resource manager dispatch table and register the
 *		built-in resource managers listed in access/undormgrlist.h.
 *
 * Called from both UndoShmemInit() (postmaster/standalone) and
 * UndoShmemAttach_internal() (EXEC_BACKEND children).  Idempotent:
 * InitUndoRmgrs() zeroes the dispatch table before the *UndoRmgrInit() calls
 * re-register, so RegisterUndoRmgr()'s double-registration guard is not
 * tripped on a second invocation in the same process.
 *
 * This function has no compile-time knowledge of which resource managers
 * exist: it just expands access/undormgrlist.h through the UNDO_RMGR_INIT
 * macro.  A new UNDO-writing consumer (a new index AM, table AM, or other
 * subsystem) adds one line to that list; this function and the rest of
 * undo.c are never modified.
 */
static void
RegisterUndoRmgrs(void)
{
	/*
	 * Initialize the UNDO resource manager dispatch table.
	 */
	InitUndoRmgrs();

	/*
	 * Register every built-in resource manager listed in
	 * access/undormgrlist.h.
	 */
#define UNDO_RMGR_INIT(initfunc) \
	initfunc();
#include "access/undormgrlist.h"
#undef UNDO_RMGR_INIT
}

/*
 * InitializeUndo
 *		Per-backend initialization for the undo subsystem.
 *
 * Called once per backend from InitPostgres().
 */
void
InitializeUndo(void)
{
	InitializeXactUndo();

	on_shmem_exit(AtProcExit_Undo, 0);
}

/*
 * AtProcExit_Undo
 *		Shut down undo subsystems in the correct order.
 *
 * Higher-level stuff should be shut down first.
 */
static void
AtProcExit_Undo(int code, Datum arg)
{
	AtProcExit_XactUndo();
}
