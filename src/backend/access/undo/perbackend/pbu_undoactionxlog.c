/*-------------------------------------------------------------------------
 *
 * undoactionxlog.c
 *	  WAL replay logic for undo actions.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undoactionxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/perbackend/pbu_compat.h"

#include "access/perbackend/pbu_undoaction_xlog.h"
#include "access/perbackend/pbu_undoinsert.h"
#include "access/perbackend/pbu_undorecord.h"
#include "access/undormgr.h"
#include "access/xlog.h"
#include "access/xlogutils.h"

/*
 * Replay of undo apply progress.
 */
static void
undo_xlog_apply_progress(XLogReaderState *record)
{
	xl_undoapply_progress *xlrec = (xl_undoapply_progress *) XLogRecGetData(record);

	/* Update the progress in the transaction header. */
	if (PrepareUpdateUndoActionProgress(record, xlrec->urec_ptr, xlrec->progress))
		UndoRecordUpdateTransInfo(0);
	UnlockReleaseUndoBuffers();
}

/*
 * Replay the marking of an undo record as applied by ROLLBACK TO SAVEPOINT.
 * Setting the rmid is idempotent, so a page that already has it is harmless.
 */
static void
undo_xlog_apply_subxact(XLogReaderState *record)
{
	xl_undoapply_subxact *xlrec = (xl_undoapply_subxact *) XLogRecGetData(record);
	Buffer		buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);

		((char *) page)[UndoRecPtrGetPageOffset(xlrec->urec_ptr) +
						offsetof(UndoRecordHeader, urec_rmid)] = UNDO_RMID_INVALID;
		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/* Replay each piece of a payload patch, including pieces on later pages. */
static void
undo_xlog_amend_record(XLogReaderState *record)
{
	int			i;

	for (i = 0; i <= XLogRecMaxBlockId(record); i++)
	{
		Buffer		buffer;

		if (XLogReadBufferForRedo(record, i, &buffer) == BLK_NEEDS_REDO)
		{
			Size		len;
			char	   *data = XLogRecGetBlockData(record, i, &len);
			uint16		offset;
			Page		page = BufferGetPage(buffer);

			if (len < sizeof(offset))
				elog(PANIC, "truncated undo amendment");
			memcpy(&offset, data, sizeof(offset));
			if (offset < UndoLogBlockHeaderSize ||
				offset > BLCKSZ || len - sizeof(offset) > BLCKSZ - offset)
				elog(PANIC, "invalid undo amendment offset");
			memcpy((char *) page + offset, data + sizeof(offset),
				   len - sizeof(offset));
			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

void
undoaction_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_UNDO_AMEND_RECORD:
			undo_xlog_amend_record(record);
			break;
		case XLOG_UNDO_APPLY_PROGRESS:
			undo_xlog_apply_progress(record);
			break;
		case XLOG_UNDO_APPLY_SUBXACT:
			undo_xlog_apply_subxact(record);
			break;
		default:
			elog(PANIC, "undoaction_redo: unknown op code %u", info);
	}
}
