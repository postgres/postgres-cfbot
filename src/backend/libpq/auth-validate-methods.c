/*-------------------------------------------------------------------------
 *
 * auth-validate-methods.c
 *	  Implementation of authentication credential validation methods
 *
 * This module implements the credential validators.  The baseline role-level
 * check (rolvaliduntil / role existence) implemented here is applied to every
 * authenticated session, regardless of authentication method.  Method-specific
 * validators are registered with the framework via
 * RegisterCredentialValidator().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/libpq/auth-validate-methods.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_authid.h"
#include "libpq/auth-validate-methods.h"
#include "miscadmin.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/*
 * Initialize validation methods
 */
void
InitializeValidationMethods(void)
{
	/* No method-specific validators are registered yet. */
}

/*
 * Baseline role-level credential check, applied to every authenticated
 * session regardless of authentication method.
 *
 * Checks pg_authid.rolvaliduntil for the session role; this is role-level and
 * auth-method-independent, so it governs password, certificate, OAuth, etc.
 * sessions alike.  Also treats a role that no longer exists as invalid.
 *
 * Returns true if the role is still valid, false if it has expired or has
 * been dropped.
 */
bool
ValidateRoleValidity(void)
{
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	TimestampTz valid_until;
	bool		result;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(GetSessionUserId()));

	if (!HeapTupleIsValid(tuple))
		return false;			/* role no longer exists */

	datum = SysCacheGetAttr(AUTHOID, tuple,
							Anum_pg_authid_rolvaliduntil,
							&isnull);
	if (!isnull)
	{
		valid_until = DatumGetTimestampTz(datum);
		result = (valid_until >= GetCurrentTimestamp());
	}
	else
		result = true;			/* no expiration set */

	ReleaseSysCache(tuple);
	return result;
}
