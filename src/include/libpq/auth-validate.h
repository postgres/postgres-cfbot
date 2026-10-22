/*-------------------------------------------------------------------------
 *
 * auth-validate.h
 *	  Interface for authentication credential validation
 *
 * This file provides a common interface for validating credentials
 * during an active PostgreSQL session.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/auth-validate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTH_VALIDATE_H
#define AUTH_VALIDATE_H

/*
 * Define credential validation method types as an enum.  Enumerators use
 * the "CVT_" prefix, short for CredentialValidationType.
 */
typedef enum CredentialValidationType
{
	CVT_OAUTH = 0,				/* OAuth bearer token authentication */
	CVT_CERT,					/* TLS client certificate authentication */
	CVT_COUNT					/* Total number of credential validation types */
} CredentialValidationType;

/* Process credential validation */
extern void ProcessCredentialValidation(void);

/* GUC variables */
extern PGDLLIMPORT bool credential_validation_enabled;
extern PGDLLIMPORT int credential_validation_interval;

/* Common credential validation callback prototype */
typedef bool (*CredentialValidationCallback) (void);

/* Initialize credential validation system */
extern void InitializeCredentialValidation(void);

/* Register a validation callback for a specific authentication method */
extern void RegisterCredentialValidator(CredentialValidationType method_type,
										CredentialValidationCallback validator);

/*
 * Check credential validity for the current session.  Returns true if the
 * credentials are still valid, false if they have expired.  Must be called
 * within a transaction, since the validators read the system catalogs.
 */
extern bool CheckCredentialValidity(void);

/*
 * Records why validation failed (log only, never sent to client), for
 * ProcessCredentialValidation() to include via errdetail_log().  Callable
 * more than once per cycle; the most recent call wins.
 */
extern void SetCredentialValidationFailureDetail(const char *fmt,...) pg_attribute_printf(1, 2);

/* Enable credential validation timeout timer with a fresh full interval */
extern void EnableCredentialValidationTimeout(void);

/* Re-arm the timer after error recovery without moving the deadline */
extern void RearmCredentialValidationTimeout(void);

#endif							/* AUTH_VALIDATE_H */
