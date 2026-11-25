/*-------------------------------------------------------------------------
 *
 * pg_trace_etw.c
 *	  ETW (Event Tracing for Windows) provider definition and lifecycle.
 *
 * Defines the single "PostgreSQL.Server" TraceLogging provider that the
 * generated TRACE_POSTGRESQL_* macros write to, and the per-process register
 * and unregister entry points from pg_trace_etw.h.  Compiled only with the
 * "etw" option (Windows SDK builds; MinGW lacks TraceLoggingProvider.h).
 *
 * Registration is best-effort.  If it fails, the provider handle stays
 * disabled and every probe macro is a no-op, so a failure never affects
 * server startup.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/pg_trace_etw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "storage/ipc.h"
#include "utils/pg_trace_etw.h"

/*
 * Define the "PostgreSQL.Server" provider.  The tuple is the provider GUID
 * ea7811de-e757-5488-7ad6-2b057cced37d, which is the name hash recommended
 * in TraceLoggingProvider.h (the same value .NET's EventSource.GetGuid
 * computes for this name).  See pg_trace_etw.h.
 */
TRACELOGGING_DEFINE_PROVIDER(
							 pg_etw_provider,
							 "PostgreSQL.Server",
							 (0xea7811de, 0xe757, 0x5488, 0x7a, 0xd6, 0x2b, 0x05, 0x7c, 0xce, 0xd3, 0x7d));

static bool etw_registered = false;

static void pg_trace_etw_atexit(int code, Datum arg);

/*
 * Register the provider for this process, once.  Safe to call more than once.
 */
void
pg_trace_etw_register(void)
{
	if (etw_registered)
		return;

	if (TraceLoggingRegister(pg_etw_provider) == ERROR_SUCCESS)
	{
		etw_registered = true;
		on_proc_exit(pg_trace_etw_atexit, 0);
	}
}

/*
 * Unregister the provider for this process, if registered.
 */
void
pg_trace_etw_unregister(void)
{
	if (!etw_registered)
		return;

	TraceLoggingUnregister(pg_etw_provider);
	etw_registered = false;
}

/*
 * Return the number of bytes of a string probe argument to write, which is
 * its length clipped to PG_ETW_MAX_STRING without splitting a multibyte
 * character.  A NULL argument is written as an empty string.
 *
 * The generated macros only call this when a session has enabled the event.
 */
uint16
pg_trace_etw_strlen(const char *str)
{
	int			len;

	if (str == NULL)
		return 0;

	len = strlen(str);
	if (len > PG_ETW_MAX_STRING)
		len = pg_mbcliplen(str, len, PG_ETW_MAX_STRING);

	return (uint16) len;
}

/*
 * on_proc_exit callback wrapper.
 */
static void
pg_trace_etw_atexit(int code, Datum arg)
{
	pg_trace_etw_unregister();
}
