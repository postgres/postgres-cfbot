/*-------------------------------------------------------------------------
 * auxprocess.c
 *	  functions related to auxiliary processes.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/auxprocess.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "access/xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/wait_event.h"


static void ShutdownAuxiliaryProcess(int code, Datum arg);


/*
 *	 AuxiliaryProcessMainCommon
 *
 *	 Common initialization code for auxiliary processes, such as the bgwriter,
 *	 walwriter, walreceiver, and the startup process.
 */
void
AuxiliaryProcessMainCommon(void)
{
	Assert(IsUnderPostmaster);

	/* Release postmaster's working memory context */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	init_ps_display(NULL);

	Assert(GetProcessingMode() == InitProcessing);

	IgnoreSystemIndexes = true;

	/*
	 * As an auxiliary process, we aren't going to do the full InitPostgres
	 * pushups, but there are a couple of things that need to get lit up even
	 * in an auxiliary process.
	 */

	/*
	 * Create a PGPROC so we can use LWLocks and access shared memory.
	 */
	InitAuxiliaryProcess();

	BaseInit();

	/*
	 * Prevent consuming interrupts between ProcSignalInit() and the
	 * initialization of local states that are kept in sync with shared memory
	 * via procsignal-based barriers. If a barrier is emitted, and absorbed,
	 * before local cached state is initialized the state transition can be
	 * invalid.
	 *
	 * These initializations intentionally happen after ProcSignalInit(),
	 * otherwise we might miss a state change. This means we may also receive
	 * a barrier for the state we've just initialized, but it can happen only
	 * once.
	 *
	 * The postmaster (which is what gets forked into the new child process)
	 * does not handle barriers, therefore its local states may not reflect
	 * the current state of the shared memory.
	 *
	 * NB: Even if the postmaster handled barriers, the value might still be
	 * stale, as it might have changed after this process forked.
	 */
	HOLD_INTERRUPTS();

	ProcSignalInit(NULL, 0);

	/*
	 * LocalDataChecksumState inherited from Postmaster will have the value
	 * read from the control file, which may be arbitrarily old. Update it.
	 */
	InitLocalDataChecksumState();

	/*
	 * Refresh per-backend protections for resizable shmem structures, in case
	 * these structures have been resized since the startup. Structures are
	 * expected to be kept in sync by respective subsystems using
	 * procsignal-based barriers. But we modify their protections en-masse
	 * here, rather than relying on individual subsystems to do it.
	 */
	ShmemReprotectResizableStructs();

	RESUME_INTERRUPTS();

	/*
	 * Initialize the process-local logical info WAL logging state.
	 *
	 * This must be called after ProcSignalInit() so that the process can
	 * participate in procsignal-based barriers that update this state.
	 */
	InitializeProcessXLogLogicalInfo();

	/*
	 * Auxiliary processes don't run transactions, but they may need a
	 * resource owner anyway to manage buffer pins acquired outside
	 * transactions (and, perhaps, other things in future).
	 */
	CreateAuxProcessResourceOwner();


	/* Initialize backend status information */
	pgstat_beinit();
	pgstat_bestart_initial();
	pgstat_bestart_final();

	/* register a before-shutdown callback for LWLock cleanup */
	before_shmem_exit(ShutdownAuxiliaryProcess, 0);

	SetProcessingMode(NormalProcessing);
}

/*
 * Begin shutdown of an auxiliary process.  This is approximately the equivalent
 * of ShutdownPostgres() in postinit.c.  We can't run transactions in an
 * auxiliary process, so most of the work of AbortTransaction() is not needed,
 * but we do need to make sure we've released any LWLocks we are holding.
 * (This is only critical during an error exit.)
 */
static void
ShutdownAuxiliaryProcess(int code, Datum arg)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
}
