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

/*
 * A column of a custom format that needs its typmod or domain constraints
 * applied to a value that the format builds by itself.
 */
typedef struct CopyFromCoerceColumn
{
	int			fieldno;		/* position in attnumlist */
	AttrNumber	attnum;
	FunctionCallInfo typmod_fcinfo; /* applies the typmod, or NULL */
	bool		typmod_unsupported; /* typmod we cannot apply (arrays) */
	Oid			domain_type;	/* domain to check, or InvalidOid */
	void	   *domain_extra;	/* cache for domain_check_safe() */
} CopyFromCoerceColumn;

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
