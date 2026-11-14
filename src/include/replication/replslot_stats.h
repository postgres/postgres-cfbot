/*-------------------------------------------------------------------------
 *
 * replslot_stats.h
 *	  Replication slot statistics shared definitions.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/replication/replslot_stats.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLSLOT_STATS_H
#define REPLSLOT_STATS_H

typedef struct PgStat_ReplSlotStats
{
	int64		spill_txns;
	int64		spill_count;
	int64		spill_bytes;
	int64		stream_txns;
	int64		stream_count;
	int64		stream_bytes;
	int64		mem_exceeded_count;
	int64		total_txns;
	int64		total_bytes;
	int64		output_bytes;
} PgStat_ReplSlotStats;

#endif							/* REPLSLOT_STATS_H */