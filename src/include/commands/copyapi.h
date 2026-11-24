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
 *
 * A custom format can keep its own state for the COPY in
 * cstate->format_private.
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
	 *
	 * For the built-in formats only.  A custom format leaves this NULL, and
	 * gets the text output functions in cstate->out_functions.
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
 * Information about a custom format, passed to RegisterCopyCustomFormat().
 */
typedef struct CopyCustomFormatEntry
{
	const char *name;			/* format name, stored by reference */
	bool		binary;			/* use binary format in the COPY protocol? */
	const CopyToRoutine *to_routine;
} CopyCustomFormatEntry;

extern void RegisterCopyCustomFormat(const CopyCustomFormatEntry *entry);

extern const CopyCustomFormatEntry *GetCopyCustomFormatEntry(const char *name);

/*
 * Functions for a format to exchange data with the source or destination of
 * COPY, which can be a file, a program, the client, or a callback.
 *
 * CopyToSendData() appends data to the output buffer, and CopyToFlushData()
 * sends the buffered data to the destination.  A format must flush the
 * buffered data by the end of its CopyToEnd callback.  Unlike the built-in
 * text-like formats, these functions don't add any line termination.
 *
 * CopyFromGetData() reads at least 'minread' and at most 'maxread' bytes into
 * 'databuf', and returns the number of bytes read; if this is less than
 * 'minread', EOF was detected.
 *
 * None of these functions applies encoding conversion.  A format converts
 * the data with CopyToConvertFromServer() and CopyFromConvertToServer(),
 * between the server encoding and the encoding specified by the ENCODING
 * option, or the client encoding when it isn't specified.  Both return 'str'
 * itself if no conversion is needed, and a palloc'd null-terminated string
 * otherwise.  CopyFromConvertToServer() also verifies that the data is valid
 * in the encoding.
 */
extern void CopyToSendData(CopyToState cstate, const void *databuf,
						   int datasize);
extern void CopyToFlushData(CopyToState cstate);
extern char *CopyToConvertFromServer(CopyToState cstate, const char *str,
									 int len);
extern int	CopyFromGetData(CopyFromState cstate, void *databuf, int minread,
							int maxread);
extern char *CopyFromConvertToServer(CopyFromState cstate, const char *str,
									 int len);

#endif							/* COPYAPI_H */
