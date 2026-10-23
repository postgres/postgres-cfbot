/*-------------------------------------------------------------------------
 *
 * config.h
 *		JSON / JSONC configuration file parser, serializer (--config and
 *		--dump-config), and multi-stage initialization runner for pgbench.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/config.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_CONFIG_H
#define PGBENCH_CONFIG_H

#include "pgbench.h"
#include "variable.h"

extern char *config_suite_name;
extern char *config_description;
extern bool config_scripts_loaded;
extern int	num_config_init_steps;
extern Variables config_variables;

/* Shared option state from pgbench.c */
extern char *initialize_steps;
extern bool foreign_keys;
extern bool is_no_vacuum;
extern bool do_vacuum_accounts;
extern bool scale_given;
extern bool internal_script_used;
extern int	copy_nthreads;
extern int64 random_seed;
extern bool random_seed_set;

/* Option helpers in pgbench.c reused by config.c */
extern void parseSlaSpec(const char *spec);
extern bool set_random_seed(const char *seed);

/* Config loading, execution, and serialization */
extern void clearConfigScriptsAndGroups(void);
extern void loadConfigFile(const char *config_path,
						   bool is_init_mode,
						   Variables *client0_vars,
						   bool *benchmarking_option_set,
						   bool *initialization_option_set,
						   bool *jobs_option_set);
extern void runConfigInitSteps(Variables *cli_vars);
extern void writeConfigJsonObject(FILE *fp, int base_indent,
								  bool include_init_steps);
extern void dumpConfigFile(const char *dump_path);

#endif							/* PGBENCH_CONFIG_H */
