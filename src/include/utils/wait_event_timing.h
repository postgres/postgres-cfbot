/*-------------------------------------------------------------------------
 *
 * wait_event_timing.h
 *	  Per-backend wait event timing instrumentation.
 *
 * This header declares the public surface of the wait-event-timing
 * feature, gated by the compile-time option --enable-wait-event-timing
 * (USE_WAIT_EVENT_TIMING) and the runtime GUC wait_event_capture.
 *
 * This commit introduces only the scaffolding: the capture-level enum
 * and the GUC backing variable.  Later commits in the series add the
 * recording hot path, the SQL-visible statistics, and the per-session
 * trace ring.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/utils/wait_event_timing.h
 *-------------------------------------------------------------------------
 */
#ifndef WAIT_EVENT_TIMING_H
#define WAIT_EVENT_TIMING_H

#include "c.h"

/*
 * Capture levels for the wait_event_capture GUC.  Order is significant:
 * higher values are strict supersets of lower ones, so code paths can
 * test for activation with "level >= WAIT_EVENT_CAPTURE_STATS".
 *
 *   OFF   - No instrumentation, no hot-path cost.
 *   STATS - Aggregated per-event statistics (added by a later commit).
 *
 * A further TRACE level is added later in the series.
 */
typedef enum WaitEventCaptureLevel
{
	WAIT_EVENT_CAPTURE_OFF = 0,
	WAIT_EVENT_CAPTURE_STATS,
}			WaitEventCaptureLevel;

/*
 * Pin the enum ordering at compile time so future code that compares with
 * >= against WAIT_EVENT_CAPTURE_STATS keeps working, and so reordering is
 * caught at build time rather than via mysterious runtime mode switches.
 */
StaticAssertDecl(WAIT_EVENT_CAPTURE_OFF == 0 &&
				 WAIT_EVENT_CAPTURE_STATS == 1,
				 "WaitEventCaptureLevel values must be 0=OFF < 1=STATS");

/* GUC variable (defined in wait_event_timing.c, even in stub builds). */
extern PGDLLIMPORT int wait_event_capture;

#endif							/* WAIT_EVENT_TIMING_H */
