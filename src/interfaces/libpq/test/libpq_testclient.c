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
#include <zstd.h>

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
		!conn->compression_buffers_initialized ||
		conn->compression_buffer.len != 0)
	{
		fprintf(stderr, "server response was not compressed\n");
		PQfinish(conn);
		return 1;
	}

	/*
	 * Model an unfinished compressed DataRow without relying on socket
	 * timing.
	 */
	appendPQExpBufferChar(&conn->compression_buffer, PqMsg_DataRow);
	if (PQExpBufferBroken(&conn->compression_buffer))
	{
		fprintf(stderr, "out of memory\n");
		PQfinish(conn);
		return 1;
	}
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
	PQsetTraceFlags(conn, PQTRACE_SUPPRESS_TIMESTAMPS | PQTRACE_REGRESS_MODE);
	PQtrace(conn, trace);

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

	res = PQexec(conn, "SELECT octet_length(data) FROM compression_copy");
	if (PQresultStatus(res) == PGRES_TUPLES_OK &&
		!strcmp(PQgetvalue(res, 0, 0), "17825792"))
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
#ifdef USE_ZSTD
	if (argc == 3 && !strcmp(argv[1], "--compression-large-datarow"))
		return test_compression_large_datarow(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--compression-frame-boundaries"))
		return test_compression_frame_boundaries(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--compression-pipeline"))
		return test_compression_pipeline(argv[2]);
#endif

	printf("supported arguments are --ssl, --compression-reset CONNINFO, "
		   "--compression-large-copy CONNINFO, "
		   "--compression-large-datarow CONNINFO, "
		   "--compression-frame-boundaries CONNINFO, and "
		   "--compression-pipeline CONNINFO\n");
	return 1;
}
