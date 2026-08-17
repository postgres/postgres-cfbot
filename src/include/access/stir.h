/*-------------------------------------------------------------------------
 *
 * stir.h
 *	  header file for postgres stir access method implementation.
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/access/stir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STIR_H
#define STIR_H

#include "access/amapi.h"
#include "nodes/pathnodes.h"
#include "storage/bufpage.h"

/* Support procedures numbers */
#define STIR_NPROC				0

/* Scan strategies (none: STIR carries no key data, so no operators) */
#define STIR_NSTRATEGIES		0

#define STIR_OPTIONS_PROC				0

/* Macros for accessing stir page structures */
#define StirPageGetOpaque(page) ((StirPageOpaque) PageGetSpecialPointer(page))
#define StirPageGetMaxOffset(page) (StirPageGetOpaque(page)->maxoff)
#define StirPageIsMeta(page) \
	((StirPageGetOpaque(page)->flags & STIR_META) != 0)
#define StirPageGetTuple(page, offset) \
	((StirTuple *)(PageGetContents(page) \
		+ sizeof(StirTuple) * ((offset) - 1)))
#define StirPageGetNextTuple(tuple) \
	((StirTuple *)((char *)(tuple) + sizeof(StirTuple)))



/* Preserved page numbers */
#define STIR_METAPAGE_BLKNO	(0)
#define STIR_HEAD_BLKNO		(1) /* first data page */


/* Opaque for stir pages */
typedef struct StirPageOpaqueData
{
	OffsetNumber maxoff;		/* number of index tuples on the page */
	uint16		flags;			/* see bit definitions below */
	uint16		stir_page_id;	/* for identification of STIR indexes */
} StirPageOpaqueData;

/* Stir page flags */
#define STIR_META		(1<<0)

typedef StirPageOpaqueData *StirPageOpaque;

#define STIR_PAGE_ID		0xFF84

/*
 * Number of independent insertion pages the inserts are spread across,
 * to avoid contention on a single insertion point.  Each backend picks
 * a slot by its ProcNumber.
 */
#define STIR_INSERT_SLOTS		16

/* Metadata of stir index */
typedef struct StirMetaPageData
{
	uint32		magicNumber;
	/* current insertion page of each slot, 0 while the slot has no page */
	BlockNumber slotBlkNo[STIR_INSERT_SLOTS];
	bool		skipInserts;	/* should we just exit without any inserts? */
} StirMetaPageData;

/* Magic number to distinguish stir pages from others */
#define STIR_MAGIC_NUMBER (0xDBAC0DEF)

#define StirPageGetMeta(page)	((StirMetaPageData *) PageGetContents(page))

typedef struct StirTuple
{
	ItemPointerData heapPtr;
} StirTuple;

#define StirPageGetFreeSpace(page) \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
		- StirPageGetMaxOffset(page) * (sizeof(StirTuple)) \
		- MAXALIGN(sizeof(StirPageOpaqueData)))

extern void StirFillMetapage(Relation index, Page metaPage, bool skipInserts);
extern void StirInitMetapage(Relation index);
extern void StirInitPage(Page page, uint16 flags);
extern void StirMarkAsSkipInserts(Relation index);

/* index access method interface functions */
extern bool stirvalidate(Oid opclassoid);
extern bool stirinsert(Relation index, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
extern IndexScanDesc stirbeginscan(Relation r, int nkeys, int norderbys);
extern void stirrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);
extern void stirendscan(IndexScanDesc scan);
extern IndexBuildResult *stirbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);
extern void stirbuildempty(Relation index);
extern IndexBulkDeleteResult *stirbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
											 void *callback_state);
extern IndexBulkDeleteResult *stirvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);
extern bytea *stiroptions(Datum reloptions, bool validate);

#endif							/* STIR_H */
