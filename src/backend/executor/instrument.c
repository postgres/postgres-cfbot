/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/executor/instrument.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "portability/instr_time.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

WalUsage	pgWalUsage;
Instrumentation instr_top;
InstrStackState instr_stack = {
	.stack_space = 0,
	.stack_size = 0,
	.entries = NULL,
	.current = &instr_top,
};

static void BufferUsageAdd(BufferUsage *dst, const BufferUsage *add);
static void WalUsageAdd(WalUsage *dst, const WalUsage *add);

void
InstrStackGrow(void)
{
	int			space = instr_stack.stack_space;

	Assert(instr_stack.stack_size >= instr_stack.stack_space);

	if (instr_stack.entries == NULL)
	{
		space = 10;				/* Allocate sufficient initial space for
								 * typical activity */
		instr_stack.entries = MemoryContextAlloc(TopMemoryContext,
												 sizeof(Instrumentation *) * space);
	}
	else
	{
		space *= 2;
		instr_stack.entries = repalloc_array(instr_stack.entries, Instrumentation *, space);
	}

	/* Update stack space after allocation succeeded to protect against OOMs */
	instr_stack.stack_space = space;
}

/* General purpose instrumentation handling */
static inline bool
InstrNeedStack(int instrument_options)
{
	return (instrument_options & (INSTRUMENT_BUFFERS | INSTRUMENT_WAL)) != 0;
}

void
InstrInitOptions(Instrumentation *instr, int instrument_options)
{
	instr->need_stack = InstrNeedStack(instrument_options);
	instr->need_timer = (instrument_options & INSTRUMENT_TIMER) != 0;
}

static inline void
InstrStartTimer(Instrumentation *instr)
{
	if (!INSTR_TIME_IS_ZERO(instr->starttime))
		elog(ERROR, "InstrStart called twice in a row");

	INSTR_TIME_SET_CURRENT_FAST(instr->starttime);
}

static inline void
InstrStopTimer(Instrumentation *instr, instr_time *accum_time)
{
	instr_time	endtime;

	if (INSTR_TIME_IS_ZERO(instr->starttime))
		elog(ERROR, "InstrStop called without start");

	INSTR_TIME_SET_CURRENT_FAST(endtime);
	INSTR_TIME_ACCUM_DIFF(*accum_time, endtime, instr->starttime);

	INSTR_TIME_SET_ZERO(instr->starttime);
}

/*
 * Helper for InstrStop() and InstrStopNode(), to avoid code duplication
 * despite slightly different needs about how time is accumulated.
 */
static inline void
InstrStopCommon(Instrumentation *instr, instr_time *accum_time)
{
	/* update the time only if the timer was requested */
	if (instr->need_timer)
		InstrStopTimer(instr, accum_time);

	/* pop the stack, unless InstrStopFinalize previously cleaned up */
	if (instr->on_stack)
		InstrPopStack(instr);
}

void
InstrStart(Instrumentation *instr)
{
	if (instr->need_timer)
		InstrStartTimer(instr);

	if (instr->need_stack)
		InstrPushStack(instr);
}

void
InstrStop(Instrumentation *instr)
{
	InstrStopCommon(instr, &instr->total);
}

/*
 * Workhorse for InstrStopFinalize and the abort cleanup.
 *
 * If 'allow_stopped' is set, the entry may already have been stopped (as
 * happens for a QueryInstrumentation between two InstrQueryStart/InstrQueryStop
 * pairs, e.g. between fetches from a cursor), in which case there is no timer
 * to stop and only the accumulation to the parent remains.
 */
static void
InstrStopFinalizeInternal(Instrumentation *instr, bool allow_stopped)
{
	/*
	 * If our current node is on the stack, make sure we reset the stack to
	 * the parent of whichever of the released stack entries has the lowest
	 * index
	 */
	if (instr->on_stack)
	{
		int			idx = -1;

		for (int i = instr_stack.stack_size - 1; i >= 0; i--)
		{
			if (instr_stack.entries[i] == instr)
			{
				idx = i;
				break;
			}
		}

		if (idx < 0)
			elog(ERROR, "instrumentation entry not found on stack");

		/* Clear on_stack for any intermediate entries we're skipping over */
		for (int i = instr_stack.stack_size - 1; i > idx; i--)
			instr_stack.entries[i]->on_stack = false;

		while (instr_stack.stack_size > idx + 1)
			instr_stack.stack_size--;
	}

	if (!(allow_stopped && instr->need_timer &&
		  INSTR_TIME_IS_ZERO(instr->starttime)))
		InstrStop(instr);

	/*
	 * Accumulate all instrumentation to the currently active instrumentation,
	 * so that callers get a complete picture of activity, even after an abort
	 */
	InstrAccumStack(instr_stack.current, instr);
}

/*
 * Stops instrumentation, finalizes the stack entry and accumulates to its parent.
 *
 * Note that this intentionally allows passing a stack that is not the current
 * top, as can happen with PG_FINALLY, or resource owners, which don't have a
 * guaranteed cleanup order.
 */
void
InstrStopFinalize(Instrumentation *instr)
{
	InstrStopFinalizeInternal(instr, false);
}

/*
 * Finalize a child entry registered with InstrQueryRememberChild, by
 * accumulating its buffer/WAL usage to the provided instrumentation, which may
 * be the current entry, or one the caller treats as a parent and will add to
 * the totals later.
 *
 * The entry is removed from its parent's unfinalized list, so that the abort
 * handling does not count it again, and marked as finalized: later calls are
 * no-ops.  The latter matters for the plan tree walkers in execProcnode.c,
 * since planstate_tree_walker can reach the same PlanState more than once (a
 * SubPlan appearing in several expressions of a plan node gets one
 * SubPlanState per expression, all pointing at the same subplan PlanState).
 *
 * For entries that are not registered, e.g. copies of other processes'
 * instrumentation, use InstrAccumStack instead.
 */
void
InstrFinalizeChild(Instrumentation *instr, Instrumentation *parent)
{
	if (!instr->need_stack || instr->finalized)
		return;

	Assert(!dlist_node_is_detached(&instr->unfinalized_entry));
	dlist_delete_thoroughly(&instr->unfinalized_entry);

	InstrAccumStack(parent, instr);
	instr->finalized = true;
}


/* Query instrumentation handling */

/*
 * Use ResourceOwner mechanism to correctly reset instr_stack on abort.
 */
static void ResOwnerReleaseInstrumentation(Datum res);
static const ResourceOwnerDesc instrumentation_resowner_desc =
{
	.name = "instrumentation",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_INSTRUMENTATION,
	.ReleaseResource = ResOwnerReleaseInstrumentation,
	.DebugPrint = NULL,			/* default message is fine */
};

static inline void
ResourceOwnerRememberInstrumentation(ResourceOwner owner, QueryInstrumentation *qinstr)
{
	ResourceOwnerRemember(owner, PointerGetDatum(qinstr), &instrumentation_resowner_desc);
}

static inline void
ResourceOwnerForgetInstrumentation(ResourceOwner owner, QueryInstrumentation *qinstr)
{
	ResourceOwnerForget(owner, PointerGetDatum(qinstr), &instrumentation_resowner_desc);
}

static void
ResOwnerReleaseInstrumentation(Datum res)
{
	QueryInstrumentation *qinstr = (QueryInstrumentation *) DatumGetPointer(res);
	MemoryContext instr_cxt = qinstr->instr_cxt;
	dlist_mutable_iter iter;

	/* Accumulate data from all unfinalized child entries (nodes, triggers) */
	dlist_foreach_modify(iter, &qinstr->unfinalized_entries)
	{
		Instrumentation *child = dlist_container(Instrumentation, unfinalized_entry, iter.cur);

		InstrAccumStack(&qinstr->instr, child);
	}

	/*
	 * Ensure the stack is reset as expected, and we accumulate to the parent.
	 * The entry stays registered between InstrQueryStop and the next
	 * InstrQueryStart, so it may well not be running at this point.
	 */
	InstrStopFinalizeInternal(&qinstr->instr, true);

	/*
	 * Destroy the dedicated instrumentation context, which frees the
	 * QueryInstrumentation and all child allocations.
	 */
	MemoryContextDelete(instr_cxt);
}

QueryInstrumentation *
InstrQueryAlloc(int instrument_options)
{
	QueryInstrumentation *instr;
	MemoryContext instr_cxt;

	/*
	 * When the instrumentation stack is used, create a dedicated memory
	 * context for this query's instrumentation allocations.  It starts out as
	 * a child of the current memory context, so that it is freed along with
	 * the caller's state if the caller is abandoned before InstrQueryStart,
	 * and is moved under TopMemoryContext while a ResourceOwner is
	 * responsible for it (see InstrQueryStart), since the abort cleanup needs
	 * to access it after the caller's memory may already be gone.
	 *
	 * For simpler cases (timer/rows only), use the current memory context.
	 *
	 * All child instrumentation allocations (nodes, triggers, etc) must be
	 * allocated within this context to ensure correct clean up on abort.
	 */
	if (InstrNeedStack(instrument_options))
		instr_cxt = AllocSetContextCreate(CurrentMemoryContext,
										  "Instrumentation",
										  ALLOCSET_SMALL_SIZES);
	else
		instr_cxt = CurrentMemoryContext;

	instr = MemoryContextAllocZero(instr_cxt, sizeof(QueryInstrumentation));
	instr->instrument_options = instrument_options;
	instr->instr_cxt = instr_cxt;

	InstrInitOptions(&instr->instr, instrument_options);
	dlist_init(&instr->unfinalized_entries);

	return instr;
}

void
InstrQueryStart(QueryInstrumentation *qinstr)
{
	/*
	 * On the first start, hand the entry over to the current ResourceOwner,
	 * which stays responsible for it until InstrQueryStopFinalize (or abort).
	 * It is intentionally not released at InstrQueryStop, so that an abort
	 * between two start/stop pairs (e.g. between fetches from a cursor) still
	 * accumulates what was measured so far.
	 */
	if (qinstr->instr.need_stack && qinstr->owner == NULL)
	{
		Assert(CurrentResourceOwner != NULL);

		/*
		 * Enlarge first, as that is the only step that can fail; once the
		 * context is under TopMemoryContext we must be sure to register it.
		 */
		ResourceOwnerEnlarge(CurrentResourceOwner);
		MemoryContextSetParent(qinstr->instr_cxt, TopMemoryContext);

		qinstr->owner = CurrentResourceOwner;
		ResourceOwnerRememberInstrumentation(qinstr->owner, qinstr);
	}

	InstrStart(&qinstr->instr);
}

void
InstrQueryStop(QueryInstrumentation *qinstr)
{
	InstrStop(&qinstr->instr);
}

void
InstrQueryStopFinalize(QueryInstrumentation *qinstr)
{
	InstrStopFinalize(&qinstr->instr);

	if (!qinstr->instr.need_stack)
	{
		Assert(qinstr->owner == NULL);
		return;
	}

	Assert(qinstr->owner != NULL);
	ResourceOwnerForgetInstrumentation(qinstr->owner, qinstr);
	qinstr->owner = NULL;

	/*
	 * Reparent the dedicated instrumentation context under the current memory
	 * context, so that its lifetime is now tied to the caller's context
	 * rather than TopMemoryContext.
	 */
	MemoryContextSetParent(qinstr->instr_cxt, CurrentMemoryContext);
}

/*
 * Register a child Instrumentation entry for abort processing.
 *
 * On abort, ResOwnerReleaseInstrumentation will walk the parent's list to
 * recover buffer/WAL data from entries that were never finalized, in order for
 * aggregate totals to be accurate despite the query erroring out.
 */
void
InstrQueryRememberChild(QueryInstrumentation *parent, Instrumentation *child)
{
	if (child->need_stack)
		dlist_push_head(&parent->unfinalized_entries, &child->unfinalized_entry);
}

/* start instrumentation during parallel worker startup */
QueryInstrumentation *
InstrStartParallelQuery(void)
{
	QueryInstrumentation *qinstr = InstrQueryAlloc(INSTRUMENT_BUFFERS | INSTRUMENT_WAL);

	InstrQueryStart(qinstr);
	return qinstr;
}

/*
 * Report usage after parallel worker shutdown, to be handed to the leader
 * (which accumulates it into its own stack, see InstrAccumParallelQuery).
 */
void
InstrEndParallelQuery(QueryInstrumentation *qinstr, BufferUsage *bufusage, WalUsage *walusage)
{
	InstrQueryStopFinalize(qinstr);
	memcpy(bufusage, &qinstr->instr.bufusage, sizeof(BufferUsage));
	memcpy(walusage, &qinstr->instr.walusage, sizeof(WalUsage));
}

/*
 * Record WAL usage imported from a parallel worker.
 */
void
InstrAccumWorkerUsage(const BufferUsage *bufusage, const WalUsage *walusage)
{
	WalUsageAdd(&pgWalUsage, walusage);
}

/*
 * Accumulate work done by parallel workers in the leader's stats.
 *
 * Note that what gets added here effectively depends on whether per-node
 * instrumentation is active. If it's active the parallel worker intentionally
 * does not roll up its nodes into the query-level stats, because the leader
 * aggregates the per-node stats separately (see ExecutorFinish). Instead, this
 * only accumulates any extra activity outside of nodes.
 *
 * Otherwise this is responsible for making sure that the complete query
 * activity is accumulated.
 */
void
InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage)
{
	InstrAccumWorkerUsage(bufusage, walusage);

	/*
	 * Unlike ExecParallelRetrieveInstrumentation, which merges worker usage
	 * into the leader's own node entries (and thus into the stack via
	 * finalization), there is no node to attribute query-level worker usage
	 * to, so add it to the current stack entry directly.
	 */
	BufferUsageAdd(&instr_stack.current->bufusage, bufusage);
	WalUsageAdd(&instr_stack.current->walusage, walusage);
}

/* Node instrumentation handling */

/* Allocate new node instrumentation structure */
NodeInstrumentation *
InstrAllocNode(QueryInstrumentation *qinstr, int instrument_options,
			   bool async_mode)
{
	NodeInstrumentation *instr = MemoryContextAlloc(qinstr->instr_cxt, sizeof(NodeInstrumentation));

	InstrInitNode(instr, instrument_options, async_mode);

	InstrQueryRememberChild(qinstr, &instr->instr);

	return instr;
}

/* Initialize a pre-allocated instrumentation structure. */
void
InstrInitNode(NodeInstrumentation *instr, int instrument_options, bool async_mode)
{
	memset(instr, 0, sizeof(NodeInstrumentation));
	InstrInitOptions(&instr->instr, instrument_options);
	instr->async_mode = async_mode;
}

/* Entry to a plan node. If you modify this, check InstrNodeSetupExecProcNode. */
void
InstrStartNode(NodeInstrumentation *instr)
{
	InstrStart(&instr->instr);
}

/*
 * Count the tuples returned by a plan node call, and remember the time it
 * took to get to the first tuple of this cycle (which InstrEndLoop turns into
 * the node's startup time).  Must be called after the node's timer has been
 * stopped for this call.
 *
 * Shared by InstrStopNode and the specialized ExecProcNode wrappers below,
 * so that both account in exactly the same way.
 */
static inline void
InstrCountNodeTuples(NodeInstrumentation *instr, double nTuples)
{
	double		save_tuplecount = instr->tuplecount;

	/* count the returned tuples */
	instr->tuplecount += nTuples;

	/* Is this the first tuple of this cycle? */
	if (!instr->running)
	{
		instr->running = true;
		instr->firsttuple = instr->counter;
	}
	else
	{
		/*
		 * In async mode, if the plan node hadn't emitted any tuples before,
		 * this might be the first tuple
		 */
		if (instr->async_mode && save_tuplecount < 1.0)
			instr->firsttuple = instr->counter;
	}
}

/* Exit from a plan node */
void
InstrStopNode(NodeInstrumentation *instr, double nTuples)
{
	/*
	 * Note that in contrast to InstrStop() the time is accumulated into
	 * NodeInstrumentation->counter, with total only getting updated in
	 * InstrEndLoop.  We need the separate counter variable because we need to
	 * calculate start-up time for the first tuple in each cycle, and then
	 * accumulate it together.
	 */
	InstrStopCommon(&instr->instr, &instr->counter);

	InstrCountNodeTuples(instr, nTuples);
}

/*
 * ExecProcNode wrappers that perform instrumentation calls.  By keeping
 * them in separate functions, we avoid overhead in the normal case where
 * no instrumentation is wanted.
 *
 * These functions are equivalent to running ExecProcNodeReal wrapped in
 * InstrStartNode and InstrStopNode, but avoid the conditionals in the hot path
 * by checking the instrumentation options when the ExecProcNode pointer gets
 * first set, and then using a special-purpose function for each. This results
 * in a more optimized set of compiled instructions.
 *
 * This is implemented in instrument.c as all the functions it calls directly
 * are here, allowing them to be inlined even when not using LTO.
 */

/* Simplified pop: restore saved state instead of re-deriving from array */
static inline void
InstrPopStackTo(Instrumentation *prev)
{
	Assert(instr_stack.stack_size > 0);
	Assert(instr_stack.stack_size > 1 ? instr_stack.entries[instr_stack.stack_size - 2] == prev : &instr_top == prev);
	instr_stack.entries[instr_stack.stack_size - 1]->on_stack = false;
	instr_stack.stack_size--;
	instr_stack.current = prev;
}

static pg_always_inline TupleTableSlot *
ExecProcNodeInstr(PlanState *node, bool need_timer, bool need_stack)
{
	NodeInstrumentation *instr = node->instrument;
	Instrumentation *prev = instr_stack.current;
	TupleTableSlot *result;

	if (need_stack)
		InstrPushStack(&instr->instr);
	if (need_timer)
		InstrStartTimer(&instr->instr);

	result = node->ExecProcNodeReal(node);

	if (need_timer)
		InstrStopTimer(&instr->instr, &instr->counter);
	if (need_stack)
		InstrPopStackTo(prev);

	InstrCountNodeTuples(instr, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}

static TupleTableSlot *
ExecProcNodeInstrFull(PlanState *node)
{
	return ExecProcNodeInstr(node, true, true);
}

static TupleTableSlot *
ExecProcNodeInstrRowsStackOnly(PlanState *node)
{
	return ExecProcNodeInstr(node, false, true);
}

static TupleTableSlot *
ExecProcNodeInstrRowsTimerOnly(PlanState *node)
{
	return ExecProcNodeInstr(node, true, false);
}

static TupleTableSlot *
ExecProcNodeInstrRowsOnly(PlanState *node)
{
	return ExecProcNodeInstr(node, false, false);
}

/*
 * Returns an ExecProcNode wrapper that performs instrumentation calls,
 * tailored to the instrumentation options enabled for the node.
 */
ExecProcNodeMtd
InstrNodeSetupExecProcNode(NodeInstrumentation *instr)
{
	bool		need_timer = instr->instr.need_timer;
	bool		need_stack = instr->instr.need_stack;

	if (need_timer && need_stack)
		return ExecProcNodeInstrFull;
	else if (need_stack)
		return ExecProcNodeInstrRowsStackOnly;
	else if (need_timer)
		return ExecProcNodeInstrRowsTimerOnly;
	else
		return ExecProcNodeInstrRowsOnly;
}

/* Update tuple count */
void
InstrUpdateTupleCount(NodeInstrumentation *instr, double nTuples)
{
	/* count the returned tuples */
	instr->tuplecount += nTuples;
}

/* Finish a run cycle for a plan node */
void
InstrEndLoop(NodeInstrumentation *instr)
{
	/* Skip if nothing has happened, or already shut down */
	if (!instr->running)
		return;

	if (!INSTR_TIME_IS_ZERO(instr->instr.starttime))
		elog(ERROR, "InstrEndLoop called on running node");

	/* Accumulate per-cycle statistics into totals */
	INSTR_TIME_ADD(instr->startup, instr->firsttuple);
	INSTR_TIME_ADD(instr->instr.total, instr->counter);
	instr->ntuples += instr->tuplecount;
	instr->nloops += 1;

	/* Reset for next cycle (if any) */
	instr->running = false;
	INSTR_TIME_SET_ZERO(instr->instr.starttime);
	INSTR_TIME_SET_ZERO(instr->counter);
	INSTR_TIME_SET_ZERO(instr->firsttuple);
	instr->tuplecount = 0;
}

/*
 * Aggregate instrumentation from parallel workers. Must be called after
 * InstrEndLoop.
 */
void
InstrAggNode(NodeInstrumentation *dst, NodeInstrumentation *add)
{
	Assert(!add->running);

	INSTR_TIME_ADD(dst->startup, add->startup);
	INSTR_TIME_ADD(dst->instr.total, add->instr.total);
	dst->ntuples += add->ntuples;
	dst->ntuples2 += add->ntuples2;
	dst->nloops += add->nloops;
	dst->nfiltered1 += add->nfiltered1;
	dst->nfiltered2 += add->nfiltered2;

	if (dst->instr.need_stack)
	{
		Assert(!dst->instr.finalized);
		InstrAccumStack(&dst->instr, &add->instr);
	}
}

/* Trigger instrumentation handling */
TriggerInstrumentation *
InstrAllocTrigger(QueryInstrumentation *qinstr, int instrument_options, int n)
{
	TriggerInstrumentation *tginstr;
	int			i;

	/*
	 * Allocate in the query's dedicated instrumentation context so all
	 * instrumentation data is grouped together and cleaned up as a unit.
	 */
	Assert(qinstr != NULL && qinstr->instr_cxt != NULL);
	tginstr = MemoryContextAllocZero(qinstr->instr_cxt,
									 n * sizeof(TriggerInstrumentation));

	for (i = 0; i < n; i++)
	{
		InstrInitOptions(&tginstr[i].instr, instrument_options);
		InstrQueryRememberChild(qinstr, &tginstr[i].instr);
	}

	return tginstr;
}

void
InstrStartTrigger(TriggerInstrumentation *tginstr)
{
	InstrStart(&tginstr->instr);
}

void
InstrStopTrigger(TriggerInstrumentation *tginstr, int64 firings)
{
	/*
	 * This trigger may be called again, so we don't finalize instrumentation
	 * here. Accumulation to the parent happens at ExecutorFinish through
	 * ExecFinalizeTriggerInstrumentation.
	 */
	InstrStop(&tginstr->instr);
	tginstr->firings += firings;
}

void
InstrAccumStack(Instrumentation *dst, Instrumentation *add)
{
	Assert(dst != NULL);
	Assert(add != NULL);

	if (!add->need_stack)
		return;

	BufferUsageAdd(&dst->bufusage, &add->bufusage);
	WalUsageAdd(&dst->walusage, &add->walusage);
}

/* dst += add */
static void
BufferUsageAdd(BufferUsage *dst, const BufferUsage *add)
{
	dst->shared_blks_hit += add->shared_blks_hit;
	dst->shared_blks_read += add->shared_blks_read;
	dst->shared_blks_dirtied += add->shared_blks_dirtied;
	dst->shared_blks_written += add->shared_blks_written;
	dst->local_blks_hit += add->local_blks_hit;
	dst->local_blks_read += add->local_blks_read;
	dst->local_blks_dirtied += add->local_blks_dirtied;
	dst->local_blks_written += add->local_blks_written;
	dst->temp_blks_read += add->temp_blks_read;
	dst->temp_blks_written += add->temp_blks_written;
	INSTR_TIME_ADD(dst->shared_blk_read_time, add->shared_blk_read_time);
	INSTR_TIME_ADD(dst->shared_blk_write_time, add->shared_blk_write_time);
	INSTR_TIME_ADD(dst->local_blk_read_time, add->local_blk_read_time);
	INSTR_TIME_ADD(dst->local_blk_write_time, add->local_blk_write_time);
	INSTR_TIME_ADD(dst->temp_blk_read_time, add->temp_blk_read_time);
	INSTR_TIME_ADD(dst->temp_blk_write_time, add->temp_blk_write_time);
}

/* helper functions for WAL usage accumulation */
static inline void
WalUsageAdd(WalUsage *dst, const WalUsage *add)
{
	dst->wal_bytes += add->wal_bytes;
	dst->wal_records += add->wal_records;
	dst->wal_fpi += add->wal_fpi;
	dst->wal_fpi_bytes += add->wal_fpi_bytes;
	dst->wal_buffers_full += add->wal_buffers_full;
}

inline void
WalUsageAccumDiff(WalUsage *dst, const WalUsage *add, const WalUsage *sub)
{
	dst->wal_bytes += add->wal_bytes - sub->wal_bytes;
	dst->wal_records += add->wal_records - sub->wal_records;
	dst->wal_fpi += add->wal_fpi - sub->wal_fpi;
	dst->wal_fpi_bytes += add->wal_fpi_bytes - sub->wal_fpi_bytes;
	dst->wal_buffers_full += add->wal_buffers_full - sub->wal_buffers_full;
}

/* GUC hooks for timing_clock_source */

bool
check_timing_clock_source(int *newval, void **extra, GucSource source)
{
	/*
	 * Do nothing if timing is not initialized. This is only expected on child
	 * processes in EXEC_BACKEND builds, as GUC hooks can be called during
	 * InitializeGUCOptions() before InitProcessGlobals() has had a chance to
	 * run pg_initialize_timing(). Instead, TSC will be initialized via
	 * restore_backend_variables.
	 */
#ifdef EXEC_BACKEND
	if (!timing_initialized)
		return true;
#else
	Assert(timing_initialized);
#endif

#if PG_INSTR_TSC_CLOCK
	pg_initialize_timing_tsc();

	if (*newval == TIMING_CLOCK_SOURCE_TSC && timing_tsc_frequency_khz <= 0)
	{
		GUC_check_errdetail("TSC is not supported as timing clock source");
		return false;
	}
#endif

	return true;
}

void
assign_timing_clock_source(int newval, void *extra)
{
#ifdef EXEC_BACKEND
	if (!timing_initialized)
		return;
#else
	Assert(timing_initialized);
#endif

	/*
	 * Ignore the return code since the check hook already verified TSC is
	 * usable if it's explicitly requested.
	 */
	pg_set_timing_clock_source(newval);
}

const char *
show_timing_clock_source(void)
{
	switch (timing_clock_source)
	{
		case TIMING_CLOCK_SOURCE_AUTO:
#if PG_INSTR_TSC_CLOCK
			if (pg_current_timing_clock_source() == TIMING_CLOCK_SOURCE_TSC)
				return "auto (tsc)";
#endif
			return "auto (system)";
		case TIMING_CLOCK_SOURCE_SYSTEM:
			return "system";
#if PG_INSTR_TSC_CLOCK
		case TIMING_CLOCK_SOURCE_TSC:
			return "tsc";
#endif
	}

	/* unreachable */
	return "?";
}
