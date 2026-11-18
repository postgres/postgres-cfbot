# Copyright (c) 2026, PostgreSQL Global Development Group

# A checkpoint that starts while a MultiXact truncation is in progress must
# record the new oldestMulti.
#
# DELAY_CHKPT_START keeps a checkpoint from finishing while TruncateMultiXact()
# runs, but not from setting its redo pointer and reading oldestMulti
# (MultiXactGetCheckptMulti()) after the XLOG_MULTIXACT_TRUNCATE_ID record is
# written.  Recovery from such a checkpoint does not replay the truncation.  If
# the checkpoint read the old oldestMulti, recovery starts with an oldestMulti
# whose offsets segment is gone, and logs
#
#   LOG:  MultiXact member truncation is disabled because oldest checkpointed
#         MultiXact N does not exist on disk
#
# The test stops the truncating backend right after the record is written, and
# runs a checkpoint there.  The checkpoint takes its redo pointer, reads
# oldestMulti and waits for the backend.  Then the backend finishes, and the
# server crashes.  The checkpoint must have recorded the new oldestMulti.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# More than a pg_multixact/offsets segment of MultiXactIds, so that truncating up
# to the newest one removes segment 0000.  A segment is 32 pages; a page holds
# 1024 offsets here (8-byte offsets) and 2048 with 4-byte offsets in the back
# branches, i.e. up to 65536 MultiXactIds.
my $nrows = 70000;

# One MultiXactId per row.  A MultiXactId is created when a second transaction
# share-locks a row that another one has share-locked.  That alone is not enough:
# a backend caches the MultiXactId it created for a given set of members and
# reuses it, so two sessions locking all the rows would create just one.  So the
# members have to differ from row to row: session A locks all the rows with one
# transaction id, and session B locks them one by one, each in its own
# subtransaction (the EXCEPTION block of the DO loop), i.e. with its own
# subtransaction id.
$node->safe_psql('postgres',
	"CREATE TABLE mx (id int PRIMARY KEY); INSERT INTO mx SELECT generate_series(1, $nrows)"
);
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
$a->query_safe(q{BEGIN; SELECT count(*) FROM (SELECT id FROM mx FOR SHARE) s;});
$b->query_safe(
	q{BEGIN;
DO $$
DECLARE r record;
BEGIN
  FOR r IN SELECT id FROM mx LOOP
    BEGIN
      PERFORM 1 FROM mx WHERE id = r.id FOR SHARE;
    EXCEPTION WHEN OTHERS THEN
      NULL;
    END;
  END LOOP;
END $$;});
$a->query_safe("COMMIT;");
$b->query_safe("COMMIT;");
$a->quit;
$b->quit;

ok(-e $node->data_dir . '/pg_multixact/offsets/0000',
	'premise: offsets segment 0000 exists');
ok(-e $node->data_dir . '/pg_multixact/offsets/0001',
	'premise: the rows were locked by MultiXacts, more than one offsets segment of them');
is($node->safe_psql('postgres', "SELECT oldest_multi_xid FROM pg_control_checkpoint()"),
	'1', 'premise: the oldest MultiXactId is the first one');

# Let datminmxid of every database advance, the last one (postgres) from a
# backend that stops at the injection point.
$node->safe_psql('postgres',
	'ALTER DATABASE template0 WITH ALLOW_CONNECTIONS true');
$node->safe_psql($_, 'VACUUM (FREEZE)') for qw(template0 template1);

my $vacuum = $node->background_psql('postgres', on_error_stop => 0);
$vacuum->query_safe(
	q(SELECT injection_points_set_local();
	  SELECT injection_points_attach('multixact-truncate-after-record', 'wait');));
$vacuum->query_until(
	qr/starting_vacuum/, q(
	\echo starting_vacuum
	VACUUM (FREEZE);
));
$node->wait_for_event('client backend', 'multixact-truncate-after-record');

# The truncation record is written and flushed, and the files are still there.
ok(-e $node->data_dir . '/pg_multixact/offsets/0000',
	'premise: segment 0000 is still there when the backend is stopped');

# A checkpoint now: its redo pointer is after the truncation record.  It reads
# oldestMulti first, then waits for the backend (DELAY_CHKPT_START).
my $log_offset = -s $node->logfile;
my $ckpt = $node->background_psql('postgres', on_error_stop => 0);
$ckpt->query_until(
	qr/starting_checkpoint/, q(
	\echo starting_checkpoint
	CHECKPOINT;
));
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'CheckpointDelayStart'"
	),
	'premise: the checkpoint went past taking its redo pointer and waits for the truncating backend'
);

# Let the truncating backend finish; the checkpoint can then complete.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('multixact-truncate-after-record')");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'CheckpointDelayStart' OR wait_event = 'multixact-truncate-after-record'"
) or die "timed out waiting for the checkpoint and the vacuum to finish";
$ckpt->query_safe("SELECT 1");    # the CHECKPOINT command has returned
ok(!-e $node->data_dir . '/pg_multixact/offsets/0000',
	'premise: segment 0000 was removed');

# What the checkpoint recorded.  All the MultiXactIds are gone (every row was
# frozen), so the new oldest MultiXactId is the next one to be assigned.
my ($recorded, $next) = split(
	/\|/,
	$node->safe_psql('postgres',
		"SELECT oldest_multi_xid, next_multixact_id FROM pg_control_checkpoint()"));
note("oldest_multi_xid in the checkpoint: $recorded, next_multixact_id: $next");
cmp_ok($next, '>', 32768, 'premise: the MultiXactIds span more than one segment');
is($recorded, $next,
	'the checkpoint records the new oldest MultiXactId, not one in the removed segment');

# Crash, and recover from that checkpoint.
$node->stop('immediate');
$vacuum->{run}->finish;
$ckpt->{run}->finish;
$log_offset = -s $node->logfile;
$node->start;

unlike(
	slurp_file($node->logfile, $log_offset),
	qr/MultiXact member truncation is disabled because oldest checkpointed MultiXact \d+ does not exist on disk/,
	'recovery does not start from an oldest MultiXactId whose offsets are gone');

$node->stop;
done_testing();
