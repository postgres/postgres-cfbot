# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A speculative insertion (INSERT ... ON CONFLICT) adds a tuple to a page,
# so it must clear the page's visibility map bits, as every other insert
# does.  A page left all-visible would let an index-only scan count a row
# that no snapshot sees (an uncommitted or aborted insertion), and a page
# left all-frozen is skipped by VACUUM's first pass, which then leaves the
# aborted tuple's index entry behind.  heap is the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_spec_insert_vm');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

foreach my $ext ('pg_visibility', 'pageinspect')
{
	plan skip_all => "Extension $ext not installed"
	  unless $node->check_extension($ext);
	$node->safe_psql('postgres', "CREATE EXTENSION $ext");
}

foreach my $am ('heap', 'flux')
{
	my $vm = qq{SELECT all_visible || ',' || all_frozen
		FROM pg_visibility_map('sp_$am') WHERE blkno = 0};

	$node->safe_psql(
		'postgres', qq{
CREATE TABLE sp_$am (id int PRIMARY KEY, v int) USING $am;
INSERT INTO sp_$am SELECT g, 0 FROM generate_series(1, 10) g;
VACUUM (FREEZE) sp_$am;
});
	is($node->safe_psql('postgres', $vm),
		'true,true', "$am: page is all-visible and all-frozen");

	# An insertion still in progress.
	my $bg = $node->background_psql('postgres');
	$bg->query_safe('BEGIN');
	$bg->query_safe(
		"INSERT INTO sp_$am VALUES (11, 1) ON CONFLICT DO NOTHING");
	my $xid = $bg->query_safe('SELECT pg_current_xact_id()::xid');
	chomp $xid;
	is($node->safe_psql('postgres', $vm),
		'false,false', "$am: in-progress speculative insert clears the VM");
	$bg->query_safe('ROLLBACK');
	$bg->quit;
	is($node->safe_psql('postgres', $vm),
		'false,false', "$am: VM stays clear after the insert aborts");

	# One aggressive VACUUM must remove the aborted tuple and its index
	# entry; skipping the page in the first pass leaves both behind.
	$node->safe_psql('postgres', "VACUUM (FREEZE) sp_$am");
	is( $node->safe_psql(
			'postgres', qq{
SELECT string_agg(lp_flags::text || ':' || n, ',' ORDER BY lp_flags)
FROM (SELECT lp_flags, count(*) n
      FROM heap_page_items(get_raw_page('sp_$am', 0)) GROUP BY lp_flags) s}),
		'1:10',
		"$am: VACUUM removed the aborted tuple");
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM bt_page_items('sp_${am}_pkey', 1)"),
		'10',
		"$am: VACUUM removed its index entry");
	# relfrozenxid may pass the aborted xid only once its tuple is gone:
	# otherwise CLOG for it can be truncated while the tuple is still there.
	is( $node->safe_psql(
			'postgres', qq{
SELECT age(relfrozenxid) >= age('$xid'::xid) OR
       (SELECT count(*) FROM heap_page_items(get_raw_page('sp_$am', 0))
        WHERE lp_flags <> 0) = 10
FROM pg_class WHERE relname = 'sp_$am'}),
		't',
		"$am: relfrozenxid does not pass the xid of a tuple still on the page"
	);
	is($node->safe_psql('postgres', $vm),
		'true,true', "$am: VACUUM sets the bits again");

	# COPY (multi_insert) onto the all-frozen page.
	$node->safe_psql('postgres',
		"COPY sp_$am FROM PROGRAM 'echo 13,1' (FORMAT csv)");
	is($node->safe_psql('postgres', $vm),
		'false,false', "$am: COPY clears the VM");
	$node->safe_psql('postgres', "VACUUM (FREEZE) sp_$am");

	# A committed one.
	$node->safe_psql('postgres',
		"INSERT INTO sp_$am VALUES (12, 1) ON CONFLICT (id) DO UPDATE SET v = 2"
	);
	is($node->safe_psql('postgres', $vm),
		'false,false', "$am: committed speculative insert clears the VM");
	is($node->safe_psql('postgres', "SELECT count(*) FROM sp_$am"),
		'12', "$am: row count");
}

$node->stop;
done_testing();
