# Copyright (c) 2026, PostgreSQL Global Development Group

# Raising an autovacuum freeze weight must never lower the corresponding
# freeze score, for both the transaction ID and multixact components.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# Keep the *_freeze_max_age parameters well above the faked ages so the table
# is not force-vacuumed for wraparound, and drop the failsafe ages to their
# minimum so *_freeze_max_age alone governs the scaling threshold.
$node->append_conf(
	'postgresql.conf', qq[
autovacuum = off
autovacuum_freeze_max_age = 1000000000
autovacuum_multixact_freeze_max_age = 1000000000
vacuum_failsafe_age = 0
vacuum_multixact_failsafe_age = 0
]);
$node->start;

$node->safe_psql('postgres',
	'CREATE TABLE freezetest (i int) WITH (autovacuum_enabled = off)');

# Fake old relfrozenxid/relminmxid rather than consuming hundreds of millions
# of transactions.  The score view only reads these fields arithmetically
# (recentXid - relfrozenxid, recentMulti - relminmxid, both modulo 2^32) and
# never consults clog for them, and with autovacuum off and the faked ages
# below *_freeze_max_age nothing else acts on them, so this is safe.  There is
# no SQL primitive for the next multixact, but on a freshly initialized node it
# is near the next xid, so subtracting from pg_snapshot_xmax() yields a usable
# multixact age too; the assertions below confirm both ages landed in range.
my $age = 300_000_000;
$node->safe_psql(
	'postgres', qq[
UPDATE pg_class
   SET relfrozenxid = ((pg_snapshot_xmax(pg_current_snapshot())::text::bigint
                        - $age + 4294967296) % 4294967296)::text::xid,
       relminmxid = ((pg_snapshot_xmax(pg_current_snapshot())::text::bigint
                      - $age + 4294967296) % 4294967296)::text::xid
 WHERE relname = 'freezetest']);

my ($xid_age, $mxid_age) = split /\|/, $node->safe_psql('postgres',
	q[SELECT age(relfrozenxid) || '|' || mxid_age(relminmxid)
	    FROM pg_class WHERE relname = 'freezetest']);
cmp_ok($xid_age, '>', 100_000_000, 'xid age reaches the scaled regime');
cmp_ok($mxid_age, '>', 100_000_000, 'mxid age reaches the scaled regime');

# Weights near 3.5 are where the unguarded code collapsed the score.
my (%prev);
foreach my $weight (1.0, 2.0, 3.0, 3.5, 5.0, 10.0)
{
	foreach my $kind (qw(freeze multixact_freeze))
	{
		$node->safe_psql('postgres',
			"ALTER SYSTEM SET autovacuum_${kind}_score_weight = $weight");
	}
	$node->reload;
	$node->poll_query_until('postgres',
		"SELECT current_setting('autovacuum_freeze_score_weight')::float8 = $weight"
	) or die "timed out waiting for weight $weight";

	my ($xid, $mxid) = split /\|/, $node->safe_psql('postgres',
		q[SELECT xid_score || '|' || mxid_score
		    FROM pg_stat_autovacuum_scores WHERE relname = 'freezetest']);

	foreach my $c (['xid', $xid], ['mxid', $mxid])
	{
		my ($name, $score) = @$c;
		cmp_ok($score, '>', 0, "$name score positive at weight $weight");
		cmp_ok($score, '>=', $prev{$name},
			"$name score does not decrease at weight $weight")
		  if defined $prev{$name};
		$prev{$name} = $score;
	}
}

$node->stop;
done_testing();
