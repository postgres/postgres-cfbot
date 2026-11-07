/*-------------------------------------------------------------------------
 * relpath.c
 *		Shared frontend/backend code to compute pathnames of relation files
 *
 * This module also contains some logic associated with fork names.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/relpath.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "catalog/pg_tablespace_d.h"
#include "common/relpath.h"
#include "storage/procnumber.h"


/*
 * Lookup table of fork name by fork number.
 *
 * If you add a new entry, remember to update the errhint in
 * forkname_to_number() below, and update the SGML documentation for
 * pg_relation_size().
 */
const char *const forkNames[] = {
	[MAIN_FORKNUM] = "main",
	[FSM_FORKNUM] = "fsm",
	[VISIBILITYMAP_FORKNUM] = "vm",
	[INIT_FORKNUM] = "init",
};

StaticAssertDecl(lengthof(forkNames) == (MAX_FORKNUM + 1),
				 "array length mismatch");

/*
 * forkname_to_number - look up fork number by name
 *
 * In backend, we throw an error for no match; in frontend, we just
 * return InvalidForkNumber.
 */
ForkNumber
forkname_to_number(const char *forkName)
{
	ForkNumber	forkNum;

	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		if (strcmp(forkNames[forkNum], forkName) == 0)
			return forkNum;

#ifndef FRONTEND
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid fork name"),
			 errhint("Valid fork names are \"main\", \"fsm\", "
					 "\"vm\", and \"init\".")));
#endif

	return InvalidForkNumber;
}

/*
 * forkname_chars
 *		We use this to figure out whether a filename could be a relation
 *		fork (as opposed to an oddly named stray file that somehow ended
 *		up in the database directory).  If the passed string begins with
 *		a fork name (other than the main fork name), we return its length,
 *		and set *fork (if not NULL) to the fork number.  If not, we return 0.
 *
 * Note that the present coding assumes that there are no fork names which
 * are prefixes of other fork names.
 */
int
forkname_chars(const char *str, ForkNumber *fork)
{
	ForkNumber	forkNum;

	for (forkNum = 1; forkNum <= MAX_FORKNUM; forkNum++)
	{
		int			len = strlen(forkNames[forkNum]);

		if (strncmp(forkNames[forkNum], str, len) == 0)
		{
			if (fork)
				*fork = forkNum;
			return len;
		}
	}
	if (fork)
		*fork = InvalidForkNumber;
	return 0;
}


/*
 * GetDatabasePath - construct path to a database directory
 *
 * Result is a palloc'd string.
 *
 * XXX this must agree with GetRelationPath()!
 */
char *
GetDatabasePath(Oid dbOid, Oid spcOid)
{
	if (spcOid == GLOBALTABLESPACE_OID)
	{
		/* Shared system relations live in {datadir}/global */
		Assert(dbOid == 0);
		return pstrdup("global");
	}
	else if (spcOid == DEFAULTTABLESPACE_OID)
	{
		/* The default tablespace is {datadir}/base */
		return psprintf("base/%u", dbOid);
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
		return psprintf("%s/%u/%s/%u",
						PG_TBLSPC_DIR, spcOid,
						TABLESPACE_VERSION_DIRECTORY, dbOid);
	}
}

/*
 * GetRelationPath - construct path to a relation's file
 *
 * The result is returned in-place as a struct, to make it suitable for use in
 * critical sections etc.
 *
 * Note: ideally, procNumber would be declared as type ProcNumber, but
 * relpath.h would have to include a backend-only header to do that; doesn't
 * seem worth the trouble considering ProcNumber is just int anyway.
 */
RelPathStr
GetRelationPath(Oid dbOid, Oid spcOid, RelFileNumber relNumber,
				int procNumber, ForkNumber forkNumber)
{
	RelPathStr	rp;

	if (spcOid == GLOBALTABLESPACE_OID)
	{
		/* Shared system relations live in {datadir}/global */
		Assert(dbOid == 0);
		Assert(procNumber == INVALID_PROC_NUMBER);
		if (forkNumber != MAIN_FORKNUM)
			sprintf(rp.str, "global/%u_%s",
					relNumber, forkNames[forkNumber]);
		else
			sprintf(rp.str, "global/%u",
					relNumber);
	}
	else if (spcOid == DEFAULTTABLESPACE_OID)
	{
		/* The default tablespace is {datadir}/base */
		if (procNumber == INVALID_PROC_NUMBER)
		{
			if (forkNumber != MAIN_FORKNUM)
			{
				sprintf(rp.str, "base/%u/%u_%s",
						dbOid, relNumber,
						forkNames[forkNumber]);
			}
			else
				sprintf(rp.str, "base/%u/%u",
						dbOid, relNumber);
		}
		else
		{
			if (forkNumber != MAIN_FORKNUM)
				sprintf(rp.str, "base/%u/t%d_%u_%s",
						dbOid, procNumber, relNumber,
						forkNames[forkNumber]);
			else
				sprintf(rp.str, "base/%u/t%d_%u",
						dbOid, procNumber, relNumber);
		}
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
		if (procNumber == INVALID_PROC_NUMBER)
		{
			if (forkNumber != MAIN_FORKNUM)
				sprintf(rp.str, "%s/%u/%s/%u/%u_%s",
						PG_TBLSPC_DIR, spcOid,
						TABLESPACE_VERSION_DIRECTORY,
						dbOid, relNumber,
						forkNames[forkNumber]);
			else
				sprintf(rp.str, "%s/%u/%s/%u/%u",
						PG_TBLSPC_DIR, spcOid,
						TABLESPACE_VERSION_DIRECTORY,
						dbOid, relNumber);
		}
		else
		{
			if (forkNumber != MAIN_FORKNUM)
				sprintf(rp.str, "%s/%u/%s/%u/t%d_%u_%s",
						PG_TBLSPC_DIR, spcOid,
						TABLESPACE_VERSION_DIRECTORY,
						dbOid, procNumber, relNumber,
						forkNames[forkNumber]);
			else
				sprintf(rp.str, "%s/%u/%s/%u/t%d_%u",
						PG_TBLSPC_DIR, spcOid,
						TABLESPACE_VERSION_DIRECTORY,
						dbOid, procNumber, relNumber);
		}
	}

	Assert(strnlen(rp.str, REL_PATH_STR_MAXLEN + 1) <= REL_PATH_STR_MAXLEN);

	return rp;
}

/*
 * Basic parsing of putative relation filenames.
 *
 * This function returns true if the file appears to be in the correct format
 * for a non-temporary relation and false otherwise.
 *
 * If it returns true, it sets *relnumber, *fork, and *segno to the values
 * extracted from the filename. If it returns false, these values are set to
 * InvalidRelFileNumber, InvalidForkNumber, and 0, respectively.
 */
bool
parse_filename_for_nontemp_relation(const char *name, RelFileNumber *relnumber,
									ForkNumber *fork, unsigned *segno)
{
	unsigned long n,
				s;
	ForkNumber	f;
	char	   *endp;

	*relnumber = InvalidRelFileNumber;
	*fork = InvalidForkNumber;
	*segno = 0;

	/*
	 * Relation filenames should begin with a digit that is not a zero. By
	 * rejecting cases involving leading zeroes, the caller can assume that
	 * there's only one possible string of characters that could have produced
	 * any given value for *relnumber.
	 *
	 * (To be clear, we don't expect files with names like 0017.3 to exist at
	 * all -- but if 0017.3 does exist, it's a non-relation file, not part of
	 * the main fork for relfilenode 17.)
	 */
	if (name[0] < '1' || name[0] > '9')
		return false;

	/*
	 * Parse the leading digit string. If the value is out of range, we
	 * conclude that this isn't a relation file at all.
	 */
	errno = 0;
	n = strtoul(name, &endp, 10);
	if (errno || name == endp || n <= 0 || n > PG_UINT32_MAX)
		return false;
	name = endp;

	/* Check for a fork name. */
	if (*name != '_')
		f = MAIN_FORKNUM;
	else
	{
		int			forkchar;

		forkchar = forkname_chars(name + 1, &f);
		if (forkchar <= 0)
			return false;
		name += forkchar + 1;
	}

	/* Check for a segment number. */
	if (*name != '.')
		s = 0;
	else
	{
		/* Reject leading zeroes, just like we do for RelFileNumber. */
		if (name[1] < '1' || name[1] > '9')
			return false;

		errno = 0;
		s = strtoul(name + 1, &endp, 10);
		if (errno || name + 1 == endp || s <= 0 || s > PG_UINT32_MAX)
			return false;
		name = endp;
	}

	/* Now we should be at the end. */
	if (*name != '\0')
		return false;

	/* Set out parameters and return. */
	*relnumber = (RelFileNumber) n;
	*fork = f;
	*segno = (unsigned) s;
	return true;
}
