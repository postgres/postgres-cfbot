/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * src/bin/psql/prompt.c
 */
#include "postgres_fe.h"

#ifdef WIN32
#include <io.h>
#include <win32.h>
#endif

#include "common.h"
#include "common/logging.h"
#include "common/string.h"
#include "input.h"
#include "libpq/pqcomm.h"
#include "prompt.h"
#include "settings.h"
#include "variables.h"

#define MAX_PROMPT_SIZE 8192
#define MAX_PROMPT_SUBST_SIZE 8192

/* stdout captured from the most recent PROMPT_COMMAND execution */
static char prompt_command_result[MAX_PROMPT_SUBST_SIZE + 1];

/*
 * Export psql state for PROMPT_COMMAND subprocesses (powerline, etc.).
 *
 * Enabled only when the PROMPT_SESSION_EXPORT psql variable is on (see
 * src/bin/psql/powerline-integration.md).  A companion implementation
 * lives in the powerline project (psql extension).
 *
 * Connection settings use the standard libpq PG* names, set from the
 * current session (PQhost, PQdb, etc.) rather than stale startup env.
 * There is no PG* variable for last-command exit status; use
 * PSQL_SHELL_EXIT (from :SHELL_EXIT) for that.  The psql variable ``txid`` is
 * exported as PSQL_TXID when set (e.g. via ``\\gset`` at connect).
 * PSQL_SUPERUSER is set to ``1`` or ``0`` from the connection's superuser
 * status (same test used for the ``%#`` prompt escape).
 */
static bool
prompt_session_export_enabled(void)
{
	const char *val = GetVariable(pset.vars, "PROMPT_SESSION_EXPORT");
	bool		on = false;

	if (val != NULL && ParseVariableBool(val, "PROMPT_SESSION_EXPORT", &on))
		return on;

	return false;
}

static void
export_prompt_environment(void)
{
	const char *val;

	val = GetVariable(pset.vars, "SHELL_EXIT");
	if (val)
		setenv("PSQL_SHELL_EXIT", val, 1);

	if (pset.db)
	{
		setenv("PGDATABASE", PQdb(pset.db), 1);
		setenv("PGUSER", session_username(), 1);

		val = PQhost(pset.db);
		if (val && val[0] != '\0')
			setenv("PGHOST", val, 1);
		else
			unsetenv("PGHOST");

		val = PQport(pset.db);
		if (val && val[0] != '\0')
			setenv("PGPORT", val, 1);
		else
			unsetenv("PGPORT");

		switch (PQtransactionStatus(pset.db))
		{
			case PQTRANS_IDLE:
				setenv("PSQL_TXN", "idle", 1);
				break;
			case PQTRANS_ACTIVE:
			case PQTRANS_INTRANS:
				setenv("PSQL_TXN", "active", 1);
				break;
			case PQTRANS_INERROR:
				setenv("PSQL_TXN", "error", 1);
				break;
			default:
				setenv("PSQL_TXN", "unknown", 1);
				break;
		}

		setenv("PSQL_SUPERUSER", is_superuser() ? "1" : "0", 1);
	}
	else
	{
		unsetenv("PGDATABASE");
		unsetenv("PGUSER");
		unsetenv("PGHOST");
		unsetenv("PGPORT");
		unsetenv("PSQL_TXN");
		unsetenv("PSQL_SUPERUSER");
	}

	val = GetVariable(pset.vars, "ROW_COUNT");
	if (val)
		setenv("PSQL_ROW_COUNT", val, 1);
	else
		unsetenv("PSQL_ROW_COUNT");

	val = GetVariable(pset.vars, "txid");
	if (val && val[0] != '\0')
		setenv("PSQL_TXID", val, 1);
	else
		unsetenv("PSQL_TXID");
}

/*
 * Run PROMPT_COMMAND, if set, before generating a prompt.
 *
 * Like bash's PROMPT_COMMAND, this executes a shell command before each
 * prompt is displayed.  The first line of stdout is captured and can be
 * inserted into PROMPT1/PROMPT2 via the %D escape.  (Unlike bash, psql
 * cannot run the command in-process, so "export" in PROMPT_COMMAND will
 * not affect later %`command` substitutions.)
 */
void
run_prompt_command(void)
{
	const char *cmd = GetVariable(pset.vars, "PROMPT_COMMAND");
	FILE	   *fd;
	size_t		len = 0;
	int			c;

	prompt_command_result[0] = '\0';

	if (cmd == NULL || cmd[0] == '\0')
		return;

	if (prompt_session_export_enabled())
		export_prompt_environment();

	fflush(NULL);
	fd = popen(cmd, "r");
	if (fd == NULL)
		return;

	while (len < MAX_PROMPT_SUBST_SIZE && (c = fgetc(fd)) != EOF)
	{
		if (c == '\n' || c == '\r')
			break;
		prompt_command_result[len++] = (char) c;
	}
	prompt_command_result[len] = '\0';

	/*
	 * Do not update SHELL_EXIT or SHELL_EXIT_CODE here: PROMPT_COMMAND is not
	 * a user command, and overwriting would hide the status of the last SQL
	 * or shell command (e.g. for powerline --last-exit-code).
	 */
	(void) pclose(fd);
}

/*
 * Reset prompt-related psql variables after a successful \connect.
 *
 * Clears stale row-count and exit-status from the previous session, and
 * refreshes :varname:`txid` when that variable is already defined (e.g. via
 * ``\\gset`` in .psqlrc for powerline).
 */
void
reset_prompt_status_after_connect(void)
{
	PGresult   *res;

	SetVariable(pset.vars, "ROW_COUNT", "0");
	SetVariable(pset.vars, "SHELL_EXIT", "0");

	if (!pset.db || GetVariable(pset.vars, "txid") == NULL)
		return;

	res = PQexec(pset.db, "SELECT txid_current()::text");
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
		SetVariable(pset.vars, "txid", PQgetvalue(res, 0, 0));
	PQclear(res);
}

/*--------------------------
 * get_prompt
 *
 * Returns a statically allocated prompt made by interpolating certain
 * tcsh style escape sequences into pset.vars "PROMPT1|2|3".
 * (might not be completely multibyte safe)
 *
 * Defined interpolations are:
 * %M - database server "hostname.domainname", "[local]" for AF_UNIX
 *		sockets, "[local:/dir/name]" if not default
 * %m - like %M, but hostname only (before first dot), or always "[local]"
 * %p - backend pid
 * %P - pipeline status: on, off or abort
 * %> - database server port number
 * %n - database user name
 * %S - search_path
 * %s - service
 * %/ - current database
 * %~ - like %/ but "~" when database name equals user name
 * %w - whitespace of the same width as the most recent output of PROMPT1
 * %# - "#" if superuser, ">" otherwise
 * %R - in prompt1 normally =, or ^ if single line mode,
 *			or a ! if session is not connected to a database;
 *		in prompt2 -, *, ', or ";
 *		in prompt3 nothing
 * %i - "standby" or "primary" depending on the server's in_hot_standby
 *      status, or "?" if unavailable (empty if unknown)
 * %x - transaction status: empty, *, !, ? (unknown or no connection)
 * %l - The line number inside the current statement, starting from 1.
 * %? - the error code of the last query (not yet implemented)
 * %% - a percent sign
 *
 * %[0-9]		   - the character with the given decimal code
 * %0[0-7]		   - the character with the given octal code
 * %0x[0-9A-Fa-f]  - the character with the given hexadecimal code
 *
 * %`command`	   - The result of executing command in /bin/sh with trailing
 *					 newline stripped.
 * %:name:		   - The value of the psql variable 'name'
 * (those will not be rescanned for more escape sequences!)
 * %D			   - stdout from PROMPT_COMMAND (first line, no trailing newline)
 *
 * %[ ... %]	   - tell readline that the contained text is invisible
 *					 (use around ANSI color sequences from prompt utilities)
 *
 * PROMPT_COMMAND   - if set, a shell command run before each prompt (see
 *					 run_prompt_command()).
 *
 * PROMPT_SESSION_EXPORT - if on, export connection/session environment for
 *					 PROMPT_COMMAND (see powerline-integration.md).
 *
 * If the application-wide prompts become NULL somehow, the returned string
 * will be empty (not NULL!).
 *--------------------------
 */

char *
get_prompt(promptStatus_t status, ConditionalStack cstack)
{
	static PQExpBuffer destination = NULL;
	char		buf[MAX_PROMPT_SUBST_SIZE + 1];
	bool		esc = false;
	const char *p;
	const char *prompt_string = "? ";
	static size_t last_prompt1_width = 0;

	if (destination == NULL)
		destination = createPQExpBuffer();
	else
		resetPQExpBuffer(destination);

	if (PQExpBufferBroken(destination))
		pg_fatal("out of memory");

	switch (status)
	{
		case PROMPT_READY:
			prompt_string = pset.prompt1;
			break;

		case PROMPT_CONTINUE:
		case PROMPT_SINGLEQUOTE:
		case PROMPT_DOUBLEQUOTE:
		case PROMPT_DOLLARQUOTE:
		case PROMPT_COMMENT:
		case PROMPT_PAREN:
			prompt_string = pset.prompt2;
			break;

		case PROMPT_COPY:
			prompt_string = pset.prompt3;
			break;
	}

	for (p = prompt_string;
		 *p && destination->len < MAX_PROMPT_SIZE;
		 p++)
	{
		memset(buf, 0, sizeof(buf));
		if (esc)
		{
			switch (*p)
			{
					/* Current database */
				case '/':
					if (pset.db)
						strlcpy(buf, PQdb(pset.db), sizeof(buf));
					break;
				case '~':
					if (pset.db)
					{
						const char *var;

						if (strcmp(PQdb(pset.db), PQuser(pset.db)) == 0 ||
							((var = getenv("PGDATABASE")) && strcmp(var, PQdb(pset.db)) == 0))
							strlcpy(buf, "~", sizeof(buf));
						else
							strlcpy(buf, PQdb(pset.db), sizeof(buf));
					}
					break;

					/* Whitespace of the same width as the last PROMPT1 */
				case 'w':
					if (pset.db)
						memset(buf, ' ',
							   Min(last_prompt1_width, sizeof(buf) - 1));
					break;

					/* DB server hostname (long/short) */
				case 'M':
				case 'm':
					if (pset.db)
					{
						const char *host = PQhost(pset.db);

						/* INET socket */
						if (host && host[0] && !is_unixsock_path(host))
						{
							strlcpy(buf, host, sizeof(buf));
							if (*p == 'm')
								buf[strcspn(buf, ".")] = '\0';
						}
						/* UNIX socket */
						else
						{
							if (!host
								|| strcmp(host, DEFAULT_PGSOCKET_DIR) == 0
								|| *p == 'm')
								strlcpy(buf, "[local]", sizeof(buf));
							else
								snprintf(buf, sizeof(buf), "[local:%s]", host);
						}
					}
					break;
					/* DB server port number */
				case '>':
					if (pset.db && PQport(pset.db))
						strlcpy(buf, PQport(pset.db), sizeof(buf));
					break;
					/* DB server user name */
				case 'n':
					if (pset.db)
						strlcpy(buf, session_username(), sizeof(buf));
					break;
					/* search_path */
				case 'S':
					if (pset.db)
					{
						const char *sp = PQparameterStatus(pset.db, "search_path");

						/* Use ? for versions that don't report search_path. */
						strlcpy(buf, sp ? sp : "?", sizeof(buf));
					}
					break;
					/* service name */
				case 's':
					{
						const char *service_name = GetVariable(pset.vars, "SERVICE");

						if (service_name)
							strlcpy(buf, service_name, sizeof(buf));
					}
					break;
					/* backend pid */
				case 'p':
					if (pset.db)
					{
						int			pid = PQbackendPID(pset.db);

						if (pid)
							snprintf(buf, sizeof(buf), "%d", pid);
					}
					break;
					/* pipeline status */
				case 'P':
					if (pset.db)
					{
						PGpipelineStatus status = PQpipelineStatus(pset.db);

						if (status == PQ_PIPELINE_ON)
							strlcpy(buf, "on", sizeof(buf));
						else if (status == PQ_PIPELINE_ABORTED)
							strlcpy(buf, "abort", sizeof(buf));
						else
							strlcpy(buf, "off", sizeof(buf));
					}
					break;
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					*buf = (char) strtol(p, unconstify(char **, &p), 8);
					--p;
					break;
				case 'R':
					switch (status)
					{
						case PROMPT_READY:
							if (cstack != NULL && !conditional_active(cstack))
								buf[0] = '@';
							else if (!pset.db)
								buf[0] = '!';
							else if (!pset.singleline)
								buf[0] = '=';
							else
								buf[0] = '^';
							break;
						case PROMPT_CONTINUE:
							buf[0] = '-';
							break;
						case PROMPT_SINGLEQUOTE:
							buf[0] = '\'';
							break;
						case PROMPT_DOUBLEQUOTE:
							buf[0] = '"';
							break;
						case PROMPT_DOLLARQUOTE:
							buf[0] = '$';
							break;
						case PROMPT_COMMENT:
							buf[0] = '*';
							break;
						case PROMPT_PAREN:
							buf[0] = '(';
							break;
						default:
							buf[0] = '\0';
							break;
					}
					break;
				case 'i':
					if (pset.db)
					{
						const char *hs = PQparameterStatus(pset.db, "in_hot_standby");

						if (hs)
						{
							if (strcmp(hs, "on") == 0)
								strlcpy(buf, "standby", sizeof(buf));
							else
								strlcpy(buf, "primary", sizeof(buf));
						}
						/* Use ? for versions that don't report in_hot_standby */
						else
							buf[0] = '?';
					}
					break;
				case 'x':
					if (!pset.db)
						buf[0] = '?';
					else
						switch (PQtransactionStatus(pset.db))
						{
							case PQTRANS_IDLE:
								buf[0] = '\0';
								break;
							case PQTRANS_ACTIVE:
							case PQTRANS_INTRANS:
								buf[0] = '*';
								break;
							case PQTRANS_INERROR:
								buf[0] = '!';
								break;
							default:
								buf[0] = '?';
								break;
						}
					break;

				case 'l':
					snprintf(buf, sizeof(buf), UINT64_FORMAT, pset.stmt_lineno);
					break;

				case '?':
					/* not here yet */
					break;

				case 'D':
					strlcpy(buf, prompt_command_result, sizeof(buf));
					break;

				case '#':
					if (is_superuser())
						buf[0] = '#';
					else
						buf[0] = '>';
					break;

					/* execute command */
				case '`':
					{
						int			cmdend = strcspn(p + 1, "`");
						char	   *file = pnstrdup(p + 1, cmdend);
						FILE	   *fd;

						fflush(NULL);
						fd = popen(file, "r");
						if (fd)
						{
							if (fgets(buf, sizeof(buf), fd) == NULL)
								buf[0] = '\0';
							pclose(fd);
						}

						/* strip trailing newline and carriage return */
						(void) pg_strip_crlf(buf);

						pfree(file);
						p += cmdend + 1;
						break;
					}

					/* interpolate variable */
				case ':':
					{
						int			nameend = strcspn(p + 1, ":");
						char	   *name = pnstrdup(p + 1, nameend);
						const char *val;

						val = GetVariable(pset.vars, name);
						if (val)
							strlcpy(buf, val, sizeof(buf));
						pfree(name);
						p += nameend + 1;
						break;
					}

				case '[':
				case ']':
#if defined(USE_READLINE) && defined(RL_PROMPT_START_IGNORE)

					/*
					 * readline >=4.0 undocumented feature: non-printing
					 * characters in prompt strings must be marked as such, in
					 * order to properly display the line during editing.
					 */
					buf[0] = (*p == '[') ? RL_PROMPT_START_IGNORE : RL_PROMPT_END_IGNORE;
					buf[1] = '\0';
#endif							/* USE_READLINE */
					break;

				default:
					buf[0] = *p;
					buf[1] = '\0';
					break;
			}
			esc = false;
		}
		else if (*p == '%')
			esc = true;
		else
		{
			buf[0] = *p;
			buf[1] = '\0';
			esc = false;
		}

		if (!esc)
			appendPQExpBufferStr(destination, buf);
	}

	if (PQExpBufferBroken(destination))
		pg_fatal("out of memory");

	/* Compute the visible width of PROMPT1, for PROMPT2's %w */
	if (prompt_string == pset.prompt1)
	{
		char	   *p = destination->data;
		char	   *end = p + destination->len;
		bool		visible = true;

		last_prompt1_width = 0;
		while (*p)
		{
#if defined(USE_READLINE) && defined(RL_PROMPT_START_IGNORE)
			if (*p == RL_PROMPT_START_IGNORE)
			{
				visible = false;
				++p;
			}
			else if (*p == RL_PROMPT_END_IGNORE)
			{
				visible = true;
				++p;
			}
			else
#endif
			{
				int			chlen,
							chwidth;

				chlen = PQmblen(p, pset.encoding);
				if (p + chlen > end)
					break;		/* Invalid string */

				if (visible)
				{
					chwidth = PQdsplen(p, pset.encoding);

					if (*p == '\n')
						last_prompt1_width = 0;
					else if (chwidth > 0)
						last_prompt1_width += chwidth;
				}

				p += chlen;
			}
		}
	}

	return destination->data;
}
