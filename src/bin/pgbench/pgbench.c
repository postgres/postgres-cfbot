/*
 * pgbench.c
 *
 * A simple benchmark program for PostgreSQL
 * Originally written by Tatsuo Ishii and enhanced by many contributors.
 *
 * src/bin/pgbench/pgbench.c
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#if defined(WIN32) && FD_SETSIZE < 1024
#error FD_SETSIZE needs to have been increased
#endif

#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>		/* for getrlimit */



#include "catalog/pg_class_d.h"
#include "common/int.h"
#include "common/logging.h"
#include "common/pg_prng.h"
#include "common/string.h"
#include "common/username.h"
#include "fe_utils/cancel.h"
#include "fe_utils/conditional.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "port/pg_bitutils.h"
#include "portability/instr_time.h"

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not */

#include "engine.h"
#include "commands.h"
#include "init.h"
#include "poller.h"
#include "script.h"
#include "stats.h"
#include "variable.h"

/* quiet logging onto stderr */
bool		use_quiet = false;

static void
usage(void)
{
	printf("%s is a benchmarking tool for PostgreSQL.\n\n"
		   "Usage:\n"
		   "  %s [OPTION]... [DBNAME]\n"
		   "\nInitialization options:\n"
		   "  -i, --initialize         invokes initialization mode\n"
		   "  -I, --init-steps=[" ALL_INIT_STEPS "]+ (default \"" DEFAULT_INIT_STEPS "\")\n"
		   "                           run selected initialization steps, in the specified order\n"
		   "                           d: drop any existing pgbench tables\n"
		   "                           t: create the tables used by the standard pgbench scenario\n"
		   "                           g: generate data, client-side\n"
		   "                           G: generate data, server-side\n"
		   "                           v: invoke VACUUM on the standard tables\n"
		   "                           p: create primary key indexes on the standard tables\n"
		   "                           f: create foreign keys between the standard tables\n"
		   "  -F, --fillfactor=NUM     set fill factor\n"
		   "  -n, --no-vacuum          do not run VACUUM during initialization\n"
		   "  -q, --quiet              quiet logging (one message each 5 seconds)\n"
		   "  -s, --scale=NUM          scaling factor\n"
		   "  --foreign-keys           create foreign key constraints between tables\n"
		   "  --index-tablespace=TABLESPACE\n"
		   "                           create indexes in the specified tablespace\n"
		   "  --partition-method=(range|hash)\n"
		   "                           partition pgbench_accounts with this method (default: range)\n"
		   "  --partitions=NUM         partition pgbench_accounts into NUM parts (default: 0)\n"
		   "  --tablespace=TABLESPACE  create tables in the specified tablespace\n"
		   "  --unlogged-tables        create tables as unlogged tables\n"
		   "\nOptions to select what to run:\n"
		   "  -b, --builtin=NAME[@W]   add builtin script NAME weighted at W (default: 1)\n"
		   "                           (use \"-b list\" to list available scripts)\n"
		   "  -f, --file=FILENAME[@W]  add script FILENAME weighted at W (default: 1)\n"
		   "  -N, --skip-some-updates  skip updates of pgbench_tellers and pgbench_branches\n"
		   "                           (same as \"-b simple-update\")\n"
		   "  -S, --select-only        perform SELECT-only transactions\n"
		   "                           (same as \"-b select-only\")\n"
		   "\nBenchmarking options:\n"
		   "  -c, --client=NUM         number of concurrent database clients (default: 1)\n"
		   "  -C, --connect            establish new connection for each transaction\n"
		   "  -D, --define=VARNAME=VALUE\n"
		   "                           define variable for use by custom script\n"
		   "  -j, --jobs=NUM           number of threads (default: 1)\n"
		   "  -l, --log                write transaction times to log file\n"
		   "  -L, --latency-limit=NUM  count transactions lasting more than NUM ms as late\n"
		   "  -M, --protocol=simple|extended|prepared\n"
		   "                           protocol for submitting queries (default: simple)\n"
		   "  -n, --no-vacuum          do not run VACUUM before tests\n"
		   "  -P, --progress=NUM       show thread progress report every NUM seconds\n"
		   "  -r, --report-per-command report latencies, failures, and retries per command\n"
		   "  -R, --rate=NUM           target rate in transactions per second\n"
		   "  -s, --scale=NUM          report this scale factor in output\n"
		   "  -t, --transactions=NUM   number of transactions each client runs (default: 10)\n"
		   "  -T, --time=NUM           duration of benchmark test in seconds\n"
		   "  -v, --vacuum-all         vacuum all four standard tables before tests\n"
		   "  --aggregate-interval=NUM aggregate data over NUM seconds\n"
		   "  --continue-on-error      continue running after an SQL error\n"
		   "  --exit-on-abort          exit when any client is aborted\n"
		   "  --failures-detailed      report the failures grouped by basic types\n"
		   "  --log-prefix=PREFIX      prefix for transaction time log file\n"
		   "                           (default: \"pgbench_log\")\n"
		   "  --max-tries=NUM          max number of tries to run transaction (default: 1)\n"
		   "  --progress-timestamp     use Unix epoch timestamps for progress\n"
		   "  --random-seed=SEED       set random seed (\"time\", \"rand\", integer)\n"
		   "  --sampling-rate=NUM      fraction of transactions to log (e.g., 0.01 for 1%%)\n"
		   "  --show-script=NAME       show builtin script code, then exit\n"
		   "  --verbose-errors         print messages of all errors\n"
		   "\nCommon options:\n"
		   "  --debug                  print debugging output\n"
		   "  -d, --dbname=DBNAME      database name to connect to\n"
		   "  -h, --host=HOSTNAME      database server host or socket directory\n"
		   "  -p, --port=PORT          database server port number\n"
		   "  -U, --username=USERNAME  connect as specified database user\n"
		   "  -V, --version            output version information, then exit\n"
		   "  -?, --help               show this help, then exit\n"
		   "\n"
		   "Report bugs to <%s>.\n"
		   "%s home page: <%s>\n",
		   progname, progname, PACKAGE_BUGREPORT, PACKAGE_NAME, PACKAGE_URL);
}


/*
 * Set up a random seed according to seed parameter (NULL means default),
 * and initialize base_random_sequence for use in initializing other sequences.
 */
static bool
set_random_seed(const char *seed)
{
	uint64		iseed;

	if (seed == NULL || strcmp(seed, "time") == 0)
	{
		/* rely on current time */
		iseed = pg_time_now();
	}
	else if (strcmp(seed, "rand") == 0)
	{
		/* use some "strong" random source */
		if (!pg_strong_random(&iseed, sizeof(iseed)))
		{
			pg_log_error("could not generate random seed");
			return false;
		}
	}
	else
	{
		char		garbage;

		if (sscanf(seed, "%" SCNu64 "%c", &iseed, &garbage) != 1)
		{
			pg_log_error("unrecognized random seed option \"%s\"", seed);
			pg_log_error_detail("Expecting an unsigned integer, \"time\" or \"rand\".");
			return false;
		}
	}

	if (seed != NULL)
		pg_log_info("setting random seed to %" PRIu64, iseed);

	random_seed = iseed;

	/* Initialize base_random_sequence using seed */
	pg_prng_seed(&base_random_sequence, iseed);

	return true;
}

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		/* systematic long/short named options */
		{"builtin", required_argument, NULL, 'b'},
		{"client", required_argument, NULL, 'c'},
		{"connect", no_argument, NULL, 'C'},
		{"dbname", required_argument, NULL, 'd'},
		{"define", required_argument, NULL, 'D'},
		{"file", required_argument, NULL, 'f'},
		{"fillfactor", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"initialize", no_argument, NULL, 'i'},
		{"init-steps", required_argument, NULL, 'I'},
		{"jobs", required_argument, NULL, 'j'},
		{"log", no_argument, NULL, 'l'},
		{"latency-limit", required_argument, NULL, 'L'},
		{"no-vacuum", no_argument, NULL, 'n'},
		{"port", required_argument, NULL, 'p'},
		{"progress", required_argument, NULL, 'P'},
		{"protocol", required_argument, NULL, 'M'},
		{"quiet", no_argument, NULL, 'q'},
		{"report-per-command", no_argument, NULL, 'r'},
		{"rate", required_argument, NULL, 'R'},
		{"scale", required_argument, NULL, 's'},
		{"select-only", no_argument, NULL, 'S'},
		{"skip-some-updates", no_argument, NULL, 'N'},
		{"time", required_argument, NULL, 'T'},
		{"transactions", required_argument, NULL, 't'},
		{"username", required_argument, NULL, 'U'},
		{"vacuum-all", no_argument, NULL, 'v'},
		/* long-named only options */
		{"unlogged-tables", no_argument, NULL, 1},
		{"tablespace", required_argument, NULL, 2},
		{"index-tablespace", required_argument, NULL, 3},
		{"sampling-rate", required_argument, NULL, 4},
		{"aggregate-interval", required_argument, NULL, 5},
		{"progress-timestamp", no_argument, NULL, 6},
		{"log-prefix", required_argument, NULL, 7},
		{"foreign-keys", no_argument, NULL, 8},
		{"random-seed", required_argument, NULL, 9},
		{"show-script", required_argument, NULL, 10},
		{"partitions", required_argument, NULL, 11},
		{"partition-method", required_argument, NULL, 12},
		{"failures-detailed", no_argument, NULL, 13},
		{"max-tries", required_argument, NULL, 14},
		{"verbose-errors", no_argument, NULL, 15},
		{"exit-on-abort", no_argument, NULL, 16},
		{"debug", no_argument, NULL, 17},
		{"continue-on-error", no_argument, NULL, 18},
		{NULL, 0, NULL, 0}
	};

	int			c;
	bool		is_init_mode = false;	/* initialize mode? */
	char	   *initialize_steps = NULL;
	bool		foreign_keys = false;
	bool		is_no_vacuum = false;
	bool		do_vacuum_accounts = false; /* vacuum accounts table? */
	int			optindex;
	bool		scale_given = false;

	bool		benchmarking_option_set = false;
	bool		initialization_option_set = false;
	bool		internal_script_used = false;

	CState	   *state;			/* status of clients */
	TState	   *threads;		/* array of thread */

	pg_time_usec_t
				start_time,		/* start up time */
				bench_start = 0,	/* first recorded benchmarking time */
				conn_total_duration;	/* cumulated connection time in
										 * threads */
	int64		latency_late = 0;
	StatsData	stats;
	int			weight;

	int			i;
	int			nclients_dealt;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
#endif

	PGconn	   *con;
	char	   *env;

	int			exit_code = 0;
	struct timeval tv;

	/* initialize timing infrastructure (required for INSTR_* calls) */
	pg_initialize_timing();

	/*
	 * Record difference between Unix time and instr_time time.  We'll use
	 * this for logging and aggregation.
	 */
	gettimeofday(&tv, NULL);
	epoch_shift = tv.tv_sec * INT64CONST(1000000) + tv.tv_usec - pg_time_now();

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pgbench (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	state = pg_malloc0_object(CState);

	/* set random seed early, because it may be used while parsing scripts. */
	if (!set_random_seed(getenv("PGBENCH_RANDOM_SEED")))
		pg_fatal("error while setting random seed from PGBENCH_RANDOM_SEED environment variable");

	while ((c = getopt_long(argc, argv, "b:c:Cd:D:f:F:h:iI:j:lL:M:nNp:P:qrR:s:St:T:U:v", long_options, &optindex)) != -1)
	{
		char	   *script;

		switch (c)
		{
			case 'b':
				if (strcmp(optarg, "list") == 0)
				{
					listAvailableScripts();
					exit(0);
				}
				weight = parseScriptWeight(optarg, &script);
				process_builtin(findBuiltin(script), weight);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 'c':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-c/--client", 1, INT_MAX,
									  &nclients))
				{
					exit(1);
				}
#ifdef HAVE_GETRLIMIT
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
					pg_fatal("getrlimit failed: %m");

				if (rlim.rlim_max < nclients + 3)
				{
					pg_log_error("need at least %d open files, but system limit is %ld",
								 nclients + 3, (long) rlim.rlim_max);
					pg_log_error_hint("Reduce number of clients, or use limit/ulimit to increase the system limit.");
					exit(1);
				}

				if (rlim.rlim_cur < nclients + 3)
				{
					rlim.rlim_cur = nclients + 3;
					if (setrlimit(RLIMIT_NOFILE, &rlim) == -1)
					{
						pg_log_error("need at least %d open files, but couldn't raise the limit: %m",
									 nclients + 3);
						pg_log_error_hint("Reduce number of clients, or use limit/ulimit to increase the system limit.");
						exit(1);
					}
				}
#endif							/* HAVE_GETRLIMIT */
				break;
			case 'C':
				benchmarking_option_set = true;
				is_connect = true;
				break;
			case 'd':
				dbName = pg_strdup(optarg);
				break;
			case 'D':
				{
					char	   *p;

					benchmarking_option_set = true;

					if ((p = strchr(optarg, '=')) == NULL || p == optarg || *(p + 1) == '\0')
						pg_fatal("invalid variable definition: \"%s\"", optarg);

					*p++ = '\0';
					if (!putVariable(&state[0].variables, "option", optarg, p))
						exit(1);
				}
				break;
			case 'f':
				weight = parseScriptWeight(optarg, &script);
				process_file(script, weight);
				benchmarking_option_set = true;
				break;
			case 'F':
				initialization_option_set = true;
				if (!option_parse_int(optarg, "-F/--fillfactor", 10, 100,
									  &fillfactor))
					exit(1);
				break;
			case 'h':
				pghost = pg_strdup(optarg);
				break;
			case 'i':
				is_init_mode = true;
				break;
			case 'I':
				pg_free(initialize_steps);
				initialize_steps = pg_strdup(optarg);
				checkInitSteps(initialize_steps);
				initialization_option_set = true;
				break;
			case 'j':			/* jobs */
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-j/--jobs", 1, INT_MAX,
									  &nthreads))
				{
					exit(1);
				}
				break;
			case 'l':
				benchmarking_option_set = true;
				use_log = true;
				break;
			case 'L':
				{
					double		limit_ms = atof(optarg);

					if (limit_ms <= 0.0)
						pg_fatal("invalid latency limit: \"%s\"", optarg);
					benchmarking_option_set = true;
					latency_limit = (int64) (limit_ms * 1000);
				}
				break;
			case 'M':
				benchmarking_option_set = true;
				for (querymode = 0; querymode < NUM_QUERYMODE; querymode++)
					if (strcmp(optarg, QUERYMODE[querymode]) == 0)
						break;
				if (querymode >= NUM_QUERYMODE)
					pg_fatal("invalid query mode (-M): \"%s\"", optarg);
				break;
			case 'n':
				is_no_vacuum = true;
				break;
			case 'N':
				process_builtin(findBuiltin("simple-update"), 1);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 'p':
				pgport = pg_strdup(optarg);
				break;
			case 'P':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-P/--progress", 1, INT_MAX,
									  &progress))
					exit(1);
				break;
			case 'q':
				initialization_option_set = true;
				use_quiet = true;
				break;
			case 'r':
				benchmarking_option_set = true;
				report_per_command = true;
				break;
			case 'R':
				{
					/* get a double from the beginning of option value */
					double		throttle_value = atof(optarg);

					benchmarking_option_set = true;

					if (throttle_value <= 0.0)
						pg_fatal("invalid rate limit: \"%s\"", optarg);
					/* Invert rate limit into per-transaction delay in usec */
					throttle_delay = 1000000.0 / throttle_value;
				}
				break;
			case 's':
				scale_given = true;
				if (!option_parse_int(optarg, "-s/--scale", 1, INT_MAX,
									  &scale))
					exit(1);
				break;
			case 'S':
				process_builtin(findBuiltin("select-only"), 1);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 't':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-t/--transactions", 1, INT_MAX,
									  &nxacts))
					exit(1);
				break;
			case 'T':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-T/--time", 1, INT_MAX,
									  &duration))
					exit(1);
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'v':
				benchmarking_option_set = true;
				do_vacuum_accounts = true;
				break;
			case 1:				/* unlogged-tables */
				initialization_option_set = true;
				unlogged_tables = true;
				break;
			case 2:				/* tablespace */
				initialization_option_set = true;
				tablespace = pg_strdup(optarg);
				break;
			case 3:				/* index-tablespace */
				initialization_option_set = true;
				index_tablespace = pg_strdup(optarg);
				break;
			case 4:				/* sampling-rate */
				benchmarking_option_set = true;
				sample_rate = atof(optarg);
				if (sample_rate <= 0.0 || sample_rate > 1.0)
					pg_fatal("invalid sampling rate: \"%s\"", optarg);
				break;
			case 5:				/* aggregate-interval */
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "--aggregate-interval", 1, INT_MAX,
									  &agg_interval))
					exit(1);
				break;
			case 6:				/* progress-timestamp */
				progress_timestamp = true;
				benchmarking_option_set = true;
				break;
			case 7:				/* log-prefix */
				benchmarking_option_set = true;
				logfile_prefix = pg_strdup(optarg);
				break;
			case 8:				/* foreign-keys */
				initialization_option_set = true;
				foreign_keys = true;
				break;
			case 9:				/* random-seed */
				benchmarking_option_set = true;
				if (!set_random_seed(optarg))
					pg_fatal("error while setting random seed from --random-seed option");
				break;
			case 10:			/* list */
				{
					const BuiltinScript *s = findBuiltin(optarg);

					fprintf(stderr, "-- %s: %s\n%s\n", s->name, s->desc, s->script);
					exit(0);
				}
				break;
			case 11:			/* partitions */
				initialization_option_set = true;
				if (!option_parse_int(optarg, "--partitions", 0, INT_MAX,
									  &partitions))
					exit(1);
				break;
			case 12:			/* partition-method */
				initialization_option_set = true;
				if (pg_strcasecmp(optarg, "range") == 0)
					partition_method = PART_RANGE;
				else if (pg_strcasecmp(optarg, "hash") == 0)
					partition_method = PART_HASH;
				else
					pg_fatal("invalid partition method, expecting \"range\" or \"hash\", got: \"%s\"",
							 optarg);
				break;
			case 13:			/* failures-detailed */
				benchmarking_option_set = true;
				failures_detailed = true;
				break;
			case 14:			/* max-tries */
				{
					int32		max_tries_arg = atoi(optarg);

					if (max_tries_arg < 0)
						pg_fatal("invalid number of maximum tries: \"%s\"", optarg);

					benchmarking_option_set = true;
					max_tries = (uint32) max_tries_arg;
				}
				break;
			case 15:			/* verbose-errors */
				benchmarking_option_set = true;
				verbose_errors = true;
				break;
			case 16:			/* exit-on-abort */
				benchmarking_option_set = true;
				exit_on_abort = true;
				break;
			case 17:			/* debug */
				pg_logging_increase_verbosity();
				break;
			case 18:			/* continue-on-error */
				benchmarking_option_set = true;
				continue_on_error = true;
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/* set default script if none */
	if (num_scripts == 0 && !is_init_mode)
	{
		process_builtin(findBuiltin("tpcb-like"), 1);
		benchmarking_option_set = true;
		internal_script_used = true;
	}

	/* complete SQL command initialization and compute total weight */
	for (i = 0; i < num_scripts; i++)
	{
		Command   **commands = sql_script[i].commands;

		for (int j = 0; commands[j] != NULL; j++)
			if (commands[j]->type == SQL_COMMAND)
				postprocess_sql_command(commands[j]);

		/* cannot overflow: weight is 32b, total_weight 64b */
		total_weight += sql_script[i].weight;
	}

	if (total_weight == 0 && !is_init_mode)
		pg_fatal("total script weight must not be zero");

	/* show per script stats if several scripts are used */
	if (num_scripts > 1)
		per_script_stats = true;

	/*
	 * Don't need more threads than there are clients.  (This is not merely an
	 * optimization; throttle_delay is calculated incorrectly below if some
	 * threads have no clients assigned to them.)
	 */
	if (nthreads > nclients)
		nthreads = nclients;

	/*
	 * Convert throttle_delay to a per-thread delay time.  Note that this
	 * might be a fractional number of usec, but that's OK, since it's just
	 * the center of a Poisson distribution of delays.
	 */
	throttle_delay *= nthreads;

	if (dbName == NULL)
	{
		if (argc > optind)
			dbName = argv[optind++];
		else
		{
			if ((env = getenv("PGDATABASE")) != NULL && *env != '\0')
				dbName = env;
			else if ((env = getenv("PGUSER")) != NULL && *env != '\0')
				dbName = env;
			else
				dbName = get_user_name_or_exit(progname);
		}
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (is_init_mode)
	{
		if (benchmarking_option_set)
			pg_fatal("some of the specified options cannot be used in initialization (-i) mode");

		if (partitions == 0 && partition_method != PART_NONE)
			pg_fatal("--partition-method requires greater than zero --partitions");

		/* set default method */
		if (partitions > 0 && partition_method == PART_NONE)
			partition_method = PART_RANGE;

		if (initialize_steps == NULL)
			initialize_steps = pg_strdup(DEFAULT_INIT_STEPS);

		if (is_no_vacuum)
		{
			/* Remove any vacuum step in initialize_steps */
			char	   *p;

			while ((p = strchr(initialize_steps, 'v')) != NULL)
				*p = ' ';
		}

		if (foreign_keys)
		{
			/* Add 'f' to end of initialize_steps, if not already there */
			if (strchr(initialize_steps, 'f') == NULL)
			{
				initialize_steps = (char *)
					pg_realloc(initialize_steps,
							   strlen(initialize_steps) + 2);
				strcat(initialize_steps, "f");
			}
		}

		runInitSteps(initialize_steps);
		exit(0);
	}
	else
	{
		if (initialization_option_set)
			pg_fatal("some of the specified options cannot be used in benchmarking mode");
	}

	if (nxacts > 0 && duration > 0)
		pg_fatal("specify either a number of transactions (-t) or a duration (-T), not both");

	/* Use DEFAULT_NXACTS if neither nxacts nor duration is specified. */
	if (nxacts <= 0 && duration <= 0)
		nxacts = DEFAULT_NXACTS;

	/* --sampling-rate may be used only with -l */
	if (sample_rate > 0.0 && !use_log)
		pg_fatal("log sampling (--sampling-rate) is allowed only when logging transactions (-l)");

	/* --sampling-rate may not be used with --aggregate-interval */
	if (sample_rate > 0.0 && agg_interval > 0)
		pg_fatal("log sampling (--sampling-rate) and aggregation (--aggregate-interval) cannot be used at the same time");

	if (agg_interval > 0 && !use_log)
		pg_fatal("log aggregation is allowed only when actually logging transactions");

	if (!use_log && logfile_prefix)
		pg_fatal("log file prefix (--log-prefix) is allowed only when logging transactions (-l)");

	if (duration > 0 && agg_interval > duration)
		pg_fatal("number of seconds for aggregation (%d) must not be higher than test duration (%d)", agg_interval, duration);

	if (duration > 0 && agg_interval > 0 && duration % agg_interval != 0)
		pg_fatal("duration (%d) must be a multiple of aggregation interval (%d)", duration, agg_interval);

	if (progress_timestamp && progress == 0)
		pg_fatal("--progress-timestamp is allowed only under --progress");

	if (!max_tries)
	{
		if (!latency_limit && duration <= 0)
			pg_fatal("an unlimited number of transaction tries can only be used with --latency-limit or a duration (-T)");
	}

	/*
	 * save main process id in the global variable because process id will be
	 * changed after fork.
	 */
	main_pid = (int) getpid();

	if (nclients > 1)
	{
		state = pg_realloc_array(state, CState, nclients);
		memset(state + 1, 0, sizeof(CState) * (nclients - 1));

		/* copy any -D switch values to all clients */
		for (i = 1; i < nclients; i++)
		{
			state[i].id = i;
			if (!copyVariables(&state[i].variables, &state[0].variables))
				exit(1);
		}
	}

	/* other CState initializations */
	for (i = 0; i < nclients; i++)
	{
		state[i].cstack = conditional_stack_create();
		initRandomState(&state[i].cs_func_rs);
	}

	/* opening connection... */
	con = doConnect();
	if (con == NULL)
		pg_fatal("could not create connection for setup");

	/* report pgbench and server versions */
	printVersion(con);

	pg_log_debug("pghost: %s pgport: %s nclients: %d %s: %d dbName: %s",
				 PQhost(con), PQport(con), nclients,
				 duration <= 0 ? "nxacts" : "duration",
				 duration <= 0 ? nxacts : duration, PQdb(con));

	if (internal_script_used)
		GetTableInfo(con, scale_given);

	/*
	 * :scale variables normally get -s or database scale, but don't override
	 * an explicit -D switch
	 */
	if (lookupVariable(&state[0].variables, "scale") == NULL)
	{
		for (i = 0; i < nclients; i++)
		{
			if (!putVariableInt(&state[i].variables, "startup", "scale", scale))
				exit(1);
		}
	}

	/*
	 * Define a :client_id variable that is unique per connection. But don't
	 * override an explicit -D switch.
	 */
	if (lookupVariable(&state[0].variables, "client_id") == NULL)
	{
		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "client_id", i))
				exit(1);
	}

	/* set default seed for hash functions */
	if (lookupVariable(&state[0].variables, "default_seed") == NULL)
	{
		uint64		seed = pg_prng_uint64(&base_random_sequence);

		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "default_seed",
								(int64) seed))
				exit(1);
	}

	/* set random seed unless overwritten */
	if (lookupVariable(&state[0].variables, "random_seed") == NULL)
	{
		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "random_seed",
								random_seed))
				exit(1);
	}

	if (!is_no_vacuum)
	{
		fprintf(stderr, "starting vacuum...");
		tryExecuteStatement(con, "vacuum pgbench_branches");
		tryExecuteStatement(con, "vacuum pgbench_tellers");
		tryExecuteStatement(con, "truncate pgbench_history");
		fprintf(stderr, "end.\n");

		if (do_vacuum_accounts)
		{
			fprintf(stderr, "starting vacuum pgbench_accounts...");
			tryExecuteStatement(con, "vacuum analyze pgbench_accounts");
			fprintf(stderr, "end.\n");
		}
	}
	PQfinish(con);

	/* set up thread data structures */
	threads = pg_malloc_array(TState, nthreads);
	nclients_dealt = 0;

	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		thread->tid = i;
		thread->state = &state[nclients_dealt];
		thread->nstate =
			(nclients - nclients_dealt + nthreads - i - 1) / (nthreads - i);
		initRandomState(&thread->ts_choose_rs);
		initRandomState(&thread->ts_throttle_rs);
		initRandomState(&thread->ts_sample_rs);
		thread->logfile = NULL; /* filled in later */
		thread->latency_late = 0;
		initStats(&thread->stats, 0);

		nclients_dealt += thread->nstate;
	}

	/* all clients must be assigned to a thread */
	Assert(nclients_dealt == nclients);

	/* get start up time for the whole computation */
	start_time = pg_time_now();

	/* set alarm if duration is specified. */
	if (duration > 0)
		setalarm(duration);

	errno = THREAD_BARRIER_INIT(&barrier, nthreads);
	if (errno != 0)
		pg_fatal("could not initialize barrier: %m");

	/* start all threads but thread 0 which is executed directly later */
	for (i = 1; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		thread->create_time = pg_time_now();
		errno = THREAD_CREATE(&thread->thread, threadRun, thread);

		if (errno != 0)
			pg_fatal("could not create thread: %m");
	}

	/* compute when to stop */
	threads[0].create_time = pg_time_now();
	if (duration > 0)
		end_time = threads[0].create_time + (int64) 1000000 * duration;

	/* run thread 0 directly */
	(void) threadRun(&threads[0]);

	/* wait for other threads and accumulate results */
	initStats(&stats, 0);
	conn_total_duration = 0;

	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		if (i > 0)
			THREAD_JOIN(thread->thread);

		for (int j = 0; j < thread->nstate; j++)
			if (thread->state[j].state != CSTATE_FINISHED)
				exit_code = 2;

		/* aggregate thread level stats */
		mergeStats(&stats, &thread->stats);
		latency_late += thread->latency_late;
		conn_total_duration += thread->conn_duration;

		/* first recorded benchmarking start time */
		if (bench_start == 0 || thread->bench_start < bench_start)
			bench_start = thread->bench_start;
	}

	/*
	 * All connections should be already closed in threadRun(), so this
	 * disconnect_all() will be a no-op, but clean up the connections just to
	 * be sure. We don't need to measure the disconnection delays here.
	 */
	disconnect_all(state, nclients);

	/*
	 * Beware that performance of short benchmarks with many threads and
	 * possibly long transactions can be deceptive because threads do not
	 * start and finish at the exact same time. The total duration computed
	 * here encompasses all transactions so that tps shown is somehow slightly
	 * underestimated.
	 */
	printResults(&stats, pg_time_now() - bench_start, conn_total_duration,
				 bench_start - start_time, latency_late);

	THREAD_BARRIER_DESTROY(&barrier);

	if (exit_code != 0)
		pg_log_error("Run was aborted; the above results are incomplete.");

	return exit_code;
}
