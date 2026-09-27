/*-------------------------------------------------------------------------
 *
 * atmdesc.c
 *	  rmgr descriptor routines for access/undo/atm.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/atmdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/atm_xlog.h"

void
atm_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *data = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_ATM_ABORT:
			{
				xl_atm_abort *xlrec = (xl_atm_abort *) data;

				appendStringInfo(buf,
								 "xid %u, last_batch_lsn %X/%X, stop_lsn %X/%X, first_batch_lsn %X/%X, dboid %u",
								 xlrec->xid,
								 LSN_FORMAT_ARGS(xlrec->last_batch_lsn),
								 LSN_FORMAT_ARGS(xlrec->stop_lsn),
								 LSN_FORMAT_ARGS(xlrec->first_batch_lsn),
								 xlrec->dboid);
			}
			break;

		case XLOG_ATM_FORGET:
			{
				xl_atm_forget *xlrec = (xl_atm_forget *) data;

				appendStringInfo(buf, "xid %u", xlrec->xid);
			}
			break;
	}
}

const char *
atm_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_ATM_ABORT:
			id = "ABORT";
			break;
		case XLOG_ATM_FORGET:
			id = "FORGET";
			break;
	}

	return id;
}
