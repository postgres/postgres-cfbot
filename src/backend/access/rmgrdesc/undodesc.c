/*-------------------------------------------------------------------------
 *
 * undodesc.c
 *	  rmgr descriptor routines for access/undo
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/undodesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/undo_xlog.h"
#include "access/xlogreader.h"

/*
 * undo_desc - Describe an UNDO WAL record for pg_waldump
 *
 * This function generates human-readable output for UNDO WAL records,
 * used by pg_waldump and other debugging tools.
 */
void
undo_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_UNDO_ALLOCATE:
			{
				xl_undo_allocate *xlrec = (xl_undo_allocate *) rec;

				appendStringInfo(buf, "log %u, start %llu, len %u, xid %u",
								 xlrec->log_number,
								 (unsigned long long) xlrec->start_ptr,
								 xlrec->length,
								 xlrec->xid);
			}
			break;

		case XLOG_UNDO_DISCARD:
			{
				xl_undo_discard *xlrec = (xl_undo_discard *) rec;

				appendStringInfo(buf, "log %u, discard_ptr %llu, oldest_xid %u",
								 xlrec->log_number,
								 (unsigned long long) xlrec->discard_ptr,
								 xlrec->oldest_xid);
			}
			break;

		case XLOG_UNDO_EXTEND:
			{
				xl_undo_extend *xlrec = (xl_undo_extend *) rec;

				appendStringInfo(buf, "log %u, new_size %llu",
								 xlrec->log_number,
								 (unsigned long long) xlrec->new_size);
			}
			break;

		case XLOG_UNDO_APPLY_RECORD:
			{
				xl_undo_apply *xlrec = (xl_undo_apply *) rec;
				const char *op_name;

				switch (xlrec->operation_type)
				{
					case 0x0001:
						op_name = "INSERT";
						break;
					case 0x0002:
						op_name = "DELETE";
						break;
					case 0x0003:
						op_name = "UPDATE";
						break;
					case 0x0004:
						op_name = "PRUNE";
						break;
					case 0x0005:
						op_name = "INPLACE";
						break;
					case 0x0006:
						op_name = "HOT_UPDATE";
						break;
					default:
						op_name = "UNKNOWN";
						break;
				}

				appendStringInfo(buf,
								 "undo apply %s: urec_ptr %llu, xid %u, "
								 "block %u, offset %u, clr_flags 0x%04x, "
								 "tuple_len %u",
								 op_name,
								 (unsigned long long) xlrec->urec_ptr,
								 xlrec->xid,
								 xlrec->target_block,
								 xlrec->target_offset,
								 xlrec->clr_flags,
								 xlrec->tuple_len);
			}
			break;

		case XLOG_UNDO_PAGE_WRITE:
			{
				xl_undo_page_write *xlrec = (xl_undo_page_write *) rec;

				appendStringInfo(buf, "page_offset %u, data_len %u",
								 xlrec->page_offset,
								 xlrec->data_len);
			}
			break;

		case XLOG_UNDO_BATCH:
			{
				xl_undo_batch *xlrec = (xl_undo_batch *) rec;

				appendStringInfo(buf,
								 "undo batch: xid %u, nrecords %u, "
								 "total_len %u, chain_prev %X/%X, "
								 "primary_reloid %u, persistence %d",
								 xlrec->xid,
								 xlrec->nrecords,
								 xlrec->total_len,
								 LSN_FORMAT_ARGS(xlrec->chain_prev),
								 xlrec->primary_reloid,
								 xlrec->persistence);
			}
			break;

		case XLOG_UNDO_ROTATE:
			{
				xl_undo_rotate *xlrec = (xl_undo_rotate *) rec;
				const char *trigger_name;

				switch (xlrec->trigger)
				{
					case UNDO_ROTATE_CAPACITY:
						trigger_name = "capacity";
						break;
					case UNDO_ROTATE_CHECKPOINT:
						trigger_name = "checkpoint";
						break;
					case UNDO_ROTATE_PRESSURE:
						trigger_name = "pressure";
						break;
					case UNDO_ROTATE_MANUAL:
						trigger_name = "manual";
						break;
					default:
						trigger_name = "unknown";
						break;
				}

				appendStringInfo(buf,
								 "old_log %u, seal_ptr %llu, new_log %u, "
								 "trigger %s",
								 xlrec->old_log_number,
								 (unsigned long long) xlrec->old_seal_ptr,
								 xlrec->new_log_number,
								 trigger_name);
			}
			break;
	}
}

/*
 * undo_identify - Identify an UNDO WAL record type
 *
 * Returns a string identifying the operation type for debugging output.
 */
const char *
undo_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_UNDO_ALLOCATE:
			id = "ALLOCATE";
			break;
		case XLOG_UNDO_DISCARD:
			id = "DISCARD";
			break;
		case XLOG_UNDO_EXTEND:
			id = "EXTEND";
			break;
		case XLOG_UNDO_APPLY_RECORD:
			id = "APPLY_RECORD";
			break;
		case XLOG_UNDO_ROTATE:
			id = "ROTATE";
			break;
		case XLOG_UNDO_PAGE_WRITE:
			id = "PAGE_WRITE";
			break;
		case XLOG_UNDO_BATCH:
			id = "BATCH";
			break;
	}

	return id;
}
