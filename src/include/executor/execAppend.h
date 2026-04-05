/*-------------------------------------------------------------------------
 * execAppend.h
 *		Support functions for MergeAppend and Append nodes.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/executor/execAppend.h
 *-------------------------------------------------------------------------
 */

#ifndef EXECAPPEND_H
#define EXECAPPEND_H

#include "nodes/execnodes.h"

extern void ExecInitAppendBase(AppendBaseState *state,
							   AppendBase *node,
							   EState *estate,
							   int eflags,
							   int first_partial_plan,
							   int *first_valid_partial_plan);
extern void ExecEndAppendBase(AppendBaseState *node);
extern void ExecReScanAppendBase(AppendBaseState *node);
extern void ExecAppendBaseAsyncBegin(AppendBaseState *node);
extern void ExecAppendBaseAsyncEventWait(AppendBaseState *node,
										 int timeout,
										 uint32 wait_event_info);
extern bool classify_matching_subplans_common(Bitmapset **valid_subplans,
											  Bitmapset *asyncplans,
											  Bitmapset **valid_asyncplans);

#endif							/* EXECAPPEND_H */
