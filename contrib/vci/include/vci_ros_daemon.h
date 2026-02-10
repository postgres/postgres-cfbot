/*-------------------------------------------------------------------------
 *
 * vci_ros_daemon.h
 *	  Definitions and declarations of ROS Control Daemon and Worker
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_ros_daemon.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_ROS_DAEMON_H
#define VCI_ROS_DAEMON_H

#include "postgres.h"

#include "lib/ilist.h"
#include "postmaster/bgworker.h"
#include "utils/relcache.h"

#include "vci_ros.h"

/**
 * The threshold of tid->crid update list item coutns to execute tid->crid update
 */
#define VCI_UPDATE_TIDCRID_THRESHOLD  (1024)

/**
 * The threshold of Whiteout WOS rows to update Delete Vector
 */
#define VCI_UPDATE_DELVEC_THRESHOLD   (256 * 1024)

/**
 * @see src/backend/postmaster/bgworker.c
 */
struct BackgroundWorkerHandle
{
	int			slot;
	uint64		generation;
};

typedef struct vci_workerslot
{
	pid_t		pid;

	BackgroundWorkerHandle handle;

	Oid			dbid;
	Oid			oid;
} vci_workerslot_t;

/* ************************* */
/* daemon functions          */
/* ************************* */

extern void vci_ROS_control_daemon_setup(void);
PGDLLEXPORT void vci_ROS_control_daemon_main(Datum main_arg);

extern PGDLLEXPORT vci_workerslot_t vci_LaunchROSControlWorker(int slot_id);
PGDLLEXPORT void vci_ROS_control_worker_main(Datum main_arg);

extern BackgroundWorkerHandle vci_LaunchROSControlMaintainer(int mode);
extern void vci_ROS_control_maintainer_main(Datum main_arg);

extern void vci_InitDbPriorityList(void);

#endif							/* VCI_ROS_DAEMON_H */
