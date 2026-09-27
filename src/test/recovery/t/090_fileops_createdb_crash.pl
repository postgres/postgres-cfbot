# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# A CREATE DATABASE that crashes, or fails, part way through its copy must
# leave no database directory behind, for either strategy.  The directory
# is created by FILEOPS but filled by smgr (WAL_LOG) or copydir (FILE_COPY),
# so its UNDO must remove the whole tree, not just rmdir it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('createdb_crash');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'injection_points'
wal_level = replica
autovacuum = off
});
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE DATABASE tmpl');
$node->safe_psql('tmpl',
	"CREATE TABLE t AS SELECT g, repeat('x', 200) v FROM generate_series(1, 20000) g"
);

my $base = $node->data_dir . '/base';

# Directories under base/ that belong to no database.
sub orphans
{
	my %known = map { $_ => 1 } split /\n/,
	  $node->safe_psql('postgres', 'SELECT oid FROM pg_database');
	opendir(my $dh, $base) or die "opendir $base: $!";
	my @o = grep { /^\d+$/ && !$known{$_} } readdir($dh);
	closedir($dh);
	return join(' ', sort @o);
}

my %point = (
	wal_log => 'createdb-wal-log-after-copy-relation',
	file_copy => 'createdb-file-copy-after-copydir');

foreach my $strategy ('wal_log', 'file_copy')
{
	my $ip = $point{$strategy};
	my %before = map { $_ => 1 } split / /, orphans();

	# Crash while the new directory holds copied files.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$ip', 'wait')");
	my $bg = $node->background_psql('postgres', on_error_stop => 0);
	$bg->query_until(
		qr/start/, qq(\\echo start
CREATE DATABASE c_$strategy TEMPLATE tmpl STRATEGY $strategy;
));
	$node->wait_for_event('client backend', $ip);

	my ($new) = grep { !$before{$_} } split / /, orphans();
	ok(defined $new, "$strategy: the new database directory exists");
	$new //= 'none';
	my @files = glob("$base/$new/*");
	cmp_ok(scalar(@files), '>', 1,
		"$strategy: it holds copied files at the crash");

	my $log_offset = -s $node->logfile;
	$node->stop('immediate');
	$bg->{run}->finish;
	$node->start;

	is(orphans(), '', "$strategy: no database directory left after a crash");
	my $log = slurp_file($node->logfile, $log_offset);
	unlike(
		$log,
		qr/WARNING:  FILEOPS UNDO/,
		"$strategy: crash recovery applied the UNDO cleanly");

	# The same, with the copy failing instead.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$ip', 'error')");
	my ($ret, $out, $err) = $node->psql('postgres',
		"CREATE DATABASE e_$strategy TEMPLATE tmpl STRATEGY $strategy");
	like($err, qr/ERROR/, "$strategy: CREATE DATABASE failed mid-copy");
	$node->safe_psql('postgres', "SELECT injection_points_detach('$ip')");
	is(orphans(), '', "$strategy: no database directory left after an error");

	# A later CREATE DATABASE still works.
	$node->safe_psql('postgres',
		"CREATE DATABASE ok_$strategy TEMPLATE tmpl STRATEGY $strategy");
	is($node->safe_psql("ok_$strategy", 'SELECT count(*) FROM t'),
		'20000', "$strategy: a new CREATE DATABASE succeeds");
}

# No UNDO chain was left for the revert worker to retry forever.
sleep(1);
unlike(
	slurp_file($node->logfile),
	qr/could not rmdir .*Directory not empty/,
	'no failed rmdir of a non-empty directory');

$node->stop;
done_testing();
