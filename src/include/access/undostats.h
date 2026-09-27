/*-------------------------------------------------------------------------
 *
 * undostats.h
 *	  UNDO monitoring functions
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undostats.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDOSTATS_H
#define UNDOSTATS_H

#include "fmgr.h"

extern Datum pg_stat_get_undo_retention(PG_FUNCTION_ARGS);

#endif							/* UNDOSTATS_H */
