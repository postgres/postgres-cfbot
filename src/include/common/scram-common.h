/*-------------------------------------------------------------------------
 *
 * scram-common.h
 *		Declarations for helper functions used for SCRAM authentication
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/scram-common.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SCRAM_COMMON_H
#define SCRAM_COMMON_H

#include "common/cryptohash.h"
#include "common/sha2.h"

/* Name of SCRAM mechanisms per IANA */
#define SCRAM_SHA_256_NAME "SCRAM-SHA-256"
#define SCRAM_SHA_256_PLUS_NAME "SCRAM-SHA-256-PLUS"	/* with channel binding */

/* Length of SCRAM keys (client and server) */
#define SCRAM_SHA_256_KEY_LEN				PG_SHA256_DIGEST_LENGTH

/*
 * Size of buffers used internally by SCRAM routines, that should be the
 * maximum of SCRAM_SHA_*_KEY_LEN among the hash methods supported.
 */
#define SCRAM_MAX_KEY_LEN					SCRAM_SHA_256_KEY_LEN

/*
 * Size of random nonce generated in the authentication exchange.  This
 * is in "raw" number of bytes, the actual nonces sent over the wire are
 * encoded using only ASCII-printable characters.
 */
#define SCRAM_RAW_NONCE_LEN			18

/*
 * Length of salt when generating new secrets, in bytes.  (It will be stored
 * and sent over the wire encoded in Base64.)  16 bytes is what the example in
 * RFC 7677 uses.
 */
#define SCRAM_DEFAULT_SALT_LEN		16

/*
 * Default number of iterations when generating secret.  Should be at least
 * 4096 per RFC 7677.
 */
#define SCRAM_SHA_256_DEFAULT_ITERATIONS	4096

/*
 * Optional caller-supplied interrupt check for scram_SaltedPasswordExt().
 *
 * Called periodically from within the PBKDF2 iteration loop in frontend
 * builds.  Return true to abort, false to continue iterating.  On abort,
 * set *errstr to the error message to surface to the caller of
 * scram_SaltedPasswordExt().  arg is opaque caller-owned state.
 *
 * The SCRAM code knows nothing about the policy behind the check.  It
 * simply offers a hook analogous to the backend's CHECK_FOR_INTERRUPTS().
 */
typedef bool (*scram_interrupt_callback) (void *arg, const char **errstr);

/*
 * Original entry point.  Equivalent to scram_SaltedPasswordExt() with
 * NULL callback arguments.  Kept so existing callers do not need to change.
 */
extern int	scram_SaltedPassword(const char *password,
								 pg_cryptohash_type hash_type, int key_length,
								 const uint8 *salt, int saltlen, int iterations,
								 uint8 *result, const char **errstr);

extern int	scram_SaltedPasswordExt(const char *password,
									pg_cryptohash_type hash_type, int key_length,
									const uint8 *salt, int saltlen, int iterations,
									scram_interrupt_callback interrupt_cb,
									void *interrupt_arg,
									uint8 *result, const char **errstr);
extern int	scram_H(const uint8 *input, pg_cryptohash_type hash_type,
					int key_length, uint8 *result,
					const char **errstr);
extern int	scram_ClientKey(const uint8 *salted_password,
							pg_cryptohash_type hash_type, int key_length,
							uint8 *result, const char **errstr);
extern int	scram_ServerKey(const uint8 *salted_password,
							pg_cryptohash_type hash_type, int key_length,
							uint8 *result, const char **errstr);

extern char *scram_build_secret(pg_cryptohash_type hash_type, int key_length,
								const uint8 *salt, int saltlen, int iterations,
								const char *password, const char **errstr);

extern bool scram_parse_iterations(const char *str, int *iterations);

#endif							/* SCRAM_COMMON_H */
