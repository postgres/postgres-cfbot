/*-------------------------------------------------------------------------
 *
 * expr.h
 *	  Expression and built-in function evaluation for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/expr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPR_H
#define EXPR_H

#include "pgbench.h"
#include "variable.h"

extern bool evaluateExpr(CState *st, PgBenchExpr *expr, PgBenchValue *retval);

#endif							/* EXPR_H */
