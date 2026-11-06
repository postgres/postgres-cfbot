/*-------------------------------------------------------------------------
 *
 * batchscan.h
 *	  Support routines for index access methods' amgetbatch functions
 *	  (and for amgetbitmap functions that are implemented using batches).
 *
 * Index AMs call the functions declared here to allocate, unlock, and release
 * batches.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/batchscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BATCHSCAN_H
#define BATCHSCAN_H

#include "access/genam.h"
#include "access/relscan.h"
#include "storage/buf.h"

extern void batchscan_unlock(IndexScanDesc scan, IndexScanBatch batch,
							 Buffer buf);
extern IndexScanBatch batchscan_alloc(IndexScanDesc scan);
extern void batchscan_release(IndexScanDesc scan, IndexScanBatch batch);

#endif							/* BATCHSCAN_H */
