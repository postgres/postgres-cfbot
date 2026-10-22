/*
 * libpq_trace_regress.c
 *		A test program for libpq's protocol message tracing
 *
 * This #includes fe-trace.c, so that it can call pqTraceOutputMessage()
 * directly.  It traces one message, built from its arguments, in a buffer of
 * exactly its size, so that a memory checker such as AddressSanitizer catches
 * any read past its end.  Without one, the test still checks the output.
 *
 * Usage: libpq_trace_regress F|B TYPE HEXDATA
 *
 * F or B says whether the message goes to the server or comes from it, TYPE
 * is its type byte and HEXDATA its contents in hex (spaces are ignored).  The
 * length word is filled in to match the contents.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/interfaces/libpq/test/libpq_trace_regress.c
 */

#include "fe-trace.c"

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int
main(int argc, char *argv[])
{
	PGconn		conn;
	const char *hex;
	char	   *message;
	int			len = 0;
	uint32		n32;

	if (argc != 4 || (strcmp(argv[1], "F") != 0 && strcmp(argv[1], "B") != 0) ||
		strlen(argv[2]) != 1)
	{
		fprintf(stderr, "usage: %s F|B TYPE HEXDATA\n", argv[0]);
		return 1;
	}

	/* Type byte, length word, then the contents */
	message = malloc(1 + 4 + strlen(argv[3]) / 2);
	if (message == NULL)
	{
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	for (hex = argv[3]; *hex; hex++)
	{
		if (*hex == ' ')
			continue;
		if (hexval(hex[0]) < 0 || hex[1] == '\0' || hexval(hex[1]) < 0)
		{
			fprintf(stderr, "invalid hex data \"%s\"\n", argv[3]);
			return 1;
		}
		message[1 + 4 + len++] = hexval(hex[0]) * 16 + hexval(hex[1]);
		hex++;
	}
	message[0] = argv[2][0];
	n32 = pg_hton32(4 + len);
	memcpy(message + 1, &n32, 4);

	/* Make the buffer exactly as long as the message */
	message = realloc(message, 1 + 4 + len);
	if (message == NULL)
	{
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	memset(&conn, 0, sizeof(conn));
	conn.Pfdebug = stdout;
	conn.traceFlags = PQTRACE_SUPPRESS_TIMESTAMPS;

	pqTraceOutputMessage(&conn, message, argv[1][0] == 'F');

	free(message);
	return 0;
}
