/*-------------------------------------------------------------------------
 *
 * pbu_undologdesc.c
 *	  rmgr descriptor routines for the per-backend UNDO log WAL
 *	  (RM_UNDOLOG_ID).
 *
 * Per-backend UNDO engine derives from EnterpriseDB zheap
 * (Amit Kapila, Dilip Kumar, Kuntal Ghosh, Mahendra Singh Thalor,
 *  Rafia Sabih, Thomas Munro et al.).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/pbu_undologdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/perbackend/pbu_undolog_xlog.h"
#include "access/xlogreader.h"

void
undolog_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_UNDOLOG_CREATE:
			{
				xl_undolog_create *xlrec = (xl_undolog_create *) rec;

				appendStringInfo(buf, "logno %u", xlrec->logno);
			}
			break;
		case XLOG_UNDOLOG_EXTEND:
			{
				xl_undolog_extend *xlrec = (xl_undolog_extend *) rec;

				appendStringInfo(buf, "logno %u end " UndoLogOffsetFormat,
								 xlrec->logno, xlrec->end);
			}
			break;
		case XLOG_UNDOLOG_ATTACH:
			{
				xl_undolog_attach *xlrec = (xl_undolog_attach *) rec;

				appendStringInfo(buf, "xid %u logno %u dbid %u",
								 xlrec->xid, xlrec->logno, xlrec->dbid);
			}
			break;
		case XLOG_UNDOLOG_DISCARD:
			{
				xl_undolog_discard *xlrec = (xl_undolog_discard *) rec;

				appendStringInfo(buf,
								 "logno %u discard " UndoLogOffsetFormat
								 " end " UndoLogOffsetFormat " latestxid %u",
								 xlrec->logno, xlrec->discard, xlrec->end,
								 xlrec->latestxid);
			}
			break;
		case XLOG_UNDOLOG_REWIND:
			{
				xl_undolog_rewind *xlrec = (xl_undolog_rewind *) rec;

				appendStringInfo(buf, "logno %u insert " UndoLogOffsetFormat,
								 xlrec->logno, xlrec->insert);
			}
			break;
		case XLOG_UNDOLOG_META:
			{
				xl_undolog_meta *xlrec = (xl_undolog_meta *) rec;

				appendStringInfo(buf, "logno %u xid %u",
								 xlrec->logno, xlrec->xid);
			}
			break;
		case XLOG_UNDOLOG_SWITCH:
			appendStringInfoString(buf, "switch");
			break;
	}
}

const char *
undolog_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_UNDOLOG_CREATE:
			id = "CREATE";
			break;
		case XLOG_UNDOLOG_EXTEND:
			id = "EXTEND";
			break;
		case XLOG_UNDOLOG_ATTACH:
			id = "ATTACH";
			break;
		case XLOG_UNDOLOG_DISCARD:
			id = "DISCARD";
			break;
		case XLOG_UNDOLOG_REWIND:
			id = "REWIND";
			break;
		case XLOG_UNDOLOG_META:
			id = "META";
			break;
		case XLOG_UNDOLOG_SWITCH:
			id = "SWITCH";
			break;
	}

	return id;
}
