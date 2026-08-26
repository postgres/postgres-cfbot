/*-------------------------------------------------------------------------
 *
 * copyapi.h
 *	  API for COPY TO/FROM handlers
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/copyapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYAPI_H
#define COPYAPI_H

#include "commands/copy_state.h"
#include "commands/copy.h"

/*
 * API structure for a COPY TO format implementation. Note this must be
 * allocated in a server-lifetime manner, typically as a static const struct.
 */
typedef struct CopyToRoutine
{
	/*
	 * Set output function information. This callback is called once at the
	 * beginning of COPY TO.
	 *
	 * 'finfo' can be optionally filled to provide the catalog information of
	 * the output function.
	 *
	 * 'atttypid' is the OID of data type used by the relation's attribute.
	 */
	void		(*CopyToOutFunc) (CopyToState cstate, Oid atttypid,
								  FmgrInfo *finfo);

	/*
	 * Start a COPY TO. This callback is called once at the beginning of COPY
	 * TO.
	 *
	 * 'tupDesc' is the tuple descriptor of the relation from where the data
	 * is read.
	 */
	void		(*CopyToStart) (CopyToState cstate, TupleDesc tupDesc);

	/*
	 * Write one row stored in 'slot' to the destination.
	 */
	void		(*CopyToOneRow) (CopyToState cstate, TupleTableSlot *slot);

	/*
	 * End a COPY TO. This callback is called once at the end of COPY TO.
	 */
	void		(*CopyToEnd) (CopyToState cstate);
} CopyToRoutine;

/*
 * API structure for a COPY FROM format implementation. Note this must be
 * allocated in a server-lifetime manner, typically as a static const struct.
 */
typedef struct CopyFromRoutine
{
	/*
	 * Set input function information. This callback is called once at the
	 * beginning of COPY FROM.
	 *
	 * 'finfo' can be optionally filled to provide the catalog information of
	 * the input function.
	 *
	 * 'typioparam' can be optionally filled to define the OID of the type to
	 * pass to the input function.'atttypid' is the OID of data type used by
	 * the relation's attribute.
	 */
	void		(*CopyFromInFunc) (CopyFromState cstate, Oid atttypid,
								   FmgrInfo *finfo, Oid *typioparam);

	/*
	 * Start a COPY FROM. This callback is called once at the beginning of
	 * COPY FROM.
	 *
	 * 'tupDesc' is the tuple descriptor of the relation where the data needs
	 * to be copied. This can be used for any initialization steps required by
	 * a format.
	 */
	void		(*CopyFromStart) (CopyFromState cstate, TupleDesc tupDesc);

	/*
	 * Read one row from the source and fill *values and *nulls.
	 *
	 * 'econtext' is used to evaluate default expression for each column that
	 * is either not read from the file or is using the DEFAULT option of COPY
	 * FROM. It is NULL if no default values are used.
	 *
	 * Returns false if there are no more tuples to read.
	 */
	bool		(*CopyFromOneRow) (CopyFromState cstate, ExprContext *econtext,
								   Datum *values, bool *nulls);

	/*
	 * End a COPY FROM. This callback is called once at the end of COPY FROM.
	 */
	void		(*CopyFromEnd) (CopyFromState cstate);
} CopyFromRoutine;

/*
 * Optional callback to process one format-specific COPY option. Invoked
 * from ProcessCopyOptions() once per option that core did not recognize, after
 * every core option has been parsed (so 'opts' is fully populated).
 *
 * Returns true if the option belongs to the format and is valid. Returns false
 * if the option is not one the format recognizes, in which case core raises the
 * "not accepted" error; thus an unrecognized option always errors, whether or
 * not the format supplies this callback. For a recognized option with an invalid
 * value, the callback should ereport() itself.
 */
typedef bool (*ProcessOneCopyOptionFn) (CopyFormatOptions *opts, bool is_from,
										DefElem *option);

/*
 * Optional callback to validate a custom format's fully-parsed options as a
 * whole. Invoked once from ProcessCopyOptions() after all options have been
 * processed, so it can enforce cross-option constraints and reject
 * incompatible core options. It runs even when no format-specific options were
 * supplied. Reports problems with ereport().
 */
typedef void (*ValidateCopyOptionsFn) (CopyFormatOptions *opts, bool is_from);

/*
 * Sturct to store the registered custom format information.
 */
typedef struct CopyCustomFormatEntry
{
	const char *name;			/* constant string; never freed (see below) */
	const CopyToRoutine *to_routine;
	const CopyFromRoutine *from_routine;
	ProcessOneCopyOptionFn option_fn;
	ValidateCopyOptionsFn validate_fn;
} CopyCustomFormatEntry;

extern void RegisterCopyCustomFormat(const char *name, const CopyToRoutine *to,
									 const CopyFromRoutine *from,
									 ProcessOneCopyOptionFn option_fn,
									 ValidateCopyOptionsFn validate_fn);

extern const CopyCustomFormatEntry *GetCopyCustomFormatRoutines(const char *name);

#endif							/* COPYAPI_H */
