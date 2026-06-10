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
} GTCatCacheIdentifier;

#define NUM_GT_CAT_CACHES	((int) PG_TEMP_CLASS + 1)

extern bool GTCatCacheTupleExists(GTCatCacheIdentifier cacheId, Oid relid);
extern HeapTuple GTCatCacheSearch(GTCatCacheIdentifier cacheId, Oid relid);
extern void GTCatCacheTupleInsert(GTCatCacheIdentifier cacheId, Oid relid,
								  char relkind, TupleDesc tupdesc,
								  const Datum *values, const bool *nulls);
extern void GTCatCacheTupleUpdate(GTCatCacheIdentifier cacheId, Oid relid,
								  HeapTuple newtuple);
extern void GTCatCacheTupleDelete(GTCatCacheIdentifier cacheId, Oid relid);
extern void GTCatCacheFlush(void);
extern void AtEOXact_GTCatCache(bool isCommit);
extern void AtEOSubXact_GTCatCache(bool isCommit, SubTransactionId mySubid,
								   SubTransactionId parentSubid);

#endif							/* GTCATCACHE_H */
