/*-------------------------------------------------------------------------
 *
 * fast_recovery_worker.h
 *	  Exports from postmaster/fast_recovery_worker.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/postmaster/fast_recovery_worker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FAST_RECOVERY_WORKER_H
#define FAST_RECOVERY_WORKER_H

extern void FastRecoveryWorkerMain(const void *startup_data, size_t startup_data_len);

#endif							/* FAST_RECOVERY_WORKER_H */
