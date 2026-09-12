# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that XLogBackgroundFlush() honors the adjustment applied by
# WaitXLogInsertionsToFinish() when the flush request is past the end
# of generated WAL.
#
# Such a request arises naturally when an xlog-switch record starts
# exactly SizeOfXLogRecord bytes before a segment boundary: the
# overridden EndPos (segment boundary + SizeOfXLogLongPHD) reaches
# XLogSetAsyncXactLSN() via XactLastRecEnd, because the transaction
# around pg_switch_wal() has no XID and therefore commits
# asynchronously.  Without the fix, the walwriter fails on one of
# XLogWrite()'s sanity checks (which one fires first depends on the
# WAL buffer state) and takes the server down, in both assert and
# production builds.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');

# Small segments keep the padding cheap.
$node->init(extra => ['--wal-segsize', '1']);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_writer_delay = 10ms
wal_writer_flush_after = 0
));
$node->start;

# Create a table for the later INSERT.
$node->safe_psql('postgres', 'CREATE TABLE t AS SELECT 1 AS i');

# Pad WAL with logical messages until the insert position is exactly
# SizeOfXLogRecord (24) bytes before a segment boundary, then switch.
# The record sizes involved are all MAXALIGNed, so filling the exact
# gap is possible; a record that crosses a page boundary picks up an
# extra page header and overshoots, in which case the loop moves on to
# the next segment boundary and tries again.
my $pad_and_switch = q{
DO $$
DECLARE
    segsz  numeric := (SELECT setting::numeric FROM pg_settings
                       WHERE name = 'wal_segment_size');
    zero   pg_lsn := '0/0';
    base   numeric;
    cur    numeric;
    target numeric;
    gap    numeric;
    seg0   numeric;
BEGIN
    -- size of a logical message record with an empty payload
    cur := pg_current_wal_insert_lsn() - zero;
    PERFORM pg_logical_emit_message(false, 'x', '');
    base := (pg_current_wal_insert_lsn() - zero) - cur;

    seg0 := floor((pg_current_wal_insert_lsn() - zero) / segsz);
    LOOP
        cur := pg_current_wal_insert_lsn() - zero;
        target := (floor(cur / segsz) + 1) * segsz - 24;
        gap := target - cur;
        EXIT WHEN gap = 0;
        IF floor(cur / segsz) - seg0 > 3 THEN
            RAISE EXCEPTION 'could not align the insert position';
        END IF;
        IF gap >= base + 8192 THEN
            PERFORM pg_logical_emit_message(false, 'x', repeat('a', 4096));
        ELSIF gap >= base + 200 THEN
            -- approach the target in small steps, so that the final
            -- record below stays under 256 bytes of main data and
            -- keeps its short data header; a larger record would
            -- switch to the long data header and its extra bytes
            -- would spoil the exact fill unless MAXALIGN swallows
            -- them
            PERFORM pg_logical_emit_message(false, 'x', repeat('a', 64));
        ELSIF gap >= base THEN
            PERFORM pg_logical_emit_message(false, 'x',
                                            repeat('a', (gap - base)::int));
        ELSE
            -- too close to the boundary, step over it and retry
            PERFORM pg_logical_emit_message(false, 'x', '');
        END IF;
    END LOOP;
END
$$;
SELECT pg_switch_wal() - '0/0'::pg_lsn;
};

my $segsz = 1024 * 1024;
my $bogus;
my $log_offset;

# A concurrent WAL record (e.g. a bgwriter snapshot) between the
# padding and the switch can spoil the alignment; the switch then does
# not report the overridden position and we simply try again.
foreach my $attempt (1 .. 10)
{
	$log_offset = -s $node->logfile;
	my $off = $node->safe_psql('postgres', $pad_and_switch);

	# A normal switch reports the segment boundary itself; only the
	# overridden EndPos lies past it, at segment boundary +
	# SizeOfXLogLongPHD (36 or 40 bytes, depending on MAXALIGN).  Any
	# nonzero offset into the new segment therefore marks the hit.
	if ($off % $segsz != 0)
	{
		$bogus = $off;
		last;
	}
}
die "could not hit the segment-boundary switch window"
  unless defined $bogus;

# The walwriter's next cycle picks up the bogus request and logs the
# adjustment.
$node->wait_for_log(qr/request to flush past end of generated WAL/,
	$log_offset);

# The advertised flush position must not include the bogus request.
my $result = $node->safe_psql('postgres',
	qq{SELECT pg_current_wal_flush_lsn() - '0/0'::pg_lsn < $bogus});
is($result, 't', 'flush position stays below the bogus request');

# The walwriter must not have failed one of XLogWrite()'s sanity
# checks: no child process may have been terminated.
my $log = slurp_file($node->logfile, $log_offset);
unlike(
	$log,
	qr/terminating any other active server processes/,
	'no crash after the bogus flush request');

# Normal WAL activity gets past the bogus position.
$node->safe_psql('postgres', 'INSERT INTO t VALUES (2)');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$result = $node->safe_psql('postgres',
	qq{SELECT pg_current_wal_flush_lsn() - '0/0'::pg_lsn > $bogus});
is($result, 't', 'flush position advances past the bogus request');

$node->stop;
done_testing();
