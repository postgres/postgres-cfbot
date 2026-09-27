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


		case XLOG_UNDO_INFLIGHT:
			{
				xl_undo_inflight *xlrec = (xl_undo_inflight *) rec;

				appendStringInfo(buf, "nxacts %u", xlrec->nxacts);
				for (uint32 i = 0; i < xlrec->nxacts; i++)
				{
					xl_undo_inflight_xact *x = &xlrec->xacts[i];

					appendStringInfo(buf, "; xid %u db %u", x->xid, x->dboid);
					for (int j = 0; j < NUndoPersistenceLevels; j++)
						if (XLogRecPtrIsValid(x->last_batch_lsn[j]))
							appendStringInfo(buf, " p%d %X/%08X..%X/%08X", j,
											 LSN_FORMAT_ARGS(x->first_batch_lsn[j]),
											 LSN_FORMAT_ARGS(x->last_batch_lsn[j]));
				}
			}
			break;

		case XLOG_UNDO_BATCH:
			{
				xl_undo_batch *xlrec = (xl_undo_batch *) rec;

				appendStringInfo(buf,
								 "undo batch: xid %u, db %u, nrecords %u, "
								 "total_len %u, chain_prev %X/%X, "
								 "primary_reloid %u, persistence %d",
								 xlrec->xid,
								 xlrec->dboid,
								 xlrec->nrecords,
								 xlrec->total_len,
								 LSN_FORMAT_ARGS(xlrec->chain_prev),
								 xlrec->primary_reloid,
								 xlrec->persistence);
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



		case XLOG_UNDO_APPLY_RECORD:
			id = "APPLY_RECORD";
			break;


		case XLOG_UNDO_BATCH:
			id = "BATCH";
			break;
		case XLOG_UNDO_INFLIGHT:
			id = "INFLIGHT";
			break;
	}

	return id;
}
