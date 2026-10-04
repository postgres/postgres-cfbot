/*-------------------------------------------------------------------------
 *
 * commands.h
 *	  Meta-command execution and prepared statement helpers for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/commands.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMANDS_H
#define COMMANDS_H

#include "pgbench.h"

#define SHELL_COMMAND_SIZE	256 /* maximum size allowed for shell command */

extern void commandFailed(CState *st, const char *cmd, const char *message);
extern void commandError(CState *st, const char *message);
extern void prepareCommand(CState *st, int command);
extern ConnectionStateEnum executeMetaCommand(CState *st, pg_time_usec_t *now);

#endif							/* COMMANDS_H */
