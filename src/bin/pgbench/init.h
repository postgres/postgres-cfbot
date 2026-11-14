/*-------------------------------------------------------------------------
 *
 * init.h
 *	  Database initialization and schema setup for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/init.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INIT_H
#define INIT_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#define ERRCODE_UNDEFINED_TABLE  "42P01"

#define DEFAULT_INIT_STEPS "dtgvp"	/* default -I setting */
#define ALL_INIT_STEPS "dtgGvpf"	/* all possible steps */

#define LOG_STEP_SECONDS	5	/* seconds between log messages */

/*
 * The scale factor at/beyond which 32bit integers are incapable of storing
 * 64bit values.
 *
 * Although the actual threshold is 21474, we use 20000 because it is easier to
 * document and remember, and isn't that far away from the real threshold.
 */
#define SCALE_32BIT_THRESHOLD 20000

/* partitioning strategy for "pgbench_accounts" */
typedef enum
{
	PART_NONE,					/* no partitioning */
	PART_RANGE,					/* range partitioning */
	PART_HASH,					/* hash partitioning */
} partition_method_t;

/* callback used to build rows for COPY during data loading */
typedef void (*initRowMethod) (PQExpBufferData *sql, int64 curr);

extern int	scale;
extern int	fillfactor;
extern bool unlogged_tables;
extern char *tablespace;
extern char *index_tablespace;
extern int	partitions;
extern partition_method_t partition_method;
extern const char *const PARTITION_METHOD[];

extern void tryExecuteStatement(PGconn *con, const char *sql);
extern void checkInitSteps(const char *initialize_steps);
extern void runInitSteps(const char *initialize_steps);
extern void GetTableInfo(PGconn *con, bool scale_given);

#endif							/* INIT_H */
