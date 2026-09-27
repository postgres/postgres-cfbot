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
#
# The comparison is exact except for the two CLOG hint bits in the high byte
# of t_flags (byte 37 of the FLUX tuple header): FLUX_TUPLE_XMIN_COMMITTED
# (0x02) and FLUX_TUPLE_XMAX_COMMITTED (0x04).  The UPDATE's visibility
# check may set them on the page after the committed image was read here and
# before the UPDATE copies its before-image, so the image the worker puts
# back can carry a hint the committed image did not.  A hint bit only caches
# what CLOG says about t_xmin or t_xmax, which both images share; it is not
# WAL-logged and wal_consistency_checking masks it.  Every other byte,
# including the reserved word at bytes 16-19, must match.
my $raw_mk = q{SELECT encode(set_byte(t, 37, get_byte(t, 37) & ~6), 'hex')
  FROM (SELECT substr(get_raw_page('mk', 0), lp_off + 1, lp_len) AS t
          FROM heap_page_items(get_raw_page('mk', 0)) WHERE lp = 1) s};

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

# The worker path applies undo in the background; give it a moment.
sub check
{
	my ($t, $what) = @_;
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) || ',' || count(*) FILTER (WHERE v = 'orig') FROM $t",
			'10,10'),
		$what);
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
	foreach my $am ('flux')
	{
		foreach my $case (sort keys %cases)
		{
			my $t = "${path}_${am}_" . substr($case, 0, 1);
			my $what = "$path, $am, $case";
			$node->poll_query_until('postgres', 'SELECT true');
			$node->safe_psql(
				'postgres', qq(
CREATE TABLE $t (id int primary key, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' FROM generate_series(1, 10) g;
));
			my ($ret, $out, $err) =
			  $node->psql('postgres', $cases{$case}->($t));
			is($ret, 0, "$what: no error") or diag($err);
			check($t, "$what: rows restored");
			push @tables, [ $t, $what ];
		}
	}

	$node->restart;
	check($_->[0], "$_->[1]: rows restored after restart") foreach @tables;

	$node->stop('immediate');
	$node->start;
	check($_->[0], "$_->[1]: rows restored after crash restart")
	  foreach @tables;
}

done_testing();
