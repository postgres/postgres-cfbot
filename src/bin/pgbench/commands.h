/*-------------------------------------------------------------------------
 *
 * commands.h
 *		pgbench meta-commands execution and pluggable handler registry
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/commands.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_COMMANDS_H
#define PGBENCH_COMMANDS_H

#include "libpq-fe.h"
#include "fe_utils/conditional.h"
#include "pgbench.h"
#include "script.h"

/*
 * Pluggable Command Handler function signature.
 *
 * Receives the client state, the command to execute, and a timestamp pointer.
 * Returns the next ConnectionStateEnum to transition the client state machine to.
 */
typedef ConnectionStateEnum (*CommandHandler)(CState *st, Command *cmd, pg_time_usec_t *now);

/*
 * Registration entry for meta-commands.
 */
typedef struct CommandDescriptor
{
	const char	   *name;		/* e.g. "set", "sleep", "shell", "if", ... */
	MetaCommand		meta;		/* MetaCommand enum value */
	CommandHandler	handler;	/* Function executing this command */
} CommandDescriptor;

/* Public Command Execution APIs */
extern ConnectionStateEnum executeMetaCommand(CState *st, pg_time_usec_t *now);
extern void skipConditionalCommands(CState *st);
extern bool processGSetResult(CState *st, Command *command, PGresult *res,
							  bool is_last, int qrynum);

/* Helper APIs for commands */
extern void commandFailed(CState *st, const char *cmd, const char *message);
extern void commandError(CState *st, const char *message);
extern bool evaluateSleep(Variables *variables, int argc, char **argv, int *usecs);
extern bool runShellCommand(Variables *variables, char *variable, char **argv, int argc);

/* Handler declarations */
extern ConnectionStateEnum handle_cmd_sleep(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_set(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_if(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_elif(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_else(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_endif(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_setshell(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_shell(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_startpipeline(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_syncpipeline(CState *st, Command *cmd, pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_endpipeline(CState *st, Command *cmd, pg_time_usec_t *now);

/* Preparation helpers used across commands and SQL preparation */
extern void allocCStatePrepared(CState *st);
extern void prepareCommand(CState *st, int command_num);
extern void prepareCommandsInPipeline(CState *st);

#endif							/* PGBENCH_COMMANDS_H */
