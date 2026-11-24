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
 *
 * A custom format can keep its own state for the COPY in
 * cstate->format_private.
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
	 *
	 * For the built-in formats only.  A custom format leaves this NULL, and
	 * core converts its fields with the text input functions.
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
	 * For a custom format, the arrays have one element for each column of
	 * cstate->attnumlist, in that order, and are initialized to null.  Core
	 * sets the other columns of the row, to their default values or null. The
	 * built-in formats, which are part of core, are given the arrays of the
	 * whole row instead.
	 *
	 * 'econtext' is used to evaluate default expression for each column that
	 * is either not read from the file or is using the DEFAULT option of COPY
	 * FROM. It is NULL if no default values are used.
	 *
	 * Returns false if there are no more tuples to read, and true once a
	 * record has been read, even if core drops the row for ON_ERROR IGNORE.
	 * If the format's data ends before the input does, for example at an end
	 * marker, core reports an error for any data that follows, but it can't
	 * see what the format has already read into a buffer of its own.
	 *
	 * A custom format sets each column either to a Datum it builds by itself,
	 * or by one of the helpers below, which let core convert a field and
	 * implement ON_ERROR.
	 *
	 * A Datum that the format builds is a value of the base type of the
	 * column, without the typmod applied, as an input function would parse
	 * it.  Core applies the typmod and the domain constraints of the column
	 * to it once the callback returns, and ON_ERROR applies to the errors
	 * there too.  Core can't apply the typmod of an array type, such as
	 * varchar(3)[], so a format must use the helpers for such a column.
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
	const CopyToRoutine *to_routine;	/* NULL if COPY TO is not supported */
	const CopyFromRoutine *from_routine;	/* NULL if COPY FROM is not
											 * supported */
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

/*
 * Helpers for the CopyFromOneRow callback of a custom format, which let core
 * set a column from a field of the record.  'i' is the position of the column
 * in cstate->attnumlist, and 'values' and 'nulls' are the arrays passed to the
 * callback.
 *
 * CopyFromConvertField() converts 'str', a null-terminated string in the
 * server encoding or NULL for a null value, with the input function of the
 * column, and stores the result in values[i] and nulls[i].  This is where
 * ON_ERROR applies: if the value is invalid and ON_ERROR is IGNORE, it
 * returns false and core drops the row.  The callback may then return true
 * without converting the remaining fields, but the input must be at the next
 * record by then.  Once the row is to be dropped, the helpers do nothing.
 *
 * CopyFromSetDefault() sets the column to its default value, for a field that
 * was the DEFAULT marker.  It may be used only if the DEFAULT option is
 * given.  Core evaluates the default value once the callback returns, unless
 * the row is dropped.
 *
 * CopyFromConvertFields() does the above for a record whose fields are in the
 * order of cstate->attnumlist, with the same checks for missing and extra
 * fields as the text format.  'isdefault' tells which fields were the
 * DEFAULT marker, and may be NULL.
 */
extern bool CopyFromConvertField(CopyFromState cstate, int i, char *str,
								 Datum *values, bool *nulls);
extern void CopyFromSetDefault(CopyFromState cstate, int i);
extern bool CopyFromConvertFields(CopyFromState cstate, char **fields,
								  bool *isdefault, int nfields,
								  Datum *values, bool *nulls);

/*
 * For a custom format, core counts the records read for the line number shown
 * in the context of error messages.  CopyFromSetErrorLine() sets the current
 * record, in the encoding of the COPY data, to be quoted there.  It's
 * forgotten when the next record is read.
 */
extern void CopyFromSetErrorLine(CopyFromState cstate, const char *data,
								 int len);

#endif							/* COPYAPI_H */
