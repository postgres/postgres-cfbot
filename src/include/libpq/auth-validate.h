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

/* Define credential validation method types as an enum */
typedef enum CredentialValidationType
{
	CVT_OAUTH = 0,				/* OAuth bearer token authentication */
	CVT_CERT,					/* TLS client certificate authentication */
	CVT_GSS,					/* GSSAPI/Kerberos authentication */
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

/* Enable credential validation timeout timer */
extern void EnableCredentialValidationTimeout(void);

#endif							/* AUTH_VALIDATE_H */
