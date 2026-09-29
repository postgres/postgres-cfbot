/*-------------------------------------------------------------------------
 *
 * auth-validate-methods.c
 *	  Implementation of authentication credential validation methods
 *
 * This module implements the credential validators: the baseline
 * role-level check here, plus method-specific ones registered via
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

#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "libpq/auth-validate-methods.h"
#include "libpq/auth-validate.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/* Function declarations for internal use */
static bool validate_cert_credentials(void);

/*
 * Initialize validation methods
 */
void
InitializeValidationMethods(void)
{
	/*
	 * Register method-specific validators. Password methods need none --
	 * ValidateRoleValidity() covers every session's baseline check.
	 */
	RegisterCredentialValidator(CVT_CERT, validate_cert_credentials);
}

/*
 * Baseline check for every session: role must still exist and not have
 * passed rolvaliduntil.  Uses GetAuthenticatedUserId(), not
 * GetSessionUserId(), so SET SESSION AUTHORIZATION can't mask expiry.
 */
bool
ValidateRoleValidity(void)
{
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	TimestampTz valid_until;
	bool		result;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(GetAuthenticatedUserId()));

	if (!HeapTupleIsValid(tuple))
	{
		/*
		 * The role is gone, so we have no catalog row to name it from; fall
		 * back to the username the client originally authenticated as.
		 */
		SetCredentialValidationFailureDetail("role validity check failed for user \"%s\": role no longer exists",
											  MyProcPort ? MyProcPort->user_name : "?");
		return false;			/* role no longer exists */
	}

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

	if (!result)
		SetCredentialValidationFailureDetail("role validity check failed for user \"%s\": role has passed its VALID UNTIL expiration (%s)",
											  NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname),
											  timestamptz_to_str(valid_until));

	ReleaseSysCache(tuple);
	return result;
}

/*
 * CVT_CERT validator: the peer cert is retained on Port, so its notAfter
 * and revocation status can be re-checked locally, no round-trip needed.
 * No cert on the session (shouldn't happen) is treated as valid.
 */
static bool
validate_cert_credentials(void)
{
#ifdef USE_SSL
	Port	   *port = MyProcPort;

	if (port == NULL || !port->ssl_in_use || port->peer == NULL)
		return true;

	/* The session is no longer valid once the client certificate expires */
	if (be_tls_get_peer_cert_expired(port))
	{
		SetCredentialValidationFailureDetail("client certificate check failed for user \"%s\": certificate has expired",
											  port->user_name);
		return false;
	}

	/* Nor is it valid once the certificate has been revoked via CRL */
	if (be_tls_get_peer_cert_revoked(port))
	{
		SetCredentialValidationFailureDetail("client certificate check failed for user \"%s\": certificate has been revoked",
											  port->user_name);
		return false;
	}
#endif

	return true;
}
