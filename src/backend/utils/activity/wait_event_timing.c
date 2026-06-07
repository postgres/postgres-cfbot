/*-------------------------------------------------------------------------
 *
 * wait_event_timing.c
 *	  Per-backend wait event timing instrumentation.
 *
 * Controlled by the wait_event_capture GUC (off | stats, default off)
 * and the compile-time option --enable-wait-event-timing.
 *
 * This commit provides only the GUC scaffolding: the backing variable,
 * the enum-value table consumed by guc.c, and the check/assign hooks.
 * No instrumentation is performed yet -- later commits in the series add
 * the recording hot path and the SQL surface.  The file compiles in both
 * build configurations; in builds without --enable-wait-event-timing the
 * check hook rejects any value other than off.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/wait_event_timing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/wait_event_timing.h"

/*
 * GUC variable -- always defined so the GUC system has a backing variable
 * even when compiled without --enable-wait-event-timing.  In stub builds
 * the check hook below rejects any value other than OFF.
 */
int			wait_event_capture = WAIT_EVENT_CAPTURE_OFF;

/*
 * Enum value table consumed by guc.c.  Order matches the
 * WaitEventCaptureLevel enum and the documented "off < stats" ordering.
 */
const struct config_enum_entry wait_event_capture_options[] = {
	{"off", WAIT_EVENT_CAPTURE_OFF, false},
	{"stats", WAIT_EVENT_CAPTURE_STATS, false},
	{NULL, 0, false}
};

#ifdef USE_WAIT_EVENT_TIMING

/*
 * GUC check hook for wait_event_capture (timing build).
 *
 * All enum values are accepted at this level.  Side effects (attaching
 * storage, etc.) are introduced by later commits; for now there is
 * nothing to validate beyond the enum mapping that guc.c already did.
 */
bool
check_wait_event_capture(int *newval, void **extra, GucSource source)
{
	return true;
}

/*
 * GUC assign hook for wait_event_capture (timing build).
 *
 * No-op for now.  Later commits use this hook to drop in-flight wait
 * state and manage per-session resources when the capture level changes.
 */
void
assign_wait_event_capture(int newval, void *extra)
{
}

#else							/* !USE_WAIT_EVENT_TIMING */

/*
 * GUC check hook for the stub build.  Any value other than 'off' is
 * meaningless without --enable-wait-event-timing, so reject it -- or
 * downgrade to 'off' with a warning when the value comes from a
 * non-interactive source (config file at startup), so a leftover setting
 * does not prevent the server from starting.
 */
bool
check_wait_event_capture(int *newval, void **extra, GucSource source)
{
	if (*newval != WAIT_EVENT_CAPTURE_OFF)
	{
		if (source < PGC_S_INTERACTIVE)
		{
			ereport(WARNING,
					(errmsg("wait_event_capture is not supported by this build, "
							"forcing to \"off\""),
					 errhint("Compile PostgreSQL with "
							 "--enable-wait-event-timing.")));
			*newval = WAIT_EVENT_CAPTURE_OFF;
			return true;
		}
		GUC_check_errdetail("This build does not support wait event capture.");
		GUC_check_errhint("Compile PostgreSQL with --enable-wait-event-timing.");
		return false;
	}
	return true;
}

/* Stub assign hook -- nothing to do without compile-time support. */
void
assign_wait_event_capture(int newval, void *extra)
{
}

#endif							/* USE_WAIT_EVENT_TIMING */
