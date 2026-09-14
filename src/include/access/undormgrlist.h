/*-------------------------------------------------------------------------
 *
 * undormgrlist.h
 *
 * List of *UndoRmgrInit() initialization calls for built-in UNDO resource
 * managers.  Kept in its own source file, mirroring the pattern used by
 * storage/subsystemlist.h for ShmemCallbacks registration, so that the
 * generic UNDO core (undo.c) can register every built-in resource manager
 * without knowing any of their names at compile time: undo.c only expands
 * this list through a macro it controls, and the extern declarations are
 * generated the same way (see access/undormgrs.h).
 *
 * Each access method or subsystem that writes UNDO records adds itself here
 * when it is compiled in.  This file is edited by whichever commit
 * introduces a new UNDO-writing consumer; undo.c itself is never touched to
 * add a new consumer.
 *
 * UNDO_RMGR_INIT is defined by the caller depending on how the list is used.
 *
 * No built-in resource managers exist yet at this point in the UNDO
 * subsystem's history; this file starts empty and grows as each consumer
 * (an index AM, a table AM, or a subsystem that writes UNDO records) is
 * compiled in and adds itself here.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undormgrlist.h
 *
 *-------------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef UNDORMGRLIST_H here */
