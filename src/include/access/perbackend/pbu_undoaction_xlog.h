/*-------------------------------------------------------------------------
 *
 * undoaction_xlog.h
 *	  undo action XLOG definitions
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undoaction_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PBU_UNDOACTION_XLOG_H
#define PBU_UNDOACTION_XLOG_H

#include "access/perbackend/pbu_undolog.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/*
 * WAL record definitions for undoactions.c's WAL operations
 */
#define XLOG_UNDO_APPLY_PROGRESS	0x00
#define XLOG_UNDO_APPLY_SUBXACT		0x10
#define XLOG_UNDO_AMEND_RECORD		0x20

/* Each block carries a uint16 page offset followed by replacement bytes. */

/* This is what we need to know about undo apply progress */
typedef struct xl_undoapply_progress
{
	UndoRecPtr	urec_ptr;
	uint32		progress;
} xl_undoapply_progress;

#define SizeOfUndoActionProgress	(offsetof(xl_undoapply_progress, progress) + sizeof(uint32))

/*
 * An undo record whose change was reverted by ROLLBACK TO SAVEPOINT.  Block 0
 * is the undo page holding the record; redo marks the record as applied.
 */
typedef struct xl_undoapply_subxact
{
	UndoRecPtr	urec_ptr;
} xl_undoapply_subxact;

extern void undoaction_redo(XLogReaderState *record);
extern void undoaction_desc(StringInfo buf, XLogReaderState *record);
extern const char *undoaction_identify(uint8 info);

#endif							/* PBU_UNDOACTION_XLOG_H */
