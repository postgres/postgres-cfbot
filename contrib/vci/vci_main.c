/*-------------------------------------------------------------------------
 *
 * vci_main.c
 *	  VCI main file
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/vci_main.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "commands/tablecmds.h"
#include "common/file_utils.h"
#include "executor/nodeModifyTable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/varlena.h"

#include "vci.h"
#include "vci_mem.h"
#include "vci_ros.h"
#include "vci_ros_daemon.h"

static void vci_xact_callback(XactEvent event, void *arg);
static void vci_subxact_callback(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg);

PG_MODULE_MAGIC_EXT(
					.name = "vci",
					.version = PG_VERSION
);

/* saved hook value in case of unload */
/**
 * Commands which re-index VCI.
 */
vci_RebuildCommand vci_rebuild_command = vcirc_invalid;

ProcessUtility_hook_type process_utility_prev = NULL;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static void vci_shmem_request(void);

/**
 * _PG_init: Entry point of this module.
 * It is called when the module is loaded.
 */
void
_PG_init(void)
{
	pg_bindtextdomain(TEXTDOMAIN);

	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" must be registered in shared_preload_libraries", VCI_STRING)));
		return;					/* LCOV_EXCL_LINE */
	}

	vci_read_guc_variables();

	if (!IsPostmasterEnvironment)
	{
		VciGuc.enable = 0;
		VciGuc.enable_ros_control_daemon = false;
	}

	vci_setup_shmem();

	vci_setup_executor_hook();

	/* register process utilityhook */
	process_utility_prev = ProcessUtility_hook;
	ProcessUtility_hook = vci_process_utility;

	/* register function to custom hook */
	add_index_delete_hook = vci_add_index_delete;
	add_should_index_insert_hook = vci_add_should_index_insert;
	add_drop_relation_hook = vci_add_drop_relation;
	add_reindex_index_hook = vci_add_reindex_index;
	add_skip_vci_index_hook = vci_add_skip_vci_index;
	add_alter_tablespace_hook = vci_add_alter_tablespace;
	add_alter_table_change_owner_hook = vci_alter_table_change_owner;
	add_alter_table_change_schema_hook = vci_alter_table_change_schema;
	add_snapshot_satisfies_hook = VCITupleSatisfiesVisibility;
	add_skip_vacuum_hook = vci_isVciAdditionalRelation;

	/* If single user mode, not set environment for parallel. */
	if (IsPostmasterEnvironment)
	{
		if (!IsUnderPostmaster)
		{
#ifdef WIN32
			struct stat st;
			char	   *dir_name = "base/" PG_TEMP_FILES_DIR;

			if (stat(dir_name, &st) == 0)
			{
				if (!S_ISDIR(st.st_mode))
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("\"%s\" is not directory", dir_name)));
			}
			else
			{
				if (errno == ENOENT)
				{
					if (mkdir(dir_name, S_IRWXU) < 0)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir_name)));
				}
				else
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat directory \"%s\": %m",
									dir_name)));
			}
#endif

			/* Register ROS Control Daemon */
			vci_ROS_control_daemon_setup();
		}
	}
	else
		vci_shmem_startup_routine();

	RegisterXactCallback(vci_xact_callback, NULL);
	RegisterSubXactCallback(vci_subxact_callback, NULL);

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = vci_shmem_request;

}

static void
vci_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	/* Register LWLocks used by VCI */
	RequestNamedLWLockTranche("VciStandbyExec", 1);
	RequestNamedLWLockTranche("VciIOLoad", 1);
	RequestNamedLWLockTranche("VciMemoryEntries", 1);
	RequestNamedLWLockTranche("VciQueryContext", 1);
	RequestNamedLWLockTranche("VciMntpoint2dev", 1);
}

/*
 * Callback function for COMMIT/ABORT/PREPARE operations.
 */
static void
vci_xact_callback(XactEvent event, void *arg)
{
	vci_xact_change_handler(event);
}

/*
 * Callback function for subxact operations.
 */
static void
vci_subxact_callback(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	vci_subxact_change_handler(event, mySubid);
}
