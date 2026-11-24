/*--------------------------------------------------------------------------
 *
 * test_copy_custom_format.c
 *		Code for testing custom COPY format.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_copy_custom_format/test_copy_custom_format.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/copy.h"
#include "commands/copyapi.h"
#include "commands/copy_state.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

/* State of COPY FROM in test_format, which reads lines of the input */
typedef struct TestCopyFromState
{
	char		buf[64];		/* data read from the source */
	int			len;			/* number of bytes in buf */
	int			pos;			/* next byte to process in buf */
	bool		eof;			/* reached the end of the source? */
	StringInfoData line;		/* the line being read */
	bool	   *build_datum;	/* build the Datum of each column by itself? */
} TestCopyFromState;

static void
TestCopyFromStartCommon(CopyFromState cstate, TupleDesc tupDesc,
						bool build_datums)
{
	TestCopyFromState *tstate;
	MemoryContext oldcxt;

	ereport(NOTICE,
			errmsg("CopyFromStart: the number of attributes of table: %d, the number of attributes to input: %d",
				   tupDesc->natts, list_length(cstate->attnumlist)));

	oldcxt = MemoryContextSwitchTo(cstate->copycontext);
	tstate = palloc0_object(TestCopyFromState);
	initStringInfo(&tstate->line);

	/*
	 * In test_datum_format, build the Datums of the text-like columns by
	 * itself, leaving it to core to apply their typmod and domain
	 * constraints.
	 */
	tstate->build_datum = palloc0_array(bool, list_length(cstate->attnumlist));
	if (build_datums)
	{
		int			i = 0;

		foreach_int(attnum, cstate->attnumlist)
		{
			Oid			basetype;

			basetype = getBaseType(TupleDescAttr(tupDesc, attnum - 1)->atttypid);
			tstate->build_datum[i++] = (basetype == TEXTOID ||
										basetype == VARCHAROID ||
										basetype == BPCHAROID);
		}
	}
	MemoryContextSwitchTo(oldcxt);

	cstate->format_private = tstate;
}

static void
TestCopyFromStart(CopyFromState cstate, TupleDesc tupDesc)
{
	TestCopyFromStartCommon(cstate, tupDesc, false);
}

static void
TestCopyDatumFromStart(CopyFromState cstate, TupleDesc tupDesc)
{
	TestCopyFromStartCommon(cstate, tupDesc, true);
}

/*
 * Read the next line into tstate->line, without the newline.  Returns false
 * at the end of the input.
 */
static bool
TestCopyReadLine(CopyFromState cstate, TestCopyFromState *tstate)
{
	resetStringInfo(&tstate->line);

	for (;;)
	{
		char		c;

		if (tstate->pos >= tstate->len)
		{
			if (!tstate->eof)
			{
				tstate->len = CopyFromGetData(cstate, tstate->buf, 1,
											  sizeof(tstate->buf));
				tstate->pos = 0;
				tstate->eof = (tstate->len == 0);
			}
			if (tstate->eof)
				return tstate->line.len > 0;
		}

		c = tstate->buf[tstate->pos++];
		if (c == '\n')
			return true;
		appendStringInfoChar(&tstate->line, c);
	}
}

/*
 * Read a line of comma-separated values, in which "null" is a null value and
 * the string given by the DEFAULT option is the DEFAULT marker, and have core
 * convert each field.  Stop at the first field for which core drops the row.
 */
static bool
TestCopyFromOneRow(CopyFromState cstate, ExprContext *econtext, Datum *values, bool *nulls)
{
	TestCopyFromState *tstate = (TestCopyFromState *) cstate->format_private;
	int			ncols = list_length(cstate->attnumlist);
	char	   *field;

	if (!TestCopyReadLine(cstate, tstate))
		return false;

	/* A line "END" ends the data, so that core checks what follows it */
	if (strcmp(tstate->line.data, "END") == 0)
		return false;

	CopyFromSetErrorLine(cstate, tstate->line.data, tstate->line.len);
	field = CopyFromConvertToServer(cstate, tstate->line.data,
									tstate->line.len);

	for (int i = 0;; i++)
	{
		char	   *comma = strchr(field, ',');

		if (comma != NULL)
			*comma = '\0';

		if (i >= ncols)
			ereport(ERROR,
					errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					errmsg("test_format: too many fields"));

		if (strcmp(field, "null") == 0)
			field = NULL;

		if (field != NULL && cstate->opts.default_print != NULL &&
			strcmp(field, cstate->opts.default_print) == 0)
			CopyFromSetDefault(cstate, i);
		else if (tstate->build_datum[i])
		{
			nulls[i] = (field == NULL);
			if (field != NULL)
				values[i] = CStringGetTextDatum(field);
		}
		else if (!CopyFromConvertField(cstate, i, field, values, nulls))
			return true;		/* the row is dropped */

		if (comma == NULL)
		{
			if (i + 1 < ncols)
				ereport(ERROR,
						errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						errmsg("test_format: too few fields"));
			break;
		}
		field = comma + 1;
	}

	return true;
}

/*
 * Read and discard all the input data, and return no rows.
 */
static bool
TestCopyBinaryFromOneRow(CopyFromState cstate, ExprContext *econtext,
						 Datum *values, bool *nulls)
{
	char		buf[64];
	int			nbytes;
	int			total = 0;

	while ((nbytes = CopyFromGetData(cstate, buf, 1, sizeof(buf))) > 0)
		total += nbytes;

	ereport(NOTICE, errmsg("CopyFromOneRow: read %d bytes", total));

	return false;
}

static void
TestCopyFromEnd(CopyFromState cstate)
{
	ereport(NOTICE, errmsg("CopyFromEnd"));
}

static void
TestCopyToStart(CopyToState cstate, TupleDesc tupDesc)
{
	ereport(NOTICE,
			errmsg("CopyToStart: the number of attributes of table: %d, the number of attributes to output: %d",
				   tupDesc->natts, list_length(cstate->attnumlist)));
}

/*
 * Write the row as comma-separated values, followed by a newline.
 */
static void
TestCopyToOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	bool		need_delim = false;

	ereport(NOTICE, (errmsg("CopyToOneRow: the number of valid values: %u", slot->tts_nvalid)));

	foreach_int(attnum, cstate->attnumlist)
	{
		Datum		value = slot->tts_values[attnum - 1];
		bool		isnull = slot->tts_isnull[attnum - 1];
		char	   *str;

		if (need_delim)
			CopyToSendData(cstate, ",", 1);
		need_delim = true;

		if (isnull)
			str = "null";
		else
			str = OutputFunctionCall(&cstate->out_functions[attnum - 1], value);

		CopyToSendData(cstate, str, strlen(str));
	}

	CopyToSendData(cstate, "\n", 1);
	CopyToFlushData(cstate);
}

static void
TestCopyToEnd(CopyToState cstate)
{
	ereport(NOTICE, (errmsg("CopyToEnd")));

	/* Nothing is buffered here, so this must be a no-op */
	CopyToFlushData(cstate);
}

static const CopyToRoutine TestCopyToRoutine = {
	.CopyToStart = TestCopyToStart,
	.CopyToOneRow = TestCopyToOneRow,
	.CopyToEnd = TestCopyToEnd,
};


static const CopyFromRoutine TestCopyFromRoutine = {
	.CopyFromStart = TestCopyFromStart,
	.CopyFromOneRow = TestCopyFromOneRow,
	.CopyFromEnd = TestCopyFromEnd,
};

static const CopyFromRoutine TestCopyDatumFromRoutine = {
	.CopyFromStart = TestCopyDatumFromStart,
	.CopyFromOneRow = TestCopyFromOneRow,
	.CopyFromEnd = TestCopyFromEnd,
};

static const CopyFromRoutine TestCopyBinaryFromRoutine = {
	.CopyFromStart = TestCopyFromStart,
	.CopyFromOneRow = TestCopyBinaryFromOneRow,
	.CopyFromEnd = TestCopyFromEnd,
};

static const CopyCustomFormatEntry TestCopyFormat = {
	.name = "test_format",
	.to_routine = &TestCopyToRoutine,
	.from_routine = &TestCopyFromRoutine,
};

/*
 * Same as test_format, but builds the Datums of the text-like columns by itself,
 * and supports only COPY FROM
 */
static const CopyCustomFormatEntry TestCopyDatumFormat = {
	.name = "test_datum_format",
	.from_routine = &TestCopyDatumFromRoutine,
};

/*
 * Same as test_format, but uses binary format in the COPY protocol, and COPY FROM
 * reads and discards all the input
 */
static const CopyCustomFormatEntry TestCopyBinaryFormat = {
	.name = "test_binary_format",
	.binary = true,
	.to_routine = &TestCopyToRoutine,
	.from_routine = &TestCopyBinaryFromRoutine,
};

void
_PG_init(void)
{
	RegisterCopyCustomFormat(&TestCopyFormat);
	RegisterCopyCustomFormat(&TestCopyDatumFormat);
	RegisterCopyCustomFormat(&TestCopyBinaryFormat);
}
