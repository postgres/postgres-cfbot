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


/* Hash table control struct is an opaque type known only within dynahash.c */
typedef struct HTAB HTAB;

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
 * The hash options that work for both backend-local and shared memory hash
 * tables.  These are in a macro so that ShmemHashOpts can include the same
 * fields, without the options that cannot work in shared memory.
 *
 * hash				custom hash function (NULL for default)
 * match			custom comparison function (NULL for default)
 * keycopy			custom key copy function (NULL for default)
 * num_partitions	partition count (0 for none)
 * force_blobs		if true, use HASH_BLOBS even for string types
 *
 * HASH_COMMON_OPTS_COPY(src) expands to the designated initializers that copy
 * these fields from *src.  It must be kept in sync with the field list.
 */
#define HASH_COMMON_OPTS_FIELDS \
	HashValueFunc hash; \
	HashCompareFunc match; \
	HashCopyFunc keycopy; \
	int64		num_partitions; \
	bool		force_blobs

#define HASH_COMMON_OPTS_COPY(src) \
	.hash = (src)->hash, \
	.match = (src)->match, \
	.keycopy = (src)->keycopy, \
	.num_partitions = (src)->num_partitions, \
	.force_blobs = (src)->force_blobs

/*
 * Hash options for the hash_make() macros.
 *
 * All fields are optional: zero-initialized fields use the appropriate
 * default behavior.
 */
typedef struct HASHOPTS
{
	HASH_COMMON_OPTS_FIELDS;
	HashAllocFunc alloc;		/* custom allocator (NULL for default) */
	MemoryContext mcxt;			/* memory context (NULL for
								 * CurrentMemoryContext) */
	bool		fixed_size;		/* if true, hash table cannot grow */
} HASHOPTS;

/*
 * Helpers to detect if a type should be hashed as a string.
 *
 * String types include: char arrays and NameData.
 * Everything else is treated as a binary blob (HASH_BLOBS).
 *
 * HASH_PTR_AS_STRING(ptr, size) checks whether the key pointer points to a
 * char array of the given size or to a NameData.  The C version uses _Generic,
 * which applies lvalue conversion (so a "char[size]" member decays to
 * "char (*)[size]" here), and the C++ version reproduces that behavior with
 * std::is_same over std::decay.
 *
 * HASH_TYPE_AS_STRING(type) does the same for a bare type by forming a null
 * pointer to it.  In C that is "(type *) NULL", but that spelling fails in C++
 * for types containing brackets (e.g. char[64]), so C++ uses std::add_pointer_t
 * instead.
 */
#ifdef __cplusplus
#define HASH_PTR_AS_STRING(ptr, size) \
	(std::is_same<typename std::decay<decltype(ptr)>::type, char (*)[size]>::value || \
	 std::is_same<typename std::decay<decltype(ptr)>::type, NameData *>::value)
#define HASH_TYPE_AS_STRING(type) \
	HASH_PTR_AS_STRING(static_cast<std::add_pointer_t<type>>(nullptr), sizeof(type))
#else
#define HASH_PTR_AS_STRING(ptr, size) \
	_Generic((ptr), char (*)[size]: 1, NameData *: 1, default: 0)
#define HASH_TYPE_AS_STRING(type) \
	HASH_PTR_AS_STRING((type *) NULL, sizeof(type))
#endif
#define HASH_KEY_AS_STRING(entrytype, keymember) \
	HASH_PTR_AS_STRING(&((entrytype *)0)->keymember, \
					   sizeof(((entrytype *)0)->keymember))

/*
 * Spelling of a HASHOPTS compound literal for the hash_make macros.
 *
 * C spells this as the compound literal (HASHOPTS){...}.  GCC and Clang accept
 * that spelling in C++ too, but MSVC's C++ compiler rejects it (error C4576),
 * so in C++ we use aggregate brace-initialization instead, which relies on
 * C++20 designated initializers for the optional fields.
 */
#ifdef __cplusplus
#define HASH_MAKE_OPTS(...) HASHOPTS{__VA_ARGS__}
#else
#define HASH_MAKE_OPTS(...) (HASHOPTS){__VA_ARGS__}
#endif

/*
 * Create a hash table with minimal boilerplate.
 *
 * This is the simplest way to create a hash table. It:
 * - Derives keysize from the keymember's actual type
 * - Derives entrysize from the entrytype
 * - Automatically chooses HASH_STRINGS or HASH_BLOBS based on key type
 *   (char arrays and NameData are treated as strings)
 * - Uses CurrentMemoryContext by default
 * - Validates that keymember is at offset 0
 *
 * Optional behavior is specified via designated initializers in the
 * trailing varargs, which initialize a HASHOPTS struct.  See HASHOPTS for
 * available fields.
 *
 * NOTE: If you use char[N] to store binary data that might contain null bytes
 * and/or is not null terminated, the automatic detection will incorrectly
 * treat it as a string and use string comparison.  In such cases, pass
 * .force_blobs = true to override the automatic detection.
 *
 * Usage:
 *   typedef struct { Oid oid; char *data; } MyEntry;
 *   HTAB *h = hash_make(MyEntry, oid, "my table", 64);
 *
 *   HTAB *h = hash_make(MyEntry, oid, "my table", 64,
 *                       .mcxt = TopMemoryContext,
 *                       .num_partitions = 16);
 *
 *   HTAB *h = hash_make(MyEntry, key, "my table", 64,
 *                       .hash = my_hash_func,
 *                       .match = my_match_func);
 */
#define hash_make(entrytype, keymember, tabname, nelem, ...) \
	(StaticAssertExpr(offsetof(entrytype, keymember) == 0, \
					  #keymember " must be first member in " #entrytype), \
	 hash_make_impl((tabname), (nelem), \
					sizeof(((entrytype *)0)->keymember), \
					sizeof(entrytype), \
					HASH_KEY_AS_STRING(entrytype, keymember), \
					HASH_MAKE_OPTS(__VA_ARGS__)))

/*
 * Create a hash set where the entire entry is the key.
 *
 * Like hash_make, but the key is the entire entry.  Same designated
 * initializer syntax in the varargs for optional HASHOPTS fields.
 */
#define hashset_make(entrytype, tabname, nelem, ...) \
	hash_make_impl((tabname), (nelem), sizeof(entrytype), sizeof(entrytype), \
				   HASH_TYPE_AS_STRING(entrytype), \
				   HASH_MAKE_OPTS(__VA_ARGS__))

/*
 * Implementation function for hash_make/hashset_make macros.  Not meant to
 * be called directly.
 *
 * If string_key is true, the key is treated as a null-terminated string.
 */
extern HTAB *hash_make_impl(const char *tabname, int64 nelem,
							Size keysize, Size entrysize,
							bool string_key,
							HASHOPTS opts);

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

/*
 * Parameter data structure for hash_create (which is the low-level method of
 * initializing hash tables, hash_make macros are preferred)
 * Only those fields indicated by hash_flags need be set
 */
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
extern void hash_opts_init(HASHCTL *ctl, int *flags,
						   Size keysize, Size entrysize, bool string_key,
						   const HASHOPTS *opts);
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
