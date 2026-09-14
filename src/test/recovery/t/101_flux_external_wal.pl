
# Copyright (c) 2026, PostgreSQL Global Development Group

# Rows with out-of-line (EXTERNAL) columns survive WAL replay on FLUX:
# INSERT, a re-storing UPDATE, an indexed-column UPDATE, DELETE and VACUUM of
# ~12 kB EXTERNAL rows.  Replay runs on a streaming standby and in crash
# recovery, both with wal_consistency_checking = all.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', q{
wal_consistency_checking = all
autovacuum = off
});
$primary->start;
$primary->backup('bkp');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

my @ams = ('flux');

# About 12.8 kB of incompressible text, stored out of line.
sub big
{
	my ($seed) = @_;
	return "(SELECT string_agg(md5($seed || x::text), '') "
	  . "FROM generate_series(1, 400) x)";
}
my $ins_big = big('g::text');
my $upd_big = big("'u' || id::text");

foreach my $am (@ams)
{
	my $t = "ovf_$am";
	$primary->safe_psql(
		'postgres', qq(
CREATE TABLE $t (id int PRIMARY KEY, b int, big text) USING $am;
ALTER TABLE $t ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE INDEX ${t}_b ON $t (b);
INSERT INTO $t SELECT g, g * 10, $ins_big FROM generate_series(1, 6) g;
UPDATE $t SET big = $upd_big WHERE id IN (2, 4);
UPDATE $t SET b = b + 1 WHERE id IN (1, 3);
DELETE FROM $t WHERE id = 5;
VACUUM $t;
DELETE FROM $t WHERE id = 6;
VACUUM $t;
UPDATE $t SET b = b + 1 WHERE id = 3;
));
}

sub rowsum
{
	my ($node, $am) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) || ' ' || md5(string_agg(id || ':' || b || ':' || big, ',' ORDER BY id)) "
		  . "FROM ovf_$am");
}

my %want = map { $_ => rowsum($primary, $_) } @ams;
like($want{$_}, qr/^4 /, "$_: four rows on the primary") for @ams;

$primary->wait_for_catchup($standby);
is(rowsum($standby, $_), $want{$_}, "$_: standby matches the primary")
  for @ams;

foreach my $round (1 .. 2)
{
	$primary->stop('immediate');
	$primary->start;
	is(rowsum($primary, $_),
		$want{$_}, "$_: unchanged after crash restart $round")
	  for @ams;
}

# The standby replays the same WAL again from its own restart point.
$standby->stop('immediate');
$standby->start;
$primary->wait_for_catchup($standby);
is(rowsum($standby, $_),
	$want{$_}, "$_: standby unchanged after crash restart")
  for @ams;

ok(!$primary->log_contains('PANIC'), 'primary replayed without PANIC');
ok(!$standby->log_contains('PANIC'), 'standby replayed without PANIC');

done_testing();
