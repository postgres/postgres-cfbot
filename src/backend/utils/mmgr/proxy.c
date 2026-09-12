/*-------------------------------------------------------------------------
*
 * proxy.c
 *	  Proxy allocator definitions.
 *
 * Proxy is a MemoryContext implementation designed for memory usages which
 * require their own memory context, but which generally have few allocations
 * that generally have a very long lifetime.  Compared to ASet, every
 * allocation of a Proxy memory context gets an External chunk, i.e. every
 * palloc results in a malloc.
 *
 * Portions Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/proxy.c
 *
 * NOTE:
 *	Proxy is best suited to cases where a memory context with long lifetimes
 *	is required, but is expected to hold only a small number of small
 *	allocations, with a total memory usage smaller than the minimum aset block
 *	size.
 *
 *	Allocations are MAXALIGNed.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_memorychunk.h"
#include "utils/memutils_internal.h"

typedef struct ProxyContext
{
	MemoryContextData header;	/* Standard memory-context fields */

	dlist_head	allocations;	/* list of all allocations of this memctx */
} ProxyContext;

typedef struct ProxyBlock
{
	dlist_node	node;
	size_t		sz;
	ProxyContext *context;
} ProxyBlock;

#define PROXY_CONTEXTSIZE	(MAXALIGN(sizeof(ProxyContext)))
#define PROXY_BLOCKHDRSZ	(MAXALIGN(sizeof(ProxyBlock)))
#define PROXY_CHUNKHDRSZ	(sizeof(MemoryChunk))

/*
 * ProxyIsValid
 *		True iff ctx is valid allocation ctx.
 */
#define ProxyIsValid(ctx) \
	((ctx) && IsA(ctx, ProxyContext))

#define ExternalChunkGetBlock(chunk) \
	((ProxyBlock *) (((char *) chunk) - PROXY_BLOCKHDRSZ))


/*
 * ProxyContextCreate
 *		Create a Proxy memory context
 */
MemoryContext
ProxyContextCreate(MemoryContext parent, const char *name)
{
	size_t		allocSize = PROXY_CONTEXTSIZE;
	ProxyContext *ctx;

	/*
	 * Allocate the context struct.  Unlike other memory contexts, this
	 * context doesn't have an attached chunk of palloc'able memory.
	 */
	ctx = (ProxyContext *) malloc(allocSize);
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
	VALGRIND_MEMPOOL_ALLOC(ctx, ctx, allocSize);

	dlist_init(&ctx->allocations);

	/* Finally, do the type-independent part of context creation */
	MemoryContextCreate((MemoryContext) ctx, T_ProxyContext, MCTX_PROXY_ID,
						parent, name);

	((MemoryContext) ctx)->mem_allocated = allocSize;

	return (MemoryContext) ctx;
}

/*
 * ProxyAlloc
 *		Alloc memory into the Proxy memory context.
 */
void *
ProxyAlloc(MemoryContext context, size_t size, int flags)
{
	ProxyContext *ctx = (ProxyContext *) context;
	size_t		blksize;
	ProxyBlock *block;
	MemoryChunk *chunk;

	Assert(ProxyIsValid(ctx));

	/* validate 'size' is within the limits for the given 'flags' */
	MemoryContextCheckSize(context, size, flags);

	/* adjust size for sentinel byte */
#ifdef MEMORY_CONTEXT_CHECKING
	blksize = MAXALIGN(size + 1);
#else
	blksize = MAXALIGN(size);
#endif

	blksize += PROXY_BLOCKHDRSZ + PROXY_CHUNKHDRSZ;

	block = (ProxyBlock *) malloc(blksize);
	if (block == NULL)
		return MemoryContextAllocationFailure(context, size, flags);

	VALGRIND_MEMPOOL_ALLOC(ctx, block, PROXY_BLOCKHDRSZ);

	context->mem_allocated += blksize;

	dlist_node_init(&block->node);

	block->context = ctx;
	block->sz = blksize;

	dlist_push_tail(&ctx->allocations, &block->node);

	chunk = (MemoryChunk *) (((char *) block) + PROXY_BLOCKHDRSZ);

	MemoryChunkSetHdrMaskExternal(chunk, MCTX_PROXY_ID);

#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	/* set mark to catch clobber of "unused" space */
	set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif

#ifdef RANDOMIZE_ALLOCATED_MEMORY
	/* fill the allocated space with junk */
	randomize_mem((char *) MemoryChunkGetPointer(chunk), size);
#endif

	VALGRIND_MAKE_MEM_NOACCESS(chunk, PROXY_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * ProxyAlloc
 *		Free memory from the Proxy memory context.
 */
void
ProxyFree(void *pointer)
{
	ProxyContext *ctx;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyBlock *block;

	VALGRIND_MAKE_MEM_DEFINED(chunk, PROXY_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));

	block = ExternalChunkGetBlock(chunk);

	ctx = block->context;

	Assert(ProxyIsValid(ctx));

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
 * ProxyAlloc
 *		Realloc memory in the Proxy memory context.
 */
void *
ProxyRealloc(void *pointer, size_t size, int flags)
{
	ProxyBlock *block,
			   *new_block;
	ProxyContext *ctx;
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	size_t		blksize;

	VALGRIND_MAKE_MEM_DEFINED(chunk, PROXY_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));

	block = ExternalChunkGetBlock(chunk);

	ctx = block->context;

	Assert(ProxyIsValid(ctx));

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
	blksize = MAXALIGN(size + 1);
#else
	blksize = MAXALIGN(size);
#endif
	blksize += PROXY_BLOCKHDRSZ + PROXY_CHUNKHDRSZ;

	/*
	 * Temporarily unlink the allocation, because the address of the
	 * allocation may change.
	 */
	dlist_delete_from_thoroughly(&ctx->allocations, &block->node);

	new_block = realloc(block, blksize);
	if (new_block == NULL)
	{
		VALGRIND_MAKE_MEM_NOACCESS(chunk, PROXY_CHUNKHDRSZ);
		return MemoryContextAllocationFailure((MemoryContext) ctx, size, flags);
	}

	/* Move the block-header vchunk */
	VALGRIND_MEMPOOL_CHANGE(ctx, block, new_block, PROXY_BLOCKHDRSZ);
	block = new_block;

	dlist_push_tail(&ctx->allocations, &block->node);

	chunk = (MemoryChunk *) (((char *) block) + PROXY_BLOCKHDRSZ);

	/* randomize the newly allocated memory */
#ifdef RANDOMIZE_ALLOCATED_MEMORY
	if (block->sz < blksize)
		randomize_mem(((char *) block) + block->sz, blksize - block->sz);
#endif

	/* update chunk's size data */
#ifdef MEMORY_CONTEXT_CHECKING
	chunk->requested_size = size;
	set_sentinel(MemoryChunkGetPointer(chunk), size);
#endif

	/* and adjust the size indicator of the block header */
	block->sz = blksize;

	/* Disallow access to the chunk header. */
	VALGRIND_MAKE_MEM_NOACCESS(chunk, PROXY_CHUNKHDRSZ);

	return MemoryChunkGetPointer(chunk);
}

/*
 * ProxyReset
 *		Reset the Proxy memory context.
 */
void
ProxyReset(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;

	Assert(ProxyIsValid(ctx));

	while (!dlist_is_empty(&ctx->allocations))
	{
		ProxyBlock *block =
			dlist_container(ProxyBlock, node,
							dlist_pop_head_node(&ctx->allocations));
		size_t		blksize = block->sz;

#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(block, blksize);
#endif

		ctx->header.mem_allocated -= blksize;

		/*
		 * We need to free the block header's vchunk explicitly, although
		 * the user-data vchunks within will go away in the TRIM below.
		 * Otherwise Valgrind complains about leaked allocations.
		 */
		VALGRIND_MEMPOOL_FREE(ctx, block);

		free(block);
	}

	Assert(ctx->header.mem_allocated == sizeof(ProxyContext));
	Assert(ProxyIsEmpty(context));

	/*
	 * Instruct Valgrind to throw away all the vchunks associated with this
	 * context, except for the one covering the ProxyContext.  This gets rid
	 * of the vchunks for whatever user data is getting discarded by the
	 * context reset.
	 */
	VALGRIND_MEMPOOL_TRIM(ctx, ctx, PROXY_CONTEXTSIZE);
}

/*
 * ProxyAlloc
 *		Delete this Proxy memory context.
 */
void
ProxyDelete(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;

	Assert(ProxyIsValid(ctx));

#ifdef MEMORY_CONTEXT_CHECKING
	ProxyCheck(context);
#endif

	ProxyReset(context);

	VALGRIND_DESTROY_MEMPOOL(context);

	free(ctx);
}

/*
 * ProxyGetChunkContext
 *		Return the MemoryContext that 'pointer' belongs to.
 */
MemoryContext
ProxyGetChunkContext(void *pointer)
{
	MemoryChunk	   *chunk = PointerGetMemoryChunk(pointer);
	ProxyBlock	   *block;
	ProxyContext   *ctx;

	VALGRIND_MAKE_MEM_DEFINED(chunk, PROXY_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));
	block = ExternalChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_NOACCESS(chunk, PROXY_CHUNKHDRSZ);

	ctx = block->context;

	Assert(ProxyIsValid(ctx));

	return (MemoryContext) ctx;
}

/*
 * ProxyGetChunkSpace
*		Given a palloc'd chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */
size_t
ProxyGetChunkSpace(void *pointer)
{
	MemoryChunk *chunk = PointerGetMemoryChunk(pointer);
	ProxyBlock *block;

	VALGRIND_MAKE_MEM_DEFINED(chunk, PROXY_CHUNKHDRSZ);

	Assert(MemoryChunkIsExternal(chunk));
	block = ExternalChunkGetBlock(chunk);

	VALGRIND_MAKE_MEM_NOACCESS(chunk, PROXY_CHUNKHDRSZ);

	return block->sz;
}

/*
 * ProxyIsEmpty
*		Is the ProxyContext empty of any allocated space?
 */
bool
ProxyIsEmpty(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;

	Assert(ProxyIsValid(ctx));

	return dlist_is_empty(&ctx->allocations);
}

/*
 * ProxyStats
 *		Compute stats about memory consumption of a Proxy context.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */
void
ProxyStats(MemoryContext context, MemoryStatsPrintFunc printfunc,
		   void *passthru, MemoryContextCounters *totals,
		   bool print_to_stderr)
{
	ProxyContext *ctx = (ProxyContext *) context;
	dlist_iter	iter;
	size_t		totalspace;
	size_t		nchunks = 0;

	totalspace = MAXALIGN(sizeof(ProxyContext));

	dlist_foreach(iter, &ctx->allocations)
	{
		ProxyBlock *block = dlist_container(ProxyBlock, node, iter.cur);

		nchunks++;
		totalspace += block->sz;
	}


	if (printfunc)
	{
		char		stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu chunks;",
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
 * ProxyCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */
void
ProxyCheck(MemoryContext context)
{
	ProxyContext *ctx = (ProxyContext *) context;
	const char *name = context->name;
	dlist_iter	iter;
	size_t		total_allocated = sizeof(ProxyContext);

	/* walk all blocks in this context */
	dlist_foreach(iter, &ctx->allocations)
	{
		ProxyBlock *block = dlist_container(ProxyBlock, node, iter.cur);
		MemoryChunk *chunk = (MemoryChunk *) ((char *) block + PROXY_BLOCKHDRSZ);

		total_allocated += block->sz;

		if (block->sz != MAXALIGN(chunk->requested_size + 1) + PROXY_BLOCKHDRSZ + PROXY_CHUNKHDRSZ)
		{
			elog(WARNING, "problem in Proxy %s: bad single-chunk %p in block %p",
				 name, chunk, block);
		}

		if (!sentinel_ok(chunk, chunk->requested_size + PROXY_CHUNKHDRSZ))
		{
			elog(WARNING, "problem in Proxy %s: detected write past chunk end in block %p, chunk %p",
				 name, block, chunk);
		}
	}

	Assert(total_allocated == context->mem_allocated);

	if (total_allocated != ctx->header.mem_allocated)
	{
		elog(WARNING, "problem in Proxy %s: sum of memory %zd does not match header's %zd",
			 name, total_allocated, ctx->header.mem_allocated);
	}
}
#endif
