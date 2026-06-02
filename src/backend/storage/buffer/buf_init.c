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

#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proclist.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

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

/* number of buffer partitions */
#define NUM_CLOCK_SWEEP_PARTITIONS	4


/*
 * Register shared memory area for the buffer pool.
 */
static void
BufferManagerShmemRequest(void *arg)
{
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
					   .size = NUM_CLOCK_SWEEP_PARTITIONS * sizeof(BufferPartition),
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

	/* number of buffers per partition (make sure to not overflow) */
	int			part_buffers = NBuffers / NUM_CLOCK_SWEEP_PARTITIONS;
	int			remaining_buffers = NBuffers % NUM_CLOCK_SWEEP_PARTITIONS;

	BufferPartitionsRegistry->npartitions = NUM_CLOCK_SWEEP_PARTITIONS;

	for (int n = 0; n < BufferPartitionsRegistry->npartitions; n++)
	{
		BufferPartition *part = &BufferPartitionsRegistry->partitions[n];

		int			num_buffers = part_buffers;
		if (n < remaining_buffers)
			num_buffers += 1;

		remaining_buffers -= num_buffers;

		Assert((num_buffers > 0) && (num_buffers <= part_buffers));
		Assert((buffer >= 0) && (buffer < NBuffers));

		part->num_buffers = num_buffers;
		part->first_buffer = buffer;
		part->last_buffer = buffer + (num_buffers - 1);

		buffer += num_buffers;
	}

	AssertCheckBufferPartitions();
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
BufferPartitionGet(int idx, int *num_buffers,
				   int *first_buffer, int *last_buffer)
{
	if ((idx >= 0) && (idx < BufferPartitionsRegistry->npartitions))
	{
		BufferPartition *part = &BufferPartitionsRegistry->partitions[idx];

		*num_buffers = part->num_buffers;
		*first_buffer = part->first_buffer;
		*last_buffer = part->last_buffer;

		return;
	}

	elog(ERROR, "invalid partition index");
}
