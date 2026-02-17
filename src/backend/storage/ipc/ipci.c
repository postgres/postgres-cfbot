/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/ipci.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "storage/shmem_internal.h"
#include "storage/subsystems.h"
#include "utils/guc.h"

/* GUCs */
int			shared_memory_type = DEFAULT_SHARED_MEMORY_TYPE;

shmem_startup_hook_type shmem_startup_hook = NULL;

static Size total_addin_request = 0;

/*
 * RequestAddinShmemSpace
 *		Request that extra shmem space be allocated for use by
 *		a loadable module.
 *
 * This may only be called via the shmem_request_hook of a library that is
 * loaded into the postmaster via shared_preload_libraries.  Calls from
 * elsewhere will fail.
 */
void
RequestAddinShmemSpace(Size size)
{
	if (!process_shmem_requests_in_progress)
		elog(FATAL, "cannot request additional shared memory outside shmem_request_hook");
	total_addin_request = add_size(total_addin_request, size);
}

/*
 * CalculateShmemSize
 *		Calculates the amount of shared memory needed.
 *
 *	- `initial` is the amount of memory needed when the server startup.
 *  - `min` is the minimum amount of memory needed when all the resizable
 *  structures are shrunk to their respective minimum sizes.
 *  - `max` is the maximum amount of memory needed when all the resizable
 *  structures are expanded to their respective maximum sizes. It is also the
 *  size of address space that must be reserved for the shared memory segment.
 *
 * When no resizable structures are requested, all three totals are identical.
 *
 * We take some care to ensure that the total size request doesn't overflow
 * size_t.  If this gets through, we don't need to be so careful during the
 * actual allocation phase.
 */
void
CalculateShmemSize(size_t *initial, size_t *min, size_t *max)
{
	size_t		initial_req;
	size_t		min_req;
	size_t		max_req;
	size_t		fixed_addins;

	ShmemGetRequestedSize(&initial_req, &min_req, &max_req);

	/*
	 * Size of the Postgres shared-memory block is estimated via moderately-
	 * accurate estimates for the big hogs, plus 100K for the stuff that's too
	 * small to bother with estimating.
	 *
	 * Also include additional requested shmem from preload libraries.
	 *
	 * These are not resizable, so they contribute equally to all three
	 * totals.
	 */
	fixed_addins = add_size(100000, total_addin_request);

	*initial = add_size(initial_req, fixed_addins);
	*min = add_size(min_req, fixed_addins);
	*max = add_size(max_req, fixed_addins);

	/* might as well round each off to a multiple of a typical page size */
	*initial = add_size(*initial, 8192 - (*initial % 8192));
	*min = add_size(*min, 8192 - (*min % 8192));
	*max = add_size(*max, 8192 - (*max % 8192));
}

#ifdef EXEC_BACKEND
/*
 * AttachSharedMemoryStructs
 *		Initialize a postmaster child process's access to shared memory
 *      structures.
 *
 * In !EXEC_BACKEND mode, we inherit everything through the fork, and this
 * isn't needed.
 */
void
AttachSharedMemoryStructs(void)
{
	/* InitProcess must've been called already */
	Assert(MyProc != NULL);
	Assert(IsUnderPostmaster);

	/*
	 * In EXEC_BACKEND mode, backends don't inherit the number of fast-path
	 * groups we calculated before setting the shmem up, so recalculate it.
	 */
	InitializeFastPathLocks();

	/* Establish pointers to all shared memory areas in this backend */
	ShmemAttachRequested();

	/*
	 * Now give loadable modules a chance to set up their shmem allocations
	 */
	if (shmem_startup_hook)
		shmem_startup_hook();
}
#endif

/*
 * CreateSharedMemoryAndSemaphores
 *		Creates and initializes shared memory and semaphores.
 */
void
CreateSharedMemoryAndSemaphores(void)
{
	PGShmemHeader *shim;
	PGShmemHeader *seghdr;
	size_t		initial_size;
	size_t		min_size;
	size_t		max_size;

	Assert(!IsUnderPostmaster);

	/* Compute the size of the shared-memory block */
	CalculateShmemSize(&initial_size, &min_size, &max_size);
	elog(DEBUG3, "invoking IpcMemoryCreate(initial size=%zu, minimum size=%zu, maximum size=%zu)",
		 initial_size, min_size, max_size);

	/*
	 * Create the shmem segment.
	 *
	 * Reserve enough shared address space to accommodate every requested
	 * structure grown to its maximum size.
	 */
	seghdr = PGSharedMemoryCreate(initial_size, max_size, &shim);

	/*
	 * Make sure that huge pages are never reported as "unknown" while the
	 * server is running.
	 */
	Assert(strcmp("unknown",
				  GetConfigOption("huge_pages_status", false, false)) != 0);

	/*
	 * Set up shared memory allocation mechanism
	 */
	InitShmemAllocator(seghdr);

	/* Initialize all shmem areas */
	ShmemInitRequested();

	/* Initialize dynamic shared memory facilities. */
	dsm_postmaster_startup(shim);

	/*
	 * Now give loadable modules a chance to set up their shmem allocations
	 */
	if (shmem_startup_hook)
		shmem_startup_hook();
}

/*
 * Early initialization of various subsystems, giving them a chance to
 * register their shared memory needs before the shared memory segment is
 * allocated.
 */
void
RegisterBuiltinShmemCallbacks(void)
{
	/*
	 * Call RegisterShmemCallbacks(...) on each subsystem listed in
	 * subsystemlist.h
	 */
#define PG_SHMEM_SUBSYSTEM(subsystem_callbacks) \
	RegisterShmemCallbacks(&(subsystem_callbacks));

#include "storage/subsystemlist.h"

#undef PG_SHMEM_SUBSYSTEM
}

/*
 * InitializeShmemGUCs
 *
 * This function initializes runtime-computed GUCs related to the amount of
 * shared memory required for the current configuration.
 */
void
InitializeShmemGUCs(void)
{
	char		buf[64];
	size_t		initial_b;
	size_t		min_b;
	size_t		max_b;
	size_t		hp_size;
	size_t		size_mb;

	CalculateShmemSize(&initial_b, &min_b, &max_b);

	/* Round each size up to the nearest megabyte. */
	size_mb = add_size(initial_b, (1024 * 1024) - 1) / (1024 * 1024);
	sprintf(buf, "%zu", size_mb);
	SetConfigOption("shared_memory_initial_size", buf,
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	size_mb = add_size(min_b, (1024 * 1024) - 1) / (1024 * 1024);
	sprintf(buf, "%zu", size_mb);
	SetConfigOption("shared_memory_minimum_size", buf,
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	size_mb = add_size(max_b, (1024 * 1024) - 1) / (1024 * 1024);
	sprintf(buf, "%zu", size_mb);
	SetConfigOption("shared_memory_maximum_size", buf,
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	/* Calculate the number of huge pages required for each size. */
	GetHugePageSize(&hp_size, NULL);
	if (hp_size != 0)
	{
		size_t		hp_required;

		hp_required = initial_b / hp_size + (initial_b % hp_size != 0);
		sprintf(buf, "%zu", hp_required);
		SetConfigOption("shared_memory_initial_size_in_huge_pages", buf,
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

		hp_required = min_b / hp_size + (min_b % hp_size != 0);
		sprintf(buf, "%zu", hp_required);
		SetConfigOption("shared_memory_minimum_size_in_huge_pages", buf,
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

		hp_required = max_b / hp_size + (max_b % hp_size != 0);
		sprintf(buf, "%zu", hp_required);
		SetConfigOption("shared_memory_maximum_size_in_huge_pages", buf,
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	}

	sprintf(buf, "%d", ProcGlobalSemas());
	SetConfigOption("num_os_semaphores", buf, PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
}
