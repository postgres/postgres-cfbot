# Copyright (c) 2024-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use locale;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Node initialization
my $node_sync = PostgreSQL::Test::Cluster->new('sync');
$node_sync->init();
$node_sync->append_conf('postgresql.conf', "io_method = sync");
$node_sync->start;

# Check that the XML-formatted EXPLAIN output contains Storage I/O
# read and write information.
my $xml_sync = $node_sync->safe_psql(
	'postgres',
	q{
	EXPLAIN (ANALYZE, IO, FORMAT XML)
	SELECT * FROM pg_class;
	}
);

like(
	$xml_sync,
	qr/<Storage-I-O-Read>\d+<\/Storage-I-O-Read>/,
	"Storage-I-O-Read is shown in EXPLAIN XML (io_method=sync)"
);

like(
	$xml_sync,
	qr/<Storage-I-O-Write>\d+<\/Storage-I-O-Write>/,
	"Storage-I-O-Write is shown in EXPLAIN XML (io_method=sync)"
);

# Do the same test if io_method=io_uring is supported
if (have_io_uring())
{
	my $node_io_uring = PostgreSQL::Test::Cluster->new('io_uring');
	$node_io_uring->init;
	$node_io_uring->append_conf('postgresql.conf', "io_method = 'io_uring'");
	$node_io_uring->start;

	my $xml_io_uring = $node_io_uring->safe_psql(
		'postgres',
		q{
		EXPLAIN (ANALYZE, IO, FORMAT XML)
		SELECT * FROM pg_class;
		}
	);

	like(
		$xml_io_uring,
		qr/<Storage-I-O-Read>\d+<\/Storage-I-O-Read>/,
		"Storage-I-O-Read is shown in EXPLAIN XML (io_method=io_uring)"
	);

	like(
		$xml_io_uring,
		qr/<Storage-I-O-Write>\d+<\/Storage-I-O-Write>/,
		"Storage-I-O-Write is shown in EXPLAIN XML (io_method=io_uring)"
	);
}
else
{
    note "io_uring is not supported on this platform. skipping io_uring tests";
}

sub have_io_uring
{
        # To detect if io_uring is supported, we look at the error message for
        # assigning an invalid value to an enum GUC, which lists all the valid
        # options. We need to use -C to deal with running as administrator on
        # windows, the superuser check is omitted if -C is used.
        my ($stdout, $stderr) =
          run_command [qw(postgres -C invalid -c io_method=invalid)];
        die "can't determine supported io_method values"
          unless $stderr =~ m/Available values: ([^\.]+)\./;
        my $methods = $1;
        note "supported io_method values are: $methods";

        return ($methods =~ m/io_uring/) ? 1 : 0;
}

done_testing();
