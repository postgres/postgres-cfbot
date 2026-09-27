/*-------------------------------------------------------------------------
 *
 * pbu_undoactiondesc.c
 *	  rmgr descriptor routines for the per-backend UNDO action WAL
 *	  (RM_UNDOACTION_ID).
 *
 * Per-backend UNDO engine derives from EnterpriseDB zheap
 * (Amit Kapila, Dilip Kumar, Kuntal Ghosh, Mahendra Singh Thalor,
 *  Rafia Sabih, Thomas Munro et al.).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/pbu_undoactiondesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/perbackend/pbu_undoaction_xlog.h"
#include "access/xlogreader.h"

void
undoaction_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_UNDO_AMEND_RECORD:
			appendStringInfo(buf, "blocks %d", XLogRecMaxBlockId(record) + 1);
			break;
		case XLOG_UNDO_APPLY_PROGRESS:
			{
				xl_undoapply_progress *xlrec = (xl_undoapply_progress *) rec;

				appendStringInfo(buf, "urec_ptr %016llX progress %u",
								 (unsigned long long) xlrec->urec_ptr, xlrec->progress);
			}
			break;
		case XLOG_UNDO_APPLY_SUBXACT:
			{
				xl_undoapply_subxact *xlrec = (xl_undoapply_subxact *) rec;

				appendStringInfo(buf, "urec_ptr %016llX",
								 (unsigned long long) xlrec->urec_ptr);
			}
			break;
	}
}

const char *
undoaction_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_UNDO_AMEND_RECORD:
			id = "AMEND_RECORD";
			break;
		case XLOG_UNDO_APPLY_PROGRESS:
			id = "APPLY_PROGRESS";
			break;
		case XLOG_UNDO_APPLY_SUBXACT:
			id = "APPLY_SUBXACT";
			break;
	}

	return id;
}
