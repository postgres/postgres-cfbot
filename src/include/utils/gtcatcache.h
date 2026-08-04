/*-------------------------------------------------------------------------
 *
 * gtcatcache.h
 *	  Global temporary catalog cache.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/utils/gtcatcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GTCATCACHE_H
#define GTCATCACHE_H

#include "access/htup.h"
#include "access/tupdesc.h"

/*
 * Identifier of global temporary catalog cache.
 */
typedef enum GTCatCacheIdentifier
{
	PG_TEMP_CLASS,
	PG_TEMP_INDEX,
} GTCatCacheIdentifier;

#define NUM_GT_CAT_CACHES	((int) PG_TEMP_INDEX + 1)

extern bool GTCatCacheTupleExists(GTCatCacheIdentifier cacheId, Oid relid);
extern HeapTuple GTCatCacheSearch(GTCatCacheIdentifier cacheId, Oid relid);
extern void GTCatCacheTupleInsert(GTCatCacheIdentifier cacheId, Oid relid,
								  char relkind, TupleDesc tupdesc,
								  const Datum *values, const bool *nulls);
extern void GTCatCacheTupleUpdate(GTCatCacheIdentifier cacheId, Oid relid,
								  HeapTuple newtuple);
extern void GTCatCacheTupleUpdateInPlace(GTCatCacheIdentifier cacheId,
										 Oid relid, HeapTuple newtuple);
extern void GTCatCacheTupleDelete(GTCatCacheIdentifier cacheId, Oid relid);
extern void GTCatCacheGetMinFrozenXids(TransactionId *min_relfrozenxid,
									   MultiXactId *min_relminmxid);
extern void GTCatCacheFlush(void);
extern void AtEOXact_GTCatCache(bool isCommit);
extern void AtEOSubXact_GTCatCache(bool isCommit, SubTransactionId mySubid,
								   SubTransactionId parentSubid);
extern void GTCatCacheDiscard(void);

#endif							/* GTCATCACHE_H */
