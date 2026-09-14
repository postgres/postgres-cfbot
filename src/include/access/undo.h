/*-------------------------------------------------------------------------
 *
 * undo.h
 *	  Common undo layer interface
 *
 * The undo subsystem consists of several logically separate subsystems
 * that work together:
 *
 *   undolog.c       - Undo log file management and space allocation
 *   undorecord.c    - Record format, serialization, and UndoRecordSet
 *   xactundo.c      - Per-transaction record set management
 *   undoapply.c     - Physical undo application during rollback
 *   undoworker.c    - Background discard worker
 *   undo_bufmgr.c   - Buffer management via shared_buffers
 *   undo_xlog.c     - WAL redo routines
 *
 * This header provides the unified entry points for shared memory
 * initialization and startup/shutdown coordination across all undo
 * subsystems.  The design follows the EDB undo-record-set branch
 * pattern where UndoShmemSize()/UndoShmemInit() aggregate the
 * requirements of all subsystems.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDO_H
#define UNDO_H

#include "access/undodefs.h"
#include "utils/palloc.h"

/*
 * Unified shared memory initialization.
 *
 * UndoShmemSize() computes the total shared memory needed by all undo
 * subsystems.  UndoShmemInit() initializes all undo shared memory
 * structures.  These are called from ipci.c during postmaster startup.
 */
extern Size UndoShmemSize(void);
extern void UndoShmemInit(void);

/* Per-backend initialization */
extern void InitializeUndo(void);

/* Memory context for undo-related allocations */
extern MemoryContext UndoContext;

#endif							/* UNDO_H */
