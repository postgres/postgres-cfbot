/*-------------------------------------------------------------------------
 *
 * test_checksum_helper.c
 *	  Test checksum helper error handling using a mock cryptohash provider
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_combinebackup/test_checksum_helper.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/checksum_helper.h"

struct pg_cryptohash_ctx
{
	pg_cryptohash_type type;
};

static bool fail_init;
static bool fail_final;
static int free_count;

pg_cryptohash_ctx *
pg_cryptohash_create(pg_cryptohash_type type)
{
	pg_cryptohash_ctx *ctx = malloc(sizeof(*ctx));

	if (ctx != NULL)
		ctx->type = type;
	return ctx;
}

int
pg_cryptohash_init(pg_cryptohash_ctx *ctx)
{
	(void) ctx;
	return fail_init ? -1 : 0;
}

int
pg_cryptohash_update(pg_cryptohash_ctx *ctx, const uint8 *data, size_t len)
{
	(void) ctx;
	(void) data;
	(void) len;
	return 0;
}

int
pg_cryptohash_final(pg_cryptohash_ctx *ctx, uint8 *dest, size_t len)
{
	(void) ctx;
	if (fail_final)
		return -1;
	memset(dest, 0, len);
	return 0;
}

void
pg_cryptohash_free(pg_cryptohash_ctx *ctx)
{
	free_count++;
	free(ctx);
}

const char *
pg_cryptohash_error(pg_cryptohash_ctx *ctx)
{
	(void) ctx;
	return "injected failure";
}

static bool
check(bool condition, pg_checksum_type type, const char *message)
{
	if (condition)
		return true;

	fprintf(stderr, "checksum type %d: %s\n", type, message);
	return false;
}

int
main(void)
{
	const struct
	{
		pg_checksum_type type;
		int			digest_length;
	} sha_types[] = {
		{CHECKSUM_TYPE_SHA224, PG_SHA224_DIGEST_LENGTH},
		{CHECKSUM_TYPE_SHA256, PG_SHA256_DIGEST_LENGTH},
		{CHECKSUM_TYPE_SHA384, PG_SHA384_DIGEST_LENGTH},
		{CHECKSUM_TYPE_SHA512, PG_SHA512_DIGEST_LENGTH}
	};
	uint8		output[PG_CHECKSUM_MAX_LENGTH];
	bool		success = true;

	for (size_t i = 0; i < lengthof(sha_types); i++)
	{
		pg_checksum_context context;
		pg_checksum_type type = sha_types[i].type;

		fail_init = true;
		fail_final = false;
		free_count = 0;
		success &= check(pg_checksum_init(&context, type) == -1,
						 type, "init failure was not returned");
		success &= check(context.raw_context.c_sha2 == NULL,
						 type, "init failure did not clear context");
		success &= check(free_count == 1,
						 type, "init failure did not free context once");

		fail_init = false;
		fail_final = true;
		free_count = 0;
		success &= check(pg_checksum_init(&context, type) == 0,
						 type, "initialization failed unexpectedly");
		success &= check(pg_checksum_final(&context, output) == -1,
						 type, "final failure was not returned");
		success &= check(context.raw_context.c_sha2 == NULL,
						 type, "final failure did not clear context");
		success &= check(free_count == 1,
						 type, "final failure did not free context once");

		fail_final = false;
		free_count = 0;
		success &= check(pg_checksum_init(&context, type) == 0,
						 type, "initialization failed unexpectedly");
		success &= check(pg_checksum_final(&context, output) ==
						 sha_types[i].digest_length,
						 type, "finalization returned the wrong digest length");
		success &= check(context.raw_context.c_sha2 == NULL,
						 type, "successful finalization did not clear context");
		success &= check(free_count == 1,
						 type, "successful finalization did not free context once");
	}

	return success ? 0 : 1;
}
