/*--------------------------------------------------------------------------
 *
 * test_upper_paths_hook.c
 *		Test that create_upper_paths_hook receives UPPERREL_PARTIAL_SETOP.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_upper_paths_hook/test_upper_paths_hook.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "optimizer/planner.h"

PG_MODULE_MAGIC;

static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;
static bool saw_partial_setop = false;
static bool saw_input_null = false;
static bool saw_output_rel = false;

static void
test_upper_paths_hook(PlannerInfo *root, UpperRelationKind stage,
					  RelOptInfo *input_rel, RelOptInfo *output_rel,
					  void *extra)
{
	if (stage == UPPERREL_PARTIAL_SETOP)
	{
		saw_partial_setop = true;
		saw_input_null = (input_rel == NULL);
		saw_output_rel = (output_rel != NULL);
	}

	if (prev_create_upper_paths_hook)
		prev_create_upper_paths_hook(root, stage, input_rel, output_rel,
									 extra);
}

void
_PG_init(void)
{
	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = test_upper_paths_hook;
}

PG_FUNCTION_INFO_V1(test_upper_paths_hook_seen);
Datum
test_upper_paths_hook_seen(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(saw_partial_setop);
}

PG_FUNCTION_INFO_V1(test_upper_paths_hook_input_null);
Datum
test_upper_paths_hook_input_null(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(saw_input_null);
}

PG_FUNCTION_INFO_V1(test_upper_paths_hook_output_seen);
Datum
test_upper_paths_hook_output_seen(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(saw_output_rel);
}

PG_FUNCTION_INFO_V1(test_upper_paths_hook_reset);
Datum
test_upper_paths_hook_reset(PG_FUNCTION_ARGS)
{
	saw_partial_setop = false;
	saw_input_null = false;
	saw_output_rel = false;
	PG_RETURN_VOID();
}
