/*-------------------------------------------------------------------------
 *
 * dummy_table_am.c
 *		A dummy table access method for testing.
 *
 * The routine is the heap one with a few callbacks replaced, so that a table
 * created with this access method has real storage while the answers the
 * core gets from the AM are trivial to predict.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/dummy_table_am/dummy_table_am.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

/*
 * Report half of the heap footprint for the main fork.  The number tracks
 * the relation's real size but is never equal to it, so the SQL-level size
 * functions can be told apart from plain file measurement.  Reporting more
 * than the heap size would make RelationGetNumberOfBlocks() point past the
 * end of the relation, so it is capped at the real size.
 */
static uint64
dummy_relation_size(Relation rel, ForkNumber forknum)
{
	if (forknum == MAIN_FORKNUM || forknum == InvalidForkNumber)
		return table_block_relation_size(rel, MAIN_FORKNUM) / 2;

	return table_block_relation_size(rel, forknum);
}

PG_FUNCTION_INFO_V1(dummy_table_am_handler);

Datum
dummy_table_am_handler(PG_FUNCTION_ARGS)
{
	static TableAmRoutine routine;
	static bool initialized = false;

	if (!initialized)
	{
		memcpy(&routine, GetHeapamTableAmRoutine(), sizeof(TableAmRoutine));
		routine.relation_size = dummy_relation_size;
		initialized = true;
	}

	PG_RETURN_POINTER(&routine);
}
