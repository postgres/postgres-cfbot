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

PG_MODULE_MAGIC;

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

static const CopyCustomFormatEntry TestCopyFormat = {
	.name = "test_format",
	.to_routine = &TestCopyToRoutine,
};

void
_PG_init(void)
{
	RegisterCopyCustomFormat(&TestCopyFormat);
}
