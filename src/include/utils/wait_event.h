/*-------------------------------------------------------------------------
 * wait_event.h
 *	  Definitions related to wait event reporting
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * src/include/utils/wait_event.h
 * ----------
 */
#ifndef WAIT_EVENT_H
#define WAIT_EVENT_H

/* enums for wait events */
#include "utils/wait_event_types.h"

extern const char *pgstat_get_wait_event(uint32 wait_event_info);
extern const char *pgstat_get_wait_event_type(uint32 wait_event_info);
static inline void pgstat_report_wait_start(uint32 wait_event_info);
static inline void pgstat_report_wait_end(void);
static inline void pgstat_report_wait_start_timed(uint32 wait_event_info);
static inline void pgstat_report_wait_end_timed(void);
extern void pgstat_set_wait_event_storage(uint32 *wait_event_info);
extern void pgstat_reset_wait_event_storage(void);

extern PGDLLIMPORT uint32 *my_wait_event_info;

/*
 * Hooks for explicitly instrumented waits.  Hook implementations may use
 * only preallocated backend-local state; they must not wait, allocate memory,
 * acquire locks, or report errors.  The depth guard prevents re-entry.
 *
 * Hook users that chain callbacks must save the previous hook pointers, call
 * the previous begin hook before their own begin work, and perform their own
 * end work before calling the previous end hook.
 */
typedef void (*wait_event_hook_type) (uint32 wait_event_info);

extern PGDLLIMPORT wait_event_hook_type wait_event_begin_hook;
extern PGDLLIMPORT wait_event_hook_type wait_event_end_hook;
extern PGDLLIMPORT int wait_event_hook_depth;

/*
 * Out-of-line, cold slow paths for the hook-enabled case of the timed
 * wait-event pair below.  See pgstat_report_wait_start_timed() for why
 * these exist.
 */
extern pg_noinline pg_attribute_cold void pgstat_wait_event_hook_begin_slow(uint32 wait_event_info);
extern pg_noinline pg_attribute_cold void pgstat_wait_event_hook_end_slow(void);

/*
 * Wait Events - Extension, InjectionPoint
 *
 * Use InjectionPoint when the server process is waiting in an injection
 * point.  Use Extension for other cases of the server process waiting for
 * some condition defined by an extension module.
 *
 * Extensions can define their own wait events in these categories.  They
 * should call one of these functions with a wait event string.  If the wait
 * event associated to a string is already allocated, it returns the wait
 * event information to use.  If not, it gets one wait event ID allocated from
 * a shared counter, associates the string to the ID in the shared dynamic
 * hash and returns the wait event information.
 *
 * The ID retrieved can be used with pgstat_report_wait_start() or equivalent.
 */
extern uint32 WaitEventExtensionNew(const char *wait_event_name);
extern uint32 WaitEventInjectionPointNew(const char *wait_event_name);

extern char **GetWaitEventCustomNames(uint32 classId, int *nwaitevents);

/* ----------
 * pgstat_report_wait_start() -
 *
 *	Called from places where server process needs to wait.  This is called
 *	to report wait event information.  The wait information is stored
 *	as 4-bytes where first byte represents the wait event class (type of
 *	wait, for different types of wait, refer WaitClass) and the next
 *	3-bytes represent the actual wait event.  Currently 2-bytes are used
 *	for wait event which is sufficient for current usage, 1-byte is
 *	reserved for future usage.
 *
 *	Historically we used to make this reporting conditional on
 *	pgstat_track_activities, but the check for that seems to add more cost
 *	than it saves.
 *
 *	my_wait_event_info initially points to local memory, making it safe to
 *	call this before MyProc has been initialized.
 * ----------
 */
static inline void
pgstat_report_wait_start(uint32 wait_event_info)
{
	/*
	 * Since this is a four-byte field which is always read and written as
	 * four-bytes, updates are atomic.
	 */
	*(volatile uint32 *) my_wait_event_info = wait_event_info;
}

/* ----------
 * pgstat_report_wait_end() -
 *
 *	Called to report end of a wait.
 * ----------
 */
static inline void
pgstat_report_wait_end(void)
{
	/* see pgstat_report_wait_start() */
	*(volatile uint32 *) my_wait_event_info = 0;
}

/*
 * Explicitly instrumented variant of the ordinary wait-event reporting pair.
 * The ordinary functions above remain unchanged for uninstrumented sites.
 *
 * Every backend reaches these two functions at every instrumented wait, but
 * only a process in which a consumer has installed a hook ever takes the
 * enabled path.  The pointer test is therefore hinted with unlikely() for the
 * no-consumer path, which is laid out as the fall-through; without the hint,
 * GCC's static branch predictor treats a pointer compared with NULL as
 * non-NULL and lays out the enabled path there instead.  The enabled path --
 * the depth guard, the volatile read in the end case, and the indirect call
 * -- lives in a separate cold, noinline function rather than being inlined at
 * each call site.  A process with no consumer pays one predicted-not-taken
 * branch per call; a process with a consumer pays one additional taken jump.
 * The depth guard, the hook-chaining contract, and the exported hook
 * variables are unchanged.
 */
static inline void
pgstat_report_wait_start_timed(uint32 wait_event_info)
{
	*(volatile uint32 *) my_wait_event_info = wait_event_info;

	if (unlikely(wait_event_begin_hook != NULL))
		pgstat_wait_event_hook_begin_slow(wait_event_info);
}

static inline void
pgstat_report_wait_end_timed(void)
{
	if (unlikely(wait_event_end_hook != NULL))
		pgstat_wait_event_hook_end_slow();
	*(volatile uint32 *) my_wait_event_info = 0;
}


#endif							/* WAIT_EVENT_H */
