/*-------------------------------------------------------------------------
 *
 * vci_columns_data.h
 *	  Declarations of functions to check which columns are indexed.
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_columns_data.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_COLUMNS_DATA_H
#define VCI_COLUMNS_DATA_H

#include "access/tupdesc.h"
#include "access/attnum.h"
#include "nodes/bitmapset.h"
#include "storage/lock.h"
#include "utils/palloc.h"
#include "utils/rel.h"

#include "vci_ros.h"

extern TupleDesc vci_ExtractColumnDataUsingIds(const char *vci_column_ids, Relation heapRel);
extern PGDLLEXPORT TupleDesc vci_GetTupleDescr(vci_MainRelHeaderInfo *info);
extern Bitmapset *vci_MakeIndexedColumnBitmap(Oid mainRelationOid, MemoryContext sharedMemCtx, LOCKMODE lockmode);
extern Bitmapset *vci_MakeDroppedColumnBitmap(Relation indexRel);
extern char *vci_ConvertAttidBitmap2String(Bitmapset *attid_bitmap);
extern AttrNumber vci_GetAttNum(TupleDesc desc, const char *name);

#endif							/* VCI_COLUMNS_DATA_H */
