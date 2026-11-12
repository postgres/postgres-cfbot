# Copyright (c) 2026, PostgreSQL Global Development Group

# Test PREPARE TRANSACTION (two-phase commit) wrapping UPDATE/DELETE FOR PORTION OF.
#
# A FOR PORTION OF statement truncates the matched row and inserts temporal
# leftovers.  When it runs inside a prepared transaction, the truncation and the
# leftover inserts must survive PREPARE TRANSACTION and a server restart, then
# take effect atomically at COMMIT PREPARED (or vanish at ROLLBACK PREPARED).
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
	max_prepared_transactions = 10
	datestyle = 'ISO, YMD'
));
$node->start;

$node->safe_psql(
	'postgres', qq{
	CREATE TABLE fpo (id int4range, valid_at daterange, a text,
		PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));
	INSERT INTO fpo VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a');
});

# UPDATE FOR PORTION OF inside a prepared transaction.
$node->safe_psql(
	'postgres', qq{
	BEGIN;
	UPDATE fpo FOR PORTION OF valid_at FROM '2004-01-01' TO '2006-01-01'
		SET a = 'b' WHERE id = '[1,2)';
	PREPARE TRANSACTION 'fpo_update';
});

# Nothing is visible yet: still the single original row.
is( $node->safe_psql('postgres', "SELECT id, valid_at, a FROM fpo ORDER BY valid_at"),
	'[1,2)|[2000-01-01,2010-01-01)|a',
	'FOR PORTION OF not visible while only prepared');

# The prepared transaction (and its pending leftovers) survives a restart.
$node->restart;

is( $node->safe_psql('postgres', "SELECT gid FROM pg_prepared_xacts ORDER BY gid"),
	'fpo_update',
	'prepared FOR PORTION OF transaction survives restart');

$node->safe_psql('postgres', "COMMIT PREPARED 'fpo_update'");

# COMMIT PREPARED applies the truncation and both leftovers atomically.
is( $node->safe_psql('postgres', "SELECT id, valid_at, a FROM fpo ORDER BY valid_at"),
	"[1,2)|[2000-01-01,2004-01-01)|a\n"
	  . "[1,2)|[2004-01-01,2006-01-01)|b\n"
	  . "[1,2)|[2006-01-01,2010-01-01)|a",
	'COMMIT PREPARED applies FOR PORTION OF truncation and leftovers');

# DELETE FOR PORTION OF inside a prepared transaction, then ROLLBACK PREPARED.
$node->safe_psql(
	'postgres', qq{
	BEGIN;
	DELETE FROM fpo FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01'
		WHERE id = '[1,2)';
	PREPARE TRANSACTION 'fpo_delete';
});
$node->safe_psql('postgres', "ROLLBACK PREPARED 'fpo_delete'");

# ROLLBACK PREPARED discards the FOR PORTION OF entirely.
is( $node->safe_psql('postgres', "SELECT id, valid_at, a FROM fpo ORDER BY valid_at"),
	"[1,2)|[2000-01-01,2004-01-01)|a\n"
	  . "[1,2)|[2004-01-01,2006-01-01)|b\n"
	  . "[1,2)|[2006-01-01,2010-01-01)|a",
	'ROLLBACK PREPARED discards FOR PORTION OF');

$node->stop;

done_testing();
