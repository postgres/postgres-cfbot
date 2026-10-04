/*-------------------------------------------------------------------------
 *
 * explain_running.h
 *	  prototypes for explain_running.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain_running.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_RUNNING_H
#define EXPLAIN_RUNNING_H

#include "executor/executor.h"
#include "commands/explain_state.h"

extern void HandleLogQueryPlanInterrupt(void);
extern void ProcessLogQueryPlanInterrupt(void);
extern void LogQueryPlan(void);
extern Datum pg_log_query_plan(PG_FUNCTION_ARGS);

#endif							/* EXPLAIN_RUNNING_H */
