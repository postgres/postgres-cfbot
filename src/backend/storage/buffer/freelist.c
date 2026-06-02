/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LIBNUMA
#include <sched.h>
#endif

#ifdef USE_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))


/*
 * Information about one partition of the ClockSweep (on a subset of buffers).
 *
 * XXX Should be careful to align this to cachelines, etc.
 */
typedef struct
{
	/* Spinlock: protects the values below */
	slock_t		clock_sweep_lock;

	/* range for this clock sweep partition */
	int32		node;
	int32		firstBuffer;
	int32		numBuffers;

	/*
	 * clock-sweep hand: index of next buffer to consider grabbing. Note that
	 * this isn't a concrete buffer - we only ever increase the value. So, to
	 * get an actual buffer, it needs to be used modulo NBuffers.
	 *
	 * XXX This is relative to firstBuffer, so needs to be offset properly.
	 *
	 * XXX firstBuffer + (nextVictimBuffer % numBuffers)
	 */
	pg_atomic_uint32 nextVictimBuffer;

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32		completePasses; /* Complete cycles of the clock-sweep */
	pg_atomic_uint32 numBufferAllocs;	/* Buffers allocated since last reset */

	/* running total of allocs */
	pg_atomic_uint64 numTotalAllocs;

} ClockSweep;

/*
 * The shared freelist control information.
 */
typedef struct
{
	/* Spinlock: protects the values below */
	slock_t		buffer_strategy_lock;

	/*
	 * Bgworker process to be notified upon activity or -1 if none. See
	 * StrategyNotifyBgWriter.
	 */
	int			bgwprocno;

	/* cached info about freelist partitioning */
	int			num_nodes;
	int			num_partitions;
	int			num_partitions_per_node;

	/* clocksweep partitions */
	ClockSweep	sweeps[FLEXIBLE_ARRAY_MEMBER];
} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

static void StrategyCtlShmemRequest(void *arg);
static void StrategyCtlShmemInit(void *arg);

const ShmemCallbacks StrategyCtlShmemCallbacks = {
	.request_fn = StrategyCtlShmemRequest,
	.init_fn = StrategyCtlShmemInit,
};

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array */
	int			nbuffers;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 */
	int			current;

	/*
	 * Array of buffer numbers.  InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


/* Prototypes for internal functions */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint64 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);
static ClockSweep *ChooseClockSweep(void);

/*
 * ClockSweepTick - Helper routine for StrategyGetBuffer()
 *
 * Move the clock hand one buffer ahead of its current position and return the
 * id of the buffer now under the hand.
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;
	ClockSweep *sweep = ChooseClockSweep();

	/*
	 * Atomically move hand ahead one buffer - if there's several processes
	 * doing this, this can lead to buffers being returned slightly out of
	 * apparent order.
	 */
	victim =
		pg_atomic_fetch_add_u32(&sweep->nextVictimBuffer, 1);

	if (victim >= sweep->numBuffers)
	{
		uint32		originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors */
		victim = victim % sweep->numBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 */
				SpinLockAcquire(&sweep->clock_sweep_lock);

				wrapped = expected % sweep->numBuffers;

				success = pg_atomic_compare_exchange_u32(&sweep->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					sweep->completePasses++;
				SpinLockRelease(&sweep->clock_sweep_lock);
			}
		}
	}

	/*
	 * Make sure we've calculated a buffer in the range of the partition. Buffer
	 * IDs are 1-based, we're calculating 0-based indexes.
	 */
	Assert((victim >= 0) && (victim < sweep->numBuffers));
	Assert(BufferIsValid(1 + sweep->firstBuffer + victim));

	return sweep->firstBuffer + victim;
}

/*
 * ClockSweepPartitionIndex
 *		pick the clock-sweep partition to use based on PID and NUMA node
 *
 * With libnuma, use the NUMA node and PID to pick the partition. Otherwise
 * use just PID (as if there's a single NUMA node).
 *
 * XXX This should also check if buffers are NUMA-partitioned, not just if
 * compiled with libnuma.
 */
static int
ClockSweepPartitionIndex(void)
{
	int		node = 0,
			index;
	pid_t	pid = MyProcPid;;

	Assert(StrategyControl->num_partitions ==
		   (StrategyControl->num_nodes * StrategyControl->num_partitions_per_node));

	/*
	 * If buffers are NUMA-partitioned, determine the partition using the NUMA
	 * node and PID. Without NUMA assume everything is a single NUMA node 0, and
	 * we pick the partition based on PID.
	 */
#ifdef USE_LIBNUMA
	if (shared_buffers_numa)
	{
		int		cpu;

		/* XXX do we need to check sched_getcpu is available, somehow? */
		if ((cpu = sched_getcpu()) < 0)
			elog(ERROR, "sched_getcpu failed: %m");

		node = numa_node_of_cpu(cpu);
	}
#endif

	/*
	 * We should't get unexpected NUMA nodes, not considered when setting up the
	 * buffer partitions. It could happen if the allowed NUMA nodes get adjusted
	 * at runtime, but at this point we just create partitions for all existing
	 * nodes. We could plan for allowed partitions, but then what if those get
	 * disabled, and the user allows some other partitions?
	 */
	if ((node < 0) || (node > StrategyControl->num_nodes))
		elog(ERROR, "node out of range: %d > %u", node, StrategyControl->num_nodes);

	/*
	 * Calculate the partition index. Nodes have the same number of partitions,
	 * and we use the PID to pick one of those (for a given node). If there's
	 * only a single partition per node, we can ignore PID and use node directly.
	 */
	if (StrategyControl->num_partitions_per_node == 1)
	{
		/* fast-path */
		index = node;
	}
	else
	{
		/* use PID to pick one of node's partitions */
		index = (node * StrategyControl->num_partitions_per_node)
			+ (pid % StrategyControl->num_partitions_per_node);
	}

	/* should have a valid partition index */
	Assert((index >= 0) && (index < StrategyControl->num_partitions));

	return index;
}

/*
 * ChooseClockSweep
 *		pick a clocksweep partition based on NUMA node and PID
 *
 * Pick a partition mapped to the NUMA node the backend is currently running
 * on, and use PID if there are multiple partitions per node. Without NUMA
 * supported/enabled, use just PID.
 *
 * XXX Maybe we should do both the total and "per group" counts a power of
 * two? That'd allow using shifts instead of divisions in the calculation,
 * and that's cheaper. But how would that deal with odd number of nodes?
 */
static ClockSweep *
ChooseClockSweep(void)
{
	int			index = ClockSweepPartitionIndex();

	return &StrategyControl->sweeps[index];
}

/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	GetVictimBuffer(). The only hard requirement GetVictimBuffer() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	It is the callers responsibility to ensure the buffer ownership can be
 *	tracked via TrackNewBufferPin().
 *
 *	The buffer is pinned and marked as owned, using TrackNewBufferPin(),
 *	before returning.
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint64 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;

	*from_ring = false;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need buffer_strategy_lock.
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			return buf;
		}
	}

	/*
	 * If asked, we need to waken the bgwriter. Since we don't want to rely on
	 * a spinlock for this we force a read from shared memory once, and then
	 * set the latch based on that value. We need to go through that length
	 * because otherwise bgwprocno might be reset while/after we check because
	 * the compiler might just reread from memory.
	 *
	 * This can possibly set the latch of the wrong process if the bgwriter
	 * dies in the wrong moment. But since PGPROC->procLatch is never
	 * deallocated the worst consequence of that is that we set the latch of
	 * some arbitrary process.
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* reset bgwprocno first, before setting the latch */
		StrategyControl->bgwprocno = -1;

		/*
		 * Not acquiring ProcArrayLock here which is slightly icky. It's
		 * actually fine because procLatch isn't ever freed, so we just can
		 * potentially set the wrong process' (or no process') latch.
		 */
		SetLatch(&GetPGProcByNumber(bgwprocno)->procLatch);
	}

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 *
	 * XXX It's not quite right we call ChooseClockSweep twice - now, and then
	 * a couple lines later (through ClockSweepTick). If the process moves
	 * between CPUs / NUMA nodes in between, these call may pick different
	 * partitions, confusing the logic a bit.
	 */
	pg_atomic_fetch_add_u32(&ChooseClockSweep()->numBufferAllocs, 1);

	/*
	 * Use the "clock sweep" algorithm to find a free buffer
	 *
	 * XXX Note that ClockSweepTick() is NUMA-aware, i.e. it only looks at
	 * buffers from a single partition, aligned with the NUMA node. That means
	 * a process "sweeps" only a fraction of buffers, even if the other buffers
	 * are better candidates for eviction. Maybe there should be some logic to
	 * "steal" buffers from other partitions or other nodes?
	 *
	 * XXX This only searches a single partition, which can result in "no
	 * unpinned buffers available" even if there are buffers in other
	 * partitions. Needs to scan partitions if needed, as a fallback.
	 *
	 * XXX Would that also mean we should have multiple bgwriters, one for each
	 * node, or would one bgwriter still handle all nodes?
	 *
	 * XXX Also, the trycounter should not be set to NBuffers, but to buffer
	 * count for that one partition. In fact, this should not call ClockSweepTick
	 * for every iteration. The call is likely quite expensive (does a lot
	 * of stuff), and also may return a different partition on each call.
	 * We should just do it once, and then do the for(;;) loop. And then
	 * maybe advance to the next partition, until we scan through all of them.
	 */
	trycounter = NBuffers;
	for (;;)
	{
		uint64		old_buf_state;
		uint64		local_buf_state;

		buf = GetBufferDescriptor(ClockSweepTick());

		/*
		 * Check whether the buffer can be used and pin it if so. Do this
		 * using a CAS loop, to avoid having to lock the buffer header.
		 */
		old_buf_state = pg_atomic_read_u64(&buf->state);
		for (;;)
		{
			local_buf_state = old_buf_state;

			/*
			 * If the buffer is pinned or has a nonzero usage_count, we cannot
			 * use it; decrement the usage_count (unless pinned) and keep
			 * scanning.
			 */

			if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0)
			{
				if (--trycounter == 0)
				{
					/*
					 * We've scanned all the buffers without making any state
					 * changes, so all the buffers are pinned (or were when we
					 * looked at them). We could hope that someone will free
					 * one eventually, but it's probably better to fail than
					 * to risk getting stuck in an infinite loop.
					 */
					elog(ERROR, "no unpinned buffers available");
				}
				break;
			}

			/* See equivalent code in PinBuffer() */
			if (unlikely(local_buf_state & BM_LOCKED))
			{
				old_buf_state = WaitBufHdrUnlocked(buf);
				continue;
			}

			if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			{
				local_buf_state -= BUF_USAGECOUNT_ONE;

				if (pg_atomic_compare_exchange_u64(&buf->state, &old_buf_state,
												   local_buf_state))
				{
					trycounter = NBuffers;
					break;
				}
			}
			else
			{
				/* pin the buffer if the CAS succeeds */
				local_buf_state += BUF_REFCOUNT_ONE;

				if (pg_atomic_compare_exchange_u64(&buf->state, &old_buf_state,
												   local_buf_state))
				{
					/* Found a usable buffer */
					if (strategy != NULL)
						AddBufferToRing(strategy, buf);
					*buf_state = local_buf_state;

					TrackNewBufferPin(BufferDescriptorGetBuffer(buf));

					return buf;
				}
			}
		}
	}
}

/*
 * StrategySyncPrepare -- prepare for sync of all partitions
 *
 * Determine the number of clocksweep partitions, and calculate the recent
 * buffers allocs (as a sum of all the partitions). This allows BgBufferSync
 * to calculate average number of allocations per partition for the next
 * sync cycle.
 *
 * In addition it returns the count of recent buffer allocs, which is a total
 * summed from all partitions. The alloc counts are reset after being read,
 * as the partitions are walked.
 */
void
StrategySyncPrepare(int *num_parts, uint32 *num_buf_alloc)
{
	*num_buf_alloc = 0;
	*num_parts = StrategyControl->num_partitions;

	/*
	 * We lock the partitions one by one, so not exacly in sync, but that
	 * should be fine. We're only looking for heuristics anyway.
	 */
	for (int i = 0; i < StrategyControl->num_partitions; i++)
	{
		ClockSweep *sweep = &StrategyControl->sweeps[i];

		/* XXX Do we need the lock, if we're only accessing atomics? Surely not. */
		/* XXX Are we ever calling this without num_buf_alloc? */
		SpinLockAcquire(&sweep->clock_sweep_lock);
		if (num_buf_alloc)
		{
			uint32	allocs = pg_atomic_exchange_u32(&sweep->numBufferAllocs, 0);

			/* include the count in the running total */
			pg_atomic_fetch_add_u64(&sweep->numTotalAllocs, allocs);

			*num_buf_alloc += allocs;
		}
		SpinLockRelease(&sweep->clock_sweep_lock);
	}
}

/*
 * StrategySyncStart -- tell BgBufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BgBufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer).
 *
 * This only considers a single clocksweep partition, as BgBufferSync looks
 * at them one by one.
 */
int
StrategySyncStart(int partition, uint32 *complete_passes,
				  int *first_buffer, int *num_buffers)
{
	uint32		nextVictimBuffer;
	int			result;
	ClockSweep *sweep = &StrategyControl->sweeps[partition];

	Assert((partition >= 0) && (partition < StrategyControl->num_partitions));

	SpinLockAcquire(&sweep->clock_sweep_lock);
	nextVictimBuffer = pg_atomic_read_u32(&sweep->nextVictimBuffer);
	result = nextVictimBuffer % sweep->numBuffers;

	*first_buffer = sweep->firstBuffer;
	*num_buffers = sweep->numBuffers;

	if (complete_passes)
	{
		*complete_passes = sweep->completePasses;

		/*
		 * Additionally add the number of wraparounds that happened before
		 * completePasses could be incremented. C.f. ClockSweepTick().
		 */
		*complete_passes += nextVictimBuffer / sweep->numBuffers;
	}
	SpinLockRelease(&sweep->clock_sweep_lock);

	/* XXX buffer IDs start at 1, we're calculating 0-based indexes */
	Assert(BufferIsValid(1 + sweep->firstBuffer + result));

	return sweep->firstBuffer + result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwprocno isn't -1, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass -1 to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * We acquire buffer_strategy_lock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyCtlShmemRequest -- request shared memory for the buffer
 *		cache replacement strategy.
 */
static void
StrategyCtlShmemRequest(void *arg)
{
	int			num_partitions;

	/* get the number of buffer partitions */
	BufferPartitionsCalculate(NULL, &num_partitions, NULL);

	ShmemRequestStruct(.name = "Buffer Strategy Status",
					   .size = offsetof(BufferStrategyControl, sweeps) +
							mul_size(num_partitions, sizeof(ClockSweep)),
					   .ptr = (void **) &StrategyControl
		);
}

/*
 * StrategyCtlShmemInit -- initialize the buffer cache replacement strategy.
 */
static void
StrategyCtlShmemInit(void *arg)
{
	SpinLockInit(&StrategyControl->buffer_strategy_lock);

	/* Remember the number of partitions */
	BufferPartitionsParams(&StrategyControl->num_nodes,
						   &StrategyControl->num_partitions,
						   &StrategyControl->num_partitions_per_node);

	/* Initialize the clock sweep pointers (for all partitions) */
	for (int i = 0; i < StrategyControl->num_partitions; i++)
	{
		int			node,
					num_buffers,
					first_buffer,
					last_buffer;

		SpinLockInit(&StrategyControl->sweeps[i].clock_sweep_lock);

		pg_atomic_init_u32(&StrategyControl->sweeps[i].nextVictimBuffer, 0);

		/* get info about the buffer partition */
		BufferPartitionGet(i, &node, &num_buffers,
						   &first_buffer, &last_buffer);

		/*
		 * FIXME This may not quite right, because if NBuffers is not a
		 * perfect multiple of numBuffers, the last partition will have
		 * numBuffers set too high. buf_init handles this by tracking the
		 * remaining number of buffers, and not overflowing.
		 */
		StrategyControl->sweeps[i].node = node;
		StrategyControl->sweeps[i].numBuffers = num_buffers;
		StrategyControl->sweeps[i].firstBuffer = first_buffer;

		/* Clear statistics */
		StrategyControl->sweeps[i].completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->sweeps[i].numBufferAllocs, 0);
		pg_atomic_init_u64(&StrategyControl->sweeps[i].numTotalAllocs, 0);
	}

	/* No pending notification */
	StrategyControl->bgwprocno = -1;
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object */
			return NULL;

		case BAS_BULKREAD:
			{
				int			ring_max_kb;

				/*
				 * The ring always needs to be large enough to allow some
				 * separation in time between providing a buffer to the user
				 * of the strategy and that buffer being reused. Otherwise the
				 * user's pin will prevent reuse of the buffer, even without
				 * concurrent activity.
				 *
				 * We also need to ensure the ring always is large enough for
				 * SYNC_SCAN_REPORT_INTERVAL, as noted above.
				 *
				 * Thus we start out a minimal size and increase the size
				 * further if appropriate.
				 */
				ring_size_kb = 256;

				/*
				 * There's no point in a larger ring if we won't be allowed to
				 * pin sufficiently many buffers.  But we never limit to less
				 * than the minimal size above.
				 */
				ring_max_kb = GetPinLimit() * (BLCKSZ / 1024);
				ring_max_kb = Max(ring_size_kb, ring_max_kb);

				/*
				 * We would like the ring to additionally have space for the
				 * configured degree of IO concurrency. While being read in,
				 * buffers can obviously not yet be reused.
				 *
				 * Each IO can be up to io_combine_limit blocks large, and we
				 * want to start up to effective_io_concurrency IOs.
				 *
				 * Note that effective_io_concurrency may be 0, which disables
				 * AIO.
				 */
				ring_size_kb += (BLCKSZ / 1024) *
					io_combine_limit * effective_io_concurrency;

				if (ring_size_kb > ring_max_kb)
					ring_size_kb = ring_max_kb;
				break;
			}
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 2048;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- create a BufferAccessStrategy object with a
 *		number of buffers equivalent to the passed in size.
 *
 * If the given ring size is 0, no BufferAccessStrategy will be created and
 * the function will return NULL.  ring_size_kb must not be negative.
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* Figure out how many buffers ring_size_kb is */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 means unlimited, so no BufferAccessStrategy required */
	if (ring_buffers == 0)
		return NULL;

	/* Cap to 1/8th of shared_buffers */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers should never be less than 16, so this shouldn't happen */
	Assert(ring_buffers > 0);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- an accessor for the number of buffers in
 *		the ring
 *
 * Returns 0 on NULL input to match behavior of GetAccessStrategyWithSize()
 * returning NULL with 0 size.
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * GetAccessStrategyPinLimit -- get cap of number of buffers that should be pinned
 *
 * When pinning extra buffers to look ahead, users of a ring-based strategy are
 * in danger of pinning too much of the ring at once while performing look-ahead.
 * For some strategies, that means "escaping" from the ring, and in others it
 * means forcing dirty data to disk very frequently with associated WAL
 * flushing.  Since external code has no insight into any of that, allow
 * individual strategy types to expose a clamp that should be applied when
 * deciding on a maximum number of buffers to pin at once.
 *
 * Callers should combine this number with other relevant limits and take the
 * minimum.
 */
int
GetAccessStrategyPinLimit(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return NBuffers;

	switch (strategy->btype)
	{
		case BAS_BULKREAD:

			/*
			 * Since BAS_BULKREAD uses StrategyRejectBuffer(), dirty buffers
			 * shouldn't be a problem and the caller is free to pin up to the
			 * entire ring at once.
			 */
			return strategy->nbuffers;

		default:

			/*
			 * Tell caller not to pin more than half the buffers in the ring.
			 * This is a trade-off between look ahead distance and deferring
			 * writeback and associated WAL traffic.
			 */
			return strategy->nbuffers / 2;
	}
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty / not usable.
 *
 * The buffer is pinned and marked as owned, using TrackNewBufferPin(), before
 * returning.
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint64 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint64		old_buf_state;
	uint64		local_buf_state;	/* to avoid repeated (de-)referencing */


	/* Advance to next ring slot */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.  He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	buf = GetBufferDescriptor(bufnum - 1);

	/*
	 * Check whether the buffer can be used and pin it if so. Do this using a
	 * CAS loop, to avoid having to lock the buffer header.
	 */
	old_buf_state = pg_atomic_read_u64(&buf->state);
	for (;;)
	{
		local_buf_state = old_buf_state;

		/*
		 * If the buffer is pinned we cannot use it under any circumstances.
		 *
		 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
		 * since our own previous usage of the ring element would have left it
		 * there, but it might've been decremented by clock-sweep since then).
		 * A higher usage_count indicates someone else has touched the buffer,
		 * so we shouldn't re-use it.
		 */
		if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0
			|| BUF_STATE_GET_USAGECOUNT(local_buf_state) > 1)
			break;

		/* See equivalent code in PinBuffer() */
		if (unlikely(local_buf_state & BM_LOCKED))
		{
			old_buf_state = WaitBufHdrUnlocked(buf);
			continue;
		}

		/* pin the buffer if the CAS succeeds */
		local_buf_state += BUF_REFCOUNT_ONE;

		if (pg_atomic_compare_exchange_u64(&buf->state, &old_buf_state,
										   local_buf_state))
		{
			*buf_state = local_buf_state;

			TrackNewBufferPin(BufferDescriptorGetBuffer(buf));
			return buf;
		}
	}

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 */
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * Utility function returning the IOContext of a given BufferAccessStrategy's
 * strategy ring.
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * Currently, GetAccessStrategy() returns NULL for
			 * BufferAccessStrategyType BAS_NORMAL, so this case is
			 * unreachable.
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}

void
ClockSweepPartitionGetInfo(int idx,
						   uint32 *complete_passes, uint32 *next_victim_buffer,
						   uint64 *buffer_total_allocs, uint32 *buffer_allocs)
{
	ClockSweep *sweep = &StrategyControl->sweeps[idx];

	Assert((idx >= 0) && (idx < StrategyControl->num_partitions));

	/* get the clocksweep stats */
	*complete_passes = sweep->completePasses;
	*next_victim_buffer = pg_atomic_read_u32(&sweep->nextVictimBuffer);

	*buffer_allocs = pg_atomic_read_u32(&sweep->numBufferAllocs);
	*buffer_total_allocs = pg_atomic_read_u64(&sweep->numTotalAllocs);

	/* calculate the actual buffer ID */
	*next_victim_buffer = sweep->firstBuffer + (*next_victim_buffer % sweep->numBuffers);
}
