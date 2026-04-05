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

#endif							/* EXECAPPEND_H */
