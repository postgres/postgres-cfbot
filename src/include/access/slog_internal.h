/*-------------------------------------------------------------------------
 *
 * slog_internal.h
 *	  Shared-state definitions private to the sLog implementation
 *
 * The sLog is split across two translation units that share one
 * shared-memory segment: slog.c (the always-present transaction Aborted
 * Transaction Map) and slog_tuple.c (the optional per-tuple flat-hash
 * tracking extension).  This header exposes the shared state struct and the
 * few globals both files touch, so it is deliberately NOT part of the public
 * sLog API in access/slog.h.  Only the two sLog .c files include it.
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

#include "access/slog_flathash.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"

/*
 * Initial size for the sLog DSA area (backs the aborted-txn radix tree).
 * Grows dynamically as needed up to slog_dsa_max_size_mb.
 */
#define SLOG_DSA_INIT_SIZE		(512 * 1024)	/* 512 KB */

/*
 * Shared state for the whole sLog subsystem.
 *
 * The transaction ATM fields are always used.  The tuple flat-hash fields
 * (tuple_partitions, num_partitions) are used only when the tuple sLog
 * extension is compiled in and an access method has opted in; they are
 * initialized unconditionally in SLogShmemInit() so the two files can share
 * one segment without an init-ordering dependency.
 */
typedef struct SLogSharedState
{
	/* Transaction ATM (adaptive radix tree in the DSA area below) */
	dsa_pointer atm_handle;		/* RT handle; InvalidDsaPointer until init */
	LWLockPadded txn_lock;		/* single LWLock serializing ATM access */

	/*
	 * Tuple flat hash: N-way partitioned for reduced writer contention.
	 * Partition count determined at startup by slog_num_partitions GUC.
	 */
	SLogFlatPartition *tuple_partitions;	/* palloc'd array in shmem */
	int			num_partitions; /* actual partition count */

	/* DSA area backing the aborted-txn radix tree */
	dsa_area   *dsa_area;		/* set during SLogShmemInit, NULL until then */
	char		dsa_space[SLOG_DSA_INIT_SIZE];
} SLogSharedState;

/* The single shared-state instance (defined in slog.c). */
extern SLogSharedState *SLogState;

/* Tuple-hash shmem sizing/init helpers (defined in slog_tuple.c). */
extern Size SLogTupleShmemSize(void);
extern void SLogTupleShmemRequest(void);
extern void SLogTupleShmemInit(void);

#endif							/* SLOG_INTERNAL_H */
