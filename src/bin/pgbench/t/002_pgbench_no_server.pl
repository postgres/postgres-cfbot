
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

#
# pgbench tests which do not need a server
#

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

# create a directory for scripts
my $testname = $0;
$testname =~ s,.*/,,;
$testname =~ s/\.pl$//;

my $testdir = "$PostgreSQL::Test::Utils::tmp_check/t_${testname}_stuff";
mkdir $testdir
  or BAIL_OUT("could not create test directory \"${testdir}\": $!");

# invoke pgbench
sub pgbench
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($opts, $stat, $out, $err, $name) = @_;
	command_checks_all([ 'pgbench', split(/\s+/, $opts) ],
		$stat, $out, $err, $name);
	return;
}

# invoke pgbench with scripts
sub pgbench_scripts
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($opts, $stat, $out, $err, $name, $files) = @_;
	my @cmd = ('pgbench', split /\s+/, $opts);
	if (defined $files)
	{
		for my $fn (sort keys %$files)
		{
			my $filename = $testdir . '/' . $fn;

			# cleanup file weight if any
			$filename =~ s/\@\d+$//;

			# cleanup from prior runs
			unlink $filename;
			append_to_file($filename, $$files{$fn});
			push @cmd, '--file' => $filename;
		}
	}
	command_checks_all(\@cmd, $stat, $out, $err, $name);
	return;
}

#
# Option various errors
#

my @options = (

	# name, options, stderr checks
	[
		'bad option',
		'-h home -p 5432 -U calvin ---debug --bad-option',
		[qr{--help.*more information}]
	],
	[
		'no file',
		'-f no-such-file',
		[qr{could not open file "no-such-file":}]
	],
	[
		'no builtin',
		'-b no-such-builtin',
		[qr{no builtin script .* "no-such-builtin"}]
	],
	[
		'invalid weight',
		'--builtin=select-only@one',
		[qr{invalid weight specification: \@one}]
	],
	[
		'invalid weight',
		'-b select-only@-1',
		[qr{weight spec.* out of range .*: -1}]
	],
	[ 'too many scripts', '-S ' x 129, [qr{at most 128 SQL scripts}] ],
	[
		'bad #clients', '-c three',
		[qr{invalid value "three" for option -c/--client}]
	],
	[
		'bad #threads', '-j eleven',
		[qr{invalid value "eleven" for option -j/--jobs}]
	],
	[
		'bad scale', '-i -s two',
		[qr{invalid value "two" for option -s/--scale}]
	],
	[
		'invalid #transactions',
		'-t zil', [qr{invalid value "zil" for option -t/--transactions}]
	],
	[
		'invalid duration',
		'-T ten', [qr{invalid value "ten" for option -T/--time}]
	],
	[
		'-t XOR -T',
		'-N -l --aggregate-interval=5 --log-prefix=notused -t 1000 -T 1',
		[qr{specify either }]
	],
	[
		'-T XOR -t',
		'-P 1 --progress-timestamp -l --sampling-rate=0.001 -T 10 -t 1000',
		[qr{specify either }]
	],
	[ 'bad variable', '--define foobla', [qr{invalid variable definition}] ],
	[ 'invalid fillfactor', '-F 1', [qr{-F/--fillfactor must be in range}] ],
	[ 'invalid query mode', '-M no-such-mode', [qr{invalid query mode}] ],
	[
		'invalid progress', '--progress=0',
		[qr{-P/--progress must be in range}]
	],
	[ 'invalid rate', '--rate=0.0', [qr{invalid rate limit}] ],
	[ 'invalid latency', '--latency-limit=0.0', [qr{invalid latency limit}] ],
	[
		'invalid sampling rate', '--sampling-rate=0',
		[qr{invalid sampling rate}]
	],
	[
		'invalid aggregate interval',
		'--aggregate-interval=-3',
		[qr{--aggregate-interval must be in range}]
	],
	[
		'weight zero',
		'-b se@0 -b si@0 -b tpcb@0',
		[qr{weight must not be zero}]
	],
	[ 'init vs run', '-i -S', [qr{cannot be used in initialization}] ],
	[ 'run vs init', '-S -F 90', [qr{cannot be used in benchmarking}] ],
	[ 'ambiguous builtin', '-b s', [qr{ambiguous}] ],
	[
		'--progress-timestamp => --progress', '--progress-timestamp',
		[qr{allowed only under}]
	],
	[
		'-I without init option',
		'-I dtg',
		[qr{cannot be used in benchmarking mode}]
	],
	[
		'invalid init step',
		'-i -I dta',
		[
			qr{unrecognized initialization step},
			qr{Allowed step characters are}
		]
	],
	[
		'bad random seed',
		'--random-seed=one',
		[
			qr{unrecognized random seed option "one"},
			qr{Expecting an unsigned integer, "time" or "rand"},
			qr{error while setting random seed from --random-seed option}
		]
	],
	[
		'bad partition method',
		'-i --partition-method=BAD',
		[ qr{"range"}, qr{"hash"}, qr{"BAD"} ]
	],
	[
		'bad partition number',
		'-i --partitions -1',
		[qr{--partitions must be in range}]
	],
	[
		'partition method without partitioning',
		'-i --partition-method=hash',
		[qr{partition-method requires greater than zero --partitions}]
	],
	[
		'bad maximum number of tries',
		'--max-tries -10',
		[qr{invalid number of maximum tries: "-10"}]
	],
	[
		'an infinite number of tries',
		'--max-tries 0',
		[
			qr{an unlimited number of transaction tries can only be used with --latency-limit or a duration}
		]
	],

	# logging sub-options
	[
		'sampling => log', '--sampling-rate=0.01',
		[qr{log sampling .* only when}]
	],
	[
		'sampling XOR aggregate',
		'-l --sampling-rate=0.1 --aggregate-interval=3',
		[qr{sampling .* aggregation .* cannot be used at the same time}]
	],
	[
		'aggregate => log', '--aggregate-interval=3',
		[qr{aggregation .* only when}]
	],
	[ 'log-prefix => log', '--log-prefix=x', [qr{prefix .* only when}] ],
	[
		'duration & aggregation',
		'-l -T 1 --aggregate-interval=3',
		[qr{aggr.* not be higher}]
	],
	[
		'duration % aggregation',
		'-l -T 5 --aggregate-interval=3',
		[qr{multiple}]
	],
	[
		'bad --group spec',
		'--group invalid',
		[qr{invalid --group specification}]
	],
	[
		'unknown --group key',
		'--group foo=bar',
		[qr{unrecognized key "foo" in --group specification}]
	],
	[
		'--group without script',
		'--group name=g1,clients=2',
		[qr{--group "g1" must specify at least one script}]
	],
	[
		'--group bad sequential',
		'--group name=g1,builtin=select-only,sequential=maybe',
		[qr{invalid sequential value in --group}]
	],
	[
		'--group bad rate',
		'--group name=g1,builtin=select-only,rate=0',
		[qr{invalid rate limit in --group}]
	],
	[
		'bad --sla format',
		'--sla bad_spec',
		[qr{invalid --sla specification}]
	],
	[
		'bad --sla percentile',
		'--sla total:p105<=5s',
		[qr{invalid percentile in --sla specification}]
	],
	[
		'bad --sla unit',
		'--sla total:p90<=5minutes',
		[qr{invalid unit "minutes" in --sla specification}]
	],
	[
		'bad --dataset format',
		'--dataset bad_spec',
		[qr{invalid --dataset specification}]
	],
	[
		'nonexistent --dataset file',
		'--dataset ds:/no/such/file.fvecs',
		[qr{could not open dataset "ds" file}]
	],
	[
		'bad --copy-dataset format',
		'--copy-dataset bad_spec',
		[qr{invalid --copy-dataset specification}]
	],
	[
		'bad --tcr value',
		'--tcr 0',
		[qr{invalid replay speed / TCR value}]
	],
	[
		'bad --reservoir format',
		'--reservoir bad_spec',
		[qr{invalid --reservoir specification}]
	],
	[
		'bad --reservoir capacity',
		'--reservoir orders:0',
		[qr{invalid capacity "0" in --reservoir}]
	],
	[
		'duplicate --reservoir name',
		'--reservoir orders:10 --reservoir orders:20',
		[qr{duplicate reservoir name "orders"}]
	],
	[
		'bad --sla recall threshold',
		'--sla recall>=1.5',
		[qr{invalid recall threshold in --sla specification}]
	],
	[
		'bad --sla ontime_pct threshold',
		'--sla ontime_pct>=120',
		[qr{invalid ontime_pct threshold in --sla specification}]
	],
	[
		'bad --ontime-threshold value',
		'--ontime-threshold -5',
		[qr{invalid on-time threshold value}]
	],
	[
		'nonexistent --replay file',
		'--replay /no/such/trace.csv',
		[qr{could not open dataset "replay" file}]
	],);

for my $o (@options)
{
	my ($name, $opts, $err_checks) = @$o;
	pgbench($opts, 1, [qr{^$}], $err_checks,
		'pgbench option error: ' . $name);
}

program_help_ok('pgbench');
program_version_ok('pgbench');
program_options_handling_ok('pgbench');

# list of builtins
pgbench(
	'-b list',
	0,
	[qr{^$}],
	[
		qr{Available builtin scripts:}, qr{tpcb-like},
		qr{simple-update}, qr{select-only}
	],
	'pgbench builtin list');

# builtin listing
pgbench(
	'--show-script se',
	0,
	[qr{^$}],
	[
		qr{select-only: }, qr{SELECT abalance FROM pgbench_accounts WHERE},
		qr{(?!UPDATE)}, qr{(?!INSERT)}
	],
	'pgbench builtin listing');

my @script_tests = (

	# name, err, { file => contents }
	[
		'missing endif',
		[qr{\\if without matching \\endif}],
		{ 'if-noendif.sql' => '\if 1' }
	],
	[
		'missing if on elif',
		[qr{\\elif without matching \\if}],
		{ 'elif-noif.sql' => '\elif 1' }
	],
	[
		'missing if on else',
		[qr{\\else without matching \\if}],
		{ 'else-noif.sql' => '\else' }
	],
	[
		'missing if on endif',
		[qr{\\endif without matching \\if}],
		{ 'endif-noif.sql' => '\endif' }
	],
	[
		'elif after else',
		[qr{\\elif after \\else}],
		{ 'else-elif.sql' => "\\if 1\n\\else\n\\elif 0\n\\endif" }
	],
	[
		'else after else',
		[qr{\\else after \\else}],
		{ 'else-else.sql' => "\\if 1\n\\else\n\\else\n\\endif" }
	],
	[
		'if syntax error',
		[qr{syntax error in command "if"}],
		{ 'if-bad.sql' => "\\if\n\\endif\n" }
	],
	[
		'elif syntax error',
		[qr{syntax error in command "elif"}],
		{ 'elif-bad.sql' => "\\if 0\n\\elif +\n\\endif\n" }
	],
	[
		'else syntax error',
		[qr{unexpected argument in command "else"}],
		{ 'else-bad.sql' => "\\if 0\n\\else BAD\n\\endif\n" }
	],
	[
		'endif syntax error',
		[qr{unexpected argument in command "endif"}],
		{ 'endif-bad.sql' => "\\if 0\n\\endif BAD\n" }
	],
	[
		'not enough arguments for least',
		[qr{at least one argument expected \(least\)}],
		{ 'bad-least.sql' => "\\set i least()\n" }
	],
	[
		'not enough arguments for greatest',
		[qr{at least one argument expected \(greatest\)}],
		{ 'bad-greatest.sql' => "\\set i greatest()\n" }
	],
	[
		'not enough arguments for hash',
		[qr{unexpected number of arguments \(hash\)}],
		{ 'bad-hash-1.sql' => "\\set i hash()\n" }
	],
	[
		'too many arguments for hash',
		[qr{unexpected number of arguments \(hash\)}],
		{ 'bad-hash-2.sql' => "\\set i hash(1,2,3)\n" }
	],
	# overflow
	[
		'bigint overflow 1',
		[qr{bigint constant overflow}],
		{ 'overflow-1.sql' => "\\set i 100000000000000000000\n" }
	],
	[
		'double overflow 2',
		[qr{double constant overflow}],
		{ 'overflow-2.sql' => "\\set d 1.0E309\n" }
	],
	[
		'double overflow 3',
		[qr{double constant overflow}],
		{ 'overflow-3.sql' => "\\set d .1E310\n" }
	],
	[
		'set i',
		[ qr{set i 1 }, qr{\^ error found here} ],
		{ 'set_i_op' => "\\set i 1 +\n" }
	],
	[
		'not enough arguments to permute',
		[qr{unexpected number of arguments \(permute\)}],
		{ 'bad-permute-1.sql' => "\\set i permute(1)\n" }
	],
	[
		'too many arguments to permute',
		[qr{unexpected number of arguments \(permute\)}],
		{ 'bad-permute-2.sql' => "\\set i permute(1, 2, 3, 4)\n" }
	],
	[
		'unterminated array subscript in set',
		[qr{unterminated array subscript in \\set target}],
		{ 'bad-set-sub.sql' => "\\set arr[1 42\n" }
	],
	[
		'for without endfor',
		[qr{\\for without matching \\endfor}],
		{ 'for-no-endfor.sql' => "\\for i 1 .. 5\n\\set x :i\n" }
	],
	[
		'endfor without for',
		[qr{\\endfor without matching \\for}],
		{ 'endfor-no-for.sql' => "\\endfor\n" }
	],
	[
		'for missing dots',
		[qr{missing "\.\." in \\for command}],
		{ 'for-no-dots.sql' => "\\for i 1 5\n\\endfor\n" }
	],
	[
		'while without endwhile',
		[qr{\\while without matching \\endwhile}],
		{ 'while-no-endwhile.sql' => "\\while 1 = 1\n\\set x 1\n" }
	],
	[
		'try without endtry',
		[qr{\\try without matching \\endtry}],
		{ 'try-no-endtry.sql' => "\\try\nSELECT 1;\n" }
	],
	[
		'catch without try',
		[qr{\\catch without matching \\try}],
		{ 'catch-no-try.sql' => "\\catch\n" }
	],
	[
		'gset_meta alone',
		[qr{\\gset_meta must follow an SQL command}],
		{ 'gset-meta-alone.sql' => "\\gset_meta\n" }
	],
	[
		'gset_meta too many arguments',
		[qr{too many arguments}],
		{ 'gset-meta-args.sql' => "SELECT 1 \\gset_meta a b\n" }
	],
	[
		'keying_time missing argument',
		[qr{missing argument.*keying_time}],
		{ 'keying-missing.sql' => "\\keying_time\n" }
	],
	[
		'think_time invalid unit',
		[qr{unrecognized time unit}],
		{ 'think-bad-unit.sql' => "\\think_time 10 hours\n" }
	],
	[
		'gset_col alone',
		[qr{\\gset_col must follow an SQL command}],
		{ 'gset-col-alone.sql' => "\\gset_col v\n" }
	],
	[
		'gset_col missing argument',
		[qr{missing variable name argument in command "gset_col"}],
		{ 'gset-col-noarg.sql' => "SELECT 1 \\gset_col\n" }
	],
	[
		'dataset_bind missing argument',
		[qr{\\dataset_bind requires}],
		{ 'dataset-bind-noarg.sql' => "\\dataset_bind ds\n" }
	],
	[
		'copy_dataset missing argument',
		[qr{\\copy_dataset requires}],
		{ 'copy-dataset-noarg.sql' => "\\copy_dataset ds\n" }
	],
	[
		'wait_until missing argument',
		[qr{missing argument.*wait_until}],
		{ 'wait-until-noarg.sql' => "\\wait_until\n" }
	],
	[
		'advance_watermark missing argument',
		[qr{missing argument.*advance_watermark}],
		{ 'advance-wm-noarg.sql' => "\\advance_watermark\n" }
	],
	[
		'reservoir_push missing argument',
		[qr{\\reservoir_push requires}],
		{ 'reservoir-push-noarg.sql' => "\\reservoir_push orders\n" }
	],
	[
		'reservoir_sample missing argument',
		[qr{\\reservoir_sample requires}],
		{ 'reservoir-sample-noarg.sql' => "\\reservoir_sample orders\n" }
	],
	[
		'session_disconnect too many arguments',
		[qr{too many arguments.*session_disconnect}],
		{ 'session-disc-args.sql' => "\\session_disconnect a b\n" }
	],
	[
		'call missing argument',
		[qr{\\call requires}],
		{ 'call-noarg.sql' => "\\call\n" }
	],
	[
		'record_recall missing argument',
		[qr{\\record_recall requires}],
		{ 'record-recall-noarg.sql' => "\\record_recall\n" }
	],);

for my $t (@script_tests)
{
	my ($name, $err, $files) = @$t;
	pgbench_scripts('', 1, [qr{^$}], $err, 'pgbench option error: ' . $name,
		$files);
}

done_testing();
