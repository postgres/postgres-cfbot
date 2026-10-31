
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# pg_visibility's pg_check_visible and pg_check_frozen decode each item of a
# page marked all-visible or all-frozen as a heap tuple header.  On a FLUX
# table they must refuse, as pgstattuple, pgrowlocks, pg_surgery and
# amcheck's verify_heapam do.  They used to read the FLUX items as heap
# tuples: every row of every all-visible page was reported corrupt, and with
# an xid open in another database the backend failed an assertion in
# HeapTupleSatisfiesVacuum (pg_visibility's own 001_concurrent_transaction
# setup).  Heap is the control.  The functions that read only the visibility
# map and the page header keep working on a FLUX table.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE other_database');
my $bsession = $node->background_psql('other_database');
$bsession->query_safe('BEGIN; SELECT txid_current();');

$node->safe_psql('postgres', 'CREATE EXTENSION pg_visibility');
foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE vt_$am USING $am AS SELECT 42 i;
CREATE TABLE vf_$am USING $am AS SELECT g FROM generate_series(1, 2000) g;
VACUUM (disable_page_skipping) vt_$am;
VACUUM (FREEZE, disable_page_skipping) vf_$am;
));
}

# Heap: nothing to report.
foreach my $t ('vt_heap', 'vf_heap')
{
	foreach my $f ('pg_check_visible', 'pg_check_frozen')
	{
		is($node->safe_psql('postgres', "SELECT * FROM $f('$t')"),
			'', "$f on heap table $t reports nothing");
	}
}

# FLUX: a clean error, and the server stays up.
foreach my $t ('vt_flux', 'vf_flux')
{
	foreach my $f ('pg_check_visible', 'pg_check_frozen')
	{
		my ($ret, $out, $err) =
		  $node->psql('postgres', "SELECT * FROM $f('$t')");
		like(
			$err,
			qr/only heap AM is supported/,
			"$f on FLUX table $t is refused");
		is($out, '', "$f on FLUX table $t returns no TIDs");
	}
}
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'server still up');
my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP:|terminated by signal/, 'no backend crashed');

# The visibility map functions read no tuples: they work for FLUX too.
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) = pg_relation_size('vf_flux') / current_setting('block_size')::int
		   FROM pg_visibility('vf_flux')"),
	't',
	'pg_visibility covers every block of a FLUX table');
is( $node->safe_psql(
		'postgres',
		"SELECT all_visible > 0 AND all_frozen > 0 FROM pg_visibility_map_summary('vf_flux')"
	),
	't',
	'pg_visibility_map_summary sees the FLUX table frozen by VACUUM (FREEZE)'
);

$bsession->query_safe('COMMIT');
$bsession->quit;
$node->stop;

done_testing();
