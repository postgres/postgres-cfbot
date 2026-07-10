/*-------------------------------------------------------------------------
 *
 * ddl_deparse.h
 *	  Declarations for DDL command deparsing.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/ddl_deparse.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DDL_DEPARSE_H
#define DDL_DEPARSE_H

#include "nodes/pg_list.h"

/* ddl_deparse.c */
extern bool ddl_deparse_command_supported(const Node *parsetree);
extern char *deparse_ddl_command(const Node *original_parsetree, List *cmds);

/* ddl_json.c */
extern char *deparse_ddl_json_to_string(char *json_str);

#endif							/* DDL_DEPARSE_H */
