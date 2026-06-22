/*-------------------------------------------------------------------------
 *
 * copyapi.c
 *	  Registry for pluggable COPY TO/FROM format handlers.
 *
 * The built-in formats (text, csv, binary, json) are dispatched directly by
 * the COPY engine. Extensions can provide additional formats by registering
 * a CopyToRoutine and/or CopyFromRoutine under a name from their _PG_init();
 * ProcessCopyOptions() then resolves "COPY ... (FORMAT 'name')" against this
 * registry.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/copyapi.h"
#include "utils/memutils.h"

static CopyCustomFormatEntry *CopyCustomFormatArray = NULL;
static int	CopyCustomFormatsAssigned = 0;
static int	CopyCustomFormatsAllocated = 0;

/* Is 'name' one of the built-in format keywords? */
static bool
is_builtin_copy_format(const char *name)
{
	return (strcmp(name, "text") == 0 ||
			strcmp(name, "csv") == 0 ||
			strcmp(name, "binary") == 0 ||
			strcmp(name, "json") == 0);
}

/*
 * Register a custom COPY format. Intended to be called from an extension's
 * _PG_init(). Either routine may be NULL if the format does not support that
 * direction (but not both).
 *
 * 'option_fn' may also be NULL if the format takes no format-specific options.
 *
 * 'name' is assumed to be a constant string or allocated in storage that will
 * never be freed; it is stored by reference.
 */
void
RegisterCopyCustomFormat(const char *name, const CopyToRoutine *to,
						 const CopyFromRoutine *from, ProcessOneCopyOptionFn option_fn)
{
	Assert(name != NULL && name[0] != '\0');

	/* Must support at least one direction */
	Assert(to != NULL || from != NULL);

	Assert(to == NULL ||
		   (to->CopyToStart != NULL && to->CopyToOneRow != NULL &&
			to->CopyToEnd != NULL));
	Assert(from == NULL ||
		   (from->CopyFromStart != NULL && from->CopyFromOneRow != NULL &&
			from->CopyFromEnd != NULL));

	/* Check if it's already used by built-in format names */
	if (is_builtin_copy_format(name))
		elog(ERROR, "COPY format \"%s\" is a built-in format name", name);

	/* Reject a duplicate registration. */
	for (int i = 0; i < CopyCustomFormatsAssigned; i++)
	{
		if (strcmp(CopyCustomFormatArray[i].name, name) == 0)
			elog(ERROR, "COPY format \"%s\" is already registered", name);
	}

	/* Create the array on first use; it must outlive the current context. */
	if (CopyCustomFormatArray == NULL)
	{
		CopyCustomFormatsAllocated = 16;
		CopyCustomFormatArray = (CopyCustomFormatEntry *)
			MemoryContextAlloc(TopMemoryContext,
							   CopyCustomFormatsAllocated * sizeof(CopyCustomFormatEntry));
	}

	/* Expand if full. */
	if (CopyCustomFormatsAssigned >= CopyCustomFormatsAllocated)
	{
		CopyCustomFormatsAllocated *= 2;
		CopyCustomFormatArray = (CopyCustomFormatEntry *)
			repalloc_array(CopyCustomFormatArray, CopyCustomFormatEntry, CopyCustomFormatsAllocated);
	}

	CopyCustomFormatArray[CopyCustomFormatsAssigned].name = name;
	CopyCustomFormatArray[CopyCustomFormatsAssigned].to_routine = to;
	CopyCustomFormatArray[CopyCustomFormatsAssigned].from_routine = from;
	CopyCustomFormatArray[CopyCustomFormatsAssigned].option_fn = option_fn;
	CopyCustomFormatsAssigned++;
}

/*
 * Look up a previously registered custom format. Returns NULL if 'name' is
 * not registered.
 */
const CopyCustomFormatEntry *
GetCopyCustomFormatRoutines(const char *name)
{
	for (int i = 0; i < CopyCustomFormatsAssigned; i++)
	{
		if (strcmp(CopyCustomFormatArray[i].name, name) == 0)
			return &(CopyCustomFormatArray[i]);
	}

	return NULL;
}
