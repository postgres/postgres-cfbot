# Copyright (c) 2026, PostgreSQL Global Development Group

# Old-key images and unchanged toast must decode exactly as heap does.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('decode');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'synchronous_commit = on');
$node->start;

for my $identity ('DEFAULT', 'FULL', 'USING INDEX t_key', 'NOTHING')
{
	my %output;
	for my $am ('heap', 'flux')
	{
		$node->safe_psql(
			'postgres', qq{
			CREATE TABLE t (id int PRIMARY KEY, k text NOT NULL, v text, n int) USING $am;
			CREATE UNIQUE INDEX t_key ON t(k);
			ALTER TABLE t ALTER COLUMN k SET STORAGE EXTERNAL;
			ALTER TABLE t ALTER COLUMN v SET STORAGE EXTERNAL;
			ALTER TABLE t REPLICA IDENTITY $identity;
			SELECT pg_create_logical_replication_slot('s', 'test_decoding');
		});
		my @changes = (
			"INSERT INTO t VALUES (1, repeat('k', 2000), repeat('v', 5000), 0)",
			"INSERT INTO t VALUES (3, 'third', 'value', 0) ON CONFLICT (id) DO NOTHING",
			"INSERT INTO t VALUES (1, 'duplicate', 'value', 0) ON CONFLICT (id) DO UPDATE SET n = 2",
			'DELETE FROM t WHERE id = 3',
			'UPDATE t SET n = 1',
			'UPDATE t SET id = 2',
			"UPDATE t SET k = repeat('j', 2000)",
			"UPDATE t SET v = repeat('w', 6000)",
			'ALTER TABLE t ADD COLUMN extra int DEFAULT 42',
			'UPDATE t SET extra = 43',
			'ALTER TABLE t DROP COLUMN n',
			'UPDATE t SET extra = 44',
			'DELETE FROM t');
		for my $sql (@changes)
		{
			$node->safe_psql('postgres', $sql);
			my $decoded = $node->safe_psql(
				'postgres', q{
				SELECT data FROM pg_logical_slot_get_changes('s', NULL, NULL,
					'include-xids', '0', 'skip-empty-xacts', '1')
			});
			if ($am eq 'heap')
			{
				$output{$sql} = $decoded;
			}
			else
			{
				is($decoded, $output{$sql}, "$identity: $sql matches heap");
			}
		}
		$node->safe_psql('postgres',
			"SELECT pg_drop_replication_slot('s'); DROP TABLE t");
	}
}

# Stop both sessions after speculative heap/FLUX insertion but before the
# unique index insertion.  The second session must kill its speculative row.
my %spec_output;
for my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq{
		CREATE TABLE spec (id int, v text) USING $am;
		ALTER TABLE spec ALTER COLUMN v SET STORAGE EXTERNAL;
		CREATE FUNCTION spec_key(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS \$\$
		BEGIN
			IF current_setting('spec.calls')::int = 1 THEN
				PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int);
			END IF;
			PERFORM set_config('spec.calls', '1', false);
			RETURN \$1;
		END \$\$;
		CREATE UNIQUE INDEX spec_key_idx ON spec (spec_key(id));
		SELECT pg_create_logical_replication_slot('s', 'test_decoding');
	});
	my $controller = $node->background_psql('postgres');
	$controller->query_safe(
		'SELECT pg_advisory_lock(1), pg_advisory_lock(2)');
	my @sessions;
	for my $id (1, 2)
	{
		my $bg = $node->background_psql('postgres');
		$bg->query_safe("SET spec.calls = 0; SET spec.session = $id");
		$bg->query_until(
			qr/starting/, q{
			\echo starting
			INSERT INTO spec VALUES (1, repeat('toast', 1000)) ON CONFLICT DO NOTHING;
		});
		push @sessions, $bg;
	}
	$node->poll_query_until(
		'postgres', q{
		SELECT count(*) = 2 FROM pg_locks WHERE locktype = 'advisory' AND NOT granted
	}) or die 'speculative inserts did not reach the index';
	for my $id (1, 2)
	{
		$controller->query_safe("SELECT pg_advisory_unlock($id)");
		$sessions[ $id - 1 ]->query_safe('SELECT 1');
		$sessions[ $id - 1 ]->quit;
	}
	$controller->quit;
	is($node->safe_psql('postgres', 'SELECT count(*) FROM spec'),
		'1', "$am speculative loser discarded");
	$spec_output{$am} = $node->safe_psql(
		'postgres', q{
		SELECT data FROM pg_logical_slot_get_changes('s', NULL, NULL,
			'include-xids', '0', 'skip-empty-xacts', '1')
	});
	$node->safe_psql(
		'postgres', q{
		SELECT pg_drop_replication_slot('s'); DROP TABLE spec; DROP FUNCTION spec_key(int);
	});
}
is($spec_output{flux}, $spec_output{heap},
	'speculative abort decoding matches heap');
$node->stop;
done_testing();
