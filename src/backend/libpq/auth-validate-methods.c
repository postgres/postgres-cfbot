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
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "libpq/oauth.h"
#include "miscadmin.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/* Function declarations for internal use */
static bool validate_oauth_credentials(void);
static bool validate_cert_credentials(void);
static bool validate_gss_credentials(void);
static bool validate_ldap_credentials(void);

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
	RegisterCredentialValidator(CVT_OAUTH, validate_oauth_credentials);
	RegisterCredentialValidator(CVT_CERT, validate_cert_credentials);
	RegisterCredentialValidator(CVT_GSS, validate_gss_credentials);
	RegisterCredentialValidator(CVT_LDAP, validate_ldap_credentials);
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
 * CVT_OAUTH validator: asks the loaded OAuth validator module, through its
 * optional expire_cb, whether the bearer token the session authenticated
 * with has expired.  A module without expire_cb (including every V1 module)
 * is assumed to hold a still-valid token.
 */
static bool
validate_oauth_credentials(void)
{
	if (!CheckOAuthValidatorExpiration())
	{
		SetCredentialValidationFailureDetail("OAuth token check failed for user \"%s\": token has expired",
											 MyProcPort ? MyProcPort->user_name : "?");
		return false;
	}

	return true;
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

/*
 * CVT_GSS validator: the GSS security context established at authentication
 * has a lifetime that is fixed when the context is set up.  It is bounded by
 * the lifetime of the Kerberos ticket the client presented and by the allowed
 * clock skew.  The server never sees the ticket again after authentication,
 * so it is the context that is checked, not the ticket.  Once that lifetime
 * has elapsed the credential that authenticated the session is no longer
 * valid.  This is checked locally from the context retained on the Port, with
 * no round trip to the KDC.  A session without a GSS context is treated as
 * valid: there is nothing GSS-specific to check.
 *
 * Where MIT Kerberos is available the KDC is then also asked whether the
 * principal is still enabled (see CheckKerberosPrincipalEnabled()), which
 * does make a round trip, bounded by credential_validation_kdc_timeout.
 */
static bool
validate_gss_credentials(void)
{
#ifdef ENABLE_GSS
	Port	   *port = MyProcPort;

	if (port == NULL)
		return true;

	if (be_gssapi_get_context_expired(port))
	{
		SetCredentialValidationFailureDetail("GSSAPI check failed for user \"%s\": security context has expired",
											 port->user_name);
		return false;
	}

	/* The principal may also have been disabled or removed at the KDC */
	{
		char		reason[256];

		if (!CheckKerberosPrincipalEnabled(port, reason, sizeof(reason)))
		{
			SetCredentialValidationFailureDetail("GSSAPI check failed for user \"%s\": the KDC no longer accepts the Kerberos principal (%s)",
												 port->user_name, reason);
			return false;
		}
	}
#endif

	return true;
}

/*
 * CVT_LDAP validator: an LDAP session keeps no credential with an expiry of
 * its own, because the password is used for the bind and then discarded.  What
 * can be re-checked is whether the account still exists in the directory and
 * still satisfies the configured search filter.  CheckLDAPCredentialValidity()
 * re-runs that search; see the comment there for its scope and limits.
 *
 * Note that this makes a round trip to the LDAP server on every validation
 * cycle, so a larger credential_validation_interval is advisable for LDAP
 * sessions.
 */
static bool
validate_ldap_credentials(void)
{
#ifdef USE_LDAP
	Port	   *port = MyProcPort;

	if (port == NULL)
		return true;

	if (!CheckLDAPCredentialValidity(port))
	{
		SetCredentialValidationFailureDetail("LDAP check failed for user \"%s\": account no longer exists in the directory",
											 port->user_name);
		return false;
	}
#endif

	return true;
}
