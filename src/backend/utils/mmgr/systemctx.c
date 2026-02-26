/*-------------------------------------------------------------------------
 *
 * systemctx.c
 *	  System context allocator definitions.
 *
 * SystemContext has no concept of grouping several allocations into blocks
 * like many of the other allocators do.  Each palloc() results in a malloc(),
 * and each pfree() results in a free(), therefore, this isn't suitable for
 * cases that require many allocations.  The most suitable use cases are for
 * contexts that require very few and long-lived allocations.
 *
 * The memory overhead for the context itself is much smaller than with other
 * context types, so this implementation can be suitable when a large number
 * of contexts are required.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/systemctx.c
 *
 * NOTE: Allocations are MAXALIGNed.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

/* Struct definition for the System memory context. */
typedef struct SystemContext
{
	MemoryContextData header;	/* Standard memory-context fields */

	dlist_head	allocations;	/* list of all allocated SystemBlocks */
} SystemContext;

/* Struct definition for SystemContext's malloc()'d header. */
typedef struct SystemBlock
{
	dlist_node	node;			/* node in SystemContext->allocations */
	size_t		sz;				/* malloc'd size of this allocation */
	SystemContext *context;		/* direct pointer to the owning context */
} SystemBlock;

#define SYSTEM_CONTEXTSIZE	(MAXALIGN(sizeof(SystemContext)))
#define SYSTEM_BLOCKHDRSZ	(MAXALIGN(sizeof(SystemBlock)))
#define SYSTEM_CHUNKHDRSZ	(sizeof(MemoryChunk))

/*
 * SystemContextIsValid
 *		True iff ctx is a valid SystemContext.
 */
#define SystemContextIsValid(ctx) ((ctx) && IsA(ctx, SystemContext))

/*
 * ChunkGetBlock
 *		Return address of SystemBlock for given MemoryChunk.
 */
#define ChunkGetBlock(chunk) \
	((SystemBlock *) (((char *) chunk) - SYSTEM_BLOCKHDRSZ))

/*
 * BlockGetChunk
 *		Return address of MemoryChunk for given SystemBlock.
 */
#define BlockGetChunk(blk) \
	((MemoryChunk *) (((char *) blk) + SYSTEM_BLOCKHDRSZ))

/*
 * SystemContextCreate
 *		Create a SystemContext memory context
 */
MemoryContext
SystemContextCreate(MemoryContext parent, const char *name)
{
	SystemContext *ctx;

	/*
	 * Allocate the context struct.  Unlike other memory contexts, we don't
	 * add additional space here for the initial block.
	 */
	ctx = (SystemContext *) malloc(SYSTEM_CONTEXTSIZE);
	if (ctx == NULL)
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while creating memory context \"%s\".",
						   name)));
	}

	VALGRIND_CREATE_MEMPOOL(ctx, 0, false);
	VALGRIND_MEMPOOL_ALLOC(ctx, ctx, SYSTEM_CONTEXTSIZE);

	dlist_init(&ctx->allocations);

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) ctx, T_SystemContext, MCTX_SYSTEM_ID,
						parent, name);

	((MemoryContext) ctx)->mem_allocated = SYSTEM_CONTEXTSIZE;

	return (MemoryContext) ctx;
}

/*
 * SystemContextAlloc
 *		Alloc memory into the SystemContext memory context.
 */
void *
SystemContextAlloc(MemoryContext context, size_t size, int flags)
{
	SystemContext *ctx = (SystemContext *) context;
	size_t		blksize;
	SystemBlock *block;
	MemoryChunk *chunk;

	Assert(SystemContextIsValid(ctx));

	/* validate 'size' is within the limits for the given 'flags' */
	MemoryContextCheckSize(context, size, flags);

	/* adjust size for sentinel byte */
#ifdef MEMORY_CONTEXT_CHECKING
	blksize = MAXALIGN(size + 1);
#else
	blksize = MAXALIGN(size);
#endif

	blksize += SYSTEM_BLOCKHDRSZ + SYSTEM_CHUNKHDRSZ;

	block = (SystemBlock *) malloc(blksize);
	if (block == NULL)
		return MemoryContextAllocationFailure(context, size, flags);

	VALGRIND_MEMPOOL_ALLOC(ctx, block, SYSTEM_BLOCKHDRSZ);

	context->mem_allocated += blksize;

	dlist_node_init(&block->node);

	block->context = ctx;
	block->sz = blksize;

	dlist_push_tail(&ctx->allocations, &block->node);

	chunk = BlockGetChunk(block);

	MemoryChunkSetHdrMaskExternal(chunk, MCTX_SYSTEM_ID);

#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	VALGRIND_MAKE_MEM_NOACCESS(chunk, SYSTEM_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * SystemContextFree
 *		Free memory from the SystemContext memory context.
 */
void
SystemContextFree(void *pointer)
{
	SystemContext *ctx;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SystemBlock *block;

	VALGRIND_MAKE_MEM_DEFINED(chunk, SYSTEM_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));

	block = ChunkGetBlock(chunk);

	ctx = block->context;

	Assert(SystemContextIsValid(ctx));

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 ctx->header.name, chunk);
#endif

	/* ok, remove block from the list, and free it */
	dlist_delete_from(&ctx->allocations, &block->node);

	ctx->header.mem_allocated -= block->sz;

#ifdef CLOBBER_FREED_MEMORY
	wipe_mem(block, block->sz);
#endif

	VALGRIND_MEMPOOL_FREE(ctx, block);

	free(block);
}

/*
 * SystemContextRealloc
 *		Realloc memory in the SystemContext memory context.
 */
void *
SystemContextRealloc(void *pointer, size_t size, int flags)
{
	SystemBlock *block,
			   *new_block;
	SystemContext *ctx;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	size_t		new_blksz;

	VALGRIND_MAKE_MEM_DEFINED(chunk, SYSTEM_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));

	block = ChunkGetBlock(chunk);

	ctx = block->context;

	Assert(SystemContextIsValid(ctx));

	/* only check size in paths where the limits could be hit */
	MemoryContextCheckSize((MemoryContext) ctx, size, flags);

#ifdef MEMORY_CONTEXT_CHECKING
	/* Test for someone scribbling on unused space in chunk */
	if (!sentinel_ok(pointer, chunk->requested_size))
		elog(WARNING, "detected write past chunk end in %s %p",
			 ctx->header.name, chunk);
#endif

#ifdef MEMORY_CONTEXT_CHECKING
	/* adjust for sentinel byte, and align */
	new_blksz = MAXALIGN(size + 1);
#else
	new_blksz = MAXALIGN(size);
#endif
	new_blksz += SYSTEM_BLOCKHDRSZ + SYSTEM_CHUNKHDRSZ;

	new_block = realloc(block, new_blksz);
	if (new_block == NULL)
	{
		VALGRIND_MAKE_MEM_NOACCESS(chunk, SYSTEM_CHUNKHDRSZ);
		return MemoryContextAllocationFailure((MemoryContext) ctx, size, flags);
	}

	/*
	 * Temporarily unlink the allocation, because the address of the
	 * allocation may change.
	 */
	dlist_delete_from_thoroughly(&ctx->allocations, &new_block->node);

	/* Move the block-header vchunk */
	VALGRIND_MEMPOOL_CHANGE(ctx, block, new_block, SYSTEM_BLOCKHDRSZ);
	block = new_block;

	dlist_push_tail(&ctx->allocations, &block->node);

	chunk = BlockGetChunk(block);

	/* when enlarging, randomize the enlarged portion */
#ifdef RANDOMIZE_ALLOCATED_MEMORY
	if (new_blksz > block->sz)
		randomize_mem(((char *) block) + block->sz, new_blksz - block->sz);
#endif

	/* update chunk's size data */
#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif

	/* adjust memory accounting for the context */
	ctx->header.mem_allocated -= block->sz;
	ctx->header.mem_allocated += new_blksz;

	/* and adjust the size indicator of the block header */
	block->sz = new_blksz;

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, SYSTEM_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * SystemContextReset
 *		Reset the SystemContext memory context.
 */
void
SystemContextReset(MemoryContext context)
{
	SystemContext *ctx = (SystemContext *) context;

	Assert(SystemContextIsValid(ctx));

	while (!dlist_is_empty(&ctx->allocations))
	{
		SystemBlock *block =
			dlist_container(SystemBlock, node,
							dlist_pop_head_node(&ctx->allocations));
		size_t		blksize = block->sz;

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, blksize);
#endif

		ctx->header.mem_allocated -= blksize;

		/*
		 * We need to free the block header's vchunk explicitly, although the
		 * user-data vchunks within will go away in the TRIM below.  Otherwise
		 * Valgrind complains about leaked allocations.
		 */
		VALGRIND_MEMPOOL_FREE(ctx, block);

		free(block);
	}

	Assert(ctx->header.mem_allocated == SYSTEM_CONTEXTSIZE);
	Assert(SystemContextIsEmpty(context));

	/*
	 * Instruct Valgrind to throw away all the vchunks associated with this
	 * context, except for the one covering the SystemContext.  This gets rid
	 * of the vchunks for whatever user data is getting discarded by the
	 * context reset.
	 */
	VALGRIND_MEMPOOL_TRIM(ctx, ctx, SYSTEM_CONTEXTSIZE);
}

/*
 * SystemContextDelete
 *		Delete this SystemContext memory context.
 */
void
SystemContextDelete(MemoryContext context)
{
	SystemContext *ctx = (SystemContext *) context;

	Assert(SystemContextIsValid(ctx));

#ifdef MEMORY_CONTEXT_CHECKING
	SystemContextCheck(context);
#endif

	SystemContextReset(context);

	VALGRIND_DESTROY_MEMPOOL(context);

	free(ctx);
}

/*
 * SystemContextGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
SystemContextGetChunkContext(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SystemBlock *block;
	SystemContext *ctx;

	VALGRIND_MAKE_MEM_DEFINED(chunk, SYSTEM_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));
	block = ChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_NOACCESS(chunk, SYSTEM_CHUNKHDRSZ);

	ctx = block->context;

	Assert(SystemContextIsValid(ctx));

	return (MemoryContext) ctx;
}

/*
 * SystemContextGetChunkSpace
 *		Given a palloc'd chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
size_t
SystemContextGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	SystemBlock *block;

	VALGRIND_MAKE_MEM_DEFINED(chunk, SYSTEM_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));
	block = ChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_NOACCESS(chunk, SYSTEM_CHUNKHDRSZ);

	return block->sz;
}

/*
 * SystemContextIsEmpty
 *		Is the SystemContext empty of any allocated space?
 */
bool
SystemContextIsEmpty(MemoryContext context)
{
	SystemContext *ctx = (SystemContext *) context;

	Assert(SystemContextIsValid(ctx));

	return dlist_is_empty(&ctx->allocations);
}

/*
 * SystemContextStats
 *		Compute stats about memory consumption of a SystemContext context.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
SystemContextStats(MemoryContext context, MemoryStatsPrintFunc printfunc,
				   void *passthru, MemoryContextCounters *totals,
				   bool print_to_stderr)
{
	SystemContext *ctx = (SystemContext *) context;
	dlist_iter	iter;
	size_t		totalspace = SYSTEM_CONTEXTSIZE;
	size_t		nchunks = 0;

	dlist_foreach(iter, &ctx->allocations)
	{
		SystemBlock *block = dlist_container(SystemBlock, node, iter.cur);

		nchunks++;
		totalspace += block->sz;
	}


	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu chunks",
				 totalspace, nchunks);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

	if (totals)
	{
		totals->nblocks += nchunks;
		totals->totalspace += totalspace;
	}
}

#ifdef MEMORY_CONTEXT_CHECKING
/*
 * SystemContextCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
SystemContextCheck(MemoryContext context)
{
	SystemContext *ctx = (SystemContext *) context;
	const char *name = context->name;
	dlist_iter	iter;
	size_t		total_allocated = SYSTEM_CONTEXTSIZE;

	/* walk all blocks in this context */
	dlist_foreach(iter, &ctx->allocations)
	{
		SystemBlock *block = dlist_container(SystemBlock, node, iter.cur);
		MemoryChunk *chunk = BlockGetChunk(block);
		void	   *pointer = MemoryChunkGetPointer(chunk);
		size_t		expected_sz;

		/* Allow access to the chunk header. */
		VALGRIND_MAKE_MEM_DEFINED(chunk, SYSTEM_CHUNKHDRSZ);

		total_allocated += block->sz;
		expected_sz = MAXALIGN(chunk->requested_size + 1) + SYSTEM_BLOCKHDRSZ
			+ SYSTEM_CHUNKHDRSZ;

		if (block->sz != expected_sz)
			elog(WARNING, "problem in SystemContext %s: chunk %p has an unexpected size; block->sz is %zu but expected size is %zu bytes",
				 name, chunk, block->sz, expected_sz);

		if (!sentinel_ok(pointer, chunk->requested_size))
			elog(WARNING, "problem in SystemContext %s: detected write past chunk end in block %p, chunk %p",
				 name, block, chunk);

		VALGRIND_MAKE_MEM_NOACCESS(chunk, SYSTEM_CHUNKHDRSZ);
	}

	Assert(total_allocated == context->mem_allocated);

	if (total_allocated != ctx->header.mem_allocated)
		elog(WARNING, "problem in SystemContext %s: sum of memory %zu does not match header's %zu",
			 name, total_allocated, ctx->header.mem_allocated);
}
#endif
