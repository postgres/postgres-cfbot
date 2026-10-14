# Copyright (c) 2026, PostgreSQL Global Development Group

# Exercise VM initialization and bit preservation during HOT-indexed redo.
use strict;
use warnings FATAL => 'all';

use File::Copy qw(copy);
use IPC::Run   qw(run);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $fpw ('off', 'on')
{
	for my $checking ('', 'all')
	{
		my $name = "fpw_${fpw}_check_" . ($checking || 'off');
		my $node = PostgreSQL::Test::Cluster->new($name);
		$node->init;
		$node->append_conf(
			'postgresql.conf', qq{
full_page_writes = $fpw
wal_consistency_checking = '$checking'
autovacuum = off
checkpoint_timeout = '1h'
});
		$node->start;
		$node->safe_psql(
			'postgres', q{
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE TABLE fresh (id int PRIMARY KEY, k int) WITH (fillfactor=50);
CREATE INDEX fresh_k ON fresh(k);
INSERT INTO fresh VALUES (1, 1);
CREATE TABLE existing (id int PRIMARY KEY, k int, padding text)
  WITH (fillfactor=50);
ALTER TABLE existing ALTER padding SET STORAGE PLAIN;
CREATE INDEX existing_k ON existing(k);
INSERT INTO existing SELECT i, i, repeat('x', current_setting('block_size')::int / 4)
  FROM generate_series(1, 3) i;
VACUUM (FREEZE) existing;
UPDATE existing SET k=13 WHERE id=3;
});
		is( $node->safe_psql(
				'postgres', q{SELECT ctid FROM existing ORDER BY id}),
			"(0,1)\n(1,1)\n(2,2)",
			"$name: neighbor occupies a separate heap page");

		# Save all table/index forks while stopped.  Restoring these after the
		# crash prevents dirty-buffer eviction from satisfying the test without
		# replaying the updates.  The fresh table has no initialized VM yet.
		my $block_size = $node->safe_psql('postgres', 'SHOW block_size');
		my @paths = split /\n/, $node->safe_psql(
			'postgres', q{
SELECT pg_relation_filepath(oid) FROM pg_class
 WHERE relname IN ('fresh', 'fresh_pkey', 'fresh_k',
                   'existing', 'existing_pkey', 'existing_k') ORDER BY oid
});
		$node->stop;
		my @saved;
		for my $path (@paths)
		{
			for my $fork ('', '_vm', '_fsm')
			{
				my $file = $node->data_dir . "/$path$fork";
				my $exists = -f $file;
				copy($file, "$file.saved") or die "copy $file: $!" if $exists;
				push @saved, [ $file, $exists ];
			}
		}
		$node->start;
		my $start =
		  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
		$node->safe_psql(
			'postgres', q{
UPDATE fresh SET k=11 WHERE id=1;
UPDATE existing SET k=11 WHERE id=1;
});
		my $end =
		  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
		# Avoid discovering the WAL segment size from an unused preallocated file.
		my ($startfile, $endfile) = split /\|/, $node->safe_psql(
			'postgres', qq{
SELECT pg_walfile_name('$start'), pg_walfile_name('$end'::pg_lsn - 1)
});
		$node->stop('immediate');
		my ($wal, $err) = ('', '');
		ok( run([
					'pg_waldump', '-p', $node->data_dir . '/pg_wal',
					'-s', $start, '-e', $end, '-r', 'Heap', $startfile, $endfile
				],
				'>' => \$wal,
				'2>' => \$err),
			"$name: read update WAL") or diag $err;
		my @updates = grep { /HOT_UPDATE .*blkref #3:/ } split /\n/, $wal;
		is(scalar @updates, 2, "$name: both updates WAL-log the split bit");
		# The existing table's row lock clears all-frozen first and can carry
		# the applied image.  Its subsequent HOT_UPDATE must replay the delta.
		for my $update (@updates)
		{
			if ($fpw eq 'on' && $update eq $updates[0])
			{
				like(
					$update,
					qr/blkref #3:.* fork vm blk 0 FPW(?! for)/,
					"$name: VM has an applied full-page image");
			}
			else
			{
				unlike(
					$update,
					qr/ FPW(?! for WAL verification)/,
					"$name: update requires record redo, not image restore");
				if ($checking)
				{
					like(
						$update,
						qr/blkref #3:.* FPW for WAL verification/,
						"$name: VM image is only for consistency checking");
				}
			}
		}
		for my $saved (@saved)
		{
			my ($file, $exists) = @$saved;
			if ($exists)
			{
				copy("$file.saved", $file) or die "restore $file: $!";
				unlink "$file.saved" or die "unlink $file.saved: $!";
			}
			elsif ($file =~ /_vm$/ && -f $file)
			{
				# VM extension writes zeros; initialization need not reach disk.
				open(my $vm, '>', $file) or die "open $file: $!";
				binmode($vm);
				print $vm "\0" x $block_size or die "write $file: $!";
				close($vm) or die "close $file: $!";
			}
			elsif (-f $file)
			{
				unlink $file or die "unlink $file: $!";
			}
		}
		my $started = $node->start(fail_ok => 1);
		ok($started, "$name: recover initialized and new VM pages");
		next unless $started;

		# Inspect before any heap access can prune a chain or change its VM.
		# VM contents start after the 24-byte page header.  Each heap block
		# has four bits: visible=1, frozen=2, split=4.  Thus byte 0 is 4 for
		# fresh, and 4 | (3 << 4) = 52 for existing; byte 1 retains split=4.
		for my $table ('fresh', 'existing')
		{
			is( $node->safe_psql(
					'postgres', qq{
SELECT CASE WHEN pg_relation_size('$table', 'vm') = 0 THEN 0
  ELSE get_byte(get_raw_page('$table', 'vm', 0), 24) END
}),
				$table eq 'fresh' ? '4' : '52',
				"$name: $table split bit and neighboring visibility bits survive"
			);
			if ($table eq 'existing')
			{
				is( $node->safe_psql(
						'postgres',
						q{SELECT get_byte(get_raw_page('existing', 'vm', 0), 25)}
					),
					'4',
					"$name: neighboring split bit survives");
			}
			is( $node->safe_psql(
					'postgres', "SELECT id, k FROM $table ORDER BY id"),
				$table eq 'fresh' ? '1|11' : "1|11\n2|2\n3|13",
				"$name: $table rows survive redo");
			is( $node->safe_psql(
					'postgres',
					"SELECT count(*) FROM verify_heapam('$table', check_toast := false)"
				),
				'0',
				"$name: $table heap passes amcheck");
		}
		$node->stop;
	}
}

# VM tail clearing can reach disk without advancing the page LSN when
# neither checksums nor wal_log_hints requires an image.  Recovery must accept
# that state while replaying earlier records, before Storage/TRUNCATE.
for my $checksums (0, 1)
{
	for my $hints ('off', 'on')
	{
		for my $truncate ('off', 'on')
		{
			my $name =
			  "checksums_${checksums}_hints_${hints}_truncate_${truncate}";
			my $node = PostgreSQL::Test::Cluster->new($name);
			$node->init(no_data_checksums => !$checksums);
			$node->append_conf(
				'postgresql.conf', qq{
full_page_writes = off
wal_consistency_checking = 'all'
wal_log_hints = $hints
autovacuum = off
checkpoint_timeout = '1h'
});
			$node->start;
			$node->safe_psql(
				'postgres', q{
CREATE EXTENSION pageinspect;
CREATE EXTENSION pg_buffercache;
CREATE TABLE tail (id int PRIMARY KEY, padding text) WITH (fillfactor=50);
ALTER TABLE tail ALTER padding SET STORAGE PLAIN;
INSERT INTO tail SELECT i, repeat('x', current_setting('block_size')::int / 4)
  FROM generate_series(1, 3) i;
});
			$node->safe_psql('postgres', 'VACUUM (FREEZE) tail');
			$node->safe_psql('postgres', 'CHECKPOINT');
			is( $node->safe_psql(
					'postgres', q{SELECT ctid FROM tail ORDER BY id}),
				"(0,1)\n(1,1)\n(2,1)",
				"$name: rows occupy separate heap pages");
			$node->safe_psql('postgres', 'DELETE FROM tail WHERE id=3');
			$node->safe_psql('postgres',
				"VACUUM (FREEZE, TRUNCATE $truncate) tail");
			my $state = q{
SELECT pg_relation_size('tail') / current_setting('block_size')::int,
       get_byte(get_raw_page('tail', 'vm', 0), 24),
       get_byte(get_raw_page('tail', 'vm', 0), 25)
};
			my $expected = $truncate eq 'on' ? '2|51|0' : '3|51|3';
			is($node->safe_psql('postgres', $state),
				$expected, "$name: vacuum updates heap size and VM tail");

			# Flush dirty relation buffers without moving the checkpoint.  This
			# is ordinary writeback, not offline page replacement or corruption.
			is( $node->safe_psql(
					'postgres', q{
SELECT buffers_evicted > 0 AND buffers_skipped = 0
  FROM pg_buffercache_evict_relation('tail')
}),
				't',
				"$name: relation buffers evicted");
			$node->stop('immediate');
			my $started = $node->start(fail_ok => 1);
			ok($started, "$name: recover after VM tail writeback");
			next unless $started;
			is($node->safe_psql('postgres', $state),
				$expected, "$name: recovered heap size and VM tail");
			is( $node->safe_psql(
					'postgres', 'SELECT id FROM tail ORDER BY id'),
				"1\n2",
				"$name: surviving rows recovered");
			$node->stop;
		}
	}
}

done_testing();
