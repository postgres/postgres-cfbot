/*-------------------------------------------------------------------------
 *
 * copyfrom_internal.h
 *	  Internal definitions for COPY FROM command.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/copyfrom_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYFROM_INTERNAL_H
#define COPYFROM_INTERNAL_H

#include "commands/copy_state.h"

/*
 * Represents the insert method to be used during COPY FROM.
 */
typedef enum CopyInsertMethod
{
	CIM_SINGLE,					/* use table_tuple_insert or ExecForeignInsert */
	CIM_MULTI,					/* always use table_multi_insert or
								 * ExecForeignBatchInsert */
	CIM_MULTI_CONDITIONAL,		/* use table_multi_insert or
								 * ExecForeignBatchInsert only if valid */
} CopyInsertMethod;

extern void ReceiveCopyBegin(CopyFromState cstate);
extern void ReceiveCopyBinaryHeader(CopyFromState cstate);

/* One-row callbacks for built-in formats defined in copyfromparse.c */
extern bool CopyFromTextOneRow(CopyFromState cstate, ExprContext *econtext,
							   Datum *values, bool *nulls);
extern bool CopyFromCSVOneRow(CopyFromState cstate, ExprContext *econtext,
							  Datum *values, bool *nulls);
extern bool CopyFromBinaryOneRow(CopyFromState cstate, ExprContext *econtext,
								 Datum *values, bool *nulls);

#endif							/* COPYFROM_INTERNAL_H */
