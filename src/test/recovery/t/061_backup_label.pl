# Copyright (c) 2026, PostgreSQL Global Development Group

# Test parsing of LSN fields in backup_label.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my @tests = (
	{
		name => 'nul_in_start_lsn',
		start_lsn => "0/123\0junk",
		checkpoint_lsn => '0/1',
	},
	{
		name => 'nul_after_start_lsn',
		start_lsn => "0/123\0",
		checkpoint_lsn => '0/1',
	},
	{
		name => 'nul_in_checkpoint_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => "0/123\0junk",
	},
	{
		name => 'nul_after_checkpoint_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => "0/123\0",
	},
	{
		name => 'non_hexadecimal_start_lsn',
		start_lsn => '0/12G',
		checkpoint_lsn => '0/1',
	},
	{
		name => 'non_hexadecimal_checkpoint_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '0/12G',
	},
	{
		name => 'non_hexadecimal_start_lsn_prefix',
		start_lsn => 'G/1',
		checkpoint_lsn => '0/1',
	},
	{
		name => 'non_hexadecimal_checkpoint_lsn_prefix',
		start_lsn => '0/1',
		checkpoint_lsn => 'G/1',
	},
	{
		name => 'extra_slash_in_start_lsn',
		start_lsn => '0/1/2',
		checkpoint_lsn => '0/1',
	},
	{
		name => 'missing_slash_in_checkpoint_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '01',
	},
	{
		name => 'missing_high_component_in_start_lsn',
		start_lsn => '/1',
		checkpoint_lsn => '0/1',
	},
	{
		name => 'missing_low_component_in_checkpoint_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '0/',
	},
	{
		name => 'whitespace_in_start_lsn',
		start_lsn => '0 /1',
		checkpoint_lsn => '0/1',
	},
	{
		name => 'overlong_start_lsn',
		start_lsn => '123456789/1',
		checkpoint_lsn => '0/1',
	},
	{
		name => 'overlong_checkpoint_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '123456789/1',
	},
	{
		name => 'incremental_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '0/1',
		incremental_lsn => '0/1',
		error => 'this is an incremental backup, not a data directory',
	},
	{
		name => 'invalid_incremental_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '0/1',
		incremental_lsn => '0/0/1',
		error => 'this is an incremental backup, not a data directory',
	},
	{
		name => 'nul_in_incremental_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '0/1',
		incremental_lsn => "0/123\0junk",
		error => 'this is an incremental backup, not a data directory',
	},
	{
		name => 'too_long_incremental_lsn',
		start_lsn => '0/1',
		checkpoint_lsn => '0/1',
		incremental_lsn => '0/1111111111111111111111111111111111111',
		error => 'this is an incremental backup, not a data directory',
	},
);

for my $test (@tests)
{
	my $node = PostgreSQL::Test::Cluster->new($test->{name});
	$node->init;

	open my $fh, '>', $node->data_dir . '/backup_label'
	  or BAIL_OUT("could not create backup_label: $!");
	binmode $fh;
	print $fh
	  "START WAL LOCATION: $test->{start_lsn} "
	  . "(file 000000010000000000000000)\n";
	print $fh "CHECKPOINT LOCATION: $test->{checkpoint_lsn}\n";
	print $fh "INCREMENTAL FROM LSN: $test->{incremental_lsn}\n"
	  if defined $test->{incremental_lsn};
	close $fh or BAIL_OUT("could not close backup_label: $!");

	is($node->start(fail_ok => 1), 0, "$test->{name} prevents startup");
	my $error = $test->{error} // 'invalid data in file "backup_label"';
	ok($node->log_contains(qr/FATAL: .*\Q$error\E/),
		"$test->{name} is rejected");
}

done_testing();
