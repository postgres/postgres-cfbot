# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the end LSN reported for an xlog-switch record that ends exactly
# on a segment boundary.
#
# This happens when the switch record starts SizeOfXLogRecord bytes
# before the boundary.  The reported end must be the boundary itself,
# not the boundary plus a page header that the record never crossed.
# That position reaches XLogSetAsyncXactLSN() via XactLastRecEnd,
# because the transaction around pg_switch_wal() has no XID and
# therefore commits asynchronously, and the walwriter would then try to
# write and flush WAL that was never generated.

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
));
$node->start;

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
SELECT pg_current_wal_insert_lsn() - '0/0'::pg_lsn;
SELECT pg_switch_wal() - '0/0'::pg_lsn;
};

my $segsz = 1024 * 1024;
my ($start, $end);

# A concurrent WAL record (e.g. a bgwriter snapshot) between the
# padding and the switch can spoil the alignment; the switch record
# then does not start where we measured, and we simply try again.  A
# hit ends at most SizeOfXLogRecord + SizeOfXLogLongPHD bytes past the
# measured start, whichever end position the server reports.  The
# padding itself fails if such a record, or a page header, lands
# between the two reads that measure the size of an empty message;
# that is a miss too.
foreach my $attempt (1 .. 10)
{
	my $out;
	next if $node->psql('postgres', $pad_and_switch, stdout => \$out) != 0;
	($start, $end) = split /\n/, $out;

	last
	  if $start % $segsz == $segsz - 24
	  && $end - $start <= 24 + 40;
	undef $start;
}
die "could not hit the segment-boundary switch window"
  unless defined $start;

is($end, $start + 24,
	'switch record ending on a segment boundary reports the boundary');

$node->stop;
done_testing();
