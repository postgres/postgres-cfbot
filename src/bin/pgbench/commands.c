/*-------------------------------------------------------------------------
 *
 * commands.c
 *	  Meta-command execution and prepared statement helpers for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/commands.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <signal.h>

#include "commands.h"
#include "common/logging.h"
#include "engine.h"
#include "fe_utils/conditional.h"
#include "script.h"
#include "stats.h"
#include "variable.h"

/*
 * Run a shell command. The result is assigned to the variable if not NULL.
 * Return true if succeeded, or false on error.
 */
static bool
runShellCommand(Variables *variables, char *variable, char **argv, int argc)
{
	char		command[SHELL_COMMAND_SIZE];
	int			i,
				len = 0;
	FILE	   *fp;
	char		res[64];
	char	   *endptr;
	int			retval;

	/*----------
	 * Join arguments with whitespace separators. Arguments starting with
	 * exactly one colon are treated as variables:
	 *	name - append a string "name"
	 *	:var - append a variable named 'var'
	 *	::name - append a string ":name"
	 *----------
	 */
	for (i = 0; i < argc; i++)
	{
		char	   *arg;
		int			arglen;

		if (argv[i][0] != ':')
		{
			arg = argv[i];		/* a string literal */
		}
		else if (argv[i][1] == ':')
		{
			arg = argv[i] + 1;	/* a string literal starting with colons */
		}
		else if ((arg = getVariable(variables, argv[i] + 1)) == NULL)
		{
			pg_log_error("%s: undefined variable \"%s\"", argv[0], argv[i]);
			return false;
		}

		arglen = strlen(arg);
		if (len + arglen + (i > 0 ? 1 : 0) >= SHELL_COMMAND_SIZE - 1)
		{
			pg_log_error("%s: shell command is too long", argv[0]);
			return false;
		}

		if (i > 0)
			command[len++] = ' ';
		memcpy(command + len, arg, arglen);
		len += arglen;
	}

	command[len] = '\0';

	fflush(NULL);				/* needed before either system() or popen() */

	/* Fast path for non-assignment case */
	if (variable == NULL)
	{
		if (system(command))
		{
			if (!timer_exceeded)
				pg_log_error("%s: could not launch shell command", argv[0]);
			return false;
		}
		return true;
	}

	/* Execute the command with pipe and read the standard output. */
	if ((fp = popen(command, "r")) == NULL)
	{
		pg_log_error("%s: could not launch shell command", argv[0]);
		return false;
	}
	if (fgets(res, sizeof(res), fp) == NULL)
	{
		if (!timer_exceeded)
			pg_log_error("%s: could not read result of shell command", argv[0]);
		(void) pclose(fp);
		return false;
	}
	if (pclose(fp) < 0)
	{
		pg_log_error("%s: could not run shell command: %m", argv[0]);
		return false;
	}

	/* Check whether the result is an integer and assign it to the variable */
	retval = (int) strtol(res, &endptr, 10);
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;
	if (*res == '\0' || *endptr != '\0')
	{
		pg_log_error("%s: shell command must return an integer (not \"%s\")", argv[0], res);
		return false;
	}
	if (!putVariableInt(variables, "setshell", variable, retval))
		return false;

	pg_log_debug("%s: shell parameter name: \"%s\", value: \"%s\"", argv[0], argv[1], res);

	return true;
}

/*
 * Report the abortion of the client when processing SQL commands.
 */
void
commandFailed(CState *st, const char *cmd, const char *message)
{
	pg_log_error("client %d aborted in command %d (%s) of script %d; %s",
				 st->id, st->command, cmd, st->use_file, message);
}

/*
 * Report the error in the command while the script is executing.
 */
void
commandError(CState *st, const char *message)
{
	/*
	 * Errors should only be detected during an SQL command or the
	 * \endpipeline meta command. Any other case triggers an assertion
	 * failure.
	 */
	Assert(sql_script[st->use_file].commands[st->command]->type == SQL_COMMAND ||
		   sql_script[st->use_file].commands[st->command]->meta == META_ENDPIPELINE);

	pg_log_info("client %d got an error in command %d (SQL) of script %d; %s",
				st->id, st->command, st->use_file, message);
}

/*
 * Allocate space for CState->prepared: we need one boolean for each command
 * of each script.
 */
static void
allocCStatePrepared(CState *st)
{
	Assert(st->prepared == NULL);

	st->prepared = pg_malloc_array(bool *, num_scripts);
	for (int i = 0; i < num_scripts; i++)
	{
		ParsedScript *script = &sql_script[i];
		int			numcmds;

		for (numcmds = 0; script->commands[numcmds] != NULL; numcmds++)
			;
		st->prepared[i] = pg_malloc0_array(bool, numcmds);
	}
}

/*
 * Prepare the SQL command from st->use_file at command_num.
 */
void
prepareCommand(CState *st, int command_num)
{
	Command    *command = sql_script[st->use_file].commands[command_num];

	/* No prepare for non-SQL commands */
	if (command->type != SQL_COMMAND)
		return;

	if (!st->prepared)
		allocCStatePrepared(st);

	if (!st->prepared[st->use_file][command_num])
	{
		PGresult   *res;

		pg_log_debug("client %d preparing %s", st->id, command->prepname);
		res = PQprepare(st->con, command->prepname,
						command->argv[0], command->argc - 1, NULL);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pg_log_error("%s", PQerrorMessage(st->con));
		PQclear(res);
		st->prepared[st->use_file][command_num] = true;
	}
}

/*
 * Prepare all the commands in the script that come after the \startpipeline
 * that's at position st->command, and the first \endpipeline we find.
 *
 * This sets the ->prepared flag for each relevant command as well as the
 * \startpipeline itself, but doesn't move the st->command counter.
 */
static void
prepareCommandsInPipeline(CState *st)
{
	int			j;
	Command   **commands = sql_script[st->use_file].commands;

	Assert(commands[st->command]->type == META_COMMAND &&
		   commands[st->command]->meta == META_STARTPIPELINE);

	if (!st->prepared)
		allocCStatePrepared(st);

	/*
	 * We set the 'prepared' flag on the \startpipeline itself to flag that we
	 * don't need to do this next time without calling prepareCommand(), even
	 * though we don't actually prepare this command.
	 */
	if (st->prepared[st->use_file][st->command])
		return;

	for (j = st->command + 1; commands[j] != NULL; j++)
	{
		if (commands[j]->type == META_COMMAND &&
			commands[j]->meta == META_ENDPIPELINE)
			break;

		prepareCommand(st, j);
	}

	st->prepared[st->use_file][st->command] = true;
}

/*
 * Parse the argument to a \sleep command, and return the requested amount
 * of delay, in microseconds.  Returns true on success, false on error.
 */
static bool
evaluateSleep(Variables *variables, int argc, char **argv, int *usecs)
{
	char	   *var;
	int			usec;

	if (*argv[1] == ':')
	{
		if ((var = getVariable(variables, argv[1] + 1)) == NULL)
		{
			pg_log_error("%s: undefined variable \"%s\"", argv[0], argv[1] + 1);
			return false;
		}

		usec = atoi(var);

		/* Raise an error if the value of a variable is not a number */
		if (usec == 0 && !isdigit((unsigned char) *var))
		{
			pg_log_error("%s: invalid sleep time \"%s\" for variable \"%s\"",
						 argv[0], var, argv[1] + 1);
			return false;
		}
	}
	else
		usec = atoi(argv[1]);

	if (argc > 2)
	{
		if (pg_strcasecmp(argv[2], "ms") == 0)
			usec *= 1000;
		else if (pg_strcasecmp(argv[2], "s") == 0)
			usec *= 1000000;
	}
	else
		usec *= 1000000;

	*usecs = usec;
	return true;
}

/*
 * Subroutine for advanceConnectionState -- initiate or execute the current
 * meta command, and return the next state to set.
 *
 * *now is updated to the current time, unless the command is expected to
 * take no time to execute.
 */
ConnectionStateEnum
executeMetaCommand(CState *st, pg_time_usec_t *now)
{
	Command    *command = sql_script[st->use_file].commands[st->command];
	int			argc;
	char	  **argv;

	Assert(command != NULL && command->type == META_COMMAND);

	argc = command->argc;
	argv = command->argv;

	if (unlikely(__pg_log_level <= PG_LOG_DEBUG))
	{
		PQExpBufferData buf;

		initPQExpBuffer(&buf);

		printfPQExpBuffer(&buf, "client %d executing \\%s", st->id, argv[0]);
		for (int i = 1; i < argc; i++)
			appendPQExpBuffer(&buf, " %s", argv[i]);

		pg_log_debug("%s", buf.data);

		termPQExpBuffer(&buf);
	}

	if (command->meta == META_SLEEP)
	{
		int			usec;

		/*
		 * A \sleep doesn't execute anything, we just get the delay from the
		 * argument, and enter the CSTATE_SLEEP state.  (The per-command
		 * latency will be recorded in CSTATE_SLEEP state, not here, after the
		 * delay has elapsed.)
		 */
		if (!evaluateSleep(&st->variables, argc, argv, &usec))
		{
			commandFailed(st, "sleep", "execution of meta-command failed");
			return CSTATE_ABORTED;
		}

		pg_time_now_lazy(now);
		st->sleep_until = (*now) + usec;
		return CSTATE_SLEEP;
	}
	else if (command->meta == META_SET)
	{
		PgBenchExpr *expr = command->expr;
		PgBenchValue result;

		if (!evaluateExpr(st, expr, &result))
		{
			commandFailed(st, argv[0], "evaluation of meta-command failed");
			return CSTATE_ABORTED;
		}

		if (!putVariableValue(&st->variables, argv[0], argv[1], &result))
		{
			commandFailed(st, "set", "assignment of meta-command failed");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_IF)
	{
		/* backslash commands with an expression to evaluate */
		PgBenchExpr *expr = command->expr;
		PgBenchValue result;
		bool		cond;

		if (!evaluateExpr(st, expr, &result))
		{
			commandFailed(st, argv[0], "evaluation of meta-command failed");
			return CSTATE_ABORTED;
		}

		cond = valueTruth(&result);
		conditional_stack_push(st->cstack, cond ? IFSTATE_TRUE : IFSTATE_FALSE);
	}
	else if (command->meta == META_ELIF)
	{
		/* backslash commands with an expression to evaluate */
		PgBenchExpr *expr = command->expr;
		PgBenchValue result;
		bool		cond;

		if (conditional_stack_peek(st->cstack) == IFSTATE_TRUE)
		{
			/* elif after executed block, skip eval and wait for endif. */
			conditional_stack_poke(st->cstack, IFSTATE_IGNORED);
			return CSTATE_END_COMMAND;
		}

		if (!evaluateExpr(st, expr, &result))
		{
			commandFailed(st, argv[0], "evaluation of meta-command failed");
			return CSTATE_ABORTED;
		}

		cond = valueTruth(&result);
		Assert(conditional_stack_peek(st->cstack) == IFSTATE_FALSE);
		conditional_stack_poke(st->cstack, cond ? IFSTATE_TRUE : IFSTATE_FALSE);
	}
	else if (command->meta == META_ELSE)
	{
		switch (conditional_stack_peek(st->cstack))
		{
			case IFSTATE_TRUE:
				conditional_stack_poke(st->cstack, IFSTATE_ELSE_FALSE);
				break;
			case IFSTATE_FALSE: /* inconsistent if active */
			case IFSTATE_IGNORED:	/* inconsistent if active */
			case IFSTATE_NONE:	/* else without if */
			case IFSTATE_ELSE_TRUE: /* else after else */
			case IFSTATE_ELSE_FALSE:	/* else after else */
			default:
				/* dead code if conditional check is ok */
				Assert(false);
		}
	}
	else if (command->meta == META_ENDIF)
	{
		Assert(!conditional_stack_empty(st->cstack));
		conditional_stack_pop(st->cstack);
	}
	else if (command->meta == META_SETSHELL)
	{
		if (!runShellCommand(&st->variables, argv[1], argv + 2, argc - 2))
		{
			commandFailed(st, "setshell", "execution of meta-command failed");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_SHELL)
	{
		if (!runShellCommand(&st->variables, NULL, argv + 1, argc - 1))
		{
			commandFailed(st, "shell", "execution of meta-command failed");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_STARTPIPELINE)
	{
		/*
		 * In pipeline mode, we use a workflow based on libpq pipeline
		 * functions.
		 */
		if (querymode == QUERY_SIMPLE)
		{
			commandFailed(st, "startpipeline", "cannot use pipeline mode with the simple query protocol");
			return CSTATE_ABORTED;
		}

		/*
		 * If we're in prepared-query mode, we need to prepare all the
		 * commands that are inside the pipeline before we actually start the
		 * pipeline itself.  This solves the problem that running BEGIN
		 * ISOLATION LEVEL SERIALIZABLE in a pipeline would fail due to a
		 * snapshot having been acquired by the prepare within the pipeline.
		 */
		if (querymode == QUERY_PREPARED)
			prepareCommandsInPipeline(st);

		if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
		{
			commandFailed(st, "startpipeline", "already in pipeline mode");
			return CSTATE_ABORTED;
		}
		if (PQenterPipelineMode(st->con) == 0)
		{
			commandFailed(st, "startpipeline", "failed to enter pipeline mode");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_SYNCPIPELINE)
	{
		if (PQpipelineStatus(st->con) != PQ_PIPELINE_ON)
		{
			commandFailed(st, "syncpipeline", "not in pipeline mode");
			return CSTATE_ABORTED;
		}
		if (PQsendPipelineSync(st->con) == 0)
		{
			commandFailed(st, "syncpipeline", "failed to send a pipeline sync");
			return CSTATE_ABORTED;
		}
		st->num_syncs++;
	}
	else if (command->meta == META_ENDPIPELINE)
	{
		if (PQpipelineStatus(st->con) != PQ_PIPELINE_ON)
		{
			commandFailed(st, "endpipeline", "not in pipeline mode");
			return CSTATE_ABORTED;
		}
		if (!PQpipelineSync(st->con))
		{
			commandFailed(st, "endpipeline", "failed to send a pipeline sync");
			return CSTATE_ABORTED;
		}
		st->num_syncs++;
		/* Now wait for the PGRES_PIPELINE_SYNC and exit pipeline mode there */
		/* collect pending results before getting out of pipeline mode */
		return CSTATE_WAIT_RESULT;
	}

	/*
	 * executing the expression or shell command might have taken a
	 * non-negligible amount of time, so reset 'now'
	 */
	*now = 0;

	return CSTATE_END_COMMAND;
}
