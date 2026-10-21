
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Check the initial state of the data generated.  Tables for tellers and
# branches use NULL for their filler attribute.  The table accounts uses
# a non-NULL filler.  The history table should have no data.
sub check_data_state
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my $node = shift;
	my $type = shift;

	my $sql_result = $node->safe_psql('postgres',
		'SELECT count(*) AS null_count FROM pgbench_accounts WHERE filler IS NULL LIMIT 10;'
	);
	is($sql_result, '0',
		"$type: filler column of pgbench_accounts has no NULL data");
	$sql_result = $node->safe_psql('postgres',
		'SELECT count(*) AS null_count FROM pgbench_branches WHERE filler IS NULL;'
	);
	is($sql_result, '1',
		"$type: filler column of pgbench_branches has only NULL data");
	$sql_result = $node->safe_psql('postgres',
		'SELECT count(*) AS null_count FROM pgbench_tellers WHERE filler IS NULL;'
	);
	is($sql_result, '10',
		"$type: filler column of pgbench_tellers has only NULL data");
	$sql_result = $node->safe_psql('postgres',
		'SELECT count(*) AS data_count FROM pgbench_history;');
	is($sql_result, '0', "$type: pgbench_history has no data");
}

# start a pgbench specific server
my $node = PostgreSQL::Test::Cluster->new('main');
# Set to untranslated messages, to be able to compare program output with
# expected strings.
$node->init(extra => [ '--locale', 'C' ]);
$node->start;

# tablespace for testing, because partitioned tables cannot use pg_default
# explicitly and we want to test that table creation with tablespace works
# for partitioned tables.
my $ts = $node->basedir . '/regress_pgbench_tap_1_ts_dir';
mkdir $ts or die "cannot create directory $ts";

# the next commands will issue a syntax error if the path contains a "'"
$node->safe_psql('postgres',
	"CREATE TABLESPACE regress_pgbench_tap_1_ts LOCATION '$ts';");

# Test concurrent OID generation via pg_enum_oid_index.  This indirectly
# exercises LWLock and spinlock concurrency.
my $labels = join ',', map { "'l$_'" } 1 .. 1000;
$node->pgbench(
	'--no-vacuum --client=5 --protocol=prepared --transactions=25',
	0,
	[qr{processed: 125/125}],
	[qr{^$}],
	'concurrent OID generation',
	{
		'001_pgbench_concurrent_insert' =>
		  "CREATE TYPE pg_temp.e AS ENUM ($labels); DROP TYPE pg_temp.e;"
	});

# Test inplace updates from VACUUM concurrent with heap_update from GRANT.
# The PROC_IN_VACUUM environment can't finish MVCC table scans consistently,
# so this fails rarely.  To reproduce consistently, add a sleep after
# GetCatalogSnapshot(non-catalog-rel).
Test::More->builder->todo_start('PROC_IN_VACUUM scan breakage');
$node->safe_psql('postgres', 'CREATE TABLE ddl_target ()');
$node->pgbench(
	'--no-vacuum --client=5 --protocol=prepared --transactions=50',
	0,
	[qr{processed: 250/250}],
	[qr{^$}],
	'concurrent GRANT/VACUUM',
	{
		'001_pgbench_grant@9' => q(
			DO $$
			BEGIN
				PERFORM pg_advisory_xact_lock(42);
				FOR i IN 1 .. 10 LOOP
					GRANT SELECT ON ddl_target TO PUBLIC;
					REVOKE SELECT ON ddl_target FROM PUBLIC;
				END LOOP;
			END
			$$;
),
		'001_pgbench_vacuum_ddl_target@1' => "VACUUM ddl_target;",
	});
Test::More->builder->todo_end;

# Trigger various connection errors
$node->pgbench(
	'no-such-database',
	1,
	[qr{^$}],
	[
		qr{connection to server .* failed},
		qr{FATAL:  database "no-such-database" does not exist}
	],
	'no such database');

$node->pgbench(
	'-S -t 1', 1, [],
	[qr{Perhaps you need to do initialization}],
	'run without init');

# Initialize pgbench tables scale 1
$node->pgbench(
	'-i', 0,
	[qr{^$}],
	[
		qr{creating tables},
		qr{vacuuming},
		qr{creating primary keys},
		qr{done in \d+\.\d\d s }
	],
	'pgbench scale 1 initialization',);

# Check data state, after client-side data generation.
check_data_state($node, 'client-side');

# Again, with all possible options
$node->pgbench(
	'--initialize --init-steps=dtpvg --scale=1 --unlogged-tables --fillfactor=98 --foreign-keys --quiet --tablespace=regress_pgbench_tap_1_ts --index-tablespace=regress_pgbench_tap_1_ts --partitions=2 --partition-method=hash',
	0,
	[qr{^$}i],
	[
		qr{dropping old tables},
		qr{creating tables},
		qr{creating 2 partitions},
		qr{vacuuming},
		qr{creating primary keys},
		qr{creating foreign keys},
		qr{(?!vacuuming)},    # no vacuum
		qr{done in \d+\.\d\d s }
	],
	'pgbench scale 1 initialization');

# Test interaction of --init-steps with legacy step-selection options
$node->pgbench(
	'--initialize --init-steps=dtpvGvv --no-vacuum --foreign-keys --unlogged-tables --partitions=3',
	0,
	[qr{^$}],
	[
		qr{dropping old tables},
		qr{creating tables},
		qr{creating 3 partitions},
		qr{creating primary keys},
		qr{generating data \(server-side\)},
		qr{creating foreign keys},
		qr{(?!vacuuming)},    # no vacuum
		qr{done in \d+\.\d\d s }
	],
	'pgbench --init-steps');

# Check data state, after server-side data generation.
check_data_state($node, 'server-side');

# Run all builtin scripts, for a few transactions each
$node->pgbench(
	'--transactions=5 -Dfoo=bla --client=2 --protocol=simple --builtin=t'
	  . ' --connect -n -v -n',
	0,
	[
		qr{builtin: TPC-B},
		qr{clients: 2\b},
		qr{processed: 10/10},
		qr{mode: simple},
		qr{maximum number of tries: 1}
	],
	[qr{^$}],
	'pgbench tpcb-like');

$node->pgbench(
	'--transactions=20 --client=5 -M extended --builtin=si -C --no-vacuum -s 1',
	0,
	[
		qr{builtin: simple update},
		qr{clients: 5\b},
		qr{threads: 1\b},
		qr{processed: 100/100},
		qr{mode: extended}
	],
	[qr{scale option ignored}],
	'pgbench simple update');

$node->pgbench(
	'-t 100 -c 7 -M prepared -b se --debug',
	0,
	[
		qr{builtin: select only},
		qr{clients: 7\b},
		qr{threads: 1\b},
		qr{processed: 700/700},
		qr{mode: prepared}
	],
	[
		qr{vacuum}, qr{client 0}, qr{client 1}, qr{sending},
		qr{receiving}, qr{executing}
	],
	'pgbench select only');

# check if threads are supported
my $nthreads = 2;

{
	my ($stderr);
	run_log([ 'pgbench', '--jobs' => '2', '--bad-option' ], '2>' => \$stderr);
	$nthreads = 1 if $stderr =~ m/threads are not supported on this platform/;
}

# run custom scripts
$node->pgbench(
	"-t 100 -c 1 -j $nthreads -M prepared -n",
	0,
	[
		qr{type: multiple scripts},
		qr{mode: prepared},
		qr{script 1: .*/001_pgbench_custom_script_1},
		qr{weight: 2},
		qr{script 2: .*/001_pgbench_custom_script_2},
		qr{weight: 1},
		qr{processed: 100/100}
	],
	[qr{^$}],
	'pgbench custom scripts',
	{
		'001_pgbench_custom_script_1@1' => q{-- select only
\set aid random(1, :scale * 100000)
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
},
		'001_pgbench_custom_script_2@2' => q{-- special variables
BEGIN;
\set foo 1
-- cast are needed for typing under -M prepared
SELECT :foo::INT + :scale::INT * :client_id::INT AS bla;
COMMIT;
}
	});

$node->pgbench(
	'-n -t 10 -c 1 -M simple',
	0,
	[
		qr{type: .*/001_pgbench_custom_script_3},
		qr{processed: 10/10},
		qr{mode: simple}
	],
	[qr{^$}],
	'pgbench custom script',
	{
		'001_pgbench_custom_script_3' => q{-- select only variant
\set aid random(1, :scale * 100000)
BEGIN;
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
COMMIT;
}
	});

$node->pgbench(
	'-n -t 10 -c 2 -M extended',
	0,
	[
		qr{type: .*/001_pgbench_custom_script_4},
		qr{processed: 20/20},
		qr{mode: extended}
	],
	[qr{^$}],
	'pgbench custom script',
	{
		'001_pgbench_custom_script_4' => q{-- select only variant
\set aid random(1, :scale * 100000)
BEGIN;
SELECT abalance::INTEGER AS balance
  FROM pgbench_accounts
  WHERE aid=:aid;
COMMIT;
}
	});

# Verify server logging of query parameters.
# (This doesn't really belong here, but pgbench is a convenient way
# to issue commands using extended query mode with parameters.)

# 1. Logging neither with errors nor with statements
$node->append_conf('postgresql.conf',
		"log_min_duration_statement = 0\n"
	  . "log_parameter_max_length = 0\n"
	  . "log_parameter_max_length_on_error = 0");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type json},
		qr{(?!unnamed portal with parameters)}
	],
	'server parameter logging',
	{
		'001_param_1' => q[select '{ invalid ' as value \gset
select $$'Valame Dios!' dijo Sancho; 'no le dije yo a vuestra merced que mirase bien lo que hacia?'$$ as long \gset
select column1::jsonb from (values (:value), (:long)) as q;
]
	});
my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
unlike(
	$log,
	qr[DETAIL:  Parameters: \$1 = '\{ invalid ',],
	"no parameters logged");
$log = undef;

# 2. Logging truncated parameters on error, full with statements
$node->append_conf('postgresql.conf',
		"log_parameter_max_length = -1\n"
	  . "log_parameter_max_length_on_error = 64");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  division by zero},
		qr{CONTEXT:  unnamed portal with parameters: \$1 = '1', \$2 = NULL}
	],
	'server parameter logging',
	{
		'001_param_2' => q{select '1' as one \gset
SELECT 1 / (random() / 2)::int, :one::int, :two::int;
}
	});
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type json},
		qr[CONTEXT:  JSON data, line 1: \{ invalid\.\.\.[\r\n]+unnamed portal with parameters: \$1 = '\{ invalid ', \$2 = '''Valame Dios!'' dijo Sancho; ''no le dije yo a vuestra merced que \.\.\.']m
	],
	'server parameter logging',
	{
		'001_param_3' => q[select '{ invalid ' as value \gset
select $$'Valame Dios!' dijo Sancho; 'no le dije yo a vuestra merced que mirase bien lo que hacia?'$$ as long \gset
select column1::jsonb from (values (:value), (:long)) as q;
]
	});
$log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
like(
	$log,
	qr[DETAIL:  Parameters: \$1 = '\{ invalid ', \$2 = '''Valame Dios!'' dijo Sancho; ''no le dije yo a vuestra merced que mirase bien lo que hacia\?'''],
	"parameter report does not truncate");
$log = undef;

# 3. Logging full parameters on error, truncated with statements
$node->append_conf('postgresql.conf',
		"log_min_duration_statement = -1\n"
	  . "log_parameter_max_length = 7\n"
	  . "log_parameter_max_length_on_error = -1");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  division by zero},
		qr{CONTEXT:  unnamed portal with parameters: \$1 = '1', \$2 = NULL}
	],
	'server parameter logging',
	{
		'001_param_4' => q{select '1' as one \gset
SELECT 1 / (random() / 2)::int, :one::int, :two::int;
}
	});

$node->append_conf('postgresql.conf', "log_min_duration_statement = 0");
$node->reload;
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type json},
		qr[CONTEXT:  JSON data, line 1: \{ invalid\.\.\.[\r\n]+unnamed portal with parameters: \$1 = '\{ invalid ', \$2 = '''Valame Dios!'' dijo Sancho; ''no le dije yo a vuestra merced que mirase bien lo que hacia\?']m
	],
	'server parameter logging',
	{
		'001_param_5' => q[select '{ invalid ' as value \gset
select $$'Valame Dios!' dijo Sancho; 'no le dije yo a vuestra merced que mirase bien lo que hacia?'$$ as long \gset
select column1::jsonb from (values (:value), (:long)) as q;
]
	});
$log = PostgreSQL::Test::Utils::slurp_file($node->logfile);
like(
	$log,
	qr[DETAIL:  Parameters: \$1 = '\{ inval\.\.\.', \$2 = '''Valame\.\.\.'],
	"parameter report truncates");
$log = undef;

# Check that bad parameters are reported during typinput phase of BIND
$node->pgbench(
	'-n -t1 -c1 -M prepared',
	2,
	[],
	[
		qr{ERROR:  invalid input syntax for type smallint: "1a"},
		qr{CONTEXT:  unnamed portal parameter \$2 = '1a'}
	],
	'server parameter logging',
	{
		'001_param_6' => q{select 42 as value1, '1a' as value2 \gset
select :value1::smallint, :value2::smallint;
}
	});

# Restore default logging config
$node->append_conf('postgresql.conf',
		"log_min_duration_statement = -1\n"
	  . "log_parameter_max_length_on_error = 0\n"
	  . "log_parameter_max_length = -1");
$node->reload;

# test expressions
$node->pgbench(
	'--random-seed=5432 -t 1 -Dfoo=-10.1 -Dbla=false -Di=+3 -Dn=null -Dt=t -Df=of -Dd=1.0',
	0,
	[ qr{type: .*/001_pgbench_expressions}, qr{processed: 1/1} ],
	[
		qr{setting random seed to 5432\b},

		# After explicit seeding, the four random checks (1-3,20) are
		# deterministic; but see also magic values in checks 111,113.
		qr{command=1.: int 17\b},      # uniform random
		qr{command=2.: int 104\b},     # exponential random
		qr{command=3.: int 1498\b},    # gaussian random
		qr{command=4.: int 4\b},
		qr{command=5.: int 5\b},
		qr{command=6.: int 6\b},
		qr{command=7.: int 7\b},
		qr{command=8.: int 8\b},
		qr{command=9.: int 9\b},
		qr{command=10.: int 10\b},
		qr{command=11.: int 11\b},
		qr{command=12.: int 12\b},
		qr{command=15.: double 15\b},
		qr{command=16.: double 16\b},
		qr{command=17.: double 17\b},
		qr{command=20.: int 3\b},    # zipfian random
		qr{command=21.: double -27\b},
		qr{command=22.: double 1024\b},
		qr{command=23.: double 1\b},
		qr{command=24.: double 1\b},
		qr{command=25.: double -0.125\b},
		qr{command=26.: double -0.125\b},
		qr{command=27.: double -0.00032\b},
		qr{command=28.: double 8.50705917302346e\+0?37\b},
		qr{command=29.: double 1e\+0?30\b},
		qr{command=30.: boolean false\b},
		qr{command=31.: boolean true\b},
		qr{command=32.: int 32\b},
		qr{command=33.: int 33\b},
		qr{command=34.: double 34\b},
		qr{command=35.: int 35\b},
		qr{command=36.: int 36\b},
		qr{command=37.: double 37\b},
		qr{command=38.: int 38\b},
		qr{command=39.: int 39\b},
		qr{command=40.: boolean true\b},
		qr{command=41.: null\b},
		qr{command=42.: null\b},
		qr{command=43.: boolean true\b},
		qr{command=44.: boolean true\b},
		qr{command=45.: boolean true\b},
		qr{command=46.: int 46\b},
		qr{command=47.: boolean true\b},
		qr{command=48.: boolean true\b},
		qr{command=49.: int -5817877081768721676\b},
		qr{command=50.: boolean true\b},
		qr{command=51.: int -7793829335365542153\b},
		qr{command=52.: int -?\d+\b},
		qr{command=53.: boolean true\b},
		qr{command=65.: int 65\b},
		qr{command=74.: int 74\b},
		qr{command=83.: int 83\b},
		qr{command=86.: int 86\b},
		qr{command=93.: int 93\b},
		qr{command=95.: int 0\b},
		qr{command=96.: int 1\b},                       # :scale
		qr{command=97.: int 0\b},                       # :client_id
		qr{command=98.: int 5432\b},                    # :random_seed
		qr{command=99.: int -9223372036854775808\b},    # min int
		qr{command=100.: int 9223372036854775807\b},    # max int
			# pseudorandom permutation tests
		qr{command=101.: boolean true\b},
		qr{command=102.: boolean true\b},
		qr{command=103.: boolean true\b},
		qr{command=104.: boolean true\b},
		qr{command=105.: boolean true\b},
		qr{command=109.: boolean true\b},
		qr{command=110.: boolean true\b},
		qr{command=111.: boolean true\b},
		qr{command=113.: boolean true\b},
	],
	'pgbench expressions',
	{
		'001_pgbench_expressions' => q{-- integer functions
\set i1 debug(random(10, 19))
\set i2 debug(random_exponential(100, 199, 10.0))
\set i3 debug(random_gaussian(1000, 1999, 10.0))
\set i4 debug(abs(-4))
\set i5 debug(greatest(5, 4, 3, 2))
\set i6 debug(11 + least(-5, -4, -3, -2))
\set i7 debug(int(7.3))
-- integer arithmetic and bit-wise operators
\set i8 debug(17 / (4|1) + ( 4 + (7 >> 2)))
\set i9 debug(- (3 * 4 - (-(~ 1) + -(~ 0))) / -1 + 3 % -1)
\set ia debug(10 + (0 + 0 * 0 - 0 / 1))
\set ib debug(:ia + :scale)
\set ic debug(64 % (((2 + 1 * 2 + (1 # 2) | 4 * (2 & 11)) - (1 << 2)) + 2))
-- double functions and operators
\set d1 debug(sqrt(+1.5 * 2.0) * abs(-0.8E1))
\set d2 debug(double(1 + 1) * (-75.0 / :foo))
\set pi debug(pi() * 4.9)
\set d4 debug(greatest(4, 2, -1.17) * 4.0 * Ln(Exp(1.0)))
\set d5 debug(least(-5.18, .0E0, 1.0/0) * -3.3)
-- reset variables
\set i1 0
\set d1 false
-- yet another integer function
\set id debug(random_zipfian(1, 9, 1.3))
--- pow and power
\set poweri debug(pow(-3,3))
\set powerd debug(pow(2.0,10))
\set poweriz debug(pow(0,0))
\set powerdz debug(pow(0.0,0.0))
\set powernegi debug(pow(-2,-3))
\set powernegd debug(pow(-2.0,-3.0))
\set powernegd2 debug(power(-5.0,-5.0))
\set powerov debug(pow(9223372036854775807, 2))
\set powerov2 debug(pow(10,30))
-- comparisons and logical operations
\set c0 debug(1.0 = 0.0 and 1.0 != 0.0)
\set c1 debug(0 = 1 Or 1.0 = 1)
\set c4 debug(case when 0 < 1 then 32 else 0 end)
\set c5 debug(case when true then 33 else 0 end)
\set c6 debug(case when false THEN -1 when 1 = 1 then 13 + 19 + 2.0 end )
\set c7 debug(case when (1 > 0) and (1 >= 0) and (0 < 1) and (0 <= 1) and (0 != 1) and (0 = 0) and (0 <> 1) then 35 else 0 end)
\set c8 debug(CASE \
                WHEN (1.0 > 0.0) AND (1.0 >= 0.0) AND (0.0 < 1.0) AND (0.0 <= 1.0) AND \
                     (0.0 != 1.0) AND (0.0 = 0.0) AND (0.0 <> 1.0) AND (0.0 = 0.0) \
                  THEN 36 \
                  ELSE 0 \
              END)
\set c9 debug(CASE WHEN NOT FALSE THEN 3 * 12.3333334 END)
\set ca debug(case when false then 0 when 1-1 <> 0 then 1 else 38 end)
\set cb debug(10 + mod(13 * 7 + 12, 13) - mod(-19 * 11 - 17, 19))
\set cc debug(NOT (0 > 1) AND (1 <= 1) AND NOT (0 >= 1) AND (0 < 1) AND \
    NOT (false and true) AND (false OR TRUE) AND (NOT :f) AND (NOT FALSE) AND \
    NOT (NOT TRUE))
-- NULL value and associated operators
\set n0 debug(NULL + NULL * exp(NULL))
\set n1 debug(:n0)
\set n2 debug(NOT (:n0 IS NOT NULL OR :d1 IS NULL))
\set n3 debug(:n0 IS NULL AND :d1 IS NOT NULL AND :d1 NOTNULL)
\set n4 debug(:n0 ISNULL AND NOT :n0 IS TRUE AND :n0 IS NOT FALSE)
\set n5 debug(CASE WHEN :n IS NULL THEN 46 ELSE NULL END)
-- use a variables of all types
\set n6 debug(:n IS NULL AND NOT :f AND :t)
-- conditional truth
\set cs debug(CASE WHEN 1 THEN TRUE END AND CASE WHEN 1.0 THEN TRUE END AND CASE WHEN :n THEN NULL ELSE TRUE END)
-- hash functions
\set h0 debug(hash(10, 5432))
\set h1 debug(:h0 = hash_murmur2(10, 5432))
\set h3 debug(hash_fnv1a(10, 5432))
\set h4 debug(hash(10))
\set h5 debug(hash(10) = hash(10, :default_seed))
-- lazy evaluation
\set zy 0
\set yz debug(case when :zy = 0 then -1 else (1 / :zy) end)
\set yz debug(case when :zy = 0 or (1 / :zy) < 0 then -1 else (1 / :zy) end)
\set yz debug(case when :zy > 0 and (1 / :zy) < 0 then (1 / :zy) else 1 end)
-- substitute variables of all possible types
\set v0 NULL
\set v1 TRUE
\set v2 5432
\set v3 -54.21E-2
SELECT :v0, :v1, :v2, :v3;
-- if tests
\set nope 0
\if 1 > 0
\set id debug(65)
\elif 0
\set nope 1
\else
\set nope 1
\endif
\if 1 < 0
\set nope 1
\elif 1 > 0
\set ie debug(74)
\else
\set nope 1
\endif
\if 1 < 0
\set nope 1
\elif 1 < 0
\set nope 1
\else
\set if debug(83)
\endif
\if 1 = 1
\set ig debug(86)
\elif 0
\set nope 1
\endif
\if 1 = 0
\set nope 1
\elif 1 <> 0
\set ih debug(93)
\endif
-- must be zero if false branches where skipped
\set nope debug(:nope)
-- check automatic variables
\set sc debug(:scale)
\set ci debug(:client_id)
\set rs debug(:random_seed)
-- minint constant parsing
\set min debug(-9223372036854775808)
\set max debug(-(:min + 1))
-- parametric pseudorandom permutation function
\set t debug(permute(0, 2) + permute(1, 2) = 1)
\set t debug(permute(0, 3) + permute(1, 3) + permute(2, 3) = 3)
\set t debug(permute(0, 4) + permute(1, 4) + permute(2, 4) + permute(3, 4) = 6)
\set t debug(permute(0, 5) + permute(1, 5) + permute(2, 5) + permute(3, 5) + permute(4, 5) = 10)
\set t debug(permute(0, 16) + permute(1, 16) + permute(2, 16) + permute(3, 16) + \
             permute(4, 16) + permute(5, 16) + permute(6, 16) + permute(7, 16) + \
             permute(8, 16) + permute(9, 16) + permute(10, 16) + permute(11, 16) + \
             permute(12, 16) + permute(13, 16) + permute(14, 16) + permute(15, 16) = 120)
-- random sanity checks
\set size random(2, 1000)
\set v random(0, :size - 1)
\set p permute(:v, :size)
\set t debug(0 <= :p and :p < :size and :p = permute(:v + :size, :size) and :p <> permute(:v + 1, :size))
-- actual values
\set t debug(permute(:v, 1) = 0)
\set t debug(permute(0, 2, 5431) = 0 and permute(1, 2, 5431) = 1 and \
             permute(0, 2, 5433) = 1 and permute(1, 2, 5433) = 0)
-- check permute's portability across architectures
\set size debug(:max - 10)
\set t debug(permute(:size-1, :size, 5432) = 520382784483822430 and \
             permute(:size-2, :size, 5432) = 1143715004660802862 and \
             permute(:size-3, :size, 5432) = 447293596416496998 and \
             permute(:size-4, :size, 5432) = 916527772266572956 and \
             permute(:size-5, :size, 5432) = 2763809008686028849 and \
             permute(:size-6, :size, 5432) = 8648551549198294572 and \
             permute(:size-7, :size, 5432) = 4542876852200565125)
}
	});

# test nested \if constructs
$node->pgbench(
	'--no-vacuum --client=1 --exit-on-abort --transactions=1',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'nested ifs',
	{
		'pgbench_nested_if' => q(
			\if false
				SELECT 1 / 0;
				\if true
					SELECT 1 / 0;
				\elif true
					SELECT 1 / 0;
				\else
					SELECT 1 / 0;
				\endif
				SELECT 1 / 0;
			\elif false
				\if true
					SELECT 1 / 0;
				\elif true
					SELECT 1 / 0;
				\else
					SELECT 1 / 0;
				\endif
			\else
				\if false
					SELECT 1 / 0;
				\elif false
					SELECT 1 / 0;
				\else
					SELECT 'correct';
				\endif
			\endif
			\if true
				SELECT 'correct';
			\else
				\if true
					SELECT 1 / 0;
				\elif true
					SELECT 1 / 0;
				\else
					SELECT 1 / 0;
				\endif
			\endif
		)
	});

# random determinism when seeded
$node->safe_psql('postgres',
	'CREATE UNLOGGED TABLE seeded_random(seed INT8 NOT NULL, rand TEXT NOT NULL, val INTEGER NOT NULL);'
);

# same value to check for determinism
my $seed = int(rand(1000000000));
for my $i (1, 2)
{
	$node->pgbench(
		"--random-seed=$seed -t 1",
		0,
		[qr{processed: 1/1}],
		[qr{setting random seed to $seed\b}],
		"random seeded with $seed",
		{
			"001_pgbench_random_seed_$i" => q{-- test random functions
\set ur random(1000, 1999)
\set er random_exponential(2000, 2999, 2.0)
\set gr random_gaussian(3000, 3999, 3.0)
\set zr random_zipfian(4000, 4999, 1.5)
INSERT INTO seeded_random(seed, rand, val) VALUES
  (:random_seed, 'uniform', :ur),
  (:random_seed, 'exponential', :er),
  (:random_seed, 'gaussian', :gr),
  (:random_seed, 'zipfian', :zr);
}
		});
}

# check that all runs generated the same 4 values
my ($ret, $out, $err) = $node->psql('postgres',
	'SELECT seed, rand, val, COUNT(*) FROM seeded_random GROUP BY seed, rand, val'
);

is($ret, 0, "psql seeded_random count ok");
is($err, '', "psql seeded_random count stderr is empty");
like(
	$out,
	qr/\b$seed\|uniform\|1\d\d\d\|2/,
	"psql seeded_random count uniform");
like(
	$out,
	qr/\b$seed\|exponential\|2\d\d\d\|2/,
	"psql seeded_random count exponential");
like(
	$out,
	qr/\b$seed\|gaussian\|3\d\d\d\|2/,
	"psql seeded_random count gaussian");
like(
	$out,
	qr/\b$seed\|zipfian\|4\d\d\d\|2/,
	"psql seeded_random count zipfian");

$node->safe_psql('postgres', 'DROP TABLE seeded_random;');

# backslash commands
$node->pgbench(
	'-t 1', 0,
	[
		qr{type: .*/001_pgbench_backslash_commands},
		qr{processed: 1/1},
		qr{shell-echo-output}
	],
	[qr{command=8.: int 1\b}],
	'pgbench backslash commands',
	{
		'001_pgbench_backslash_commands' => q{-- run set
\set zero 0
\set one 1.0
-- sleep
\sleep :one ms
\sleep 100 us
\sleep 0 s
\sleep :zero
-- setshell and continuation
\setshell another_one\
  echo \
    :one
\set n debug(:another_one)
-- shell
\shell echo shell-echo-output
}
	});

# working \gset
$node->pgbench(
	'-t 1', 0,
	[ qr{type: .*/001_pgbench_gset}, qr{processed: 1/1} ],
	[
		qr{command=3.: int 0\b},
		qr{command=5.: int 1\b},
		qr{command=6.: int 2\b},
		qr{command=8.: int 3\b},
		qr{command=10.: int 4\b},
		qr{command=12.: int 5\b}
	],
	'pgbench gset command',
	{
		'001_pgbench_gset' => q{-- test gset
-- no columns
SELECT \gset
-- one value
SELECT 0 AS i0 \gset
\set i debug(:i0)
-- two values
SELECT 1 AS i1, 2 AS i2 \gset
\set i debug(:i1)
\set i debug(:i2)
-- with prefix
SELECT 3 AS i3 \gset x_
\set i debug(:x_i3)
-- overwrite existing variable
SELECT 0 AS i4, 4 AS i4 \gset
\set i debug(:i4)
-- work on the last SQL command under \;
\; \; SELECT 0 AS i5 \; SELECT 5 AS i5 \; \; \gset
\set i debug(:i5)
}
	});
# \gset cannot accept more than one row, causing command to fail.
$node->pgbench(
	'-t 1', 2,
	[ qr{type: .*/001_pgbench_gset_two_rows}, qr{processed: 0/1} ],
	[qr{expected one row, got 2\b}],
	'pgbench gset command with two rows',
	{
		'001_pgbench_gset_two_rows' => q{
SELECT 5432 AS fail UNION SELECT 5433 ORDER BY 1 \gset
}
	});

# \gset stores a SQL NULL as the null value.
$node->pgbench(
	'-t 1', 0,
	[ qr{type: .*/001_pgbench_gset_null}, qr{processed: 1/1} ],
	[ qr{command=2.: null\b}, qr{command=3.: boolean true\b} ],
	'pgbench gset command with NULL',
	{
		'001_pgbench_gset_null' => q{-- NULL is stored as the null value
SELECT NULL AS nv \gset
\set i debug(:nv)
\set i debug(:nv IS NULL)
}
	});

# working \aset
# Valid cases.
$node->pgbench(
	'-t 1', 0,
	[ qr{type: .*/001_pgbench_aset}, qr{processed: 1/1} ],
	[ qr{command=3.: int 8\b}, qr{command=4.: int 7\b} ],
	'pgbench aset command',
	{
		'001_pgbench_aset' => q{
-- test aset, which applies to a combined query
\; SELECT 6 AS i6 \; SELECT 7 AS i7 \; \aset
-- unless it returns more than one row, last is kept
SELECT 8 AS i6 UNION SELECT 9 ORDER BY 1 DESC \aset
\set i debug(:i6)
\set i debug(:i7)
}
	});
# Empty result set with \aset, causing command to fail.
$node->pgbench(
	'-t 1', 2,
	[ qr{type: .*/001_pgbench_aset_empty}, qr{processed: 0/1} ],
	[
		qr{undefined variable \"i8\"},
		qr{evaluation of meta-command failed\b}
	],
	'pgbench aset command with empty result',
	{
		'001_pgbench_aset_empty' => q{
-- empty result
\; SELECT 5432 AS i8 WHERE FALSE \; \aset
\set i debug(:i8)
}
	});

# Working \startpipeline
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[ qr{type: .*/001_pgbench_pipeline}, qr{actually processed: 1/1} ],
	[],
	'working \startpipeline',
	{
		'001_pgbench_pipeline' => q{
-- test startpipeline
\startpipeline
} . "select 1;\n" x 10 . q{
\endpipeline
}
	});

# Working \startpipeline with \syncpipeline
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[ qr{type: .*/001_pgbench_pipeline_sync}, qr{actually processed: 1/1} ],
	[],
	'working \startpipeline with \syncpipeline',
	{
		'001_pgbench_pipeline_sync' => q{
-- test startpipeline
\startpipeline
select 1;
\syncpipeline
\syncpipeline
select 2;
\syncpipeline
select 3;
\endpipeline
}
	});

# Working \startpipeline in prepared query mode
$node->pgbench(
	'-t 1 -n -M prepared',
	0,
	[ qr{type: .*/001_pgbench_pipeline_prep}, qr{actually processed: 1/1} ],
	[],
	'working \startpipeline',
	{
		'001_pgbench_pipeline_prep' => q{
-- test startpipeline
\startpipeline
\endpipeline
\startpipeline
} . "select 1;\n" x 10 . q{
\endpipeline
}
	});

# Try \startpipeline twice
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[qr{already in pipeline mode}],
	'error: call \startpipeline twice',
	{
		'001_pgbench_pipeline_2' => q{
-- startpipeline twice
\startpipeline
\startpipeline
}
	});

# Try to end a pipeline that hasn't started
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[qr{not in pipeline mode}],
	'error: \endpipeline with no start',
	{
		'001_pgbench_pipeline_3' => q{
-- pipeline not started
\endpipeline
}
	});

# Try \gset in pipeline mode
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[qr{gset is not allowed in pipeline mode}],
	'error: \gset not allowed in pipeline mode',
	{
		'001_pgbench_pipeline_4' => q{
\startpipeline
select 1 \gset f
\endpipeline
}
	});

# Try \startpipeline without \endpipeline in a single transaction
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[qr{end of script reached with pipeline open}],
	'error: call \startpipeline without \endpipeline in a single transaction',
	{
		'001_pgbench_pipeline_5' => q{
-- startpipeline only with single transaction
\startpipeline
}
	});

# Try \startpipeline without \endpipeline
$node->pgbench(
	'-t 2 -n -M extended',
	2,
	[],
	[qr{end of script reached with pipeline open}],
	'error: call \startpipeline without \endpipeline',
	{
		'001_pgbench_pipeline_6' => q{
-- startpipeline only
\startpipeline
}
	});

# Try \startpipeline with \syncpipeline without \endpipeline
$node->pgbench(
	'-t 2 -n -M extended',
	2,
	[],
	[qr{end of script reached with pipeline open}],
	'error: call \startpipeline and \syncpipeline without \endpipeline',
	{
		'001_pgbench_pipeline_7' => q{
-- startpipeline with \syncpipeline only
\startpipeline
\syncpipeline
}
	});

# Try SET LOCAL as first pipeline command.  This succeeds and the first
# command is not executed inside an implicit transaction block, causing
# a WARNING.
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[],
	[qr{WARNING:  SET LOCAL can only be used in transaction blocks}],
	'SET LOCAL outside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_set_local_1' => q{
\startpipeline
SET LOCAL statement_timeout='1h';
\endpipeline
}
	});

# Try SET LOCAL as second pipeline command.  This succeeds and the second
# command does not cause a WARNING to be generated.
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[],
	[qr{^$}],
	'SET LOCAL inside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_set_local_2' => q{
\startpipeline
SELECT 1;
SET LOCAL statement_timeout='1h';
\endpipeline
}
	});

# Try SET LOCAL with \syncpipeline.  This succeeds and the command
# launched after the sync is outside the implicit transaction block
# of the pipeline, causing a WARNING.
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[],
	[qr{WARNING:  SET LOCAL can only be used in transaction blocks}],
	'SET LOCAL and \syncpipeline',
	{
		'001_pgbench_pipeline_set_local_3' => q{
\startpipeline
SELECT 1;
\syncpipeline
SET LOCAL statement_timeout='1h';
\endpipeline
}
	});

# Try REINDEX CONCURRENTLY as first pipeline command.  This succeeds
# as the first command is outside the implicit transaction block of
# a pipeline.
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[],
	[],
	'REINDEX CONCURRENTLY outside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_reindex_1' => q{
\startpipeline
REINDEX TABLE CONCURRENTLY pgbench_accounts;
SELECT 1;
\endpipeline
}
	});

# Try REINDEX CONCURRENTLY as second pipeline command.  This fails
# as the second command is inside an implicit transaction block.
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[],
	'error: REINDEX CONCURRENTLY inside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_reindex_2' => q{
\startpipeline
SELECT 1;
REINDEX TABLE CONCURRENTLY pgbench_accounts;
\endpipeline
}
	});

# Try VACUUM as first pipeline command.  Like REINDEX CONCURRENTLY, this
# succeeds as this is outside the implicit transaction block of a pipeline.
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[],
	[],
	'VACUUM outside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_vacuum_1' => q{
\startpipeline
VACUUM pgbench_accounts;
\endpipeline
}
	});

# Try VACUUM as second pipeline command.  This fails, as the second command
# of a pipeline is inside an implicit transaction block.
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[],
	'error: VACUUM inside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_vacuum_2' => q{
\startpipeline
SELECT 1;
VACUUM pgbench_accounts;
\endpipeline
}
	});

# Try subtransactions in a pipeline.  These are forbidden in implicit
# transaction blocks.
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[],
	'error: subtransactions not allowed in pipeline',
	{
		'001_pgbench_pipeline_subtrans' => q{
\startpipeline
SAVEPOINT a;
SELECT 1;
ROLLBACK TO SAVEPOINT a;
SELECT 2;
\endpipeline
}
	});

# Try LOCK TABLE as first pipeline command.  This fails as LOCK is outside
# an implicit transaction block.
$node->pgbench(
	'-t 1 -n -M extended',
	2,
	[],
	[],
	'error: LOCK TABLE outside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_lock_1' => q{
\startpipeline
LOCK pgbench_accounts;
SELECT 1;
\endpipeline
}
	});

# Try LOCK TABLE as second pipeline command.  This succeeds as LOCK is inside
# an implicit transaction block.
$node->pgbench(
	'-t 1 -n -M extended',
	0,
	[],
	[],
	'LOCK TABLE inside implicit transaction block of pipeline',
	{
		'001_pgbench_pipeline_lock_2' => q{
\startpipeline
SELECT 1;
LOCK pgbench_accounts;
\endpipeline
}
	});

# Working \startpipeline in prepared query mode with serializable
$node->pgbench(
	'-c4 -t 10 -n -M prepared',
	0,
	[
		qr{type: .*/001_pgbench_pipeline_serializable},
		qr{actually processed: (\d+)/\1}
	],
	[],
	'working \startpipeline with serializable',
	{
		'001_pgbench_pipeline_serializable' => q{
-- test startpipeline with serializable
\startpipeline
BEGIN ISOLATION LEVEL SERIALIZABLE;
} . "select 1;\n" x 10 . q{
END;
\endpipeline
}
	});

# trigger many expression errors
my @errors = (

	# [ test name, expected status, expected stderr, script ]
	# SQL
	[
		'sql syntax error',
		2,
		[
			qr{ERROR:  syntax error},
			qr{prepared statement .* does not exist}
		],
		q{-- SQL syntax error
    SELECT 1 + ;
}
	],
	[
		'sql too many args', 1,
		[qr{statement has too many arguments.*\b255\b}],
		q{-- MAX_ARGS=256 for prepared
\set i 0
SELECT LEAST(} . join(', ', (':i') x 256) . q{)}
	],

	# SHELL
	[
		'shell bad command', 2,
		[qr{\(shell\) .* meta-command failed}], q{\shell no-such-command}
	],
	[
		'shell undefined variable', 2,
		[qr{undefined variable ":nosuchvariable"}],
		q{-- undefined variable in shell
\shell echo ::foo :nosuchvariable
}
	],
	[ 'shell missing command', 1, [qr{missing command }], q{\shell} ],
	[
		'shell too many args', 1, [qr{too many arguments in command "shell"}],
		q{-- 256 arguments to \shell
\shell echo } . join(' ', ('arg') x 255)
	],

	# SET
	[
		'set syntax error', 1,
		[qr{syntax error in command "set"}], q{\set i 1 +}
	],
	[
		'set no such function', 1,
		[qr{unexpected function name}], q{\set i noSuchFunction()}
	],
	[
		'set invalid variable name', 2,
		[qr{invalid variable name}], q{\set . 1}
	],
	[ 'set division by zero', 2, [qr{division by zero}], q{\set i 1/0} ],
	[
		'set undefined variable',
		2,
		[qr{undefined variable "nosuchvariable"}],
		q{\set i :nosuchvariable}
	],
	[ 'set unexpected char', 1, [qr{unexpected character .;.}], q{\set i ;} ],
	[
		'set too many args',
		2,
		[qr{too many function arguments}],
		q{\set i least(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16)}
	],
	[
		'set empty random range', 2,
		[qr{empty range given to random}], q{\set i random(5,3)}
	],
	[
		'set random range too large', 2,
		[qr{random range is too large}], q{\set i random(:minint, :maxint)}
	],
	[
		'set gaussian param too small',
		2,
		[qr{gaussian param.* at least 2}],
		q{\set i random_gaussian(0, 10, 1.0)}
	],
	[
		'set exponential param greater 0',
		2,
		[qr{exponential parameter must be greater }],
		q{\set i random_exponential(0, 10, 0.0)}
	],
	[
		'set zipfian param to 1',
		2,
		[qr{zipfian parameter must be in range \[1\.001, 1000\]}],
		q{\set i random_zipfian(0, 10, 1)}
	],
	[
		'set zipfian param too large',
		2,
		[qr{zipfian parameter must be in range \[1\.001, 1000\]}],
		q{\set i random_zipfian(0, 10, 1000000)}
	],
	[
		'set non numeric value', 2,
		[qr{malformed variable "foo" value: "bla"}], q{\set i :foo + 1}
	],
	[ 'set no expression', 1, [qr{syntax error}], q{\set i} ],
	[ 'set missing argument', 1, [qr{missing argument}i], q{\set} ],
	[
		'set not a bool', 2,
		[qr{cannot coerce double to boolean}], q{\set b NOT 0.0}
	],
	[
		'set not an int', 2,
		[qr{cannot coerce boolean to int}], q{\set i TRUE + 2}
	],
	[
		'set not a double', 2,
		[qr{cannot coerce boolean to double}], q{\set d ln(TRUE)}
	],
	[
		'set case error',
		1,
		[qr{syntax error in command "set"}],
		q{\set i CASE TRUE THEN 1 ELSE 0 END}
	],
	[
		'set random error', 2,
		[qr{cannot coerce boolean to int}], q{\set b random(FALSE, TRUE)}
	],
	[
		'set number of args mismatch', 1,
		[qr{unexpected number of arguments}], q{\set d ln(1.0, 2.0))}
	],
	[
		'set at least one arg', 1,
		[qr{at least one argument expected}], q{\set i greatest())}
	],

	# SET: ARITHMETIC OVERFLOW DETECTION
	[
		'set double to int overflow', 2,
		[qr{double to int overflow for 100}], q{\set i int(1E32)}
	],
	[
		'set bigint add overflow', 2,
		[qr{int add out}], q{\set i (1<<62) + (1<<62)}
	],
	[
		'set bigint sub overflow',
		2, [qr{int sub out}], q{\set i 0 - (1<<62) - (1<<62) - (1<<62)}
	],
	[
		'set bigint mul overflow', 2,
		[qr{int mul out}], q{\set i 2 * (1<<62)}
	],
	[
		'set bigint div out of range', 2,
		[qr{bigint div out of range}], q{\set i :minint / -1}
	],

	# SETSHELL
	[
		'setshell not an int', 2,
		[qr{command must return an integer}], q{\setshell i echo -n one}
	],
	[ 'setshell missing arg', 1, [qr{missing argument }], q{\setshell var} ],
	[
		'setshell no such command', 2,
		[qr{could not read result }], q{\setshell var no-such-command}
	],

	# SLEEP
	[
		'sleep undefined variable', 2,
		[qr{sleep: undefined variable}], q{\sleep :nosuchvariable}
	],
	[
		'sleep too many args', 1,
		[qr{too many arguments}], q{\sleep too many args}
	],
	[
		'sleep missing arg', 1,
		[ qr{missing argument}, qr{\\sleep} ], q{\sleep}
	],
	[
		'sleep unknown unit', 1,
		[qr{unrecognized time unit}], q{\sleep 1 week}
	],

	# MISC
	[
		'misc invalid backslash command', 1,
		[qr{invalid command .* "nosuchcommand"}], q{\nosuchcommand}
	],
	[ 'misc empty script', 1, [qr{empty command list for script}], q{} ],
	[
		'bad boolean', 2,
		[qr{malformed variable.*trueXXX}], q{\set b :badtrue or true}
	],
	[
		'invalid permute size',
		2,
		[qr{permute size parameter must be greater than zero}],
		q{\set i permute(0, 0)}
	],

	# GSET
	[
		'gset no row', 2,
		[qr{expected one row, got 0\b}], q{SELECT WHERE FALSE \gset}
	],
	[ 'gset alone', 1, [qr{gset must follow an SQL command}], q{\gset} ],
	[
		'gset no SQL', 1,
		[qr{gset must follow an SQL command}], q{\set i +1
\gset}
	],
	[
		'gset too many arguments', 1,
		[qr{too many arguments}], q{SELECT 1 \gset a b}
	],
	[
		'gset after gset', 1,
		[qr{gset must follow an SQL command}], q{SELECT 1 AS i \gset
\gset}
	],
	[
		'gset non SELECT',
		2,
		[qr{expected one row, got 0}],
		q{DROP TABLE IF EXISTS no_such_table \gset}
	],
	[
		'gset bad default name', 2,
		[qr{error storing into variable \?column\?}], q{SELECT 1 \gset}
	],
	[
		'gset bad name',
		2,
		[qr{error storing into variable bad name!}],
		q{SELECT 1 AS "bad name!" \gset}
	],);

for my $e (@errors)
{
	my ($name, $status, $re, $script, $no_prepare) = @$e;
	$status != 0 or die "invalid expected status for test \"$name\"";
	my $n = '001_pgbench_error_' . $name;
	$n =~ s/ /_/g;
	$node->pgbench(
		'-n -t 1 -Dfoo=bla -Dnull=null -Dtrue=true -Done=1 -Dzero=0.0 -Dbadtrue=trueXXX'
		  . ' -Dmaxint=9223372036854775807 -Dminint=-9223372036854775808'
		  . ($no_prepare ? '' : ' -M prepared'),
		$status,
		[ $status == 1 ? qr{^$} : qr{processed: 0/1} ],
		$re,
		'pgbench script error: ' . $name,
		{ $n => $script });
}

# throttling
$node->pgbench(
	'-t 100 -S --rate=100000 --latency-limit=1000000 -c 2 -n -r',
	0,
	[ qr{processed: 200/200}, qr{builtin: select only} ],
	[qr{^$}],
	'pgbench throttling');

$node->pgbench(

	# given the expected rate and the 2 ms tx duration, at most one is executed
	'-t 10 --rate=100000 --latency-limit=1 -n -r',
	0,
	[
		qr{processed: [01]/10},
		qr{type: .*/001_pgbench_sleep},
		qr{above the 1.0 ms latency limit: [01]/}
	],
	[qr{^$}],
	'pgbench late throttling',
	{ '001_pgbench_sleep' => q{\sleep 2ms} });

# return a list of files from directory $dir matching regexpr $re
# this works around glob portability and escaping issues
sub list_files
{
	my ($dir, $re) = @_;
	opendir my $dh, $dir or die "cannot opendir $dir: $!";
	my @files = grep /$re/, readdir $dh;
	closedir $dh or die "cannot closedir $dir: $!";
	return map { $dir . '/' . $_ } @files;
}

# Check log contents and clean them up:
#   $dir: directory holding logs
#   $prefix: file prefix for per-thread logs
#   $nb: number of expected files
#   $min/$max: minimum and maximum number of lines in log files
#   $re: regular expression each log line has to match
sub check_pgbench_logs
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($dir, $prefix, $nb, $min, $max, $re) = @_;

	# $prefix is simple enough, thus does not need escaping
	my @logs = list_files($dir, qr{^$prefix\..*$});
	is(scalar(@logs), $nb, "number of log files");
	is(scalar(grep(/\/$prefix\.\d+(\.\d+)?$/, @logs)),
		$nb, "file name format");

	my $log_number = 0;
	for my $log (sort @logs)
	{
		# Check the contents of each log file.
		my $contents_raw = slurp_file($log);

		my @contents = split(/\n/, $contents_raw);
		my $clen = @contents;
		cmp_ok($clen, '>=', $min,
			"transaction count for $log ($clen) is above min");
		cmp_ok($clen, '<=', $max,
			"transaction count for $log ($clen) is below max");
		my $clen_match = grep(/$re/, @contents);
		is($clen_match, $clen, "transaction format for $prefix");

		# Show more information if some logs don't match
		# to help with debugging.
		if ($clen_match != $clen)
		{
			foreach my $log (@contents)
			{
				print "# Log entry not matching: $log\n"
				  unless $log =~ /$re/;
			}
		}
	}
	return;
}

my $bdir = $node->basedir;

# Run with sampling rate, 2 clients with 50 transactions each.
$node->pgbench(
	"-n -S -t 50 -c 2 --log --sampling-rate=0.5", 0,
	[ qr{select only}, qr{processed: 100/100} ], [qr{^$}],
	'pgbench logs', undef,
	"--log-prefix=$bdir/001_pgbench_log_2");
# The IDs of the clients (1st field) in the logs should be either 0 or 1.
check_pgbench_logs($bdir, '001_pgbench_log_2', 1, 8, 92,
	qr{^[01] \d{1,2} \d+ \d \d+ \d+$});

# Run with different read-only option pattern, 1 client with 10 transactions.
$node->pgbench(
	"-n -b select-only -t 10 -l", 0,
	[ qr{select only}, qr{processed: 10/10} ], [qr{^$}],
	'pgbench logs contents', undef,
	"--log-prefix=$bdir/001_pgbench_log_3");
# The ID of a single client (1st field) should match 0.
check_pgbench_logs($bdir, '001_pgbench_log_3', 1, 10, 10,
	qr{^0 \d{1,2} \d+ \d \d+ \d+$});

# abortion of the client if the script contains an incomplete transaction block
$node->pgbench(
	'--no-vacuum',
	2,
	[qr{processed: 1/10}],
	[
		qr{client 0 aborted: end of script reached without completing the last transaction}
	],
	'incomplete transaction block',
	{ '001_pgbench_incomplete_transaction_block' => q{BEGIN;SELECT 1;} });

# Test the concurrent update in the table row and deadlocks.

$node->safe_psql('postgres',
		'CREATE UNLOGGED TABLE first_client_table (value integer); '
	  . 'CREATE UNLOGGED TABLE xy (x integer, y integer); '
	  . 'INSERT INTO xy VALUES (1, 2);');

# Serialization error and retry

local $ENV{PGOPTIONS} = "-c default_transaction_isolation=repeatable\\ read";

# Check that we have a serialization error and the same random value of the
# delta variable in the next try
my $err_pattern =
	"(client (0|1) sending UPDATE xy SET y = y \\+ -?\\d+\\b).*"
  . "client \\2 got an error in command 3 \\(SQL\\) of script 0; "
  . "ERROR:  could not serialize access due to concurrent update\\b.*"
  . "\\1";

$node->pgbench(
	"-n -c 2 -t 1 --debug --verbose-errors --max-tries 2",
	0,
	[
		qr{processed: 2/2\b},
		qr{number of transactions retried: 1\b},
		qr{total number of retries: 1\b}
	],
	[qr/$err_pattern/s],
	'concurrent update with retrying',
	{
		'001_pgbench_serialization' => q{
-- What's happening:
-- The first client starts the transaction with the isolation level Repeatable
-- Read:
--
-- BEGIN;
-- UPDATE xy SET y = ... WHERE x = 1;
--
-- The second client starts a similar transaction with the same isolation level:
--
-- BEGIN;
-- UPDATE xy SET y = ... WHERE x = 1;
-- <waiting for the first client>
--
-- The first client commits its transaction, and the second client gets a
-- serialization error.

\set delta random(-5000, 5000)

-- The second client will stop here
SELECT pg_advisory_lock(0);

-- Start transaction with concurrent update
BEGIN;
UPDATE xy SET y = y + :delta WHERE x = 1 AND pg_advisory_lock(1) IS NOT NULL;

-- Wait for the second client
DO $$
DECLARE
  exists boolean;
  waiters integer;
BEGIN
  -- The second client always comes in second, and the number of rows in the
  -- table first_client_table reflect this. Here the first client inserts a row,
  -- so the second client will see a non-empty table when repeating the
  -- transaction after the serialization error.
  SELECT EXISTS (SELECT * FROM first_client_table) INTO STRICT exists;
  IF NOT exists THEN
	-- Let the second client begin
	PERFORM pg_advisory_unlock(0);
	-- And wait until the second client tries to get the same lock
	LOOP
	  SELECT COUNT(*) INTO STRICT waiters FROM pg_locks WHERE
	  locktype = 'advisory' AND objsubid = 1 AND
	  ((classid::bigint << 32) | objid::bigint = 1::bigint) AND NOT granted;
	  IF waiters = 1 THEN
		INSERT INTO first_client_table VALUES (1);

		-- Exit loop
		EXIT;
	  END IF;
	END LOOP;
  END IF;
END$$;

COMMIT;
SELECT pg_advisory_unlock_all();
}
	});

# Clean up

$node->safe_psql('postgres', 'DELETE FROM first_client_table;');

local $ENV{PGOPTIONS} = "-c default_transaction_isolation=read\\ committed";

# Deadlock error and retry

# Check that we have a deadlock error
$err_pattern =
	"client (0|1) got an error in command (3|5) \\(SQL\\) of script 0; "
  . "ERROR:  deadlock detected\\b";

$node->pgbench(
	"-n -c 2 -t 1 --max-tries 2 --verbose-errors",
	0,
	[
		qr{processed: 2/2\b},
		qr{number of transactions retried: 1\b},
		qr{total number of retries: 1\b}
	],
	[qr{$err_pattern}],
	'deadlock with retrying',
	{
		'001_pgbench_deadlock' => q{
-- What's happening:
-- The first client gets the lock 2.
-- The second client gets the lock 3 and tries to get the lock 2.
-- The first client tries to get the lock 3 and one of them gets a deadlock
-- error.
--
-- A client that does not get a deadlock error must hold a lock at the
-- transaction start. Thus in the end it releases all of its locks before the
-- client with the deadlock error starts a retry (we do not want any errors
-- again).

-- Since the client with the deadlock error has not released the blocking locks,
-- let's do this here.
SELECT pg_advisory_unlock_all();

-- The second client and the client with the deadlock error stop here
SELECT pg_advisory_lock(0);
SELECT pg_advisory_lock(1);

-- The second client and the client with the deadlock error always come after
-- the first and the number of rows in the table first_client_table reflects
-- this. Here the first client inserts a row, so in the future the table is
-- always non-empty.
DO $$
DECLARE
  exists boolean;
BEGIN
  SELECT EXISTS (SELECT * FROM first_client_table) INTO STRICT exists;
  IF exists THEN
	-- We are the second client or the client with the deadlock error

	-- The first client will take care by itself of this lock (see below)
	PERFORM pg_advisory_unlock(0);

	PERFORM pg_advisory_lock(3);

	-- The second client can get a deadlock here
	PERFORM pg_advisory_lock(2);
  ELSE
	-- We are the first client

	-- This code should not be used in a new transaction after an error
	INSERT INTO first_client_table VALUES (1);

	PERFORM pg_advisory_lock(2);
  END IF;
END$$;

DO $$
DECLARE
  num_rows integer;
  waiters integer;
BEGIN
  -- Check if we are the first client
  SELECT COUNT(*) FROM first_client_table INTO STRICT num_rows;
  IF num_rows = 1 THEN
	-- This code should not be used in a new transaction after an error
	INSERT INTO first_client_table VALUES (2);

	-- Let the second client begin
	PERFORM pg_advisory_unlock(0);
	PERFORM pg_advisory_unlock(1);

	-- Make sure the second client is ready for deadlock
	LOOP
	  SELECT COUNT(*) INTO STRICT waiters FROM pg_locks WHERE
	  locktype = 'advisory' AND
	  objsubid = 1 AND
	  ((classid::bigint << 32) | objid::bigint = 2::bigint) AND
	  NOT granted;

	  IF waiters = 1 THEN
	    -- Exit loop
		EXIT;
	  END IF;
	END LOOP;

	PERFORM pg_advisory_lock(0);
    -- And the second client took care by itself of the lock 1
  END IF;
END$$;

-- The first client can get a deadlock here
SELECT pg_advisory_lock(3);

SELECT pg_advisory_unlock_all();
}
	});

# Clean up
$node->safe_psql('postgres', 'DROP TABLE first_client_table, xy;');

# Test --exit-on-abort
$node->safe_psql('postgres',
	'CREATE TABLE counter(i int); ' . 'INSERT INTO counter VALUES (0);');

$node->pgbench(
	'-t 10 -c 2 -j 2 --exit-on-abort',
	2,
	[],
	[ qr{division by zero}, qr{Run was aborted due to an error in thread} ],
	'test --exit-on-abort',
	{
		'001_exit_on_abort' => q{
update counter set i = i+1 returning i \gset
\if :i = 5
\set y 1/0
\endif
}
	});

# Test copy in pgbench
$node->pgbench(
	'-t 10', 2,
	[],
	[qr{COPY is not supported in pgbench, aborting}],
	'Test copy in script',
	{
		'001_copy' => q{ COPY pgbench_accounts FROM stdin }
	});

# Clean up
$node->safe_psql('postgres', 'DROP TABLE counter;');

# Test --continue-on-error
$node->safe_psql('postgres', 'CREATE TABLE unique_table(i int unique);');

$node->pgbench(
	'-n -t 10 --continue-on-error --failures-detailed',
	0,
	[ qr{processed: 1/10\b}, qr{other failures: 9\b} ],
	[],
	'test --continue-on-error',
	{
		'001_continue_on_error' => q{
		INSERT INTO unique_table VALUES(0);
		}
	});

# Clean up
$node->safe_psql('postgres', 'DROP TABLE unique_table;');

# Test extended PgBenchValue types (text, timestamp, array, subscripting)
$node->pgbench(
	'-n -t 1 -Darr={10,20,30}',
	0,
	[qr{processed: 1/1}],
	[
		qr{command=2.: text 'hello'world'},
		qr{command=4.: boolean true\b},
		qr{command=6.: int 20\b},
		qr{command=9.: int 99\b},
		qr{command=10.: array \{10,99,30\}}
	],
	'pgbench extended value types (text, array, subscripting)',
	{
		'001_pgbench_extended_types' => q{
\set s 'hello''world'
\set d debug(:s)
\set cmp ('abc' < 'def' and :s = 'hello''world')
\set d debug(:cmp)
\set elem :arr[1]
\set d debug(:elem)
\set idx 2
\set arr[:idx - 1] 99
\set d debug(:arr[1])
\set d debug(:arr)
SELECT :s, ':s', :arr::int[];
}
	});

# Test TPC, string, date/timestamp, and array functions
$node->pgbench(
	'-n -t 1',
	0,
	[qr{processed: 1/1}],
	[
		qr{command=2.: boolean true\b},
		qr{command=4.: text 'BARPRESATION'},
		qr{command=6.: text 'CUST_000042_BAR'},
		qr{command=8.: timestamp 1998-09-02\b},
		qr{command=10.: timestamp 1999-01-01\b},
		qr{command=12.: array \{1,2,3\}},
		qr{command=14.: int 7\b}
	],
	'pgbench TPC/string/date/array functions',
	{
		'001_pgbench_tpc_funcs' => q{
\set nu (nurand(255, 0, 999) >= 0 and nurand(255, 0, 999, 42) <= 999)
\set d debug(:nu)
\set cl c_last(48)
\set d debug(:cl)
\set fs format_string('CUST_%06d_%s', 42, 'BAR')
\set d debug(:fs)
\set q1d int_to_date('1998-12-01', -90, 'day')
\set d debug(:q1d)
\set wind int_to_date(to_timestamp('1992-01-01'), 10000, 10000, '7 years')
\set d debug(:wind)
\set a array(1, 2, 3)
\set d debug(:a)
\set ag array_generate(4, 7)
\set d debug(:ag[3])
\set as random_astring(5, 10)
\set ns random_nstring(4, 4)
SELECT :cl, :fs, :q1d::date, :a::int[];
}
	});

$node->pgbench(
	'-n -t 1',
	2,
	[qr{processed: 0/1}],
	[qr{invalid input syntax for type timestamp: "2026-02-30"}],
	'pgbench invalid calendar date in to_timestamp',
	{
		'001_pgbench_bad_date' => q{
\set bad to_timestamp('2026-02-30')
}
	});

# Test control flow (\for, \while, \try, \catch, \expect_error, expected_rollbacks)
$node->pgbench(
	'-n -t 1',
	0,
	[
		qr{processed: 1/1},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{number of expected rollbacks: 3\b}
	],
	[
		qr{command=8.: int 15\b},
		qr{command=15.: int 6\b},
		qr{command=24.: int 1\b},
		qr{command=34.: int 2\b}
	],
	'pgbench loops (\for, \while) and exception handling (\try, \catch, \expect_error)',
	{
		'001_pgbench_control_flow' => q{
\set sum 0
\for i 1 .. 5 BY 1
\set sum :sum + :i
\endfor
\for k 5 .. 1 BY 1
\set sum :sum + 100
\endfor
\set d debug(:sum)
\set wsum 0
\set j 3
\while :j > 0
\set wsum :wsum + :j
\set j :j - 1
\endwhile
\set d debug(:wsum)
\set caught 0
\try
SELECT 1/0;
\set caught -1
\catch '22012'
\set caught :caught + 1
ROLLBACK;
\endtry
\set d debug(:caught)
BEGIN;
\try
SELECT 1/0;
\catch
\set caught :caught + 1
ROLLBACK;
\endtry
\expect_error '22012'
SELECT 1/0;
\set d debug(:caught)
SELECT 1;
}
	});

$node->pgbench(
	'-n -t 1 -M extended',
	0,
	[
		qr{processed: 1/1},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{number of expected rollbacks: 1\b}
	],
	[qr{^$}],
	'pgbench \expect_error inside pipeline',
	{
		'001_pgbench_pipeline_expect_error' => q{
\startpipeline
\expect_error '22012'
SELECT 1/0;
\endpipeline
SELECT 1;
}
	});

# Test \gset_meta and array column parsing in \gset / \aset
$node->pgbench(
	'-n -t 1',
	0,
	[
		qr{processed: 1/1},
		qr{number of expected rollbacks: 2\b}
	],
	[
		qr{command=2.: array \{10,20,30\}},
		qr{command=3.: int 20\b},
		qr{command=4.: text 'hello'},
		qr{command=6.: text 'OK'},
		qr{command=7.: text '00000'},
		qr{command=8.: int 2\b},
		qr{command=11.: text 'ERROR'},
		qr{command=12.: text '22012'},
		qr{command=13.: int 0\b},
		qr{command=20.: text 'ROLLBACK'},
		qr{command=21.: text '22012'},
		qr{command=22.: int 0\b}
	],
	'pgbench gset_meta and array column parsing in gset',
	{
		'001_pgbench_gset_meta' => q{
SELECT ARRAY[10, 20, 30] AS arr, 'hello'::text AS msg \gset p
\set d debug(:p_arr)
\set d debug(:p_arr[1])
\set d debug(:p_msg)
SELECT 1 AS a UNION ALL SELECT 2 AS a \gset_meta q
\set d debug(:q_status)
\set d debug(:q_sqlstate)
\set d debug(:q_nrows)
\expect_error '22012'
SELECT 1/0 \gset_meta err
\set d debug(:err_status)
\set d debug(:err_sqlstate)
\set d debug(:err_nrows)
BEGIN;
\try
SELECT 1/0;
\catch '22012'
\gset_meta rb_
\endtry
\set d debug(:rb_status)
\set d debug(:rb_sqlstate)
\set d debug(:rb_nrows)
}
	});

$node->pgbench(
	'-n -t 1',
	0,
	[
		qr{processed: 1/1},
		qr{latency average = \d+\.\d+ ms},
		qr{keying time average = \d+\.\d+ ms},
		qr{think time average = \d+\.\d+ ms}
	],
	[qr{^$}],
	'pgbench dynamic sleep, keying_time, and think_time',
	{
		'001_pgbench_pacing' => q{
\keying_time 2 ms
\sleep random_exponential(100, 500, 2.0) us
SELECT 1;
\think_time (1 + 2) ms
}
	});

# Test heterogeneous client groups (--group), sequential stream execution, and rate limiting
my $grp_s1 = $node->basedir . '/001_pgbench_grp_s1.sql';
my $grp_s2 = $node->basedir . '/001_pgbench_grp_s2.sql';
my $grp_oltp = $node->basedir . '/001_pgbench_grp_oltp.sql';
append_to_file($grp_s1, "\\set d debug(format_string('%s:%d:s1', :group_name, :stream_id))\nSELECT 1;\n");
append_to_file($grp_s2, "\\set d debug(format_string('%s:%d:s2', :group_name, :stream_id))\nSELECT 2;\n");
append_to_file($grp_oltp, "\\set d debug(format_string('%s:%d:oltp', :group_name, :stream_id))\nSELECT 3;\n");

$node->pgbench(
	"-n -t 2 --group name=oltp,clients=2,threads=1,file=$grp_oltp,rate=1000 --group name=olap,clients=1,threads=1,file=$grp_s1,file=$grp_s2,sequential=true",
	0,
	[
		qr{processed: 6/6},
		qr{client group 1: oltp \(clients: 2, threads: 1\)},
		qr{client group 2: olap \(clients: 1, threads: 1, sequential\)},
	],
	[
		qr{text 'oltp:0:oltp'},
		qr{text 'oltp:1:oltp'},
		qr{text 'olap:0:s1'.*text 'olap:0:s2'}s,
	],
	'pgbench heterogeneous client groups and sequential streams');

# Test latency percentiles, SLA pass/fail validation, composite metrics (tpmC, QphH), and JSON report export
my $tpcc_no = $node->basedir . '/001_pgbench_tpcc_new_order.sql';
my $json_out = $node->basedir . '/001_pgbench_report.json';
append_to_file($tpcc_no, "SELECT 1;\n");

$node->pgbench(
	"-n -t 2 --group name=oltp,clients=1,threads=1,file=$tpcc_no --group name=olap,clients=1,threads=1,file=$grp_s1,sequential=true --sla new_order:p90<=10s --sla olap:p95<=10000ms --json-report=$json_out",
	0,
	[
		qr{processed: 4/4},
		qr{latency percentiles: p50 = \d+\.\d+ ms, p90 = \d+\.\d+ ms, p95 = \d+\.\d+ ms, p99 = \d+\.\d+ ms, p99\.9 = \d+\.\d+ ms},
		qr{tpmC = \d+\.\d+ \(New-Order transactions: 2\)},
		qr{QphH\@SF1 = \d+\.\d+ \(queries: 2\)},
		qr{SLA new_order:p90 <= 10000\.000 ms: actual = \d+\.\d+ ms \[PASS\]},
		qr{SLA olap:p95 <= 10000\.000 ms: actual = \d+\.\d+ ms \[PASS\]},
	],
	[
		qr{text 'olap:0:s1'},
	],
	'pgbench percentiles, SLA pass, tpmC, QphH, and json-report');

ok(-f $json_out, 'JSON report file was created');
my $json_content = slurp_file($json_out);
like($json_content, qr/"sla_all_passed": true/, 'JSON report indicates sla_all_passed: true');
like($json_content, qr/"tpmC": \d+\.\d+/, 'JSON report contains tpmC');
like($json_content, qr/"QphH": \d+\.\d+/, 'JSON report contains QphH');

# Test SLA failure returns exit code 1
$node->pgbench(
	'-n -t 1 --sla total:p50<=1us',
	1,
	[
		qr{SLA total:p50 <= 0\.001 ms: actual = \d+\.\d+ ms \[FAIL\]},
	],
	[
		qr{One or more SLA compliance checks failed},
	],
	'pgbench SLA failure exit status',
	{
		'001_pgbench_sla_fail' => q{
SELECT pg_sleep(0.005);
}
	});

# Test Tier 1: PGBT_VECTOR, PGBT_BYTEA, vector/sparse/graph generators, and copy-on-write mutation
$node->pgbench(
	'-n -t 1 -Dcli_vec=[3,4,0] -Dcli_bin=\\xdeadbeef',
	0,
	[qr{processed: 1/1}],
	[
		qr{command=1.: vector \[3,4,0\]},
		qr{command=2.: bytea \\xdeadbeef},
		qr{command=4.: int 3\b},
		qr{command=6.: double 5\b},
		qr{command=9.: vector \[3,12,0\]},
		qr{command=10.: double 12\.369},
		qr{command=12.: double 5\b},
		qr{command=14.: double 1\b},
		qr{command=16.: double -32\b},
		qr{command=24.: boolean true\b},
		qr{command=28.: boolean true\b}
	],
	'pgbench PGBT_VECTOR, PGBT_BYTEA, vector/graph generators, and distance metrics',
	{
		'001_pgbench_tier1_types_and_generators' => q{
\set d debug(:cli_vec)
\set d debug(:cli_bin)
\set vdim vector_dims(:cli_vec)
\set d debug(:vdim)
\set vnorm vector_norm(:cli_vec)
\set d debug(:vnorm)
\set v2 :cli_vec
\set v2[1] 12.0
\set d debug(:v2)
\set d debug(vector_norm(:v2))
\set dl2 vector_distance(vector(3, 4, 0), vector(0, 0, 0), 'l2')
\set d debug(:dl2)
\set dcos vector_distance(vector(1, 0, 0), vector(0, 1, 0), 'cosine')
\set d debug(:dcos)
\set dip vector_distance(vector(1, 2, 3), vector(4, 5, 6), 'ip')
\set d debug(:dip)
\set rv random_vector(8, -1.0, 1.0, true)
\set gv random_vector_gaussian(8, 1.0, true)
\set cv1 clustered_vector(8, 4, 0.0, 42, 2)
\set cv2 clustered_vector(8, 4, 0.0, 42, 2)
\set pv0 planted_vector(8, 100, 0.0, 42)
\set pv1 planted_vector(8, 100, 0.25, 42)
\set pv_ok (abs(vector_distance(:pv0, :pv1, 'l2') - 0.25) < 0.0001 and vector_distance(:cv1, :cv2, 'l2') < 0.0001)
\set d debug(:pv_ok)
\set sv random_sparse_vector(50, 3, 0.5, 0.5)
\set edge rmat_edge(10, 0.57, 0.19, 0.19, 42)
\set edge_ok (:edge[0] >= 0 and :edge[0] < 1024 and :edge[1] >= 0 and :edge[1] < 1024)
\set d debug(:edge_ok)
SELECT ':cli_vec'::text, ':cli_bin'::bytea, ':sv'::text;
}
	});

# Test Tier 1: \gset_col and multi-row \aset
$node->pgbench(
	'-n -t 1',
	0,
	[qr{processed: 1/1}],
	[
		qr{command=2.: array \{10,20,30\}},
		qr{command=4.: array \{a,b,c\}},
		qr{command=6.: array \{100,200\}},
		qr{command=7.: array \{x,y\}},
		qr{command=9.: int 20\b}
	],
	'pgbench \gset_col and multi-row \aset',
	{
		'001_pgbench_gset_col_and_aset' => q{
SELECT g * 10 AS id, chr(96 + g) AS lbl FROM generate_series(1, 3) g \gset_col col_ids 0
\set d debug(:col_ids)
SELECT g * 10 AS id, chr(96 + g) AS lbl FROM generate_series(1, 3) g \gset_col col_lbls lbl
\set d debug(:col_lbls)
SELECT 100 AS k, 'x'::text AS v UNION ALL SELECT 200 AS k, 'y'::text AS v \aset mr_
\set d debug(:mr_k)
\set d debug(:mr_v)
\set mid :col_ids[1]
\set d debug(:mid)
}
	});

# Test Tier 1: Shared Read-Only Memory-Mapped Dataset Pool (.fvecs, .ivecs, .csv, .tsv) and Parallel COPY Ingestion
my $fvecs_file = $node->basedir . '/test_vecs.fvecs';
my $ivecs_file = $node->basedir . '/test_gt.ivecs';
my $csv_file = $node->basedir . '/test_items.csv';
my $tsv_file = $node->basedir . '/test_edges.tsv';

{
	open my $fh, '>:raw', $fvecs_file or die "open $fvecs_file: $!";
	# 4 vectors of dim 3: [1,2,3], [4,5,6], [7,8,9], [10,11,12]
	for my $r (0 .. 3)
	{
		my $base = $r * 3;
		print $fh pack('l<f<3', 3, $base + 1.0, $base + 2.0, $base + 3.0);
	}
	close $fh;

	open my $ifh, '>:raw', $ivecs_file or die "open $ivecs_file: $!";
	# 4 rows of dim 2 int32 ground-truth IDs
	for my $r (0 .. 3)
	{
		print $ifh pack('l<3', 2, $r * 10 + 1, $r * 10 + 2);
	}
	close $ifh;
}
append_to_file($csv_file, "id,name,score\n1,alpha,10.5\n2,\"hello,world\",20.5\n3,gamma,30.0\n4,delta,40.25\n");
append_to_file($tsv_file, "src\tdst\n10\t20\n20\t30\n30\t40\n40\t10\n");

$node->safe_psql('postgres',
	'CREATE TABLE copy_vecs(id bigint, v text); '
	  . 'CREATE TABLE copy_csv(id int, name text, score float8); '
	  . 'CREATE TABLE copy_tsv(src int, dst int);');

# Test --copy-dataset during initialization (-i -I d) with multiple worker threads (-j 2)
$node->pgbench(
	"-i -I d -j 2 --dataset=vecs:$fvecs_file --dataset=csv:$csv_file --copy-dataset=vecs:copy_vecs --copy-dataset=csv:copy_csv",
	0,
	[qr{^$}],
	[
		qr{copying dataset "vecs" \(4 rows\) into copy_vecs},
		qr{copying dataset "csv" \(4 rows\) into copy_csv}
	],
	'pgbench --copy-dataset parallel bulk ingestion');

is($node->safe_psql('postgres', 'SELECT count(*) FROM copy_vecs;'), '4', 'copy_vecs row count after --copy-dataset');
is($node->safe_psql('postgres', "SELECT v FROM copy_vecs WHERE id = 0;"), '[1,2,3]', 'copy_vecs row 0 vector value');
is($node->safe_psql('postgres', 'SELECT count(*) FROM copy_csv;'), '4', 'copy_csv row count after --copy-dataset');
is($node->safe_psql('postgres', "SELECT name FROM copy_csv WHERE id = 2;"), 'hello,world', 'copy_csv quoted comma field');

# Test dataset_count, dataset_get (zero-copy + copy-on-write), dataset_next_id, \dataset_bind, and \copy_dataset
$node->pgbench(
	"-n -c 2 -j 2 -t 1 --vector-dataset=vecs:$fvecs_file --dataset=gt:$ivecs_file --dataset=csv:$csv_file --dataset=tsv:$tsv_file",
	0,
	[qr{processed: 2/2}],
	[
		qr{int 4\b},
		qr{vector \[1,2,3\]},
		qr{vector \[99,2,3\]},
		qr{array \{1,2\}},
		qr{text 'hello,world'},
		qr{text 'alpha'}
	],
	'pgbench dataset functions, \dataset_bind, and \copy_dataset',
	{
		'001_pgbench_datasets' => q{
\if :client_id = 0
\set cnt dataset_count('vecs')
\set d debug(:cnt)
\set v0 dataset_get('vecs', 0)
\set d debug(:v0)
\set v0[0] 99.0
\set d debug(:v0)
\set v0_orig dataset_get('vecs', 0)
\set d debug(:v0_orig)
\set gt0 dataset_get('gt', 0)
\set d debug(:gt0)
\set c1 dataset_get('csv', 1, 1)
\set d debug(:c1)
\dataset_bind csv 0 row_id row_name row_score
\set d debug(:row_name)
\endif
\copy_dataset tsv copy_tsv
}
	});

is($node->safe_psql('postgres', 'SELECT count(*) FROM copy_tsv;'), '4', 'copy_tsv row count after parallel \copy_dataset');
$node->safe_psql('postgres', 'DROP TABLE copy_vecs, copy_csv, copy_tsv;');

# Test Tier 2 Pillar 2: Global Monotonic Clock, Causal Watermarks (\wait_until, \advance_watermark, get_watermark, set_watermark), and --tcr
$node->safe_psql('postgres', 'CREATE TABLE causal_log(seq serial, msg text);');

$node->pgbench(
	'-n -c 2 -j 2 -t 1 --tcr=10.0',
	0,
	[qr{processed: 2/2}],
	[
		qr{int 100\b},
		qr{int 250\b}
	],
	'pgbench causal watermarks (\wait_until, \advance_watermark) across threads',
	{
		'001_pgbench_causal_watermark' => q{
\if :client_id = 1
\wait_until 5000 dep=100
\set wm get_watermark()
\set d debug(:wm)
INSERT INTO causal_log(msg) VALUES ('c1_after_wm:' || :wm);
\set wm2 set_watermark(250)
\set d debug(get_watermark())
\else
\sleep 2 ms
INSERT INTO causal_log(msg) VALUES ('c0_before_wm');
\advance_watermark 100
\endif
}
	});

is(
	$node->safe_psql('postgres', "SELECT string_agg(msg, ',' ORDER BY seq) FROM causal_log;"),
	'c0_before_wm,c1_after_wm:100',
	'cross-thread causal watermark ordering enforced by \wait_until and \advance_watermark');
$node->safe_psql('postgres', 'DROP TABLE causal_log;');

# Test Tier 2 Pillar 3: Shared Entity Reservoirs (--reservoir, \reservoir_push, \reservoir_sample, reservoir_push/sample/count)
$node->pgbench(
	'-n -t 1 --reservoir=orders:3',
	0,
	[qr{processed: 1/1}],
	[
		qr{command=2.: int 0\b},
		qr{command=4.: null\b},
		qr{command=6.: int 999\b},
		qr{command=8.: null\b},
		qr{command=12.: int 2\b},
		qr{command=17.: int 3\b},
		qr{command=20.: boolean true\b},
		qr{command=23.: boolean true\b}
	],
	'pgbench shared entity reservoirs (empty handling, push, sample, ring wraparound)',
	{
		'001_pgbench_reservoirs' => q{
\set c0 reservoir_count('orders')
\set d debug(:c0)
\set s0 reservoir_sample('orders')
\set d debug(:s0)
\reservoir_sample orders def_ord 999
\set d debug(:def_ord)
\reservoir_sample orders null_ord
\set d debug(:null_ord)
\reservoir_push orders 101
\set p_ok reservoir_push('orders', 102)
\set c2 reservoir_count('orders')
\set d debug(:c2)
\reservoir_push orders 103
\reservoir_push orders 104
\reservoir_push orders 105
\set c3 reservoir_count('orders')
\set d debug(:c3)
\reservoir_sample orders samp1
\set s1_ok (:samp1 >= 101 and :samp1 <= 105)
\set d debug(:s1_ok)
\set samp2 reservoir_sample('orders')
\set s2_ok (:samp2 >= 101 and :samp2 <= 105)
\set d debug(:s2_ok)
SELECT :samp1, :samp2;
}
	});

# Test Tier 2 Pillar 6: Non-blocking session lifecycle (\session_disconnect, \session_connect, --max-concurrent-sessions) and \call sub-scripts
my $call_sub = $node->basedir . '/001_pgbench_call_sub.sql';
my $call_err = $node->basedir . '/001_pgbench_call_err.sql';
my $call_main = $node->basedir . '/001_pgbench_call_main.sql';

append_to_file($call_sub, q{
\set sub_res (:arg_x * 10 + :shared_v)
SELECT :sub_res AS sub_col \gset
});

append_to_file($call_err, q{
SELECT 1/0;
});

append_to_file($call_main, q{
\set shared_v 7
\call 001_pgbench_call_sub arg_x=4
\set d debug(:sub_col)
\set caught_sub 0
\try
\call 001_pgbench_call_err
\catch '22012'
\set caught_sub 1
\endtry
\set d debug(:caught_sub)
\session_disconnect
\session_connect db=postgres
SELECT current_database() AS cur_db \gset
\set d debug(:cur_db)
});

$node->pgbench(
	"-n -t 1 -f $call_main\@100 -f $call_sub\@0 -f $call_err\@0",
	0,
	[
		qr{processed: 1/1},
		qr{001_pgbench_call_sub\.sql},
		qr{number of expected rollbacks: 1\b}
	],
	[
		qr{int 47\b},
		qr{int 1\b},
		qr{text 'postgres'}
	],
	'pgbench \call sub-scripts (arguments, error catching across frames) and non-blocking \session_disconnect / \session_connect');

# Test --max-concurrent-sessions=1 with 2 clients across 2 threads
$node->pgbench(
	'-n -c 2 -j 2 -t 1 --max-concurrent-sessions=1',
	0,
	[qr{processed: 2/2}],
	[qr{^$}],
	'pgbench --max-concurrent-sessions=1 admission control across 2 clients',
	{
		'001_pgbench_max_sessions' => q{
\session_connect
SELECT 1;
\session_disconnect
}
	});

# Test Tier 3 Pillar 7: recall_at_k, ndcg_at_k, \record_recall (Forms A and B), :last_recall, and Recall@K SLA gates
my $recall_json = $node->basedir . '/001_pgbench_recall_report.json';
$node->pgbench(
	"-n -t 1 --ontime-threshold=1000 --sla=recall>=0.5 --sla=recall:p50>=0.5 "
	  . "--sla=schedule_lag:p95<=5000ms --sla=ontime_pct>=50 --json-report=$recall_json",
	0,
	[
		qr{processed: 1/1},
		qr{recall average = 0\.7500 \(stddev = 0\.2500, min = 0\.5000, max = 1\.0000, samples = 2\)},
		qr{recall percentiles: p5 = 0\.50\d+, p50 = 0\.50\d+, p95 = 1\.0000},
		qr{schedule lag percentiles: p50 = \d+\.\d+ ms},
		qr{schedule on-time \(<= 1000\.0 ms\): 1/1 \(100\.000%\)},
		qr{SLA total:recall >= 0\.5000: actual = 0\.7500 \[PASS\]},
		qr{SLA total:recall_p50 >= 0\.5000: actual = 0\.\d+ \[PASS\]},
		qr{SLA total:schedule_lag:p95 <= 5000\.000 ms: actual = \d+\.\d+ ms \[PASS\]},
		qr{SLA total:ontime_pct >= 50\.00%: actual = 100\.00% \[PASS\]},
	],
	[
		qr{double 1\b},
		qr{double 0\.5\b},
		qr{double 0\b},
		qr{double 1\b},
		qr{double 1\b},
		qr{boolean true\b},
		qr{double 0\.5\b},
		qr{double 1\b}
	],
	'pgbench recall_at_k, ndcg_at_k, \record_recall, schedule_lag HDR histogram, and SLA gates (PASS)',
	{
		'001_pgbench_recall_and_lag' => q{
\wait_until 0
\set gt array(10, 20, 30, 40)
\set ret_perf array(10, 20, 30, 40)
\set ret_half array(20, 10, 99, 88)
\set ret_none array(91, 92, 93, 94)
\set empty_gt array()
\set r_perf recall_at_k(:ret_perf, :gt, 4)
\set d debug(:r_perf)
\set r_half recall_at_k(:ret_half, :gt, 4)
\set d debug(:r_half)
\set r_none recall_at_k(:ret_none, :gt, 4)
\set d debug(:r_none)
\set r_empty recall_at_k(:ret_perf, :empty_gt, 4)
\set d debug(:r_empty)
\set n_perf ndcg_at_k(:ret_perf, :gt, 4)
\set d debug(:n_perf)
\set ret_rank1 array(10, 99)
\set ret_rank2 array(99, 10)
\set gt_single array(10, 20)
\set n1 ndcg_at_k(:ret_rank1, :gt_single, 2)
\set n2 ndcg_at_k(:ret_rank2, :gt_single, 2)
\set rank_ok (:n1 > :n2)
\set d debug(:rank_ok)
\record_recall :ret_half :gt 4
\set d debug(:last_recall)
\record_recall 1.0
\set d debug(:last_recall)
SELECT 1;
}
	});

my $recall_json_content = slurp_file($recall_json);
like($recall_json_content, qr/"schedule_lag_ms":\s*\{/, 'JSON report contains schedule_lag_ms object');
like($recall_json_content, qr/"recall":\s*\{\s*"samples":\s*2/, 'JSON report contains recall object with 2 samples');

# Test failing Recall@K SLA gate returns exit status 1
$node->pgbench(
	'-n -t 1 --sla=recall>=0.95',
	1,
	[
		qr{SLA total:recall >= 0\.9500: actual = 0\.2500 \[FAIL\]},
	],
	[qr{One or more SLA compliance checks failed}],
	'pgbench Recall@K SLA violation exits with status 1',
	{
		'001_pgbench_recall_fail' => q{
\record_recall 0.25
SELECT 1;
}
	});

# Test VecBench Phase 1: Named Recall Metrics (r10, r100), 10-bucket distribution, :last_recall_<name>, and SLA compliance
my $vec_recall_json = $node->basedir . '/001_pgbench_vec_recall_report.json';
$node->pgbench(
	"-n -t 2 --sla=r10:recall>=0.5 --sla=r100:recall>=0.90 --json-report=$vec_recall_json",
	0,
	[
		qr{processed: 2/2},
		qr{recall\[r10\] average = 0\.5000},
		qr{recall\[r100\] average = 0\.9800},
		qr{recall\[r10\] distribution: \[0\.5-0\.6\): 2},
		qr{recall\[r100\] distribution: \[0\.9-1\.0\]: 2},
		qr{SLA r10:r10:recall >= 0\.5000: actual = 0\.5000 \[PASS\]},
		qr{SLA r100:r100:recall >= 0\.9000: actual = 0\.9800 \[PASS\]},
	],
	[
		qr{double 0\.5\b},
		qr{double 0\.98\b}
	],
	'pgbench VecBench named recall metrics and 10-bucket distribution (PASS)',
	{
		'001_pgbench_vecbench_recalls' => q{
\set gt array(10, 20, 30, 40)
\set ret array(20, 10, 99, 88)
\record_recall r10 recall_at_k(:ret, :gt, 4)
\set d debug(:last_recall_r10)
\record_recall r100 0.98
\set d debug(:last_recall_r100)
SELECT 1;
}
	});

my $vec_recall_json_content = slurp_file($vec_recall_json);
like($vec_recall_json_content, qr/"named_recall":\s*\{/, 'JSON report contains named_recall object');
like($vec_recall_json_content, qr/"r10":\s*\{/, 'JSON report contains r10 named metric');
like($vec_recall_json_content, qr/"r100":\s*\{/, 'JSON report contains r100 named metric');
like($vec_recall_json_content, qr/"distribution":\s*\{/, 'JSON report contains distribution object');

# Test --replay=<trace.csv> mode with causal watermarks
$node->safe_psql('postgres', 'CREATE TABLE replay_log(seq serial, step text);');
my $replay_csv = $node->basedir . '/001_trace_replay.csv';
append_to_file($replay_csv,
	"0,0,10,INSERT INTO replay_log(step) VALUES ('step1_wm10')\n"
  . "5,10,20,INSERT INTO replay_log(step) VALUES ('step2_after_wm10')\n"
);

$node->pgbench(
	"-n -c 2 -j 2 -t 1 --replay=$replay_csv",
	0,
	[
		qr{transaction type: <builtin: trace-replay>},
		qr{processed: 2/2},
	],
	[qr{loaded dataset "replay"}],
	'pgbench --replay trace replay with causal watermarks');

is(
	$node->safe_psql('postgres', "SELECT string_agg(step, ',' ORDER BY seq) FROM replay_log;"),
	'step1_wm10,step2_after_wm10',
	'--replay trace executed in causal watermark order');
$node->safe_psql('postgres', 'TRUNCATE replay_log;');

# Test --replay when total_rows % nclients != 0 (3 rows across 2 clients -> 4 tx slots)
my $replay_odd_csv = $node->basedir . '/001_trace_replay_odd.csv';
append_to_file($replay_odd_csv,
	"0,0,10,INSERT INTO replay_log(step) VALUES ('odd_step1')\n"
  . "0,10,20,INSERT INTO replay_log(step) VALUES ('odd_step2')\n"
  . "0,20,30,INSERT INTO replay_log(step) VALUES ('odd_step3')\n"
);

$node->pgbench(
	"-n -c 2 -j 2 --replay=$replay_odd_csv",
	0,
	[
		qr{transaction type: <builtin: trace-replay>},
		qr{processed: 4/4},
	],
	[qr{loaded dataset "replay"}],
	'pgbench --replay with non-divisible row count does not wrap around');

is(
	$node->safe_psql('postgres', "SELECT string_agg(step, ',' ORDER BY seq) FROM replay_log;"),
	'odd_step1,odd_step2,odd_step3',
	'--replay trace with 3 rows and 2 clients executes each row exactly once');
$node->safe_psql('postgres', 'DROP TABLE replay_log;');

# Test causal watermark deadlock detection when no active client can advance the watermark
$node->pgbench(
	'-n -c 2 -j 2 -t 1',
	2,
	[],
	[qr{no active client can advance the watermark}],
	'pgbench detects causal watermark deadlock and aborts cleanly',
	{
		'001_pgbench_watermark_deadlock' => q{
\if :client_id = 1
\wait_until 0 WATERMARK 999 CHANNEL 2
\endif
SELECT 1;
}
	});

# Test random_bytea, 3-arg random_vector with string dist, 3-arg array_generate,
# multi-channel watermarks, and \dataset_bind AT
my $at_csv = $node->basedir . '/001_dataset_at.csv';
append_to_file($at_csv, "100,alpha\n200,beta\n300,gamma\n");

$node->pgbench(
	"-n -t 1 --dataset=at_ds:$at_csv",
	0,
	[qr{processed: 1/1}],
	[qr{loaded dataset "at_ds"}],
	'pgbench random_bytea, string random_vector, 3-arg array_generate, multi-channel watermarks, and dataset_bind AT',
	{
		'001_pgbench_pillars_extra' => q{
\set b random_bytea(8, 16)
\set v random_vector(4, 'gaussian', true)
\set arr array_generate(3, 10, 20)
\set wm1 advance_watermark(500, 1)
\advance_watermark 600 CHANNEL 1
\set wm1_read get_watermark(1)
\dataset_bind at_ds AT 1 row_id row_label
SELECT octet_length(:b::bytea) BETWEEN 8 AND 16,
       :wm1_read = 600,
       :row_id = 200,
       :row_label = 'beta';
}
	});

# Test Phase 1 LDBC Engine & Generator Extensions:
# 1) ldbc_correlated_edge & ldbc_flashmob_ts
# 2) \begin_dependent_write / \end_dependent_write multi-writer min-watermark gatekeeper
# 3) --dataset :header flag and \dataset_bind partitioned / sample cursor modes
# 4) \reservoir_push with PGBT_ARRAY (from \gset_col)
# 5) \phase_barrier across multiple clients
# 6) \validate_result and --sla validation_failures / validation_pct
my $ldbc_hdr_csv = $node->basedir . '/001_ldbc_header.csv';
append_to_file($ldbc_hdr_csv, "personId|firstName\n101|Alice\n102|Bob\n103|Carol\n104|Dave\n");

my $ldbc_val_json = $node->basedir . '/001_ldbc_validation.json';
$node->pgbench(
	"-n -c 2 -j 2 -t 2 --dataset=ldbc_p:$ldbc_hdr_csv:pipe:header --reservoir=ldbc_res:64 "
	  . "--sla validation_failures<=0 --sla validation_pct>=100.0 --json-report=$ldbc_val_json",
	0,
	[
		qr{processed: 4/4},
		qr{validation results: passed = 12, failed = 0 \(100\.00%\)},
		qr{SLA total:validation_failures <= 0: actual = 0 \[PASS\]},
		qr{SLA total:validation_pct >= 100\.00%: actual = 100\.00% \[PASS\]},
	],
	[
		qr{loaded dataset "ldbc_p".*4 rows},
	],
	'pgbench Phase 1 LDBC generators, dataset :header/partitioned/sample, array reservoir_push, phase_barrier, and validate_result',
	{
		'001_pgbench_ldbc_phase1' => q{
\set dst ldbc_correlated_edge(10, 100, 5, 1.0, 42)
\set fts ldbc_flashmob_ts(1000000, 500000, 4, 0.8, 42)
\validate_result (:dst >= 1 and :dst <= 100 and :dst != 10 and (:dst % 5) = 0) true
\validate_result (:fts >= 1000000 and :fts <= 1500000) true
\dataset_bind ldbc_p partitioned pid pname
\dataset_bind ldbc_p sample spid spname
SELECT g FROM generate_series(1001, 1003) g \gset_col discovered_ids
\reservoir_push ldbc_res :discovered_ids
\phase_barrier
\reservoir_sample ldbc_res sampled_id 0
SELECT (:pid >= 101 AND :pid <= 104) AND (:spid >= 101 AND :spid <= 104) AND (:sampled_id >= 1001 AND :sampled_id <= 1003) AS ok \gset
\validate_result :ok true
}
	});

my $ldbc_val_json_content = slurp_file($ldbc_val_json);
like($ldbc_val_json_content, qr/"validation_passed": 12/, 'JSON report contains validation_passed: 12');
like($ldbc_val_json_content, qr/"validation_failed": 0/, 'JSON report contains validation_failed: 0');

# Test multi-writer min-watermark gatekeeper (\begin_dependent_write / \end_dependent_write)
# Out-of-order completion: writer A starts at t=100, writer B starts at t=200 and finishes at t=200
# while writer A is still in flight -> get_watermark(3) must stay at 99 until writer A ends!
$node->pgbench(
	'-n -t 1',
	0,
	[
		qr{processed: 1/1},
		qr{validation results: passed = 3, failed = 0 \(100\.00%\)},
	],
	[qr{^$}],
	'pgbench multi-writer min-watermark gatekeeper holds back watermark until oldest in-flight writer completes',
	{
		'001_pgbench_dep_write_gatekeeper' => q{
\begin_dependent_write 100 CHANNEL 3
\set wm_during_100 get_watermark(3)
\validate_result :wm_during_100 0
\set dummy begin_dependent_write(200, 3)
\set dummy end_dependent_write(200, 3)
\set wm_while_100_still_active get_watermark(3)
\validate_result :wm_while_100_still_active 99
\end_dependent_write 100 CHANNEL 3
\set wm_after_100_done get_watermark(3)
\validate_result :wm_after_100_done 200
SELECT 1;
}
	});

# Test standard benchmark workload suites (pgbench-b, pgbench-c, pgbench-e, pgbench-h, pgbench-ds, pgbench-htap, pgbench-vec, pgbench-graph)
my $wl_dir = "$FindBin::RealBin/../workloads";

for my $db (qw(wl_pgbench_b wl_pgbench_c wl_pgbench_e wl_pgbench_h wl_pgbench_ds wl_pgbench_htap wl_pgbench_vec wl_pgbench_graph))
{
	$node->safe_psql('postgres', "CREATE DATABASE $db;");
}

# 0. pgbench-b parallel initialization and workload execution
$node->pgbench(
	"-n -d wl_pgbench_b -t 1 -f $wl_dir/pgbench-b/pgbench_b_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-b schema creation');

$node->pgbench(
	"-n -d wl_pgbench_b -c 2 -j 2 -t 1 -s 2 -D num_workers=2 -f $wl_dir/pgbench-b/pgbench_b_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-b parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_b -t 1 -f $wl_dir/pgbench-b/pgbench_b_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-b post-load constraints');

is($node->safe_psql('wl_pgbench_b', 'SELECT count(*) FROM pgbench_branches;'), '2', 'pgbench-b branches count');
is($node->safe_psql('wl_pgbench_b', 'SELECT count(*) FROM pgbench_tellers;'), '20', 'pgbench-b tellers count');
is($node->safe_psql('wl_pgbench_b', 'SELECT count(*) FROM pgbench_accounts;'), '200000', 'pgbench-b accounts count');

my $b_json = $node->basedir . '/001_pgbench_b_report.json';
$node->pgbench(
	"-n -d wl_pgbench_b -c 2 -j 2 -t 5 -s 2 "
	  . "-f $wl_dir/pgbench-b/pgbench_b_tx.sql\@70 "
	  . "-f $wl_dir/pgbench-b/pgbench_b_simple_update.sql\@20 "
	  . "-f $wl_dir/pgbench-b/pgbench_b_select_only.sql\@10 "
	  . "--sla total:p95<=60s --json-report=$b_json",
	0,
	[
		qr{processed: 10/10},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{SLA total:p95 <= 60000\.000 ms: actual = \d+\.\d+ ms \[PASS\]},
	],
	[qr{^$}],
	'pgbench-b workload suite execution');

# 1. pgbench-c parallel initialization and workload execution
$node->pgbench(
	"-n -d wl_pgbench_c -t 1 -f $wl_dir/pgbench-c/pgbench_c_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-c schema creation');

$node->pgbench(
	"-n -d wl_pgbench_c -c 2 -j 2 -t 1 -s 2 -D num_workers=2 -f $wl_dir/pgbench-c/pgbench_c_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-c parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_c -t 1 -f $wl_dir/pgbench-c/pgbench_c_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-c post-load constraints');

is($node->safe_psql('wl_pgbench_c', 'SELECT count(*) FROM warehouse;'), '2', 'pgbench-c warehouse count');
is($node->safe_psql('wl_pgbench_c', 'SELECT count(*) FROM district;'), '20', 'pgbench-c district count');
is($node->safe_psql('wl_pgbench_c', 'SELECT count(*) FROM item;'), '1000', 'pgbench-c item count');

my $c_json = $node->basedir . '/001_pgbench_c_report.json';
$node->pgbench(
	"-n -d wl_pgbench_c -c 2 -j 2 -t 5 -s 2 "
	  . "-f $wl_dir/pgbench-c/pgbench_c_new_order.sql\@45 "
	  . "-f $wl_dir/pgbench-c/pgbench_c_payment.sql\@43 "
	  . "-f $wl_dir/pgbench-c/pgbench_c_order_status.sql\@4 "
	  . "-f $wl_dir/pgbench-c/pgbench_c_delivery.sql\@4 "
	  . "-f $wl_dir/pgbench-c/pgbench_c_stock_level.sql\@4 "
	  . "--sla total:p95<=60s --json-report=$c_json",
	0,
	[
		qr{processed: 10/10},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{SLA total:p95 <= 60000\.000 ms: actual = \d+\.\d+ ms \[PASS\]},
	],
	[qr{^$}],
	'pgbench-c workload suite execution');

# 2. pgbench-e parallel initialization and workload execution
$node->pgbench(
	"-n -d wl_pgbench_e -t 1 -f $wl_dir/pgbench-e/pgbench_e_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-e schema creation');

$node->pgbench(
	"-n -d wl_pgbench_e -c 2 -j 2 -t 1 -s 2 -D num_workers=2 -f $wl_dir/pgbench-e/pgbench_e_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-e parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_e -t 1 -f $wl_dir/pgbench-e/pgbench_e_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-e post-load constraints');

is($node->safe_psql('wl_pgbench_e', 'SELECT count(*) FROM broker;'), '20', 'pgbench-e broker count');
is($node->safe_psql('wl_pgbench_e', 'SELECT count(*) FROM security;'), '200', 'pgbench-e security count');

$node->pgbench(
	"-n -d wl_pgbench_e -c 2 -j 2 -t 5 -s 2 "
	  . "-f $wl_dir/pgbench-e/pgbench_e_trade_order.sql\@10 "
	  . "-f $wl_dir/pgbench-e/pgbench_e_trade_result.sql\@10 "
	  . "-f $wl_dir/pgbench-e/pgbench_e_customer_position.sql\@13 "
	  . "-f $wl_dir/pgbench-e/pgbench_e_market_watch.sql\@18 "
	  . "-f $wl_dir/pgbench-e/pgbench_e_broker_volume.sql\@5 "
	  . "-f $wl_dir/pgbench-e/pgbench_e_trade_status.sql\@44",
	0,
	[
		qr{processed: 10/10},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-e workload suite execution');

# 3. pgbench-h parallel initialization and workload execution
$node->pgbench(
	"-n -d wl_pgbench_h -t 1 -f $wl_dir/pgbench-h/pgbench_h_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-h schema creation');

$node->pgbench(
	"-n -d wl_pgbench_h -c 2 -j 2 -t 1 -s 1 -D num_workers=2 -f $wl_dir/pgbench-h/pgbench_h_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-h parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_h -t 1 -f $wl_dir/pgbench-h/pgbench_h_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-h post-load constraints');

is($node->safe_psql('wl_pgbench_h', 'SELECT count(*) FROM region;'), '5', 'pgbench-h region count');
is($node->safe_psql('wl_pgbench_h', 'SELECT count(*) FROM nation;'), '25', 'pgbench-h nation count');
is($node->safe_psql('wl_pgbench_h', 'SELECT count(*) FROM supplier;'), '100', 'pgbench-h supplier count');

$node->pgbench(
	"-n -d wl_pgbench_h -t 1 -s 1 "
	  . "--group name=queries,clients=1,threads=1,file=$wl_dir/pgbench-h/pgbench_h_queries.sql "
	  . "--group name=refresh,clients=1,threads=1,file=$wl_dir/pgbench-h/pgbench_h_refresh.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-h workload suite execution (queries + refresh groups)');

# 4. pgbench-ds parallel initialization and workload execution
$node->pgbench(
	"-n -d wl_pgbench_ds -t 1 -f $wl_dir/pgbench-ds/pgbench_ds_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-ds schema creation');

$node->pgbench(
	"-n -d wl_pgbench_ds -c 2 -j 2 -t 1 -s 1 -D num_workers=2 -f $wl_dir/pgbench-ds/pgbench_ds_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-ds parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_ds -t 1 -f $wl_dir/pgbench-ds/pgbench_ds_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-ds post-load constraints');

is($node->safe_psql('wl_pgbench_ds', 'SELECT count(*) FROM date_dim;'), '730', 'pgbench-ds date_dim count');
is($node->safe_psql('wl_pgbench_ds', 'SELECT count(*) FROM item;'), '200', 'pgbench-ds item count');

$node->pgbench(
	"-n -d wl_pgbench_ds -t 2 -s 1 "
	  . "--group name=ds_queries,clients=1,threads=1,"
	  . "file=$wl_dir/pgbench-ds/pgbench_ds_q_store_sales.sql\@25,"
	  . "file=$wl_dir/pgbench-ds/pgbench_ds_q_cross_channel.sql\@25,"
	  . "file=$wl_dir/pgbench-ds/pgbench_ds_q_inventory.sql\@25,"
	  . "file=$wl_dir/pgbench-ds/pgbench_ds_q_promotion.sql\@25 "
	  . "--group name=ds_maint,clients=1,threads=1,file=$wl_dir/pgbench-ds/pgbench_ds_maintenance.sql",
	0,
	[
		qr{processed: 4/4},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-ds workload suite execution');

# 5. pgbench-htap parallel initialization and hybrid HTAP execution
$node->pgbench(
	"-n -d wl_pgbench_htap -t 1 -f $wl_dir/pgbench-htap/pgbench_htap_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-htap schema creation');

$node->pgbench(
	"-n -d wl_pgbench_htap -c 2 -j 2 -t 1 -s 2 -D num_workers=2 -f $wl_dir/pgbench-htap/pgbench_htap_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-htap parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_htap -t 1 -f $wl_dir/pgbench-htap/pgbench_htap_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-htap post-load constraints');

is($node->safe_psql('wl_pgbench_htap', 'SELECT count(*) FROM htap_supplier;'), '100', 'pgbench-htap supplier count');
is($node->safe_psql('wl_pgbench_htap', 'SELECT count(*) FROM warehouse;'), '2', 'pgbench-htap warehouse count');

my $htap_json = $node->basedir . '/001_htap_report.json';
$node->pgbench(
	"-n -d wl_pgbench_htap -t 2 -s 2 "
	  . "--group name=oltp,clients=2,threads=1,file=$wl_dir/pgbench-htap/pgbench_htap_oltp.sql "
	  . "--group name=olap,clients=1,threads=1,"
	  . "file=$wl_dir/pgbench-htap/pgbench_htap_olap_q1.sql,"
	  . "file=$wl_dir/pgbench-htap/pgbench_htap_olap_q3.sql,"
	  . "file=$wl_dir/pgbench-htap/pgbench_htap_olap_q5.sql,"
	  . "file=$wl_dir/pgbench-htap/pgbench_htap_olap_q6.sql,"
	  . "file=$wl_dir/pgbench-htap/pgbench_htap_olap_q18.sql,sequential=true "
	  . "--sla oltp:p95<=60s --sla olap:p95<=60s --json-report=$htap_json",
	0,
	[
		qr{processed: 6/6},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{SLA oltp:p95 <= 60000\.000 ms: actual = \d+\.\d+ ms \[PASS\]},
		qr{SLA olap:p95 <= 60000\.000 ms: actual = \d+\.\d+ ms \[PASS\]},
	],
	[qr{^$}],
	'pgbench-htap hybrid OLTP + OLAP workload execution');

# 6. pgbench-vec parallel initialization and ANN vector workload execution
$node->pgbench(
	"-n -d wl_pgbench_vec -t 1 -f $wl_dir/pgbench-vec/pgbench_vec_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-vec schema creation');

$node->pgbench(
	"-n -d wl_pgbench_vec -c 2 -j 2 -t 1 -s 1 -D num_workers=2 -f $wl_dir/pgbench-vec/pgbench_vec_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-vec parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_vec -t 1 -f $wl_dir/pgbench-vec/pgbench_vec_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-vec post-load constraints');

is($node->safe_psql('wl_pgbench_vec', 'SELECT count(*) FROM pgbench_vec_items;'), '200', 'pgbench-vec pgbench_vec_items count');

$node->pgbench(
	"-n -d wl_pgbench_vec -c 2 -j 2 -t 3 -s 1 --reservoir=vec_ids:256 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_search.sql\@60 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_filtered.sql\@25 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_insert.sql\@15 "
	  . "--sla recall>=0.90 --sla total:p95<=60s",
	0,
	[
		qr{processed: 6/6},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{SLA total:recall >= 0\.9000: actual = (?:0\.9\d+|1\.0000) \[PASS\]},
	],
	[qr{^$}],
	'pgbench-vec ANN vector workload suite execution');

# Also verify pgbench-vec insert script works without --reservoir (reservoir_count returns -1)
$node->pgbench(
	"-n -d wl_pgbench_vec -c 1 -t 2 -s 1 -f $wl_dir/pgbench-vec/pgbench_vec_insert.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-vec insert execution without --reservoir');

# VecBench Phase 2: Ground truth precomputation and multi-mode hybrid & multi-vector workloads (Modes 1, 2, 3, 5)
$node->pgbench(
	"-n -d wl_pgbench_vec -t 1 -f $wl_dir/pgbench-vec/pgbench_vec_gt_gen.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-vec ground truth precomputation');

is($node->safe_psql('wl_pgbench_vec', 'SELECT count(*) FROM pgbench_vec_queries;'), '20', 'pgbench-vec queries count');
is($node->safe_psql('wl_pgbench_vec', 'SELECT count(*) FROM pgbench_vec_ground_truth;'), '200', 'pgbench-vec ground truth count');

my $vecbench_phase2_json = $node->basedir . '/001_pgbench_vecbench_phase2_report.json';
$node->pgbench(
	"-n -d wl_pgbench_vec -c 2 -j 2 -t 25 -s 1 --reservoir=vec_ids:256 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_search.sql\@30 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_filtered.sql\@25 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_postfiltered.sql\@20 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_multivec.sql\@15 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_insert.sql\@10 "
	  . "--sla=total:r_unfiltered:recall>=0.80 --sla=total:r_prefilter:recall>=0.80 "
	  . "--sla=total:r_postfilter:recall>=0.05 --sla=total:r_multivec:recall>=0.50 "
	  . "--sla total:p95<=60s --json-report=$vecbench_phase2_json",
	0,
	[
		qr{processed: 50/50},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{SLA total:r_unfiltered:recall >= 0\.8000: actual = (?:0\.\d+|1\.0000) \[PASS\]},
		qr{SLA total:r_prefilter:recall >= 0\.8000: actual = (?:0\.\d+|1\.0000) \[PASS\]},
		qr{SLA total:r_postfilter:recall >= 0\.0500: actual = (?:0\.\d+|1\.0000) \[PASS\]},
		qr{SLA total:r_multivec:recall >= 0\.5000: actual = (?:0\.\d+|1\.0000) \[PASS\]},
	],
	[qr{^$}],
	'pgbench-vec full hybrid & multi-vector benchmark execution (Modes 1, 2, 3, 5)');

my $vecbench_phase2_json_content = slurp_file($vecbench_phase2_json);
like($vecbench_phase2_json_content, qr/"named_recall":\s*\{/, 'JSON report contains named_recall');
like($vecbench_phase2_json_content, qr/"r_unfiltered":\s*\{/, 'JSON report contains r_unfiltered');
like($vecbench_phase2_json_content, qr/"r_prefilter":\s*\{/, 'JSON report contains r_prefilter');
like($vecbench_phase2_json_content, qr/"r_postfilter":\s*\{/, 'JSON report contains r_postfilter');
like($vecbench_phase2_json_content, qr/"r_multivec":\s*\{/, 'JSON report contains r_multivec');

# Phase 3 VecBench: Mode 6 Continuous Ingestion, Aging, and Degradation
my $vecbench_phase3_json = $node->basedir . '/001_pgbench_vecbench_phase3_report.json';
$node->pgbench(
	"-n -d wl_pgbench_vec -c 2 -j 2 -t 10 -s 1 --reservoir=vec_ids:256 "
	  . "-f $wl_dir/pgbench-vec/pgbench_vec_aging.sql "
	  . "--sla=total:r_aging:recall>=0.70 "
	  . "--sla total:p95<=60s --json-report=$vecbench_phase3_json",
	0,
	[
		qr{processed: 20/20},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{SLA total:r_aging:recall >= 0\.7000: actual = (?:0\.\d+|1\.0000) \[PASS\]},
		qr{recall\[r_aging\] aging degradation: initial = \d+\.\d+, mature = \d+\.\d+},
	],
	[qr{^$}],
	'pgbench-vec continuous ingestion, aging & degradation benchmark execution (Mode 6)');

my $vecbench_phase3_json_content = slurp_file($vecbench_phase3_json);
like($vecbench_phase3_json_content, qr/"aging_degradation":\s*\{/, 'JSON report contains aging_degradation');
like($vecbench_phase3_json_content, qr/"initial_recall":\s*\d+\.\d+/, 'JSON report contains initial_recall');
like($vecbench_phase3_json_content, qr/"mature_recall":\s*\d+\.\d+/, 'JSON report contains mature_recall');
like($vecbench_phase3_json_content, qr/"delta":\s*-?\d+\.\d+/, 'JSON report contains delta');
like($vecbench_phase3_json_content, qr/"degradation_pct":\s*-?\d+\.\d+/, 'JSON report contains degradation_pct');

# 7. pgbench-graph parallel initialization and LDBC-like graph workload execution
$node->pgbench(
	"-n -d wl_pgbench_graph -t 1 -f $wl_dir/pgbench-graph/pgbench_graph_schema.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-graph schema creation');

$node->pgbench(
	"-n -d wl_pgbench_graph -c 2 -j 2 -t 1 -s 1 -D num_workers=2 -f $wl_dir/pgbench-graph/pgbench_graph_init.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-graph parallel in-database initialization');

$node->pgbench(
	"-n -d wl_pgbench_graph -t 1 -f $wl_dir/pgbench-graph/pgbench_graph_constraints.sql",
	0,
	[qr{processed: 1/1}],
	[qr{^$}],
	'pgbench-graph post-load constraints');

is($node->safe_psql('wl_pgbench_graph', 'SELECT count(*) FROM pgbench_graph_persons;'), '200', 'pgbench-graph person count');

$node->pgbench(
	"-n -d wl_pgbench_graph -c 2 -j 2 -t 3 -s 1 --reservoir=persons:256 "
	  . "-f $wl_dir/pgbench-graph/pgbench_graph_1hop.sql\@50 "
	  . "-f $wl_dir/pgbench-graph/pgbench_graph_2hop.sql\@30 "
	  . "-f $wl_dir/pgbench-graph/pgbench_graph_add_edge.sql\@20 "
	  . "--sla total:p95<=60s",
	0,
	[
		qr{processed: 6/6},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-graph LDBC-like graph workload suite execution');

# Also verify pgbench-graph scripts work without --reservoir (reservoir_count returns -1)
$node->pgbench(
	"-n -d wl_pgbench_graph -c 2 -j 2 -t 2 -s 1 "
	  . "-f $wl_dir/pgbench-graph/pgbench_graph_1hop.sql\@40 "
	  . "-f $wl_dir/pgbench-graph/pgbench_graph_2hop.sql\@40 "
	  . "-f $wl_dir/pgbench-graph/pgbench_graph_add_edge.sql\@20",
	0,
	[
		qr{processed: 4/4},
		qr{number of failed transactions: 0 \(0\.000%\)},
	],
	[qr{^$}],
	'pgbench-graph workload execution without --reservoir');

# Phase 2 LDBC Suite 1: LSQB (Q1-Q9 cyclic, acyclic, and anti-join pattern matching)
$node->pgbench(
	"-n -d wl_pgbench_graph -c 2 -j 2 -t 1 -s 1 "
	  . "--sla validation_failures<=0 --sla validation_pct>=100.0 "
	  . "-f $wl_dir/pgbench-graph/pgbench_ldbc_lsqb.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{validation results: passed = 18, failed = 0 \(100\.00%\)},
		qr{SLA total:validation_failures <= 0: actual = 0 \[PASS\]},
	],
	[qr{^$}],
	'pgbench-graph LDBC LSQB (Q1-Q9) workload suite execution');

# Phase 2 LDBC Suite 2: LDBC SNB Interactive v1/v2 (multi-group readers IC->\call IS + writers IU/ID)
$node->pgbench(
	"-n -d wl_pgbench_graph -t 3 -s 1 --tcr=0.1 "
	  . "--reservoir=persons:512 --reservoir=messages:1024 "
	  . "--group name=snb_readers,clients=1,threads=1,"
	  . "file=$wl_dir/pgbench-graph/pgbench_ldbc_snb_ic.sql\@50,"
	  . "file=$wl_dir/pgbench-graph/pgbench_ldbc_snb_is.sql\@50 "
	  . "--group name=snb_writers,clients=1,threads=1,"
	  . "file=$wl_dir/pgbench-graph/pgbench_ldbc_snb_iu.sql\@70,"
	  . "file=$wl_dir/pgbench-graph/pgbench_ldbc_snb_id.sql\@30 "
	  . "--sla validation_failures<=0 --sla total:p95<=60s",
	0,
	[
		qr{processed: 6/6},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{SLA total:validation_failures <= 0: actual = 0 \[PASS\]},
	],
	[qr{^$}],
	'pgbench-graph LDBC SNB Interactive v1/v2 multi-group workload execution');

# Phase 3 LDBC Suite 3: LDBC SNB Business Intelligence (Power -> \phase_barrier -> Batch Update -> \phase_barrier -> Throughput)
$node->pgbench(
	"-n -d wl_pgbench_graph -c 2 -j 2 -t 1 -s 1 "
	  . "--sla validation_failures<=0 --sla validation_pct>=100.0 "
	  . "-f $wl_dir/pgbench-graph/pgbench_ldbc_snb_bi.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{validation results: passed = 14, failed = 0 \(100\.00%\)},
		qr{SLA total:validation_failures <= 0: actual = 0 \[PASS\]},
	],
	[qr{^$}],
	'pgbench-graph LDBC SNB BI phased workload execution with phase_barrier');

# Phase 3 LDBC Suite 4a: LDBC FinBench (TSR, TCR temporal cycles, TW, and TRW conditional ROLLBACK)
$node->pgbench(
	"-n -d wl_pgbench_graph -c 2 -j 2 -t 4 -s 1 "
	  . "--sla validation_failures<=0 "
	  . "-f $wl_dir/pgbench-graph/pgbench_ldbc_finbench.sql",
	0,
	[
		qr{processed: 8/8},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{validation results: passed = 32, failed = 0 \(100\.00%\)},
		qr{SLA total:validation_failures <= 0: actual = 0 \[PASS\]},
	],
	[qr{^$}],
	'pgbench-graph LDBC FinBench workload execution (TSR, TCR, TW, TRW)');

# Phase 3 LDBC Suite 4b: LDBC Graphalytics (BFS, WCC, SSSP, PageRank, LCC with \validate_result)
$node->pgbench(
	"-n -d wl_pgbench_graph -c 2 -j 2 -t 1 -s 1 "
	  . "--sla validation_failures<=0 --sla validation_pct>=100.0 "
	  . "-f $wl_dir/pgbench-graph/pgbench_ldbc_graphalytics.sql",
	0,
	[
		qr{processed: 2/2},
		qr{number of failed transactions: 0 \(0\.000%\)},
		qr{validation results: passed = 16, failed = 0 \(100\.00%\)},
		qr{SLA total:validation_failures <= 0: actual = 0 \[PASS\]},
	],
	[qr{^$}],
	'pgbench-graph LDBC Graphalytics (BFS, WCC, SSSP, PageRank, LCC) execution');

for my $db (qw(wl_pgbench_b wl_pgbench_c wl_pgbench_e wl_pgbench_h wl_pgbench_ds wl_pgbench_htap wl_pgbench_vec wl_pgbench_graph))
{
	$node->safe_psql('postgres', "DROP DATABASE $db;");
}

# done
$node->safe_psql('postgres', 'DROP TABLESPACE regress_pgbench_tap_1_ts');
$node->stop;
done_testing();

