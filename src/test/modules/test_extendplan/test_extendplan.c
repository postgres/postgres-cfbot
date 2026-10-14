/*-------------------------------------------------------------------------
 *
 * test_extendplan.c
 *		Test module for planner extension state
 *
 * When the planner builds a base or join relation, this module stores a tag
 * in the relation's extension state recording which RelOptInfo it was
 * stored in.  join_path_setup_hook then checks the joinrel and both of its
 * input rels: any tag found there must belong to that same RelOptInfo.
 *
 * The planner creates some RelOptInfos by flat-copying another one, such as
 * the grouped rels built for eager aggregation and the unique-ified rels
 * built for semijoins.  Such a copy must not inherit, or share, the extension
 * state of the rel it was copied from.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_extendplan/test_extendplan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "optimizer/extendplan.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"

PG_MODULE_MAGIC;

/* What we store in the extension state of each rel we see being built */
typedef struct TestExtendPlanTag
{
	RelOptInfo *owner;			/* the RelOptInfo the tag was stored in */
} TestExtendPlanTag;

static int	extension_id = -1;

/* Counters reported by test_extendplan_summary() */
static int64 grouped_rels_checked = 0;
static int64 unique_rels_checked = 0;
static int64 mismatches = 0;

static build_simple_rel_hook_type prev_build_simple_rel_hook = NULL;
static joinrel_setup_hook_type prev_joinrel_setup_hook = NULL;
static join_path_setup_hook_type prev_join_path_setup_hook = NULL;

PG_FUNCTION_INFO_V1(test_extendplan_reset);
PG_FUNCTION_INFO_V1(test_extendplan_summary);

/*
 * Store a tag naming rel in rel's extension state.
 */
static void
tag_rel(RelOptInfo *rel)
{
	TestExtendPlanTag *tag = palloc_object(TestExtendPlanTag);

	tag->owner = rel;
	SetRelOptInfoExtensionState(rel, extension_id, tag);
}

/*
 * Complain if rel's extension state holds a tag stored in some other rel.
 */
static void
check_rel(RelOptInfo *rel, const char *role)
{
	TestExtendPlanTag *tag;

	tag = GetRelOptInfoExtensionState(rel, extension_id);
	if (tag != NULL && tag->owner != rel)
	{
		mismatches++;
		elog(WARNING, "extension state of %s%s %s belongs to a different RelOptInfo",
			 IS_GROUPED_REL(rel) ? "grouped " : "", role,
			 bmsToString(rel->relids));
	}
}

static void
test_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (prev_build_simple_rel_hook)
		prev_build_simple_rel_hook(root, rel, rte);

	tag_rel(rel);
}

static void
test_joinrel_setup(PlannerInfo *root, RelOptInfo *joinrel,
				   RelOptInfo *outer_rel, RelOptInfo *inner_rel,
				   SpecialJoinInfo *sjinfo, List *restrictlist)
{
	if (prev_joinrel_setup_hook)
		prev_joinrel_setup_hook(root, joinrel, outer_rel, inner_rel,
								sjinfo, restrictlist);

	tag_rel(joinrel);
}

static void
test_join_path_setup(PlannerInfo *root, RelOptInfo *joinrel,
					 RelOptInfo *outerrel, RelOptInfo *innerrel,
					 JoinType jointype, JoinPathExtraData *extra)
{
	if (prev_join_path_setup_hook)
		prev_join_path_setup_hook(root, joinrel, outerrel, innerrel,
								  jointype, extra);

	/* Count the flat-copied rels this call lets us check */
	if (IS_GROUPED_REL(joinrel) || IS_GROUPED_REL(outerrel) ||
		IS_GROUPED_REL(innerrel))
		grouped_rels_checked++;
	if (jointype == JOIN_UNIQUE_OUTER || jointype == JOIN_UNIQUE_INNER)
		unique_rels_checked++;

	check_rel(joinrel, "joinrel");
	check_rel(outerrel, "outer rel");
	check_rel(innerrel, "inner rel");
}

void
_PG_init(void)
{
	extension_id = GetPlannerExtensionId("test_extendplan");

	prev_build_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = test_build_simple_rel;
	prev_joinrel_setup_hook = joinrel_setup_hook;
	joinrel_setup_hook = test_joinrel_setup;
	prev_join_path_setup_hook = join_path_setup_hook;
	join_path_setup_hook = test_join_path_setup;
}

/*
 * Reset the counters.
 */
Datum
test_extendplan_reset(PG_FUNCTION_ARGS)
{
	grouped_rels_checked = 0;
	unique_rels_checked = 0;
	mismatches = 0;

	PG_RETURN_VOID();
}

/*
 * Report whether grouped and unique-ified rels were checked since the last
 * reset, and how many tags were found in the wrong RelOptInfo.
 */
Datum
test_extendplan_summary(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {0};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values[0] = BoolGetDatum(grouped_rels_checked > 0);
	values[1] = BoolGetDatum(unique_rels_checked > 0);
	values[2] = Int64GetDatum(mismatches);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
