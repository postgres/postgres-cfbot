/*-------------------------------------------------------------------------
 *
 * heap_hintdesc.c
 *	  rmgr descriptor routines for heap hint-bit WAL records.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/heap_hintdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_hint.h"

void
heap_hint_desc(StringInfo buf, XLogReaderState *record)
{
	if (XLogRecHasBlockData(record, 0))
	{
		Size		datalen;
		xl_heap_hint *xlrec = (xl_heap_hint *)
			XLogRecGetBlockData(record, 0, &datalen);

		appendStringInfo(buf, "ntuples: %u", xlrec->ntuples);
	}
}

const char *
heap_hint_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_HEAP_HINT)
		return "HINT";

	return NULL;
}
