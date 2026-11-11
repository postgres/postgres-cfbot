/*-------------------------------------------------------------------------
 *
 * fileops_xlog.h
 *	  Transactional file operations XLOG resource manager definitions
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/access/fileops_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILEOPS_XLOG_H
#define FILEOPS_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/* XLOG stuff - all record types defined upfront for WAL compatibility */
#define XLOG_FILEOPS_CREATE			0x00
#define XLOG_FILEOPS_DELETE			0x10
#define XLOG_FILEOPS_RENAME			0x20
#define XLOG_FILEOPS_WRITE			0x30
#define XLOG_FILEOPS_TRUNCATE		0x40
#define XLOG_FILEOPS_CHMOD			0x50
#define XLOG_FILEOPS_CHOWN			0x60
#define XLOG_FILEOPS_MKDIR			0x70
#define XLOG_FILEOPS_RMDIR			0x80
#define XLOG_FILEOPS_SYMLINK		0x90
#define XLOG_FILEOPS_LINK			0xA0
#define XLOG_FILEOPS_SETXATTR		0xB0
#define XLOG_FILEOPS_REMOVEXATTR	0xC0

/* Resource manager functions */
extern void fileops_redo(XLogReaderState *record);
extern void fileops_desc(StringInfo buf, XLogReaderState *record);
extern const char *fileops_identify(uint8 info);

#endif							/* FILEOPS_XLOG_H */
