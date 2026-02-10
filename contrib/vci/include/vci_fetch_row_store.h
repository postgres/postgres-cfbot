/*-------------------------------------------------------------------------
 * vci_fetch_row_store.h
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_fetch_row_store.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_FETCH_ROW_STORE_H
#define VCI_FETCH_ROW_STORE_H

#include "access/heapam.h"

struct VciScanState;

extern void VciExecAssignScanProjectionInfo(struct VciScanState *node);
extern HeapTuple vci_heap_getnext(struct VciScanState *scanstate, HeapScanDesc scan, ScanDirection direction);

#endif							/* VCI_FETCH_ROW_STORE_H */
