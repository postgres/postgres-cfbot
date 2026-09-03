/*--------------------------------------------------------------------------
 *
 * test_walwriter.c
 *		Test facilities for the WAL writer.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_walwriter/test_walwriter.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "fmgr.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_walwriter_bogus_async_lsn);

/*
 * Store a position where nothing has been inserted in asyncXactLSN, and
 * return it.
 *
 * Request a segment switch, then store the current insert position in
 * asyncXactLSN.  After the switch that position is just past the new
 * segment's long page header, beyond the end of generated WAL, so the
 * walwriter's next cycle requests a flush past the end of generated WAL.
 * (If the insert position was already at a segment boundary the switch is
 * a no-op, but the position returned still lies past the end of reserved
 * WAL, so the outcome is the same.)
 */
Datum
test_walwriter_bogus_async_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	ptr;

	(void) RequestXLogSwitch(false);
	ptr = GetXLogInsertRecPtr();
	XLogSetAsyncXactLSN(ptr);

	PG_RETURN_LSN(ptr);
}
