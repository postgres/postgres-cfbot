/*-------------------------------------------------------------------------
 *
 * Query cancellation support for frontend code
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/cancel.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CANCEL_H
#define CANCEL_H

#include <signal.h>

#include "libpq-fe.h"

extern PGDLLIMPORT volatile sig_atomic_t CancelRequested;

extern void SetCancelConn(PGconn *conn);
extern void ResetCancelConn(void);

extern void setup_cancel_handler(void (*signal_callback) (void),
								 void (*thread_callback) (void));

extern void LockCancelThread(void);
extern void UnlockCancelThread(void);

#ifndef WIN32
extern void ResetCancelAfterFork(void);
extern void CancelSignalHandler(SIGNAL_ARGS);
#endif

#endif							/* CANCEL_H */
