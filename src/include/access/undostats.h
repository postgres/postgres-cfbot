/*-------------------------------------------------------------------------
 *
 * undostats.h
 *	  UNDO log statistics collection and reporting
 *
 * Provides monitoring and observability for the UNDO subsystem,
 * including per-log statistics and buffer cache statistics.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undostats.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDOSTATS_H
#define UNDOSTATS_H

#include "access/undolog.h"
#include "fmgr.h"

/*
 * UndoLogStat - Per-log statistics snapshot
 *
 * Point-in-time snapshot of a single UNDO log's state.
 */
typedef struct UndoLogStat
{
	uint32		log_number;		/* UNDO log number */
	UndoRecPtr	insert_ptr;		/* Current insert pointer */
	UndoRecPtr	discard_ptr;	/* Current discard pointer */
	TransactionId oldest_xid;	/* Oldest transaction in this log */
	uint64		size_bytes;		/* Active size (insert - discard) */
	UndoLogState state;			/* Current lifecycle state */
} UndoLogStat;

/*
 * UndoBufferStat - UNDO buffer cache statistics
 *
 * Aggregate statistics from the UNDO buffer cache.
 */
typedef struct UndoBufferStat
{
	int			num_buffers;	/* Number of buffer slots */
	uint64		cache_hits;		/* Total cache hits */
	uint64		cache_misses;	/* Total cache misses */
	uint64		cache_evictions;	/* Total evictions */
	uint64		cache_writes;	/* Total dirty buffer writes */
} UndoBufferStat;

/* Functions for collecting statistics */
extern int	GetUndoLogStats(UndoLogStat *stats, int max_stats);
extern void GetUndoBufferStats(UndoBufferStat *stats);

/*
 * pg_undo_force_discard is declared via PG_FUNCTION_INFO_V1 in
 * undostats.c.  Do not redeclare it here: on Windows that emits a
 * __declspec(dllimport) prototype that conflicts with the implicit
 * dllexport from the V1 info macro.  Catalog references go through
 * pg_proc by name.
 */

#endif							/* UNDOSTATS_H */
