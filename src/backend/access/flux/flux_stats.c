/*-------------------------------------------------------------------------
 *
 * flux_stats.c
 *	  FLUX-specific statistics collection for ANALYZE
 *
 * This module collects statistics that are unique to the FLUX storage
 * format: compression ratios, overflow usage, space efficiency, and the
 * distribution of the per-tuple commit-ts word.  These statistics supplement
 * the standard
 * per-column statistics (MCV, histograms, NULL fractions, etc.) that
 * PostgreSQL's ANALYZE framework collects automatically via the
 * scan_analyze_next_block / scan_analyze_next_tuple callbacks.
 *
 * The collected statistics are logged at DEBUG1 level and made available
 * through the FluxCollectRelationStats() interface so that the planner
 * can incorporate FLUX-specific cost adjustments.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_stats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/flux.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"

/*
 * FluxCollectRelationStats
 *
 * Scan the relation to collect FLUX-specific statistics.  This performs
 * a full sequential pass over every page, examining each item to measure
 * compression ratios, overflow usage, tuple sizes, free space, and the
 * distribution of the per-tuple commit-ts word.
 *
 * This is designed to be called during ANALYZE after the standard sampling
 * is complete.  It does its own full scan because the standard sampling
 * only visits a random subset of blocks, which is fine for per-column
 * statistics but insufficient for accurate relation-wide measurements
 * like total overflow bytes or bloat factor.
 *
 * The caller must pass a zeroed FluxRelationStats struct.
 */
void
FluxCollectRelationStats(Relation rel, FluxRelationStats *stats)
{
	BlockNumber nblocks;
	BlockNumber blkno;
	int64		total_tuple_bytes = 0;
	int64		total_compressed_tuples = 0;
	int64		total_overflow_tuples = 0;
	int64		total_overflow_chains = 0;
	int64		total_live = 0;
	int64		total_dead = 0;
	double		total_free_space = 0.0;
	int64		total_uncompressed_size = 0;
	int64		total_compressed_size = 0;
	bool		commit_ts_seen = false;
	uint64		commit_ts_min = PG_UINT64_MAX;
	uint64		commit_ts_max = 0;

	/* Initialize output */
	memset(stats, 0, sizeof(FluxRelationStats));

	/* Get number of blocks */
	if (!smgrexists(RelationGetSmgr(rel), MAIN_FORKNUM))
		return;

	nblocks = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	stats->total_pages = nblocks;

	if (nblocks == 0)
		return;

	/*
	 * Scan every page.  We take only a shared lock on each page and release
	 * it before moving to the next, keeping contention low.
	 */
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buffer;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber offnum;
		Size		page_free;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
									RBM_NORMAL, NULL);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		/* Skip uninitialized pages */
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}

		maxoff = PageGetMaxOffsetNumber(page);
		page_free = PageGetFreeSpace(page);
		total_free_space += (double) page_free / (double) BLCKSZ;

		for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
		{
			ItemId		itemid = PageGetItemId(page, offnum);
			FluxTupleHeader *hdr;
			Size		item_len;

			if (!ItemIdIsNormal(itemid))
			{
				if (ItemIdIsDead(itemid))
					total_dead++;
				continue;
			}

			item_len = ItemIdGetLength(itemid);
			hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

			/* Skip overflow records -- counted separately */
			if (FluxIsOverflowRecordInline(hdr, item_len))
			{
				total_overflow_chains++;
				stats->total_overflow_bytes += item_len;
				continue;
			}

			/* This is a real tuple */
			total_tuple_bytes += item_len;

			if (hdr->t_flags & FLUX_TUPLE_DELETED)
			{
				total_dead++;
				continue;
			}

			/* Live tuple */
			total_live++;

			/* Check compression */
			if (hdr->t_flags & FLUX_TUPLE_COMPRESSED)
			{
				total_compressed_tuples++;

				/*
				 * Estimate compression ratio from the compression header that
				 * follows the tuple header, if present.
				 */
				if (item_len > FLUX_TUPLE_OVERHEAD + sizeof(FluxCompressionHeader))
				{
					FluxCompressionHeader *comp_hdr;

					comp_hdr = (FluxCompressionHeader *)
						((char *) hdr + FLUX_TUPLE_OVERHEAD);
					total_uncompressed_size += comp_hdr->orig_size;
					total_compressed_size += comp_hdr->comp_size;
				}
			}

			/* Check overflow */
			if (hdr->t_flags & FLUX_TUPLE_HAS_OVERFLOW)
				total_overflow_tuples++;

			/* Track commit-ts word range (diagnostic) */
			if (hdr->t_commit_ts > 0)
			{
				commit_ts_seen = true;
				if (hdr->t_commit_ts < commit_ts_min)
					commit_ts_min = hdr->t_commit_ts;
				if (hdr->t_commit_ts > commit_ts_max)
					commit_ts_max = hdr->t_commit_ts;
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	/* Compute derived statistics */
	stats->total_live_tuples = total_live;
	stats->total_dead_tuples = total_dead;

	if (total_live > 0)
	{
		stats->avg_tuple_size = (double) total_tuple_bytes / (double) total_live;
		stats->pct_compressed = (double) total_compressed_tuples / (double) total_live;
		stats->pct_overflow = (double) total_overflow_tuples / (double) total_live;
	}

	if (total_compressed_size > 0 && total_uncompressed_size > 0)
		stats->compression_ratio = (double) total_uncompressed_size /
			(double) total_compressed_size;
	else
		stats->compression_ratio = 1.0;

	if (total_overflow_tuples > 0)
		stats->avg_overflow_chain_len = (double) total_overflow_chains /
			(double) total_overflow_tuples;

	if (nblocks > 0)
	{
		stats->avg_live_per_page = (double) total_live / (double) nblocks;
		stats->free_space_frac = total_free_space / (double) nblocks;
	}

	/* Bloat = total allocated space / actual live data */
	if (total_tuple_bytes > 0)
		stats->bloat_factor = ((double) nblocks * BLCKSZ) /
			(double) total_tuple_bytes;
	else
		stats->bloat_factor = 1.0;

	/* commit-ts word range */
	if (commit_ts_seen)
	{
		stats->commit_ts_stats_valid = true;
		stats->commit_ts_min = commit_ts_min;
		stats->commit_ts_max = commit_ts_max;
	}
}

/*
 * FluxLogRelationStats
 *
 * Emit the collected FLUX statistics at the given log level (typically
 * DEBUG1 during ANALYZE, or LOG for diagnostic purposes).  Produces three
 * separate ereport messages:
 *   1. Page counts and live/dead tuple totals
 *   2. Average tuple size, compression percentage/ratio, overflow stats
 *   3. Average live tuples per page, free space fraction, bloat factor
 * If commit-ts word statistics are valid, a fourth message shows the range.
 *
 * Parameters:
 *   rel    - the relation whose statistics are being logged
 *   stats  - the collected FluxRelationStats structure
 *   elevel - ereport log level (e.g., DEBUG1, LOG, WARNING)
 */
void
FluxLogRelationStats(Relation rel, const FluxRelationStats *stats, int elevel)
{
	ereport(elevel,
			(errmsg("FLUX stats for \"%s\": "
					"%lld pages, %lld live tuples, %lld dead tuples",
					RelationGetRelationName(rel),
					(long long) stats->total_pages,
					(long long) stats->total_live_tuples,
					(long long) stats->total_dead_tuples)));

	ereport(elevel,
			(errmsg("FLUX stats for \"%s\": "
					"avg tuple size %.1f bytes, "
					"%.1f%% compressed (ratio %.2f), "
					"%.1f%% overflow (avg chain %.1f)",
					RelationGetRelationName(rel),
					stats->avg_tuple_size,
					stats->pct_compressed * 100.0,
					stats->compression_ratio,
					stats->pct_overflow * 100.0,
					stats->avg_overflow_chain_len)));

	ereport(elevel,
			(errmsg("FLUX stats for \"%s\": "
					"avg %.1f live/page, "
					"%.1f%% free space, "
					"bloat factor %.2f",
					RelationGetRelationName(rel),
					stats->avg_live_per_page,
					stats->free_space_frac * 100.0,
					stats->bloat_factor)));

	if (stats->commit_ts_stats_valid)
	{
		ereport(elevel,
				(errmsg("FLUX stats for \"%s\": "
						"commit-ts word range [%llu .. %llu]",
						RelationGetRelationName(rel),
						(unsigned long long) stats->commit_ts_min,
						(unsigned long long) stats->commit_ts_max)));
	}
}
