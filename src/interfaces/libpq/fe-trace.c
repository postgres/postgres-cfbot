/*-------------------------------------------------------------------------
 *
 *	fe-trace.c
 *	  functions for libpq protocol tracing
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-trace.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "port/pg_bswap.h"


/* Enable tracing */
void
PQtrace(PGconn *conn, FILE *debug_port)
{
	if (conn == NULL)
		return;
	PQuntrace(conn);
	if (debug_port == NULL)
		return;

	conn->Pfdebug = debug_port;
	conn->traceFlags = 0;
}

/* Disable tracing */
void
PQuntrace(PGconn *conn)
{
	if (conn == NULL)
		return;
	if (conn->Pfdebug)
	{
		fflush(conn->Pfdebug);
		conn->Pfdebug = NULL;
	}

	conn->traceFlags = 0;
}

/* Set flags for current tracing session */
void
PQsetTraceFlags(PGconn *conn, int flags)
{
	if (conn == NULL)
		return;
	/* If PQtrace() failed, do nothing. */
	if (conn->Pfdebug == NULL)
		return;
	conn->traceFlags = flags;
}

/*
 * Print the current time, with microseconds, into a caller-supplied
 * buffer.
 * Cribbed from get_formatted_log_time, but much simpler.
 */
static void
pqTraceFormatTimestamp(char *timestr, size_t ts_len)
{
	struct timeval tval;
	time_t		now;
	struct tm	tmbuf;

	gettimeofday(&tval, NULL);

	/*
	 * MSVC's implementation of timeval uses a long for tv_sec, however,
	 * localtime() expects a time_t pointer.  Here we'll assign tv_sec to a
	 * local time_t variable so that we pass localtime() the correct pointer
	 * type.
	 */
	now = tval.tv_sec;
	strftime(timestr, ts_len,
			 "%Y-%m-%d %H:%M:%S",
			 localtime_r(&now, &tmbuf));
	/* append microseconds */
	snprintf(timestr + strlen(timestr), ts_len - strlen(timestr),
			 ".%06u", (unsigned int) (tval.tv_usec));
}

/*
 *   pqTraceOutputByte1: output a 1-char message to the log
 */
static void
pqTraceOutputByte1(PQExpBuffer buf, const char *data, int *cursor)
{
	const char *v = data + *cursor;

	/*
	 * Show non-printable data in hex format, including the terminating \0
	 * that completes ErrorResponse and NoticeResponse messages.
	 */
	if (!isprint((unsigned char) *v))
		appendPQExpBuffer(buf, " \\x%02x", (unsigned char) *v);
	else
		appendPQExpBuffer(buf, " %c", *v);
	*cursor += 1;
}

/*
 *   pqTraceOutputInt16: output a 2-byte integer message to the log
 */
static int
pqTraceOutputInt16(PQExpBuffer buf, const char *data, int *cursor)
{
	uint16		tmp;
	int			result;

	memcpy(&tmp, data + *cursor, 2);
	*cursor += 2;
	result = (int) pg_ntoh16(tmp);
	appendPQExpBuffer(buf, " %d", result);

	return result;
}

/*
 *   pqTraceOutputInt32: output a 4-byte integer message to the log
 *
 * If 'suppress' is true, print a literal NNNN instead of the actual number.
 */
static int
pqTraceOutputInt32(PQExpBuffer buf, const char *data, int *cursor, bool suppress)
{
	int			result;

	memcpy(&result, data + *cursor, 4);
	*cursor += 4;
	result = (int) pg_ntoh32(result);
	if (suppress)
		appendPQExpBufferStr(buf, " NNNN");
	else
		appendPQExpBuffer(buf, " %d", result);

	return result;
}

/*
 *   pqTraceOutputString: output a string message to the log
 *
 * If 'suppress' is true, print a literal "SSSS" instead of the actual string.
 */
static void
pqTraceOutputString(PQExpBuffer buf, const char *data, int *cursor, bool suppress)
{
	if (suppress)
	{
		appendPQExpBufferStr(buf, " \"SSSS\"");
		*cursor += strlen(data + *cursor) + 1;
	}
	else
	{
		appendPQExpBuffer(buf, " \"%s\"", data + *cursor);

		/*
		 * This is a null-terminated string. So add 1.
		 */
		*cursor += strlen(data + *cursor) + 1;
	}
}

/*
 * pqTraceOutputNchar: output a string of exactly len bytes message to the log
 *
 * If 'suppress' is true, print a literal 'BBBB' instead of the actual bytes.
 */
static void
pqTraceOutputNchar(PQExpBuffer buf, int len, const char *data, int *cursor, bool suppress)
{
	int			i,
				next;			/* first char not yet printed */
	const char *v = data + *cursor;

	if (suppress)
	{
		appendPQExpBufferStr(buf, " 'BBBB'");
		*cursor += len;
		return;
	}

	appendPQExpBufferStr(buf, " \'");

	for (next = i = 0; i < len; ++i)
	{
		if (isprint((unsigned char) v[i]))
			continue;
		else
		{
			appendBinaryPQExpBuffer(buf, v + next, i - next);
			appendPQExpBuffer(buf, "\\x%02x", (unsigned char) v[i]);
			next = i + 1;
		}
	}
	if (next < len)
		appendBinaryPQExpBuffer(buf, v + next, len - next);

	appendPQExpBufferStr(buf, "\'");
	*cursor += len;
}

/*
 * pqTraceOutputNbyte: output bytes in hexadecimal of exactly len bytes to the log
 *
 * If 'suppress' is true, print a literal 'BBBB' instead of the actual bytes.
 */
static void
pqTraceOutputNbyte(PQExpBuffer buf, int len, const char *data, int *cursor, bool suppress)
{
	int			i;
	const char *v = data + *cursor;

	if (suppress)
	{
		appendPQExpBufferStr(buf, " 'BBBB'");
		*cursor += len;
		return;
	}

	appendPQExpBufferStr(buf, " \'");

	for (i = 0; i < len; ++i)
		appendPQExpBuffer(buf, "\\x%02x", (unsigned char) v[i]);

	appendPQExpBufferStr(buf, "\'");
	*cursor += len;
}

/*
 * Output functions by protocol message type
 */

static void
pqTraceOutput_NotificationResponse(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	appendPQExpBufferStr(buf, "NotificationResponse\t");
	pqTraceOutputInt32(buf, message, cursor, regress);
	pqTraceOutputString(buf, message, cursor, false);
	pqTraceOutputString(buf, message, cursor, false);
}

static void
pqTraceOutput_Bind(PQExpBuffer buf, const char *message, int *cursor)
{
	int			nparams;

	appendPQExpBufferStr(buf, "Bind\t");
	pqTraceOutputString(buf, message, cursor, false);
	pqTraceOutputString(buf, message, cursor, false);
	nparams = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt16(buf, message, cursor);

	nparams = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nparams; i++)
	{
		int			nbytes;

		nbytes = pqTraceOutputInt32(buf, message, cursor, false);
		if (nbytes == -1)
			continue;
		pqTraceOutputNchar(buf, nbytes, message, cursor, false);
	}

	nparams = pqTraceOutputInt16(buf, message, cursor);
	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt16(buf, message, cursor);
}

static void
pqTraceOutput_Close(PQExpBuffer buf, const char *message, int *cursor)
{
	appendPQExpBufferStr(buf, "Close\t");
	pqTraceOutputByte1(buf, message, cursor);
	pqTraceOutputString(buf, message, cursor, false);
}

static void
pqTraceOutput_CommandComplete(PQExpBuffer buf, const char *message, int *cursor)
{
	appendPQExpBufferStr(buf, "CommandComplete\t");
	pqTraceOutputString(buf, message, cursor, false);
}

static void
pqTraceOutput_CopyData(PQExpBuffer buf, const char *message, int *cursor, int length,
					   bool suppress)
{
	appendPQExpBufferStr(buf, "CopyData\t");
	pqTraceOutputNchar(buf, length - *cursor + 1, message, cursor, suppress);
}

static void
pqTraceOutput_DataRow(PQExpBuffer buf, const char *message, int *cursor)
{
	int			nfields;
	int			len;
	int			i;

	appendPQExpBufferStr(buf, "DataRow\t");
	nfields = pqTraceOutputInt16(buf, message, cursor);
	for (i = 0; i < nfields; i++)
	{
		len = pqTraceOutputInt32(buf, message, cursor, false);
		if (len == -1)
			continue;
		pqTraceOutputNchar(buf, len, message, cursor, false);
	}
}

static void
pqTraceOutput_Describe(PQExpBuffer buf, const char *message, int *cursor)
{
	appendPQExpBufferStr(buf, "Describe\t");
	pqTraceOutputByte1(buf, message, cursor);
	pqTraceOutputString(buf, message, cursor, false);
}

/* shared code NoticeResponse / ErrorResponse */
static void
pqTraceOutputNR(PQExpBuffer buf, const char *type, const char *message, int *cursor,
				bool regress)
{
	appendPQExpBuffer(buf, "%s\t", type);
	for (;;)
	{
		char		field;
		bool		suppress;

		pqTraceOutputByte1(buf, message, cursor);
		field = message[*cursor - 1];
		if (field == '\0')
			break;

		suppress = regress && (field == 'L' || field == 'F' || field == 'R');
		pqTraceOutputString(buf, message, cursor, suppress);
	}
}

static void
pqTraceOutput_ErrorResponse(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	pqTraceOutputNR(buf, "ErrorResponse", message, cursor, regress);
}

static void
pqTraceOutput_NoticeResponse(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	pqTraceOutputNR(buf, "NoticeResponse", message, cursor, regress);
}

static void
pqTraceOutput_Execute(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	appendPQExpBufferStr(buf, "Execute\t");
	pqTraceOutputString(buf, message, cursor, false);
	pqTraceOutputInt32(buf, message, cursor, false);
}

static void
pqTraceOutput_CopyFail(PQExpBuffer buf, const char *message, int *cursor)
{
	appendPQExpBufferStr(buf, "CopyFail\t");
	pqTraceOutputString(buf, message, cursor, false);
}

static void
pqTraceOutput_GSSResponse(PQExpBuffer buf, const char *message, int *cursor,
						  int length, bool regress)
{
	appendPQExpBufferStr(buf, "GSSResponse\t");
	pqTraceOutputNchar(buf, length - *cursor + 1, message, cursor, regress);
}

static void
pqTraceOutput_PasswordMessage(PQExpBuffer buf, const char *message, int *cursor)
{
	appendPQExpBufferStr(buf, "PasswordMessage\t");
	pqTraceOutputString(buf, message, cursor, false);
}

static void
pqTraceOutput_SASLInitialResponse(PQExpBuffer buf, const char *message, int *cursor,
								  bool regress)
{
	int			initialResponse;

	appendPQExpBufferStr(buf, "SASLInitialResponse\t");
	pqTraceOutputString(buf, message, cursor, false);
	initialResponse = pqTraceOutputInt32(buf, message, cursor, false);
	if (initialResponse != -1)
		pqTraceOutputNchar(buf, initialResponse, message, cursor, regress);
}

static void
pqTraceOutput_SASLResponse(PQExpBuffer buf, const char *message, int *cursor,
						   int length, bool regress)
{
	appendPQExpBufferStr(buf, "SASLResponse\t");
	pqTraceOutputNchar(buf, length - *cursor + 1, message, cursor, regress);
}

static void
pqTraceOutput_FunctionCall(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	int			nfields;
	int			nbytes;

	appendPQExpBufferStr(buf, "FunctionCall\t");
	pqTraceOutputInt32(buf, message, cursor, regress);
	nfields = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(buf, message, cursor);

	nfields = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nfields; i++)
	{
		nbytes = pqTraceOutputInt32(buf, message, cursor, false);
		if (nbytes == -1)
			continue;
		pqTraceOutputNchar(buf, nbytes, message, cursor, false);
	}

	pqTraceOutputInt16(buf, message, cursor);
}

static void
pqTraceOutput_CopyInResponse(PQExpBuffer buf, const char *message, int *cursor)
{
	int			nfields;

	appendPQExpBufferStr(buf, "CopyInResponse\t");
	pqTraceOutputByte1(buf, message, cursor);
	nfields = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(buf, message, cursor);
}

static void
pqTraceOutput_CopyOutResponse(PQExpBuffer buf, const char *message, int *cursor)
{
	int			nfields;

	appendPQExpBufferStr(buf, "CopyOutResponse\t");
	pqTraceOutputByte1(buf, message, cursor);
	nfields = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt16(buf, message, cursor);
}

static void
pqTraceOutput_BackendKeyData(PQExpBuffer buf, const char *message, int *cursor, int length,
							 bool regress)
{
	appendPQExpBufferStr(buf, "BackendKeyData\t");
	pqTraceOutputInt32(buf, message, cursor, regress);
	pqTraceOutputNbyte(buf, length - *cursor + 1, message, cursor, regress);
}

static void
pqTraceOutput_Parse(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	int			nparams;

	appendPQExpBufferStr(buf, "Parse\t");
	pqTraceOutputString(buf, message, cursor, false);
	pqTraceOutputString(buf, message, cursor, false);
	nparams = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nparams; i++)
		pqTraceOutputInt32(buf, message, cursor, regress);
}

static void
pqTraceOutput_Query(PQExpBuffer buf, const char *message, int *cursor)
{
	appendPQExpBufferStr(buf, "Query\t");
	pqTraceOutputString(buf, message, cursor, false);
}

static void
pqTraceOutput_Authentication(PQExpBuffer buf, const char *message, int *cursor,
							 int length, bool suppress)
{
	int			authType = 0;

	memcpy(&authType, message + *cursor, 4);
	authType = (int) pg_ntoh32(authType);
	*cursor += 4;
	switch (authType)
	{
		case AUTH_REQ_OK:
			appendPQExpBufferStr(buf, "AuthenticationOk");
			break;
			/* AUTH_REQ_KRB4 not supported */
			/* AUTH_REQ_KRB5 not supported */
		case AUTH_REQ_PASSWORD:
			appendPQExpBufferStr(buf, "AuthenticationCleartextPassword");
			break;
			/* AUTH_REQ_CRYPT not supported */
		case AUTH_REQ_MD5:
			appendPQExpBufferStr(buf, "AuthenticationMD5Password");
			break;
		case AUTH_REQ_GSS:
			appendPQExpBufferStr(buf, "AuthenticationGSS");
			break;
		case AUTH_REQ_GSS_CONT:
			appendPQExpBufferStr(buf, "AuthenticationGSSContinue\t");
			pqTraceOutputNchar(buf, length - *cursor + 1, message, cursor,
							   suppress);
			break;
		case AUTH_REQ_SSPI:
			appendPQExpBufferStr(buf, "AuthenticationSSPI");
			break;
		case AUTH_REQ_SASL:
			appendPQExpBufferStr(buf, "AuthenticationSASL\t");
			while (message[*cursor] != '\0')
				pqTraceOutputString(buf, message, cursor, false);
			pqTraceOutputString(buf, message, cursor, false);
			break;
		case AUTH_REQ_SASL_CONT:
			appendPQExpBufferStr(buf, "AuthenticationSASLContinue\t");
			pqTraceOutputNchar(buf, length - *cursor + 1, message, cursor,
							   suppress);
			break;
		case AUTH_REQ_SASL_FIN:
			appendPQExpBufferStr(buf, "AuthenticationSASLFinal\t");
			pqTraceOutputNchar(buf, length - *cursor + 1, message, cursor,
							   suppress);
			break;
		default:
			appendPQExpBuffer(buf, "Unknown authentication message %d", authType);
	}
}

static void
pqTraceOutput_ParameterStatus(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	appendPQExpBufferStr(buf, "ParameterStatus\t");
	pqTraceOutputString(buf, message, cursor, false);
	pqTraceOutputString(buf, message, cursor, regress);
}

static void
pqTraceOutput_ParameterDescription(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	int			nfields;

	appendPQExpBufferStr(buf, "ParameterDescription\t");
	nfields = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nfields; i++)
		pqTraceOutputInt32(buf, message, cursor, regress);
}

static void
pqTraceOutput_RowDescription(PQExpBuffer buf, const char *message, int *cursor, bool regress)
{
	int			nfields;

	appendPQExpBufferStr(buf, "RowDescription\t");
	nfields = pqTraceOutputInt16(buf, message, cursor);

	for (int i = 0; i < nfields; i++)
	{
		pqTraceOutputString(buf, message, cursor, false);
		pqTraceOutputInt32(buf, message, cursor, regress);
		pqTraceOutputInt16(buf, message, cursor);
		pqTraceOutputInt32(buf, message, cursor, regress);
		pqTraceOutputInt16(buf, message, cursor);
		pqTraceOutputInt32(buf, message, cursor, false);
		pqTraceOutputInt16(buf, message, cursor);
	}
}

static void
pqTraceOutput_NegotiateProtocolVersion(PQExpBuffer buf, const char *message, int *cursor)
{
	int			nparams;

	appendPQExpBufferStr(buf, "NegotiateProtocolVersion\t");
	pqTraceOutputInt32(buf, message, cursor, false);
	nparams = pqTraceOutputInt32(buf, message, cursor, false);
	for (int i = 0; i < nparams; i++)
	{
		pqTraceOutputString(buf, message, cursor, false);
	}
}

static void
pqTraceOutput_FunctionCallResponse(PQExpBuffer buf, const char *message, int *cursor)
{
	int			len;

	appendPQExpBufferStr(buf, "FunctionCallResponse\t");
	len = pqTraceOutputInt32(buf, message, cursor, false);
	if (len != -1)
		pqTraceOutputNchar(buf, len, message, cursor, false);
}

static void
pqTraceOutput_CopyBothResponse(PQExpBuffer buf, const char *message, int *cursor, int length)
{
	appendPQExpBufferStr(buf, "CopyBothResponse\t");
	pqTraceOutputByte1(buf, message, cursor);

	while (length > *cursor)
		pqTraceOutputInt16(buf, message, cursor);
}

static void
pqTraceOutput_ReadyForQuery(PQExpBuffer buf, const char *message, int *cursor)
{
	appendPQExpBufferStr(buf, "ReadyForQuery\t");
	pqTraceOutputByte1(buf, message, cursor);
}

/*
 * Print the given message to the trace output stream.
 */
void
pqTraceOutputMessage(PGconn *conn, const char *message, bool toServer)
{
	char		id;
	int			length;
	char	   *prefix = toServer ? "F" : "B";
	int			logCursor = 0;
	bool		regress;
	PQExpBufferData buf;

	initPQExpBuffer(&buf);

	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		appendPQExpBuffer(&buf, "%s\t", timestr);
	}
	regress = (conn->traceFlags & PQTRACE_REGRESS_MODE) != 0;

	id = message[logCursor++];

	memcpy(&length, message + logCursor, 4);
	length = (int) pg_ntoh32(length);
	logCursor += 4;

	/*
	 * In regress mode, suppress the length of ErrorResponse, NoticeResponse
	 * and ParameterStatus. The F (file name), L (line number) and R (routine
	 * name) fields can change as server code is modified, and if their
	 * lengths differ from the originals, that would break tests. For
	 * ParameterStatus, the size changes depending on the parameters' value,
	 * whose values depend on the test environment.
	 */
	if (regress && !toServer && (id == PqMsg_ErrorResponse
								 || id == PqMsg_NoticeResponse
								 || id == PqMsg_ParameterStatus))
		appendPQExpBuffer(&buf, "%s\tNN\t", prefix);
	else
		appendPQExpBuffer(&buf, "%s\t%d\t", prefix, length);

	switch (id)
	{
		case PqMsg_ParseComplete:
			appendPQExpBufferStr(&buf, "ParseComplete");
			/* No message content */
			break;
		case PqMsg_BindComplete:
			appendPQExpBufferStr(&buf, "BindComplete");
			/* No message content */
			break;
		case PqMsg_CloseComplete:
			appendPQExpBufferStr(&buf, "CloseComplete");
			/* No message content */
			break;
		case PqMsg_NotificationResponse:
			pqTraceOutput_NotificationResponse(&buf, message, &logCursor, regress);
			break;
		case PqMsg_Bind:
			pqTraceOutput_Bind(&buf, message, &logCursor);
			break;
		case PqMsg_CopyDone:
			appendPQExpBufferStr(&buf, "CopyDone");
			/* No message content */
			break;
		case PqMsg_CommandComplete:
			/* Close(F) and CommandComplete(B) use the same identifier. */
			Assert(PqMsg_Close == PqMsg_CommandComplete);
			if (toServer)
				pqTraceOutput_Close(&buf, message, &logCursor);
			else
				pqTraceOutput_CommandComplete(&buf, message, &logCursor);
			break;
		case PqMsg_CopyData:
			pqTraceOutput_CopyData(&buf, message, &logCursor,
								   length, regress);
			break;
		case PqMsg_Describe:
			/* Describe(F) and DataRow(B) use the same identifier. */
			Assert(PqMsg_Describe == PqMsg_DataRow);
			if (toServer)
				pqTraceOutput_Describe(&buf, message, &logCursor);
			else
				pqTraceOutput_DataRow(&buf, message, &logCursor);
			break;
		case PqMsg_Execute:
			/* Execute(F) and ErrorResponse(B) use the same identifier. */
			Assert(PqMsg_Execute == PqMsg_ErrorResponse);
			if (toServer)
				pqTraceOutput_Execute(&buf, message, &logCursor, regress);
			else
				pqTraceOutput_ErrorResponse(&buf, message, &logCursor, regress);
			break;
		case PqMsg_CopyFail:
			pqTraceOutput_CopyFail(&buf, message, &logCursor);
			break;
		case PqMsg_GSSResponse:
			Assert(PqMsg_GSSResponse == PqMsg_PasswordMessage);
			Assert(PqMsg_GSSResponse == PqMsg_SASLInitialResponse);
			Assert(PqMsg_GSSResponse == PqMsg_SASLResponse);

			/*
			 * These messages share a common type byte, so we discriminate by
			 * having the code store the auth type separately.
			 */
			switch (conn->current_auth_response)
			{
				case AUTH_RESPONSE_GSS:
					pqTraceOutput_GSSResponse(&buf, message,
											  &logCursor, length, regress);
					break;
				case AUTH_RESPONSE_PASSWORD:
					pqTraceOutput_PasswordMessage(&buf, message,
												  &logCursor);
					break;
				case AUTH_RESPONSE_SASL_INITIAL:
					pqTraceOutput_SASLInitialResponse(&buf, message,
													  &logCursor, regress);
					break;
				case AUTH_RESPONSE_SASL:
					pqTraceOutput_SASLResponse(&buf, message,
											   &logCursor, length, regress);
					break;
				default:
					appendPQExpBufferStr(&buf, "UnknownAuthenticationResponse");
					break;
			}
			conn->current_auth_response = '\0';
			break;
		case PqMsg_FunctionCall:
			pqTraceOutput_FunctionCall(&buf, message, &logCursor, regress);
			break;
		case PqMsg_CopyInResponse:
			pqTraceOutput_CopyInResponse(&buf, message, &logCursor);
			break;
		case PqMsg_Flush:
			/* Flush(F) and CopyOutResponse(B) use the same identifier */
			Assert(PqMsg_CopyOutResponse == PqMsg_Flush);
			if (toServer)
				appendPQExpBufferStr(&buf, "Flush");	/* no message content */
			else
				pqTraceOutput_CopyOutResponse(&buf, message, &logCursor);
			break;
		case PqMsg_EmptyQueryResponse:
			appendPQExpBufferStr(&buf, "EmptyQueryResponse");
			/* No message content */
			break;
		case PqMsg_BackendKeyData:
			pqTraceOutput_BackendKeyData(&buf, message, &logCursor,
										 length, regress);
			break;
		case PqMsg_NoData:
			appendPQExpBufferStr(&buf, "NoData");
			/* No message content */
			break;
		case PqMsg_NoticeResponse:
			pqTraceOutput_NoticeResponse(&buf, message, &logCursor, regress);
			break;
		case PqMsg_Parse:
			pqTraceOutput_Parse(&buf, message, &logCursor, regress);
			break;
		case PqMsg_Query:
			pqTraceOutput_Query(&buf, message, &logCursor);
			break;
		case PqMsg_AuthenticationRequest:
			pqTraceOutput_Authentication(&buf, message, &logCursor,
										 length, regress);
			break;
		case PqMsg_PortalSuspended:
			appendPQExpBufferStr(&buf, "PortalSuspended");
			/* No message content */
			break;
		case PqMsg_Sync:
			/* ParameterStatus(B) and Sync(F) use the same identifier */
			Assert(PqMsg_ParameterStatus == PqMsg_Sync);
			if (toServer)
				appendPQExpBufferStr(&buf, "Sync"); /* no message content */
			else
				pqTraceOutput_ParameterStatus(&buf, message, &logCursor, regress);
			break;
		case PqMsg_ParameterDescription:
			pqTraceOutput_ParameterDescription(&buf, message, &logCursor, regress);
			break;
		case PqMsg_RowDescription:
			pqTraceOutput_RowDescription(&buf, message, &logCursor, regress);
			break;
		case PqMsg_NegotiateProtocolVersion:
			pqTraceOutput_NegotiateProtocolVersion(&buf, message, &logCursor);
			break;
		case PqMsg_FunctionCallResponse:
			pqTraceOutput_FunctionCallResponse(&buf, message, &logCursor);
			break;
		case PqMsg_CopyBothResponse:
			pqTraceOutput_CopyBothResponse(&buf, message, &logCursor, length);
			break;
		case PqMsg_Terminate:
			appendPQExpBufferStr(&buf, "Terminate");
			/* No message content */
			break;
		case PqMsg_ReadyForQuery:
			pqTraceOutput_ReadyForQuery(&buf, message, &logCursor);
			break;
		default:
			appendPQExpBuffer(&buf, "Unknown message: %02x", id);
			break;
	}

	appendPQExpBufferChar(&buf, '\n');

	/*
	 * Verify the printing routine did it right.  Note that the one-byte
	 * message identifier is not included in the length, but our cursor does
	 * include it.
	 */
	if (logCursor - 1 != length)
		appendPQExpBuffer(&buf,
						  "mismatched message length: consumed %d, expected %d\n",
						  logCursor - 1, length);

	fwrite(buf.data, 1, buf.len, conn->Pfdebug);
	termPQExpBuffer(&buf);
}

/*
 * Print special messages (those containing no type byte) to the trace output
 * stream.
 */
void
pqTraceOutputNoTypeByteMessage(PGconn *conn, const char *message)
{
	int			length;
	int			version;
	bool		regress;
	int			logCursor = 0;
	PQExpBufferData buf;

	initPQExpBuffer(&buf);
	regress = (conn->traceFlags & PQTRACE_REGRESS_MODE) != 0;

	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		appendPQExpBuffer(&buf, "%s\t", timestr);
	}

	memcpy(&length, message + logCursor, 4);
	length = (int) pg_ntoh32(length);
	logCursor += 4;

	if (length < 8)
	{
		appendPQExpBuffer(&buf, "F\t%d\tUnknown message\n", length);
		fprintf(conn->Pfdebug, "%s", buf.data);
		termPQExpBuffer(&buf);
		return;
	}

	memcpy(&version, message + logCursor, 4);
	version = (int) pg_ntoh32(version);

	/*
	 * In regress, suppress the length of StartupMessage. The parameter values
	 * depend on the test environment, so the test may break depending on
	 * where it's executed.
	 */
	if (regress && (version != CANCEL_REQUEST_CODE
					&& version != NEGOTIATE_SSL_CODE
					&& version != NEGOTIATE_GSS_CODE))
		appendPQExpBufferStr(&buf, "F\tNN\t");
	else
		appendPQExpBuffer(&buf, "F\t%d\t", length);

	if (version == CANCEL_REQUEST_CODE && length >= 16)
	{
		appendPQExpBufferStr(&buf, "CancelRequest\t");
		pqTraceOutputInt16(&buf, message, &logCursor);
		pqTraceOutputInt16(&buf, message, &logCursor);
		pqTraceOutputInt32(&buf, message, &logCursor, regress);
		pqTraceOutputNbyte(&buf, length - logCursor, message,
						   &logCursor, regress);
	}
	else if (version == NEGOTIATE_SSL_CODE)
	{
		appendPQExpBufferStr(&buf, "SSLRequest\t");
		pqTraceOutputInt16(&buf, message, &logCursor);
		pqTraceOutputInt16(&buf, message, &logCursor);
	}
	else if (version == NEGOTIATE_GSS_CODE)
	{
		appendPQExpBufferStr(&buf, "GSSENCRequest\t");
		pqTraceOutputInt16(&buf, message, &logCursor);
		pqTraceOutputInt16(&buf, message, &logCursor);
	}
	else
	{
		appendPQExpBufferStr(&buf, "StartupMessage\t");
		pqTraceOutputInt16(&buf, message, &logCursor);
		pqTraceOutputInt16(&buf, message, &logCursor);
		while (message[logCursor] != '\0')
		{
			pqTraceOutputString(&buf, message, &logCursor, false);
			pqTraceOutputString(&buf, message, &logCursor, regress);
		}

		/*
		 * Startup messages end with a trailing terminator, advance our cursor
		 * to include it
		 */
		logCursor++;
	}

	appendPQExpBufferChar(&buf, '\n');

	/*
	 * Verify the printing routine did it right. There's no one-byte message
	 * identifier here, so logCursor should match the length
	 */
	if (logCursor != length)
		appendPQExpBuffer(&buf,
						  "mismatched message length: consumed %d, expected %d\n",
						  logCursor, length);

	fwrite(buf.data, 1, buf.len, conn->Pfdebug);
	termPQExpBuffer(&buf);
}

/*
 * Trace a single-byte backend response received for a known request
 * type the frontend previously sent.  Only useful for the simplest of
 * FE/BE interaction workflows such as SSL/GSS encryption requests.
 */
void
pqTraceOutputCharResponse(PGconn *conn, const char *responseType,
						  char response)
{
	PQExpBufferData buf;

	initPQExpBuffer(&buf);

	if ((conn->traceFlags & PQTRACE_SUPPRESS_TIMESTAMPS) == 0)
	{
		char		timestr[128];

		pqTraceFormatTimestamp(timestr, sizeof(timestr));
		appendPQExpBuffer(&buf, "%s\t", timestr);
	}

	appendPQExpBuffer(&buf, "B\t1\t%s\t %c\n", responseType, response);

	fprintf(conn->Pfdebug, "%s", buf.data);
	termPQExpBuffer(&buf);
}
