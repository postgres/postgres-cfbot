
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test indexallkeysmatch verification: each index tuple must point to a heap
# tuple with the same key.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum=off');
$node->start;

$node->safe_psql('postgres', q(
	CREATE EXTENSION amcheck;
	CREATE EXTENSION pageinspect;
));

#
# Test 1: indexallkeysmatch on uncorrupted table (plain column)
#
$node->safe_psql('postgres', q(
	CREATE TABLE idxall_plain (id int);
	INSERT INTO idxall_plain SELECT generate_series(1, 1000);
	CREATE INDEX idxall_plain_idx ON idxall_plain (id);
));

my $result = $node->safe_psql('postgres',
	q(SELECT bt_index_check('idxall_plain_idx', false, false, true)));
is($result, '', 'indexallkeysmatch passes on uncorrupted plain table');

#
# Test 2: indexallkeysmatch on uncorrupted table with deduplication
#
$node->safe_psql('postgres', q(
	CREATE TABLE idxall_dedup (id int);
	INSERT INTO idxall_dedup SELECT i % 10 FROM generate_series(1, 1000) i;
	CREATE INDEX idxall_dedup_idx ON idxall_dedup (id)
		WITH (deduplicate_items = on);
));

$result = $node->safe_psql('postgres',
	q(SELECT bt_index_check('idxall_dedup_idx', false, false, true)));
is($result, '', 'indexallkeysmatch passes on uncorrupted table with deduplication');

#
# Test 3: indexallkeysmatch on uncorrupted expression index
#
$node->safe_psql('postgres', q(
	CREATE FUNCTION idxall_func(int) RETURNS int LANGUAGE sql IMMUTABLE AS
	$$ SELECT $1; $$;

	CREATE TABLE idxall_expr (id int);
	INSERT INTO idxall_expr SELECT generate_series(1, 500);
	CREATE INDEX idxall_expr_idx ON idxall_expr (idxall_func(id));
));

$result = $node->safe_psql('postgres',
	q(SELECT bt_index_check('idxall_expr_idx', false, false, true)));
is($result, '', 'indexallkeysmatch passes on uncorrupted expression index');

#
# Test 4: Detect corruption -- swap expression function so heap re-evaluation
# produces different keys than what the index stores.
#
# The index was built with idxall_func(x) = x.  Now change it to return x+1.
# This simulates the corruption scenario: index says key=42 for a TID, but
# re-reading the heap and re-evaluating gives key=43.
#
$node->safe_psql('postgres', q(
	CREATE OR REPLACE FUNCTION idxall_func(int) RETURNS int LANGUAGE sql IMMUTABLE AS
	$$ SELECT $1 + 1; $$;
));

my ($stdout, $stderr);
($result, $stdout, $stderr) = $node->psql('postgres',
	q(SELECT bt_index_check('idxall_expr_idx', false, false, true)));
like($stderr,
	qr/index tuple in index "idxall_expr_idx" does not match heap tuple/,
	'detected index-heap key mismatch via expression function swap');

#
# Test 5: Restore function and verify no corruption reported
#
$node->safe_psql('postgres', q(
	CREATE OR REPLACE FUNCTION idxall_func(int) RETURNS int LANGUAGE sql IMMUTABLE AS
	$$ SELECT $1; $$;
));

$result = $node->safe_psql('postgres',
	q(SELECT bt_index_check('idxall_expr_idx', false, false, true)));
is($result, '', 'indexallkeysmatch passes after restoring correct function');

#
# Test 6: indexallkeysmatch with bt_index_parent_check
#
$node->safe_psql('postgres', q(
	CREATE OR REPLACE FUNCTION idxall_func(int) RETURNS int LANGUAGE sql IMMUTABLE AS
	$$ SELECT $1 + 1; $$;
));

($result, $stdout, $stderr) = $node->psql('postgres',
	q(SELECT bt_index_parent_check('idxall_expr_idx', false, false, false, true)));
like($stderr,
	qr/index tuple in index "idxall_expr_idx" does not match heap tuple/,
	'bt_index_parent_check also detects index-heap key mismatch');

# A HOT update leaves the index pointing to the root, not the visible tuple.
# Hold an old snapshot to prevent pruning, then release it and VACUUM to
# exercise LP_REDIRECT as well.  Changing the expression forces Bloom misses:
# a successful check of an uncorrupted index alone need not fetch any tuples.
$node->safe_psql('postgres', q(
	CREATE FUNCTION idxall_hot_func(int) RETURNS int LANGUAGE sql IMMUTABLE AS
	$$ SELECT $1; $$;
	CREATE TABLE idxall_hot (id int, payload int) WITH (fillfactor = 10);
	INSERT INTO idxall_hot SELECT i, 0 FROM generate_series(1, 10) i;
	CREATE INDEX idxall_hot_idx ON idxall_hot (idxall_hot_func(id));
));
my $holder = $node->background_psql('postgres');
$holder->query_safe(q(
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	SELECT count(*) FROM idxall_hot;
));
$node->safe_psql('postgres', 'UPDATE idxall_hot SET payload = 1');
is($node->safe_psql('postgres', q(
	SELECT count(*) FROM heap_page_items(get_raw_page('idxall_hot', 0))
	WHERE lp_flags = 1 AND (t_infomask2 & 16384) <> 0;
)), '10', 'HOT roots remain normal line pointers before pruning');
check_hot_chain('unpruned HOT chain');

$holder->query_safe('COMMIT');
$holder->quit;
$node->safe_psql('postgres', 'VACUUM idxall_hot');
is($node->safe_psql('postgres', q(
	SELECT count(*) FROM heap_page_items(get_raw_page('idxall_hot', 0))
	WHERE lp_flags = 2;
)), '10', 'VACUUM converted HOT roots to LP_REDIRECT');
check_hot_chain('LP_REDIRECT');

$node->stop;
done_testing();

sub check_hot_chain
{
	my ($description) = @_;

	for my $function ('bt_index_check', 'bt_index_parent_check')
	{
		my $extra = $function eq 'bt_index_check' ? 'false' : 'false, false';
		is($node->safe_psql('postgres',
			qq(SELECT $function('idxall_hot_idx', true, $extra, true))),
			'', "$function accepts $description");
	}

	$node->safe_psql('postgres', q(
		CREATE OR REPLACE FUNCTION idxall_hot_func(int) RETURNS int
		LANGUAGE sql IMMUTABLE AS $$ SELECT $1 + 1; $$;
	));
	for my $function ('bt_index_check', 'bt_index_parent_check')
	{
		my $extra = $function eq 'bt_index_check' ? 'false' : 'false, false';
		my ($ret, $out, $err) = $node->psql('postgres',
			qq(SELECT $function('idxall_hot_idx', false, $extra, true)));
		is($ret, 3, "$function rejects mismatched key behind $description");
		like($err,
			qr/index tuple in index "idxall_hot_idx" does not match heap tuple/,
			"$function follows $description to compare keys");
	}
	$node->safe_psql('postgres', q(
		CREATE OR REPLACE FUNCTION idxall_hot_func(int) RETURNS int
		LANGUAGE sql IMMUTABLE AS $$ SELECT $1; $$;
	));
}
