# Copyright (c) 2026, PostgreSQL Global Development Group

# When a hash index allocates a new splitpoint, it extends the file to the
# last block and WAL-logs that block.  If a base backup's start checkpoint
# came between the two steps, and the extension came after the WAL record,
# the backup could copy the file without that block, and recovery would not
# add it back.  Inserts would then fail once they need an overflow page past
# the end of the file.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

$node->safe_psql(
	'postgres', q(
	CREATE TABLE hash_tab (k int);
	CREATE INDEX hash_idx ON hash_tab USING hash (k);
	INSERT INTO hash_tab SELECT (random() * 1e9)::int
	  FROM generate_series(1, 20000);
));

my $splitter = $node->background_psql('postgres');
my $splitter_pid = $splitter->query_safe('SELECT pg_backend_pid()');
$splitter->query_safe(
	q(SELECT injection_points_set_local();
	  SELECT injection_points_attach('hash-expandtable-start', 'wait');
	  SELECT injection_points_attach('hash-alloc-buckets-before-wal', 'wait');
));

# Insert until the index file grows, which is either a new splitpoint or a
# new overflow page.
my $insert_loop = q{
	\echo starting_inserts
	DO $$
	DECLARE sz bigint := pg_relation_size('hash_idx');
	BEGIN
	  LOOP
		INSERT INTO hash_tab VALUES ((random() * 1e9)::int);
		EXIT WHEN pg_relation_size('hash_idx') <> sz;
	  END LOOP;
	END $$;
};
$splitter->query_until(qr/starting_inserts/, $insert_loop);

# Each split stops at hash-expandtable-start.  Run a checkpoint there, so
# that the metapage is clean when the split stops in _hash_alloc_buckets().
# Otherwise the backup's checkpoint would wait for the metapage content lock
# until the split finishes.
my $max_attempts = $PostgreSQL::Test::Utils::timeout_default * 100;
while (1)
{
	die "timed out waiting for a splitpoint allocation"
	  if --$max_attempts < 0;

	my $state = $node->safe_psql('postgres',
		"SELECT state || ':' || coalesce(wait_event, '') FROM pg_stat_activity WHERE pid = $splitter_pid"
	);
	if ($state eq 'active:hash-expandtable-start')
	{
		$node->safe_psql('postgres', 'CHECKPOINT');
		$node->safe_psql('postgres',
			q(SELECT injection_points_wakeup('hash-expandtable-start')));

		# The waiter checks for the wakeup only every 100ms, and the next
		# insert may come right back here.  Give it time to move on.
		usleep(250_000);
	}
	elsif ($state eq 'active:hash-alloc-buckets-before-wal')
	{
		last;
	}
	elsif ($state =~ /^idle/)
	{
		# The file grew because of an overflow page, try again.
		$splitter->query_until(qr/starting_inserts/, $insert_loop);
	}
	else
	{
		usleep(10_000);
	}
}

# Take a base backup while the split waits between the extension and the WAL
# record, and wait until the backup either finishes, or its checkpoint waits
# for the split.
my $backup_path = $node->backup_dir . '/hash_backup';
my ($bb_stdout, $bb_stderr) = ('', '');
my $bb = IPC::Run::start(
	[
		'pg_basebackup', '--no-sync',
		'--pgdata' => $backup_path,
		'--host' => $node->host,
		'--port' => $node->port,
		'--checkpoint' => 'fast',
	],
	'>' => \$bb_stdout,
	'2>' => \$bb_stderr);

$max_attempts = $PostgreSQL::Test::Utils::timeout_default * 10;
while ($bb->pumpable)
{
	die "timed out waiting for the base backup" if --$max_attempts < 0;
	$bb->pump_nb;
	last
	  if $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'checkpointer' AND wait_event = 'CheckpointDelayStart'"
	  ) eq '1';
	usleep(100_000);
}

$node->safe_psql('postgres',
	q(SELECT injection_points_wakeup('hash-alloc-buckets-before-wal')));
$bb->finish or die "pg_basebackup failed: $bb_stderr";
$splitter->query_safe(
	q(SELECT injection_points_detach('hash-expandtable-start');
	  SELECT injection_points_detach('hash-alloc-buckets-before-wal');));
$splitter->quit;

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'hash_backup', has_streaming => 1);
$standby->start;
$node->wait_for_replay_catchup($standby);
$standby->promote;

my ($ret, $stdout, $stderr) = $standby->psql('postgres',
	q(INSERT INTO hash_tab SELECT (random() * 1e9)::int
	  FROM generate_series(1, 200000)));
is($ret, 0, 'hash index restored from backup accepts inserts');
is($standby->safe_psql('postgres', q(SELECT pg_relation_size('hash_idx') > 0)),
	't', 'hash index is not empty');
$standby->stop;

# If the step after the extension fails, and a savepoint catches the error,
# checkpoints must not wait for the rest of the transaction, and the next
# split must be able to allocate the same blocks again.
my $failer = $node->background_psql('postgres', on_error_stop => 0);
$failer->query_safe(
	q(SELECT injection_points_set_local();
	  SELECT injection_points_attach('hash-alloc-buckets-before-wal', 'error');
	  BEGIN;
	  SAVEPOINT s;));
$failer->query(
	q{DO $$
	BEGIN
	  FOR i IN 1..3000 LOOP
		INSERT INTO hash_tab SELECT (random() * 1e9)::int
		  FROM generate_series(1, 1000);
	  END LOOP;
	END $$;});
like(
	$failer->{stderr},
	qr/error triggered for injection point hash-alloc-buckets-before-wal/,
	'bucket allocation failed after extending the file');
$failer->{stderr} = '';
$failer->query_safe('ROLLBACK TO s');

my $timed_out = 0;
$node->psql(
	'postgres', 'CHECKPOINT',
	timeout => $PostgreSQL::Test::Utils::timeout_default,
	timed_out => \$timed_out);
ok(!$timed_out, 'checkpoint does not wait after rollback to savepoint');

$failer->query_safe(
	q(SELECT injection_points_detach('hash-alloc-buckets-before-wal');
	  SELECT injection_points_attach('hash-alloc-buckets-before-wal', 'notice');));
$failer->{stderr} = '';
$failer->query(
	q(INSERT INTO hash_tab SELECT (random() * 1e9)::int
	  FROM generate_series(1, 100000)));
like(
	$failer->{stderr},
	qr/notice triggered for injection point hash-alloc-buckets-before-wal/,
	'bucket allocation was retried');
unlike($failer->{stderr}, qr/ERROR/, 'retried bucket allocation succeeded');

$failer->query(
	q(SELECT injection_points_detach('hash-alloc-buckets-before-wal')));
$failer->{stderr} = '';
$failer->query_safe('COMMIT');
$failer->quit;
$node->stop;

done_testing();
