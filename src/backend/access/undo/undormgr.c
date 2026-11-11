/*-------------------------------------------------------------------------
 *
 * undormgr.c
 *	  UNDO resource manager registration and dispatch
 *
 * This module manages the registration table for UNDO resource managers.
 * Each access method or subsystem that writes UNDO records registers
 * its callbacks here.  The generic UNDO infrastructure dispatches to
 * the appropriate callback based on the urec_rmid in the record header.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undormgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/undormgr.h"

/* Global registration table, indexed by RM ID */
const UndoRmgrData *UndoRmgrs[MAX_UNDO_RMGRS];

/*
 * RegisterUndoRmgr - Register an UNDO resource manager
 *
 * Called by each AM/subsystem during initialization to register its
 * UNDO apply and describe callbacks.
 */
void
RegisterUndoRmgr(uint8 rmid, const UndoRmgrData *rmgr)
{
	if (rmid == UNDO_RMID_INVALID)
		elog(ERROR, "cannot register UNDO RM with invalid ID 0");

	if (UndoRmgrs[rmid] != NULL)
		elog(ERROR, "UNDO RM ID %u already registered as \"%s\"",
			 rmid, UndoRmgrs[rmid]->rm_name);

	if (rmgr->rm_undo == NULL)
		elog(ERROR, "UNDO RM \"%s\" must provide an rm_undo callback",
			 rmgr->rm_name ? rmgr->rm_name : "(null)");

	UndoRmgrs[rmid] = rmgr;
}

/*
 * GetUndoRmgr - Look up an UNDO resource manager by ID
 *
 * Returns the registration entry, or NULL if not registered.
 */
const UndoRmgrData *
GetUndoRmgr(uint8 rmid)
{
	return UndoRmgrs[rmid];
}

/*
 * InitUndoRmgrs - Initialize the UNDO resource manager table
 *
 * Called during postmaster startup.  Individual RMs register themselves
 * via RegisterUndoRmgr() during their initialization.
 */
void
InitUndoRmgrs(void)
{
	MemSet(UndoRmgrs, 0, sizeof(UndoRmgrs));
}
