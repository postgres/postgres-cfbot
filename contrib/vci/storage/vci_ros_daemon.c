/*-------------------------------------------------------------------------
 *
 * vci_ros_daemon.c
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_ros_daemon.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "c.h"
#include "catalog/index.h"
#include "catalog/pg_database.h"
#include "fmgr.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker.h"
#include "storage/bufpage.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"	/* for TransactionIdIsInProgress() */
/* #include "storage/shmem.h" */
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "pgstat.h"

#include "vci.h"
#include "vci_mem.h"
#include "vci_ros.h"
#include "vci_ros_daemon.h"
#include "vci_ros_command.h"

#include "vci_memory_entry.h"

static bool TryToOpenVCIRelations(Oid indexOid, LOCKMODE heapLock, LOCKMODE indexLock,
								  Relation *heapRel, Relation *indexRel);
static void CheckRosControlWorkerCancel(void);

/* BGW_MAXREN = 64 */
/* If the ROS control worker name is changed then update the bgw_name check in LockAcquire() too.*/
static const char VCI_ROS_CONTROL_DAEMON_NAME[BGW_MAXLEN] = "vci:ROS control daemon";
static const char VCI_ROS_CONTROL_WORKER_NAME_TEMP[BGW_MAXLEN] = "vci:ROS control worker(slot=%d)";
static const char VCI_ROS_CONTROL_WORKER_TYPE[BGW_MAXLEN] = "vci:ROS control worker";

/* flags set by signal handlers */
static volatile sig_atomic_t gotSighup = false;
static volatile sig_atomic_t gotSigterm = false;

static vci_workerslot_t *workerslot;

static char probeMessage[num_vci_rc][1024] =
{
	"     data WOS count : %8d / %8d.",
	" whiteout WOS count : %8d / %8d.",
	"                CDR : %8d / %8d (extent %d).",
	"                CDE : %8d / %8d (extent %d).",
	"            TIDCRID : %8d / %8d.",
};

/* ------------  daemon  -------------- */

/**
 * Register ROS Control daemon function called from _PG_init_
 */
void
vci_ROS_control_daemon_setup(void)
{
	BackgroundWorker worker;

	/* for internal use */
	if (VciGuc.enable_ros_control_daemon == false)
	{
		elog(DEBUG1, "vci: no daemon mode");
		return;
	}

	memset(&worker, 0, sizeof(worker));
	/* set up common data for all our workers */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	/* worker.bgw_start_time = BgWorkerStart_ConsistentState; */
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	/* worker.bgw_start_time = BgWorkerStart_PostmasterStart; */

	worker.bgw_restart_time = VCI_DAEMON_RESTART_TIME;
	worker.bgw_notify_pid = 0;

	snprintf(worker.bgw_name, BGW_MAXLEN, VCI_ROS_CONTROL_DAEMON_NAME);
	snprintf(worker.bgw_type, BGW_MAXLEN, VCI_ROS_CONTROL_DAEMON_NAME);
	strcpy(worker.bgw_library_name, VCI_STRING);
	strcpy(worker.bgw_function_name, "vci_ROS_control_daemon_main");
	worker.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&worker);
}

/**
 * Signal handler for SIGTERM
 *
 * @description
 *	Set a flag to let the main loop to terminate, and set our latch to wake it up.
 *
 * @param[in] SIGNAL_ARGS
 */
static void
vci_ROSControlDaemonSigterm(SIGNAL_ARGS)
{
	gotSigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

/**
 * Signal handler for SIGHUP
 *
 * @description
 * Set a flag to tell the main loop to reread the config file, and set
 * our latch to wake it up.
 *
 * @params[in] SIGNAL_ARGS
 */
static void
vci_ROSControlDaemonSighup(SIGNAL_ARGS)
{
	gotSighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

/**
 *  ROS control DAEMON's entory point.
 */
void
vci_ROS_control_daemon_main(Datum main_arg)
{
	/*
	 * XXX - VCI wants to pretend this worker is like an autovacuum launcher;
	 * Let's set the MyBackendType to achieve this.
	 */
	MyBackendType = B_AUTOVAC_LAUNCHER;

	pg_bindtextdomain(TEXTDOMAIN);

	/* StringInfoData buf; */
	elog(DEBUG1, "start initialize %s", MyBgworkerEntry->bgw_name);

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, vci_ROSControlDaemonSighup);
	pqsignal(SIGTERM, vci_ROSControlDaemonSigterm);
	pqsignal(SIGQUIT, vci_ROSControlDaemonSigterm);
	pqsignal(SIGINT, vci_ROSControlDaemonSigterm);

	/* pqsignal(SIGUSR1, vci_ROSNotify); */

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(NULL, NULL, 0);	/* Connect to Shared
															 * database */

	/* Connect DB to access common system catalog */

	workerslot = palloc0_array(vci_workerslot_t, VciGuc.control_max_workers);

	/* Main loop  */
	while (!gotSigterm)
	{
		int			rc;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   VciGuc.control_naptime * INT64CONST(1000),
					   PG_WAIT_EXTENSION);
		ResetLatch(&MyProc->procLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);		/* abnormal end */

		if (gotSigterm)
			goto done;

		LWLockAcquire(VciShmemAddr->io_load_lock, LW_EXCLUSIVE);

		/* Check VCI' database is exists */
		vci_RemoveMemoryEntryOnDroppedDatabase();

		vci_update_memoryentry_in_devloadinfo();

		if (gotSigterm)
		{
			LWLockRelease(VciShmemAddr->io_load_lock);
			goto done;
		}

		VciShmemAddr->translated_dev_pos = 0;

		elog(DEBUG2, ">>> 1. control_max_workers = %d", VciGuc.control_max_workers);
		for (int i = 0; i < VciGuc.control_max_workers; i++)
		{
			elog(DEBUG2, ">>> 1. workerslot[%d].pid is %d", i, (int) workerslot[i].pid);
			if (workerslot[i].pid != 0)
			{
				pid_t		pid;
				BgwHandleStatus status;

				status = GetBackgroundWorkerPid(&workerslot[i].handle, &pid);
				switch (status)
				{
					case BGWH_STOPPED:
						workerslot[i].pid = 0;
						break;
					case BGWH_NOT_YET_STARTED:
					case BGWH_POSTMASTER_DIED:
					case BGWH_STARTED:
						break;
					default:
						/* LCOV_EXCL_START */
						elog(PANIC, "invalid BgwHandleStatus in vci_ROS_control_daemon_main");
						/* LCOV_EXCL_STOP */
						break;
				}

				if (gotSigterm)
				{
					LWLockRelease(VciShmemAddr->io_load_lock);
					goto done;
				}
			}
		}

		LWLockAcquire(VciShmemAddr->memory_entries->lock, LW_SHARED);

		vci_ResetDevloadCurrentPos();

		if (!fullPageWrites)
			goto reload_configuration;

		elog(DEBUG2, ">>> 2. control_max_workers = %d", VciGuc.control_max_workers);
		for (int i = 0; i < VciGuc.control_max_workers; i++)
		{
			elog(DEBUG2, ">>> 2. workerslot[%d].pid is %d", i, (int) workerslot[i].pid);
			if (workerslot[i].pid == 0)
			{
				bool		worker_running = false;

				if (!vci_GetWosRosConvertingVCI(&VciShmemAddr->worker_args_array[i]))
					break;

				Assert(OidIsValid(VciShmemAddr->worker_args_array[i].dbid));
				Assert(OidIsValid(VciShmemAddr->worker_args_array[i].oid));

				for (int j = 0; j < VciGuc.control_max_workers; j++)
				{
					if (workerslot[j].pid != 0 &&
						workerslot[j].dbid == VciShmemAddr->worker_args_array[i].dbid &&
						workerslot[j].oid == VciShmemAddr->worker_args_array[i].oid)
					{
						elog(DEBUG1, "a worker is running on VCI (oid=%d, dbid=%d)",
							 VciShmemAddr->worker_args_array[i].oid,
							 VciShmemAddr->worker_args_array[i].dbid);
						worker_running = true;
						break;
					}
				}

				if (!worker_running)
				{
					workerslot[i] = vci_LaunchROSControlWorker(i);
					workerslot[i].oid = VciShmemAddr->worker_args_array[i].oid;
					workerslot[i].dbid = VciShmemAddr->worker_args_array[i].dbid;
				}
			}

		}

		/*
		 * In case of a SIGHUP, just reload the configuration. (?)
		 */
reload_configuration:
		if (gotSighup)
		{
			gotSighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		vci_MoveTranslatedVCI2Tail();

		LWLockRelease(VciShmemAddr->memory_entries->lock);

		LWLockRelease(VciShmemAddr->io_load_lock);
	}

done:

	/*
	 * Daemon terminate by exit code=1, restart by postmaster as necessary.
	 */
	proc_exit(1);
}

/* ------------  Worker  -------------- */

vci_workerslot_t
vci_LaunchROSControlWorker(int slot_id)
/* vci_database_priority_t *item, */
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	pid_t		pid;

	vci_workerslot_t result;

	/* Assert(MyDatabaseId == InvalidOid); */

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION |
		BGWORKER_INTERRUPTIBLE;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;

	sprintf(worker.bgw_library_name, VCI_STRING);
	sprintf(worker.bgw_function_name, "vci_ROS_control_worker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, VCI_ROS_CONTROL_WORKER_NAME_TEMP, slot_id);
	snprintf(worker.bgw_type, BGW_MAXLEN, VCI_ROS_CONTROL_WORKER_TYPE);

	worker.bgw_main_arg = Int32GetDatum(slot_id);
	worker.bgw_notify_pid = 0;	/* don't notify by SIG_USR1 since it calls
								 * SetLatch and and awakens the parent process
								 * ROS daemon. That results ROS daemon
								 * spawning unnecessary multiple ROS control
								 * workers. */

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background process"),
				 errhint("You may need to increase max_worker_processes.")));

	/* Wait for workers to become ready. */
	while (true)
	{
		BgwHandleStatus status;

		status = GetBackgroundWorkerPid(handle, &pid);
		if (gotSigterm)
			break;

		switch (status)
		{
			case BGWH_NOT_YET_STARTED:
				continue;

			case BGWH_STARTED:
				goto done;

			case BGWH_STOPPED:
				pid = 0;
				goto done;

			case BGWH_POSTMASTER_DIED:
				pid = 0;
				goto done;

			default:
				/* LCOV_EXCL_START */
				elog(PANIC, "should not reach here");
				/* LCOV_EXCL_STOP */
				goto done;
		}
	}

done:
	result.pid = pid;
	result.handle = *handle;

	pfree(handle);

	return result;
}

/**
 *
 */
static inline bool
vci_GetRosCommandExecFlag(char flag, vci_ros_command_t command_id)
{
	return (flag & (1 << command_id)) != 0;
}

static inline void
vci_SetRosCommandExecFlag(char *flag, vci_ros_command_t command_id)
{
	*flag |= (1 << command_id);
}

static int
determine_ExecCommand_and_Extent(const Oid vci_oid,
								 char *targetExecFlag,
								 int32 *targetExtentForCdr,
								 bool force_wosros_conv)
{
	Relation	indexRel;
	Relation	heapRel;

	/* Transaction Start */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Try to open the heap relation & the index relation. */
	if (!TryToOpenVCIRelations(vci_oid, AccessShareLock, AccessShareLock,
							   &heapRel, &indexRel))
	{
		AbortCurrentTransaction();
		return -1;
	}

	/* Check request for ros control worker cancel. */
	CheckRosControlWorkerCancel();

	MemSet(targetExecFlag, 0, sizeof(char));
	MemSet(targetExtentForCdr, 0, sizeof(int32));

	for (vci_ros_command_t command = 0; command < num_vci_rc; command++)
	{
		int32		count = 0;
		vci_target_extent_info_t extent_info = {0, -1};
		int32		targetExtentId;

		switch (command)
		{
			case vci_rc_wos_ros_conv:
				/* 1. count DataWOS */
				count = vci_CountFreezedInDataWos(indexRel, MaxAllocSize);
				break;

			case vci_rc_update_del_vec:
				/* 2. count WhiteoutWOS */
				count = vci_CountFreezedInWhiteoutWos(indexRel, MaxAllocSize);
				break;

			case vci_rc_collect_deleted:
				/* 3. count deleted rows in each extent */
				extent_info = vci_CountDeletedRowsInROS(indexRel, (uint32) VciGuc.cdr_threshold);
				break;

			case vci_rc_update_tid_crid:
				/* 5. count TID->CRID update list */
				count = vci_CountTidCridUpdateListLength(indexRel, MaxAllocSize);
				break;

			case vci_rc_collect_extent:
				/* 6. count unused extents */
				extent_info = vci_CountUnusedExtents(indexRel);
				break;

			default:
				/* LCOV_EXCL_START */
				elog(ERROR, "unexpected ROS command");
				/* LCOV_EXCL_STOP */
				break;
		}

		switch (command)
		{
			case vci_rc_wos_ros_conv:
				elog(DEBUG2, &probeMessage[vci_rc_wos_ros_conv][0], count, VciGuc.wosros_conv_threshold);
				if (force_wosros_conv || count >= VciGuc.wosros_conv_threshold)
					vci_SetRosCommandExecFlag(targetExecFlag, vci_rc_wos_ros_conv);
				break;

			case vci_rc_update_del_vec:
				elog(DEBUG2, &probeMessage[vci_rc_update_del_vec][0], count, VCI_UPDATE_DELVEC_THRESHOLD);
				if (force_wosros_conv || count >= VCI_UPDATE_DELVEC_THRESHOLD)
					vci_SetRosCommandExecFlag(targetExecFlag, vci_rc_update_del_vec);
				break;

			case vci_rc_update_tid_crid:
				elog(DEBUG2, &probeMessage[vci_rc_update_tid_crid][0], count, VCI_UPDATE_TIDCRID_THRESHOLD);
				if (count >= VCI_UPDATE_TIDCRID_THRESHOLD)
					vci_SetRosCommandExecFlag(targetExecFlag, vci_rc_update_tid_crid);
				break;

			case vci_rc_collect_extent:
			case vci_rc_collect_deleted:
				targetExtentId = VCI_INVALID_EXTENT_ID;
				if (extent_info.num_fit_extents > 0)
				{
					targetExtentId = extent_info.best_extent_id;

					if (command == vci_rc_collect_deleted)
						*targetExtentForCdr = targetExtentId;

					vci_SetRosCommandExecFlag(targetExecFlag, command);
				}
				break;

			default:
				/* LCOV_EXCL_START */
				elog(ERROR, "unexpected ROS command");
				/* LCOV_EXCL_STOP */
				break;
		}
	}

	/* unlock VCI main rel */
	index_close(indexRel, AccessShareLock);

	table_close(heapRel, AccessShareLock);

	/* Transaction End */
	PopActiveSnapshot();
	CommitTransactionCommand();

	return 0;
}

/**
 * update ROS
 *
 * @param[in] targetIndexOid target index oid
 * @param[in] targetExecCommandFlag target exec commands
 * @param[in] targetExtentId target extent id
 * @param[out] num_converted_data_wos number of rows coverted in Data WOS
 * @param[out] num_converted_whiteout_wos number of rows converted in Whiteout WOS
 */
static void
vci_executeROScommand(Oid targetIndexOid, char targetExecCommandFlag, int32 targetExtentId,
					  int *num_converted_data_wos, int *num_converted_whiteout_wos)
{
	/*
	 * loop for executing ROS commaand each command is excuted in anoter
	 * Transaction();
	 */
	for (vci_ros_command_t command = 0; command < num_vci_rc; command++)
	{
		if (vci_GetRosCommandExecFlag(targetExecCommandFlag, command))
		{
			Relation	mainRel;
			Relation	heapRel;
			Size		workAreaSize = VciGuc.maintenance_work_mem * INT64CONST(1024);

			instr_time	s_time;
			instr_time	e_time;
			volatile Snapshot snapshot;

			/* Check request for ros control worker cancel. */
			CheckRosControlWorkerCancel();

			/* transaction start */
			SetCurrentStatementStartTimestamp();
			StartTransactionCommand();
			snapshot = GetTransactionSnapshot();
			PushActiveSnapshot(snapshot);
			GetCurrentTransactionId();

			/** Try to open the heap relation & the index relation,
			 * and get ShareUpdateExclusiveLock for the index relation. */
			if (!TryToOpenVCIRelations(targetIndexOid, AccessShareLock, ShareUpdateExclusiveLock,
									   &heapRel, &mainRel))
			{
				/* Exit worker process. */
				AbortCurrentTransaction();
				return;
			}

			elog(LOG, "starts ROS command \"%s\"", vci_GetRosCommandName(command));
			INSTR_TIME_SET_CURRENT(s_time);

			switch (command)
			{
				case vci_rc_wos_ros_conv:
					/* 1. WOS->ROS conversion */
					*num_converted_data_wos = vci_ConvertWos2Ros(mainRel, workAreaSize, VciGuc.wosros_conv_threshold);
					break;

				case vci_rc_update_del_vec:
					/* 2. update delete vector */
					*num_converted_whiteout_wos = vci_UpdateDelVec(mainRel, workAreaSize, VCI_UPDATE_DELVEC_THRESHOLD);
					break;

				case vci_rc_collect_deleted:
					/* 3. collect deleted rows */
					vci_CollectDeletedRows(mainRel, workAreaSize, targetExtentId);
					break;

				case vci_rc_update_tid_crid:
					/* 5. update TID->CRID update list to TID-CRID  tree */
					vci_UpdateTidCrid(mainRel, workAreaSize, 10000);
					break;

				case vci_rc_collect_extent:
					/* 6. collect an unused extent */
					vci_CollectUnusedExtent(mainRel, workAreaSize);
					break;

				default:
					/* LCOV_EXCL_START */
					elog(ERROR, "unexpected ROS command");
					/* LCOV_EXCL_STOP */
					break;
			}

			index_close(mainRel, ShareUpdateExclusiveLock);
			table_close(heapRel, AccessShareLock);

			PopActiveSnapshot();
			CommitTransactionCommand();

			INSTR_TIME_SET_CURRENT(e_time);
			INSTR_TIME_SUBTRACT(e_time, s_time);
			elog(LOG, "finished ROS command \"%s\" (%.03f ms)", vci_GetRosCommandName(command),
				 INSTR_TIME_GET_MILLISEC(e_time));
		}
	}
}

/*
 * @param[in] dboid id of db to which the worker connects.
 * @pramm[in] username user name
 */
static void
BackgroundWorkerInitializeConnectionByOid1(Oid dboid, const char *username)
{
	BackgroundWorker *worker = MyBgworkerEntry;

	/* XXX is this the right errcode? */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(NULL, dboid, username, InvalidOid, 0, NULL);

	/* it had better not gotten out of "init" mode yet */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/**
 * @param[in] main_arg id of vci, a WOS->ROS transfomation of which is performed.
 */
void
vci_ROS_control_worker_main(Datum main_arg)
{
	Oid			targetIndexOid = InvalidOid;
	int32		targetExtentId = 01;
	char		targetExecCommandFlag = 0x00;

	Oid			dboid;
	vci_wosros_conv_worker_arg_t *vciinfo;
	int			slot_id = DatumGetInt32(main_arg);
	int			ret;
	int			num_converted_data_wos = INT_MAX;
	int			num_converted_whiteout_wos = INT_MAX;

	pg_bindtextdomain(TEXTDOMAIN);

	pqsignal(SIGHUP, vci_ROSControlDaemonSighup);
	pqsignal(SIGTERM, vci_ROSControlDaemonSigterm);
	pqsignal(SIGQUIT, vci_ROSControlDaemonSigterm);
	pqsignal(SIGINT, vci_ROSControlDaemonSigterm);
	/* pqsignal(SIGUSR1, vci_ROSNotify); */

	/* Check full_page_writers=off */
	if (!fullPageWrites)
		return;

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/*
	 * Checkout the Postmaster was rebooted. if
	 * (MyBgworkerEntry->bgw_notify_pid == 0) return;
	 */

	/* Connect to DB corresponding to dbid */

	vciinfo = (vci_wosros_conv_worker_arg_t *) &VciShmemAddr->worker_args_array[slot_id];
	targetIndexOid = vciinfo->oid;
	dboid = vciinfo->dbid;

	BackgroundWorkerInitializeConnectionByOid1(dboid, NULL);

	elog(DEBUG1, "worker: connect to %d is OK. do wos->ros conversion on vci %d", dboid, targetIndexOid);

	ret = determine_ExecCommand_and_Extent(targetIndexOid, &targetExecCommandFlag,
										   &targetExtentId, vciinfo->force_next_wosros_conv);

	if (ret == 0)
		vci_executeROScommand(targetIndexOid, targetExecCommandFlag, targetExtentId,
							  &num_converted_data_wos, &num_converted_whiteout_wos);

	if (vciinfo->force_next_wosros_conv &&
		num_converted_data_wos == 0 &&
		num_converted_whiteout_wos == 0)
	{
		vci_id_t	vciid;

		vciid.oid = targetIndexOid;
		vciid.dbid = dboid;

		vci_SetForceNextWosRosConvFlag(&vciid, false);
	}

}

/**
 * Try to open the heap relation & the index relation.
 * open the heap relation to detect AccessExclusiveLock of the heap
 * relation, before opening the index relation.
 */
static bool
TryToOpenVCIRelations(Oid indexOid, LOCKMODE heapLock, LOCKMODE indexLock,
					  Relation *heapRel, Relation *indexRel)
{
	Oid			heapOid;

	heapOid = IndexGetRelation(indexOid, true);
	if (OidIsValid(heapOid))
	{
		*heapRel = try_relation_open(heapOid, heapLock);
		if (*heapRel != NULL)
		{
			*indexRel = try_relation_open(indexOid, indexLock);
			if (*indexRel != NULL)
			{
				if (isVciIndexRelation(*indexRel))
					return true;

				relation_close(*indexRel, indexLock);
			}

			relation_close(*heapRel, heapLock);
		}
	}

	elog(DEBUG1, "worker: The relation the OID=%d indicates was deleted.", indexOid);

	return false;
}

/**
 * Check request for ros control worker cancel.
 */
static void
CheckRosControlWorkerCancel(void)
{
#ifdef WIN32
	if (UNBLOCKED_SIGNAL_QUEUE())
		pgwin32_dispatch_queued_signals();
#endif							/* WIN32 */

	if (gotSigterm)
	{
		ereport(DEBUG1,
				(errcode(ERRCODE_ADMIN_SHUTDOWN),
				 errmsg_internal("terminating VCI worker process due to administrator command")));
		/* process terminate. */
		exit(1);

	}
}
