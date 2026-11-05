/*-------------------------------------------------------------------------
 *
 * test_ready_for_query.c
 *		Test code for ReadyForQuery hook and wire protocol extension.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_ready_for_query/test_ready_for_query.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "fmgr.h"
#include "tcop/dest.h"

PG_MODULE_MAGIC;

static ready_for_query_hook_type prev_ready_for_query_hook = NULL;

static void
test_ready_for_query_hook(StringInfo buf)
{
	if (prev_ready_for_query_hook)
		prev_ready_for_query_hook(buf);

	/*
	 * Example extension behavior: check if "temp_tables_info" is not already present,
	 * and if the session has temporary tables, append it.
	 */
	if (!ready_for_query_has_key(buf, "temp_tables_info", 16))
	{
		if (HasSessionTempTables())
		{
			const char *info = "has_temp_namespace";

			ready_for_query_append_kv(buf, "temp_tables_info", 16, info, (uint8) strlen(info));
		}
	}
}

void
_PG_init(void)
{
	prev_ready_for_query_hook = ready_for_query_hook;
	ready_for_query_hook = test_ready_for_query_hook;
}
