/*
 * libpq_testclient.c
 *		A test program for libpq and the frontend/backend protocol
 *
 * Copyright (c) 2022-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/interfaces/libpq/test/libpq_testclient.c
 */

#include "postgres_fe.h"

#include <limits.h>

#include "libpq-fe.h"

#ifdef USE_ZSTD
#include "libpq/protocol_compression.h"

#include "libpq-int.h"
#include "port/pg_bswap.h"
#endif

static void
print_ssl_library(void)
{
	const char *lib = PQsslAttribute(NULL, "library");

	if (!lib)
		fprintf(stderr, "SSL is not enabled\n");
	else
		printf("%s\n", lib);
}

static int
test_compression_reset(const char *conninfo)
{
	PGconn	   *conn = PQconnectdb(conninfo);
	PGresult   *res;

	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQfinish(conn);
		return 1;
	}
#ifdef USE_ZSTD
	res = PQexec(conn, "SELECT repeat('before reset ', 1000)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
	if (conn->compression_dctx == NULL ||
		conn->decompressBuffer.start != conn->decompressBuffer.end)
	{
		fprintf(stderr, "server response was not compressed\n");
		PQfinish(conn);
		return 1;
	}

	/*
	 * Model an unfinished compressed DataRow without relying on socket
	 * timing.
	 */
	if (conn->decompressBuffer.buffer == NULL ||
		conn->decompressBuffer.bufSize < 1)
	{
		fprintf(stderr, "out of memory\n");
		PQfinish(conn);
		return 1;
	}
	conn->decompressBuffer.buffer[0] = PqMsg_DataRow;
	conn->decompressBuffer.start = 0;
	conn->decompressBuffer.end = 1;
	conn->compression_in_frame = true;
#else
	res = PQexec(conn, "SELECT repeat('x', 17 * 1024 * 1024)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQgetlength(res, 0, 0) != 17 * 1024 * 1024)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
#endif
	PQreset(conn);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQfinish(conn);
		return 1;
	}
#ifdef USE_ZSTD
	if (conn->compression_dctx != NULL ||
		conn->compression_buffers_initialized ||
		conn->compression_in_frame)
	{
		fprintf(stderr, "PQreset did not discard compression state\n");
		PQfinish(conn);
		return 1;
	}
#endif
	res = PQexec(conn, "SELECT repeat('after reset ', 1000)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
#ifdef USE_ZSTD
	if (conn->compression_dctx == NULL)
	{
		fprintf(stderr, "server response after PQreset was not compressed\n");
		PQfinish(conn);
		return 1;
	}
#endif
	PQfinish(conn);
	return 0;
}

#ifdef USE_ZSTD
static PGconn *
compression_connect(const char *conninfo)
{
	PGconn	   *conn = PQconnectdb(conninfo);

	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}
	if (PQsslInUse(conn) || PQgssEncInUse(conn))
	{
		fprintf(stderr, "raw protocol test requires an unencrypted connection\n");
		PQfinish(conn);
		return NULL;
	}
	return conn;
}

static int
test_compression_large_datarow(const char *conninfo)
{
	const int	data_size = 17 * 1024 * 1024;
	PGconn	   *conn;
	PGresult   *res = NULL;
	const char *data;
	int			i;
	int			status = 1;

	conn = compression_connect(conninfo);
	if (conn == NULL)
		return 1;

	res = PQexec(conn, "SELECT repeat('x', 17 * 1024 * 1024)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 || PQnfields(res) != 1 ||
		PQgetisnull(res, 0, 0) || PQgetlength(res, 0, 0) != data_size)
		goto done;

	data = PQgetvalue(res, 0, 0);
	for (i = 0; i < data_size; i++)
	{
		if (data[i] != 'x')
			goto done;
	}

	/* Creating the decompressor proves that a wrapper was received. */
	if (conn->compression_dctx == NULL)
		goto done;

	status = 0;

done:
	if (status != 0)
		fprintf(stderr, "large compressed DataRow was not preserved: %s",
				PQerrorMessage(conn));
	PQclear(res);
	PQfinish(conn);
	return status;
}

static int
send_raw_bytes(PGconn *conn, const char *data, size_t len)
{
	int			sock = PQsocket(conn);

	while (len > 0)
	{
		int			flags = 0;
		int			sent;

#ifdef MSG_NOSIGNAL
		flags |= MSG_NOSIGNAL;
#endif
		sent = send(sock, data, (int) Min(len, (size_t) INT_MAX), flags);
		if (sent <= 0)
			return 1;
		data += sent;
		len -= sent;
	}
	return 0;
}

/* Send a complete or deliberately unfinished frame containing one CopyData. */
static int
send_compressed_copy_frame(PGconn *conn, const char *data, size_t len,
						   bool finish)
{
	char	   *input_data;
	char	   *wire_data;
	size_t		input_size = len + 5;
	size_t		output_size = ZSTD_compressBound(input_size) +
		ZSTD_CStreamOutSize() + 64;
	ZSTD_CCtx  *cctx = NULL;
	ZSTD_inBuffer input;
	ZSTD_outBuffer output;
	size_t		result;
	uint32		n32;
	int			status = 1;

	input_data = malloc(input_size);
	wire_data = malloc(output_size + 5);
	if (input_data == NULL || wire_data == NULL)
		goto done;
	input_data[0] = PqMsg_CopyData;
	n32 = pg_hton32((uint32) len + 4);
	memcpy(input_data + 1, &n32, 4);
	memcpy(input_data + 5, data, len);

	cctx = ZSTD_createCCtx();
	if (cctx == NULL)
		goto done;
	result = ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 16);
	if (ZSTD_isError(result))
		goto done;

	input.src = input_data;
	input.size = input_size;
	input.pos = 0;
	output.dst = wire_data + 5;
	output.size = output_size;
	output.pos = 0;
	do
	{
		result = ZSTD_compressStream2(cctx, &output, &input,
									  finish ? ZSTD_e_end : ZSTD_e_flush);
		if (ZSTD_isError(result))
			goto done;
	} while (result != 0);

	wire_data[0] = PqMsg_CompressedData;
	n32 = pg_hton32((uint32) output.pos + 4);
	memcpy(wire_data + 1, &n32, 4);
	status = send_raw_bytes(conn, wire_data, output.pos + 5);

done:
	ZSTD_freeCCtx(cctx);
	free(input_data);
	free(wire_data);
	return status;
}

static int
start_copy(PGconn *conn)
{
	PGresult   *res;

	res = PQexec(conn, "CREATE TEMP TABLE compression_frame (data text); "
				 "COPY compression_frame FROM STDIN");
	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		PQclear(res);
		return 1;
	}
	PQclear(res);
	return 0;
}

static int
expect_copy_result(PGconn *conn, ExecStatusType expected)
{
	PGresult   *res;
	bool		matched = false;
	bool		unexpected = false;

	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) == expected)
			matched = true;
		else
			unexpected = true;
		PQclear(res);
	}
	return matched && !unexpected ? 0 : 1;
}

static int
expect_protocol_rejection(PGconn *conn)
{
	if (PQputCopyEnd(conn, NULL) != 1)
		return PQstatus(conn) == CONNECTION_BAD ? 0 : 1;
	return expect_copy_result(conn, PGRES_FATAL_ERROR);
}

static int
test_compression_frame_boundaries(const char *conninfo)
{
	PGconn	   *conn;
	PGresult   *res;
	int			status = 1;

	/* The frame epilogue may share a wrapper with the final CopyData. */
	conn = compression_connect(conninfo);
	if (conn == NULL)
		return 1;
	if (start_copy(conn) ||
		send_compressed_copy_frame(conn, "complete\n", strlen("complete\n"),
								   true) ||
		PQputCopyEnd(conn, NULL) != 1 ||
		expect_copy_result(conn, PGRES_COMMAND_OK))
		goto done;
	res = PQexec(conn, "SELECT data FROM compression_frame");
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 || strcmp(PQgetvalue(res, 0, 0), "complete") != 0)
	{
		PQclear(res);
		goto done;
	}
	PQclear(res);
	PQfinish(conn);

	/* CopyDone cannot terminate an unfinished compressed frame. */
	conn = compression_connect(conninfo);
	if (conn == NULL)
		return 1;
	if (start_copy(conn) ||
		send_compressed_copy_frame(conn, "unfinished\n",
								   strlen("unfinished\n"), false) ||
		expect_protocol_rejection(conn))
		goto done;
	PQfinish(conn);

	/* A second frame cannot start before the COPY protocol boundary. */
	conn = compression_connect(conninfo);
	if (conn == NULL)
		return 1;
	if (start_copy(conn) ||
		send_compressed_copy_frame(conn, "first\n", strlen("first\n"), true) ||
		send_compressed_copy_frame(conn, "second\n", strlen("second\n"), true) ||
		expect_protocol_rejection(conn))
		goto done;

	status = 0;

done:
	if (status != 0)
		fprintf(stderr, "%s", PQerrorMessage(conn));
	PQfinish(conn);
	return status;
}

static int
test_compression_incompressible_copy(const char *conninfo)
{
	const int	data_size = PQ_COMPRESSION_MAX_SEGMENT_SIZE + 1;
	const size_t copy_size = (size_t) data_size + 27;
	PGconn	   *conn = PQconnectdb(conninfo);
	PGresult   *res = NULL;
	char	   *copy_data = NULL;
	pg_prng_state state;
	uint16		n16;
	uint32		n32;
	size_t		offset;
	int			status = 1;

	if (PQstatus(conn) != CONNECTION_OK)
		goto done;
	copy_data = malloc(copy_size);
	if (copy_data == NULL)
		goto done;

	/* Binary COPY header, one bytea field, and the end-of-data marker. */
	memcpy(copy_data, "PGCOPY\n\377\r\n", 11);
	memset(copy_data + 11, 0, 8);
	n16 = pg_hton16(1);
	memcpy(copy_data + 19, &n16, 2);
	n32 = pg_hton32(data_size);
	memcpy(copy_data + 21, &n32, 4);
	pg_prng_seed(&state, 42);
	for (offset = 0; offset < (size_t) data_size; offset += sizeof(uint64))
	{
		uint64		value = pg_prng_uint64(&state);
		size_t		len = Min(sizeof(value), (size_t) data_size - offset);

		memcpy(copy_data + 25 + offset, &value, len);
	}
	n16 = pg_hton16(0xffff);
	memcpy(copy_data + 25 + data_size, &n16, 2);

	res = PQexec(conn, "CREATE TEMP TABLE compression_copy_bytes (data bytea); "
				 "COPY compression_copy_bytes FROM STDIN (FORMAT binary)");
	if (PQresultStatus(res) != PGRES_COPY_IN)
		goto done;
	PQclear(res);
	res = NULL;
	if (PQputCopyData(conn, copy_data, (int) copy_size) != 1 ||
		PQputCopyEnd(conn, NULL) != 1 ||
		expect_copy_result(conn, PGRES_COMMAND_OK))
		goto done;

	res = PQexecParams(conn, "SELECT data FROM compression_copy_bytes",
					   0, NULL, NULL, NULL, NULL, 1);
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 || PQnfields(res) != 1 ||
		PQgetisnull(res, 0, 0) || PQgetlength(res, 0, 0) != data_size ||
		memcmp(PQgetvalue(res, 0, 0), copy_data + 25, data_size) != 0)
		goto done;
	status = 0;

done:
	if (status != 0)
		fprintf(stderr, "incompressible COPY round trip failed: %s",
				PQerrorMessage(conn));
	PQclear(res);
	free(copy_data);
	PQfinish(conn);
	return status;
}

static int
test_compression_pipeline(const char *conninfo)
{
	const char *values[1];
	PGconn	   *conn = PQconnectdb(conninfo);
	PGresult   *res;
	int			status = 1;

	if (PQstatus(conn) != CONNECTION_OK || PQenterPipelineMode(conn) != 1)
		goto done;
	values[0] = "1";
	if (PQsendQueryParams(conn,
						  "SELECT $1::integer, repeat('pipeline result ', 1000)",
						  1, NULL, values, NULL, NULL, 0) != 1)
		goto done;
	values[0] = "2";
	if (PQsendQueryParams(conn,
						  "SELECT $1::integer, repeat('pipeline result ', 1000)",
						  1, NULL, values, NULL, NULL, 0) != 1 ||
		PQpipelineSync(conn) != 1)
		goto done;

	res = PQgetResult(conn);
	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK ||
		strcmp(PQgetvalue(res, 0, 0), "1") != 0)
		goto done;
	PQclear(res);
	if (PQgetResult(conn) != NULL)
		goto done;
	res = PQgetResult(conn);
	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK ||
		strcmp(PQgetvalue(res, 0, 0), "2") != 0)
		goto done;
	PQclear(res);
	if (PQgetResult(conn) != NULL)
		goto done;
	res = PQgetResult(conn);
	if (res == NULL || PQresultStatus(res) != PGRES_PIPELINE_SYNC)
		goto done;
	PQclear(res);
	if (PQgetResult(conn) != NULL || PQexitPipelineMode(conn) != 1)
		goto done;

	/* Creating the decompressor proves that a wrapper was received. */
	if (conn->compression_dctx == NULL)
		goto done;
	status = 0;

done:
	if (status != 0)
		fprintf(stderr, "%s", PQerrorMessage(conn));
	PQfinish(conn);
	return status;
}
#endif

static int
test_compression_large_copy(const char *conninfo)
{
	const int	data_size = 17 * 1024 * 1024;
	PGconn	   *conn = PQconnectdb(conninfo);
	PGresult   *res;
	char	   *data;
	char		small_data[1025];
	char		trace_line[256];
	FILE	   *trace = NULL;
	bool		traced_compressed_data = false;
	int			status = 1;

	if (PQstatus(conn) != CONNECTION_OK)
		goto fail;
	res = PQexec(conn, "CREATE TEMP TABLE compression_copy (data text); "
				 "COPY compression_copy FROM STDIN");
	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		PQclear(res);
		goto fail;
	}
	PQclear(res);
	trace = tmpfile();
	if (trace == NULL)
		goto fail;
	PQtrace(conn, trace);
	PQsetTraceFlags(conn, PQTRACE_SUPPRESS_TIMESTAMPS | PQTRACE_REGRESS_MODE);

	data = malloc(data_size + 1);
	if (data == NULL)
		goto fail;
	memset(data, 'x', data_size);
	data[data_size] = '\n';
	if (PQputCopyData(conn, data, data_size + 1) != 1 ||
		PQputCopyEnd(conn, NULL) != 1)
	{
		free(data);
		goto fail;
	}
	free(data);
	PQuntrace(conn);
	rewind(trace);
	while (fgets(trace_line, sizeof(trace_line), trace) != NULL)
	{
		if (strstr(trace_line, "CompressedData") != NULL)
			traced_compressed_data = true;
		if (strstr(trace_line, "mismatched message length") != NULL)
			goto fail;
	}
	fclose(trace);
	trace = NULL;
	if (!traced_compressed_data)
		goto fail;
	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			PQclear(res);
			goto fail;
		}
		PQclear(res);
	}

	res = PQexec(conn, "SELECT count(*) = 1 AND "
				 "bool_and(data = repeat('x', 17 * 1024 * 1024)) "
				 "FROM compression_copy");
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		strcmp(PQgetvalue(res, 0, 0), "t") != 0)
	{
		PQclear(res);
		goto fail;
	}
	PQclear(res);

	/* Reuse the connection after large buffers were released at COPY end. */
	res = PQexec(conn, "COPY compression_copy FROM STDIN");
	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		PQclear(res);
		goto fail;
	}
	PQclear(res);
	memset(small_data, 'y', sizeof(small_data) - 1);
	small_data[sizeof(small_data) - 1] = '\n';
	if (PQputCopyData(conn, small_data, sizeof(small_data)) != 1 ||
		PQputCopyData(conn, "short\n", 6) != 1 ||
		PQputCopyData(conn, small_data, sizeof(small_data)) != 1 ||
		PQputCopyEnd(conn, NULL) != 1)
		goto fail;
	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			PQclear(res);
			goto fail;
		}
		PQclear(res);
	}
	res = PQexec(conn, "SELECT count(*) = 4 AND "
				 "count(*) FILTER (WHERE data = repeat('y', 1024)) = 2 AND "
				 "count(*) FILTER (WHERE data = 'short') = 1 "
				 "FROM compression_copy");
	if (PQresultStatus(res) == PGRES_TUPLES_OK &&
		strcmp(PQgetvalue(res, 0, 0), "t") == 0)
		status = 0;
	PQclear(res);

fail:
	if (trace != NULL)
	{
		PQuntrace(conn);
		fclose(trace);
	}
	if (status != 0)
		fprintf(stderr, "%s", PQerrorMessage(conn));
	PQfinish(conn);
	return status;
}

/* Exercise the message visibility and reset cases from the original series. */
static int
test_compression_trace(const char *conninfo, const char *filename)
{
	static const struct
	{
		const char *sql;
		ExecStatusType expected;
	}			commands[] = {
		{"BEGIN", PGRES_COMMAND_OK},
		{"SELECT repeat('a', 80)", PGRES_TUPLES_OK},
		{"SET application_name = 'compressed trace'", PGRES_COMMAND_OK},
		{"DO $$ BEGIN RAISE NOTICE 'trace notice'; END $$", PGRES_COMMAND_OK},
		{"SELECT repeat('b', 80), 1 / i FROM generate_series(2, 0, -1) i",
		PGRES_FATAL_ERROR},
		{"ROLLBACK", PGRES_COMMAND_OK},
		{"SELECT repeat('c', 80)", PGRES_TUPLES_OK}
	};
	PGconn	   *conn = PQconnectdb(conninfo);
	PGresult   *res;
	FILE	   *trace = NULL;
	char	   *data;
	int			len;
	int			status = 1;

	if (PQstatus(conn) != CONNECTION_OK)
		goto done;
	trace = fopen(filename, "w");
	if (trace == NULL)
		goto done;
	PQtrace(conn, trace);
	PQsetTraceFlags(conn, PQTRACE_SUPPRESS_TIMESTAMPS);
	res = PQexec(conn, "SELECT repeat('trace payload ', 1000)");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		goto done;
	}
	PQclear(res);
	PQsetTraceFlags(conn, PQTRACE_SUPPRESS_TIMESTAMPS | PQTRACE_REGRESS_MODE);
	for (int i = 0; i < lengthof(commands); i++)
	{
		res = PQexec(conn, commands[i].sql);
		if (PQresultStatus(res) != commands[i].expected)
		{
			PQclear(res);
			goto done;
		}
		PQclear(res);
#ifdef USE_ZSTD
		if (conn->compression_in_frame || conn->compression_frame_ended)
			goto done;
#endif
	}
	res = PQexec(conn,
				 "COPY (SELECT repeat('d', 80) FROM generate_series(1, 3)) TO STDOUT");
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		PQclear(res);
		goto done;
	}
	PQclear(res);
	while ((len = PQgetCopyData(conn, &data, 0)) > 0)
		PQfreemem(data);
	if (len != -1)
		goto done;
	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			PQclear(res);
			goto done;
		}
		PQclear(res);
	}
	status = 0;
done:
	if (status != 0)
		fprintf(stderr, "%s", PQerrorMessage(conn));
	PQuntrace(conn);
	if (trace)
		fclose(trace);
	PQfinish(conn);
	return status;
}

int
main(int argc, char *argv[])
{
	if ((argc > 1) && !strcmp(argv[1], "--ssl"))
	{
		print_ssl_library();
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "--compression-reset"))
		return test_compression_reset(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--compression-large-copy"))
		return test_compression_large_copy(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "--compression-trace"))
		return test_compression_trace(argv[2], argv[3]);
#ifdef USE_ZSTD
	if (argc == 3 && !strcmp(argv[1], "--compression-incompressible-copy"))
		return test_compression_incompressible_copy(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--compression-large-datarow"))
		return test_compression_large_datarow(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--compression-frame-boundaries"))
		return test_compression_frame_boundaries(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--compression-pipeline"))
		return test_compression_pipeline(argv[2]);
#endif

	printf("supported arguments are --ssl, --compression-reset CONNINFO, "
		   "--compression-large-copy CONNINFO, "
		   "--compression-incompressible-copy CONNINFO, "
		   "--compression-large-datarow CONNINFO, "
		   "--compression-frame-boundaries CONNINFO, "
		   "--compression-pipeline CONNINFO, and "
		   "--compression-trace CONNINFO FILE\n");
	return 1;
}
