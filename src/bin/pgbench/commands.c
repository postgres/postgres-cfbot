/*-------------------------------------------------------------------------
 *
 * commands.c
 *		pgbench meta-commands execution and pluggable handler registry
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
#include <math.h>

#include "commands.h"
#include "common/logging.h"
#include "fe_utils/conditional.h"
#include "fe_utils/string_utils.h"
#include "pgbench.h"
#include "script.h"
#include "variable.h"

#define SHELL_COMMAND_SIZE	256 /* maximum size allowed for shell command */

/*
 * Report the abortion of the client when processing SQL commands or meta commands.
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
void
allocCStatePrepared(CState *st)
{
	int			i;

	Assert(st->prepared == NULL);

	st->prepared = pg_malloc_array(bool *, num_scripts);

	for (i = 0; i < num_scripts; i++)
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
void
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
bool
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
 * Run a shell command. The result is assigned to the variable if not NULL.
 */
bool
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
 * Meta-command Handlers
 */

ConnectionStateEnum
handle_cmd_sleep(CState *st, Command *cmd, pg_time_usec_t *now)
{
	int			usec;

	/*
	 * A \sleep doesn't execute anything, we just get the delay from the
	 * argument, and enter the CSTATE_SLEEP state.  (The per-command
	 * latency will be recorded in CSTATE_SLEEP state, not here, after the
	 * delay has elapsed.)
	 */
	if (!evaluateSleep(&st->variables, cmd->argc, cmd->argv, &usec))
	{
		commandFailed(st, "sleep", "execution of meta-command failed");
		return CSTATE_ABORTED;
	}

	pg_time_now_lazy(now);
	st->sleep_until = (*now) + usec;
	return CSTATE_SLEEP;
}

ConnectionStateEnum
handle_cmd_set(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchExpr *expr = cmd->expr;
	PgBenchValue result;

	if (!evaluateExpr(st, expr, &result))
	{
		commandFailed(st, cmd->argv[0], "evaluation of meta-command failed");
		return CSTATE_ABORTED;
	}

	if (!putVariableValue(&st->variables, cmd->argv[0], cmd->argv[1], &result))
	{
		commandFailed(st, "set", "assignment of meta-command failed");
		return CSTATE_ABORTED;
	}

	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_if(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchExpr *expr = cmd->expr;
	PgBenchValue result;
	bool		cond;

	if (!evaluateExpr(st, expr, &result))
	{
		commandFailed(st, cmd->argv[0], "evaluation of meta-command failed");
		return CSTATE_ABORTED;
	}

	cond = valueTruth(&result);
	conditional_stack_push(st->cstack, cond ? IFSTATE_TRUE : IFSTATE_FALSE);
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_elif(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchExpr *expr = cmd->expr;
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
		commandFailed(st, cmd->argv[0], "evaluation of meta-command failed");
		return CSTATE_ABORTED;
	}

	cond = valueTruth(&result);
	Assert(conditional_stack_peek(st->cstack) == IFSTATE_FALSE);
	conditional_stack_poke(st->cstack, cond ? IFSTATE_TRUE : IFSTATE_FALSE);
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_else(CState *st, Command *cmd, pg_time_usec_t *now)
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
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_endif(CState *st, Command *cmd, pg_time_usec_t *now)
{
	Assert(!conditional_stack_empty(st->cstack));
	conditional_stack_pop(st->cstack);
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_setshell(CState *st, Command *cmd, pg_time_usec_t *now)
{
	if (!runShellCommand(&st->variables, cmd->argv[1], cmd->argv + 2, cmd->argc - 2))
	{
		commandFailed(st, "setshell", "execution of meta-command failed");
		return CSTATE_ABORTED;
	}
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_shell(CState *st, Command *cmd, pg_time_usec_t *now)
{
	if (!runShellCommand(&st->variables, NULL, cmd->argv + 1, cmd->argc - 1))
	{
		commandFailed(st, "shell", "execution of meta-command failed");
		return CSTATE_ABORTED;
	}
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_startpipeline(CState *st, Command *cmd, pg_time_usec_t *now)
{
	/*
	 * In pipeline mode, we use a workflow based on libpq pipeline functions.
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
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_syncpipeline(CState *st, Command *cmd, pg_time_usec_t *now)
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
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_endpipeline(CState *st, Command *cmd, pg_time_usec_t *now)
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

/* Pluggable meta-command dispatch table */
static const CommandDescriptor command_handlers[] = {
	{"set", META_SET, handle_cmd_set},
	{"setshell", META_SETSHELL, handle_cmd_setshell},
	{"shell", META_SHELL, handle_cmd_shell},
	{"sleep", META_SLEEP, handle_cmd_sleep},
	{"if", META_IF, handle_cmd_if},
	{"elif", META_ELIF, handle_cmd_elif},
	{"else", META_ELSE, handle_cmd_else},
	{"endif", META_ENDIF, handle_cmd_endif},
	{"startpipeline", META_STARTPIPELINE, handle_cmd_startpipeline},
	{"syncpipeline", META_SYNCPIPELINE, handle_cmd_syncpipeline},
	{"endpipeline", META_ENDPIPELINE, handle_cmd_endpipeline},
	{NULL, META_NONE, NULL}
};

/*
 * Execute a meta command and return the next connection state.
 */
ConnectionStateEnum
executeMetaCommand(CState *st, pg_time_usec_t *now)
{
	Command    *command = sql_script[st->use_file].commands[st->command];
	int			argc;
	char	  **argv;
	int			i;
	const CommandDescriptor *desc;

	Assert(command != NULL && command->type == META_COMMAND);

	argc = command->argc;
	argv = command->argv;

	if (unlikely(__pg_log_level <= PG_LOG_DEBUG))
	{
		PQExpBufferData buf;

		initPQExpBuffer(&buf);

		printfPQExpBuffer(&buf, "client %d executing \\%s", st->id, argv[0]);
		for (i = 1; i < argc; i++)
			appendPQExpBuffer(&buf, " %s", argv[i]);

		pg_log_debug("%s", buf.data);

		termPQExpBuffer(&buf);
	}

	for (desc = command_handlers; desc->name != NULL; desc++)
	{
		if (desc->meta == command->meta)
		{
			ConnectionStateEnum next_state = desc->handler(st, command, now);

			if (next_state == CSTATE_END_COMMAND)
				*now = 0;

			return next_state;
		}
	}

	pg_log_error("client %d: unknown meta-command %d", st->id, (int) command->meta);
	return CSTATE_ABORTED;
}

/*
 * Skip commands until an active branch or matching endif is reached.
 * Implements the skipping logic for CSTATE_SKIP_COMMAND.
 */
void
skipConditionalCommands(CState *st)
{
	Command    *command;

	Assert(!conditional_active(st->cstack));

	while (true)
	{
		command = sql_script[st->use_file].commands[st->command];

		/* cannot reach end of script in that state */
		Assert(command != NULL);

		/*
		 * if this is conditional related, update conditional
		 * state
		 */
		if (command->type == META_COMMAND &&
			(command->meta == META_IF ||
			 command->meta == META_ELIF ||
			 command->meta == META_ELSE ||
			 command->meta == META_ENDIF))
		{
			switch (conditional_stack_peek(st->cstack))
			{
				case IFSTATE_FALSE:
					if (command->meta == META_IF)
					{
						/* nested if in skipped branch - ignore */
						conditional_stack_push(st->cstack, IFSTATE_IGNORED);
						st->command++;
					}
					else if (command->meta == META_ELIF)
					{
						/* we must evaluate the condition */
						st->state = CSTATE_START_COMMAND;
					}
					else if (command->meta == META_ELSE)
					{
						/* we must execute next command */
						conditional_stack_poke(st->cstack, IFSTATE_ELSE_TRUE);
						st->state = CSTATE_START_COMMAND;
						st->command++;
					}
					else if (command->meta == META_ENDIF)
					{
						Assert(!conditional_stack_empty(st->cstack));
						conditional_stack_pop(st->cstack);
						if (conditional_active(st->cstack))
							st->state = CSTATE_START_COMMAND;
						/* else state remains CSTATE_SKIP_COMMAND */
						st->command++;
					}
					break;

				case IFSTATE_IGNORED:
				case IFSTATE_ELSE_FALSE:
					if (command->meta == META_IF)
						conditional_stack_push(st->cstack, IFSTATE_IGNORED);
					else if (command->meta == META_ENDIF)
					{
						Assert(!conditional_stack_empty(st->cstack));
						conditional_stack_pop(st->cstack);
						if (conditional_active(st->cstack))
							st->state = CSTATE_START_COMMAND;
					}
					/* could detect "else" & "elif" after "else" */
					st->command++;
					break;

				case IFSTATE_NONE:
				case IFSTATE_TRUE:
				case IFSTATE_ELSE_TRUE:
				default:
					/*
					 * inconsistent if inactive, unreachable dead code
					 */
					Assert(false);
			}
		}
		else
		{
			/* skip and consider next */
			st->command++;
		}

		if (st->state != CSTATE_SKIP_COMMAND)
			/* out of quick skip command loop */
			break;
	}
}

/*
 * Process \gset and \aset variable storage from query result.
 * Returns true if successful, false on error.
 */
bool
processGSetResult(CState *st, Command *command, PGresult *res, bool is_last, int qrynum)
{
	MetaCommand meta = command->meta;
	char	   *varprefix = command->varprefix;
	int			ntuples = PQntuples(res);
	const char *context = (meta == META_ASET) ? "aset" : "gset";
	int			fld;

	if (meta == META_GSET && ntuples != 1)
	{
		pg_log_error("client %d script %d command %d query %d: expected one row, got %d",
					 st->id, st->use_file, st->command, qrynum, ntuples);
		st->estatus = ESTATUS_META_COMMAND_ERROR;
		return false;
	}
	else if (meta == META_ASET && ntuples <= 0)
	{
		/* skip empty result under \aset */
		return true;
	}

	/* store results into variables */
	for (fld = 0; fld < PQnfields(res); fld++)
	{
		char	   *varname = PQfname(res, fld);
		bool		ok;

		if (*varprefix != '\0')
			varname = psprintf("%s%s", varprefix, varname);

		/* store last row result */
		if (PQgetisnull(res, ntuples - 1, fld))
		{
			PgBenchValue nullval;

			setNullValue(&nullval);
			ok = putVariableValue(&st->variables, context,
								  varname, &nullval);
		}
		else
			ok = putVariable(&st->variables, context, varname,
							 PQgetvalue(res, ntuples - 1, fld));

		if (!ok)
		{
			pg_log_error("client %d script %d command %d query %d: error storing into variable %s",
						 st->id, st->use_file, st->command, qrynum, varname);
			st->estatus = ESTATUS_META_COMMAND_ERROR;
			if (*varprefix != '\0')
				pfree(varname);
			return false;
		}

		if (*varprefix != '\0')
			pfree(varname);
	}

	return true;
}
