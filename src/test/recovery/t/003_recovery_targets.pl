
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test for recovery targets: name, timestamp, XID
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Create and test a standby from given backup, with a certain recovery target.
# Choose $until_lsn later than the transaction commit that causes the row
# count to reach $num_rows, yet not later than the recovery target.
# If options is given, pass it through Cluster->start(options => ...).  This is
# used to exercise scenarios that require the postmaster command line to receive
# multiple "-c name=value" instances of the same GUC, which postgresql.conf
# cannot express because ProcessConfigFile collapses duplicate keys.
sub test_recovery_standby
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $test_name = shift;
	my $node_name = shift;
	my $node_primary = shift;
	my $recovery_params = shift;
	my $num_rows = shift;
	my $until_lsn = shift;
	my %params = @_;

	my $node_standby = PostgreSQL::Test::Cluster->new($node_name);
	$node_standby->init_from_backup($node_primary, 'my_backup',
		has_restoring => 1);

	foreach my $param_item (@$recovery_params)
	{
		$node_standby->append_conf('postgresql.conf', qq($param_item));
	}

	$node_standby->start(
		defined $params{options} ? (options => $params{options}) : ());

	# Wait until standby has replayed enough data
	my $caughtup_query =
	  "SELECT '$until_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
	$node_standby->poll_query_until('postgres', $caughtup_query)
	  or die "Timed out while waiting for standby to catch up";

	# Create some content on primary and check its presence in standby
	my $result =
	  $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int");
	is($result, qq($num_rows), "check standby content for $test_name");

	$params{checks}->($node_standby) if defined $params{checks};

	# Stop standby node
	$node_standby->teardown_node;

	return;
}

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(has_archiving => 1, allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
allow_in_place_tablespaces = on
));

# Bump the transaction ID epoch.  This is useful to stress the portability
# of recovery_target_xid parsing.
system_or_bail('pg_resetwal', '--epoch' => '1', $node_primary->data_dir);

# Start it
$node_primary->start;

# Create data before taking the backup, aimed at testing
# recovery_target = 'immediate'
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1000) AS a");
$node_primary->safe_psql('postgres',
	"CREATE TABLESPACE ts_dropped LOCATION ''");
$node_primary->safe_psql('postgres',
	"CREATE TABLESPACE ts_target LOCATION ''");
$node_primary->safe_psql('postgres', "CREATE DATABASE db_dropped");
$node_primary->safe_psql('db_dropped',
	"CREATE TABLE tab_dropped AS SELECT generate_series(1,100) AS a");
$node_primary->safe_psql('postgres', "CREATE DATABASE db_moved");
my $lsn1 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

# Take backup from which all operations will be run
$node_primary->backup('my_backup');

# Insert some data with used as a replay reference, with a recovery
# target TXID.
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(1001,2000))");
my $ret = $node_primary->safe_psql('postgres',
	"SELECT pg_current_wal_lsn(), pg_current_xact_id();");
my ($lsn2, $recovery_txid) = split /\|/, $ret;

# More data, with recovery target timestamp
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(2001,3000))");
my $lsn3 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
my $recovery_time = $node_primary->safe_psql('postgres', "SELECT now()");

# Even more data, this time with a recovery target name
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(3001,4000))");
my $recovery_name = "my_target";
my $lsn4 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
$node_primary->safe_psql('postgres',
	"SELECT pg_create_restore_point('$recovery_name');");

# And now for a recovery target LSN
$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(4001,5000))");
my $lsn5 = my $recovery_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");

$node_primary->safe_psql('postgres',
	"INSERT INTO tab_int VALUES (generate_series(5001,6000))");

my $lsn6 =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn()");

$ret =
  $node_primary->safe_psql('postgres', "SELECT now(), pg_current_wal_lsn()");
my ($time_before_dbdrop, $lsn_before_dbdrop) = split /\|/, $ret;
$node_primary->safe_psql('postgres', "DROP DATABASE db_dropped");

$ret =
  $node_primary->safe_psql('postgres', "SELECT now(), pg_current_wal_lsn()");
my ($time_before_dbcreate, $lsn_before_dbcreate) = split /\|/, $ret;
$node_primary->safe_psql('postgres', "CREATE DATABASE db_created");
my $oid_db_created = $node_primary->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'db_created'");

$ret =
  $node_primary->safe_psql('postgres', "SELECT now(), pg_current_wal_lsn()");
my ($time_before_tscreate, $lsn_before_tscreate) = split /\|/, $ret;
$node_primary->safe_psql('postgres',
	"CREATE TABLESPACE ts_created LOCATION ''");
my $oid_ts_created = $node_primary->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'ts_created'");

my $oid_ts_target = $node_primary->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'ts_target'");
$ret =
  $node_primary->safe_psql('postgres', "SELECT now(), pg_current_wal_lsn()");
my ($time_before_dbmove, $lsn_before_dbmove) = split /\|/, $ret;
$node_primary->safe_psql('postgres',
	"ALTER DATABASE db_moved SET TABLESPACE ts_target");

my $oid_ts_dropped = $node_primary->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'ts_dropped'");
$ret =
  $node_primary->safe_psql('postgres', "SELECT now(), pg_current_wal_lsn()");
my ($time_before_tsdrop, $lsn_before_tsdrop) = split /\|/, $ret;
$node_primary->safe_psql('postgres', "DROP TABLESPACE ts_dropped");

# Force archiving of WAL file containing $lsn6
$node_primary->safe_psql('postgres', "SELECT pg_switch_wal()");

# Test recovery targets
my @recovery_params = ("recovery_target = 'immediate'");
test_recovery_standby('immediate target',
	'standby_1', $node_primary, \@recovery_params, "1000", $lsn1);
@recovery_params = ("recovery_target_xid = '$recovery_txid'");
test_recovery_standby('XID', 'standby_2', $node_primary, \@recovery_params,
	"2000", $lsn2);
@recovery_params = ("recovery_target_time = '$recovery_time'");
test_recovery_standby('time', 'standby_3', $node_primary, \@recovery_params,
	"3000", $lsn3);
@recovery_params = ("recovery_target_name = '$recovery_name'");
test_recovery_standby('name', 'standby_4', $node_primary, \@recovery_params,
	"4000", $lsn4);
@recovery_params = ("recovery_target_lsn = '$recovery_lsn'");
test_recovery_standby('LSN', 'standby_5', $node_primary, \@recovery_params,
	"5000", $lsn5);

@recovery_params = ("recovery_target_time = '$time_before_dbdrop'");
test_recovery_standby(
	'time before DROP DATABASE',
	'standby_dbdrop',
	$node_primary,
	\@recovery_params,
	"6000",
	$lsn_before_dbdrop,
	checks => sub {
		my $node = shift;
		my ($ret, $stdout, $stderr) =
		  $node->psql('db_dropped', "SELECT count(*) FROM tab_dropped");
		is($ret, 0, 'can connect to db_dropped');
		is($stdout, '100', 'content of db_dropped is intact');
	});

@recovery_params = ("recovery_target_time = '$time_before_dbcreate'");
test_recovery_standby(
	'time before CREATE DATABASE',
	'standby_dbcreate',
	$node_primary,
	\@recovery_params,
	"6000",
	$lsn_before_dbcreate,
	checks => sub {
		my $node = shift;
		ok( !-d $node->data_dir . "/base/$oid_db_created",
			'no orphaned directory of db_created');
	});

@recovery_params = ("recovery_target_time = '$time_before_tscreate'");
test_recovery_standby(
	'time before CREATE TABLESPACE',
	'standby_tscreate',
	$node_primary,
	\@recovery_params,
	"6000",
	$lsn_before_tscreate,
	checks => sub {
		my $node = shift;
		ok(!-e $node->data_dir . "/pg_tblspc/$oid_ts_created",
			'no orphaned directory of ts_created');
	});

@recovery_params = ("recovery_target_time = '$time_before_dbmove'");
test_recovery_standby(
	'time before ALTER DATABASE SET TABLESPACE',
	'standby_dbmove',
	$node_primary,
	\@recovery_params,
	"6000",
	$lsn_before_dbmove,
	checks => sub {
		my $node = shift;
		is( $node->safe_psql(
				'postgres',
				"SELECT count(*) FROM pg_tablespace_databases($oid_ts_target)"
			),
			'0',
			'no orphaned copy of db_moved in ts_target');
	});

@recovery_params = ("recovery_target_time = '$time_before_tsdrop'");
test_recovery_standby(
	'time before DROP TABLESPACE',
	'standby_tsdrop',
	$node_primary,
	\@recovery_params,
	"6000",
	$lsn_before_tsdrop,
	checks => sub {
		my $node = shift;
		ok(-e $node->data_dir . "/pg_tblspc/$oid_ts_dropped",
			'directory of ts_dropped exists');
	});

# Regression: empty-string for one recovery_target_* GUC must not clobber
# another non-empty target.  Setting recovery_target_xid + recovery_target_time
# = '' must recover to the xid, not run as no-target recovery.
@recovery_params =
  ("recovery_target_xid = '$recovery_txid'", "recovery_target_time = ''");
test_recovery_standby('xid with empty time GUC',
	'standby_xid_empty_time', $node_primary, \@recovery_params,
	"2000", $lsn2);

# Multiple targets
#
# Multiple conflicting non-empty settings are rejected, but setting the same
# parameter twice or clearing one with an empty string is allowed.

@recovery_params = (
	"recovery_target_name = '$recovery_name'",
	"recovery_target_name = ''",
	"recovery_target_time = '$recovery_time'");
test_recovery_standby('multiple overriding settings',
	'standby_6', $node_primary, \@recovery_params, "3000", $lsn3);

# Check behavior when recovery ends before target is reached

my $node_standby = PostgreSQL::Test::Cluster->new('standby_8');
$node_standby->init_from_backup(
	$node_primary, 'my_backup',
	has_restoring => 1,
	standby => 0);
$node_standby->append_conf('postgresql.conf',
	"recovery_target_name = 'does_not_exist'");

run_log(
	[
		'pg_ctl',
		'--pgdata' => $node_standby->data_dir,
		'--log' => $node_standby->logfile,
		'start',
	]);

# wait for postgres to terminate
foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	last if !-f $node_standby->data_dir . '/postmaster.pid';
	usleep(100_000);
}
my $logfile = slurp_file($node_standby->logfile());
like(
	$logfile,
	qr/FATAL: .* recovery ended before configured recovery target was reached/,
	'recovery end before target reached is a fatal error');

# Conflicts are rejected at every startup, even without recovery.signal.
# init_from_backup without has_restoring creates no recovery.signal, so this
# cluster would otherwise start as a plain primary; the conflict must still be
# caught.
my $node_no_signal = PostgreSQL::Test::Cluster->new('multi_target_no_signal');
$node_no_signal->init_from_backup($node_primary, 'my_backup');
$node_no_signal->append_conf(
	'postgresql.conf', "recovery_target_name = '$recovery_name'
recovery_target_time = '$recovery_time'");

ok( !$node_no_signal->start(fail_ok => 1),
	'server fails to start with conflicting recovery targets and no recovery.signal'
);

my $logfile_no_signal = slurp_file($node_no_signal->logfile());
like(
	$logfile_no_signal,
	qr/cannot specify more than one recovery target/,
	'expected error message logged without recovery.signal');
like(
	$logfile_no_signal,
	qr/Parameters set are: "recovery_target_name", "recovery_target_time"/,
	'errdetail lists the set parameters in order without recovery.signal');
unlike(
	$logfile_no_signal,
	qr/Parameters set are:[^\n]*=/,
	'errdetail does not echo parameter values without recovery.signal');

my $node_immediate_conflict =
  PostgreSQL::Test::Cluster->new('immediate_target_conflict');
$node_immediate_conflict->init_from_backup($node_primary, 'my_backup');
$node_immediate_conflict->append_conf(
	'postgresql.conf',
	"recovery_target = 'immediate'
recovery_target_xid = '$recovery_txid'");

ok( !$node_immediate_conflict->start(fail_ok => 1),
	'server fails to start with recovery_target=immediate and a second target'
);
like(
	slurp_file($node_immediate_conflict->logfile()),
	qr/cannot specify more than one recovery target/,
	'recovery_target=immediate conflicting with another target is rejected');

# Same-GUC set-then-clear: setting a recovery_target_* GUC and then setting the
# same GUC to an empty string leaves no target, so recovery runs to the end of
# WAL.  Duplicate keys collapse in postgresql.conf, so "pg_ctl --options" passes
# both assignments on the postmaster command line.
test_recovery_standby(
	'recovery_target_xid set then cleared',
	'standby_xid_set_clear',
	$node_primary,
	[],
	"6000",
	$lsn6,
	options => "-c recovery_target_xid=$recovery_txid -c recovery_target_xid="
);

# Set recovery_target_xid, then set and clear recovery_target_name.  Only the
# xid remains, so recovery must stop at it rather than running to the end of WAL
# (a competing target that is set then cleared must not strand the first one).
test_recovery_standby(
	'recovery target preserved when a competing one is set then cleared',
	'standby_clobber_clear',
	$node_primary,
	[],
	"2000",
	$lsn2,
	options =>
	  "-c recovery_target_xid=$recovery_txid -c recovery_target_name=$recovery_name -c recovery_target_name="
);

# Invalid recovery_target_timeline tests
my ($result, $stdout, $stderr) = $node_primary->psql('postgres',
	"ALTER SYSTEM SET recovery_target_timeline TO 'bogus'");
like(
	$stderr,
	qr/is not a valid number/,
	"invalid recovery_target_timeline (bogus value)");

($result, $stdout, $stderr) = $node_primary->psql('postgres',
	"ALTER SYSTEM SET recovery_target_timeline TO '0'");
like(
	$stderr,
	qr/must be between 1 and 4294967295/,
	"invalid recovery_target_timeline (lower bound check)");

($result, $stdout, $stderr) = $node_primary->psql('postgres',
	"ALTER SYSTEM SET recovery_target_timeline TO '4294967296'");
like(
	$stderr,
	qr/must be between 1 and 4294967295/,
	"invalid recovery_target_timeline (upper bound check)");

# Invalid recovery_target_xid tests
($result, $stdout, $stderr) = $node_primary->psql('postgres',
	"ALTER SYSTEM SET recovery_target_xid TO 'bogus'");
like(
	$stderr,
	qr/is not a valid number/,
	"invalid recovery_target_xid (bogus value)");

($result, $stdout, $stderr) = $node_primary->psql('postgres',
	"ALTER SYSTEM SET recovery_target_xid TO '-1'");
like(
	$stderr,
	qr/is not a valid number/,
	"invalid recovery_target_xid (negative)");

($result, $stdout, $stderr) = $node_primary->psql('postgres',
	"ALTER SYSTEM SET recovery_target_xid TO '0'");
like(
	$stderr,
	qr/without epoch must be greater than or equal to 3/,
	"invalid recovery_target_xid (lower bound check)");

done_testing();
