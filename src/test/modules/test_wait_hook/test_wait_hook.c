/*--------------------------------------------------------------------------
 *
 * test_wait_hook.c
 *		Test module for the wait_event_begin_hook/wait_event_end_hook
 *		contract (see src/include/utils/wait_event.h).
 *
 * All state kept by this module is backend-local: preallocated static
 * arrays only, exactly as the hook contract requires.  Nothing here
 * palloc's, waits, takes a lock, or calls elog/ereport from inside a hook,
 * except for test_wait_hook_nested_wait_in_hook() mode, which exists
 * specifically to demonstrate what happens when a hook implementation
 * breaks that rule.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_wait_hook/test_wait_hook.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* ----------------------------------------------------------------------
 * Event ring
 * ----------------------------------------------------------------------
 */

typedef enum TestWaitHookEventKind
{
	TEST_WAIT_HOOK_BEGIN,
	TEST_WAIT_HOOK_END,
} TestWaitHookEventKind;

typedef struct TestWaitHookEvent
{
	TestWaitHookEventKind kind;
	char		consumer;		/* 'A' or 'B' */
	uint32		wait_event_info;
	int			depth;			/* wait_event_hook_depth seen by the hook */
} TestWaitHookEvent;

#define TEST_WAIT_HOOK_RING_SIZE 64

static TestWaitHookEvent ring[TEST_WAIT_HOOK_RING_SIZE];
static int	ring_head = 0;		/* index of the oldest recorded event */
static int	ring_len = 0;		/* number of valid events, <= RING_SIZE */

/*
 * Record one event, discarding the oldest once the ring is full.
 *
 * Only PgSleep events are recorded -- the ones test_wait_hook_wait() and
 * test_wait_hook_error_after_start() generate.  The hooks are installed
 * for the whole backend, not just for the statement under test: between
 * statements this session blocks in secure_read() on a ClientRead wait,
 * and ordinary catalog access can block on file I/O, both of which are
 * timed waits on this branch too.  Without this filter the ring would
 * nondeterministically pick up ClientRead and IO:DataFileRead pairs that
 * have nothing to do with the scenario being tested.  This also covers
 * wait_event_info == 0, the sentinel that pgstat_report_wait_start()/
 * pgstat_report_wait_end() use to mean "not currently waiting", which a
 * contract-violating hook (see test_wait_hook_nested_wait_in_hook()
 * below) can otherwise leak in here: a wait started from inside a hook
 * body clobbers my_wait_event_info before the outer wait's own end call
 * reads it back.
 */
static void
ring_push(TestWaitHookEventKind kind, char consumer, uint32 wait_event_info)
{
	TestWaitHookEvent *e;
	int			idx;

	if (wait_event_info != WAIT_EVENT_PG_SLEEP)
		return;

	if (ring_len < TEST_WAIT_HOOK_RING_SIZE)
		idx = (ring_head + ring_len++) % TEST_WAIT_HOOK_RING_SIZE;
	else
	{
		idx = ring_head;
		ring_head = (ring_head + 1) % TEST_WAIT_HOOK_RING_SIZE;
	}

	e = &ring[idx];
	e->kind = kind;
	e->consumer = consumer;
	e->wait_event_info = wait_event_info;
	e->depth = wait_event_hook_depth;
}

/* ----------------------------------------------------------------------
 * Consumers A and B
 *
 * Each consumer chains onto whatever was installed before it: the begin
 * hook calls the saved previous begin hook before doing its own work, and
 * the end hook does its own work before calling the saved previous end
 * hook.  That is the ordering the contract in wait_event.h requires of a
 * chaining consumer.
 * ----------------------------------------------------------------------
 */

static wait_event_hook_type prevA_begin = NULL;
static wait_event_hook_type prevA_end = NULL;
static wait_event_hook_type prevB_begin = NULL;
static wait_event_hook_type prevB_end = NULL;

static bool a_installed = false;
static bool b_installed = false;

/* LIFO order in which consumers were installed, for uninstall_all() */
#define TEST_WAIT_HOOK_MAX_CONSUMERS 2
static char install_order[TEST_WAIT_HOOK_MAX_CONSUMERS];
static int	n_installed = 0;

/* set by test_wait_hook_nested_wait_in_hook() */
static bool nested_wait_in_hook = false;

static void
consumerA_begin(uint32 wait_event_info)
{
	if (prevA_begin != NULL)
		prevA_begin(wait_event_info);

	ring_push(TEST_WAIT_HOOK_BEGIN, 'A', wait_event_info);

	if (nested_wait_in_hook)
	{
		/*
		 * Deliberately violate the "no waits inside a hook" rule to prove
		 * that the depth guard in wait_event.h suppresses the resulting
		 * nested begin/end pair: wait_event_hook_depth is already 1 here,
		 * so the timed reporting functions will not call back into any
		 * installed hook.  What is not suppressed is that
		 * pgstat_report_wait_end_timed() unconditionally clears
		 * my_wait_event_info once this nested wait finishes; that is why
		 * the outer wait's own end call arrives here with
		 * wait_event_info == 0 and gets filtered out by ring_push().
		 */
		WaitLatch(MyLatch, WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1,
				  WAIT_EVENT_PG_SLEEP);
	}
}

static void
consumerA_end(uint32 wait_event_info)
{
	ring_push(TEST_WAIT_HOOK_END, 'A', wait_event_info);

	if (prevA_end != NULL)
		prevA_end(wait_event_info);
}

static void
consumerB_begin(uint32 wait_event_info)
{
	if (prevB_begin != NULL)
		prevB_begin(wait_event_info);

	ring_push(TEST_WAIT_HOOK_BEGIN, 'B', wait_event_info);
}

static void
consumerB_end(uint32 wait_event_info)
{
	ring_push(TEST_WAIT_HOOK_END, 'B', wait_event_info);

	if (prevB_end != NULL)
		prevB_end(wait_event_info);
}

/* ----------------------------------------------------------------------
 * SQL-callable functions
 * ----------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(test_wait_hook_install);
PG_FUNCTION_INFO_V1(test_wait_hook_uninstall_all);
PG_FUNCTION_INFO_V1(test_wait_hook_events);
PG_FUNCTION_INFO_V1(test_wait_hook_nested_wait_in_hook);
PG_FUNCTION_INFO_V1(test_wait_hook_error_after_start);
PG_FUNCTION_INFO_V1(test_wait_hook_wait);

Datum
test_wait_hook_install(PG_FUNCTION_ARGS)
{
	char	   *consumer_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char		consumer;

	if (strcmp(consumer_str, "A") == 0)
		consumer = 'A';
	else if (strcmp(consumer_str, "B") == 0)
		consumer = 'B';
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("consumer must be \"A\" or \"B\"")));

	if ((consumer == 'A' && a_installed) || (consumer == 'B' && b_installed))
		ereport(ERROR,
				(errmsg("consumer \"%c\" is already installed", consumer)));

	if (n_installed >= TEST_WAIT_HOOK_MAX_CONSUMERS)
		ereport(ERROR,
				(errmsg("both consumers are already installed")));

	if (consumer == 'A')
	{
		prevA_begin = wait_event_begin_hook;
		prevA_end = wait_event_end_hook;
		wait_event_begin_hook = consumerA_begin;
		wait_event_end_hook = consumerA_end;
		a_installed = true;
	}
	else
	{
		prevB_begin = wait_event_begin_hook;
		prevB_end = wait_event_end_hook;
		wait_event_begin_hook = consumerB_begin;
		wait_event_end_hook = consumerB_end;
		b_installed = true;
	}

	install_order[n_installed++] = consumer;

	PG_RETURN_VOID();
}

Datum
test_wait_hook_uninstall_all(PG_FUNCTION_ARGS)
{
	/* Unwind in reverse installation order, restoring saved pointers. */
	while (n_installed > 0)
	{
		char		consumer = install_order[--n_installed];

		if (consumer == 'A')
		{
			wait_event_begin_hook = prevA_begin;
			wait_event_end_hook = prevA_end;
			prevA_begin = NULL;
			prevA_end = NULL;
			a_installed = false;
		}
		else
		{
			wait_event_begin_hook = prevB_begin;
			wait_event_end_hook = prevB_end;
			prevB_begin = NULL;
			prevB_end = NULL;
			b_installed = false;
		}
	}

	nested_wait_in_hook = false;

	PG_RETURN_VOID();
}

Datum
test_wait_hook_events(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	for (int i = 0; i < ring_len; i++)
	{
		TestWaitHookEvent *e = &ring[(ring_head + i) % TEST_WAIT_HOOK_RING_SIZE];
		Datum		values[5];
		bool		nulls[5];
		char		consumer_str[2];

		memset(nulls, 0, sizeof(nulls));

		consumer_str[0] = e->consumer;
		consumer_str[1] = '\0';

		values[0] = CStringGetTextDatum(e->kind == TEST_WAIT_HOOK_BEGIN ? "begin" : "end");
		values[1] = CStringGetTextDatum(consumer_str);
		values[2] = CStringGetTextDatum(pgstat_get_wait_event_type(e->wait_event_info));
		values[3] = CStringGetTextDatum(pgstat_get_wait_event(e->wait_event_info));
		values[4] = Int32GetDatum(e->depth);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	/* The SRF has copied everything it needs; the ring is now free. */
	ring_head = 0;
	ring_len = 0;

	return (Datum) 0;
}

Datum
test_wait_hook_nested_wait_in_hook(PG_FUNCTION_ARGS)
{
	nested_wait_in_hook = PG_GETARG_BOOL(0);

	PG_RETURN_VOID();
}

Datum
test_wait_hook_error_after_start(PG_FUNCTION_ARGS)
{
	pgstat_report_wait_start_timed(WAIT_EVENT_PG_SLEEP);

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("deliberate error after wait start")));

	PG_RETURN_VOID();			/* unreachable */
}

/*
 * A single, deterministic PgSleep wait: exactly one WaitLatch() call with
 * a fixed 1ms timeout and no WL_LATCH_SET.  Deliberately not using
 * pg_sleep() here: its loop passes WL_LATCH_SET, so if the process latch
 * is already set (for reasons unrelated to this test), WaitLatch() returns
 * immediately and pg_sleep() loops around and waits again, recording a
 * second, spurious begin/end pair.  Omitting WL_LATCH_SET makes the latch's
 * set state irrelevant, so this always produces exactly one pair.
 */
Datum
test_wait_hook_wait(PG_FUNCTION_ARGS)
{
	(void) WaitLatch(MyLatch, WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1,
					 WAIT_EVENT_PG_SLEEP);

	PG_RETURN_VOID();
}
