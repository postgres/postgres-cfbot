# Copyright (c) 2026, PostgreSQL Global Development Group

# Undo pages must participate in both initial and online checksums.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib $FindBin::RealBin;
use DataChecksums::Utils;

for my $am (qw(heap flux))
{
	for my $online (0, 1)
	{
		my $node = PostgreSQL::Test::Cluster->new("${am}_${online}");
		$node->init(
			allows_streaming => 1,
			extra => [ $online ? '--no-data-checksums' : '--data-checksums' ]
		);
		$node->start;
		$node->safe_psql(
			'postgres', qq[
			CREATE TABLE t (a int, b text) USING $am;
			INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1,10000) g;
		]);
		my $writer = $node->background_psql('postgres');
		$writer->query_safe('BEGIN; UPDATE t SET a = a + 1;');
		# Force pre-enable undo pages to disk while the transaction is open.
		$node->safe_psql('postgres', 'CHECKPOINT');
		$writer->query_safe('COMMIT');
		$writer->quit;
		enable_data_checksums($node, wait => 'on') if $online;
		$node->backup('checksummed');
		$node->stop;
		command_ok(
			[ 'pg_checksums', '--check', '-D', $node->data_dir ],
			"$am offline verification (online=$online)");
	}
}
done_testing();
