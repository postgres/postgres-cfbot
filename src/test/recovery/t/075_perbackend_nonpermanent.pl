# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Per-backend UNDO for UNLOGGED and TEMP tables.  Their undo lives in undo
# logs whose contents are never WAL-logged, so redo must not try to patch
# those logs; temp undo must be applied by its own backend and discarded once
# its transaction has ended.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_nonperm');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# A rollback on an unlogged table marks its undo as applied.  That must not
# leave anything in the WAL that redo cannot replay: the undo pages it would
# touch were never WAL-logged.
foreach my $am ('flux')
{
	$node->safe_psql(
		'postgres', qq(
CREATE UNLOGGED TABLE u_$am (id int, v text) USING $am;
INSERT INTO u_$am SELECT g, 'orig' || g FROM generate_series(1, 100) g;
BEGIN;
UPDATE u_$am SET v = 'upd' WHERE id <= 10;
ROLLBACK;
));
	is( $node->safe_psql(
			'postgres', "SELECT count(*) FROM u_$am WHERE v = 'orig' || id"),
		'100',
		"$am unlogged: rollback restored every row");
}

# pg_stat_get_undo_logs() shows the per-backend undo logs: the rollbacks
# above wrote to an unlogged one.
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_get_undo_logs()
		  WHERE persistence = 'unlogged'"),
	't',
	'pg_stat_get_undo_logs shows the unlogged undo log');

# The numbers of the unlogged and temporary undo logs, as a SQL list.  Their
# pages are the undo pseudo-relations with these relfilenumbers.
sub nonpermanent_lognos
{
	return $node->safe_psql(
		'postgres', q(
SELECT coalesce(string_agg(logno::text, ','), '0') FROM pg_stat_get_undo_logs()
 WHERE persistence <> 'permanent'));
}

# WAL records that discard or extend an unlogged or temporary undo log since
# $from.  Those logs are not WAL-logged: their contents never reach the WAL,
# crash recovery empties them, and a standby replaying their discards and
# extensions would create, recycle and remove segments of logs it never has.
sub nonpermanent_undolog_wal
{
	my ($from) = @_;
	my $lognos = nonpermanent_lognos();
	return $node->safe_psql(
		'postgres', qq(
SELECT count(*) FROM pg_get_wal_records_info('$from', pg_current_wal_lsn())
 WHERE resource_manager = 'UndoLog' AND record_type IN ('DISCARD', 'EXTEND')
   AND split_part(description, ' ', 2)::int IN ($lognos)));
}

# Has the discard worker discarded all of the unlogged undo?
my $unlogged_discarded = q(
SELECT bool_and(discard = insert) FROM pg_stat_get_undo_logs()
 WHERE persistence = 'unlogged');

# Undo pages of unlogged and temp tables are not written at a checkpoint, as
# for any unlogged page; only permanent undo must be.
$node->safe_psql('postgres', 'CREATE EXTENSION pg_buffercache');
$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$node->safe_psql(
	'postgres', q(
CREATE TEMP TABLE bt (id int, v text) USING flux;
INSERT INTO bt SELECT g, 'orig' || g FROM generate_series(1, 100) g;
UPDATE bt SET v = 'upd';
UPDATE u_flux SET v = 'upd2';
UPDATE u_flux SET v = 'orig' || id;
CHECKPOINT;
));
my $lognos = nonpermanent_lognos();
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) > 0 FROM pg_buffercache
		  WHERE reldatabase = 9 AND relfilenode IN ($lognos) AND isdirty"),
	't',
	'checkpoint leaves unlogged and temp undo pages dirty');

# Write committed unlogged undo after that checkpoint too, so the crash
# loses undo the discard worker already knew about, and leave an
# unlogged-table transaction in flight, so the crash finds undo for it whose
# pages never reached disk.
my $lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$node->safe_psql('postgres', "UPDATE u_flux SET v = 'orig' || id");
ok($node->poll_query_until('postgres', $unlogged_discarded),
	'discard worker discards the unlogged undo');
is(nonpermanent_undolog_wal($lsn), '0',
	'unlogged undo discards write no WAL');
my $inflight = $node->background_psql('postgres');
$inflight->query_safe("BEGIN; UPDATE u_flux SET v = 'inflight';");

$node->stop('immediate');
$inflight->{run}->kill_kill;
ok($node->start(fail_ok => 1),
	'server restarts after a crash following unlogged rollbacks');

# The unlogged undo written before the crash, including the in-flight
# transaction's, is garbage now, like the unlogged tables themselves: crash
# recovery must not walk it, and the discard worker must not trip over it.
my $rlog = slurp_file($node->logfile);
unlike(
	$rlog,
	qr/per-backend UNDO recovery: rolling back/,
	'crash recovery does not walk lost unlogged undo');
foreach my $am ('flux')
{
	is($node->safe_psql('postgres', "SELECT count(*) FROM u_$am"),
		'0', "$am unlogged: table reset by crash recovery");
	$node->safe_psql(
		'postgres', qq(
INSERT INTO u_$am SELECT g, 'orig' || g FROM generate_series(1, 100) g;
BEGIN;
UPDATE u_$am SET v = 'upd' WHERE id <= 10;
ROLLBACK;
));
	is( $node->safe_psql(
			'postgres', "SELECT count(*) FROM u_$am WHERE v = 'orig' || id"),
		'100',
		"$am unlogged: rollback after crash recovery restored every row");
}

# The discard worker skips an undo log with nothing to discard, so it meets
# a reset unlogged log only once undo has been written to it again, as just
# now.  Wait until it has discarded that.
ok( $node->poll_query_until('postgres', $unlogged_discarded),
	'discard worker discards unlogged undo written after the reset');
unlike(
	slurp_file($node->logfile),
	qr/cannot move discard pointer backwards|TRAP|PANIC/,
	'discard worker copes with reset unlogged undo logs');

# A rollback handed to an undo apply worker that cannot apply every record
# must be retried until it is, not left marked in progress forever.
SKIP:
{
	skip 'Injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';

	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	# The worker requeueing the request logs "Undo action pushed(error)".
	$node->safe_psql('postgres',
		'ALTER SYSTEM SET undo_rollback_overflow_size = 0');
	$node->safe_psql('postgres',
		'ALTER SYSTEM SET log_min_messages = debug1');
	$node->reload;
	my $off = -s $node->logfile;
	$node->safe_psql(
		'postgres', q(
CREATE TABLE w_flux (id int, v text) USING flux;
INSERT INTO w_flux SELECT g, 'orig' || g FROM generate_series(1, 10) g;
SELECT injection_points_attach('pbu-undo-apply-skip', 'notice');
BEGIN;
UPDATE w_flux SET v = 'upd';
ROLLBACK;
));
	$node->wait_for_log(qr/Undo action pushed\(error\)/, $off);
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('pbu-undo-apply-skip')");
	ok( $node->poll_query_until(
			'postgres',
			"SELECT count(*) = 10 FROM w_flux WHERE v = 'orig' || id"),
		'incomplete worker rollback is retried and applied');

	# Temp undo cannot be applied by a worker, which cannot see the table.
	foreach my $am ('flux')
	{
		is( $node->safe_psql(
				'postgres', qq(
CREATE TEMP TABLE tw_$am (id int, v text) USING $am;
INSERT INTO tw_$am SELECT g, 'orig' || g FROM generate_series(1, 10) g;
BEGIN;
UPDATE tw_$am SET v = 'upd';
ROLLBACK;
SELECT count(*) FROM tw_$am WHERE v = 'orig' || id;
)),
			'10',
			"$am temp: rollback is applied by the backend, not a worker");
	}
	$node->safe_psql('postgres',
		'ALTER SYSTEM RESET undo_rollback_overflow_size');
	$node->safe_psql('postgres', 'ALTER SYSTEM RESET log_min_messages');
	$node->reload;
}

# One transaction that writes tables of several persistence levels has an
# undo chain in each level's undo log; a rollback must apply all of them,
# whether the backend applies them itself or hands them to an undo apply
# worker (undo_rollback_overflow_size = 0; temp undo always stays in the
# backend).  Two chains of one transaction are also two rollback requests with
# the same xid, which the request table must keep apart.
foreach my $overflow (16, 0)
{
	$node->safe_psql('postgres',
		"ALTER SYSTEM SET undo_rollback_overflow_size = $overflow");
	$node->reload;
	foreach my $am ('flux')
	{
		my $out = $node->safe_psql(
			'postgres', qq(
CREATE TABLE mp_$am (id int, v text) USING $am;
CREATE UNLOGGED TABLE mu_$am (id int, v text) USING $am;
CREATE TEMP TABLE mt_$am (id int, v text) USING $am;
INSERT INTO mp_$am SELECT g, 'orig' || g FROM generate_series(1, 10) g;
INSERT INTO mu_$am SELECT g, 'orig' || g FROM generate_series(1, 10) g;
INSERT INTO mt_$am SELECT g, 'orig' || g FROM generate_series(1, 10) g;
BEGIN;
UPDATE mp_$am SET v = 'upd';
UPDATE mu_$am SET v = 'upd';
UPDATE mt_$am SET v = 'upd';
ROLLBACK;
SELECT count(*) FROM mt_$am WHERE v = 'orig' || id;
));
		is($out, '10', "$am mixed, overflow $overflow: temp rows restored");
		ok( $node->poll_query_until(
				'postgres', qq(
SELECT (SELECT count(*) FROM mp_$am WHERE v = 'orig' || id) +
       (SELECT count(*) FROM mu_$am WHERE v = 'orig' || id) = 20)),
			"$am mixed, overflow $overflow: permanent and unlogged rows restored"
		);
		$node->safe_psql('postgres', "DROP TABLE mp_$am, mu_$am");
	}
}
$node->safe_psql('postgres',
	'ALTER SYSTEM RESET undo_rollback_overflow_size');
$node->reload;

# Temp undo is private to its backend and needed only until the transaction
# that wrote it ends, so a session churning a temp table must not accumulate
# undo segments: each transaction's undo is about 0.3 MB, 30 of them would
# need 10 one-megabyte segments if nothing were ever discarded.
sub undo_segments
{
	opendir(my $dh, $node->data_dir . '/base/undo') or return 0;
	my $n = grep { !/^\./ } readdir($dh);
	closedir($dh);
	return $n;
}

# Its discards, at the first temp write of every transaction, and the
# segments added and removed for it, write no WAL.
my $temp_lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
foreach my $am ('flux')
{
	my $before = undo_segments();
	my $out = $node->safe_psql(
		'postgres', qq(
CREATE TEMP TABLE t_$am (id int, v text) USING $am;
INSERT INTO t_$am SELECT g, repeat('x', 200) FROM generate_series(1, 1000) g;
SELECT format('UPDATE t_$am SET v = repeat(%L, 200)', chr(97 + i % 20))
  FROM generate_series(1, 30) i \\gexec
BEGIN;
UPDATE t_$am SET v = 'rolled back' WHERE id <= 10;
ROLLBACK;
SELECT count(*) FROM t_$am WHERE v = repeat(chr(97 + 30 % 20), 200);
));
	is($out, '1000', "$am temp: rollback after churn restored every row");
	cmp_ok(undo_segments() - $before,
		'<=', 3, "$am temp: undo is discarded as transactions end");
}
is(nonpermanent_undolog_wal($temp_lsn),
	'0', 'temp undo discards and segment changes write no WAL');

$node->stop;
done_testing();
