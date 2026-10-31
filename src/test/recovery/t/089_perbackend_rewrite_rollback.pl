# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Per-backend UNDO of a table that the same transaction then rewrote into a
# new relfilenode (TRUNCATE, ALTER TABLE ... TYPE) must be applied to the
# file the undo was written against.  The rollback restores the old
# relfilenode, so the records belong to it; applying them to the new file
# (or counting them as done there) leaves the aborted images in the old file
# and the committed rows are lost.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_rewrite_rollback');
$node->init;
# Applying undo to the wrong file can crash the undo apply worker; let the
# remaining checks run.
$node->append_conf('postgresql.conf',
	"autovacuum = off\nrestart_after_crash = on\n");
$node->start;
if (!$node->check_extension('pageinspect'))
{
	plan skip_all => 'Extension pageinspect not installed';
}

# The tables live in rr, so that the undo apply worker, which connects to the
# database it reverts, can be kept out of it (ALLOW_CONNECTIONS false) while
# a rollback handed to it is checked to be still pending.
$node->safe_psql('postgres', 'CREATE DATABASE rr');
$node->safe_psql(
	'rr', q{
CREATE EXTENSION pageinspect;
CREATE TABLE mk (v int) USING flux;
INSERT INTO mk VALUES (0);
VACUUM (FREEZE) mk;
});

# Freeze the marker before capturing its bytes: DML before-images can freeze
# globally visible xmin, so an unfrozen baseline is not byte-stable.
#
# A deferred transaction updates the marker row mk first, so that its undo
# record is the last of the request to be applied: while mk's raw bytes
# differ from the committed image the rollback is pending, and once they are
# back the worker has applied all of it.
#
# The comparison is exact except for the two CLOG hint bits in the high byte
# of t_flags (byte 33 of the FLUX tuple header): FLUX_TUPLE_XMIN_COMMITTED
# (0x02) and FLUX_TUPLE_XMAX_COMMITTED (0x04).  The UPDATE's visibility
# check may set them on the page after the committed image was read here and
# before the UPDATE copies its before-image, so the image the worker puts
# back can carry a hint the committed image did not.  A hint bit only caches
# what CLOG says about t_xmin or t_xmax, which both images share; it is not
# WAL-logged and wal_consistency_checking masks it.  Every other byte,
# must match.
my $raw_mk = q{SELECT encode(set_byte(t, 33, get_byte(t, 33) & ~6), 'hex')
  FROM (SELECT substr(get_raw_page('mk', 0), lp_off + 1, lp_len) AS t
          FROM heap_page_items(get_raw_page('mk', 0)) WHERE lp = 1) s};
my $orig_mk = $node->safe_psql('rr', $raw_mk);

my %cases = (
	'a_update_truncate_insert' => sub {
		my ($t) = @_;
		return qq(
BEGIN;
UPDATE $t SET v = 'x';
TRUNCATE $t;
INSERT INTO $t VALUES (99, 'new');
ROLLBACK;);
	},
	'b_update_truncate' => sub {
		my ($t) = @_;
		return qq(
BEGIN;
UPDATE $t SET v = 'x';
TRUNCATE $t;
ROLLBACK;);
	},
	'c_savepoint_update_truncate_insert' => sub {
		my ($t) = @_;
		return qq(
BEGIN;
SAVEPOINT s;
UPDATE $t SET v = 'x';
TRUNCATE $t;
INSERT INTO $t VALUES (99, 'new');
ROLLBACK TO s;
COMMIT;);
	},
	'd_update_alter_type' => sub {
		my ($t) = @_;
		return qq(
BEGIN;
UPDATE $t SET v = 'x';
ALTER TABLE $t ALTER COLUMN v TYPE varchar(100);
ROLLBACK;);
	},);

# The cases whose top-level ROLLBACK goes to the worker on the worker path.
my %deferred = (a => 1, b => 1, d => 1);

# The worker path applies undo in the background; wait for it.
sub check
{
	my ($t, $what) = @_;
	my $q =
	  "SELECT count(*) || ',' || count(*) FILTER (WHERE v = 'orig') FROM $t";
	ok($node->poll_query_until('rr', $q, '10,10'), $what)
	  or diag($node->safe_psql('rr', $q));
}

foreach my $path ('inline', 'worker')
{
	if ($path eq 'worker')
	{
		$node->safe_psql('postgres',
			'ALTER SYSTEM SET undo_rollback_overflow_size = 0');
		$node->restart;
	}

	my @tables;
	my $bg = $node->background_psql('rr', on_error_stop => 0);
	foreach my $am ('flux')
	{
		foreach my $case (sort keys %cases)
		{
			my $c = substr($case, 0, 1);
			my $t = "${path}_${am}_$c";
			my $what = "$path, $am, $case";
			my $defer = $path eq 'worker' && $deferred{$c};
			$bg->query_safe(
				qq(
CREATE TABLE $t (id int primary key, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' FROM generate_series(1, 10) g;
));
			my $sql = $cases{$case}->($t);
			if ($defer)
			{
				$sql =~ s/BEGIN;/BEGIN;\nUPDATE mk SET v = v + 1;/;
				$node->safe_psql('postgres',
					'ALTER DATABASE rr ALLOW_CONNECTIONS false');
				ok( $node->poll_query_until(
						'postgres',
						q{SELECT count(*) = 0 FROM pg_stat_activity
						  WHERE backend_type = 'undo apply worker'}),
					"$what: no undo apply worker is connected");
			}
			$bg->{stderr} = '';
			$bg->query($sql);
			is($bg->{stderr}, '', "$what: no error");
			if ($defer)
			{
				isnt($bg->query_safe($raw_mk),
					$orig_mk, "$what: the rollback is left to the worker");
				$node->safe_psql('postgres',
					'ALTER DATABASE rr ALLOW_CONNECTIONS true');
				ok( $node->poll_query_until(
						'rr', "SELECT ($raw_mk) = '$orig_mk'"),
					"$what: the worker applied the rollback");
			}
			check($t, "$what: rows restored");
			push @tables, [ $t, $what ];
		}
	}
	$bg->quit;

	$node->restart;
	check($_->[0], "$_->[1]: rows restored after restart") foreach @tables;

	$node->stop('immediate');
	$node->start;
	check($_->[0], "$_->[1]: rows restored after crash restart")
	  foreach @tables;
}

done_testing();
