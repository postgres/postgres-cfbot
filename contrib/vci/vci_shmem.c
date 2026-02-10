/*-------------------------------------------------------------------------
 *
 * vci_shmem.c
 *	  Managing shared memory
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/vci_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/ipc.h"

#include "vci.h"
#include "vci_mem.h"
#include "vci_ros_daemon.h"

/*
 * Pointer to fixed-position shared memory area
 */
VciShmemStruct *VciShmemAddr;

/* Saved hook value */
static shmem_startup_hook_type shmem_startup_prev = NULL;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static void vci_shmem_request(void);

/*
 * Setup shmem_startup_hook
 */
void
vci_setup_shmem(void)
{
	Assert(VciGuc.have_loaded_postgresql_conf);

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = vci_shmem_request;

	shmem_startup_prev = shmem_startup_hook;
	shmem_startup_hook = vci_shmem_startup_routine;
}

/*
 * Request additional shared resources
 */
static void
vci_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(sizeof(VciShmemStruct));
	RequestAddinShmemSpace(vci_GetSizeOfMemoryEntries());

	/*
	 *
	 */

	/*
	 * The + 1 is for wos->ros conversion of vci assigned to unmonitored
	 * devices. vci_devload_t is allocated per device. It monitors and stores
	 * the IO load, holds a set of vci on the device, and is used to determine
	 * which vci to perform wos->ros conversion. Since only a fixed number of
	 * vci_devload_t type values are prepared, if more devices are added and
	 * exceed the number, they fall outside the management scope for wos->ros
	 * conversion. To ensure that no vci is left without wos->ros conversion,
	 * a vci_devload_t type value is prepared to store the set of vci on
	 * devices outside the management scope, and it is handled like other
	 * vci_devload_t values to convert vci. This is the area allocated by + 1.
	 * In order to be treated in the program similarly to the device being
	 * monitored, the value used to determine whether to convert vci on that
	 * device should be set appropriately as a device for conversion. Such
	 * devices are processed as if they are collectively one device, so the
	 * conversion frequency becomes lower.
	 */
	RequestAddinShmemSpace(sizeof(vci_devload_t) * (VciGuc.max_devices + 1));
}

/**
 * initialize devload info
 */
static void
vci_Initialize_devload_info(void)
{
	vci_devload_t *dl_not_monitored;
	vci_memory_entry_list_t *list;

	LWLockAcquire(VciShmemAddr->io_load_lock, LW_EXCLUSIVE);

	dlist_init(&(VciShmemAddr->free_memory_entry_queue_list));

	/* OSS has no loop for monitored devices: just init for [0] */
	Assert(VciShmemAddr->max_devices == 0);
	list = &(VciShmemAddr->memory_entry_queue_array[0]);
	dlist_init(&(list->head));
	dlist_push_tail(&(VciShmemAddr->free_memory_entry_queue_list), &(list->link));

	/* Setup for unmonitored device */
	dl_not_monitored = &(VciShmemAddr->devload_array[0]);
	strcpy(dl_not_monitored->devname, VCI_PSEUDO_UNMONITORED_DEVICE);
	list = dlist_container(vci_memory_entry_list_t, link, dlist_pop_head_node(&(VciShmemAddr->free_memory_entry_queue_list)));
	dl_not_monitored->memory_entry_queue = list;

	/* OSS has just 1 devload_info */
	VciShmemAddr->num_devload_info = 1;

	LWLockRelease(VciShmemAddr->io_load_lock);
}

/*
 * Initialize shared memory
 */
void
vci_shmem_startup_routine(void)
{
	bool		found;

	VciGuc.max_devices = 0;

	if (IsPostmasterEnvironment)
	{
		if (shmem_startup_prev)
			shmem_startup_prev();

		VciShmemAddr =
			(VciShmemStruct *) ShmemInitStruct("vci: shared memory", sizeof(VciShmemStruct), &found);
		Assert(VciShmemAddr != NULL);

#ifdef WIN32
		if (IsUnderPostmaster)
		{
			/** Later process is only necessary in Postmaster,
			  * so child process processing ends here
			  */
			return;
		}
#endif

		/*
		 * Prepare the same number of vci_id_t as the number of worker This
		 * area is used to pass parameters from the ros daemon to the worker
		 * that actually does the conversion Note: The minimum value of
		 * control_max_workers is set to 1, so the allocation size would not
		 * be 0
		 */
		VciShmemAddr->worker_args_array =
			ShmemInitStruct("vci: arguments for workers ", sizeof(vci_wosros_conv_worker_arg_t) * VciGuc.control_max_workers, &found);
		Assert(VciShmemAddr->worker_args_array != NULL);

		VciShmemAddr->memory_entries =
			ShmemInitStruct("vci: memory entries", vci_GetSizeOfMemoryEntries(), &found);
		Assert(VciShmemAddr->memory_entries != NULL);

		/*
		 * + 1 for non-monitored devices: ramfs and the ones that cannot be
		 * observed because of space limitation
		 */
		VciShmemAddr->devload_array =
			ShmemInitStruct("vci: io load watch", sizeof(vci_devload_t) * (VciGuc.max_devices + 1), &found);
		Assert(VciShmemAddr->devload_array != NULL);
		VciShmemAddr->memory_entry_queue_array =
			ShmemInitStruct("vci: memory entry queue", sizeof(vci_memory_entry_list_t) * (VciGuc.max_devices + 1), &found);
		Assert(VciShmemAddr->memory_entry_queue_array != NULL);
	}
	else
	{
		VciShmemAddr = malloc(sizeof(VciShmemStruct));
		MemSet(VciShmemAddr, 0, sizeof(VciShmemStruct));
		VciShmemAddr->worker_args_array = malloc(sizeof(vci_wosros_conv_worker_arg_t) * VciGuc.control_max_workers);
		MemSet(VciShmemAddr->worker_args_array, 0, sizeof(vci_wosros_conv_worker_arg_t) * VciGuc.control_max_workers);
		VciShmemAddr->memory_entries = malloc(vci_GetSizeOfMemoryEntries());
		MemSet(VciShmemAddr->memory_entries, 0, vci_GetSizeOfMemoryEntries());
		VciShmemAddr->devload_array = malloc(sizeof(vci_devload_t) * (VciGuc.max_devices + 1));
		MemSet(VciShmemAddr->devload_array, 0, sizeof(vci_devload_t) * (VciGuc.max_devices + 1));
		VciShmemAddr->memory_entry_queue_array = malloc(sizeof(vci_memory_entry_list_t) * (VciGuc.max_devices + 1));
		MemSet(VciShmemAddr->memory_entry_queue_array, 0, sizeof(vci_memory_entry_list_t) * (VciGuc.max_devices + 1));
	}

	/*
	 * Standby server execution control
	 */
	VciShmemAddr->standby_exec_loc = &(GetNamedLWLockTranche("VciStandbyExec"))->lock;

	/*
	 * Set the number of monitorable devices and initialize lock for IO load
	 * monitoring
	 */
	VciShmemAddr->max_devices = VciGuc.max_devices;
	VciShmemAddr->io_load_lock = &(GetNamedLWLockTranche("VciIOLoad"))->lock;

	/* Additional LWLocks Initialization */
	VciShmemAddr->vci_memory_entries_lock = &(GetNamedLWLockTranche("VciMemoryEntries"))->lock;
	VciShmemAddr->vci_query_context_lock = &(GetNamedLWLockTranche("VciQueryContext"))->lock;
	VciShmemAddr->vci_mnt_point2dev_lock = &(GetNamedLWLockTranche("VciMntpoint2dev"))->lock;

	/* initialize the lists of vci_devload_t */
	vci_Initialize_devload_info();

	/* Initialize vci-memory-entries */
	vci_InitMemoryEntries();
}
