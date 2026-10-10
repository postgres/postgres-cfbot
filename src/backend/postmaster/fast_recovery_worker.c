/*-------------------------------------------------------------------------
 *
 * fast_recovery_worker.c
 *	  Auxiliary process that recovers the pages left pending by fast crash
 *	  recovery, so that none is left for a backend to replay on first use
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/fast_recovery_worker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/lsn_indexer.h"
#include "access/rmgr.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "lib/dshash.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgwriter.h"
#include "postmaster/fast_recovery_worker.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"

/*
 * FastRecoveryMain - read every page in the LSN index, which replays it.
 *
 * dshash_seq_next() holds a partition lock until the next call, and reading
 * a page runs on-demand replay, which looks the page up in the same table;
 * LWLocks are not reentrant.  So collect the tags first, end the scan, and
 * only then read the pages.
 */
static void
FastRecoveryMain(void)
{
	dshash_seq_status seq;
	PageLSNEntry *entry;
	BufferTag  *tags = NULL;
	int			ntags = 0;
	int			ncap = 0;

	ereport(LOG,
			errmsg("fast recovery worker started"));

	LSNIndexAttach();

	dshash_seq_init(&seq, LSNIndexGetHash(), false);
	while ((entry = (PageLSNEntry *) dshash_seq_next(&seq)) != NULL)
	{
		if (ntags == ncap)
		{
			ncap = ncap ? ncap * 2 : 1024;
			if (tags == NULL)
				tags = (BufferTag *) palloc(sizeof(BufferTag) * ncap);
			else
				tags = (BufferTag *) repalloc(tags, sizeof(BufferTag) * ncap);
		}
		tags[ntags++] = entry->tag;
	}
	dshash_seq_term(&seq);

	ereport(LOG,
			errmsg_plural("fast recovery worker replaying %d pending page",
						  "fast recovery worker replaying %d pending pages",
						  ntags, ntags));

	/* Reading a page replays its pending records; see bufmgr.c. */
	for (int i = 0; i < ntags; i++)
	{
		Buffer		buffer;

		/* keep up with ProcSignal barriers and config reloads */
		ProcessMainLoopInterrupts();

		buffer = ReadBufferWithoutRelcache(BufTagGetRelFileLocator(&tags[i]),
										   tags[i].forkNum,
										   tags[i].blockNum,
										   RBM_NORMAL, NULL, true);
		ReleaseBuffer(buffer);
	}

	if (tags)
		pfree(tags);

	/*
	 * Every page has been replayed.  End fast recovery: stop new replays,
	 * wait out the running ones, free the index.  Checkpoints are allowed
	 * from then on.
	 */
	LSNIndexFinish();

	/*
	 * Checkpoints were refused until now, so the last one is still the one
	 * from before the crash, and another crash would have to recover all of
	 * that WAL again.  Ask for a new checkpoint, like the end-of-recovery
	 * checkpoint normal crash recovery takes, but without waiting for it.
	 */
	RequestCheckpoint(CHECKPOINT_FORCE);

	ereport(LOG,
			errmsg("fast crash recovery complete"));
}

void
FastRecoveryWorkerMain(const void *startup_data, size_t startup_data_len)
{
	MyBackendType = B_FAST_RECOVERY_WORKER;
	AuxiliaryProcessMainCommon();

	/*
	 * Properly accept or ignore signals that might be sent to us.
	 *
	 * SIGTERM is ignored on purpose.  A smart or fast shutdown must not write
	 * its shutdown checkpoint while pages are still unrecovered, so the
	 * postmaster waits for us to finish instead of stopping us: we are in the
	 * set of processes it waits for in PM_WAIT_BACKENDS.  An immediate
	 * shutdown uses SIGQUIT, whose handler InitPostmasterChild already set
	 * up; it writes no checkpoint, so the next startup recovers again.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, PG_SIG_IGN);
	pqsignal(SIGTERM, PG_SIG_IGN);
	pqsignal(SIGALRM, PG_SIG_IGN);
	pqsignal(SIGPIPE, PG_SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, PG_SIG_IGN);
	pqsignal(SIGCHLD, PG_SIG_DFL);

	/* Unblock signals (they were blocked when the postmaster forked us) */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	FastRecoveryMain();

	proc_exit(0);
}
