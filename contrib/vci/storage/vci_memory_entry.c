/*-------------------------------------------------------------------------
 *
 * vci_memory_entry.c
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_memory_entry.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "c.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/reloptions.h"
#include "catalog/indexing.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"

#include "vci.h"

#include "vci_mem.h"
#include "vci_memory_entry.h"

#define VCI_LOCKTAG_MEMORY_ENTRY  LOCKTAG_USERLOCK

static void debug_show_vciid_ts(const char *head, Oid oid, Oid tsid);
static char *getTablespacePath(Oid tsid);
static HeapTuple GetDatabaseTupleByOid(Oid dboid);
static void initializeMemoryEntryCommon(vci_memory_entry_t *entry, vci_id_t *vciid, Oid tsid);
static void initializeMemoryEntry(vci_memory_entry_t *entry, vci_id_t *vciid, Oid tsid, int32 timeStamp);
static void resetMemoryEntry(vci_memory_entry_t *entry, vci_id_t *vciid, Oid tsid, int32 timeStamp);
static int	findMemoryEntryLocation(vci_id_t *vciid);
static void stampOnMemoryEntry(vci_memory_entry_t *entry, vci_memory_entries_t *entries);
static int	determineRoomPosition(Oid oid);
static void setLockTagPointer(LOCKTAG *locktag, void *ptr);
static LockAcquireResult lockAcquirePointer(vci_memory_entry_t *entry, LOCKMODE lockmode, bool dontWait);
static bool lockReleasePointer(vci_memory_entry_t *entry, LOCKMODE lockmode);
static int	makeRoomAndGetLocation(vci_id_t *vciid, Oid tsid);
static void registerMemoryEntry2Device(vci_memory_entry_t *entry);

/**
 * output debug log
 * @param[in] head the string in the head of a log
 * @param[in] oid vci oid
 * @param[in] tsid tablespace oid
 * @param[in] path path of tablespace
 */
static void
debug_show_vciid_ts(const char *head, Oid oid, Oid tsid)
{
	bool		free_flag = true;
	char	   *path = getTablespacePath(tsid);

	if (path == NULL)
	{
		path = DataDir;
		free_flag = false;
	}
	elog(DEBUG2, "%s entry oid=%u tsid=%u (%s)", head, oid, tsid, path);

	if (free_flag)
		pfree(path);
}

#define VCI_HASH_WIDTH 65536

Size
vci_GetSizeOfMemoryEntries(void)
{
	Size		result;
	uint32		capacity;

	/* LCOV_EXCL_START */
	capacity = Max(VCI_HASH_WIDTH / NUM_BUFFER_PARTITIONS, 1);
	/* LCOV_EXCL_STOP */

	result = offsetof(vci_memory_entries_t, data) +
		sizeof(vci_memory_entry_t) * capacity;

	return result;
}

/**
 * @brief Initialize the area for VCI memory objects residing with PostgreSQL
 * instance, and the hash table area.
 */
void
vci_InitMemoryEntries(void)
{
	uint32		capacity;
	vci_id_t	vciid;

	elog(DEBUG2, "vci_InitMemoryEntries");
	dlist_init(&VciShmemAddr->memory_entry_device_unknown_list);

	/* LCOV_EXCL_START */
	capacity = Max(VCI_HASH_WIDTH / NUM_BUFFER_PARTITIONS, 1);
	elog(DEBUG2, ">>> capacity = %d", capacity);
	/* LCOV_EXCL_STOP */

	VciShmemAddr->memory_entries->capacity_hash_entries = capacity;

	VciShmemAddr->memory_entries->lock = VciShmemAddr->vci_memory_entries_lock;

	vciid.oid = InvalidOid;
	vciid.dbid = InvalidOid;

	for (int i = 0; i < capacity; i++)
		initializeMemoryEntryCommon(&VciShmemAddr->memory_entries->data[i], &vciid, InvalidOid);
}

static char *
getTablespacePath(Oid tsid)
{
	char	   *tablespace_path;

	Assert(OidIsValid(tsid));

	if (tsid == DEFAULTTABLESPACE_OID ||
		tsid == GLOBALTABLESPACE_OID)
	{
		tablespace_path = NULL;
	}
	else
	{
		LOCAL_FCINFO(ci, 1);

		ci->nargs = 1;
		ci->args[0].value = ObjectIdGetDatum(tsid);
		ci->args[0].isnull = false;

		tablespace_path = text_to_cstring(DatumGetTextP(pg_tablespace_location(ci)));
	}

	return tablespace_path;
}

/*
 * This function is a static function in src/backend/utils/init/postinit.c in
 * PostgreSQL
 *
 * @param[in] dboid oid of a database
 */
static HeapTuple
GetDatabaseTupleByOid(Oid dboid)
{
	HeapTuple	tuple;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];

	/*
	 * form a scan key
	 */
	ScanKeyInit(&key[0],
				Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dboid));

	/*
	 * Open pg_database and fetch a tuple.  Force heap scan if we haven't yet
	 * built the critical shared relcache entries (i.e., we're starting up
	 * without a shared relcache cache file).
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(relation, DatabaseOidIndexId,
							  criticalSharedRelcachesBuilt,
							  NULL,
							  1, key);

	tuple = systable_getnext(scan);

	/* Must copy tuple before releasing buffer */
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	/* all done */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	return tuple;
}

/**
 * DefaultTablespaceOid for dbid.
 * This function must be called in a transaction
 *
 * @param[in] dboid oid of a database
 *
 * @return Oid of default tablespace
 */
static Oid
getDefaultTablespaceOid(Oid dboid)
{
	HeapTuple	tuple;
	Form_pg_database dbform;

	tuple = GetDatabaseTupleByOid(dboid);
	if (!HeapTupleIsValid(tuple))
	{
		elog(DEBUG2,
			 "database %u does not exist", dboid);
		return InvalidOid;
	}
	dbform = (Form_pg_database) GETSTRUCT(tuple);

	return dbform->dattablespace;
}

/**
 * Initialize memory entry.
 * If a valid vci oid is given, this memory entry is registered to the list in vci_devload_t
 * for WOS->ROS transformation.
 *
 * @param[in] entry memory entry to be initialized
 * @param[in] vciid id of vci
 * @param[in] tsid tablespace oid
 */
static void
initializeMemoryEntryCommon(vci_memory_entry_t *entry, vci_id_t *vciid, Oid tsid)
{
	elog(DEBUG2, "initializeMemoryEntryCommon: vciid->oid: %d, vciid->dbid: %d, tsid: %d)", vciid->oid, vciid->dbid, tsid);

	entry->id.oid = vciid->oid;
	entry->id.dbid = vciid->dbid;
	entry->tsid = tsid;
	if (!OidIsValid(tsid) && OidIsValid(vciid->dbid))
	{
		entry->real_tsid = getDefaultTablespaceOid(vciid->dbid);
		if (!OidIsValid(entry->real_tsid))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database %u does not exist", vciid->dbid)));
	}
	else
		entry->real_tsid = tsid;

	if (OidIsValid(vciid->oid))
	{
		dlist_push_head(&VciShmemAddr->memory_entry_device_unknown_list, &(entry->link));

		entry->force_next_wosros_conv = false;
	}
}

/**
 * Initialize memory entry
 *
 * @param[in] entry memory entry to be initialized
 * @param[in] vciid id of vci (a pair of oid of vci and oid of database)
 * @param[in] tsid oid of tablespace
 * @param[in] timeStamp timestamp
 */
static void
initializeMemoryEntry(vci_memory_entry_t *entry, vci_id_t *vciid, Oid tsid, int32 timeStamp)
{
	elog(DEBUG2, "initializeMemoryEntry: vciid->oid: %d, vciid->dbid: %d, tsid: %d)", vciid->oid, vciid->dbid, tsid);

	if (OidIsValid(entry->id.oid))
	{
		/* clean up previous registration. */
		dlist_delete(&entry->link);
	}

	initializeMemoryEntryCommon(entry, vciid, tsid);

	entry->time_stamp = timeStamp;
}

/**
 * reset memory entry
 *
 * @param[in] entry memory entry to be initialized
 * @param[in] vciid id of vci (a pair of oid of vci and oid of database)
 * @param[in] tsid oid of tablespace
 * @param[in] timeStamp timestamp
 */
static void
resetMemoryEntry(vci_memory_entry_t *entry, vci_id_t *vciid, Oid tsid, int32 timeStamp)
{
	elog(DEBUG2, "reset memory object of OID %d", vciid->oid);

	initializeMemoryEntry(entry, vciid, tsid, timeStamp);
}

#define MAKE_ROOM_MAX_SCAN_SPAN	 (128)
#define MAKE_ROOM_SCAN_SPAN		 (8)
#define MAKE_ROOM_THRESHOLD		 (0x10000000)

/**
 * free memory entry
 *
 * @param[in] vciid id of vci whose memory entry is freed
 */
void
vci_freeMemoryEntry(vci_id_t *vciid)
{
	int			index;
	vci_memory_entries_t *entries = VciShmemAddr->memory_entries;
	vci_id_t	invalid_vciid;

	LWLockAcquire(entries->lock, LW_EXCLUSIVE);

	index = findMemoryEntryLocation(vciid);

	invalid_vciid.oid = InvalidOid;
	invalid_vciid.dbid = InvalidOid;
	if (index != -1)
		initializeMemoryEntry(&entries->data[index], &invalid_vciid, InvalidOid, entries->time_stamp);
	LWLockRelease(entries->lock);

	return;
}

/**
 * @brief This function returns the position in vci_memory_entries_t.data.
 *
 * If some data[ptr].oid has the given oid or InvalidOid, return ptr.
 * In other case, return -1, meaning no room for the OID.
 *
 * @param[in] vciid id of vci, information of which are stored in the found
 * memory entry.
 * @return The index to the found object.
 *
 * @note This function must be called under the lock of the
 * vci_memory_entries_t is acquired exclusively. Since this function does not
 * acquire any lock on the entry, user must lock the entry and check if it has
 * the correct OID.
 */
static int
findMemoryEntryLocation(vci_id_t *vciid)
{
	vci_memory_entries_t *entries = VciShmemAddr->memory_entries;
	int			capacity = entries->capacity_hash_entries;
	int			ptr = vciid->oid % capacity;
	int			ptr_candidate = -1;
	int			aIdMax = Min(capacity, MAKE_ROOM_MAX_SCAN_SPAN);

	for (int aId = 0; aId < aIdMax; ++aId)
	{
		if (entries->data[ptr].id.oid == vciid->oid && entries->data[ptr].id.dbid == vciid->dbid)
		{
			return ptr;
		}
		else if (ptr_candidate == -1 && !OidIsValid(entries->data[ptr].id.oid))
		{
			ptr_candidate = ptr;
		}
		ptr = (ptr == (capacity - 1)) ? 0 : (1 + ptr);
	}

	return ptr_candidate;
}

/**
 * set timestamp on memory entry
 *
 * @param[in] entry memory entry whose timestamp are set
 * @param[in] entries memory entries in which set timestamp exists
 */
static void
stampOnMemoryEntry(vci_memory_entry_t *entry, vci_memory_entries_t *entries)
{
	entry->time_stamp = entries->time_stamp;
}

/**
 * This function is called when no room is available in the hash table.
 * It finds a position to store information for oid.  First, it
 * calculates the hash value (HV) by oid % capacity.  Then, look into the
 * range [HV, HV + MAKE_ROOM_MAX_SCAN_SPAN - 1].  If there are entries, whose
 * timestamps are older by MAKE_ROOM_THRESHOLD from the currrent, the oldest
 * one is selected as the result of this function.  Otherwise, it repeats the
 * sequence in the next range,
 * [HV + MAKE_ROOM_MAX_SCAN_SPAN, HV + 2 * MAKE_ROOM_MAX_SCAN_SPAN - 1], and
 * so on.  If there is no such entry, then it returns HV itself.
 *
 * @param[in] oid Oid of VCI main relation, for which the hash table entry is
 * allocated.
 *
 * @return position ID of hash table entry.
 */
static int
determineRoomPosition(Oid oid)
{
	vci_memory_entries_t *entries = VciShmemAddr->memory_entries;
	int			capacity = entries->capacity_hash_entries;
	int32		currentStamp = entries->time_stamp;
	vci_memory_entry_t *data = entries->data;
	int			ptr = oid % capacity;
	int			maxPtr = ptr;
	uint32		maxDiff = 0;
	int			aIdMax = Min(capacity, MAKE_ROOM_MAX_SCAN_SPAN);

	for (int aId = 0; aId < aIdMax; aId += MAKE_ROOM_SCAN_SPAN)
	{
		int			bIdMax = Min(capacity, aId + MAKE_ROOM_SCAN_SPAN);

		for (int bId = aId; bId < bIdMax; ++bId)
		{
			uint32		newDiff = currentStamp - data[ptr].time_stamp;

			if (maxDiff < newDiff)
			{
				maxDiff = newDiff;
				maxPtr = ptr;
			}
			ptr = (ptr == (capacity - 1)) ? 0 : (1 + ptr);
		}

		if (MAKE_ROOM_THRESHOLD <= maxDiff)
			break;
	}

	elog(DEBUG2,
		 "discard OID %d at position %d with time stamp difference %d"
		 " for OID %d under capacity %d",
		 data[ptr].id.oid, ptr, maxDiff, oid, capacity);

	return maxPtr;
}

static void
setLockTagPointer(LOCKTAG *locktag, void *ptr)
{
	uintptr_t addr = (uintptr_t) ptr;

	locktag->locktag_field1 = (uint32) (addr & 0xFFFFFFFF);
	locktag->locktag_field2 = 0;
#if SIZEOF_VOID_P >= 8
	/* On 64-bit platforms, store high 32 bits */
	locktag->locktag_field3 = (uint32) ((addr >> 32) & 0xFFFFFFFF);
#else
	/* On 32-bit platforms, high bits are always zero */
	locktag->locktag_field3 = 0;
#endif
	locktag->locktag_field4 = 0;
	locktag->locktag_type = VCI_LOCKTAG_MEMORY_ENTRY;
	locktag->locktag_lockmethodid = DEFAULT_LOCKMETHOD;
}

static LockAcquireResult
lockAcquirePointer(vci_memory_entry_t *entry, LOCKMODE lockmode, bool dontWait)
{
	LOCKTAG		locktag;

	Assert(entry);

	setLockTagPointer(&locktag, entry);

	return LockAcquire(&locktag, lockmode, false, dontWait);
}

static bool
lockReleasePointer(vci_memory_entry_t *entry, LOCKMODE lockmode)
{
	LOCKTAG		locktag;

	setLockTagPointer(&locktag, entry);

	return LockRelease(&locktag, lockmode, false);
}

/**
 * @brief This function removes idle entry of vci_memory_entry_t
 * in vci_memory_entries_t, resets the area, and return the index.
 *
 * @param[in] vciid id of vci whose information is stored in the determined
 * memory entry.
 * @param[in] tsid tablespace oid
 * @return The index to the object acquired and resetted.
 *
 * @note This function must be called under the lock of the
 * vci_memory_entries_t is acquired exclusively.
 */
static int
makeRoomAndGetLocation(vci_id_t *vciid, Oid tsid)
{
	vci_memory_entries_t *entries = VciShmemAddr->memory_entries;
	vci_memory_entry_t *entry = NULL;
	int			result = -1;

	do
	{
		result = determineRoomPosition(vciid->oid);
		Assert(0 <= result);
		entry = &(entries->data[result]);

		/* Test if the entry is really free. */
		switch (lockAcquirePointer(entry, AccessExclusiveLock, true /* don't wait */ ))
		{
			case LOCKACQUIRE_OK:
				/* It is free. */
				break;

			case LOCKACQUIRE_NOT_AVAIL:

				/*
				 * The lock should be taken in shared mode. We have to search
				 * another entry. To skip current entry, we stamp it.
				 */
				stampOnMemoryEntry(entry, entries);
				result = -1;
				break;

				/* LCOV_EXCL_START */
			case LOCKACQUIRE_ALREADY_HELD:
				/* NEVER COME HERE */
				ereport(ERROR,
						(errmsg("duplicate lock detected"),
						 errhint("Disable VCI by 'SELECT vci_disable();'")));
				break;

			default:
				/* NEVER COME HERE */
				ereport(ERROR,
						(errmsg("undefined lock state"),
						 errhint("Disable VCI by 'SELECT vci_disable();'")));
				/* LCOV_EXCL_STOP */
		}
	} while (result < 0);

	Assert(NULL != entry);

	resetMemoryEntry(entry, vciid, tsid, entries->time_stamp);

	/* LCOV_EXCL_START */
	if (!lockReleasePointer(entry, AccessExclusiveLock))
	{
		/* NEVER COME HERE */
		ereport(ERROR,
				(errmsg("undefined lock state"),
				 errhint("Disable VCI by 'SELECT vci_disable();'")));
	}
	/* LCOV_EXCL_STOP */

	return result;
}

/**
 * get memory entry which corresponds to vciid
 *
 * @param[in] vciid vci_id_t identifying vci
 * @param[in] tsid oid of tablespace
 *
 * @return memory entry which corresponds to vciid
 */
static vci_memory_entry_t *
vci_GetMemoryEntry(vci_id_t *vciid, Oid tsid)
{
	vci_memory_entries_t *entries = VciShmemAddr->memory_entries;
	vci_memory_entry_t *entry = NULL;
	LockAcquireResult lockResult;
	int			ptr;

	LWLockAcquire(entries->lock, LW_SHARED);

	entries->time_stamp++;

retry:
	ptr = findMemoryEntryLocation(vciid);

	if (ptr == -1)
	{
		LWLockRelease(entries->lock);
		ptr = makeRoomAndGetLocation(vciid, tsid);
		LWLockAcquire(entries->lock, LW_EXCLUSIVE);
	}

	entry = &entries->data[ptr];

	if (!OidIsValid(entry->id.oid))
		initializeMemoryEntry(entry, vciid, tsid, entries->time_stamp);

	LWLockRelease(entries->lock);

	lockResult = lockAcquirePointer(entry, AccessShareLock, false /* wait */ );

	if (lockResult == LOCKACQUIRE_OK)
	{
		if (entry->id.oid == vciid->oid)
			return entry;
		else
			lockReleasePointer(entry, AccessShareLock);
	}

	LWLockAcquire(entries->lock, LW_SHARED);

	goto retry;
}

/**
 * @brief release memory entry
 *
 * @param[out] entry The memory entry to be released
 */
static void
vci_ReleaseMemoryEntry(vci_memory_entry_t *entry)
{
	lockReleasePointer(entry, AccessShareLock);
}

static void
registerMemoryEntry2Device(vci_memory_entry_t *entry)
{
	vci_devload_t *dload;
	const char *devname;
	char	   *tablespace_path;
	bool		free_flag = true;

	elog(DEBUG2, "registerMemoryEntry2Device");

	tablespace_path = getTablespacePath(entry->real_tsid);
	if (tablespace_path == NULL)
	{
		tablespace_path = DataDir;
		free_flag = false;
	}

	/* OSS always uses "unmonitored" device */
	devname = VCI_PSEUDO_UNMONITORED_DEVICE;
	elog(DEBUG2, "vci oid %u tablespace(%s) is on a device (%s)", entry->id.oid, tablespace_path, devname);

	if (free_flag)
		pfree(tablespace_path);

	Assert(VciShmemAddr->num_devload_info == 1);
	dload = &(VciShmemAddr->devload_array[0]);
	Assert(dload != NULL);
	Assert(strcmp(dload->devname, VCI_PSEUDO_UNMONITORED_DEVICE) == 0);

	dlist_push_head(&(dload->memory_entry_queue->head), &(entry->link));
}

/**
 * @return the index to be checked
 *
 * XXX: Consider just removing this function, because for OSS it only returns 0.
 */
static int
get_new_checked_device_index(int index)
{
	/*
	 * For OSS the num_devload_info is hardwired as 1, so this function can
	 * only return an index of 0. ([0] is the "unmonitored" device)
	 */
	Assert(VciShmemAddr->num_devload_info == 1);
	Assert(index == 0);

	return index;
}

/**
 * @param[out] vciid id on which WOS->ROS conversion should be done
 *
 * @return true if VCI for transformation is found, false otherwise.
 */
bool
vci_GetWosRosConvertingVCI(vci_wosros_conv_worker_arg_t *vciinfo)
{
	int			index;
	int			head_index;
	vci_memory_entry_t *entry;
	bool		found = false;
	bool		check_started = false;
	vci_devload_t *dl;

	index = VciShmemAddr->translated_dev_pos;

	head_index = index;

	elog(DEBUG2, ">>> index=%d, head_index=%d, check_started=%d, found=%d", index, head_index, check_started, found);
	dl = &(VciShmemAddr->devload_array[index]);
	while ((index != head_index || !check_started)
		   && !found)
	{
		check_started = true;

		if (dl->memory_entry_pos == NULL)
		{
			elog(LOG, "wos->ros translation: skip translation on device [%s] because no memory entry", dl->devname);
			index = get_new_checked_device_index(index);

			elog(LOG, ">>> vci_GetWosRosConvertingVCI: index=%d, num_devload_info=%d", index, VciShmemAddr->num_devload_info);
			dl = &(VciShmemAddr->devload_array[index]);
		}
		else
			found = true;

		elog(DEBUG2, ">>> index=%d, head_index=%d, check_started=%d, found=%d", index, head_index, check_started, found);
	}

	if (!found)
	{
		elog(LOG, "wos->ros translation: no vci is found for translation");

	}
	else
	{
		dlist_node *ret;
		dlist_head *memory_entry_queue;

		elog(LOG, "dev info: [%s] ", VciShmemAddr->devload_array[index].devname);

		memory_entry_queue = &(dl->memory_entry_queue->head);

		Assert(dl->memory_entry_pos != NULL);

		ret = dl->memory_entry_pos;
		if (dlist_has_next(memory_entry_queue, ret))
			dl->memory_entry_pos = dlist_next_node(memory_entry_queue, ret);
		else
			dl->memory_entry_pos = NULL;

		VciShmemAddr->translated_dev_pos = get_new_checked_device_index(index);

		entry = dlist_container(vci_memory_entry_t, link, ret);

		vciinfo->dbid = entry->id.dbid;
		vciinfo->oid = entry->id.oid;
		vciinfo->force_next_wosros_conv = entry->force_next_wosros_conv;

		elog(LOG, "wos->ros conversion on device (%s): vci oid=%u dbid=%u", dl->devname, vciinfo->oid, vciinfo->dbid);
	}

	return found;
}

/**
 * update a timestamp or newly create a memoryentry for a vci.
 *
 * @param[in] vciid id of a vci index
 * @param[in] tsid Oid of tablespace of vci index identified by the first argument'oid'
 */
void
vci_TouchMemoryEntry(vci_id_t *vciid, Oid tsid)
{
	vci_memory_entry_t *entry;

	entry = vci_GetMemoryEntry(vciid, tsid);
	entry->time_stamp = VciShmemAddr->memory_entries->time_stamp;
	vci_ReleaseMemoryEntry(entry);
}

void
vci_update_memoryentry_in_devloadinfo(void)
{
	elog(DEBUG2, "vci_update_memoryentry_in_devloadinfo: start");

	/* dlist_mutable_iter miter; */

	LWLockAcquire(VciShmemAddr->memory_entries->lock, LW_EXCLUSIVE);
	while (!dlist_is_empty(&VciShmemAddr->memory_entry_device_unknown_list))
	{
		dlist_node *tmp;		/* vci_memory_entry_t */
		vci_memory_entry_t *entry;

		elog(DEBUG2, ">>> vci_update_memoryentry_in_devloadinfo: in loop");
		tmp = dlist_pop_head_node(&VciShmemAddr->memory_entry_device_unknown_list);
		entry = dlist_container(vci_memory_entry_t, link, tmp);
		Assert(OidIsValid(entry->id.dbid));

#if 1
		debug_show_vciid_ts("ros extract one: ", entry->id.oid, entry->real_tsid);
#endif

		registerMemoryEntry2Device(entry);
	}
	LWLockRelease(VciShmemAddr->memory_entries->lock);
}

void
vci_ResetDevloadCurrentPos(void)
{
	vci_devload_t *item;

	elog(DEBUG2, "vci_ResetDevloadCurrentPos: start; VciShmemAddr->num_devload_info is %d", VciShmemAddr->num_devload_info);
	for (int i = 0; i < VciShmemAddr->num_devload_info; i++)
	{
		dlist_head *memory_entry_queue = &(VciShmemAddr->devload_array[i].memory_entry_queue->head);

		item = &(VciShmemAddr->devload_array[i]);

		if (dlist_is_empty(memory_entry_queue))
			item->memory_entry_pos = NULL;
		else
			item->memory_entry_pos = dlist_head_node(memory_entry_queue);

	}
}

void
vci_MoveTranslatedVCI2Tail(void)
{
	for (int i = 0; i < VciShmemAddr->num_devload_info; i++)
	{
		vci_devload_t *dl;

		dl = &(VciShmemAddr->devload_array[i]);

		{
			dlist_head *memory_entry_queue = &(dl->memory_entry_queue->head);

			if (dl->memory_entry_pos != NULL
				&& dl->memory_entry_pos != dlist_head_node(memory_entry_queue))
			{
				while (dlist_head_node(memory_entry_queue) != dl->memory_entry_pos)
				{
					dlist_node *n;

					n = dlist_pop_head_node(memory_entry_queue);
					dlist_push_tail(memory_entry_queue, n);
				}
			}
		}
	}
}

/**
 * Check VCI' database is exists, and
 * remove memory entory on dropped database.
 */
void
vci_RemoveMemoryEntryOnDroppedDatabase(void)
{
	vci_id_t	invalid_vciid;
	int32		time_stamp;

	invalid_vciid.oid = InvalidOid;
	invalid_vciid.dbid = InvalidOid;
	time_stamp = VciShmemAddr->memory_entries->time_stamp;

	/* start transaction */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	for (int i = 0; i < VciShmemAddr->num_devload_info; i++)
	{
		vci_devload_t *devload;
		dlist_head *memory_entry_queue;
		dlist_node *node;
		dlist_node *next_node;

		devload = &(VciShmemAddr->devload_array[i]);
		memory_entry_queue = &(devload->memory_entry_queue->head);

		if (dlist_is_empty(memory_entry_queue))
			node = NULL;
		else
			node = dlist_head_node(memory_entry_queue);

		while (node != NULL)
		{
			vci_memory_entry_t *entry;
			HeapTuple	tuple;

			if (dlist_has_next(memory_entry_queue, node))
				next_node = dlist_next_node(memory_entry_queue, node);
			else
				next_node = NULL;

			entry = dlist_container(vci_memory_entry_t, link, node);
			tuple = GetDatabaseTupleByOid(entry->id.dbid);
			if (!HeapTupleIsValid(tuple))
			{
				elog(DEBUG2,
					 "vci %d was dropped by DROP DATABASE", entry->id.oid);

				initializeMemoryEntry(entry, &invalid_vciid, InvalidOid, time_stamp);
			}

			node = next_node;
		}

		if (dlist_is_empty(memory_entry_queue))
			devload->memory_entry_pos = NULL;
		else
			devload->memory_entry_pos = dlist_head_node(memory_entry_queue);
	}

	/* close transaction */
	PopActiveSnapshot();
	CommitTransactionCommand();
}

/**
 * Set the flag to force WOS->ROS conversion next time.
 * @param[in] vciid id of vci
 * @param[in] value flag
 */
void
vci_SetForceNextWosRosConvFlag(vci_id_t *vciid, bool value)
{
	int			index;
	vci_memory_entries_t *entries = VciShmemAddr->memory_entries;

	LWLockAcquire(entries->lock, LW_EXCLUSIVE);

	index = findMemoryEntryLocation(vciid);
	if (index != -1)
		entries->data[index].force_next_wosros_conv = value;

	LWLockRelease(entries->lock);
}
