/*-------------------------------------------------------------------------
 *
 * slog_internal.h
 *	  Shared-state definitions private to the sLog implementation
 *
 * The transaction sLog's shared state (slog.c).  Deliberately not part of
 * the public sLog API in access/slog.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slog_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOG_INTERNAL_H
#define SLOG_INTERNAL_H

#include "storage/lwlock.h"
#include "utils/dsa.h"

/*
 * Fixed shared state of the transaction sLog: only its lock and where its
 * tree is.  The tree lives in a DSA area that the first transaction to write
 * UNDO creates.
 */
typedef struct SLogSharedState
{
	LWLockPadded txn_lock;		/* single LWLock serializing ATM access */
	dsa_handle	area;			/* the ATM's DSA area, or DSA_HANDLE_INVALID */
	dsa_pointer atm_handle;		/* the ATM radix tree in it, or
								 * InvalidDsaPointer */
} SLogSharedState;

/* The single shared-state instance (defined in slog.c). */
extern SLogSharedState *SLogState;

#endif							/* SLOG_INTERNAL_H */
