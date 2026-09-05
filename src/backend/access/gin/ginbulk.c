/*-------------------------------------------------------------------------
 *
 * ginbulk.c
 *	  routines for fast build of inverted index
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginbulk.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/gin_private.h"
#include "common/hashfn.h"
#include "utils/datum.h"
#include "utils/memutils.h"

#define DEF_NENTRY			2048	/* Initial hash table size */
#define DEF_ITEMS_PER_KEY	8		/* Initial ItemPointer array size per key */

typedef struct GinHashKey
{
	OffsetNumber	attnum;
	GinNullCategory	category;
	Datum			key;
} GinHashKey;

typedef struct GinHashEntry
{
	GinHashKey		hashkey;
	uint32			hash;
	char			status;
	ItemPointerData	*items;
	uint32			numItems;
	uint32			allocatedItems;
} GinHashEntry;

typedef struct GinSortEntry
{
	GinHashKey		hashkey;
	ItemPointerData *items;
	uint32			numItems;
} GinSortEntry;

static uint32 gin_hash_key(struct ginbuild_hash *tb, GinHashKey *key);
static bool gin_equal_key(struct ginbuild_hash *tb, GinHashKey *a, GinHashKey *b);

#define SH_PREFIX ginbuild
#define SH_ELEMENT_TYPE GinHashEntry
#define SH_KEY_TYPE GinHashKey
#define SH_KEY hashkey
#define SH_HASH_KEY(tb, key) gin_hash_key(tb, &key)
#define SH_EQUAL(tb, a, b) gin_equal_key(tb, &a, &b)
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) (a)->hash
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

static uint32
gin_hash_key(struct ginbuild_hash *tb, GinHashKey *key)
{
	BuildAccumulator *accum = (BuildAccumulator *) tb->private_data;
	uint32		hash;

	hash = hash_combine(0, murmurhash32((uint32) key->attnum));
	hash = hash_combine(hash, murmurhash32((uint32) key->category));

	if (key->category == GIN_CAT_NORM_KEY)
	{
		CompactAttribute *att;

		att = TupleDescCompactAttr(accum->ginstate->origTupdesc, key->attnum - 1);
		hash = hash_combine(hash, datum_image_hash(key->key, att->attbyval, att->attlen));
	}

	return hash;
}

static bool
gin_equal_key(struct ginbuild_hash *tb, GinHashKey *a, GinHashKey *b)
{
	BuildAccumulator *accum = (BuildAccumulator *) tb->private_data;
	CompactAttribute *att;

	if (a->attnum != b->attnum)
		return false;
	if (a->category != b->category)
		return false;
	if (a->category != GIN_CAT_NORM_KEY)
		return true;

	/*
	 * Compare the actual key values using image equality.
	 * This is correct because we don't want to deduplicate at this point.
	 */
	att = TupleDescCompactAttr(accum->ginstate->origTupdesc, a->attnum - 1);
	return datumIsEqual(a->key, b->key, att->attbyval, att->attlen);
}

#define ST_SORT sort_itempointers
#define ST_ELEMENT_TYPE ItemPointerData
#define ST_COMPARE(a, b) ginCompareItemPointers(a, b)
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

#define ST_SORT sort_keys
#define ST_ELEMENT_TYPE GinSortEntry
#define ST_COMPARE_ARG_TYPE GinState
#define ST_COMPARE(a, b, state) ginCompareAttEntries(state, a->hashkey.attnum, a->hashkey.key, a->hashkey.category, b->hashkey.attnum, b->hashkey.key, b->hashkey.category)
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

void
ginInitBA(BuildAccumulator *accum)
{
	/* accum->ginstate is intentionally not set here */
	accum->hash = ginbuild_create(CurrentMemoryContext, DEF_NENTRY, accum);
	accum->allocatedMemory = accum->hash->size * sizeof(GinHashEntry);
	accum->sorted_entries = NULL;
	accum->num_entries = 0;
	accum->current_pos = 0;
}

/*
 * This is basically the same as datumCopy(), but extended to count
 * palloc'd space in accum->allocatedMemory.
 */
static Datum
getDatumCopy(BuildAccumulator *accum, OffsetNumber attnum, Datum value)
{
	CompactAttribute *att;
	Datum		res;

	att = TupleDescCompactAttr(accum->ginstate->origTupdesc, attnum - 1);
	if (att->attbyval)
		res = value;
	else
	{
		res = datumCopy(value, false, att->attlen);
		accum->allocatedMemory += GetMemoryChunkSpace(DatumGetPointer(res));
	}
	return res;
}

/*
 * Insert one entry into the hash map.
 * If the key already exists, append to its ItemPointer array.
 * Otherwise, create a new hash entry with a new ItemPointer array.
 */
static void
ginInsertBAEntry(BuildAccumulator *accum,
				 ItemPointer heapptr, OffsetNumber attnum,
				 Datum key, GinNullCategory category)
{
	GinHashKey	hashkey;
	GinHashEntry *entry;
	bool		found;
	uint64		oldsize;

	hashkey.attnum = attnum;
	hashkey.category = category;
	if (category == GIN_CAT_NORM_KEY)
		hashkey.key = getDatumCopy(accum, attnum, key);
	else
		hashkey.key = key;

	oldsize = accum->hash->size;
	entry = ginbuild_insert(accum->hash, hashkey, &found);

	if (!found)
	{
		entry->items = palloc_array(ItemPointerData, DEF_ITEMS_PER_KEY);
		entry->numItems = 0;
		entry->allocatedItems = DEF_ITEMS_PER_KEY;
		accum->allocatedMemory += (accum->hash->size - oldsize) * sizeof(GinHashEntry);
		accum->allocatedMemory += GetMemoryChunkSpace(entry->items);
	}

	if (entry->numItems >= entry->allocatedItems)
	{
		uint32		new_allocated;

		if (entry->allocatedItems > UINT32_MAX / 2)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many GIN item pointers for a single key"),
					 errhint("Reduce \"maintenance_work_mem\".")));

		accum->allocatedMemory -= GetMemoryChunkSpace(entry->items);
		new_allocated = entry->allocatedItems * 2;
		entry->items = repalloc_huge(entry->items, mul_size(sizeof(ItemPointerData), new_allocated));
		entry->allocatedItems = new_allocated;
		accum->allocatedMemory += GetMemoryChunkSpace(entry->items);
	}

	entry->items[entry->numItems++] = *heapptr;
}

void
ginInsertBAEntries(BuildAccumulator *accum,
				   ItemPointer heapptr, OffsetNumber attnum,
				   Datum *entries, GinNullCategory *categories,
				   int32 nentries)
{
	if (nentries <= 0)
		return;

	Assert(ItemPointerIsValid(heapptr) && attnum >= FirstOffsetNumber);

	for (int i = 0; i < nentries; i++)
		ginInsertBAEntry(accum, heapptr, attnum, entries[i], categories[i]);
}

/* Prepare to read out the hash table contents using ginGetBAEntry */
void
ginBeginBAScan(BuildAccumulator *accum)
{
	ginbuild_iterator iter;
	GinHashEntry *entry;
	uint32		i = 0;

	accum->num_entries = accum->hash->members;
	accum->current_pos = 0;

	if (accum->num_entries == 0)
		return;

	accum->sorted_entries = palloc_array(GinSortEntry, accum->num_entries);
	ginbuild_start_iterate(accum->hash, &iter);

	while ((entry = ginbuild_iterate(accum->hash, &iter)) != NULL)
	{
		GinSortEntry *se = &accum->sorted_entries[i];
		sort_itempointers(entry->items, entry->numItems);

		se->hashkey = entry->hashkey;
		se->items = entry->items;
		se->numItems = entry->numItems;
		i++;
	}

	Assert(i == accum->num_entries);
	sort_keys(accum->sorted_entries, accum->num_entries, accum->ginstate);
	accum->current_pos = 0;
}

/*
 * Get the next entry in sequence from the BuildAccumulator's sorted hash entries.
 * This consists of a single key datum and a list (array) of one or more
 * heap TIDs in which that key is found.  The list is guaranteed sorted.
 */
ItemPointerData *
ginGetBAEntry(BuildAccumulator *accum,
			  OffsetNumber *attnum, Datum *key, GinNullCategory *category,
			  uint32 *n)
{
	GinSortEntry *entry;

	if (accum->current_pos >= accum->num_entries)
		return NULL;			/* no more entries */

	entry = &accum->sorted_entries[accum->current_pos];
	accum->current_pos++;

	*attnum = entry->hashkey.attnum;
	*key = entry->hashkey.key;
	*category = entry->hashkey.category;
	*n = entry->numItems;

	return entry->items;
}
