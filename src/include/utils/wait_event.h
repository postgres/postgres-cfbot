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
static inline uint32 pgstat_report_wait_start_nested(uint32 wait_event_info);
static inline void pgstat_report_wait_end_nested(uint32 outer_wait_event_info);
extern void pgstat_set_wait_event_storage(uint32 *wait_event_info);
extern void pgstat_reset_wait_event_storage(void);

extern PGDLLIMPORT uint32 *my_wait_event_info;


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

/* ----------
 * pgstat_report_wait_start_nested() -
 *
 *	Like pgstat_report_wait_start(), for a wait that can occur while another
 *	wait event is already published, such as a write to the server log made
 *	from inside a wait region.  Returns the wait event that was published
 *	before, for the caller to hand back to pgstat_report_wait_end_nested()
 *	once its own wait is over.
 * ----------
 */
static inline uint32
pgstat_report_wait_start_nested(uint32 wait_event_info)
{
	uint32		outer_wait_event_info;

	/* see pgstat_report_wait_start() */
	outer_wait_event_info = *(volatile uint32 *) my_wait_event_info;
	*(volatile uint32 *) my_wait_event_info = wait_event_info;

	return outer_wait_event_info;
}

/* ----------
 * pgstat_report_wait_end_nested() -
 *
 *	Called to report end of a wait started with
 *	pgstat_report_wait_start_nested().  Where pgstat_report_wait_end() would
 *	clear the wait event, this publishes the one that was current before the
 *	nested wait began, so that an enclosing wait region keeps its event.  If
 *	there was none, outer_wait_event_info is 0 and the effect is the same as
 *	pgstat_report_wait_end().
 * ----------
 */
static inline void
pgstat_report_wait_end_nested(uint32 outer_wait_event_info)
{
	/* see pgstat_report_wait_start() */
	*(volatile uint32 *) my_wait_event_info = outer_wait_event_info;
}


#endif							/* WAIT_EVENT_H */
