/*-------------------------------------------------------------------------
 *
 * auth-validate.c
 *	  Implementation of authentication credential validation
 *
 * This module provides a mechanism for validating credentials during
 * an active PostgreSQL session.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/libpq/auth-validate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "libpq/auth-validate-methods.h"
#include "libpq/auth-validate.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "utils/timeout.h"

/* GUC variables */
bool		credential_validation_enabled;
int			credential_validation_interval;


/* Registered credential validators */
static CredentialValidationCallback validators[CVT_COUNT];


/*
 * Convert UserAuth enum to CredentialValidationType for validator selection
 */
static CredentialValidationType
UserAuthToValidationType(UserAuth auth_method)
{
	switch (auth_method)
	{
		case uaOAuth:
			return CVT_OAUTH;
		case uaCert:
			return CVT_CERT;
		case uaGSS:
			return CVT_GSS;
		case uaLDAP:
			return CVT_LDAP;
		default:
			/*
			 * No method-specific validator for other auth methods.  Password
			 * methods (password/md5/scram) fall here intentionally: their only
			 * credential check is role-level (rolvaliduntil), which is handled
			 * for every session by the baseline ValidateRoleValidity().
			 */
			return CVT_COUNT;	/* Invalid value */
	}
}

/*
 * ProcessCredentialValidation
 *
 * Called from the main command loop when a credential validation cycle is
 * due.  Runs a full validity check and terminates the session with FATAL if
 * the credentials have expired.
 */
void
ProcessCredentialValidation(void)
{
	bool		valid;
	bool		own_xact = false;

	if (ClientAuthInProgress || IsInitProcessingMode() || IsBootstrapProcessingMode())
		return;

	if (!credential_validation_enabled || MyClientConnectionInfo.authn_id == NULL)
		return;

	/*
	 * The validators read the system catalogs, which requires a live,
	 * non-aborted transaction.  In an aborted transaction block catalog
	 * access is not possible, so skip this cycle and retry at the next
	 * interval.
	 */
	if (IsAbortedTransactionBlockState())
		return;

	/*
	 * Between commands there is no transaction, so start a short-lived one of
	 * our own.  Inside an open transaction block (or a multi-message
	 * extended-query sequence) reuse the existing transaction, so that
	 * validation still happens at each command boundary within the block; but
	 * do not commit it, since it belongs to the user.
	 */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		own_xact = true;
	}

	valid = CheckCredentialValidity();

	if (own_xact)
		CommitTransactionCommand();

	if (!valid)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("session credentials have expired"),
				 errhint("Please reconnect to establish a new authenticated session.")));
}

/*
 * InitializeCredentialValidation
 *
 * Called from InitPostgres after authentication completes.  Registers all
 * method-specific validation callbacks.
 */
void
InitializeCredentialValidation(void)
{
	int			i;

	/* Initialize validator callbacks to NULL */
	for (i = 0; i < CVT_COUNT; i++)
		validators[i] = NULL;

	/* Register all method-specific validation callbacks */
	InitializeValidationMethods();
}

/*
 * Enable or re-enable the credential validation timeout timer.
 * Called at session startup and after each validation or error recovery.
 */
void
EnableCredentialValidationTimeout(void)
{
	int			interval_ms;

	/* Only enable if credential validation is configured */
	if (!credential_validation_enabled)
		return;

	/* Skip for non-client backends */
	if (!IsExternalConnectionBackend(MyBackendType))
		return;

	/* Convert interval from seconds to milliseconds */
	interval_ms = credential_validation_interval * 1000;

	enable_timeout_after(CREDENTIAL_VALIDATION_TIMEOUT, interval_ms);

	elog(DEBUG1, "credential validation timeout enabled, interval=%d s", credential_validation_interval);
}

/*
 * Register a validator callback for a specific authentication method
 */
void
RegisterCredentialValidator(CredentialValidationType method_type, CredentialValidationCallback validator)
{
	if (method_type < 0 || method_type >= CVT_COUNT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid validation method type: %d", method_type)));

	validators[method_type] = validator;
}

/*
 * Check credential validity for the current session.
 *
 * Returns true if the credentials are still valid, false if they have expired.
 * Must be called within a transaction (the validators read the catalogs).
 */
bool
CheckCredentialValidity(void)
{
	CredentialValidationCallback validator = NULL;
	CredentialValidationType validation_type;
	bool		result;

	/*
	 * Skip validation (treat as valid) for any process that does not have a
	 * client session with credentials to validate:
	 * - during shutdown or recovery
	 * - non-client backends (autovacuum, background workers, etc.)
	 * - while authentication is still in progress
	 */
	if (proc_exit_inprogress ||
		RecoveryInProgress() ||
		!IsExternalConnectionBackend(MyBackendType) ||
		AmAutoVacuumLauncherProcess() ||
		AmAutoVacuumWorkerProcess() ||
		AmBackgroundWorkerProcess() ||
		ClientAuthInProgress)
		return true;

	/* Without an authenticated session there is nothing to validate. */
	if (MyClientConnectionInfo.authn_id == NULL)
		return true;

	elog(DEBUG1, "credential validation: checking auth_method=%d",
		 (int) MyClientConnectionInfo.auth_method);

	/*
	 * Role-level validity (rolvaliduntil / role existence) is a baseline that
	 * applies to every authenticated session, regardless of auth method.
	 */
	result = ValidateRoleValidity();

	/*
	 * Additionally run the method-specific validator if one is registered for
	 * this auth method (e.g. OAuth token expiry, client certificate expiry).
	 */
	validation_type = UserAuthToValidationType(MyClientConnectionInfo.auth_method);
	if (validation_type < CVT_COUNT)
		validator = validators[validation_type];

	if (result && validator != NULL)
		result = validator();

	return result;
}
