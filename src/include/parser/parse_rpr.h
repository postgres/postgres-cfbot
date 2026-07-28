/*-------------------------------------------------------------------------
 *
 * parse_rpr.h
 *	  handle Row Pattern Recognition in parser
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_rpr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_RPR_H
#define PARSE_RPR_H

#include "parser/parse_node.h"

extern void transformRPR(ParseState *pstate, WindowClause *wc,
						 WindowDef *windef, List **targetlist);

#endif							/* PARSE_RPR_H */
