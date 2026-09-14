/*-------------------------------------------------------------------------
 *
 * undormgr.h
 *	  UNDO resource manager dispatch definitions
 *
 * This module provides a dispatch mechanism for UNDO record application,
 * analogous to the WAL resource manager (rmgr) system.  Each access method
 * or subsystem that writes UNDO records registers an UndoRmgrData entry
 * with callbacks for applying UNDO records and describing them for debugging.
 *
 * The generic UNDO infrastructure (undoapply.c) dispatches to the appropriate
 * RM callback based on the urec_rmid field in the UNDO record header.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undormgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDORMGR_H
#define UNDORMGR_H

#include "postgres_ext.h"
#include "access/undodefs.h"
#include "access/xlogdefs.h"
#include "lib/stringinfo.h"

/*
 * UNDO Resource Manager IDs
 *
 * Each AM or subsystem that writes UNDO records is assigned a unique ID.
 * This ID is stored in the urec_rmid field of every UNDO record header,
 * enabling the generic UNDO infrastructure to dispatch to the correct
 * apply callback during rollback.
 *
 * The core defines only UNDO_RMID_INVALID and the built-in index-AM IDs.
 * Every other consumer defines its own UNDO_RMID_* constant in its own
 * header (the same commit that registers it in access/undormgrlist.h), so
 * this core header names no specific table AM or subsystem.  The ID number
 * space is a shared resource: IDs must be unique and stable across releases
 * (they are stamped into WAL-durable UNDO records), so a new consumer takes
 * the next free value in [1, MAX_UNDO_RMGRS) and never reuses one.
 */
#define UNDO_RMID_INVALID	0
#define UNDO_RMID_NBTREE	1
#define UNDO_RMID_HASH		3
/* 2, 4, and 5 are reserved by out-of-core consumers (see their own headers) */

#define MAX_UNDO_RMGRS		256

/*
 * UndoApplyResult - Return value from undo apply callbacks
 */
typedef enum UndoApplyResult
{
	UNDO_APPLY_SUCCESS = 0,		/* Successfully applied */
	UNDO_APPLY_SKIPPED,			/* Skipped (e.g., relation dropped) */
	UNDO_APPLY_ERROR			/* Error during application */
} UndoApplyResult;

/*
 * UndoRmgrData - Resource manager registration entry
 *
 * Each UNDO RM provides:
 *   rm_name:  Human-readable name for debugging/logging
 *   rm_undo:  Apply one UNDO record (rollback callback)
 *   rm_desc:  Describe an UNDO record for debugging output
 *
 * The rm_undo callback receives:
 *   - rmid:        The RM ID (for verification)
 *   - info:        RM-specific subtype/flags from urec_info
 *   - xid:         Transaction being rolled back
 *   - reloid:      Target relation OID (may be InvalidOid for non-relation ops)
 *   - payload:     RM-specific opaque payload data
 *   - payload_len: Length of payload
 *   - urec_ptr:    Position of this record in UNDO log (for CLR generation)
 *
 * The callback is responsible for:
 *   - Opening the relation (if applicable)
 *   - Locking and modifying the target page
 *   - Generating a CLR WAL record
 *   - Releasing all locks and buffers
 */
typedef UndoApplyResult (*UndoRmgrApplyFunc) (uint8 rmid,
											  uint16 info,
											  TransactionId xid,
											  Oid reloid,
											  const char *payload,
											  Size payload_len,
											  UndoRecPtr urec_ptr);

typedef void (*UndoRmgrDescFunc) (StringInfo buf,
								  uint8 rmid,
								  uint16 info,
								  const char *payload,
								  Size payload_len);

typedef struct UndoRmgrData
{
	const char *rm_name;		/* Human-readable name */
	UndoRmgrApplyFunc rm_undo;	/* Apply callback */
	UndoRmgrDescFunc rm_desc;	/* Describe callback */
} UndoRmgrData;

/* Global registration table */
extern const UndoRmgrData *UndoRmgrs[MAX_UNDO_RMGRS];

/* Registration function (called during _PG_init or startup) */
extern void RegisterUndoRmgr(uint8 rmid, const UndoRmgrData *rmgr);

/* Lookup function */
extern const UndoRmgrData *GetUndoRmgr(uint8 rmid);

/* Initialization */
extern void InitUndoRmgrs(void);

#endif							/* UNDORMGR_H */
