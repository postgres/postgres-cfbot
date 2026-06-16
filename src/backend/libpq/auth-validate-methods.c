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
	 * Register the method-specific validators.  Role-level validity
	 * (rolvaliduntil and role existence) is checked for every authenticated
	 * session by ValidateRoleValidity(), so password-based methods need no
	 * separate validator of their own.
	 */
	RegisterCredentialValidator(CVT_CERT, validate_cert_credentials);
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

/*
 * Validate TLS client certificate credentials.
 *
 * The client certificate presented at connection time is retained on the
 * Port for the lifetime of the session, so its validity period can be
 * re-checked cheaply without any network round-trip.  Returns false if the
 * certificate's notAfter date has passed, true otherwise.
 *
 * If the session is not using a client certificate (which should not happen
 * for a cert-authenticated session), there is nothing certificate-specific to
 * validate, so the credentials are considered valid.
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
		return false;
#endif

	return true;
}
