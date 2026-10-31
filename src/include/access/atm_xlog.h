/*-------------------------------------------------------------------------
 *
 * atm_xlog.h
 *	  Aborted Transaction Map XLOG resource manager definitions
 *
 * This header is safe for inclusion from frontend code (e.g., pg_waldump).
 * For the full ATM API, include "access/atm.h" instead.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/atm_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ATM_XLOG_H
#define ATM_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/* WAL record types for RM_ATM_ID */
#define XLOG_ATM_ABORT		0x00
#define XLOG_ATM_FORGET		0x10

/* WAL record structures */
typedef struct xl_atm_abort
{
	TransactionId xid;
	XLogRecPtr	last_batch_lsn; /* LSN of last UNDO batch for this xid */
	XLogRecPtr	stop_lsn;		/* revert only batches newer than this, or
								 * InvalidXLogRecPtr for the whole chain */
	XLogRecPtr	first_batch_lsn;	/* oldest batch of the chain; WAL from
									 * here on is retained until the entry is
									 * forgotten */
	Oid			dboid;
	Oid			reloid;			/* InvalidOid (kept for struct layout) */
	bool		fileops_owned;	/* FILEOPS completed or owned by abort WAL */
} xl_atm_abort;

#define SizeOfXlAtmAbort	(offsetof(xl_atm_abort, fileops_owned) + sizeof(bool))

typedef struct xl_atm_forget
{
	TransactionId xid;
} xl_atm_forget;

#define SizeOfXlAtmForget	sizeof(xl_atm_forget)

/* Resource manager functions */
extern void atm_redo(XLogReaderState *record);
extern void atm_desc(StringInfo buf, XLogReaderState *record);
extern const char *atm_identify(uint8 info);

#endif							/* ATM_XLOG_H */
