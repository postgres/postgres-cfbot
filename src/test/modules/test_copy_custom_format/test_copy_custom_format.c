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
#include "commands/defrem.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

typedef struct TestCopyOptions
{
	int			max_attributes;
	bool		disallow_freeze;
} TestCopyOptions;

static bool
TestCopyProcessOneOption(CopyFormatOptions *opts, bool is_from, DefElem *option)
{
	TestCopyOptions *t = (TestCopyOptions *) opts->format_private_opts;

	if (t == NULL)
	{
		t = palloc0_object(TestCopyOptions);
		opts->format_private_opts = (void *) t;
	}

	if (strcmp(option->defname, "max_attributes") == 0)
	{
		int			val = defGetInt32(option);

		if (val < 1)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("\"max_attributes\" must be a positive integer"));

		t->max_attributes = val;
		return true;
	}
	else if (strcmp(option->defname, "disallow_freeze") == 0)
	{
		t->disallow_freeze = defGetBoolean(option);
		return true;
	}

	return false;
}

static void
TestCopyValidateOptions(CopyFormatOptions *opts, bool is_from)
{
	TestCopyOptions *t = (TestCopyOptions *) opts->format_private_opts;

	if (!t)
		return;

	if (t->disallow_freeze && opts->freeze)
		ereport(ERROR,
				errmsg("FREEZE cannot be used with \"disallow_freeze\" option"));
}

static void
TestCopyFromInFunc(CopyFromState cstate, Oid atttypid, FmgrInfo *finfo, Oid *typioparam)
{
	ereport(NOTICE,
			errmsg("CopyFromInFunc: attribute: %s", format_type_be(atttypid)));
}

static void
check_max_attributes(CopyFormatOptions *opts, TupleDesc tupDesc)
{
	TestCopyOptions *t = (TestCopyOptions *) opts->format_private_opts;

	if (t != NULL && t->max_attributes > 0 && tupDesc->natts > t->max_attributes)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("relation has %d columns, exceeds max_attributes %d",
					   tupDesc->natts, t->max_attributes));
}

static void
TestCopyFromStart(CopyFromState cstate, TupleDesc tupDesc)
{
	check_max_attributes(&cstate->opts, tupDesc);

	ereport(NOTICE,
			errmsg("CopyFromStart: the number of attributes of table: %d, the number of attributes to input: %d",
				   tupDesc->natts, list_length(cstate->attnumlist)));
}

static bool
TestCopyFromOneRow(CopyFromState cstate, ExprContext *econtext, Datum *values, bool *nulls)
{
	ereport(NOTICE, errmsg("CopyFromOneRow"));

	return false;
}

static void
TestCopyFromEnd(CopyFromState cstate)
{
	ereport(NOTICE, errmsg("CopyFromEnd"));
}

static void
TestCopyToOutFunc(CopyToState cstate, Oid atttypid, FmgrInfo *finfo)
{
	ereport(NOTICE, errmsg("CopyToOutFunc: attribute: %s", format_type_be(atttypid)));
}

static void
TestCopyToStart(CopyToState cstate, TupleDesc tupDesc)
{
	check_max_attributes(&cstate->opts, tupDesc);

	ereport(NOTICE,
			errmsg("CopyToStart: the number of attributes of table: %d, the number of attributes to output: %d",
				   tupDesc->natts, list_length(cstate->attnumlist)));
}

static void
TestCopyToOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	ereport(NOTICE, (errmsg("CopyToOneRow: the number of valid values: %u", slot->tts_nvalid)));
}

static void
TestCopyToEnd(CopyToState cstate)
{
	ereport(NOTICE, (errmsg("CopyToEnd")));
}

static const CopyToRoutine TestCopyToRoutine = {
	.CopyToOutFunc = TestCopyToOutFunc,
	.CopyToStart = TestCopyToStart,
	.CopyToOneRow = TestCopyToOneRow,
	.CopyToEnd = TestCopyToEnd,
};


static const CopyFromRoutine TestCopyFromRoutine = {
	.CopyFromInFunc = TestCopyFromInFunc,
	.CopyFromStart = TestCopyFromStart,
	.CopyFromOneRow = TestCopyFromOneRow,
	.CopyFromEnd = TestCopyFromEnd,
};

void
_PG_init(void)
{
	RegisterCopyCustomFormat("test_format",
							 &TestCopyToRoutine,
							 &TestCopyFromRoutine,
							 &TestCopyProcessOneOption,
							 &TestCopyValidateOptions);
}
