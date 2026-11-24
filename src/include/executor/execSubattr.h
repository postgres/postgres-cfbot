/*-------------------------------------------------------------------------
 *
 * execSubattr.h
 *	  Prove that an UPDATE cannot change what an index reads of a column.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/executor/execSubattr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECSUBATTR_H
#define EXECSUBATTR_H

#include "nodes/execnodes.h"

extern Bitmapset *ExecSubattrProveUntouched(ResultRelInfo *resultRelInfo,
											Node *setexpr, int varno,
											AttrNumber attnum,
											ParamListInfo params);

#endif							/* EXECSUBATTR_H */
