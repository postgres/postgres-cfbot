/*-------------------------------------------------------------------------
 *
 * heapam_hint.c
 *	  WAL logging and replay of heap tuple visibility hint bits.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_hint.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_hint.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"

/*
 * WAL-log heap tuple visibility hint bits.
 *
 * Checksums still require a full-page image to protect against torn writes.
 * Otherwise, store tuple offsets and hint bits in a compact WAL record.  Its
 * block reference also identifies the changed block for pg_rewind.
 *
 * Unlike an FPI_FOR_HINT record, this record does not advance the page LSN.
 * Hint bits are non-critical changes, so the page need not wait for this
 * record before being written.  Leaving the LSN alone also ensures that the
 * next ordinary change after a checkpoint still produces the FPI required by
 * full_page_writes.
 */
XLogRecPtr
log_heap_hint_bits(Buffer buffer)
{
	xl_heap_hint xlrec;
	xl_heap_hint_tuple tuples[MaxHeapTuplesPerPage];
	XLogRecPtr	RedoRecPtr;
	XLogRecPtr	recptr PG_USED_FOR_ASSERTS_ONLY;
	Page		page = BufferGetPage(buffer);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	RedoRecPtr = GetRedoRecPtr();
	if (PageGetLSN(page) > RedoRecPtr)
		return InvalidXLogRecPtr;

	/* A record without an FPI cannot protect against torn page writes. */
	if (DataChecksumsNeedWrite())
		return XLogSaveBufferForHint(buffer, true);

	xlrec.ntuples = 0;
	for (OffsetNumber offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		HeapTupleHeader tuple;
		uint16		infomask;

		if (!ItemIdIsNormal(itemid))
			continue;

		tuple = (HeapTupleHeader) PageGetItem(page, itemid);
		infomask = tuple->t_infomask & XL_HEAP_HINT_BITS;
		if (infomask == 0)
			continue;

		/* Be defensive about corrupt pages in non-assert builds. */
		if (xlrec.ntuples == lengthof(tuples))
			return XLogSaveBufferForHint(buffer, true);
		tuples[xlrec.ntuples].offnum = offnum;
		tuples[xlrec.ntuples].infomask = infomask;
		xlrec.ntuples++;
	}

	if (xlrec.ntuples == 0)
		return InvalidXLogRecPtr;

	XLogBeginInsert();
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | REGBUF_NO_IMAGE);
	XLogRegisterBufData(0, &xlrec, sizeof(xlrec));
	XLogRegisterBufData(0, tuples,
						xlrec.ntuples * sizeof(xl_heap_hint_tuple));

	recptr = XLogInsert(RM_HEAP_HINT_ID, XLOG_HEAP_HINT);
	Assert(XLogRecPtrIsValid(recptr));

	/* The caller must not install this record's LSN on the heap page. */
	return InvalidXLogRecPtr;
}

/* Replay a heap hint-bit WAL record. */
void
heap_hint_redo(XLogReaderState *record)
{
	Buffer		buffer = InvalidBuffer;
	XLogRedoAction action;

	action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *data = XLogRecGetBlockData(record, 0, &datalen);
		xl_heap_hint *xlrec;
		xl_heap_hint_tuple *tuples;
		Page		page = BufferGetPage(buffer);

		if (data == NULL || datalen < sizeof(xl_heap_hint))
			elog(PANIC, "invalid heap hint WAL record");

		xlrec = (xl_heap_hint *) data;
		if (datalen != sizeof(xl_heap_hint) +
			xlrec->ntuples * sizeof(xl_heap_hint_tuple))
			elog(PANIC, "invalid heap hint WAL record length");

		tuples = (xl_heap_hint_tuple *) (data + sizeof(xl_heap_hint));
		for (int i = 0; i < xlrec->ntuples; i++)
		{
			OffsetNumber offnum = tuples[i].offnum;
			ItemId		itemid;
			HeapTupleHeader tuple;

			if (offnum < FirstOffsetNumber ||
				offnum > PageGetMaxOffsetNumber(page))
				elog(PANIC, "heap hint WAL record offset out of range");

			itemid = PageGetItemId(page, offnum);
			if (!ItemIdIsNormal(itemid))
				elog(PANIC, "heap hint WAL record references invalid line pointer");
			if (tuples[i].infomask & ~XL_HEAP_HINT_BITS)
				elog(PANIC, "heap hint WAL record contains invalid bits");

			tuple = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple->t_infomask |= tuples[i].infomask;
		}

		MarkBufferDirty(buffer);
	}

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}
