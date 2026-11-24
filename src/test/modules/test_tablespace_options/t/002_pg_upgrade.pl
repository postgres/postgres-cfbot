# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_upgrade checks that the new cluster accepts the old cluster's
# tablespace options

use strict;
use warnings FATAL => 'all';

use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $preload = "shared_preload_libraries = 'test_tablespace_options'\n";

# Use a different encoding in the old cluster, to check that the options
# are passed on unchanged.
my $old = PostgreSQL::Test::Cluster->new('old');
$old->init(extra => [ '--encoding' => 'LATIN1', '--locale' => 'C' ]);
$old->append_conf('postgresql.conf',
	"allow_in_place_tablespaces = on\n" . $preload);
$old->start;
$old->safe_psql('postgres',
	"SET client_encoding = 'LATIN1';
	 CREATE TABLESPACE regress_ts LOCATION '' WITH (test_tablespace_options.level = 7, test_tablespace_options.label = 'caf\xe9', seq_page_cost = 3)"
);

# Text in shared catalogs has no defined encoding, so the non-ASCII value
# comes out of the upgrade in the encoding the restore session used.  Only
# check that it is there.
my $query = "SELECT string_agg(option_name || '=' ||
				CASE WHEN option_name = 'test_tablespace_options.label'
					 THEN '?' ELSE option_value END, ',' ORDER BY option_name)
			 FROM pg_tablespace, pg_options_to_table(spcoptions)
			 WHERE spcname = 'regress_ts'";
my $before = $old->safe_psql('postgres', $query);
$old->stop;

my $new = PostgreSQL::Test::Cluster->new('new');
$new->init(extra => [ '--encoding' => 'UTF8', '--locale' => 'C' ]);
$new->append_conf('postgresql.conf', "allow_in_place_tablespaces = on\n");

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

my @pg_upgrade = (
	'pg_upgrade', '--no-sync',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $old->config_data('--bindir'),
	'--new-bindir' => $new->config_data('--bindir'),
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port,
	'--copy');

# Without the extension, the new cluster does not accept the options
command_checks_all(
	[ @pg_upgrade, '--check' ],
	1,
	[qr/Checking for tablespace options\s+fatal/],
	[],
	'pg_upgrade --check fails without the extension in the new cluster');

my @reports = glob($new->data_dir . '/pg_upgrade_output.d/*/tablespace_options.txt');
is(scalar(@reports), 1, 'tablespace_options.txt is written');
like(
	slurp_file($reports[0]),
	qr/tablespace "regress_ts": ERROR:  unrecognized parameter "test_tablespace_options\.\w+"/,
	'tablespace_options.txt names the tablespace');
rmtree($new->data_dir . '/pg_upgrade_output.d');

# With the extension, the check passes and the options are carried over
$new->append_conf('postgresql.conf', $preload);
command_ok([ @pg_upgrade, '--check' ],
	'pg_upgrade --check passes with the extension in the new cluster');
command_ok(\@pg_upgrade, 'pg_upgrade');

$new->start;
is($new->safe_psql('postgres', $query),
	$before, 'tablespace options are preserved');
$new->stop;

done_testing();
