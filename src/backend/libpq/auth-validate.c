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
#include "libpq/auth-validate-methods.h"
#include "libpq/auth-validate.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* GUC variables */
bool		credential_validation_enabled;
int			credential_validation_interval;


/* Registered credential validators */
static CredentialValidationCallback validators[CVT_COUNT];

/*
 * Why the last validation check failed (log only, never sent to the
 * client).  Set via SetCredentialValidationFailureDetail(), reset each
 * cycle, read by ProcessCredentialValidation().
 */
#define CREDENTIAL_VALIDATION_DETAIL_LEN 256
static char credential_validation_detail[CREDENTIAL_VALIDATION_DETAIL_LEN];

void
SetCredentialValidationFailureDetail(const char *fmt,...)
{
	va_list		args;

	va_start(args, fmt);
	vsnprintf(credential_validation_detail, sizeof(credential_validation_detail), fmt, args);
	va_end(args);
}

/*
 * Deadline for the next cycle, tracked separately so error recovery can
 * re-arm at this deadline (not a fresh interval) and not be gamed by
 * repeated errors.
 */
static TimestampTz next_validation_deadline = 0;


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
		default:
			/*
			 * No validator for password/md5/scram; their only check is
			 * the baseline one (ValidateRoleValidity()).
			 */
			return CVT_COUNT;	/* Invalid value */
	}
}

/*
 * Runs a full validity check when a validation cycle is due; FATALs the
 * session if credentials have expired.
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
	 * Validators read the catalogs, which needs a live transaction; skip
	 * and retry next interval if we're in an aborted transaction block.
	 */
	if (IsAbortedTransactionBlockState())
		return;

	/*
	 * Start a short-lived transaction if none is open; reuse (but don't
	 * commit) an existing one, so validation still runs at each command
	 * boundary inside a transaction block.
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
				 errdetail_log("%s", credential_validation_detail),
				 errhint("Please reconnect to establish a new authenticated session.")));
}

/*
 * Called from InitPostgres() after authentication; registers all
 * method-specific validators.
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
 * Arms the validation timer with a fresh interval and sets the next
 * deadline.  Called at session start and after each validation cycle.
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

	/*
	 * Physical walsenders never run SQL and never revisit the dispatch
	 * loop, so arming this would be a no-op; logical (db-connected) ones
	 * do run SQL and are serviced via WalSndHandleCredentialValidation().
	 */
	if (AmWalSenderProcess() && !am_db_walsender)
		return;

	/* Convert interval from seconds to milliseconds */
	interval_ms = credential_validation_interval * 1000;

	next_validation_deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), interval_ms);

	enable_timeout_after(CREDENTIAL_VALIDATION_TIMEOUT, interval_ms);

	elog(DEBUG1, "credential validation timeout enabled, interval=%d s", credential_validation_interval);
}

/*
 * Re-arms the timer after error recovery cancels it, at the previously
 * established deadline (not a fresh interval) -- an already-elapsed
 * deadline fires almost immediately, so repeated errors can't delay this.
 */
void
RearmCredentialValidationTimeout(void)
{
	if (!credential_validation_enabled)
		return;

	if (!IsExternalConnectionBackend(MyBackendType))
		return;

	/* See the matching check in EnableCredentialValidationTimeout(). */
	if (AmWalSenderProcess() && !am_db_walsender)
		return;

	/* No deadline established yet (shouldn't normally happen); start over. */
	if (next_validation_deadline == 0)
	{
		EnableCredentialValidationTimeout();
		return;
	}

	enable_timeout_at(CREDENTIAL_VALIDATION_TIMEOUT, next_validation_deadline);
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
 * Returns true if the session's credentials are still valid. Must be
 * called within a transaction, since validators read the catalogs.
 */
bool
CheckCredentialValidity(void)
{
	CredentialValidationCallback validator = NULL;
	CredentialValidationType validation_type;
	bool		result;

	/*
	 * Nothing to validate during shutdown, for non-client backends, for
	 * physical walsenders (out of scope; logical ones are handled), or
	 * mid-authentication.  Hot standby sessions are NOT skipped.
	 */
	if (proc_exit_inprogress ||
		!IsExternalConnectionBackend(MyBackendType) ||
		(AmWalSenderProcess() && !am_db_walsender) ||
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

	/* Discard any leftover detail from a previous, unrelated check. */
	credential_validation_detail[0] = '\0';

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
	{
		result = validator();

		/* Fallback in case a validator forgot to set its own detail. */
		if (!result && credential_validation_detail[0] == '\0')
			SetCredentialValidationFailureDetail("method-specific credential check failed for user \"%s\" (auth method %d)",
												  MyProcPort ? MyProcPort->user_name : "?",
												  (int) MyClientConnectionInfo.auth_method);
	}

	return result;
}
