/*-------------------------------------------------------------------------
 *
 * like_bmh.h
 *	  Runtime state for BMH-optimized LIKE searches.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/like_bmh.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIKE_BMH_H
#define LIKE_BMH_H

#include "fmgr.h"

#define LIKE_BMH_FALLBACK	(-2)

typedef enum LikeBMHMode
{
	LIKE_BMH_GENERIC,
	LIKE_BMH_SEARCH,
	LIKE_BMH_SEARCH_REVALIDATE,
} LikeBMHMode;

typedef struct LikeBMHState
{
	LikeBMHMode mode;
} LikeBMHState;

extern int	like_bmh_match(const char *s, int slen, const char *p, int plen,
						   FmgrInfo *flinfo, Oid collation);

#endif							/* LIKE_BMH_H */
