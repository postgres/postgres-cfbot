/*-------------------------------------------------------------------------
 *
 * test_shmem.c
 *		Helpers to test shmem management routines
 *
 * Test fixed-size and resizable shared memory structures created during
 * postmaster startup and after startup respectively.
 *
 * Copyright (c) 2020-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shmem/test_shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "commands/extension.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"


PG_MODULE_MAGIC;


/* ----------------------------------------------------------------
 *						Fixed-size shared memory structure
 * ----------------------------------------------------------------
 */

typedef struct TestShmemData
{
	int			value;
	bool		initialized;
	int			attach_count;
} TestShmemData;

static TestShmemData *TestShmem;

static bool attached_or_initialized = false;

static void
test_shmem_request(void *arg)
{
	elog(LOG, "test_shmem_request callback called");

	ShmemRequestStruct(.name = "test_shmem area",
					   .size = sizeof(TestShmemData),
					   .ptr = (void **) &TestShmem);
}

static void
test_shmem_init(void *arg)
{
	elog(LOG, "init callback called");

	/*
	 * Reset the per-process flag and the shared "initialized" marker during
	 * postmaster induced restart.
	 */
	if (!IsUnderPostmaster)
	{
		attached_or_initialized = false;
		TestShmem->initialized = false;
	}

	if (TestShmem->initialized)
		elog(ERROR, "shmem area already initialized");
	TestShmem->initialized = true;

	if (attached_or_initialized)
		elog(ERROR, "attach or initialize already called in this process");
	attached_or_initialized = true;
}

static void
test_shmem_attach(void *arg)
{
	elog(LOG, "test_shmem_attach callback called");
	if (!TestShmem->initialized)
		elog(ERROR, "shmem area not yet initialized");
	TestShmem->attach_count++;

	if (attached_or_initialized)
		elog(ERROR, "attach or initialize already called in this process");
	attached_or_initialized = true;
}

static const ShmemCallbacks TestShmemCallbacks = {
	.flags = SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP,
	.request_fn = test_shmem_request,
	.init_fn = test_shmem_init,
	.attach_fn = test_shmem_attach,
};

PG_FUNCTION_INFO_V1(get_test_shmem_attach_count);
Datum
get_test_shmem_attach_count(PG_FUNCTION_ARGS)
{
	if (!attached_or_initialized)
		elog(ERROR, "shmem area not attached or initialized in this process");
	if (!TestShmem->initialized)
		elog(ERROR, "shmem area not yet initialized");
	PG_RETURN_INT32(TestShmem->attach_count);
}

/*
 * Attempt to resize the fixed-size shared memory structure.  This should
 * fail because the structure was not allocated with a maximum_size.
 */
PG_FUNCTION_INFO_V1(test_shmem_resize_fixed);
Datum
test_shmem_resize_fixed(PG_FUNCTION_ARGS)
{
	int32		new_size = PG_GETARG_INT32(0);

	ShmemResizeStruct("test_shmem area", new_size);
	PG_RETURN_VOID();
}


/* ----------------------------------------------------------------
 *						Resizable shared memory structure
 * ----------------------------------------------------------------
 */

/*
 * The test module may be loaded after postmaster startup in which case only
 * 100K of shared memory is available for the extension. Keep the default
 * initial and maximum sizes small enough to fit in that space.
 */
#define TEST_INITIAL_ENTRIES_DEFAULT		1
#define TEST_MAX_ENTRIES_DEFAULT			1024

#define TEST_ENTRY_SIZE			sizeof(int32)	/* Size of each entry */

/*
 * Resizable test data structure stored in shared memory.
 *
 * The test performs resizing, reads or writes, only one at a time and never
 * concurrently. Hence, there is no need for locks in the test structure.
 */
typedef struct TestResizableShmemStruct
{
	/* Metadata */
	int32		num_entries;	/* Number of entries that can fit */

	/* Data area - variable size */
	int32		data[FLEXIBLE_ARRAY_MEMBER];
} TestResizableShmemStruct;

static TestResizableShmemStruct *resizable_shmem = NULL;

/* GUC variables controlling the size of the test structure */
static int	test_initial_entries;
static int	test_max_entries;

/* Whether to use SHMEM_ATTACH_UNKNOWN_SIZE when attaching to the shared memory */
/* TODO: We may use opaque_arg to pass this value to the request function.*/
static bool use_unknown_size = false;

/*
 * Request shared memory resources.
 */
static void
resizable_shmem_request(void *arg)
{
	Size		initial_size = add_size(offsetof(TestResizableShmemStruct, data),
										mul_size(test_initial_entries, TEST_ENTRY_SIZE));

/*
 * Create resizable structure on the platforms which support it. Otherwise create
 * as a fixed-size structure. Other way would be to conditionally include
 * .maximum_size in the call to ShmemRequestStruct().
 */
#ifdef HAVE_RESIZABLE_SHMEM
	Size		max_size = add_size(offsetof(TestResizableShmemStruct, data),
									mul_size(test_max_entries, TEST_ENTRY_SIZE));
	Size		min_size = offsetof(TestResizableShmemStruct, data);
#else
	Size		max_size = 0;
	Size		min_size = 0;
#endif

	ShmemRequestStruct(.name = "resizable_shmem",
					   .size = use_unknown_size ? SHMEM_ATTACH_UNKNOWN_SIZE : initial_size,
					   .minimum_size = min_size,
					   .maximum_size = max_size,
					   .ptr = (void **) &resizable_shmem,
		);
}

/*
 * Initialize shared memory structure.
 */
static void
resizable_shmem_shmem_init(void *arg)
{
	Assert(resizable_shmem != NULL);

	resizable_shmem->num_entries = test_initial_entries;
	memset(resizable_shmem->data, 0, mul_size(test_initial_entries, TEST_ENTRY_SIZE));
}

/*
 * Attach to the already-allocated shared memory structure.
 */
static void
resizable_shmem_shmem_attach(void *arg)
{
	Assert(resizable_shmem != NULL);
}

static ShmemCallbacks resizable_shmem_callbacks = {
	.request_fn = resizable_shmem_request,
	.init_fn = resizable_shmem_shmem_init,
	.attach_fn = resizable_shmem_shmem_attach,
};

/*
 * Resize the shared memory structure to accommodate the specified number of
 * entries.
 *
 * Negative value for new_entries can be used to test resizing below the
 * minimum size.
 *
 * Returns true if the resize was successful, false if ShmemResizeStruct()
 * could not allocate the requested memory.  On platforms that do not support
 * resizable shared memory, ShmemResizeStruct() raises an error.
 */
PG_FUNCTION_INFO_V1(resizable_shmem_resize);
Datum
resizable_shmem_resize(PG_FUNCTION_ARGS)
{
	int32		new_entries = PG_GETARG_INT32(0);
	Size		new_size;

	if (!resizable_shmem)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("resizable_shmem is not initialized"));

	if (new_entries < 0)
		new_size = 1;
	else
		new_size = add_size(offsetof(TestResizableShmemStruct, data),
							mul_size(new_entries, TEST_ENTRY_SIZE));
	if (!ShmemResizeStruct("resizable_shmem", new_size))
		PG_RETURN_BOOL(false);

	ShmemProtectStruct("resizable_shmem");
	resizable_shmem->num_entries = new_entries;

	PG_RETURN_BOOL(true);
}

/*
 * Write the given integer value to all entries in the data array.
 */
PG_FUNCTION_INFO_V1(resizable_shmem_write);
Datum
resizable_shmem_write(PG_FUNCTION_ARGS)
{
	int32		entry_value = PG_GETARG_INT32(0);
	int32		i;

	if (!resizable_shmem)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("resizable_shmem is not initialized"));

#ifdef HAVE_RESIZABLE_SHMEM

	/*
	 * Ideally the structure should be protected through a synchronization
	 * cycle across all the backends that may access the structure. But we
	 * don't implement any such synchronization in this test module to keep it
	 * simple. Given that ProcSignalBarrier mechanism is not extensible, we
	 * may not be able to do that as well here. Hence add protect just before
	 * accessing the structure.
	 */
	ShmemProtectStruct("resizable_shmem");
#endif

	for (i = 0; i < resizable_shmem->num_entries; i++)
		resizable_shmem->data[i] = entry_value;

	PG_RETURN_VOID();
}

/*
 * Check whether the first 'entry_count' entries all have the expected 'entry_value'.
 * Returns true if all match, false otherwise.
 */
PG_FUNCTION_INFO_V1(resizable_shmem_read);
Datum
resizable_shmem_read(PG_FUNCTION_ARGS)
{
	int32		entry_count = PG_GETARG_INT32(0);
	int32		entry_value = PG_GETARG_INT32(1);
	int32		i;

	if (resizable_shmem == NULL)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("resizable_shmem is not initialized"));

	if (entry_count < 0 || entry_count > resizable_shmem->num_entries)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("entry_count %d is out of range (0..%d)", entry_count, resizable_shmem->num_entries));

#ifdef HAVE_RESIZABLE_SHMEM

	/*
	 * Ideally the structure should be protected through a synchronization
	 * cycle across all the backends that may access the structure. But we
	 * don't implement any such synchronization in this test module to keep it
	 * simple. Given that ProcSignalBarrier mechanism is not extensible, we
	 * may not be able to do that as well here. Hence add protect just before
	 * accessing the structure.
	 */
	ShmemProtectStruct("resizable_shmem");
#endif

	for (i = 0; i < entry_count; i++)
	{
		if (resizable_shmem->data[i] != entry_value)
			PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}

/*
 * Return the memory mapped against the main shared memory segment in this
 * backend.
 *
 * The VMA containing our resizable_shmem pointer identifies the start of the
 * main shared-memory segment.
 *
 * mprotect() calls issued when the resizable structure grows and shrinks can
 * split the original mmap into several adjacent VMAs, so we sum the accounting
 * fields across the base VMA and any VMAs contiguous with it.
 */
PG_FUNCTION_INFO_V1(test_shmem_usage);
Datum
test_shmem_usage(PG_FUNCTION_ARGS)
{
	FILE	   *f;
	char		line[256];
	uintptr_t	target = (uintptr_t) resizable_shmem;
	bool		in_target_vma = false;
	bool		use_hugetlb = (huge_pages_status == HUGE_PAGES_ON);
	unsigned long prev_end = 0;
	int64		total_rss_kb = 0;
	int64		total_swap_kb = 0;
	int64		total_shared_hugetlb_kb = 0;
	int64		val;
	size_t		result;

	f = AllocateFile("/proc/self/smaps", "r");
	if (f == NULL)
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not open /proc/self/smaps: %m"));

	while (fgets(line, sizeof(line), f) != NULL)
	{
		unsigned long start;
		unsigned long end;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2)
		{
			if (in_target_vma)
			{
				/*
				 * Continue accumulating only across VMAs that are contiguous
				 * with the previous one; stop as soon as we hit a gap or a
				 * different mapping.
				 */
				if (start != prev_end)
					break;
			}
			else
				in_target_vma = (target >= start && target < end);

			prev_end = end;
		}
		else if (in_target_vma)
		{
			if (use_hugetlb)
			{
				if (sscanf(line, "Shared_Hugetlb: %ld kB", &val) == 1)
					total_shared_hugetlb_kb += val;
			}
			else
			{
				if (sscanf(line, "Rss: %ld kB", &val) == 1)
					total_rss_kb += val;
				else if (sscanf(line, "Swap: %ld kB", &val) == 1)
					total_swap_kb += val;
			}
		}
	}

	FreeFile(f);

	if (use_hugetlb)
		result = mul_size(total_shared_hugetlb_kb, 1024);
	else
	{
		result = mul_size(total_rss_kb, 1024);
		result = add_size(result, mul_size(total_swap_kb, 1024));
	}

	PG_RETURN_INT64(result);
}

/*
 * Return the shared memory page size.
 */
PG_FUNCTION_INFO_V1(test_shmem_pagesize);
Datum
test_shmem_pagesize(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(pg_get_shmem_pagesize());
}

/*
 * Walk the entries between the current size and the reserved maximum, accessing
 * each one. Ideally, this function should (seg)fault the moment we try to access
 * the entry outside the currently allocated size, but the memory allocation and
 * protection mechanisms work on page basis. Hence it may only (seg)fault when a
 * page boundary is crossed.  The mode argument selects between "read" and
 * "write" access.
 *
 * When the current end of the structure and end of maximal structure are on the
 * same page, this function may not (seg)fault at all.
 */
PG_FUNCTION_INFO_V1(resizable_shmem_access_beyond_size);
Datum
resizable_shmem_access_beyond_size(PG_FUNCTION_ARGS)
{
	text	   *mode_txt = PG_GETARG_TEXT_PP(0);
	const char *mode = text_to_cstring(mode_txt);
	bool		do_write;
	int32		sink = 0;

	if (!resizable_shmem)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("resizable_shmem is not initialized"));

	if (strcmp(mode, "read") == 0)
		do_write = false;
	else if (strcmp(mode, "write") == 0)
		do_write = true;
	else
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("mode must be \"read\" or \"write\""));

#ifdef HAVE_RESIZABLE_SHMEM

	/*
	 * Ideally the structure should be protected through a synchronization
	 * cycle across all the backends that may access the structure. But we
	 * don't implement any such synchronization in this test module to keep it
	 * simple. Given that ProcSignalBarrier mechanism is not extensible, we
	 * may not be able to do that as well here. Hence add protect just before
	 * accessing the structure.
	 */
	ShmemProtectStruct("resizable_shmem");
#endif

	for (int i = resizable_shmem->num_entries; i < test_max_entries; i++)
	{
		if (do_write)
			resizable_shmem->data[i] = 0xdead;
		else
			sink = resizable_shmem->data[i];
	}

	/*
	 * Return the last read value so that compiler doesn't optimize away the
	 * assignment to sink.
	 */
	PG_RETURN_INT32(sink);
}


/* ----------------------------------------------------------------
 *						Module initialization
 * ----------------------------------------------------------------
 */

void
_PG_init(void)
{
	int			guc_context;

	elog(LOG, "test_shmem module's _PG_init called");

	RegisterShmemCallbacks(&TestShmemCallbacks);

	/*
	 * Use PGC_POSTMASTER when loaded at startup so the values are fixed once
	 * the shared memory segment is created.  When loaded after startup
	 * PGC_POSTMASTER is not allowed, so we use PGC_SIGHUP instead.  Although
	 * we do not intend to change these values at config reload, PGC_SIGHUP is
	 * the least permissive context that allows defining the GUC after startup
	 * and still prevents it from being changed via SET.
	 */
	if (process_shared_preload_libraries_in_progress)
		guc_context = PGC_POSTMASTER;
	else
	{
		guc_context = PGC_SIGHUP;
		resizable_shmem_callbacks.flags = SHMEM_CALLBACKS_ALLOW_AFTER_STARTUP;
	}

	DefineCustomIntVariable("resizable_shmem.initial_entries",
							"Initial number of entries in the test structure.",
							NULL,
							&test_initial_entries,
							TEST_INITIAL_ENTRIES_DEFAULT,
							1,
							INT_MAX,
							guc_context,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("resizable_shmem.max_entries",
							"Maximum number of entries in the test structure.",
							NULL,
							&test_max_entries,
							TEST_MAX_ENTRIES_DEFAULT,
							1,
							INT_MAX,
							guc_context,
							0,
							NULL, NULL, NULL);

	/*
	 * When loaded after startup by a backend that is not creating the
	 * extension, the shared memory might have been resized to a size other
	 * than the initial size. Use SHMEM_ATTACH_UNKNOWN_SIZE to attach without
	 * knowing the exact size.
	 */
	if (!process_shared_preload_libraries_in_progress && !creating_extension)
		use_unknown_size = true;

	RegisterShmemCallbacks(&resizable_shmem_callbacks);
}
