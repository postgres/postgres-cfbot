# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX: PREPARE TRANSACTION after rows were written and the relation's storage
# was then replaced in the same transaction (SET ACCESS METHOD, ALTER COLUMN
# TYPE, CLUSTER, TRUNCATE), then COMMIT PREPARED or ROLLBACK PREPARED from
# another backend, with and without a restart in between.  Heap is the
# control: every FLUX table must end up as its heap twin does, on the primary
# and on a streaming standby.
#
# FLUX remembers the tuples a transaction wrote by relation OID and TID.  At
# PREPARE it marks the rows a rolled-back savepoint inserted as deleted; after
# a rewrite that work landed on the new storage, where the same TIDs name
# other rows (or are past its end).  src/test/regress/sql/flux_rewrite_xact.sql
# covers COMMIT and ROLLBACK.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_prepared_transactions = 100
));
$node->start;
my $have_amcheck = $node->check_extension('amcheck');
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck') if $have_amcheck;
$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->start;

$node->safe_psql(
	'postgres', q{
CREATE FUNCTION rx_val(k int) RETURNS text LANGUAGE sql IMMUTABLE
  RETURN repeat(md5(k::text), 150);
-- What a reader sees: rows, their ids and keys, intact values.
CREATE FUNCTION rx_seen(rel regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; ids bigint; ks bigint; good bigint;
BEGIN
  EXECUTE format('SELECT count(*), sum(id), sum(k),
                         count(*) FILTER (WHERE w = rx_val(k)) FROM %s', rel)
    INTO n, ids, ks, good;
  RETURN format('rows=%s ids=%s ks=%s good=%s', n, ids, ks, good);
EXCEPTION WHEN OTHERS THEN
  RETURN 'ERROR: ' || SQLERRM;
END $$;
-- What a writer finds, rolled back so the standby keeps the same rows.
CREATE FUNCTION rx_found(rel regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE lck bigint; upd bigint; del bigint; r text;
BEGIN
  BEGIN
    EXECUTE format('SELECT count(*) FROM (SELECT 1 FROM %s FOR UPDATE) x', rel)
      INTO lck;
    EXECUTE format('WITH x AS (UPDATE %s SET w = w RETURNING 1)
                    SELECT count(*) FROM x', rel) INTO upd;
    EXECUTE format('WITH x AS (DELETE FROM %s RETURNING 1)
                    SELECT count(*) FROM x', rel) INTO del;
    r := format('lock=%s upd=%s del=%s', lck, upd, del);
    RAISE EXCEPTION USING ERRCODE = 'P0099', MESSAGE = r;
  EXCEPTION WHEN SQLSTATE 'P0099' THEN
    RETURN SQLERRM;
  END;
EXCEPTION WHEN OTHERS THEN
  RETURN 'ERROR: ' || SQLERRM;
END $$;
});

# name => transaction body; $T is the table, $AM its access method.  20 rows
# are committed first.  The rows written in the transaction have the same
# width as those, so the TIDs of the old and the new storage collide.
my $ins21 =
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g';
my $ins41 =
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(41, 60) g';
my $refill =
  'INSERT INTO $T SELECT g, g + 100, rx_val(g + 100) FROM generate_series(1, 20) g';
my $sp = 'SAVEPOINT a; ' . $ins21 . '; ROLLBACK TO a';
my %cases = (
	fh => "$ins21; ALTER TABLE \$T SET ACCESS METHOD heap",
	fhf => "$ins21; ALTER TABLE \$T SET ACCESS METHOD heap; "
	  . "ALTER TABLE \$T SET ACCESS METHOD \$AM",
	sp_fh => "$sp; $ins41; ALTER TABLE \$T SET ACCESS METHOD heap",
	trunc => "TRUNCATE \$T; $refill",
	sp_trunc => "$sp; TRUNCATE \$T; $refill",
	act => "$ins21; ALTER TABLE \$T ALTER COLUMN id TYPE bigint",
	sp_act => "$sp; $ins41; ALTER TABLE \$T ALTER COLUMN id TYPE bigint",
	cluster =>
	  'UPDATE $T SET k = k + 100, w = rx_val(k + 100) WHERE id % 2 = 0; '
	  . "DELETE FROM \$T WHERE id % 5 = 0; $ins21; CLUSTER \$T USING \$T_pkey",
	sp_trunc_act => 'SAVEPOINT a; '
	  . 'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 400) g; '
	  . 'ROLLBACK TO a; TRUNCATE $T; '
	  . "ALTER TABLE \$T ALTER COLUMN id TYPE bigint; $refill",);

# Each case on each AM, then COMMIT PREPARED or ROLLBACK PREPARED, before or
# after a restart.
my @runs;
foreach my $name (sort keys %cases)
{
	foreach my $fin ('commit', 'rollback')
	{
		foreach my $when ('live', 'restart')
		{
			foreach my $am ('heap', 'flux')
			{
				my $t = "rx_${name}_${fin}_${when}_$am";
				(my $body = $cases{$name}) =~ s/\$T/$t/g;
				$body =~ s/\$AM/$am/g;
				$node->safe_psql(
					'postgres', qq{
CREATE TABLE $t (id int PRIMARY KEY, k int, w text) USING $am;
INSERT INTO $t SELECT g, g, rx_val(g) FROM generate_series(20, 1, -1) g;
});
				my ($ret, $out, $err) = $node->psql('postgres',
					"BEGIN; $body; PREPARE TRANSACTION '$t';");
				is($ret, 0, "$t: PREPARE TRANSACTION")
				  or diag($err);
				push @runs, [ $name, $fin, $when, $am, $t ];
			}
		}
	}
}

sub resolve
{
	my ($when) = @_;
	foreach my $r (grep { $_->[2] eq $when } @runs)
	{
		my ($name, $fin, $w, $am, $t) = @$r;
		my ($ret, $out, $err) = $node->psql('postgres',
			($fin eq 'commit' ? 'COMMIT' : 'ROLLBACK') . " PREPARED '$t'");
		is($ret, 0, "$t: $fin prepared from another backend")
		  or diag($err);
	}
}
resolve('live');
$node->restart;
resolve('restart');
$node->wait_for_replay_catchup($standby);

foreach my $r (grep { $_->[3] eq 'heap' } @runs)
{
	my ($name, $fin, $when, $am, $heap) = @$r;
	(my $flux = $heap) =~ s/_heap$/_flux/;
	my $what = "$name, $fin prepared, $when";

	my $want = $node->safe_psql('postgres', "SELECT rx_seen('$heap')");
	is($node->safe_psql('postgres', "SELECT rx_seen('$flux')"),
		$want, "$what: rows on the primary");
	is($standby->safe_psql('postgres', "SELECT rx_seen('$flux')"),
		$want, "$what: rows on the standby");
	is( $node->safe_psql('postgres', "SELECT rx_found('$flux')"),
		$node->safe_psql('postgres', "SELECT rx_found('$heap')"),
		"$what: rows UPDATE, DELETE and FOR UPDATE find");

	# A rewrite to heap must leave a valid heap.
	if ($have_amcheck
		&& $node->safe_psql('postgres',
			"SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam "
			  . "WHERE c.oid = '$flux'::regclass") eq 'heap')
	{
		is( $node->safe_psql(
				'postgres', "SELECT count(*) FROM verify_heapam('$flux')"),
			'0',
			"$what: verify_heapam");
	}
}

is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts'),
	'0', 'no prepared transactions left');

done_testing();
