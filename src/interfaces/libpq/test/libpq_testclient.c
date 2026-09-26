/*
 * libpq_testclient.c
 *		A test program for the libpq public API
 *
 * Copyright (c) 2022-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/interfaces/libpq/test/libpq_testclient.c
 */

#include "postgres_fe.h"

#include "libpq-fe.h"

static void
print_ssl_library(void)
{
	const char *lib = PQsslAttribute(NULL, "library");

	if (!lib)
		fprintf(stderr, "SSL is not enabled\n");
	else
		printf("%s\n", lib);
}

/*
 * Print the compiled-in defaults that the passfile lookup falls back to,
 * for use by the TAP test.
 */
static void
print_passfile_defaults(void)
{
	printf("%s\n%s\n", DEF_PGPORT_STR, DEFAULT_PGSOCKET_DIR);
}

/*
 * Look up a password with PQpassfileLookup().  The arguments are passfile,
 * hostname, port, dbname and username; an argument of "-" is passed as
 * NULL, and an argument of "=" as an empty string (an empty command-line
 * argument cannot be relied on to survive process spawning everywhere).
 */
static int
test_passfile_lookup(int argc, char *argv[])
{
	const char *args[5];
	char	   *password;

	if (argc != 7)
	{
		fprintf(stderr, "usage: libpq_testclient --passfile PASSFILE HOSTNAME PORT DBNAME USERNAME\n");
		return 1;
	}

	for (int i = 0; i < 5; i++)
	{
		if (strcmp(argv[i + 2], "-") == 0)
			args[i] = NULL;
		else if (strcmp(argv[i + 2], "=") == 0)
			args[i] = "";
		else
			args[i] = argv[i + 2];
	}

	password = PQpassfileLookup(args[1], args[2], args[3], args[4], args[0]);

	if (!password)
	{
		fprintf(stderr, "no password found\n");
		return 1;
	}

	printf("%s\n", password);
	PQfreemem(password);
	return 0;
}

int
main(int argc, char *argv[])
{
	if ((argc > 1) && !strcmp(argv[1], "--ssl"))
	{
		print_ssl_library();
		return 0;
	}
	else if ((argc > 1) && !strcmp(argv[1], "--passfile"))
		return test_passfile_lookup(argc, argv);
	else if ((argc > 1) && !strcmp(argv[1], "--passfile-defaults"))
	{
		print_passfile_defaults();
		return 0;
	}

	printf("currently only --ssl, --passfile and --passfile-defaults are supported\n");
	return 1;
}
