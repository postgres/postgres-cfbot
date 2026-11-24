# Copyright (c) 2026, PostgreSQL Global Development Group

# Field widths in the "fixed" format are counted in bytes of the file
# encoding, not of the server encoding.  Check that with a server in UTF8
# and a file in LATIN1, where the two disagree about how wide a character is.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(extra => [ '--encoding=UTF8', '--no-locale' ]);
$node->append_conf('postgresql.conf',
	"session_preload_libraries = 'copy_fixed'");
$node->start;

my $outfile = $node->basedir . '/latin1.dat';

$node->safe_psql('postgres', q{CREATE TABLE t (a text)});
# Two characters that are one byte each in LATIN1 and two bytes each in UTF8.
$node->safe_psql('postgres', q{INSERT INTO t VALUES (U&'\00e9\00e9')});

$node->safe_psql('postgres',
	"COPY t TO '$outfile' WITH (format 'fixed', widths '4', encoding 'LATIN1')"
);

# Two LATIN1 bytes of data, two pad bytes, one newline.  Had the width been
# measured in the server encoding, the value would already have filled the
# field and no padding would have been written.  Comparing the exact bytes,
# rather than just the length, also catches the padding count being right
# but the pad character being wrong.
is(slurp_file($outfile), "\xe9\xe9  \n",
	'value is padded out to the field width');

# A value that exactly fills the field width in LATIN1 (2 bytes) would
# overflow it in UTF8 (4 bytes).  A module that measures in the server
# encoding rejects this value; only one that measures in the file encoding
# accepts it and writes it back with no padding at all.
my $exact_outfile = $node->basedir . '/latin1_exact.dat';
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"COPY t TO '$exact_outfile' WITH (format 'fixed', widths '2', encoding 'LATIN1')"
);
is($ret, 0,
	'a value that exactly fills the field width in LATIN1 is accepted, though the same value would overflow the field if measured in UTF8'
);

SKIP:
{
	skip 'COPY TO failed, no output file to check', 1 if $ret != 0;
	is(slurp_file($exact_outfile), "\xe9\xe9\n",
		'the value that exactly fills the LATIN1 field width is written with no padding'
	);
}

# And the file reads back into the same value.
$node->safe_psql('postgres', q{CREATE TABLE t2 (a text)});
$node->safe_psql('postgres',
	"COPY t2 FROM '$outfile' WITH (format 'fixed', widths '4', encoding 'LATIN1')"
);
is($node->safe_psql('postgres', q{SELECT a = (SELECT a FROM t) FROM t2}),
	't', 'round trip through a LATIN1 fixed-width file');

$node->stop;
done_testing();
