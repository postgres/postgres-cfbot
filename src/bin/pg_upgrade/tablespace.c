/*
 *	tablespace.c
 *
 *	tablespace functions
 *
 *	Copyright (c) 2010-2026, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/tablespace.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

static void get_tablespace_paths(void);
static void get_tablespace_options(void);
static void set_tablespace_directory_suffix(ClusterInfo *cluster);

/* user tablespaces of the old cluster that have options set */
static char **old_spcnames = NULL;
static char **old_spcoptions = NULL;
static int	num_old_spcoptions = 0;
static int	old_spcoptions_encoding;


void
init_tablespaces(void)
{
	get_tablespace_paths();
	get_tablespace_options();

	set_tablespace_directory_suffix(&old_cluster);
	set_tablespace_directory_suffix(&new_cluster);

	if (old_cluster.num_tablespaces > 0 &&
		strcmp(old_cluster.tablespace_suffix, new_cluster.tablespace_suffix) == 0)
	{
		for (int i = 0; i < old_cluster.num_tablespaces; i++)
		{
			/*
			 * In-place tablespaces are okay for same-version upgrades because
			 * their paths will differ between clusters.
			 */
			if (strcmp(old_cluster.tablespaces[i], new_cluster.tablespaces[i]) == 0)
				pg_fatal("Cannot upgrade to/from the same system catalog version when\n"
						 "using tablespaces.");
		}
	}
}


/*
 * get_tablespace_paths()
 *
 * Scans pg_tablespace and returns a malloc'ed array of all tablespace
 * paths. It's the caller's responsibility to free the array.
 */
static void
get_tablespace_paths(void)
{
	PGconn	   *conn = connectToServer(&old_cluster, "template1");
	PGresult   *res;
	int			tblnum;
	int			i_spclocation;
	char		query[QUERY_ALLOC];

	snprintf(query, sizeof(query),
			 "SELECT pg_catalog.pg_tablespace_location(oid) AS spclocation "
			 "FROM	pg_catalog.pg_tablespace "
			 "WHERE	spcname != 'pg_default' AND "
			 "		spcname != 'pg_global'");

	res = executeQueryOrDie(conn, "%s", query);

	old_cluster.num_tablespaces = PQntuples(res);
	new_cluster.num_tablespaces = PQntuples(res);

	if (PQntuples(res) != 0)
	{
		old_cluster.tablespaces =
			pg_malloc_array(char *, old_cluster.num_tablespaces);
		new_cluster.tablespaces =
			pg_malloc_array(char *, new_cluster.num_tablespaces);
	}
	else
	{
		old_cluster.tablespaces = NULL;
		new_cluster.tablespaces = NULL;
	}

	i_spclocation = PQfnumber(res, "spclocation");

	for (tblnum = 0; tblnum < old_cluster.num_tablespaces; tblnum++)
	{
		struct stat statBuf;
		char	   *spcloc = PQgetvalue(res, tblnum, i_spclocation);

		/*
		 * For now, we do not expect non-in-place tablespaces to move during
		 * upgrade.  If that changes, it will likely become necessary to run
		 * the above query on the new cluster, too.
		 *
		 * pg_tablespace_location() returns absolute paths for non-in-place
		 * tablespaces and relative paths for in-place ones, so we use
		 * is_absolute_path() to distinguish between them.
		 */
		if (is_absolute_path(PQgetvalue(res, tblnum, i_spclocation)))
		{
			old_cluster.tablespaces[tblnum] = pg_strdup(spcloc);
			new_cluster.tablespaces[tblnum] = old_cluster.tablespaces[tblnum];
		}
		else
		{
			old_cluster.tablespaces[tblnum] = psprintf("%s/%s", old_cluster.pgdata, spcloc);
			new_cluster.tablespaces[tblnum] = psprintf("%s/%s", new_cluster.pgdata, spcloc);
		}

		/*
		 * Check that the tablespace path exists and is a directory.
		 * Effectively, this is checking only for tables/indexes in
		 * non-existent tablespace directories.  Databases located in
		 * non-existent tablespaces already throw a backend error.
		 * Non-existent tablespace directories can occur when a data directory
		 * that contains user tablespaces is moved as part of pg_upgrade
		 * preparation and the symbolic links are not updated.
		 */
		if (stat(old_cluster.tablespaces[tblnum], &statBuf) != 0)
		{
			if (errno == ENOENT)
				report_status(PG_FATAL,
							  "tablespace directory \"%s\" does not exist",
							  old_cluster.tablespaces[tblnum]);
			else
				report_status(PG_FATAL,
							  "could not stat tablespace directory \"%s\": %m",
							  old_cluster.tablespaces[tblnum]);
		}
		if (!S_ISDIR(statBuf.st_mode))
			report_status(PG_FATAL,
						  "tablespace path \"%s\" is not a directory",
						  old_cluster.tablespaces[tblnum]);
	}

	PQclear(res);

	PQfinish(conn);
}


/*
 * get_tablespace_options()
 *
 * Remember the options of the old cluster's user tablespaces, so that
 * check_tablespace_options() can try them on the new cluster.  Only
 * tablespaces pg_dumpall dumps are of interest.
 */
static void
get_tablespace_options(void)
{
	PGconn	   *conn = connectToServer(&old_cluster, "template1");
	PGresult   *res;

	res = executeQueryOrDie(conn,
							"SELECT spcname, spcoptions "
							"FROM pg_catalog.pg_tablespace "
							"WHERE spcname !~ '^pg_' AND "
							"      spcoptions IS NOT NULL");

	old_spcoptions_encoding = PQclientEncoding(conn);
	num_old_spcoptions = PQntuples(res);
	if (num_old_spcoptions > 0)
	{
		old_spcnames = pg_malloc_array(char *, num_old_spcoptions);
		old_spcoptions = pg_malloc_array(char *, num_old_spcoptions);
	}

	for (int i = 0; i < num_old_spcoptions; i++)
	{
		old_spcnames[i] = pg_strdup(PQgetvalue(res, i, 0));
		old_spcoptions[i] = pg_strdup(PQgetvalue(res, i, 1));
	}

	PQclear(res);
	PQfinish(conn);
}

/*
 * check_tablespace_options()
 *
 * Verify that the new cluster accepts the options of the old cluster's
 * tablespaces.  Options registered by an extension are only known when the
 * extension is in the new cluster's shared_preload_libraries, otherwise
 * restoring the tablespaces would fail.
 */
void
check_tablespace_options(void)
{
	PGconn	   *conn;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for tablespace options");

	if (num_old_spcoptions == 0)
	{
		check_ok();
		return;
	}

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir, "tablespace_options.txt");

	conn = connectToServer(&new_cluster, "template1");

	/* send the options in the encoding we fetched them in */
	if (PQsetClientEncoding(conn,
							pg_encoding_to_char(old_spcoptions_encoding)) != 0)
		pg_fatal("could not set client encoding: %s", PQerrorMessage(conn));

	for (int i = 0; i < num_old_spcoptions; i++)
	{
		const char *params[1];
		PGresult   *res;

		params[0] = old_spcoptions[i];
		res = PQexecParams(conn,
						   "SELECT pg_catalog.pg_validate_tablespace_options($1::pg_catalog.text[])",
						   1, NULL, params, NULL, NULL, 0);

		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %m", output_path);
			fprintf(script, "tablespace \"%s\": %s",
					old_spcnames[i], PQerrorMessage(conn));
		}

		PQclear(res);
	}

	PQfinish(conn);

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains tablespaces with options the new cluster does\n"
				 "not accept.  Options defined by an extension require that extension in\n"
				 "\"shared_preload_libraries\" of the new cluster.  A list of problem\n"
				 "tablespaces is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

static void
set_tablespace_directory_suffix(ClusterInfo *cluster)
{
	/* This cluster has a version-specific subdirectory */

	/* The leading slash is needed to start a new directory. */
	cluster->tablespace_suffix = psprintf("/PG_%s_%d",
										  cluster->major_version_str,
										  cluster->controldata.cat_ver);
}
