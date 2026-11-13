/*-------------------------------------------------------------------------
 *
 * vci_memory_entry.h
 *	  Definitions and declarations of on-memory structures per VCI index
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_memory_entry.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VCI_MEMORY_ENTRY_H
#define VCI_MEMORY_ENTRY_H

#include "lib/ilist.h"
#include "storage/lwlock.h"

#include "vci_ros.h"

/**
 * the key when searching a vci_memory_entry_t type value from its set.
 */
typedef struct
{
	Oid			oid;			/* Oid of VCI main relation */
	Oid			dbid;			/* Oid of database where VCI main relations
								 * belongs */
} vci_id_t;

/**
 * VCI index placeholder to determine the target of ROS command by ROS daemon
 */
typedef struct
{
	vci_id_t	id;				/* identifier of vci_memory_entry_t */
	Oid			tsid;			/* Oid of tablespace where VCI a main relation
								 * belongs */

	/**
	 * If tsid is equal to InvalidOid, the Oid corresponding to default table
	 * space. Otherwise, this is equal to tsid.
	 */
	Oid			real_tsid;

	/**
	 * Timestamp used for least recent update.
	 * We do nothing for the wraparound effect, aka "wraparound failures" in
	 * the PostgreSQL manual.
	 */
	int32		time_stamp;

	/**
	 * flag to force the ROS control daemon to do WOS->ROS conversion
	 * at next WOS->ROS conversion stage regardless of the WOS size.
	 *
	 * This flag is set to true when a local WOS->ROS conversion fails
	 * on account of out-of-memory error. This flag is set to false when
	 * WOS->ROS conversion is done.
	 */
	bool		force_next_wosros_conv;

	dlist_node	link;			/* links of vci indexes on a same device */

} vci_memory_entry_t;

/**
 * @brief Contains the pointer to the array of vci_memory_entry_t,
 * and a lock.
 *
 * The lock must be used when the array is exclusively accessed, say
 * add / remove entries to / from the array, or so.
 *
 * The instance of vci_memory_entries_t and the array of entries must
 * be allocated in shared memory living throughout the PostgreSQL instance.
 */
typedef struct
{
	/**
	 * Lock to update member variables of vci_memory_entries_t.
	 */
	LWLock	   *lock;

	/**
	 * Number of allocated vci_memory_entry_t pointed by data[].
	 */
	uint32		capacity_hash_entries;

	/**
	 * Current time stamp value, used to least-recently-updated method.
	 * Instances of vci_memory_entry_t have the timestamp of last access,
	 * which we do not care wraparound effect, aka "wraparound failures" in
	 * the PostgreSQL manual.
	 */
	int32		time_stamp;

	/**
	 * Pointer to the array of vci_memory_entry_t.
	 */
	vci_memory_entry_t data[1]; /* VARIABLE LENGTH ARRAY */

} vci_memory_entries_t;

extern Size vci_GetSizeOfMemoryEntries(void);
extern void vci_InitMemoryEntries(void);

extern void vci_TouchMemoryEntry(vci_id_t *vciid, Oid tsid);
extern bool vci_GetWosRosConvertingVCI(vci_wosros_conv_worker_arg_t *vci_info);
extern void vci_freeMemoryEntry(vci_id_t *vciid);

extern void vci_update_memoryentry_in_devloadinfo(void);
extern void vci_MoveTranslatedVCI2Tail(void);
extern void vci_ResetDevloadCurrentPos(void);
extern void vci_RemoveMemoryEntryOnDroppedDatabase(void);
extern void vci_SetForceNextWosRosConvFlag(vci_id_t *vciid, bool value);

#endif							/* VCI_MEMORY_ENTRY_H */
