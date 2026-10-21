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
#include <math.h>
#include <signal.h>

#include "commands.h"
#include "common/int.h"
#include "common/logging.h"
#include "dataset.h"
#include "engine.h"
#include "fe_utils/conditional.h"
#include "script.h"
#include "stats.h"
#include "variable.h"

#define SHELL_COMMAND_SIZE	256 /* maximum size allowed for shell command */
#define PGBENCH_MAX_GSET_COL_ROWS 100000	/* max rows captured by \gset_col / \aset */

/*
 * Run a shell command. The result is assigned to the variable if not NULL.
 * Return true if succeeded, or false on error.
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
void
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

void
freeCStatePrepared(CState *st)
{
	if (st->prepared != NULL)
	{
		for (int i = 0; i < num_scripts; i++)
			pg_free(st->prepared[i]);
		pg_free(st->prepared);
		st->prepared = NULL;
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
evaluateSleep(Variables *variables, int argc, char **argv, int64 *usecs)
{
	char	   *var;
	int64		usec;
	double		dval = 0.0;
	bool		is_float = false;

	if (*argv[1] == ':')
	{
		char	   *endptr = NULL;

		if ((var = getVariable(variables, argv[1] + 1)) == NULL)
		{
			pg_log_error("%s: undefined variable \"%s\"", argv[0], argv[1] + 1);
			return false;
		}

		dval = strtod(var, &endptr);
		while (endptr != NULL && *endptr != '\0' && isspace((unsigned char) *endptr))
			endptr++;
		if (*var == '\0' || endptr == var || (endptr != NULL && *endptr != '\0'))
		{
			pg_log_error("%s: invalid sleep time \"%s\" for variable \"%s\"",
						 argv[0], var, argv[1] + 1);
			return false;
		}
		is_float = true;
	}
	else
		usec = atoll(argv[1]);

	if (is_float)
	{
		double		mult = 1000000.0;

		if (argc > 2)
		{
			if (pg_strcasecmp(argv[2], "us") == 0)
				mult = 1.0;
			else if (pg_strcasecmp(argv[2], "ms") == 0)
				mult = 1000.0;
			else if (pg_strcasecmp(argv[2], "s") == 0)
				mult = 1000000.0;
		}
		usec = (int64) llround(dval * mult);
	}
	else
	{
		if (argc > 2)
		{
			if (pg_strcasecmp(argv[2], "ms") == 0)
				usec *= INT64CONST(1000);
			else if (pg_strcasecmp(argv[2], "s") == 0)
				usec *= INT64CONST(1000000);
		}
		else
			usec *= INT64CONST(1000000);
	}

	if (usec < 0)
		usec = 0;

	*usecs = usec;
	return true;
}

/*
 * Meta-command Handlers
 */

ConnectionStateEnum
handle_cmd_sleep(CState *st, Command *cmd, pg_time_usec_t *now)
{
	int64		usec = 0;

	/*
	 * A \sleep, \keying_time, or \think_time gets the delay from the argument
	 * or expression and enters the CSTATE_SLEEP state.  (The per-command
	 * latency and keying/think time will be recorded in CSTATE_SLEEP state
	 * after the delay has elapsed.)
	 */
	if (cmd->expr != NULL)
	{
		PgBenchValue val;
		double		dval;
		double		mult = 1000000.0;

		if (!evaluateExpr(st, cmd->expr, &val))
		{
			commandFailed(st, cmd->argv[0], "evaluation of meta-command failed");
			return CSTATE_ABORTED;
		}

		if (!coerceToDouble(&val, &dval))
		{
			freeValue(&val);
			commandFailed(st, cmd->argv[0], "invalid sleep time, must be a number");
			return CSTATE_ABORTED;
		}
		freeValue(&val);

		if (cmd->argc > 2 && cmd->argv[2] != NULL)
		{
			if (pg_strcasecmp(cmd->argv[2], "us") == 0)
				mult = 1.0;
			else if (pg_strcasecmp(cmd->argv[2], "ms") == 0)
				mult = 1000.0;
			else if (pg_strcasecmp(cmd->argv[2], "s") == 0)
				mult = 1000000.0;
		}

		usec = (int64) llround(dval * mult);
		if (usec < 0)
			usec = 0;
	}
	else
	{
		if (!evaluateSleep(&st->variables, cmd->argc, cmd->argv, &usec))
		{
			commandFailed(st, cmd->argv[0], "execution of meta-command failed");
			return CSTATE_ABORTED;
		}
	}

	/*
	 * Reset *now if cmd->expr was evaluated, then capture start of sleep.
	 */
	if (cmd->expr != NULL)
		*now = 0;
	pg_time_now_lazy(now);
	st->sleep_begin = *now;
	st->sleep_type = (int) cmd->meta;
	if (cmd->meta == META_KEYING_TIME)
		st->has_keying_time = true;
	else if (cmd->meta == META_THINK_TIME)
		st->has_think_time = true;
	st->sleep_until = (*now) + usec;
	return CSTATE_SLEEP;
}

ConnectionStateEnum
handle_cmd_set(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchExpr *expr = cmd->expr;
	PgBenchValue result;
	bool		ok;

	if (!evaluateExpr(st, expr, &result))
	{
		commandFailed(st, cmd->argv[0], "evaluation of meta-command failed");
		return CSTATE_ABORTED;
	}

	if (cmd->index_expr != NULL)
	{
		PgBenchValue idx_val;
		int64		idx;

		if (!evaluateExpr(st, cmd->index_expr, &idx_val))
		{
			freeValue(&result);
			commandFailed(st, cmd->argv[0], "evaluation of array index failed");
			return CSTATE_ABORTED;
		}

		if (!coerceToInt(&idx_val, &idx))
		{
			freeValue(&idx_val);
			freeValue(&result);
			commandFailed(st, cmd->argv[0], "array index is not an integer");
			return CSTATE_ABORTED;
		}
		freeValue(&idx_val);

		ok = putVariableArrayElement(&st->variables, cmd->argv[0],
									 cmd->argv[1], idx, &result);
	}
	else
	{
		ok = putVariableValue(&st->variables, cmd->argv[0], cmd->argv[1], &result);
	}

	freeValue(&result);

	if (!ok)
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
	freeValue(&result);
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
	freeValue(&result);
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
handle_cmd_for(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchValue start_v,
				end_v;
	int64		start_val,
				end_val,
				step_val = 1;
	bool		enter_loop;

	if (!evaluateExpr(st, cmd->expr, &start_v))
	{
		commandFailed(st, cmd->argv[0], "evaluation of start expression failed");
		return CSTATE_ABORTED;
	}
	if (!coerceToInt(&start_v, &start_val))
	{
		freeValue(&start_v);
		commandFailed(st, cmd->argv[0], "start expression is not an integer");
		return CSTATE_ABORTED;
	}
	freeValue(&start_v);

	if (!evaluateExpr(st, cmd->end_expr, &end_v))
	{
		commandFailed(st, cmd->argv[0], "evaluation of end expression failed");
		return CSTATE_ABORTED;
	}
	if (!coerceToInt(&end_v, &end_val))
	{
		freeValue(&end_v);
		commandFailed(st, cmd->argv[0], "end expression is not an integer");
		return CSTATE_ABORTED;
	}
	freeValue(&end_v);

	if (cmd->step_expr != NULL)
	{
		PgBenchValue step_v;

		if (!evaluateExpr(st, cmd->step_expr, &step_v))
		{
			commandFailed(st, cmd->argv[0], "evaluation of step expression failed");
			return CSTATE_ABORTED;
		}
		if (!coerceToInt(&step_v, &step_val))
		{
			freeValue(&step_v);
			commandFailed(st, cmd->argv[0], "step expression is not an integer");
			return CSTATE_ABORTED;
		}
		freeValue(&step_v);

		if (step_val == 0)
		{
			pg_log_error("\\for step cannot be zero");
			commandFailed(st, cmd->argv[0], "step cannot be zero");
			return CSTATE_ABORTED;
		}
	}

	enter_loop = (step_val > 0) ? (start_val <= end_val) : (start_val >= end_val);
	if (enter_loop)
	{
		ForLoopState *loop;

		if (st->loop_depth >= MAX_LOOP_DEPTH)
		{
			commandFailed(st, cmd->argv[0], "maximum loop nesting depth exceeded");
			return CSTATE_ABORTED;
		}

		if (!putVariableInt(&st->variables, "for", cmd->argv[1], start_val))
		{
			commandFailed(st, cmd->argv[0], "assignment of loop variable failed");
			return CSTATE_ABORTED;
		}

		loop = &st->loop_stack[st->loop_depth++];
		loop->for_cmd = st->command;
		loop->varname = cmd->argv[1];
		loop->current = start_val;
		loop->end = end_val;
		loop->step = step_val;
	}
	else
	{
		/* Skip the loop body: jump past matching \endfor */
		st->next_command = cmd->jump_target + 1;
	}

	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_endfor(CState *st, Command *cmd, pg_time_usec_t *now)
{
	ForLoopState *loop;
	int64		next_val;
	bool		cont = false;

	Assert(st->loop_depth > 0);
	loop = &st->loop_stack[st->loop_depth - 1];

	if (!pg_add_s64_overflow(loop->current, loop->step, &next_val))
	{
		cont = (loop->step > 0) ? (next_val <= loop->end) : (next_val >= loop->end);
	}

	if (cont)
	{
		loop->current = next_val;
		if (!putVariableInt(&st->variables, "for", loop->varname, next_val))
		{
			commandFailed(st, cmd->argv[0], "assignment of loop variable failed");
			return CSTATE_ABORTED;
		}
		st->next_command = cmd->jump_target + 1;
	}
	else
	{
		st->loop_depth--;
	}

	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_while(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchValue result;
	bool		cond;

	if (!evaluateExpr(st, cmd->expr, &result))
	{
		commandFailed(st, cmd->argv[0], "evaluation of meta-command failed");
		return CSTATE_ABORTED;
	}

	cond = valueTruth(&result);
	freeValue(&result);

	if (!cond)
	{
		/* Skip the loop body: jump past matching \endwhile */
		st->next_command = cmd->jump_target + 1;
	}

	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_endwhile(CState *st, Command *cmd, pg_time_usec_t *now)
{
	/* Jump back to matching \while to re-evaluate the condition */
	st->next_command = cmd->jump_target;
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_try(CState *st, Command *cmd, pg_time_usec_t *now)
{
	TryState   *ts;
	const char *sqlstate_pat = NULL;

	if (st->try_depth >= MAX_TRY_DEPTH)
	{
		commandFailed(st, cmd->argv[0], "maximum \\try nesting depth exceeded");
		return CSTATE_ABORTED;
	}

	if (cmd->jump_target != cmd->endtry_target)
	{
		Command    *catch_cmd = sql_script[st->use_file].commands[cmd->jump_target];

		if (catch_cmd->argc > 1)
			sqlstate_pat = catch_cmd->argv[1];
	}

	ts = &st->try_stack[st->try_depth++];
	ts->try_cmd = st->command;
	ts->catch_cmd = cmd->jump_target;
	ts->endtry_cmd = cmd->endtry_target;
	ts->sqlstate_pat = sqlstate_pat;
	ts->cstack_depth = conditional_stack_depth(st->cstack);
	ts->loop_depth = st->loop_depth;
	ts->in_catch = false;

	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_catch(CState *st, Command *cmd, pg_time_usec_t *now)
{
	/*
	 * Reaching \catch during normal execution means the \try block completed
	 * without error: pop the try frame and jump past \endtry.
	 */
	if (st->try_depth > 0)
		st->try_depth--;
	st->next_command = cmd->jump_target + 1;
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_endtry(CState *st, Command *cmd, pg_time_usec_t *now)
{
	if (st->try_depth > 0 &&
		st->try_stack[st->try_depth - 1].endtry_cmd == st->command)
		st->try_depth--;
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_expect_error(CState *st, Command *cmd, pg_time_usec_t *now)
{
	st->expect_error = true;
	st->expect_sqlstate = (cmd->argc > 1) ? cmd->argv[1] : NULL;
	return CSTATE_END_COMMAND;
}

ConnectionStateEnum
handle_cmd_gset_meta(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *prefix = (cmd->argc > 1) ? cmd->argv[1] : "";

	if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
	{
		commandFailed(st, "gset_meta", "\\gset_meta is not allowed in pipeline mode");
		return CSTATE_ABORTED;
	}

	if (!populateGSetMeta(st, prefix))
	{
		commandFailed(st, "gset_meta", "execution of meta-command failed");
		return CSTATE_ABORTED;
	}

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

ConnectionStateEnum
handle_cmd_session_connect(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *db_val = NULL;
	const char *user_val = NULL;
	int			pos_arg = 0;

	for (int k = 1; k < cmd->argc; k++)
	{
		const char *arg = cmd->argv[k];
		const char *target = NULL;
		bool		is_db = false;
		bool		is_user = false;

		if (arg == NULL || *arg == '\0')
			continue;

		if (pg_strncasecmp(arg, "db=", 3) == 0)
		{
			target = arg + 3;
			is_db = true;
		}
		else if (pg_strncasecmp(arg, "user=", 5) == 0)
		{
			target = arg + 5;
			is_user = true;
		}
		else
		{
			target = arg;
			if (pos_arg == 0)
				is_db = true;
			else if (pos_arg == 1)
				is_user = true;
			pos_arg++;
		}

		if (target != NULL && target[0] == ':' && target[1] != ':')
		{
			const char *vname = target + 1;

			target = getVariable(&st->variables, (char *) vname);
			if (target == NULL)
			{
				pg_log_error("\\session_connect: undefined variable \"%s\"", vname);
				commandFailed(st, "session_connect", "undefined variable");
				return CSTATE_ABORTED;
			}
		}

		if (is_db)
			db_val = target;
		else if (is_user)
			user_val = target;
	}

	if (st->con != NULL)
	{
		if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
		{
			commandFailed(st, "session_connect",
						  "cannot reconnect while in pipeline mode");
			return CSTATE_ABORTED;
		}
		PQfinish(st->con);
		st->con = NULL;
		(void) __atomic_fetch_sub(&active_sessions_count, 1, __ATOMIC_RELEASE);
		if (max_concurrent_sessions > 0)
			wakeWatermarkWaiters();
	}
	freeCStatePrepared(st);

	pg_free(st->session_target_db);
	st->session_target_db = (db_val != NULL) ? pg_strdup(db_val) : NULL;
	pg_free(st->session_target_user);
	st->session_target_user = (user_val != NULL) ? pg_strdup(user_val) : NULL;

	return CSTATE_IDLE;
}

ConnectionStateEnum
handle_cmd_session_disconnect(CState *st, Command *cmd, pg_time_usec_t *now)
{
	if (st->con != NULL && PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
	{
		commandFailed(st, "session_disconnect",
					  "cannot disconnect while in pipeline mode");
		return CSTATE_ABORTED;
	}
	return CSTATE_DISCONNECTING;
}

static int
findScriptByName(const char *target)
{
	int			idx_match = -1;

	if (target == NULL || *target == '\0')
		return -1;

	/* 1. Exact match on script descriptor */
	for (int i = 0; i < num_scripts; i++)
	{
		if (strcmp(sql_script[i].desc, target) == 0)
			return i;
	}

	/* 2. Exact match on basename or stem (without .sql / .pgbench) */
	for (int i = 0; i < num_scripts; i++)
	{
		const char *desc = sql_script[i].desc;
		const char *slash = strrchr(desc, '/');
		const char *base = (slash != NULL) ? (slash + 1) : desc;
		const char *dot;

		if (strcmp(base, target) == 0)
			return i;

		dot = strrchr(base, '.');
		if (dot != NULL &&
			(pg_strcasecmp(dot, ".sql") == 0 || pg_strcasecmp(dot, ".pgbench") == 0))
		{
			size_t		stem_len = (size_t) (dot - base);

			if (strlen(target) == stem_len && strncmp(base, target, stem_len) == 0)
				return i;
		}
	}

	/* 3. Builtin descriptor match "<builtin: ...>" or substring match */
	for (int i = 0; i < num_scripts; i++)
	{
		if (strstr(sql_script[i].desc, target) != NULL)
		{
			if (idx_match >= 0)
				return -2;		/* ambiguous */
			idx_match = i;
		}
	}
	if (idx_match >= 0)
		return idx_match;

	/* 4. Integer index (1-based or 0-based if valid) */
	if (is_an_int(target))
	{
		int			val = atoi(target);

		if (val >= 0 && val < num_scripts)
			return val;
	}

	return -1;
}

ConnectionStateEnum
handle_cmd_call(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *target_name = cmd->argv[1];
	int			target_script;
	CallFrame  *frame;

	if (target_name[0] == ':' && target_name[1] != ':')
	{
		target_name = getVariable(&st->variables, target_name + 1);
		if (target_name == NULL)
		{
			pg_log_error("\\call: undefined variable \"%s\"", cmd->argv[1] + 1);
			commandFailed(st, "call", "undefined script variable");
			return CSTATE_ABORTED;
		}
	}

	target_script = findScriptByName(target_name);
	if (target_script < 0)
	{
		if (target_script == -2)
			pg_log_error("\\call: ambiguous script name \"%s\"", target_name);
		else
			pg_log_error("\\call: unknown script \"%s\"", target_name);
		commandFailed(st, "call", "target script not found");
		return CSTATE_ABORTED;
	}

	if (st->call_depth >= MAX_CALL_DEPTH)
	{
		pg_log_error("\\call: maximum call nesting depth (%d) exceeded",
					 MAX_CALL_DEPTH);
		commandFailed(st, "call", "maximum \\call nesting depth exceeded");
		return CSTATE_ABORTED;
	}

	/* Bind any optional inline var=val arguments (argv[2..argc-1]) */
	for (int k = 2; k < cmd->argc; k++)
	{
		const char *arg = cmd->argv[k];
		const char *eq = strchr(arg, '=');

		if (eq != NULL && eq > arg)
		{
			char	   *vname = pg_malloc((eq - arg) + 1);
			const char *val_str = eq + 1;
			const char *vn;

			memcpy(vname, arg, eq - arg);
			vname[eq - arg] = '\0';
			vn = (vname[0] == ':') ? (vname + 1) : vname;

			if (val_str[0] == ':' && val_str[1] != ':')
			{
				Variable   *src = lookupVariable(&st->variables, (char *) (val_str + 1));

				if (src == NULL)
				{
					pg_log_error("\\call: undefined variable \"%s\"", val_str + 1);
					pg_free(vname);
					commandFailed(st, "call", "undefined parameter variable");
					return CSTATE_ABORTED;
				}
				if (src->value.type == PGBT_NO_VALUE)
					(void) makeVariableValue(src);
				if (!putVariableValue(&st->variables, "call", (char *) vn, &src->value))
				{
					pg_free(vname);
					return CSTATE_ABORTED;
				}
			}
			else
			{
				int64		ival;

				if (strtoint64(val_str, true, &ival))
				{
					if (!putVariableInt(&st->variables, "call", (char *) vn, ival))
					{
						pg_free(vname);
						return CSTATE_ABORTED;
					}
				}
				else if (!putVariable(&st->variables, "call", (char *) vn, val_str))
				{
					pg_free(vname);
					return CSTATE_ABORTED;
				}
			}
			pg_free(vname);
		}
	}

	pg_time_now_lazy(now);

	/* Ensure per-script stats are collected when \call is used */
	per_script_stats = true;

	frame = &st->call_stack[st->call_depth++];
	frame->script_idx = st->use_file;
	frame->return_cmd = st->command + 1;
	frame->cstack = st->cstack;
	st->cstack = conditional_stack_create();
	frame->loop_depth = st->loop_depth;
	if (st->loop_depth > 0)
		memcpy(frame->loop_stack, st->loop_stack,
			   sizeof(ForLoopState) * st->loop_depth);
	st->loop_depth = 0;
	frame->try_depth = st->try_depth;
	if (st->try_depth > 0)
		memcpy(frame->try_stack, st->try_stack,
			   sizeof(TryState) * st->try_depth);
	st->try_depth = 0;
	frame->call_begin = *now;

	st->use_file = target_script;
	st->command = 0;
	st->next_command = -1;

	return CSTATE_START_COMMAND;
}

ConnectionStateEnum
handle_cmd_record_recall(CState *st, Command *cmd, pg_time_usec_t *now)
{
	double		recall_val = 0.0;
	PgBenchValue v_rec;

	if (cmd->col_target != NULL && strcmp(cmd->col_target, "VARS") == 0)
	{
		const char *v1_name = (cmd->argv[1][0] == ':') ? (cmd->argv[1] + 1) : cmd->argv[1];
		const char *v2_name = (cmd->argv[2][0] == ':') ? (cmd->argv[2] + 1) : cmd->argv[2];
		Variable   *var1 = lookupVariable(&st->variables, v1_name);
		Variable   *var2 = lookupVariable(&st->variables, v2_name);
		int64		k_val = 0;

		if (var1 == NULL)
		{
			pg_log_error("\\record_recall: undefined variable \"%s\"", v1_name);
			commandFailed(st, "record_recall", "undefined retrieved variable");
			return CSTATE_ABORTED;
		}
		if (var2 == NULL)
		{
			pg_log_error("\\record_recall: undefined variable \"%s\"", v2_name);
			commandFailed(st, "record_recall", "undefined ground truth variable");
			return CSTATE_ABORTED;
		}
		if (var1->value.type == PGBT_NO_VALUE && !makeVariableValue(var1))
		{
			commandFailed(st, "record_recall", "invalid retrieved variable value");
			return CSTATE_ABORTED;
		}
		if (var2->value.type == PGBT_NO_VALUE && !makeVariableValue(var2))
		{
			commandFailed(st, "record_recall", "invalid ground truth variable value");
			return CSTATE_ABORTED;
		}

		if (cmd->expr != NULL)
		{
			PgBenchValue kv;

			if (! evaluateExpr(st, cmd->expr, &kv) || !coerceToInt(&kv, &k_val))
			{
				commandFailed(st, "record_recall", "evaluation of k parameter failed");
				return CSTATE_ABORTED;
			}
		}

		if (!computeRecallOrNdcgAtK(&var1->value, &var2->value, k_val, false, &recall_val))
		{
			commandFailed(st, "record_recall", "could not compute recall_at_k");
			return CSTATE_ABORTED;
		}
	}
	else
	{
		PgBenchValue val;

		if (!evaluateExpr(st, cmd->expr, &val) || !coerceToDouble(&val, &recall_val))
		{
			commandFailed(st, "record_recall", "evaluation of recall expression failed");
			return CSTATE_ABORTED;
		}
	}

	if (recall_val < 0.0)
		recall_val = 0.0;
	else if (recall_val > 1.0)
		recall_val = 1.0;

	memset(&v_rec, 0, sizeof(v_rec));
	setDoubleValue(&v_rec, recall_val);
	if (!putVariableValue(&st->variables, "record_recall", "last_recall", &v_rec))
	{
		commandFailed(st, "record_recall", "assignment of last_recall failed");
		return CSTATE_ABORTED;
	}

	if (st->txn_recall_count < (int) lengthof(st->txn_recalls))
		st->txn_recalls[st->txn_recall_count++] = recall_val;

	return CSTATE_END_COMMAND;
}

static void parseResultCellValue(const char *valstr, PgBenchValue *elem);

static bool
comparePgBenchValues(const PgBenchValue *v1, const PgBenchValue *v2, double tol)
{
	double		d1,
				d2;

	if (v1->type == PGBT_NULL && v2->type == PGBT_NULL)
		return true;
	if (v1->type == PGBT_NULL || v2->type == PGBT_NULL)
		return false;

	if (v1->type == PGBT_ARRAY || v2->type == PGBT_ARRAY)
	{
		if (v1->type != PGBT_ARRAY || v2->type != PGBT_ARRAY)
			return false;
		if (v1->u.aval.nelems != v2->u.aval.nelems)
			return false;
		for (int i = 0; i < v1->u.aval.nelems; i++)
		{
			if (!comparePgBenchValues(&v1->u.aval.elems[i],
									  &v2->u.aval.elems[i], tol))
				return false;
		}
		return true;
	}

	if (v1->type == PGBT_VECTOR || v2->type == PGBT_VECTOR)
	{
		if (v1->type != PGBT_VECTOR || v2->type != PGBT_VECTOR)
			return false;
		if (v1->u.vval.dim != v2->u.vval.dim)
			return false;
		for (int i = 0; i < v1->u.vval.dim; i++)
		{
			if (fabs((double) v1->u.vval.elems[i] -
					 (double) v2->u.vval.elems[i]) > tol)
				return false;
		}
		return true;
	}

	if (v1->type == PGBT_BYTEA || v2->type == PGBT_BYTEA)
	{
		if (v1->type != PGBT_BYTEA || v2->type != PGBT_BYTEA)
			return false;
		if (v1->u.byval.len != v2->u.byval.len)
			return false;
		return (memcmp(v1->u.byval.data, v2->u.byval.data,
					   (size_t) v1->u.byval.len) == 0);
	}

	if (v1->type == PGBT_BOOLEAN || v2->type == PGBT_BOOLEAN)
	{
		bool		b1,
					b2;

		if (coerceToBool(v1, &b1) && coerceToBool(v2, &b2))
			return (b1 == b2);
	}

	if (v1->type == PGBT_INT && v2->type == PGBT_INT && tol == 0.0)
		return (v1->u.ival == v2->u.ival);

	if ((v1->type == PGBT_INT || v1->type == PGBT_DOUBLE) &&
		(v2->type == PGBT_INT || v2->type == PGBT_DOUBLE))
	{
		d1 = (v1->type == PGBT_INT) ? (double) v1->u.ival : v1->u.dval;
		d2 = (v2->type == PGBT_INT) ? (double) v2->u.ival : v2->u.dval;
		if (isnan(d1) && isnan(d2))
			return true;
		if (isinf(d1) && isinf(d2))
			return ((d1 > 0) == (d2 > 0));
		return (fabs(d1 - d2) <= tol);
	}

	/* Try numeric coercion if one side is PGBT_TEXT and the other is numeric */
	if (coerceToDouble(v1, &d1) && coerceToDouble(v2, &d2))
	{
		if (isnan(d1) && isnan(d2))
			return true;
		if (isinf(d1) && isinf(d2))
			return ((d1 > 0) == (d2 > 0));
		return (fabs(d1 - d2) <= tol);
	}

	/* Fallback to string comparison */
	{
		char	   *s1 = NULL;
		char	   *s2 = NULL;
		bool		eq = false;

		if (coerceToText(v1, &s1) && coerceToText(v2, &s2))
			eq = (strcmp(s1, s2) == 0);
		pg_free(s1);
		pg_free(s2);
		return eq;
	}
}

ConnectionStateEnum
handle_cmd_validate_result(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchValue v_actual;
	PgBenchValue v_expected;
	double		tol = 0.0;
	bool		matched;
	PgBenchValue v_ok;

	memset(&v_actual, 0, sizeof(v_actual));
	memset(&v_expected, 0, sizeof(v_expected));

	if (cmd->expr == NULL || !evaluateExpr(st, cmd->expr, &v_actual))
	{
		commandFailed(st, "validate_result", "evaluation of actual expression failed");
		return CSTATE_ABORTED;
	}

	if (cmd->end_expr == NULL || !evaluateExpr(st, cmd->end_expr, &v_expected))
	{
		freeValue(&v_actual);
		commandFailed(st, "validate_result", "evaluation of expected expression failed");
		return CSTATE_ABORTED;
	}

	if (cmd->step_expr != NULL)
	{
		PgBenchValue tv;

		memset(&tv, 0, sizeof(tv));
		if (!evaluateExpr(st, cmd->step_expr, &tv) || !coerceToDouble(&tv, &tol) || tol < 0.0)
		{
			freeValue(&tv);
			freeValue(&v_actual);
			freeValue(&v_expected);
			commandFailed(st, "validate_result", "invalid tolerance expression");
			return CSTATE_ABORTED;
		}
		freeValue(&tv);
	}

	matched = comparePgBenchValues(&v_actual, &v_expected, tol);

	if (matched)
	{
		st->txn_validation_passed++;
	}
	else
	{
		char	   *act_str = formatValue(&v_actual);
		char	   *exp_str = formatValue(&v_expected);

		st->txn_validation_failed++;
		pg_log_warning("client %d: \\validate_result mismatch (actual=%s, expected=%s, tol=%g)",
					   st->id,
					   act_str ? act_str : "NULL",
					   exp_str ? exp_str : "NULL",
					   tol);
		pg_free(act_str);
		pg_free(exp_str);
	}

	freeValue(&v_actual);
	freeValue(&v_expected);

	memset(&v_ok, 0, sizeof(v_ok));
	setBoolValue(&v_ok, matched);
	if (!putVariableValue(&st->variables, "validate_result", "last_validation_ok", &v_ok))
	{
		commandFailed(st, "validate_result", "assignment of last_validation_ok failed");
		return CSTATE_ABORTED;
	}

	return CSTATE_END_COMMAND;
}

/* Pluggable meta-command dispatch table */
static const CommandDescriptor command_handlers[] = {
	{"set", META_SET, handle_cmd_set},
	{"setshell", META_SETSHELL, handle_cmd_setshell},
	{"shell", META_SHELL, handle_cmd_shell},
	{"sleep", META_SLEEP, handle_cmd_sleep},
	{"keying_time", META_KEYING_TIME, handle_cmd_sleep},
	{"think_time", META_THINK_TIME, handle_cmd_sleep},
	{"if", META_IF, handle_cmd_if},
	{"elif", META_ELIF, handle_cmd_elif},
	{"else", META_ELSE, handle_cmd_else},
	{"endif", META_ENDIF, handle_cmd_endif},
	{"for", META_FOR, handle_cmd_for},
	{"endfor", META_ENDFOR, handle_cmd_endfor},
	{"while", META_WHILE, handle_cmd_while},
	{"endwhile", META_ENDWHILE, handle_cmd_endwhile},
	{"try", META_TRY, handle_cmd_try},
	{"catch", META_CATCH, handle_cmd_catch},
	{"endtry", META_ENDTRY, handle_cmd_endtry},
	{"expect_error", META_EXPECT_ERROR, handle_cmd_expect_error},
	{"gset_meta", META_GSET_META, handle_cmd_gset_meta},
	{"dataset_bind", META_DATASET_BIND, handle_cmd_dataset_bind},
	{"copy_dataset", META_COPY_DATASET, handle_cmd_copy_dataset},
	{"wait_until", META_WAIT_UNTIL, handle_cmd_wait_until},
	{"advance_watermark", META_ADVANCE_WATERMARK, handle_cmd_advance_watermark},
	{"begin_dependent_write", META_BEGIN_DEPENDENT_WRITE, handle_cmd_begin_dependent_write},
	{"end_dependent_write", META_END_DEPENDENT_WRITE, handle_cmd_end_dependent_write},
	{"phase_barrier", META_PHASE_BARRIER, handle_cmd_phase_barrier},
	{"reservoir_push", META_RESERVOIR_PUSH, handle_cmd_reservoir_push},
	{"reservoir_sample", META_RESERVOIR_SAMPLE, handle_cmd_reservoir_sample},
	{"session_connect", META_SESSION_CONNECT, handle_cmd_session_connect},
	{"session_disconnect", META_SESSION_DISCONNECT, handle_cmd_session_disconnect},
	{"call", META_CALL, handle_cmd_call},
	{"record_recall", META_RECORD_RECALL, handle_cmd_record_recall},
	{"validate_result", META_VALIDATE_RESULT, handle_cmd_validate_result},
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
 * Parse a non-NULL PGresult cell string into a typed PgBenchValue
 * (for \gset_col and multi-row \aset <prefix>).
 */
static void
parseResultCellValue(const char *valstr, PgBenchValue *elem)
{
	size_t		vlen = strlen(valstr);
	int64		ival;
	double		dval;

	if (vlen >= 2 && valstr[0] == '{' && valstr[vlen - 1] == '}' &&
		parseArrayLiteral(valstr, elem))
		return;

	if (vlen >= 3 && valstr[0] == '[' && valstr[vlen - 1] == ']' &&
		parseVectorLiteral(valstr, elem))
		return;

	if (vlen >= 2 && valstr[0] == '\\' &&
		(valstr[1] == 'x' || valstr[1] == 'X') &&
		parseByteaLiteral(valstr, elem))
		return;

	if (pg_strcasecmp(valstr, "true") == 0 || pg_strcasecmp(valstr, "t") == 0)
	{
		setBoolValue(elem, true);
		return;
	}
	if (pg_strcasecmp(valstr, "false") == 0 || pg_strcasecmp(valstr, "f") == 0)
	{
		setBoolValue(elem, false);
		return;
	}
	if (is_an_int(valstr) && strtoint64(valstr, true, &ival))
	{
		setIntValue(elem, ival);
		return;
	}
	if (strtodouble(valstr, true, &dval))
	{
		setDoubleValue(elem, dval);
		return;
	}

	setTextValue(elem, valstr);
}

/*
 * Store a non-NULL column value from \gset, \aset, or \gset_meta into a
 * variable, converting PostgreSQL array literals "{...}", pgvector literals
 * "[...]", and hex bytea literals "\x..." directly into typed values and
 * non-numeric/non-boolean text values into PGBT_TEXT.
 */
static bool
storeGSetColumnValue(Variables *variables, const char *context,
					 const char *varname, const char *valstr)
{
	size_t		vlen = strlen(valstr);
	PgBenchValue val;

	if ((vlen >= 2 && valstr[0] == '{' && valstr[vlen - 1] == '}' &&
		 parseArrayLiteral(valstr, &val)) ||
		(vlen >= 3 && valstr[0] == '[' && valstr[vlen - 1] == ']' &&
		 parseVectorLiteral(valstr, &val)) ||
		(vlen >= 2 && valstr[0] == '\\' &&
		 (valstr[1] == 'x' || valstr[1] == 'X') &&
		 parseByteaLiteral(valstr, &val)))
	{
		bool		ok = putVariableValue(variables, context, varname, &val);

		freeValue(&val);
		return ok;
	}

	if (!putVariable(variables, context, varname, valstr))
		return false;

	{
		Variable   *var = lookupVariable(variables, varname);

		if (var != NULL && var->value.type == PGBT_NO_VALUE)
		{
			double		dv;

			if (vlen == 0)
			{
				setTextValue(&var->value, "");
			}
			else if (pg_strcasecmp(valstr, "null") != 0 &&
					 pg_strncasecmp(valstr, "true", vlen) != 0 &&
					 pg_strncasecmp(valstr, "yes", vlen) != 0 &&
					 pg_strcasecmp(valstr, "on") != 0 &&
					 pg_strncasecmp(valstr, "false", vlen) != 0 &&
					 pg_strncasecmp(valstr, "no", vlen) != 0 &&
					 pg_strcasecmp(valstr, "off") != 0 &&
					 pg_strcasecmp(valstr, "of") != 0 &&
					 !is_an_int(valstr) &&
					 !strtodouble(valstr, true, &dv))
			{
				setTextValue(&var->value, valstr);
			}
		}
	}

	return true;
}

/*
 * Process \gset, \aset, and \gset_col variable storage from query result.
 * Returns true if successful, false on error.
 */
bool
processGSetResult(CState *st, Command *command, PGresult *res, bool is_last, int qrynum)
{
	MetaCommand meta = command->meta;
	char	   *varprefix = command->varprefix;
	int			ntuples = PQntuples(res);
	int			nfields = PQnfields(res);
	const char *context = (meta == META_ASET) ? "aset" :
		(meta == META_GSET_COL) ? "gset_col" : "gset";
	int			fld;

	if (meta == META_GSET_COL)
	{
		int			col_idx = 0;
		PgBenchValue *elems = NULL;
		PgBenchValue arr_val;
		bool		ok;

		if (ntuples > PGBENCH_MAX_GSET_COL_ROWS)
		{
			pg_log_error("client %d script %d command %d query %d: \\gset_col result row count %d exceeds maximum %d",
						 st->id, st->use_file, st->command, qrynum,
						 ntuples, PGBENCH_MAX_GSET_COL_ROWS);
			st->estatus = ESTATUS_META_COMMAND_ERROR;
			return false;
		}

		if (nfields <= 0)
		{
			pg_log_error("client %d script %d command %d query %d: \\gset_col query returned no columns",
						 st->id, st->use_file, st->command, qrynum);
			st->estatus = ESTATUS_META_COMMAND_ERROR;
			return false;
		}

		if (command->col_target != NULL)
		{
			int			fnum = PQfnumber(res, command->col_target);

			if (fnum >= 0)
			{
				col_idx = fnum;
			}
			else if (is_an_int(command->col_target))
			{
				int			idx = atoi(command->col_target);

				if (idx < 0 || idx >= nfields)
				{
					pg_log_error("client %d script %d command %d query %d: \\gset_col column index %d out of range (0 .. %d)",
								 st->id, st->use_file, st->command, qrynum,
								 idx, nfields - 1);
					st->estatus = ESTATUS_META_COMMAND_ERROR;
					return false;
				}
				col_idx = idx;
			}
			else
			{
				pg_log_error("client %d script %d command %d query %d: \\gset_col unknown column \"%s\"",
							 st->id, st->use_file, st->command, qrynum,
							 command->col_target);
				st->estatus = ESTATUS_META_COMMAND_ERROR;
				return false;
			}
		}

		if (ntuples > 0)
		{
			elems = pg_malloc_array(PgBenchValue, ntuples);
			for (int r = 0; r < ntuples; r++)
			{
				if (PQgetisnull(res, r, col_idx))
					setNullValue(&elems[r]);
				else
					parseResultCellValue(PQgetvalue(res, r, col_idx), &elems[r]);
			}
		}

		arr_val.type = PGBT_ARRAY;
		arr_val.u.aval.nelems = ntuples;
		arr_val.u.aval.elems = elems;

		ok = putVariableValue(&st->variables, context, varprefix, &arr_val);
		freeValue(&arr_val);
		if (!ok)
		{
			pg_log_error("client %d script %d command %d query %d: error storing into variable %s",
						 st->id, st->use_file, st->command, qrynum, varprefix);
			st->estatus = ESTATUS_META_COMMAND_ERROR;
			return false;
		}
		return true;
	}

	if (meta == META_GSET && ntuples != 1)
	{
		pg_log_error("client %d script %d command %d query %d: expected one row, got %d",
					 st->id, st->use_file, st->command, qrynum, ntuples);
		st->estatus = ESTATUS_META_COMMAND_ERROR;
		return false;
	}
	else if (meta == META_ASET && *varprefix == '\0' && ntuples <= 0)
	{
		/* skip empty result under bare \aset */
		return true;
	}

	/*
	 * When \aset <prefix> is given a non-empty prefix, capture all returned
	 * rows for each column into parallel PGBT_ARRAY variables
	 * (<prefix><col> and <prefix>_<col>).
	 */
	if (meta == META_ASET && *varprefix != '\0')
	{
		if (ntuples > PGBENCH_MAX_GSET_COL_ROWS)
		{
			pg_log_error("client %d script %d command %d query %d: \\aset result row count %d exceeds maximum %d",
						 st->id, st->use_file, st->command, qrynum,
						 ntuples, PGBENCH_MAX_GSET_COL_ROWS);
			st->estatus = ESTATUS_META_COMMAND_ERROR;
			return false;
		}

		for (fld = 0; fld < nfields; fld++)
		{
			char	   *colname = PQfname(res, fld);
			char	   *varname = psprintf("%s%s", varprefix, colname);
			PgBenchValue *elems = NULL;
			PgBenchValue arr_val;
			bool		ok;

			if (ntuples > 0)
			{
				elems = pg_malloc_array(PgBenchValue, ntuples);
				for (int r = 0; r < ntuples; r++)
				{
					if (PQgetisnull(res, r, fld))
						setNullValue(&elems[r]);
					else
						parseResultCellValue(PQgetvalue(res, r, fld), &elems[r]);
				}
			}

			arr_val.type = PGBT_ARRAY;
			arr_val.u.aval.nelems = ntuples;
			arr_val.u.aval.elems = elems;

			ok = putVariableValue(&st->variables, context, varname, &arr_val);
			if (ok && varprefix[strlen(varprefix) - 1] != '_' &&
				valid_variable_name(colname))
			{
				char	   *alt_varname = psprintf("%s_%s", varprefix, colname);

				(void) putVariableValue(&st->variables, context, alt_varname, &arr_val);
				pfree(alt_varname);
			}
			freeValue(&arr_val);

			if (!ok)
			{
				pg_log_error("client %d script %d command %d query %d: error storing into variable %s",
							 st->id, st->use_file, st->command, qrynum, varname);
				st->estatus = ESTATUS_META_COMMAND_ERROR;
				pfree(varname);
				return false;
			}
			pfree(varname);
		}
		return true;
	}

	/* store results into variables */
	for (fld = 0; fld < nfields; fld++)
	{
		char	   *colname = PQfname(res, fld);
		char	   *varname = colname;
		bool		ok;

		if (*varprefix != '\0')
			varname = psprintf("%s%s", varprefix, colname);

		/* store last row result */
		if (PQgetisnull(res, ntuples - 1, fld))
		{
			PgBenchValue nullval;

			setNullValue(&nullval);
			ok = putVariableValue(&st->variables, context,
								  varname, &nullval);
			if (ok && *varprefix != '\0' && varprefix[strlen(varprefix) - 1] != '_' &&
				valid_variable_name(colname))
			{
				char	   *alt_varname = psprintf("%s_%s", varprefix, colname);

				(void) putVariableValue(&st->variables, context, alt_varname, &nullval);
				pfree(alt_varname);
			}
		}
		else
		{
			const char *valstr = PQgetvalue(res, ntuples - 1, fld);

			ok = storeGSetColumnValue(&st->variables, context, varname, valstr);
			if (ok && *varprefix != '\0' && varprefix[strlen(varprefix) - 1] != '_' &&
				valid_variable_name(colname))
			{
				char	   *alt_varname = psprintf("%s_%s", varprefix, colname);

				(void) storeGSetColumnValue(&st->variables, context, alt_varname, valstr);
				pfree(alt_varname);
			}
		}

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

/*
 * Populate :prefix_status, :prefix_sqlstate, and :prefix_nrows for \gset_meta.
 */
bool
populateGSetMeta(CState *st, const char *prefix)
{
	const char *p = (prefix != NULL) ? prefix : "";
	size_t		plen = strlen(p);
	const char *sep = (plen > 0 && p[plen - 1] != '_') ? "_" : "";
	char	   *status_var = psprintf("%s%sstatus", p, sep);
	char	   *sqlstate_var = psprintf("%s%ssqlstate", p, sep);
	char	   *nrows_var = psprintf("%s%snrows", p, sep);
	const char *status_str = (st->last_status != NULL) ? st->last_status : "OK";
	const char *sqlstate_str = (st->last_sqlstate[0] != '\0') ? st->last_sqlstate : "00000";
	PgBenchValue v;
	bool		ok = true;

	setTextValue(&v, status_str);
	if (!putVariableValue(&st->variables, "gset_meta", status_var, &v))
		ok = false;
	freeValue(&v);

	if (ok)
	{
		setTextValue(&v, sqlstate_str);
		if (!putVariableValue(&st->variables, "gset_meta", sqlstate_var, &v))
			ok = false;
		freeValue(&v);
	}

	if (ok)
	{
		if (!putVariableInt(&st->variables, "gset_meta", nrows_var, st->last_nrows))
			ok = false;
	}

	if (!ok)
		st->estatus = ESTATUS_META_COMMAND_ERROR;

	pfree(status_var);
	pfree(sqlstate_var);
	pfree(nrows_var);

	return ok;
}

/*
 * Process \gset_meta when attached to a query returning tuples.
 * If \gset / \aset was not also specified on the same command, any columns
 * with valid identifier names from the last row are also captured into
 * :prefix_<colname> before populating :prefix_status, :prefix_sqlstate, and
 * :prefix_nrows.
 */
bool
processGSetMetaResult(CState *st, Command *command, PGresult *res)
{
	const char *p = (command->meta_varprefix != NULL) ? command->meta_varprefix : "";
	size_t		plen = strlen(p);
	const char *sep = (plen > 0 && p[plen - 1] != '_') ? "_" : "";
	int			ntuples = PQntuples(res);

	if (command->meta == META_NONE && ntuples >= 1)
	{
		for (int fld = 0; fld < PQnfields(res); fld++)
		{
			char	   *colname = PQfname(res, fld);
			char	   *varname;

			if (!valid_variable_name(colname))
				continue;

			varname = psprintf("%s%s%s", p, sep, colname);
			if (!valid_variable_name(varname))
			{
				pfree(varname);
				continue;
			}

			if (PQgetisnull(res, ntuples - 1, fld))
			{
				PgBenchValue nullval;

				setNullValue(&nullval);
				(void) putVariableValue(&st->variables, "gset_meta", varname, &nullval);
			}
			else
			{
				(void) storeGSetColumnValue(&st->variables, "gset_meta", varname,
											PQgetvalue(res, ntuples - 1, fld));
			}
			pfree(varname);
		}
	}

	return populateGSetMeta(st, p);
}
