# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A reader walking a FLUX version chain reconstructs each prior image with
# FluxUndoPriorImage() and then reads it as a FluxTupleHeader (FluxTupleGetXmin
# and the version pointer, at offsets below SizeOfFluxTupleHeader = 36).
# FluxUndoPriorImage used to set the image length straight from the undo
# record's recorded length and check only that the payload held that many
# bytes -- never that the result is at least a tuple header.  A corrupt undo
# record with a 1..35-byte before-image therefore produced a palloc'd image
# shorter than the header the walk then reads, an out-of-bounds read (on the
# ASAN build a heap-buffer-overflow / hard abort).
#
# The window is only reachable with a corrupt record, so the injection point
# flux-undo-prior-image-short forces the reconstructed length to 1 (what such a
# record would yield) just before the image is allocated.  Without the fix the
# subsequent walk over-reads; with the fix FluxUndoPriorImage rejects the
# sub-header length and the reader raises a data_corrupted ERROR instead.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_undo_short_image');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_preload_libraries = 'injection_points'
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE TABLE f (id int PRIMARY KEY, v text) USING flux;
INSERT INTO f VALUES (1, 'original');
});

# A reader whose snapshot predates the UPDATE must walk back through the undo
# record to reconstruct the prior ('original') image -- that is the path that
# reads the reconstructed image as a tuple header.  Hold the old snapshot open
# in a background session.
my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$reader->query_safe('SELECT 1');    # take the snapshot now
is($reader->query_safe('SELECT v FROM f WHERE id = 1'),
	'original', 'the old snapshot sees the original value before the update');

# Create the version chain the reader must walk back over.
$node->safe_psql('postgres', "UPDATE f SET v = 'updated' WHERE id = 1");

# Simulate the corrupt record: force the reconstructed before-image to a
# sub-header length on the next prior-image step.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('flux-undo-prior-image-short', 'notice')");

# The old snapshot now has to walk back to the 'original' image, where the
# corrupt (sub-header) length is injected.  query() returns the error on
# stderr rather than dying.
my ($out, $ret) = $reader->query('SELECT v FROM f WHERE id = 1');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('flux-undo-prior-image-short')");

# The read must fail cleanly as corruption -- never an ASAN report / crash.
like(
	$reader->{stderr},
	qr/version chain of .* is broken .*before-image shorter than a tuple header/,
	'a sub-header before-image is rejected as data corruption, not over-read');

$reader->query('ROLLBACK');
$reader->quit;

# The backend survived (no crash): a fresh read of the current value works.
is( $node->safe_psql('postgres', 'SELECT v FROM f WHERE id = 1'),
	'updated', 'the server is still up and serving the current value');

$node->stop;
done_testing();
