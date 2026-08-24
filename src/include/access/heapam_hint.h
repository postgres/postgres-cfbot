/*-------------------------------------------------------------------------
 *
 * heapam_hint.h
 *	  WAL definitions for heap tuple visibility hint bits.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/access/heapam_hint.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_HINT_H
#define HEAPAM_HINT_H

#include "access/htup_details.h"
#include "access/xlogreader.h"
#include "storage/buf.h"

#define XLOG_HEAP_HINT		0x00

/*
 * A hint-bit WAL record contains the hint bits of every normal tuple on the
 * page.  Recording all of them keeps the insertion interface independent of
 * whether a caller set one hint bit or batched a whole page's worth.
 */
typedef struct xl_heap_hint
{
	uint16		ntuples;
}			xl_heap_hint;

typedef struct xl_heap_hint_tuple
{
	OffsetNumber offnum;
	uint16		infomask;
}			xl_heap_hint_tuple;

#define XL_HEAP_HINT_BITS \
	(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | \
	 HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)

extern XLogRecPtr log_heap_hint_bits(Buffer buffer);
extern void heap_hint_redo(XLogReaderState *record);
extern void heap_hint_desc(StringInfo buf, XLogReaderState *record);
extern const char *heap_hint_identify(uint8 info);

#endif							/* HEAPAM_HINT_H */
