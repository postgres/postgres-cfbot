/*-------------------------------------------------------------------------
 *
 * hsearch.h
 *	  exported definitions for utils/hash/dynahash.c; see notes therein
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/hsearch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HSEARCH_H
#define HSEARCH_H


/*
 * Hash functions must have this signature.
 */
typedef uint32 (*HashValueFunc) (const void *key, Size keysize);

/*
 * Key comparison functions must have this signature.  Comparison functions
 * return zero for match, nonzero for no match.  (The comparison function
 * definition is designed to allow memcmp() and strncmp() to be used directly
 * as key comparison functions.)
 */
typedef int (*HashCompareFunc) (const void *key1, const void *key2,
								Size keysize);

/*
 * Key copying functions must have this signature.  The return value is not
 * used.  (The definition is set up to allow memcpy() and strlcpy() to be
 * used directly.)
 */
typedef void *(*HashCopyFunc) (void *dest, const void *src, Size keysize);

/*
 * Space allocation function for a hashtable.  Note: there is no free function
 * API; can't destroy a hashtable unless you use the default allocator.
 */
typedef void *(*HashAllocFunc) (Size request, void *alloc_arg);

/*
 * HASHELEMENT is the private part of a hashtable entry.  The caller's data
 * follows the HASHELEMENT structure (on a MAXALIGN'd boundary).  The hash key
 * is expected to be at the start of the caller's hash entry data structure.
 */
typedef struct HASHELEMENT
{
	struct HASHELEMENT *link;	/* link to next entry in same bucket */
	uint32		hashvalue;		/* hash function result for this entry */
} HASHELEMENT;

/* Hash table header struct is an opaque type known only within dynahash.c */
typedef struct HASHHDR HASHHDR;

/* Hash table control struct is an opaque type known only within dynahash.c */
typedef struct HTAB HTAB;

/* Parameter data structure for hash_create */
/* Only those fields indicated by hash_flags need be set */
typedef struct HASHCTL
{
	/* Used if HASH_PARTITION flag is set: */
	int64		num_partitions; /* # partitions (must be power of 2) */
	/* Used if HASH_ELEM flag is set (which is now required): */
	Size		keysize;		/* hash key length in bytes */
	Size		entrysize;		/* total user element size in bytes */
	/* Used if HASH_FUNCTION flag is set: */
	HashValueFunc hash;			/* hash function */
	/* Used if HASH_COMPARE flag is set: */
	HashCompareFunc match;		/* key comparison function */
	/* Used if HASH_KEYCOPY flag is set: */
	HashCopyFunc keycopy;		/* key copying function */
	/* Used if HASH_ALLOC flag is set: */
	HashAllocFunc alloc;		/* memory allocator */
	void	   *alloc_arg;		/* opaque argument passed to allocator */
	/* Used if HASH_CONTEXT flag is set: */
	MemoryContext hcxt;			/* memory context to use for allocations */
	/* Used if HASH_ATTACH flag is set: */
	HASHHDR    *hctl;			/* location of header in shared mem */
} HASHCTL;

/* Flag bits for hash_create; most indicate which parameters are supplied */
#define HASH_PARTITION	0x0001	/* Hashtable is used w/partitioned locking */
/* 0x0002 is unused */
/* 0x0004 is unused */
#define HASH_ELEM		0x0008	/* Set keysize and entrysize (now required!) */
#define HASH_STRINGS	0x0010	/* Select support functions for string keys */
#define HASH_BLOBS		0x0020	/* Select support functions for binary keys */
#define HASH_FUNCTION	0x0040	/* Set user defined hash function */
#define HASH_COMPARE	0x0080	/* Set user defined comparison function */
#define HASH_KEYCOPY	0x0100	/* Set user defined key-copying function */
#define HASH_ALLOC		0x0200	/* Set memory allocator */
#define HASH_CONTEXT	0x0400	/* Set memory allocation context */
#define HASH_SHARED_MEM 0x0800	/* Hashtable is in shared memory */
#define HASH_ATTACH		0x1000	/* Do not initialize hctl */
#define HASH_FIXED_SIZE 0x2000	/* Initial size is a hard limit */

/* max_dsize value to indicate expansible directory */
#define NO_MAX_DSIZE			(-1)

/* hash_search operations */
typedef enum
{
	HASH_FIND,
	HASH_ENTER,
	HASH_REMOVE,
	HASH_ENTER_NULL,
} HASHACTION;

/* hash_seq status (should be considered an opaque type by callers) */
typedef struct
{
	HTAB	   *hashp;
	uint32		curBucket;		/* index of current bucket */
	HASHELEMENT *curEntry;		/* current entry in bucket */
	bool		hasHashvalue;	/* true if hashvalue was provided */
	uint32		hashvalue;		/* hashvalue to start seqscan over hash */
} HASH_SEQ_STATUS;

extern void hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp);

/*
 * Same as hash_seq_init(), but returns the status struct instead of taking a
 * pointer. This way we can use it in the initialization clause of a for loop,
 * which we need in the foreach_hash macro.
 *
 * Not intended to be called directly by user code.
 */
static inline HASH_SEQ_STATUS
foreach_hash_start(HTAB *hashp)
{
	HASH_SEQ_STATUS status;

	hash_seq_init(&status, hashp);
	return status;
}


/*
 * foreach_hash - iterate over all entries in a hash table
 *
 * This macro simplifies hash table iteration by combining hash_seq_init
 * and hash_seq_search into a single for-loop construct.
 *
 * Usage:
 *   foreach_hash(MyEntry, entry, my_hashtable)
 *   {
 *       // use entry
 *   }
 *
 * For early termination, use foreach_hash_term() before break:
 *   foreach_hash(MyEntry, entry, my_hashtable)
 *   {
 *       if (found_it)
 *       {
 *           foreach_hash_term(entry);
 *           break;
 *       }
 *   }
 *
 * This macro actually generates two loops in order to declare two variables of
 * different types.  The outer loop only iterates once, so we expect optimizing
 * compilers will unroll it, thereby optimizing it away. (This is the same
 * trick that's used in the foreach_internal macro in pg_list.h)
 */
#define foreach_hash(type, var, htab) \
	for (type *var = NULL, *var##__outerloop = (type *) 1; \
		 var##__outerloop; \
		 var##__outerloop = 0) \
		for (HASH_SEQ_STATUS var##__status = foreach_hash_start(htab); \
			 (var = (type *) hash_seq_search(&var##__status)) != NULL; )

/*
 * foreach_hash_term - terminate a foreach_hash loop early
 *
 * Always call this before breaking out of a foreach_hash loop, whether done by
 * using "break", "return" or "goto". (Not needed when using "continue")
 */
#define foreach_hash_term(var) hash_seq_term(&var##__status)

/*
 * foreach_hash_restart - restart iteration from the beginning
 *
 * Use when modifications during iteration may have invalidated the scan.
 * The next iteration will start from the first entry again.
 */
#define foreach_hash_restart(var, htab) \
	(hash_seq_term(&var##__status), \
	 hash_seq_init(&var##__status, htab))

/*
 * prototypes for functions in dynahash.c
 */
extern HTAB *hash_create(const char *tabname, int64 nelem,
						 const HASHCTL *info, int flags);
extern void hash_destroy(HTAB *hashp);
extern void hash_stats(const char *caller, HTAB *hashp);
extern void *hash_search(HTAB *hashp, const void *keyPtr, HASHACTION action,
						 bool *foundPtr);
extern uint32 get_hash_value(HTAB *hashp, const void *keyPtr);
extern void *hash_search_with_hash_value(HTAB *hashp, const void *keyPtr,
										 uint32 hashvalue, HASHACTION action,
										 bool *foundPtr);
extern bool hash_update_hash_key(HTAB *hashp, void *existingEntry,
								 const void *newKeyPtr);
extern int64 hash_get_num_entries(HTAB *hashp);
extern void hash_seq_init_with_hash_value(HASH_SEQ_STATUS *status,
										  HTAB *hashp,
										  uint32 hashvalue);
extern void *hash_seq_search(HASH_SEQ_STATUS *status);
extern void hash_seq_term(HASH_SEQ_STATUS *status);
extern Size hash_estimate_size(int64 num_entries, Size entrysize);
extern void AtEOXact_HashTables(bool isCommit);
extern void AtEOSubXact_HashTables(bool isCommit, int nestDepth);

#endif							/* HSEARCH_H */
