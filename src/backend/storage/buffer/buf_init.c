/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif

#include "port/pg_numa.h"
#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proclist.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "utils/guc_hooks.h"
#include "utils/varlena.h"

BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
ConditionVariableMinimallyPadded *BufferIOCVArray;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;
BufferPartitions *BufferPartitionsRegistry;

static void BufferManagerShmemRequest(void *arg);
static void BufferManagerShmemInit(void *arg);
static void BufferManagerShmemAttach(void *arg);
static void BufferPartitionsInit(void);

const ShmemCallbacks BufferManagerShmemCallbacks = {
	.request_fn = BufferManagerShmemRequest,
	.init_fn = BufferManagerShmemInit,
	.attach_fn = BufferManagerShmemAttach,
};

/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffer pool will
 *		become inconsistent.
 *
 * Buffer Replacement:
 *		see freelist.c.  A buffer cannot be replaced while in
 *		use either by data manager or during IO.
 *
 *
 * Synchronization/Locking:
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.  It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.  For efficiency the
 *		shared refcount isn't increased if an individual backend pins a buffer
 *		multiple times. Check the PrivateRefCount infrastructure in bufmgr.c.
 */

/*
 * Minimum number of buffer partitions, no matter the number of NUMA nodes.
 */
#define MIN_BUFFER_PARTITIONS	4

bool	shared_buffers_numa = false;

/*
 * Register shared memory area for the buffer pool.
 */
static void
BufferManagerShmemRequest(void *arg)
{
	int		nparts;

	BufferPartitionsCalculate(NULL, &nparts, NULL);

	ShmemRequestStruct(.name = "Buffer Descriptors",
					   .size = NBuffers * sizeof(BufferDescPadded),
	/* Align descriptors to a cacheline boundary. */
					   .alignment = PG_CACHE_LINE_SIZE,
					   .ptr = (void **) &BufferDescriptors,
		);

	ShmemRequestStruct(.name = "Buffer Blocks",
					   .size = NBuffers * (Size) BLCKSZ,
	/* Align buffer pool on IO page size boundary. */
					   .alignment = PG_IO_ALIGN_SIZE,
					   .ptr = (void **) &BufferBlocks,
		);

	ShmemRequestStruct(.name = "Buffer IO Condition Variables",
					   .size = NBuffers * sizeof(ConditionVariableMinimallyPadded),
	/* Align descriptors to a cacheline boundary. */
					   .alignment = PG_CACHE_LINE_SIZE,
					   .ptr = (void **) &BufferIOCVArray,
		);

	ShmemRequestStruct(.name = "Buffer Partition Registry",
					   .size = nparts * sizeof(BufferPartition),
	/* Align descriptors to a cacheline boundary. */
					   .alignment = PG_CACHE_LINE_SIZE,
					   .ptr = (void **) &BufferPartitionsRegistry,
		);

	/*
	 * The array used to sort to-be-checkpointed buffer ids is located in
	 * shared memory, to avoid having to allocate significant amounts of
	 * memory at runtime. As that'd be in the middle of a checkpoint, or when
	 * the checkpointer is restarted, memory allocation failures would be
	 * painful.
	 */
	ShmemRequestStruct(.name = "Checkpoint BufferIds",
					   .size = NBuffers * sizeof(CkptSortItem),
					   .ptr = (void **) &CkptBufferIds,
		);
}

/*
 * Initialize shared buffer pool
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
static void
BufferManagerShmemInit(void *arg)
{
	/*
	 * Initialize the buffer partition registry first, before other parts
	 * have a chance to touch the memory.
	 *
	 * Also moves memory to different NUMA nodes (if enabled by a GUC).
	 * Do this before the loop that initializes buffer headers etc. which
	 * may fault some of the memory pages etc.
	 */
	BufferPartitionsInit();

	/*
	 * Initialize all the buffer headers.
	 */
	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *buf = GetBufferDescriptor(i);

		ClearBufferTag(&buf->tag);

		pg_atomic_init_u64(&buf->state, 0);
		buf->wait_backend_pgprocno = INVALID_PROC_NUMBER;

		buf->buf_id = i;

		pgaio_wref_clear(&buf->io_wref);

		proclist_init(&buf->lock_waiters);
		ConditionVariableInit(BufferDescriptorGetIOCV(buf));
	}

	/* Initialize per-backend file flush context */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

static void
BufferManagerShmemAttach(void *arg)
{
	/* Initialize per-backend file flush context */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

/*
 * Sanity checks of buffers partitions - there must be no gaps, it must cover
 * the whole range of buffers, etc.
 */
static void
AssertCheckBufferPartitions(void)
{
#ifdef USE_ASSERT_CHECKING
	int			num_buffers = 0;

	Assert(BufferPartitionsRegistry->npartitions > 0);

	for (int i = 0; i < BufferPartitionsRegistry->npartitions; i++)
	{
		BufferPartition *part = &BufferPartitionsRegistry->partitions[i];

		/*
		 * We can get a single-buffer partition, if the sizing forces the last
		 * partition to be just one buffer. But it's unlikely (and
		 * undesirable).
		 */
		Assert(part->first_buffer <= part->last_buffer);
		Assert((part->last_buffer - part->first_buffer + 1) == part->num_buffers);

		num_buffers += part->num_buffers;

		/*
		 * The first partition needs to start on buffer 0. Later partitions
		 * need to be contiguous, without skipping any buffers.
		 */
		if (i == 0)
		{
			Assert(part->first_buffer == 0);
		}
		else
		{
			BufferPartition *prev = &BufferPartitionsRegistry->partitions[i - 1];

			Assert((part->first_buffer - 1) == prev->last_buffer);
		}

		/* the last partition needs to end on buffer (NBuffers - 1) */
		if (i == (BufferPartitionsRegistry->npartitions - 1))
		{
			Assert(part->last_buffer == (NBuffers - 1));
		}
	}

	Assert(num_buffers == NBuffers);
#endif
}

/*
 * BufferPartitionsInit
 *		Initialize registry of buffer partitions.
 */
static void
BufferPartitionsInit(void)
{
	int			buffer = 0;

	int			nnodes,
				npartitions,
				npartitions_per_node;

	int			buffers_per_partition,
				buffers_remaining;

	/* calculate partitioning parameters */
	BufferPartitionsCalculate(&nnodes, &npartitions, &npartitions_per_node);

	/* paranoia */
	Assert(nnodes > 0);
	Assert(npartitions >= MIN_BUFFER_PARTITIONS);
	Assert((npartitions % nnodes) == 0);
	Assert((npartitions_per_node * nnodes) == npartitions);

	BufferPartitionsRegistry->nnodes = nnodes;
	BufferPartitionsRegistry->npartitions = npartitions;
	BufferPartitionsRegistry->npartitions_per_node = npartitions_per_node;

	/* regular partition size, the first couple get an extra buffer */
	buffers_per_partition = (NBuffers / npartitions);
	buffers_remaining = (NBuffers % buffers_per_partition);

	/* should have all the buffers */
	Assert((buffers_per_partition * npartitions + buffers_remaining) == NBuffers);

	/*
	 * Now walk the partitions, and set the buffer range. Optionally, place
	 * the partitions on a given node (for all partitions at once).
	 */
	for (int n = 0; n < nnodes; n++)
	{
		for (int p = 0; p < npartitions_per_node; p++)
		{
			int			idx = (n * npartitions_per_node) + p;
			BufferPartition *part = &BufferPartitionsRegistry->partitions[idx];

			/*
			 * Assign to the NUMA node, but only with shared_buffers_numa=on.
			 *
			 * XXX we should get an actual node ID from the mask, in case the
			 * task is restricted to only some nodes.
			 */
			part->numa_node = (shared_buffers_numa) ? n : -1;

			/* The first couple partitions may get an extra buffer. */
			part->num_buffers = buffers_per_partition;
			if (idx < buffers_remaining)
				part->num_buffers += 1;

			/* remember the buffer range */
			part->first_buffer = buffer;
			part->last_buffer = buffer + (part->num_buffers - 1);

			/* remember start of the next partition */
			buffer += part->num_buffers;
		}

#ifdef USE_LIBNUMA
		/*
		 * Now try to locate buffers and buffer descriptors to the node (all
		 * partitions for the node at once).
		 */
		if (shared_buffers_numa)
		{
			Size	numa_page_size = pg_numa_page_size();

			int		part_first,
					part_last,
					buff_first,
					buff_last;

			char   *startptr,
				   *endptr;

			/* first/last partition for this node */
			part_first = (n * npartitions_per_node);
			part_last = part_first + (npartitions_per_node - 1);

			/* buffers (blocks) */

			/* first/last buffer */
			buff_first = BufferPartitionsRegistry->partitions[part_first].first_buffer;
			buff_last = BufferPartitionsRegistry->partitions[part_last].last_buffer;

			/* beginning of the first block, end of last block */
			startptr = BufferBlocks + ((Size) buff_first * BLCKSZ);
			endptr = BufferBlocks + ((Size) (buff_last + 1) * BLCKSZ);

			/* print some warnings when the partitions are not aligned */
			if ((startptr != (char *) TYPEALIGN(numa_page_size, startptr)) ||
				(endptr != (char *) TYPEALIGN_DOWN(numa_page_size, endptr)))
			{
				elog(WARNING, "buffers for node %d not well aligned [%p,%p] aligned [%p,%p]",
					 n, startptr, endptr,
					 (char *) TYPEALIGN(numa_page_size, startptr),
					 (char *) TYPEALIGN_DOWN(numa_page_size, endptr));
			}

			/* best effort: align the pointers, so that the mbind() works */
			startptr = (char *) TYPEALIGN(numa_page_size, startptr);
			endptr = (char *) TYPEALIGN_DOWN(numa_page_size, endptr);

			/* XXX or should we use pg_numa_move_to_node? */
			pg_numa_bind_to_node(startptr, endptr, n);

			/* buffer descriptors */

			/* beginning of the first descriptor, end of last descriptor */
			startptr = (char *) &BufferDescriptors[buff_first];
			endptr = (char *) &BufferDescriptors[buff_last] + 1;

			/* print some warnings when the partitions are not aligned */
			if ((startptr != (char *) TYPEALIGN(numa_page_size, startptr)) ||
				(endptr != (char *) TYPEALIGN_DOWN(numa_page_size, endptr)))
			{
				elog(WARNING, "buffers descriptors for node %d not well aligned [%p,%p] aligned [%p,%p]",
					 n, startptr, endptr,
					 (char *) TYPEALIGN(numa_page_size, startptr),
					 (char *) TYPEALIGN_DOWN(numa_page_size, endptr));
			}

			/* best effort: align the pointers, so that the mbind() works */
			startptr = (char *) TYPEALIGN(numa_page_size, startptr);
			endptr = (char *) TYPEALIGN_DOWN(numa_page_size, endptr);

			/* XXX or should we use pg_numa_move_to_node? */
			pg_numa_bind_to_node(startptr, endptr, n);
		}
#endif
	}

	AssertCheckBufferPartitions();
}

/*
 * BufferPartitionsCalculate
 *		Pick number of buffer partitions for the number of nodes and
 *		MIN_BUFFER_PARTITIONS.
 *
 * Picks the smallest number of partitions higher thah MIN_BUFFER_PARTITIONS,
 * such that all nodes have the same number of partitions.
 *
 * This is best-effort with respect to size of the partitions. It's possible
 * the partitions are not a perfect multiple of page size, in which case
 * we set location only for the part where that is possible. The buffers on
 * the "boundary" may get located up on arbitrary nodes.
 *
 * The extra complexity of figuring out the right "partition size" is not
 * worth it, and it can lead to some partitions being much smaller. This way
 * we end up with partitions of almost exactly the same size (one BLCKSZ is
 * the largest difference).
 *
 * We expect shared buffers to be much larger than page size (at least on
 * system where NUMA is a relevant feature), so the number of "not located"
 * buffers should be a negligible fraction. This only affects pages between
 * partitions for different nodes, so (nodes-1) pages. This is certainly
 * fine with 2MB huge pages, but even with 1GB pages it should be OK (as
 * such systems should have humongous amounts of memory).
 *
 * It also means we don't need to worry about memory page size before knowing
 * if huge pages got used (which we only learn during allocation).
 */
void
BufferPartitionsCalculate(int *num_nodes, int *num_partitions,
						  int *num_partitions_per_node)
{
	int		nnodes,
			nparts,
			nparts_per_node;

#if USE_LIBNUMA
	nnodes = numa_num_configured_nodes();
	nparts_per_node = 1;	/* at least one partition per node */

	while ((nparts_per_node * nnodes) < MIN_BUFFER_PARTITIONS)
		nparts_per_node++;

	nparts = (nnodes * nparts_per_node);
#else
	/* without NUMA, assume there's just one node */
	nnodes = 1;
	nparts = MIN_BUFFER_PARTITIONS;
	nparts_per_node = MIN_BUFFER_PARTITIONS;
#endif

	if (num_nodes)
		*num_nodes = nnodes;

	if (num_partitions)
		*num_partitions = nparts;

	if (num_partitions_per_node)
		*num_partitions_per_node = nparts_per_node;
}

/*
 * BufferPartitionCount
 *		Returns the number of partitions created.
 */
int
BufferPartitionCount(void)
{
	return BufferPartitionsRegistry->npartitions;
}

/*
 * BufferPartitionGet
 *		Returns information about a partition at the provided index.
 *
 * The returned information is first/last buffer, number of buffers.
 */
void
BufferPartitionGet(int idx, int *node, int *num_buffers,
				   int *first_buffer, int *last_buffer)
{
	if ((idx >= 0) && (idx < BufferPartitionsRegistry->npartitions))
	{
		BufferPartition *part = &BufferPartitionsRegistry->partitions[idx];

		*node = part->numa_node;
		*num_buffers = part->num_buffers;
		*first_buffer = part->first_buffer;
		*last_buffer = part->last_buffer;

		return;
	}

	elog(ERROR, "invalid partition index");
}

void
BufferPartitionsParams(int *num_nodes, int *num_partitions,
					   int *num_partitions_per_node)
{
	if (num_nodes)
		*num_nodes = BufferPartitionsRegistry->nnodes;

	if (num_partitions)
		*num_partitions = BufferPartitionsRegistry->npartitions;

	if (num_partitions_per_node)
		*num_partitions_per_node = BufferPartitionsRegistry->npartitions_per_node;
}
