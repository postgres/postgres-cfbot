# Copyright (c) 2026, PostgreSQL Global Development Group
#
# DML on an UNLOGGED or TEMP table writes no WAL proportional to the rows it
# touches, for FLUX as for heap: not for the table, not for its indexes or
# TOAST table, not for the per-backend undo that FLUX writes for it, and not
# for rolling that undo back, whether the backend or an undo apply worker does
# it.  After a crash an unlogged FLUX table is empty and a temp one is gone,
# and recovery does not roll back the undo either left behind.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('nonperm_wal');
$node->init(allows_streaming => 'logical');
$node->append_conf(
	'postgresql.conf', q(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');

# Each session first changes a permanent FLUX table, so it holds a permanent
# undo log whose meta precedes the next checkpoint: its first undo insert after
# that checkpoint would WAL-log the meta of whatever undo log it uses.
$node->safe_psql('postgres',
	'CREATE TABLE p (v int) USING flux; INSERT INTO p VALUES (0)');

# Has the discard worker discarded all of the unlogged undo?  It cannot get
# past a rollback that has not been applied yet.
my $unlogged_discarded = q(
SELECT coalesce(bool_and(discard = insert), true) FROM pg_stat_get_undo_logs()
 WHERE persistence = 'unlogged');

# The WAL each statement writes is recorded in the session's temp table lsn;
# writing a temp table writes no WAL itself.
sub measured
{
	my ($op, $sql) = @_;
	return qq(
INSERT INTO lsn VALUES ('$op', pg_current_wal_insert_lsn(), NULL);
$sql;
UPDATE lsn SET e = pg_current_wal_insert_lsn() WHERE op = '$op';
);
}

my %bytes;
foreach my $overflow (16, 0)
{
	# 0 hands every unlogged rollback to an undo apply worker.
	$node->safe_psql('postgres',
		"ALTER SYSTEM SET undo_rollback_overflow_size = $overflow");
	$node->reload;
	foreach my $am ('heap', 'flux')
	{
		foreach my $pers ('unlogged', 'temp')
		{
			foreach my $n (10000, 100000)
			{
				my $case = "$am $pers $n overflow $overflow";
				my $s = $node->background_psql('postgres');
				$s->query_safe('UPDATE p SET v = v + 1');
				$node->safe_psql('postgres', 'CHECKPOINT');
				$s->query_safe(
					qq(
SET client_min_messages = warning;
CREATE $pers TABLE w (id int, v text, t text) USING $am;
CREATE INDEX w_id ON w (id);
CREATE INDEX w_v ON w USING hash (v);
CREATE TEMP TABLE lsn (op text, s pg_lsn, e pg_lsn);
)
					  . measured(
						'insert',
						"INSERT INTO w SELECT g, 'x' || g, CASE WHEN g % 100 = 0
						  THEN repeat(md5(g::text), 300) END
						  FROM generate_series(1, $n) g")
					  . measured('update', "UPDATE w SET v = v || 'u'")
					  . measured(
						'rollback',
						"BEGIN; UPDATE w SET v = v || 'r', t = t || 'r'; ROLLBACK"
					  )
					  . measured(
						'aborted insert',
						"BEGIN; INSERT INTO w SELECT -g, 'n' FROM generate_series(1, $n) g; ROLLBACK"
					  )
					  . measured(
						'savepoint',
						"BEGIN; SAVEPOINT s; UPDATE w SET v = v || 's'; DELETE FROM w WHERE id % 2 = 0;
						 ROLLBACK TO s; COMMIT")
					  . measured('delete', 'DELETE FROM w') . q(
INSERT INTO lsn VALUES ('settle', pg_current_wal_insert_lsn(), NULL);
));

				# Let an undo apply worker finish the rollbacks and the
				# discard worker discard their undo: whatever WAL they write
				# for it is counted too.
				$node->poll_query_until('postgres', $unlogged_discarded)
				  or die "$case: unlogged undo not discarded";
				$s->query_safe(
					"UPDATE lsn SET e = pg_current_wal_insert_lsn() WHERE op = 'settle'"
				);

				# Flush the WAL, for pg_walinspect to read it.  Commits that
				# wrote no WAL are not flushed.
				$node->safe_psql('postgres', 'CHECKPOINT');

				# Every op restored or kept what it should.
				is($s->query_safe("SELECT count(*) FROM w"),
					'0', "$case: rows deleted");

				# Full-page images for hint bits are of catalog pages, never of
				# unlogged ones, and come and go with checkpoints: leave them
				# out.
				foreach my $row (
					split /\n/,
					$s->query_safe(
						q(
SELECT op || '|' || ((e - s) - CASE WHEN e > s THEN
         (SELECT coalesce(sum(record_length + fpi_length), 0)
            FROM pg_get_wal_records_info(s, e)
           WHERE record_type = 'FPI_FOR_HINT') ELSE 0 END)
  FROM lsn ORDER BY s)))
				{
					my ($op, $b) = split /\|/, $row;
					$bytes{$case}{$op} = $b;
					cmp_ok($b, '<', 65536, "$case: $op WAL is small");
				}

				# No WAL record in the whole run references a block of the
				# table, its indexes, its TOAST table or a non-permanent undo
				# log.
				is( $s->query_safe(
						q(
WITH r AS (SELECT oid FROM pg_class
            WHERE oid IN ('w'::regclass, (SELECT reltoastrelid FROM pg_class WHERE oid = 'w'::regclass))),
     f AS (SELECT pg_relation_filenode(oid) AS fn FROM r
           UNION ALL
           SELECT pg_relation_filenode(indexrelid) FROM pg_index
            WHERE indrelid IN (SELECT oid FROM r))
SELECT count(*) FROM pg_get_wal_block_info(
         (SELECT s FROM lsn WHERE op = 'insert'),
         (SELECT e FROM lsn WHERE op = 'settle'))
 WHERE relfilenode IN (SELECT fn FROM f)
    OR (reldatabase = 9 AND relfilenode IN
         (SELECT logno FROM pg_stat_get_undo_logs() WHERE persistence <> 'permanent'))
)),
					'0',
					"$case: no WAL references the relation or its undo");

				# Nor does any undo log record name a non-permanent undo log,
				# except the one that creates it: the creation of any undo log
				# is WAL-logged, which keeps replay's log numbers in step.
				is( $s->query_safe(
						q(
SELECT string_agg(record_type || ' ' || description, ', ')
  FROM pg_get_wal_records_info(
         (SELECT s FROM lsn WHERE op = 'insert'),
         (SELECT e FROM lsn WHERE op = 'settle'))
 WHERE resource_manager = 'UndoLog' AND record_type <> 'CREATE'
   AND substring(description from 'logno (\d+)')::oid IN
         (SELECT logno FROM pg_stat_get_undo_logs() WHERE persistence <> 'permanent')
)),
					'',
					"$case: no undo log record names a non-permanent undo log"
				);
				$s->query_safe('DROP TABLE w');
				$s->quit;
			}

			# The WAL does not grow with the number of rows.
			my $small = "$am $pers 10000 overflow $overflow";
			my $big = "$am $pers 100000 overflow $overflow";
			foreach my $op (sort keys %{ $bytes{$big} })
			{
				cmp_ok($bytes{$big}{$op}, '<', $bytes{$small}{$op} + 16384,
					"$am $pers overflow $overflow: $op WAL does not grow with rows"
				);
			}
		}
	}
}
$node->safe_psql('postgres',
	'ALTER SYSTEM RESET undo_rollback_overflow_size');
$node->reload;

# A crash with FLUX transactions on an unlogged and on a temp table in
# flight.  Recovery resets the unlogged table to empty and removes the temp
# table's files, and must not roll back the undo either of them left behind:
# that undo was never WAL-logged and describes pages that are gone.
$node->safe_psql(
	'postgres', q(
CREATE UNLOGGED TABLE cu (id int, v text) USING flux;
CREATE INDEX ON cu (id);
INSERT INTO cu SELECT g, 'orig' || g FROM generate_series(1, 10000) g;
));
my $ut = $node->background_psql('postgres');
$ut->query_safe(
	"BEGIN; UPDATE cu SET v = 'inflight'; DELETE FROM cu WHERE id % 3 = 0;");
my $tt = $node->background_psql('postgres');
$tt->query_safe(
	q(
CREATE TEMP TABLE ct (id int, v text) USING flux;
CREATE INDEX ON ct (id);
INSERT INTO ct SELECT g, 'orig' || g FROM generate_series(1, 10000) g;
BEGIN;
UPDATE ct SET v = 'inflight';
));
my $tpath = $node->data_dir . '/'
  . $tt->query_safe("SELECT pg_relation_filepath('ct')");
ok(-f $tpath, 'temp table file exists before the crash');

my $logstart = -s $node->logfile;
$node->stop('immediate');
$ut->{run}->kill_kill;
$tt->{run}->kill_kill;
$node->start;

is($node->safe_psql('postgres', 'SELECT count(*) FROM cu'),
	'0', 'unlogged FLUX table is empty after the crash');
ok(!-e $tpath, 'temp FLUX table files are gone after the crash');
my $rlog = slurp_file($node->logfile, $logstart);
unlike(
	$rlog,
	qr/UNDO recovery: rolling back/,
	'crash recovery rolls back no undo of the unlogged and temp tables');
unlike($rlog, qr/TRAP|PANIC/,
	'no assertion failure or PANIC after the crash');

# The reset table works.
$node->safe_psql(
	'postgres', q(
INSERT INTO cu SELECT g, 'orig' || g FROM generate_series(1, 100) g;
BEGIN;
UPDATE cu SET v = 'upd';
ROLLBACK;
));
is( $node->safe_psql(
		'postgres', "SELECT count(*) FROM cu WHERE v = 'orig' || id"),
	'100',
	'unlogged FLUX table rolls back after the crash');

$node->stop;
done_testing();
