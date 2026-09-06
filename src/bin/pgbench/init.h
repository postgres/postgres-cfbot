/*-------------------------------------------------------------------------
 *
 * init.h
 *		pgbench database initialization, table creation, and data generator
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/init.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_INIT_H
#define PGBENCH_INIT_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

/* Step constants */
#define DEFAULT_INIT_STEPS "dtgvp"	/* default -I setting */
#define ALL_INIT_STEPS "dtgGvpf"	/* all possible steps */

/* Partitioning strategy for "pgbench_accounts" */
typedef enum
{
	PART_NONE,					/* no partitioning */
	PART_RANGE,					/* range partitioning */
	PART_HASH,					/* hash partitioning */
} partition_method_t;

/* Configuration parameters for database initialization */
extern int	scale;
extern int	fillfactor;
extern bool	unlogged_tables;
extern char *tablespace;
extern char *index_tablespace;
extern int	partitions;
extern partition_method_t partition_method;
extern const char *const PARTITION_METHOD[];

/* Callback used to build rows for COPY during data loading */
typedef void (*initRowMethod) (PQExpBufferData *sql, int64 curr);

/* SQL Execution helpers */
extern void executeStatement(PGconn *con, const char *sql);
extern void tryExecuteStatement(PGconn *con, const char *sql);
extern char get_table_relkind(PGconn *con, const char *table);

/* Initialization steps */
extern void initDropTables(PGconn *con);
extern void createPartitions(PGconn *con);
extern void initCreateTables(PGconn *con);
extern void initTruncateTables(PGconn *con);
extern void initGenerateDataClientSide(PGconn *con);
extern void initGenerateDataServerSide(PGconn *con);
extern void initVacuum(PGconn *con);
extern void initCreatePKeys(PGconn *con);
extern void initCreateFKeys(PGconn *con);

/* High-level initialization drivers and validators */
extern void checkInitSteps(const char *initialize_steps);
extern void runInitSteps(const char *initialize_steps);
extern void GetTableInfo(PGconn *con, bool scale_given);

#endif							/* PGBENCH_INIT_H */
