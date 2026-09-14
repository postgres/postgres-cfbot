# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX VACUUM reclaims rows of aborted INSERTs.
#
# INSERT writes no UNDO; an aborted inserter's rows are hidden by CLOG
# alone and nothing marks them DELETED.  VACUUM used to count them as live
# and never removed them, so the space was lost for good.  They must be
# reclaimed like heap reclaims them: dead once the inserter is known
# aborted.  Rows of an in-place UPDATE or DELETE whose writer aborted are
# not dead (the row lives on at its before-image) and must stay.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_x2');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('pageinspect'))
{
	plan skip_all => 'Extension pageinspect not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE TABLE a (id int PRIMARY KEY, pad text) USING flux;
INSERT INTO a SELECT g, 'live' FROM generate_series(1, 10) g;
-- aborted top-level insert
BEGIN;
INSERT INTO a SELECT g, repeat('x', 200) FROM generate_series(1001, 2000) g;
ROLLBACK;
-- savepoint-aborted insert in a committed transaction
BEGIN;
INSERT INTO a VALUES (11, 'keep');
SAVEPOINT s;
INSERT INTO a SELECT g, repeat('y', 200) FROM generate_series(2001, 3000) g;
ROLLBACK TO s;
COMMIT;
});

my $normal_items = q{
SELECT count(*)
FROM generate_series(0, pg_relation_size('a') / 8192 - 1) b,
     heap_page_items(get_raw_page('a', b::int))
WHERE lp_flags = 1};

is($node->safe_psql('postgres', $normal_items),
	'2011', 'aborted rows are on the pages before VACUUM');

$node->safe_psql('postgres', 'VACUUM a');

is($node->safe_psql('postgres', $normal_items),
	'11', 'VACUUM removed the aborted INSERTs');
is($node->safe_psql('postgres', 'SELECT count(*) FROM a'),
	'11', 'committed rows are all still there');
is( $node->safe_psql(
		'postgres',
		'SET enable_seqscan = off; SET enable_bitmapscan = off; '
		  . 'SELECT count(*) FROM a WHERE id > 1000'),
	'0',
	'no index entry leads to a removed row');

# The cached insertion target determines where id=11 lands.  With
# RELCACHE_FORCE_RELEASE it can land on block zero, letting VACUUM truncate
# the entire empty tail; otherwise it can pin a middle page.  Re-insertion
# may therefore grow the post-VACUUM file, but should need no more space than
# a fresh copy of the live rows.  Retained free space must be reused.
$node->safe_psql('postgres',
	"INSERT INTO a SELECT g, repeat('z', 200) FROM generate_series(1001, 2000) g"
);
$node->safe_psql(
	'postgres', q{
CREATE TABLE packed (LIKE a) USING flux;
INSERT INTO packed SELECT * FROM a ORDER BY id;
});
cmp_ok(
	$node->safe_psql('postgres', "SELECT pg_relation_size('a')"),
	'<=',
	$node->safe_psql('postgres', "SELECT pg_relation_size('packed')"),
	'reinserted rows need no more space than a fresh copy');
is($node->safe_psql('postgres', 'SELECT count(*) FROM a'),
	'1011', 'all committed and reinserted rows survive');

# Span multiple FSM leaf pages.  VACUUM must propagate the reclaimed space
# to the upper FSM levels so a new backend finds it without extending first.
# Disable truncation to isolate reuse from the empty-tail policy tested below.
$node->safe_psql(
	'postgres', q{
CREATE TABLE fsm_reuse (pad text) USING flux;
ALTER TABLE fsm_reuse ALTER COLUMN pad SET STORAGE PLAIN;
BEGIN;
INSERT INTO fsm_reuse SELECT repeat('f', 6000) FROM generate_series(1, 4500);
ROLLBACK;
VACUUM (TRUNCATE false) fsm_reuse;
});
my $fsm_size =
  $node->safe_psql('postgres', "SELECT pg_relation_size('fsm_reuse')");
$node->safe_psql('postgres',
	"INSERT INTO fsm_reuse SELECT repeat('g', 6000) FROM generate_series(1, 4500)"
);
is($node->safe_psql('postgres', "SELECT pg_relation_size('fsm_reuse')"),
	$fsm_size, 'freed space across FSM leaf pages is reused');

# An aborted UPDATE / DELETE of a committed row must survive VACUUM.
$node->safe_psql(
	'postgres', q{
BEGIN; UPDATE a SET pad = 'aborted' WHERE id = 1; DELETE FROM a WHERE id = 2; ROLLBACK;
VACUUM a;
});
is( $node->safe_psql(
		'postgres',
		'SELECT string_agg(id || pad, \',\' ORDER BY id) FROM a WHERE id <= 2'
	),
	'1live,2live',
	'VACUUM kept rows whose UPDATE / DELETE aborted');

# VACUUM truncates the empty pages it leaves at the end of the table, as heap
# does: all of them, not just the last one.
$node->safe_psql(
	'postgres', q{
CREATE TABLE tr (id int, pad text) USING flux;
INSERT INTO tr VALUES (1, 'first');
BEGIN;
INSERT INTO tr SELECT g, repeat('t', 200) FROM generate_series(2, 20000) g;
ROLLBACK;
});
cmp_ok($node->safe_psql('postgres', "SELECT pg_relation_size('tr') / 8192"),
	'>', 100, 'aborted rows fill many pages');
$node->safe_psql('postgres', 'VACUUM tr');
is($node->safe_psql('postgres', "SELECT pg_relation_size('tr') / 8192"),
	'1', 'VACUUM truncated every trailing empty page');
is($node->safe_psql('postgres', 'SELECT id, pad FROM tr'),
	'1|first', 'the live row survives');
$node->safe_psql('postgres', 'DELETE FROM tr; VACUUM tr');
is($node->safe_psql('postgres', "SELECT pg_relation_size('tr')"),
	'0', 'an empty table is truncated to nothing');

$node->stop;
done_testing();
