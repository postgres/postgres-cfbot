/*-------------------------------------------------------------------------
 *
 * undormgrs.h
 *	  Provide extern declarations for all the built-in UNDO resource
 *	  manager *Init() functions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undormgrs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDORMGRS_H
#define UNDORMGRS_H

/*
 * Extern declarations of all the built-in *UndoRmgrInit() functions.
 *
 * The actual list is in undormgrlist.h, so that the same list can be used
 * for other purposes (e.g. undo.c's RegisterUndoRmgrs()).
 */
#define UNDO_RMGR_INIT(initfunc) \
	extern void initfunc(void);
#include "access/undormgrlist.h"
#undef UNDO_RMGR_INIT

#endif							/* UNDORMGRS_H */
