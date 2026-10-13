# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A change WAL-logged for a page sets the page's LSN to its record's, on the
# primary and in replay, so that the buffer manager writes the page only
# after its WAL is flushed and a later change after a checkpoint knows the
# page is logged.  A FLUX UPDATE that rewrites the row in place under the
# exclusive lock (it grows or shrinks the row, changes a key, or the
# relation is logically logged) left the page with the LSN of an earlier
# change.  heap is the control throughout.
#
# 1. For each kind of change: the page LSN after the change is at least the
#    end of its record, on the primary and on a standby that replayed it.
# 2. The consequence: an open transaction rewrites a row in place, the page
#    is written out (pg_buffercache_evict) while the UPDATE's WAL is not
#    flushed, and the server crashes.  The committed row must come back.
#    The WAL writer is held at an injection point, and the test checks that
#    the UPDATE's record is not flushed before the page is written.
# 3. A checkpoint between UPDATEs of a page makes only the first UPDATE after
#    it log a full-page image.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq{
autovacuum = off
shared_buffers = 32MB
bgwriter_lru_maxpages = 0
wal_writer_delay = 10000ms
});
$primary->start;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';
foreach my $ext (
	'pageinspect', 'pg_walinspect',
	'pg_buffercache', 'amcheck',
	'injection_points')
{
	plan skip_all => "Extension $ext not installed"
	  unless $primary->check_extension($ext);
	$primary->safe_psql('postgres', "CREATE EXTENSION $ext");
}

my $copyfile = $primary->basedir . '/rows.tsv';
open(my $fh, '>', $copyfile) or die "could not write $copyfile: $!";
print $fh "11\t0\tx\n12\t0\tx\n";
close($fh);

# Each change touches block 0 of its own table; %t is the table.  An
# INCLUDE-only change moves a FLUX row to a new TID (out of place).  A
# speculative abort is set up below.  The relation is logically logged in the
# last case only, while a logical slot exists.
my @kinds = (
	[ 'insert', "INSERT INTO %t VALUES (11, 0, 'x')" ],
	[ 'copy', "COPY %t FROM '$copyfile'" ],
	[
		'spec confirm',
		"INSERT INTO %t VALUES (11, 0, 'x') ON CONFLICT DO NOTHING"
	],
	[ 'spec abort', undef ],
	[ 'delete', "DELETE FROM %t WHERE id = 3" ],
	[ 'same-size update', "UPDATE %t SET v = 1 WHERE id = 3" ],
	[ 'growing update', "UPDATE %t SET pad = repeat('y', 200) WHERE id = 3" ],
	[ 'shrinking update', "UPDATE %t SET pad = '' WHERE id = 3" ],
	[ 'key update', "UPDATE %t SET id = 100 WHERE id = 3" ],
	[
		'out-of-place update',
		"UPDATE %t SET pad = 'q' WHERE id = 3",
		"CREATE INDEX ON %t (v) INCLUDE (pad)"
	],
	[ 'row lock', "SELECT * FROM %t WHERE id = 3 FOR UPDATE" ],
	[ 'logical same-size update', "UPDATE %t SET v = 1 WHERE id = 3" ],);

# A speculative insertion that finds, after inserting its tuple, that another
# transaction committed the same key in the meantime, and backs out.
sub spec_abort
{
	my ($t) = @_;
	$primary->safe_psql('postgres',
		"SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait')"
	);
	my $bg = $primary->background_psql('postgres');
	$bg->query_until(
		qr/started/, qq{
\\echo started
INSERT INTO $t VALUES (11, 0, 'x') ON CONFLICT DO NOTHING;
});
	$primary->wait_for_event('client backend',
		'exec-insert-before-insert-speculative');
	$primary->safe_psql('postgres', "INSERT INTO $t VALUES (11, 1, 'y')");
	$primary->safe_psql(
		'postgres', qq{
SELECT injection_points_detach('exec-insert-before-insert-speculative');
SELECT injection_points_wakeup('exec-insert-before-insert-speculative');
});
	$bg->quit;
}

$primary->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->start;
$standby->safe_psql('postgres', 'SELECT 1');

my @cases;
foreach my $am ('heap', 'flux')
{
	my $n = 0;
	foreach my $k (@kinds)
	{
		my $t = "ln_${n}_$am";
		$n++;
		$primary->safe_psql(
			'postgres', qq{
CREATE TABLE $t (id int PRIMARY KEY, v int, pad text) USING $am;
INSERT INTO $t SELECT g, 0, 'x' FROM generate_series(1, 10) g;
});
		$primary->safe_psql('postgres', $k->[2] =~ s/%t/$t/gr)
		  if defined $k->[2];
		push @cases,
		  {
			t => $t,
			name => "$am $k->[0]",
			sql => (defined $k->[1] ? $k->[1] =~ s/%t/$t/gr : undef),
			logical => ($k->[0] =~ /^logical/ ? 1 : 0)
		  };
	}
}

$primary->safe_psql('postgres', 'CHECKPOINT');
foreach my $c (@cases)
{
	my $t = $c->{t};
	if ($c->{logical})
	{
		$primary->safe_psql('postgres',
			"SELECT pg_create_logical_replication_slot('ln_slot', 'test_decoding')"
		);
		$primary->poll_query_until('postgres',
			"SELECT current_setting('effective_wal_level') = 'logical'")
		  or die "logical decoding did not turn on";
	}
	my $before =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	if (defined $c->{sql})
	{
		$primary->safe_psql('postgres', $c->{sql});
	}
	else
	{
		spec_abort($t);
	}
	$c->{end} = $primary->safe_psql(
		'postgres', qq{
SELECT max(end_lsn) FROM pg_get_wal_block_info('$before',
  pg_current_wal_insert_lsn(), false)
WHERE relfilenode = pg_relation_filenode('$t') AND relforknumber = 0
  AND relblocknumber = 0
  AND record_type NOT IN ('VM_SET', 'VM_CLEAR', 'VISIBLE')});
	$primary->safe_psql('postgres',
		"SELECT pg_drop_replication_slot('ln_slot')")
	  if $c->{logical};
	ok($c->{end} ne '', "$c->{name}: found a record for the page");
	is( $primary->safe_psql(
			'postgres', qq{
SELECT lsn >= '$c->{end}'::pg_lsn FROM page_header(get_raw_page('$t', 0))}),
		't',
		"$c->{name}: primary: page LSN covers the record")
	  or diag $primary->safe_psql(
		'postgres', qq{
SELECT (SELECT lsn FROM page_header(get_raw_page('$t', 0))) AS page_lsn,
       string_agg(start_lsn || '-' || end_lsn || ' ' || record_type, ', '
                  ORDER BY start_lsn)
FROM pg_get_wal_block_info('$before', pg_current_wal_insert_lsn(), false)
WHERE relfilenode = pg_relation_filenode('$t') AND relforknumber = 0
  AND relblocknumber = 0});
}

$primary->wait_for_replay_catchup($standby);
foreach my $c (@cases)
{
	is( $standby->safe_psql(
			'postgres', qq{
SELECT lsn >= '$c->{end}'::pg_lsn FROM page_header(get_raw_page('$c->{t}', 0))}
		),
		't',
		"$c->{name}: standby: page LSN covers the record");
}
$standby->stop;

# 2. A page written out before the WAL of its uncommitted in-place UPDATE.
# The table is vacuumed and the pages the queries below read are hinted
# before the checkpoint, so nothing logs a hint-bit image or flushes WAL
# between the UPDATE and the write, and the WAL writer is held at
# walwriter-before-background-flush: the page is written with the UPDATE's
# WAL still unflushed unless the page LSN names the UPDATE's record.  The
# server crashes.  The row is read right after the restart, and again after
# new write transactions, which take the xids the crash forgot.
$primary->safe_psql('postgres', 'CREATE TABLE evflush (i int)');
my $evict = q{
SELECT buffer_evicted FROM pg_buffercache_evict(
  (SELECT bufferid FROM pg_buffercache
   WHERE relfilenode = pg_relation_filenode('%s') AND relblocknumber = 0
     AND relforknumber = 0))};
foreach my $am ('heap', 'flux')
{
	my $t = "ev_$am";
	$primary->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int PRIMARY KEY, v int, pad text) USING $am;
INSERT INTO $t SELECT g, g, 'x' FROM generate_series(1, 10) g;
INSERT INTO evflush VALUES (0);
SELECT count(*) FROM $t;
SELECT count(*) FROM evflush;
VACUUM $t;
CHECKPOINT;
});
	foreach my $i (1 .. 2)
	{
		$primary->safe_psql('postgres', sprintf($evict, 'evflush'));
		$primary->safe_psql('postgres',
			"SELECT lsn FROM page_header(get_raw_page('$t', 0))");
	}
	$primary->safe_psql('postgres', 'INSERT INTO evflush VALUES (1)');
	$primary->safe_psql('postgres',
		"SELECT injection_points_attach('walwriter-before-background-flush', 'wait')"
	);
	$primary->wait_for_event('walwriter',
		'walwriter-before-background-flush');

	my $bg = $primary->background_psql('postgres');
	$bg->query_safe('BEGIN');
	$bg->query_safe(
		"UPDATE $t SET v = -1, pad = repeat('u', 300) WHERE id = 3");
	my $upd_end =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	is( $primary->safe_psql(
			'postgres',
			"SELECT pg_current_wal_flush_lsn() < '$upd_end'::pg_lsn"),
		't',
		"$am: the UPDATE's WAL is not flushed yet");
	is($primary->safe_psql('postgres', sprintf($evict, $t)),
		't', "$am: the page was written out");
	note "$am: page written, WAL flushed to "
	  . $primary->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()')
	  . ", UPDATE ends at $upd_end";
	$primary->stop('immediate');
	$bg->{run}->kill_kill;
	$primary->start;

	my $check = sub {
		my ($when) = @_;
		my ($rc, $out, $err) = $primary->psql('postgres',
			"SELECT id, v, pad FROM $t WHERE id = 3");
		is("$rc|$out", '0|3|3|x', "$am: $when: the committed row is back")
		  or diag "rc $rc, out '" . substr($out, 0, 30) . "', err $err";
		($rc, $out, $err) = $primary->psql('postgres',
				"SET enable_indexscan = off; SET enable_bitmapscan = off;"
			  . " SELECT count(*), sum(v) FROM $t");
		is("$rc|$out", '0|10|55', "$am: $when: every row is unchanged")
		  or diag "rc $rc, out '$out', err $err";
	};
	$check->('after the crash');
	foreach my $i (1 .. 3)
	{
		$primary->safe_psql('postgres', "INSERT INTO evflush VALUES ($i)");
	}
	$check->('after new transactions');
	my ($rc, $out, $err) =
	  $primary->psql('postgres', "SELECT bt_index_check('${t}_pkey', true)");
	is($rc, 0, "$am: amcheck after recovery") or diag $err;
}

# 3. Full-page images: one per page per checkpoint.  The rows are hinted
# before the checkpoint and found through the index, so no hint-bit image
# sets the page LSN in between.  Only images that replay applies count:
# wal_consistency_checking adds one to every record.
foreach my $am ('heap', 'flux')
{
	my $t = "fpi_$am";
	$primary->safe_psql(
		'postgres', qq{
CREATE TABLE $t (id int PRIMARY KEY, v int, pad text) USING $am;
INSERT INTO $t SELECT g, 0, 'x' FROM generate_series(1, 10) g;
SELECT count(*) FROM $t;
CHECKPOINT;
});
	my $before =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	foreach my $i (1 .. 5)
	{
		$primary->safe_psql('postgres',
				"SET enable_seqscan = off; SET enable_bitmapscan = off;"
			  . " UPDATE $t SET pad = repeat('g', 20 * $i) WHERE id = $i");
	}
	my $fpis = $primary->safe_psql(
		'postgres', qq{
SELECT count(*), coalesce(sum(block_fpi_length), 0),
       pg_current_wal_insert_lsn() - '$before'::pg_lsn
FROM pg_get_wal_block_info('$before', pg_current_wal_insert_lsn(), false)
WHERE relfilenode = pg_relation_filenode('$t') AND relforknumber = 0
  AND 'APPLY' = ANY (block_fpi_info)});
	note "$am: full-page images | their bytes | WAL bytes: $fpis";
	is( (split /\|/, $fpis)[0],
		'1',
		"$am: five growing UPDATEs after a checkpoint log one full-page image"
	);
}

done_testing();
