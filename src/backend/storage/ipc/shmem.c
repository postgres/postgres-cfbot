/*-------------------------------------------------------------------------
 *
 * shmem.c
 *	  create shared memory and initialize shared memory data structures.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/shmem.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * POSTGRES processes share one or more regions of shared memory.
 * The shared memory is created by a postmaster and is inherited
 * by each backend via fork() (or, in some ports, via other OS-specific
 * methods).  The routines in this file are used for allocating and
 * binding to shared memory data structures.
 *
 * This module provides facilities to allocate fixed-size as well as resizable
 * structures in shared memory, for things like variables shared between all
 * backend processes. Each such structure has a string name to identify it,
 * specified when it is requested.  shmem_hash.c provides a shared hash table
 * implementation on top of fixed-size structures.
 *
 * Shared memory areas should usually not be allocated after postmaster
 * startup, although we do allow small allocations later for the benefit of
 * extension modules that are loaded after startup.  Despite that allowance,
 * extensions that need shared memory should be added in
 * shared_preload_libraries, because the allowance is quite small and there is
 * no guarantee that any memory is available after startup.
 *
 * Nowadays, there is also another way to allocate shared memory called
 * Dynamic Shared Memory.  See dsm.c for that facility.  One big difference
 * between traditional shared memory handled by shmem.c and dynamic shared
 * memory is that traditional shared memory areas are mapped to the same
 * address in all processes, so you can use normal pointers in shared memory
 * structs.  With Dynamic Shared Memory, you must use offsets or DSA pointers
 * instead.
 *
 * Shared memory managed by shmem.c can never be freed, once allocated.  Each
 * hash table has its own free list, so hash buckets can be reused when an
 * item is deleted.
 *
 * Usage
 * -----
 *
 * To allocate shared memory, you need to register a set of callback functions
 * which handle the lifecycle of the allocation.  In the request_fn callback,
 * call ShmemRequestStruct() with the desired name and size.  When the area is
 * later allocated or attached to, the global variable pointed to by the .ptr
 * option is set to the shared memory location of the allocation.  The init_fn
 * callback can perform additional initialization.
 *
 *	typedef struct MyShmemData {
 *		...
 *	} MyShmemData;
 *
 *	static MyShmemData *MyShmem;
 *
 *	static void my_shmem_request(void *arg);
 *	static void my_shmem_init(void *arg);
 *
 *  const ShmemCallbacks MyShmemCallbacks = {
 *		.request_fn = my_shmem_request,
 *		.init_fn = my_shmem_init,
 *	};
 *
 *	static void
 *	my_shmem_request(void *arg)
 *	{
 *		ShmemRequestStruct(.name = "My shmem area",
 *						   .size = sizeof(MyShmemData),
 *						   .ptr = (void **) &MyShmem,
 *			);
 *	}
 *
 * In builtin PostgreSQL code, add the callbacks to the list in
 * src/include/storage/subsystemlist.h.  In an add-in module, you can register
 * the callbacks by calling RegisterShmemCallbacks(&MyShmemCallbacks) in the
 * extension's _PG_init() function.
 *
 * Lifecycle
 * ---------
 *
 * Initializing shared memory happens in multiple phases.  In the first phase,
 * during postmaster startup, all the request_fn callbacks are called.  Only
 * after all the request_fn callbacks have been called and all the shmem areas
 * have been requested by the ShmemRequestStruct() calls we know how much
 * shared memory we need in total.  After that, postmaster allocates global
 * shared memory segment, and calls all the init_fn callbacks to initialize
 * all the requested shmem areas.
 *
 * In standard Unix-ish environments, individual backends do not need to
 * re-establish their local pointers into shared memory, because they inherit
 * correct values of those variables via fork() from the postmaster.  However,
 * this does not work in the EXEC_BACKEND case.  In ports using EXEC_BACKEND,
 * backend startup also calls the shmem_request callbacks to re-establish the
 * knowledge about each shared memory area, sets the pointer variables
 * (*options->ptr), and calls the attach_fn callback, if any, for additional
 * per-backend setup.
 *
 * Resizable shared memory structures
 * ----------------------------------
 *
 * In order to allocate resizable shared memory structures, set
 * ShmemRequestStructOpts::maximum_size to the maximum size that the structure
 * can grow to.  The address space for the maximum size will be reserved at
 * startup, but memory is allocated or freed as the structure grows or shrinks
 * respectively. ShmemRequestStructOpts::size should be set to the initial size
 * of the structure, which is the amount of memory allocated at the startup.
 * Optionally, ShmemRequestStructOpts::minimum_size can be set to the minimum
 * size that the structure can shrink to. After startup, the structure can be
 * resized by calling ShmemResizeStruct(). ShmemResizeStruct() enforces that the
 * new size is within [minimum_size, maximum_size].
 *
 * While resizable structures can be created after the startup, the memory
 * available for them is quite limited.
 *
 * Legacy ShmemInitStruct()/ShmemInitHash() functions
 * --------------------------------------------------
 *
 * ShmemInitStruct()/ShmemInitHash() is another way of registering shmem
 * areas.  It pre-dates the ShmemRequestStruct()/ShmemRequestHash() functions,
 * and should not be used in new code, but as of this writing it is still
 * widely used in extensions.
 *
 * To allocate a shmem area with ShmemInitStruct(), you need to separately
 * register the size needed for the area by calling RequestAddinShmemSpace()
 * from the extension's shmem_request_hook, and allocate the area by calling
 * ShmemInitStruct() from the extension's shmem_startup_hook.  There are no
 * init/attach callbacks.  Instead, the caller of ShmemInitStruct() must check
 * the return status of ShmemInitStruct() and initialize the struct if it was
 * not previously initialized.
 *
 * Calling ShmemAlloc() directly
 * -----------------------------
 *
 * There's a more low-level way of allocating shared memory too: you can call
 * ShmemAlloc() directly.  It's used to implement the higher level mechanisms,
 * and should generally not be called directly.
 */

#include "postgres.h"

#include <unistd.h>

#include "access/slru.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "port/pg_numa.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/shmem_internal.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

/*
 * Registered callbacks.
 *
 * During postmaster startup, we accumulate the callbacks from all subsystems
 * in this list.
 *
 * This is in process private memory, although on Unix-like systems, we expect
 * all the registrations to happen at postmaster startup time and be inherited
 * by all the child processes via fork().
 */
static List *registered_shmem_callbacks;

/*
 * In the shmem request phase, all the shmem areas requested with the
 * ShmemRequest*() functions are accumulated here.
 */
typedef struct
{
	ShmemStructOpts *options;
	ShmemRequestKind kind;
} ShmemRequest;

static List *pending_shmem_requests;

/*
 * Per-process state machine, for sanity checking that we do things in the
 * right order.
 *
 * Postmaster:
 *   INITIAL -> REQUESTING -> INITIALIZING -> DONE
 *
 * Backends in EXEC_BACKEND mode:
 *   INITIAL -> REQUESTING -> ATTACHING -> DONE
 *
 * Late request:
 *   DONE -> REQUESTING -> AFTER_STARTUP_ATTACH_OR_INIT -> DONE
 */
enum shmem_request_state
{
	/* Initial state */
	SRS_INITIAL,

	/*
	 * When we start calling the shmem_request callbacks, we enter the
	 * SRS_REQUESTING phase.  All ShmemRequestStruct calls happen in this
	 * state.
	 */
	SRS_REQUESTING,

	/*
	 * Postmaster has finished all shmem requests, and is now initializing the
	 * shared memory segment.  init_fn callbacks are called in this state.
	 */
	SRS_INITIALIZING,

	/*
	 * A postmaster child process is starting up.  attach_fn callbacks are
	 * called in this state.
	 */
	SRS_ATTACHING,

	/* An after-startup allocation or attachment is in progress */
	SRS_AFTER_STARTUP_ATTACH_OR_INIT,

	/* Normal state after shmem initialization / attachment */
	SRS_DONE,
};
static enum shmem_request_state shmem_request_state = SRS_INITIAL;

/*
 * This is the first data structure stored in the shared memory segment, at
 * the offset that PGShmemHeader->content_offset points to.  Allocations by
 * ShmemAlloc() are carved out of the space after this.
 *
 * For the base pointer and the total size of the shmem segment, we rely on
 * the PGShmemHeader.
 */
typedef struct ShmemAllocatorData
{
	Size		free_offset;	/* offset to first free space from ShmemBase */

	/* protects 'free_offset' */
	slock_t		shmem_lock;

	HASHHDR    *index;			/* location of ShmemIndex */
	size_t		index_size;		/* size of shmem region holding ShmemIndex */
	LWLock		index_lock;		/* protects ShmemIndex */
} ShmemAllocatorData;

#define ShmemIndexLock (&ShmemAllocator->index_lock)

static void *ShmemAllocRaw(Size size, Size alignment, Size *allocated_size);

/* shared memory global variables */

static PGShmemHeader *ShmemSegHdr;	/* shared mem segment header */
static void *ShmemBase;			/* start address of shared memory */
static void *ShmemEnd;			/* end+1 address of shared memory */

static ShmemAllocatorData *ShmemAllocator;

/*
 * ShmemIndex is a global directory of shmem areas, itself also stored in the
 * shared memory.
 */
static HTAB *ShmemIndex;

 /* max size of data structure string name */
#define SHMEM_INDEX_KEYSIZE		 (48)

/*
 * # of additional entries to reserve in the shmem index table, for
 * allocations after postmaster startup.  (This is not a hard limit, the hash
 * table can grow larger than that if there is shared memory available)
 */
#define SHMEM_INDEX_ADDITIONAL_SIZE		 (128)

/* this is a hash bucket in the shmem index table */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];	/* string name */
	void	   *location;		/* location in shared mem */
	Size		size;			/* # bytes requested for the structure */
	Size		allocated_size; /* # bytes actually allocated */
	Size		minimum_size;	/* the minimum size the structure can shrink
								 * to */
	Size		maximum_size;	/* the maximum size the structure can grow to */
	Size		reserved_space; /* the total address space reserved */
} ShmemIndexEnt;

/* To get reliable results for NUMA inquiry we need to "touch pages" once */
static bool firstNumaTouch = true;

static void CallShmemCallbacksAfterStartup(const ShmemCallbacks *callbacks);
static void InitShmemIndexEntry(ShmemRequest *request);
static bool AttachShmemIndexEntry(ShmemRequest *request, bool missing_ok);
static Size EstimateAllocatedSize(ShmemIndexEnt *entry);
static void ShmemProtectStructInternal(ShmemIndexEnt *entry);

Datum		pg_numa_available(PG_FUNCTION_ARGS);

/*
 *	ShmemRequestStruct() --- request a named shared memory area
 *
 * Subsystems call this to register their shared memory needs.  This is
 * usually done early in postmaster startup, before the shared memory segment
 * has been created, so that the size can be included in the estimate for
 * total amount of shared memory needed.  We set aside a small amount of
 * memory for allocations that happen later, for the benefit of non-preloaded
 * extensions, but that should not be relied upon.
 *
 * This does not yet allocate the memory, but merely registers the need for
 * it.  The actual allocation happens later in the postmaster startup
 * sequence.
 *
 * This must be called from a shmem_request callback function, registered with
 * RegisterShmemCallbacks().  This enforces a coding pattern that works the
 * same in normal Unix systems and with EXEC_BACKEND.  On Unix systems, the
 * shmem_request callbacks are called once, early in postmaster startup, and
 * the child processes inherit the struct descriptors and any other
 * per-process state from the postmaster.  In EXEC_BACKEND mode, shmem_request
 * callbacks are *also* called in each backend, at backend startup, to
 * re-establish the struct descriptors.  By calling the same function in both
 * cases, we ensure that all the shmem areas are registered the same way in
 * all processes.
 *
 * 'options' defines the name and size of the area, and any other optional
 * features.  Leave unused options as zeros.  The options are copied to
 * longer-lived memory, so it doesn't need to live after the
 * ShmemRequestStruct() call and can point to a local variable in the calling
 * function.  The 'name' must point to a long-lived string though, only the
 * pointer to it is copied.
 */
void
ShmemRequestStructWithOpts(const ShmemStructOpts *options)
{
	ShmemStructOpts *options_copy;

	options_copy = MemoryContextAlloc(TopMemoryContext,
									  sizeof(ShmemStructOpts));
	memcpy(options_copy, options, sizeof(ShmemStructOpts));

	ShmemRequestInternal(options_copy, SHMEM_KIND_STRUCT);
}

/*
 * Internal workhorse of ShmemRequestStruct() and ShmemRequestHash().
 *
 * Note: Unlike in the public ShmemRequestStruct() and ShmemRequestHash()
 * functions, 'options' is *not* copied.  It must be allocated in
 * TopMemoryContext by the caller, and will be freed after the init/attach
 * callbacks have been called.  This allows ShmemRequestHash() to pass a
 * pointer to the extended ShmemHashOpts struct instead.
 */
void
ShmemRequestInternal(ShmemStructOpts *options, ShmemRequestKind kind)
{
	ShmemRequest *request;

	/* Check the options */
	if (options->name == NULL)
		elog(ERROR, "shared memory request is missing 'name' option");

#ifndef HAVE_RESIZABLE_SHMEM
	if (options->maximum_size > 0)
		elog(ERROR, "resizable shared memory is not supported on this platform");
	if (options->minimum_size > 0)
		elog(ERROR, "resizable shared memory is not supported on this platform");
#else
	if (options->maximum_size > 0 && shared_memory_type != SHMEM_TYPE_MMAP)
		elog(ERROR, "resizable shared memory requires shared_memory_type = mmap");
#endif

	if (IsUnderPostmaster)
	{
		if (options->size <= 0 && options->size != SHMEM_ATTACH_UNKNOWN_SIZE)
			elog(ERROR, "invalid size %zd for shared memory request for \"%s\"",
				 options->size, options->name);
		if (options->minimum_size < 0 && options->minimum_size != SHMEM_ATTACH_UNKNOWN_SIZE)
			elog(ERROR, "invalid minimum_size %zd for shared memory request for \"%s\"",
				 options->minimum_size, options->name);
		if (options->maximum_size < 0 && options->maximum_size != SHMEM_ATTACH_UNKNOWN_SIZE)
			elog(ERROR, "invalid maximum_size %zd for shared memory request for \"%s\"",
				 options->maximum_size, options->name);
	}
	else
	{
		if (options->size == SHMEM_ATTACH_UNKNOWN_SIZE ||
			options->minimum_size == SHMEM_ATTACH_UNKNOWN_SIZE ||
			options->maximum_size == SHMEM_ATTACH_UNKNOWN_SIZE)
			elog(ERROR, "SHMEM_ATTACH_UNKNOWN_SIZE cannot be used during startup");
		if (options->size <= 0)
			elog(ERROR, "invalid size %zd for shared memory request for \"%s\"",
				 options->size, options->name);
		if (options->minimum_size < 0)
			elog(ERROR, "invalid minimum_size %zd for shared memory request for \"%s\"",
				 options->minimum_size, options->name);
		if (options->maximum_size < 0)
			elog(ERROR, "invalid maximum_size %zd for shared memory request for \"%s\"",
				 options->maximum_size, options->name);
	}

	if (options->alignment != 0 && pg_nextpower2_size_t(options->alignment) != options->alignment)
		elog(ERROR, "invalid alignment %zu for shared memory request for \"%s\"",
			 options->alignment, options->name);

	if (options->minimum_size > 0 && options->size != SHMEM_ATTACH_UNKNOWN_SIZE &&
		options->minimum_size > options->size)
		elog(ERROR, "resizable shared memory structure \"%s\" should have minimum size (%zd) less than or equal to size (%zd)",
			 options->name, options->minimum_size, options->size);

	if (options->maximum_size > 0 && options->size > options->maximum_size)
		elog(ERROR, "resizable shared memory structure \"%s\" should have maximum size (%zd) greater than size (%zd)",
			 options->name, options->maximum_size, options->size);

	if (options->minimum_size > 0 && options->maximum_size > 0 &&
		options->minimum_size > options->maximum_size)
		elog(ERROR, "resizable shared memory structure \"%s\" should have minimum size (%zd) less than or equal to maximum size (%zd)",
			 options->name, options->minimum_size, options->maximum_size);

	/* Check that we're in the right state */
	if (shmem_request_state != SRS_REQUESTING)
		elog(ERROR, "ShmemRequestStruct can only be called from a shmem_request callback");

	/* Check that it's not already registered in this process */
	foreach_ptr(ShmemRequest, existing, pending_shmem_requests)
	{
		if (strcmp(existing->options->name, options->name) == 0)
			ereport(ERROR,
					(errmsg("shared memory struct \"%s\" is already registered",
							options->name)));
	}

	/* Request looks valid, remember it */
	request = palloc_object(ShmemRequest);
	request->options = options;
	request->kind = kind;
	pending_shmem_requests = lappend(pending_shmem_requests, request);
}

/*
 * ShmemGetRequestedSize() --- estimate total size of all registered shared
 * memory structures.
 *
 * Returns three totals:
 * - initial - the sum of initial sizes of the requested structures which is the
 * total amount of memory required at the startup.
 * - min - the total of minimum sizes of structures
 * - max - the sum of maximum sizes of the structures, which is the address
 * space that must be reserved.
 *
 * When there are no resizable structures or on the platforms that do not
 * support resizable structures, all three totals are the same.
 *
 * This is called at postmaster startup, before the shared memory segment has
 * been created.
 */
void
ShmemGetRequestedSize(size_t *initial, size_t *min, size_t *max)
{
	size_t		initial_size;
	size_t		min_size;
	size_t		max_size;

	/* memory needed for the ShmemIndex */
	initial_size = hash_estimate_size(list_length(pending_shmem_requests) + SHMEM_INDEX_ADDITIONAL_SIZE,
									  sizeof(ShmemIndexEnt));
	initial_size = CACHELINEALIGN(initial_size);
	min_size = initial_size;
	max_size = initial_size;

	/* memory needed for all the requested areas */
	foreach_ptr(ShmemRequest, request, pending_shmem_requests)
	{
		size_t		alignment = request->options->alignment;
		size_t		req_min;
		size_t		req_max;
		size_t		req_initial = request->options->size;

		if (request->options->maximum_size > 0)
		{
			req_min = request->options->minimum_size;
			req_max = request->options->maximum_size;
		}
		else
		{
			req_min = req_initial;
			req_max = req_initial;
		}

		/* pad the start address for alignment like ShmemAllocRaw() does */
		if (alignment < PG_CACHE_LINE_SIZE)
			alignment = PG_CACHE_LINE_SIZE;
		initial_size = TYPEALIGN(alignment, initial_size);
		min_size = TYPEALIGN(alignment, min_size);
		max_size = TYPEALIGN(alignment, max_size);

		initial_size = add_size(initial_size, req_initial);
		min_size = add_size(min_size, req_min);
		max_size = add_size(max_size, req_max);
	}

	*initial = initial_size;
	*min = min_size;
	*max = max_size;
}

/*
 *	ShmemInitRequested() --- allocate and initialize requested shared memory
 *                            structures.
 *
 * This is called once at postmaster startup, after the shared memory segment
 * has been created.
 */
void
ShmemInitRequested(void)
{
	/* should be called only by the postmaster or a standalone backend */
	Assert(!IsUnderPostmaster);
	Assert(shmem_request_state == SRS_INITIALIZING);

	/*
	 * Initialize the ShmemIndex entries and perform basic initialization of
	 * all the requested memory areas.  There are no concurrent processes yet,
	 * so no need for locking.
	 *
	 * TODO: If we have resizable structures, we will mmap with MAP_NORESERVE.
	 * In case there is not enough memory to cover the initial sizes of the
	 * shared structures, the mmap will succeed but the initialization will
	 * fail with SIGBUS. Instead we should allocate the memory worth the
	 * initial size of each structure using madvise(MADV_WRITE_POPOULATE). But
	 * instead of doing that for each structure, we should combine contiguous
	 * structures and do it once for the whole range. The algorithm to use is
	 * as follows 1. start from the first request 2. note the start address of
	 * the first structure in the range 3. walk upto a resizable structure or
	 * the last request, noting the initial end address of the last structure
	 * in the range 4. call madvise(MADV_WRITE_POPULATE) for the range (start
	 * address, end address) 5. Next structure becomes the first structure in
	 * the next range, repeat from step 2
	 *
	 * If there are no resizable structure, no need to do anything, as all the
	 * memory is reserved during mmap itself.
	 */
	foreach_ptr(ShmemRequest, request, pending_shmem_requests)
	{
		InitShmemIndexEntry(request);
		pfree(request->options);
	}
	list_free_deep(pending_shmem_requests);
	pending_shmem_requests = NIL;

	/*
	 * Call the subsystem-specific init callbacks to finish initialization of
	 * all the areas.
	 */
	foreach_ptr(const ShmemCallbacks, callbacks, registered_shmem_callbacks)
	{
		if (callbacks->init_fn)
			callbacks->init_fn(callbacks->opaque_arg);
	}

	shmem_request_state = SRS_DONE;
}

/*
 * Re-establish process private state related to shmem areas.
 *
 * This is called at backend startup in EXEC_BACKEND mode, in every backend.
 */
#ifdef EXEC_BACKEND
void
ShmemAttachRequested(void)
{
	ListCell   *lc;

	/* Must be initializing a (non-standalone) backend */
	Assert(IsUnderPostmaster);
	Assert(ShmemAllocator->index != NULL);
	Assert(shmem_request_state == SRS_REQUESTING);
	shmem_request_state = SRS_ATTACHING;

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	/*
	 * Attach to all the requested memory areas.
	 */
	foreach_ptr(ShmemRequest, request, pending_shmem_requests)
	{
		AttachShmemIndexEntry(request, false);
		pfree(request->options);
	}
	list_free_deep(pending_shmem_requests);
	pending_shmem_requests = NIL;

	/* Call attach callbacks */
	foreach(lc, registered_shmem_callbacks)
	{
		const ShmemCallbacks *callbacks = (const ShmemCallbacks *) lfirst(lc);

		if (callbacks->attach_fn)
			callbacks->attach_fn(callbacks->opaque_arg);
	}

	LWLockRelease(ShmemIndexLock);

	shmem_request_state = SRS_DONE;
}
#endif

/*
 * Insert requested shmem area into the shared memory index and initialize it.
 *
 * Note that this only does performs basic initialization depending on
 * ShmemRequestKind, like setting the global pointer variable to the area for
 * SHMEM_KIND_STRUCT or setting up the backend-private HTAB control struct.
 * This does *not* call the subsystem-specific init callbacks.  That's done
 * later after all the shmem areas have been initialized or attached to.
 */
static void
InitShmemIndexEntry(ShmemRequest *request)
{
	const char *name = request->options->name;
	ShmemIndexEnt *index_entry;
	bool		found;
	size_t		allocated_size;
	size_t		requested_size;
	void	   *structPtr;

	/* look it up in the shmem index */
	index_entry = (ShmemIndexEnt *)
		hash_search(ShmemIndex, name, HASH_ENTER_NULL, &found);
	if (found)
		elog(ERROR, "shared memory struct \"%s\" is already initialized", name);
	if (!index_entry)
	{
		/* tried to add it to the hash table, but there was no space */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ShmemIndex entry for data structure \"%s\"",
						name)));
	}

	/*
	 * We inserted the entry to the shared memory index. Allocate requested
	 * amount of address space in the shared memory segment for it, and do
	 * basic initializion. The memory gets allocated during initialization as
	 * the corresponding memory pages are written to.  Allocate enough space
	 * for a resizable structure to grow to its maximum size. It is expected
	 * that the initialization callback will use only as much memory as the
	 * initial size of the resizable structure. (Well, if it doesn't, more
	 * memory will be allocated initially than expected, no further harm is
	 * done.)
	 */
	requested_size = request->options->maximum_size > 0 ?
		request->options->maximum_size : request->options->size;
	structPtr = ShmemAllocRaw(requested_size,
							  request->options->alignment,
							  &allocated_size);
	if (structPtr == NULL)
	{
		/* out of memory; remove the failed ShmemIndex entry */
		hash_search(ShmemIndex, name, HASH_REMOVE, NULL);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough shared memory space for data structure"
						" \"%s\" (%zd bytes requested)",
						name, requested_size)));
	}
	index_entry->size = request->options->size;
	index_entry->allocated_size = allocated_size;
	index_entry->location = structPtr;
	index_entry->reserved_space = allocated_size;
	if (request->options->maximum_size > 0)
	{
		index_entry->minimum_size = request->options->minimum_size;
		index_entry->maximum_size = request->options->maximum_size;

		/* Adjust allocated size of a resizable structure. */
		index_entry->allocated_size = EstimateAllocatedSize(index_entry);

		/*
		 * Protect the unused part of the reserved address space for a
		 * resizable structure. If the structure has same minimum and maximum
		 * size, it is effectively a fixed-size structure without any unused
		 * space; no protection is required.
		 */
		if (index_entry->minimum_size != index_entry->maximum_size)
			ShmemProtectStructInternal(index_entry);
	}
	else
	{
		index_entry->minimum_size = request->options->size;
		index_entry->maximum_size = request->options->size;
	}

	/* Initialize depending on the kind of shmem area it is */
	switch (request->kind)
	{
		case SHMEM_KIND_STRUCT:
			if (request->options->ptr)
				*(request->options->ptr) = index_entry->location;
			break;
		case SHMEM_KIND_HASH:
			shmem_hash_init(structPtr, request->options);
			break;
		case SHMEM_KIND_SLRU:
			shmem_slru_init(structPtr, request->options);
			break;
	}
}

/*
 * Look up a named shmem area in the shared memory index and attach to it.
 *
 * Note that this only performs the basic attachment actions depending on
 * ShmemRequestKind, like setting the global pointer variable to the area for
 * SHMEM_KIND_STRUCT or setting up the backend-private HTAB control struct.
 * This does *not* call the subsystem-specific attach callbacks.  That's done
 * later after all the shmem areas have been initialized or attached to.
 */
static bool
AttachShmemIndexEntry(ShmemRequest *request, bool missing_ok)
{
	const char *name = request->options->name;
	ShmemIndexEnt *index_entry;

	/* Look it up in the shmem index */
	index_entry = (ShmemIndexEnt *)
		hash_search(ShmemIndex, name, HASH_FIND, NULL);
	if (!index_entry)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errmsg("could not find ShmemIndex entry for data structure \"%s\"",
							request->options->name)));
		return false;
	}

	/* Check that the sizes in the index match the request. */
	if (index_entry->size != request->options->size &&
		request->options->size != SHMEM_ATTACH_UNKNOWN_SIZE)
	{
		ereport(ERROR,
				(errmsg("shared memory struct \"%s\" was created with"
						" different size: existing %zu, requested %zd",
						name, index_entry->size, request->options->size)));
	}

	/*
	 * For resizable structures, also check that minimum_size and maximum_size
	 * match. For fixed-size structures, these are derived (set to size) in
	 * the index entry and not meaningful in the request.
	 */
	if (request->options->maximum_size != 0)
	{
		if (index_entry->minimum_size != request->options->minimum_size &&
			request->options->minimum_size != SHMEM_ATTACH_UNKNOWN_SIZE)
		{
			ereport(ERROR,
					errmsg("shared memory struct \"%s\" was created with"
						   " different minimum_size: existing %zu, requested %zu",
						   name, index_entry->minimum_size,
						   request->options->minimum_size));
		}

		if (index_entry->maximum_size != request->options->maximum_size &&
			request->options->maximum_size != SHMEM_ATTACH_UNKNOWN_SIZE)
		{
			ereport(ERROR,
					errmsg("shared memory struct \"%s\" was created with"
						   " different maximum_size: existing %zu, requested %zu",
						   name, index_entry->maximum_size,
						   request->options->maximum_size));
		}
	}
	else
	{
		if (index_entry->minimum_size != index_entry->maximum_size)
			elog(ERROR, "shared memory struct \"%s\" was created as resizable, but requested as fixed-size",
				 name);
	}

	if (index_entry->minimum_size != index_entry->maximum_size)
		ShmemProtectStructInternal(index_entry);

	/*
	 * Re-establish the caller's pointer variable, or do other actions to
	 * attach depending on the kind of shmem area it is.
	 */
	switch (request->kind)
	{
		case SHMEM_KIND_STRUCT:
			if (request->options->ptr)
				*(request->options->ptr) = index_entry->location;
			break;
		case SHMEM_KIND_HASH:
			shmem_hash_attach(index_entry->location, request->options);
			break;
		case SHMEM_KIND_SLRU:
			shmem_slru_attach(index_entry->location, request->options);
			break;
	}

	return true;
}

/*
 * Estimate the actual memory allocated for a resizable structure.
 *
 * ... based on the assumption that the memory is allocated in pages.
 *
 * The memory pages covered by the current size of a resizable structure are
 * considered to be allocated. The memory page where the maximal structure ends
 * also hosts the next structure, unless the maximal structure ends on a page
 * boundary. Hence that page is allocated because of the next structure. The
 * memory pages between the page where the current structure ends and the page
 * where the next structure starts remain unallocated. Thus the memory allocated
 * for a resizable structure can be estimated as the total address space
 * reserved for the structure minus the unallocated memory pages between the
 * current end and the next structure.
 */
static Size
EstimateAllocatedSize(ShmemIndexEnt *entry)
{
	Size		page_size = GetOSPageSize();
	char	   *align_end = (char *) TYPEALIGN(page_size, (char *) entry->location + entry->size);
	char	   *floor_max_end = (char *) TYPEALIGN_DOWN(page_size, (char *) entry->location + entry->maximum_size);

	Assert(entry->maximum_size >= entry->size);
	Assert(entry->reserved_space >= entry->maximum_size);

	if (align_end < floor_max_end)
		return entry->reserved_space - (floor_max_end - align_end);

	return entry->reserved_space;
}

/*
 * ShmemResizeStruct() --- resize a resizable shared memory structure.
 *
 * The new size must be within [minimum_size, maximum_size].  If the structure
 * is being shrunk, the memory pages that are no longer needed are freed. If
 * the structure is being expanded, the memory pages that are needed for the
 * new size are allocated. See EstimateAllocatedSize() for explanation of which
 * pages are allocated for a resizable structure.
 *
 * The caller must ensure that no other backend is accessing the part of the
 * structure between the old size and the new size while this function is
 * running. It should also ensure that all backends that may access the
 * structure have observed the new size before they access the range between
 * the old size and the new size.
 *
 * If we can not allocate memory pages when expanding the structure, this
 * function will return false. On success it returns true. Instead
 * of returning false, an error is raised if we can not free the memory pages
 * when shrinking the structure, which should not happen in practice.
 */
bool
ShmemResizeStruct(const char *name, Size new_size)
{
#ifndef HAVE_RESIZABLE_SHMEM
	ereport(ERROR,
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("resizable shared memory is not supported on this platform"));
	pg_unreachable();
#else
	ShmemIndexEnt *result;
	bool		found;
	Size		page_size = GetOSPageSize();
	char	   *new_end;
	bool		success = true;

	Assert(new_size > 0);

	/*
	 * Resizable shared memory structures are only supported with mmap'ed
	 * memory.
	 */
	Assert(shared_memory_type == SHMEM_TYPE_MMAP);

	/* look it up in the shmem index */
	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);
	result = (ShmemIndexEnt *) hash_search(ShmemIndex, name, HASH_FIND, &found);
	if (!found)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("shmem struct \"%s\" is not initialized", name));

	Assert(result);

	if (result->minimum_size == result->maximum_size)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("shared memory struct \"%s\" is not resizable", name));

	if (new_size < result->minimum_size)
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				errmsg("cannot shrink shared memory structure \"%s\" below minimum size"
					   "(requested %zu bytes, minimum %zu bytes)",
					   name, new_size, result->minimum_size));

	if (result->maximum_size < new_size)
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				errmsg("not enough address space is reserved for resizing structure \"%s\""
					   " (required %zu bytes, reserved %zu bytes)",
					   name, new_size, result->maximum_size));

	/*
	 * A structure requires memory pages from the page containing the start of
	 * the structure and current end of the structure to be allocated. When
	 * expanding, we make sure that pages are allocated up to the new end.
	 * When shrinking, release memory pages beyond the new end, but not the
	 * page containing maximal end of the structure, as it may be used by the
	 * next structure.
	 *
	 * We do not consider the current end of the structure as it simplifies
	 * the calculations. Instead we rely on the underlying APIs not to touch
	 * the memory pages that will not be affected by the change in size.
	 */
	new_end = (char *) TYPEALIGN(page_size, (char *) result->location + new_size);
	if (new_size < result->size)
	{
		char	   *max_end = (char *) TYPEALIGN_DOWN(page_size, (char *) result->location + result->maximum_size);

		if (max_end > new_end)
		{
			if (!PGSharedMemoryEnsureFreed(new_end, max_end - new_end))
				ereport(ERROR,
						errcode(ERRCODE_SYSTEM_ERROR),
						errmsg("could not free %zu bytes of shared memory from structure \"%s\"",
							   result->size - new_size, name));
		}
	}
	else if (new_size > result->size)
	{
		char	   *struct_start = (char *) TYPEALIGN_DOWN(page_size, (char *) result->location);

		if (new_end > struct_start)
		{
			ShmemIndexEnt entry_copy = *result;

			/*
			 * Allocating memory pages in the expanded range may require the
			 * corresponding address space to have read-write access.
			 */
			entry_copy.size = new_size;
			ShmemProtectStructInternal(&entry_copy);

			if (!PGSharedMemoryEnsureAllocated(struct_start, new_end - struct_start))
			{
				ShmemProtectStructInternal(result);
				ereport(WARNING,
						errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("could not allocate %zu bytes of shared memory to structure \"%s\"",
							   new_size - result->size, name));
				success = false;
			}
		}
	}

	/* Update shmem index entry. */
	if (success)
	{
		result->size = new_size;
		result->allocated_size = EstimateAllocatedSize(result);
	}

	LWLockRelease(ShmemIndexLock);

	return success;
#endif
}

/*
 * ShmemProtectStruct() --- protect the unused portion of the given resizable
 * structure.
 *
 * Makes the region beyond the current size up to maximum_size inaccessible, and
 * ensures the region up to the current size is readable and writable. Depending
 * upon the platform, the protection honours the page boundaries. So it may be
 * more permissible than strictly needed.
 *
 * This function only affects the calling backend's address space. After each
 * ShmemResizeStruct(), every backend that may access the structure should call
 * this function before its next access. When backends are still in the middle
 * of that round of ShmemProtectStruct() calls, ShmemResizeStruct() should not
 * be called.
 */
void
ShmemProtectStruct(const char *name)
{
#ifndef HAVE_RESIZABLE_SHMEM
	ereport(ERROR,
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("resizable shared memory is not supported on this platform"));
#else
	ShmemIndexEnt *result;
	bool		found;

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	result = (ShmemIndexEnt *) hash_search(ShmemIndex, name, HASH_FIND, &found);
	if (!found)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("shmem struct \"%s\" is not initialized", name));

	if (result->minimum_size == result->maximum_size)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("shared memory struct \"%s\" is not resizable", name));

	ShmemProtectStructInternal(result);

	LWLockRelease(ShmemIndexLock);
#endif
}

/*
 * ShmemProtectStructInternal --- same as ShmemProtectStruct()
 *
 * ..., but called when the ShmemIndexEnt of the struct is available. The caller
 * should hold ShmemIndexLock if required.
 */
static void
ShmemProtectStructInternal(ShmemIndexEnt *entry)
{
#ifndef HAVE_RESIZABLE_SHMEM
	ereport(ERROR,
			errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("resizable shared memory is not supported on this platform"));
#else
	Size		page_size = GetOSPageSize();
	char	   *rw_start;
	char	   *rw_end;
	char	   *prot_end;

	Assert(shared_memory_type == SHMEM_TYPE_MMAP);
	Assert(entry->minimum_size != entry->maximum_size);

	/* Make at least [location, location+size) readable and writable */
	rw_start = (char *) TYPEALIGN_DOWN(page_size, entry->location);
	rw_end = (char *) TYPEALIGN(page_size,
								(char *) entry->location + entry->size);

	/*
	 * Make remaining portion inaccessible while making sure that the portion
	 * after maximum_size is not affected since it may be used by other
	 * structures.
	 */
	prot_end = (char *) TYPEALIGN_DOWN(page_size,
									   (char *) entry->location + entry->maximum_size);

	if (!PGSharedMemoryProtect(rw_start, rw_end, prot_end))
		ereport(ERROR,
				errcode(ERRCODE_SYSTEM_ERROR),
				errmsg("could not protect shared memory structure \"%s\"", entry->key));
#endif
}

/*
 * ShmemReprotectResizableStructs() --- re-apply per-backend protection to every
 * resizable shmem structure.
 *
 * On !EXEC_BACKEND platforms, a new backend inherits shmem mappings and their
 * per-process protections from the postmaster. If any resizable structure has
 * been resized since postmaster start, the inherited protections no longer
 * match the current size. This is called early in per-backend startup so the
 * new backend sees the protections according to the current sizes of the
 * resizable structures.
 */
void
ShmemReprotectResizableStructs(void)
{
#ifdef HAVE_RESIZABLE_SHMEM
	HASH_SEQ_STATUS status;
	ShmemIndexEnt *entry;

	LWLockAcquire(ShmemIndexLock, LW_SHARED);
	hash_seq_init(&status, ShmemIndex);
	while ((entry = (ShmemIndexEnt *) hash_seq_search(&status)) != NULL)
	{
		if (entry->minimum_size != entry->maximum_size)
			ShmemProtectStructInternal(entry);
	}
	LWLockRelease(ShmemIndexLock);
#endif
}

/*
 *	InitShmemAllocator() --- set up basic pointers to shared memory.
 *
 * Called at postmaster or stand-alone backend startup, to initialize the
 * allocator's data structure in the shared memory segment.  In EXEC_BACKEND,
 * this is also called at backend startup, to set up pointers to the
 * already-initialized data structure.
 */
void
InitShmemAllocator(PGShmemHeader *seghdr)
{
	Size		offset;
	int64		hash_nelems;
	HASHCTL		info;
	int			hash_flags;

#ifndef EXEC_BACKEND
	Assert(!IsUnderPostmaster);
#endif
	Assert(seghdr != NULL);

	if (IsUnderPostmaster)
	{
		Assert(shmem_request_state == SRS_INITIAL);
	}
	else
	{
		Assert(shmem_request_state == SRS_REQUESTING);
		shmem_request_state = SRS_INITIALIZING;
	}

	/*
	 * We assume the pointer and offset are MAXALIGN.  Not a hard requirement,
	 * but it's true today and keeps the math below simpler.
	 */
	Assert(seghdr == (void *) MAXALIGN(seghdr));
	Assert(seghdr->content_offset == MAXALIGN(seghdr->content_offset));

	/*
	 * Allocations after this point should go through ShmemAlloc, which
	 * expects to allocate everything on cache line boundaries.  Make sure the
	 * first allocation begins on a cache line boundary.
	 */
	offset = CACHELINEALIGN(seghdr->content_offset + sizeof(ShmemAllocatorData));
	if (offset > seghdr->totalsize)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						offset)));

	/*
	 * In postmaster or stand-alone backend, initialize the shared memory
	 * allocator so that we can allocate shared memory for ShmemIndex using
	 * ShmemAlloc().  In a regular backend just set up the pointers required
	 * by ShmemAlloc().
	 */
	ShmemAllocator = (ShmemAllocatorData *) ((char *) seghdr + seghdr->content_offset);
	if (!IsUnderPostmaster)
	{
		SpinLockInit(&ShmemAllocator->shmem_lock);
		ShmemAllocator->free_offset = offset;
		LWLockInitialize(&ShmemAllocator->index_lock, LWTRANCHE_SHMEM_INDEX);
	}

	ShmemSegHdr = seghdr;
	ShmemBase = seghdr;
	ShmemEnd = (char *) ShmemBase + seghdr->totalsize;

	/*
	 * Create (or attach to) the shared memory index of shmem areas.
	 *
	 * This is the same initialization as ShmemInitHash() does, but we cannot
	 * use ShmemInitHash() here because it relies on ShmemIndex being already
	 * initialized.
	 */
	hash_nelems = list_length(pending_shmem_requests) + SHMEM_INDEX_ADDITIONAL_SIZE;

	info.keysize = SHMEM_INDEX_KEYSIZE;
	info.entrysize = sizeof(ShmemIndexEnt);
	hash_flags = HASH_ELEM | HASH_STRINGS | HASH_FIXED_SIZE;

	if (!IsUnderPostmaster)
	{
		ShmemAllocator->index_size = hash_estimate_size(hash_nelems, info.entrysize);
		ShmemAllocator->index = (HASHHDR *) ShmemAlloc(ShmemAllocator->index_size);
	}
	ShmemIndex = shmem_hash_create(ShmemAllocator->index,
								   ShmemAllocator->index_size,
								   IsUnderPostmaster,
								   "ShmemIndex", hash_nelems,
								   &info, hash_flags);
	Assert(ShmemIndex != NULL);

	/*
	 * Add an entry for ShmemIndex itself into ShmemIndex, so that it's
	 * visible in the pg_shmem_allocations view
	 */
	if (!IsUnderPostmaster)
	{
		bool		found;
		ShmemIndexEnt *result = (ShmemIndexEnt *)
			hash_search(ShmemIndex, "ShmemIndex", HASH_ENTER, &found);

		Assert(!found);
		result->size = ShmemAllocator->index_size;
		result->allocated_size = ShmemAllocator->index_size;
		result->minimum_size = result->size;
		result->maximum_size = result->size;
		result->reserved_space = result->allocated_size;
		result->location = ShmemAllocator->index;
	}
}

/*
 * Reset state on postmaster crash restart.
 */
void
ResetShmemAllocator(void)
{
	Assert(!IsUnderPostmaster);
	shmem_request_state = SRS_INITIAL;

	pending_shmem_requests = NIL;

	/*
	 * Note that we don't clear the registered callbacks.  We will need to
	 * call them again as we restart
	 */
}

/*
 * ShmemAlloc -- allocate max-aligned chunk from shared memory
 *
 * Throws error if request cannot be satisfied.
 *
 * Assumes ShmemSegHdr is initialized.
 */
void *
ShmemAlloc(Size size)
{
	void	   *newSpace;
	Size		allocated_size;

	newSpace = ShmemAllocRaw(size, 0, &allocated_size);
	if (!newSpace)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory (%zu bytes requested)",
						size)));
	return newSpace;
}

/*
 * ShmemAllocNoError -- allocate max-aligned chunk from shared memory
 *
 * As ShmemAlloc, but returns NULL if out of space, rather than erroring.
 */
void *
ShmemAllocNoError(Size size)
{
	Size		allocated_size;

	return ShmemAllocRaw(size, 0, &allocated_size);
}

/*
 * ShmemAllocRaw -- allocate align chunk and return allocated size
 *
 * Also sets *allocated_size to the number of bytes allocated, which will
 * be equal to the number requested plus any padding we choose to add.
 */
static void *
ShmemAllocRaw(Size size, Size alignment, Size *allocated_size)
{
	Size		rawStart;
	Size		newStart;
	Size		newFree;
	void	   *newSpace;

	/*
	 * Ensure all space is adequately aligned.  We used to only MAXALIGN this
	 * space but experience has proved that on modern systems that is not good
	 * enough.  Many parts of the system are very sensitive to critical data
	 * structures getting split across cache line boundaries.  To avoid that,
	 * attempt to align the beginning of the allocation to a cache line
	 * boundary.  The calling code will still need to be careful about how it
	 * uses the allocated space - e.g. by padding each element in an array of
	 * structures out to a power-of-two size - but without this, even that
	 * won't be sufficient.
	 */
	if (alignment < PG_CACHE_LINE_SIZE)
		alignment = PG_CACHE_LINE_SIZE;

	Assert(ShmemSegHdr != NULL);

	SpinLockAcquire(&ShmemAllocator->shmem_lock);

	rawStart = ShmemAllocator->free_offset;
	newStart = TYPEALIGN(alignment, rawStart);

	newFree = newStart + size;
	if (newFree <= ShmemSegHdr->totalsize)
	{
		newSpace = (char *) ShmemBase + newStart;
		ShmemAllocator->free_offset = newFree;
	}
	else
		newSpace = NULL;

	SpinLockRelease(&ShmemAllocator->shmem_lock);

	/* note this assert is okay with newSpace == NULL */
	Assert(newSpace == (void *) TYPEALIGN(alignment, newSpace));

	*allocated_size = newFree - rawStart;
	return newSpace;
}

/*
 * ShmemAddrIsValid -- test if an address refers to shared memory
 *
 * Returns true if the pointer points within the shared memory segment.
 */
bool
ShmemAddrIsValid(const void *addr)
{
	return (addr >= ShmemBase) && (addr < ShmemEnd);
}

/*
 * Register callbacks that define a shared memory area (or multiple areas).
 *
 * The system will call the callbacks at different stages of postmaster or
 * backend startup, to allocate and initialize the area.
 *
 * This is normally called early during postmaster startup, but if the
 * SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP is set, this can also be used after
 * startup, although after startup there's no guarantee that there's enough
 * shared memory available.  When called after startup, this immediately calls
 * the right callbacks depending on whether another backend had already
 * initialized the area.
 *
 * Note: In EXEC_BACKEND mode, this needs to be called in every backend
 * process.  That's needed because we cannot pass down the callback function
 * pointers from the postmaster process, because different processes may have
 * loaded libraries to different addresses.
 */
void
RegisterShmemCallbacks(const ShmemCallbacks *callbacks)
{
	if (shmem_request_state == SRS_DONE)
	{
		/*
		 * After-startup initialization or attachment.  Call the appropriate
		 * callbacks immediately.
		 *
		 * This is not allowed from the postmaster, because the postmaster
		 * cannot acquire locks.
		 */
		if ((callbacks->flags & SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP) == 0)
			elog(ERROR, "cannot request shared memory at this time");
		Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

		CallShmemCallbacksAfterStartup(callbacks);
	}
	else if (shmem_request_state == SRS_INITIAL)
	{
		/* Remember the callbacks for later */
		registered_shmem_callbacks = lappend(registered_shmem_callbacks,
											 (void *) callbacks);
	}
	else
		elog(ERROR, "cannot request shared memory at this time");
}

/*
 * Register a shmem area (or multiple areas) after startup.
 */
static void
CallShmemCallbacksAfterStartup(const ShmemCallbacks *callbacks)
{
	bool		found_any;
	bool		notfound_any;

	Assert(shmem_request_state == SRS_DONE);
	shmem_request_state = SRS_REQUESTING;

	/*
	 * Call the request callback first.  The callback makes ShmemRequest*()
	 * calls for each shmem area, adding them to pending_shmem_requests.
	 */
	Assert(pending_shmem_requests == NIL);
	if (callbacks->request_fn)
		callbacks->request_fn(callbacks->opaque_arg);
	shmem_request_state = SRS_AFTER_STARTUP_ATTACH_OR_INIT;

	if (pending_shmem_requests == NIL)
	{
		shmem_request_state = SRS_DONE;
		return;
	}

	/*
	 * Hold ShmemIndexLock while we allocate all the shmem entries and run all
	 * the initializers.
	 */
	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);

	/*
	 * Check if the requested shared memory areas have already been
	 * initialized.  We assume all the areas requested by the request callback
	 * to form a coherent unit such that they're all already initialized or
	 * none.  Otherwise it would be ambiguous which callback, init or attach,
	 * to callback afterwards.
	 */
	found_any = notfound_any = false;
	foreach_ptr(ShmemRequest, request, pending_shmem_requests)
	{
		if (hash_search(ShmemIndex, request->options->name, HASH_FIND, NULL))
			found_any = true;
		else
			notfound_any = true;
	}
	if (found_any && notfound_any)
		elog(ERROR, "some of the requested shmem areas have already been initialized");

	/*
	 * Allocate or attach all the shmem areas requested by the request_fn
	 * callback.
	 */
	foreach_ptr(ShmemRequest, request, pending_shmem_requests)
	{
		if (found_any)
			AttachShmemIndexEntry(request, false);
		else
			InitShmemIndexEntry(request);

		pfree(request->options);
	}
	list_free_deep(pending_shmem_requests);
	pending_shmem_requests = NIL;

	/* Finish by calling the appropriate subsystem-specific callback */
	if (found_any)
	{
		if (callbacks->attach_fn)
			callbacks->attach_fn(callbacks->opaque_arg);
	}
	else
	{
		if (callbacks->init_fn)
			callbacks->init_fn(callbacks->opaque_arg);
	}

	LWLockRelease(ShmemIndexLock);
	shmem_request_state = SRS_DONE;
}

/*
 * Call all shmem request callbacks.
 */
void
ShmemCallRequestCallbacks(void)
{
	ListCell   *lc;

	Assert(shmem_request_state == SRS_INITIAL);
	shmem_request_state = SRS_REQUESTING;

	foreach(lc, registered_shmem_callbacks)
	{
		const ShmemCallbacks *callbacks = (const ShmemCallbacks *) lfirst(lc);

		if (callbacks->request_fn)
			callbacks->request_fn(callbacks->opaque_arg);
	}
}

/*
 * ShmemInitStruct -- Create/attach to a structure in shared memory.
 *
 *		This is called during initialization to find or allocate
 *		a data structure in shared memory.  If no other process
 *		has created the structure, this routine allocates space
 *		for it.  If it exists already, a pointer to the existing
 *		structure is returned.
 *
 *	Returns: pointer to the object.  *foundPtr is set true if the object was
 *		already in the shmem index (hence, already initialized).
 *
 * Note: This is a legacy interface, kept for backwards compatibility with
 * extensions.  Use ShmemRequestStruct() in new code!
 */
void *
ShmemInitStruct(const char *name, Size size, bool *foundPtr)
{
	void	   *ptr = NULL;
	ShmemStructOpts options = {
		.name = name,
		.size = size,
		.ptr = &ptr,
	};
	ShmemRequest request = {&options, SHMEM_KIND_STRUCT};

	Assert(shmem_request_state == SRS_DONE ||
		   shmem_request_state == SRS_INITIALIZING ||
		   shmem_request_state == SRS_REQUESTING);

	LWLockAcquire(ShmemIndexLock, LW_EXCLUSIVE);

	/*
	 * During postmaster startup, look up the existing entry if any.
	 */
	*foundPtr = false;
	if (IsUnderPostmaster)
		*foundPtr = AttachShmemIndexEntry(&request, true);

	/* Initialize it if not found */
	if (!*foundPtr)
		InitShmemIndexEntry(&request);

	LWLockRelease(ShmemIndexLock);

	Assert(ptr != NULL);
	return ptr;
}

/* SQL SRF showing allocated shared memory */
Datum
pg_get_shmem_allocations(PG_FUNCTION_ARGS)
{
#define PG_GET_SHMEM_SIZES_COLS 7
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hstat;
	ShmemIndexEnt *ent;
	Size		named_allocated = 0;
	Datum		values[PG_GET_SHMEM_SIZES_COLS];
	bool		nulls[PG_GET_SHMEM_SIZES_COLS];

	InitMaterializedSRF(fcinfo, 0);

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	hash_seq_init(&hstat, ShmemIndex);

	/* output all allocated entries */
	memset(nulls, 0, sizeof(nulls));
	while ((ent = (ShmemIndexEnt *) hash_seq_search(&hstat)) != NULL)
	{
		values[0] = CStringGetTextDatum(ent->key);
		values[1] = Int64GetDatum((char *) ent->location - (char *) ShmemSegHdr);
		values[2] = Int64GetDatum(ent->size);
		values[3] = Int64GetDatum(ent->allocated_size);
		values[4] = Int64GetDatum(ent->minimum_size);
		values[5] = Int64GetDatum(ent->maximum_size);
		values[6] = Int64GetDatum(ent->reserved_space);

		/*
		 * Anonymous areas are allocated in the area remaining after all named
		 * areas have been allocated. Thus the amount of shared memory
		 * allocated for anonymous areas can be calculated as the total amount
		 * of space allocated minus the amount of space allocated for named
		 * areas. The amount of free shared memory at the end of the segment
		 * can be calculated as the total size of the segment minus the total
		 * amount of space allocated.
		 */
		named_allocated += ent->reserved_space;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	/* output shared memory allocated but not counted via the shmem index */
	values[0] = CStringGetTextDatum("<anonymous>");
	nulls[1] = true;
	values[2] = Int64GetDatum(ShmemAllocator->free_offset - named_allocated);
	values[3] = values[2];
	values[4] = values[2];
	values[5] = values[2];
	values[6] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	/* output as-of-yet unused shared memory */
	nulls[0] = true;
	values[1] = Int64GetDatum(ShmemAllocator->free_offset);
	nulls[1] = false;
	values[2] = Int64GetDatum(ShmemSegHdr->totalsize - ShmemAllocator->free_offset);
	values[3] = values[2];
	values[4] = values[2];
	values[5] = values[2];
	values[6] = values[2];
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	LWLockRelease(ShmemIndexLock);

	return (Datum) 0;
}

/*
 * SQL SRF showing NUMA memory nodes for allocated shared memory
 *
 * Compared to pg_get_shmem_allocations(), this function does not return
 * information about shared anonymous allocations and unused shared memory.
 */
Datum
pg_get_shmem_allocations_numa(PG_FUNCTION_ARGS)
{
#define PG_GET_SHMEM_NUMA_SIZES_COLS 3
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hstat;
	ShmemIndexEnt *ent;
	Datum		values[PG_GET_SHMEM_NUMA_SIZES_COLS];
	bool		nulls[PG_GET_SHMEM_NUMA_SIZES_COLS];
	Size		os_page_size;
	void	  **page_ptrs;
	int		   *pages_status;
	uint64		shm_total_page_count,
				shm_ent_page_count,
				max_nodes;
	Size	   *nodes;

	if (pg_numa_init() == -1)
		elog(ERROR, "libnuma initialization failed or NUMA is not supported on this platform");

	InitMaterializedSRF(fcinfo, 0);

	max_nodes = pg_numa_get_max_node();
	nodes = palloc_array(Size, max_nodes + 2);

	/*
	 * Shared memory allocations can vary in size and may not align with OS
	 * memory page boundaries, while NUMA queries work on pages.
	 *
	 * To correctly map each allocation to NUMA nodes, we need to: 1.
	 * Determine the OS memory page size. 2. Align each allocation's start/end
	 * addresses to page boundaries. 3. Query NUMA node information for all
	 * pages spanning the allocation.
	 */
	os_page_size = pg_get_shmem_pagesize();

	/*
	 * Allocate memory for page pointers and status based on total shared
	 * memory size. This simplified approach allocates enough space for all
	 * pages in shared memory rather than calculating the exact requirements
	 * for each segment.
	 *
	 * Add 1, because we don't know how exactly the segments align to OS
	 * pages, so the allocation might use one more memory page. In practice
	 * this is not very likely, and moreover we have more entries, each of
	 * them using only fraction of the total pages.
	 */
	shm_total_page_count = (ShmemSegHdr->totalsize / os_page_size) + 1;
	page_ptrs = palloc0_array(void *, shm_total_page_count);
	pages_status = palloc_array(int, shm_total_page_count);

	if (firstNumaTouch)
		elog(DEBUG1, "NUMA: page-faulting shared memory segments for proper NUMA readouts");

	LWLockAcquire(ShmemIndexLock, LW_SHARED);

	hash_seq_init(&hstat, ShmemIndex);

	/* output all allocated entries */
	while ((ent = (ShmemIndexEnt *) hash_seq_search(&hstat)) != NULL)
	{
		char	   *startptr,
				   *endptr;
		Size		total_len;

		/*
		 * Calculate the range of OS pages used by this segment. The segment
		 * may start / end half-way through a page, we want to count these
		 * pages too. So we align the start/end pointers down/up, and then
		 * calculate the number of pages from that.
		 */
		startptr = (char *) TYPEALIGN_DOWN(os_page_size, ent->location);
		endptr = (char *) TYPEALIGN(os_page_size,
									(char *) ent->location + ent->allocated_size);
		total_len = (endptr - startptr);

		shm_ent_page_count = total_len / os_page_size;

		/*
		 * If we ever get 0xff (-1) back from kernel inquiry, then we probably
		 * have a bug in mapping buffers to OS pages.
		 */
		memset(pages_status, 0xff, sizeof(int) * shm_ent_page_count);

		/*
		 * Setup page_ptrs[] with pointers to all OS pages for this segment,
		 * and get the NUMA status using pg_numa_query_pages.
		 *
		 * In order to get reliable results we also need to touch memory
		 * pages, so that inquiry about NUMA memory node doesn't return -2
		 * (ENOENT, which indicates unmapped/unallocated pages).
		 */
		for (uint64 i = 0; i < shm_ent_page_count; i++)
		{
			page_ptrs[i] = startptr + (i * os_page_size);

			if (firstNumaTouch)
				pg_numa_touch_mem_if_required(page_ptrs[i]);

			CHECK_FOR_INTERRUPTS();
		}

		if (pg_numa_query_pages(0, shm_ent_page_count, page_ptrs, pages_status) == -1)
			elog(ERROR, "failed NUMA pages inquiry status: %m");

		/* Count number of NUMA nodes used for this shared memory entry */
		memset(nodes, 0, sizeof(Size) * (max_nodes + 2));

		for (uint64 i = 0; i < shm_ent_page_count; i++)
		{
			int			s = pages_status[i];

			/* Ensure we are adding only valid index to the array */
			if (s >= 0 && s <= max_nodes)
			{
				/* valid NUMA node */
				nodes[s]++;
				continue;
			}
			else if (s == -2)
			{
				/* -2 means ENOENT (e.g. page was moved to swap) */
				nodes[max_nodes + 1]++;
				continue;
			}

			elog(ERROR, "invalid NUMA node id outside of allowed range "
				 "[0, " UINT64_FORMAT "]: %d", max_nodes, s);
		}

		/* no NULLs for regular nodes */
		memset(nulls, 0, sizeof(nulls));

		/*
		 * Add one entry for each NUMA node, including those without allocated
		 * memory for this segment.
		 */
		for (uint64 i = 0; i <= max_nodes; i++)
		{
			values[0] = CStringGetTextDatum(ent->key);
			values[1] = Int32GetDatum(i);
			values[2] = Int64GetDatum(nodes[i] * os_page_size);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}

		/* The last entry is used for pages without a NUMA node. */
		nulls[1] = true;
		values[0] = CStringGetTextDatum(ent->key);
		values[2] = Int64GetDatum(nodes[max_nodes + 1] * os_page_size);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	LWLockRelease(ShmemIndexLock);
	firstNumaTouch = false;

	return (Datum) 0;
}

/*
 * Determine the memory page size used for the shared memory segment.
 *
 * If the shared segment was allocated using huge pages, returns the size of
 * a huge page. Otherwise returns the size of regular memory page.
 *
 * This should be used only after the server is started.
 */
Size
pg_get_shmem_pagesize(void)
{
	Assert(IsUnderPostmaster);

	return GetOSPageSize();
}

Datum
pg_numa_available(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pg_numa_init() != -1);
}
