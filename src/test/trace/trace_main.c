/*-------------------------------------------------------------------------
 *
 * trace_main --- protocol trace regression test for libpq
 *
 * Runs the same sql test scripts as pg_regress_main, but compares the
 * libpq protocol trace produced via PQTRACE against an expected trace
 * file instead of comparing psql's textual output.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/trace/trace_main.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "pg_regress.h"

/*
 * start a psql test process for specified file (including redirection),
 * with PQTRACE enabled, and return process ID
 */
static PID_TYPE
trace_start_test(const char *testname,
				 _stringlist **resultfiles,
				 _stringlist **expectfiles,
				 _stringlist **tags)
{
	PID_TYPE	pid;
	char		infile[MAXPGPATH];
	char		outfile[MAXPGPATH];
	char		expectfile[MAXPGPATH];
	char		traceresultfile[MAXPGPATH];
	char		traceexpectfile[MAXPGPATH];
	StringInfoData psql_cmd;
	char	   *appnameenv;

	/*
	 * Look for files in the output dir first, consistent with a vpath search.
	 */
	snprintf(infile, sizeof(infile), "%s/sql/%s.sql",
			 outputdir, testname);
	if (!file_exists(infile))
		snprintf(infile, sizeof(infile), "%s/sql/%s.sql",
				 inputdir, testname);

	snprintf(outfile, sizeof(outfile), "%s/results/%s.out",
			 outputdir, testname);

	snprintf(traceresultfile, sizeof(traceresultfile), "%s/results/%s.trace",
			 outputdir, testname);

	snprintf(traceexpectfile, sizeof(traceexpectfile), "%s/expected/%s.trace",
			 expecteddir, testname);
	if (!file_exists(traceexpectfile))
		snprintf(traceexpectfile, sizeof(traceexpectfile), "%s/expected/%s.trace",
				 inputdir, testname);

	snprintf(expectfile, sizeof(expectfile), "%s/expected/%s.out",
			 expecteddir, testname);
	if (!file_exists(expectfile))
		snprintf(expectfile, sizeof(expectfile), "%s/expected/%s.out",
				 inputdir, testname);

	add_stringlist_item(resultfiles, outfile);
	add_stringlist_item(expectfiles, expectfile);

	add_stringlist_item(resultfiles, traceresultfile);
	add_stringlist_item(expectfiles, traceexpectfile);

	initStringInfo(&psql_cmd);

	if (launcher)
		appendStringInfo(&psql_cmd, "%s ", launcher);

	appendStringInfo(&psql_cmd,
					 "\"%s%spsql\" -X -q -d \"%s\" %s < \"%s\" > \"%s\" 2>&1",
					 bindir ? bindir : "",
					 bindir ? "/" : "",
					 dblist->str,
					 "-v HIDE_NOTIFICATIONS=on",
					 infile,
					 outfile);

	appnameenv = psprintf("pg_trace_regress/%s", testname);
	setenv("PGAPPNAME", appnameenv, 1);
	pfree(appnameenv);

	/* Suppress timestamps and mask varying data, for reproducible traces */
	setenv("PQTRACE", traceresultfile, 1);
	setenv("PQTRACEFLAGS", psprintf("%d", PQTRACE_SUPPRESS_TIMESTAMPS | PQTRACE_REGRESS_MODE), 1);

	pid = spawn_process(psql_cmd.data);

	if (pid == INVALID_PID)
	{
		fprintf(stderr, _("could not start process for test %s\n"),
				testname);
		exit(2);
	}

	unsetenv("PGAPPNAME");
	unsetenv("PQTRACE");
	unsetenv("PQTRACEFLAGS");

	pfree(psql_cmd.data);

	return pid;
}

static void
trace_init(int argc, char **argv)
{
	/* set default regression database name */
	add_stringlist_item(&dblist, "trace_regression");
}

int
main(int argc, char *argv[])
{
	return regression_main(argc, argv,
						   trace_init,
						   trace_start_test,
						   NULL /* no postfunc needed */ );
}
