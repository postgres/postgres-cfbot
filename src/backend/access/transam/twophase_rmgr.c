/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.c
 *	  Two-phase-commit resource managers tables
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/twophase_rmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/twophase_rmgr.h"
#include "pgstat.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "storage/fileops.h"

/*
 * FILEOPS 2PC callbacks (fileops.c).  Declared locally by prototype to avoid
 * pulling storage/fileops.h in here; the signature is the generic
 * TwoPhaseCallback.  Postcommit performs the deferred commit-time file ops;
 * postabort performs the abort-time filesystem undo.
 */

/*
 * Per-backend UNDO 2PC callbacks (pbu_recovery.c).  Declared locally by
 * prototype: the per-backend engine headers cannot be included here (they
 * redefine types that conflict with the cluster-wide UNDO engine).  The
 * callback signature is the generic TwoPhaseCallback.
 */
extern void pbu_twophase_postabort(FullTransactionId fxid, uint16 info,
								   void *recdata, uint32 len);
extern void pbu_twophase_postcommit(FullTransactionId fxid, uint16 info,
									void *recdata, uint32 len);


const TwoPhaseCallback twophase_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_recover,		/* Lock */
	NULL,						/* pgstat */
	multixact_twophase_recover, /* MultiXact */
	predicatelock_twophase_recover, /* PredicateLock */
	NULL,						/* FILEOPS (post-commit/abort only) */
	NULL						/* per-backend UNDO (postabort-only) */
};

const TwoPhaseCallback twophase_postcommit_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postcommit,	/* Lock */
	pgstat_twophase_postcommit, /* pgstat */
	multixact_twophase_postcommit,	/* MultiXact */
	NULL,						/* PredicateLock */
	fileops_twophase_postcommit,	/* FILEOPS */
	pbu_twophase_postcommit		/* per-backend UNDO */
};

const TwoPhaseCallback twophase_postabort_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postabort,	/* Lock */
	pgstat_twophase_postabort,	/* pgstat */
	multixact_twophase_postabort,	/* MultiXact */
	NULL,						/* PredicateLock */
	fileops_twophase_postabort, /* FILEOPS */
	pbu_twophase_postabort		/* per-backend UNDO */
};

const TwoPhaseCallback twophase_standby_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_standby_recover,	/* Lock */
	NULL,						/* pgstat */
	NULL,						/* MultiXact */
	NULL,						/* PredicateLock */
	NULL,						/* FILEOPS */
	NULL						/* per-backend UNDO */
};
