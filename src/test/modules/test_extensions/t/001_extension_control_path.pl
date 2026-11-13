# Copyright (c) 2024-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Path qw(mkpath);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');

$node->init('auth_extra' => [ '--create-role', 'user01' ]);

# Create temporary directories for the extension control files
my $ext_dir = PostgreSQL::Test::Utils::tempdir();
mkpath("$ext_dir/extension");
my $ext_dir2 = PostgreSQL::Test::Utils::tempdir();
mkpath("$ext_dir2/extension");

my $ext_name = "test_custom_ext_paths";
create_extension($ext_name, $ext_dir);
create_extension($ext_name, $ext_dir2);

my $ext_name2 = "test_custom_ext_paths_using_directory";
mkpath("$ext_dir/$ext_name2");
create_extension($ext_name2, $ext_dir, $ext_name2);

# Make windows path use Unix slashes as canonicalize_path() is called when
# collecting extension control paths. See get_extension_control_directories().
my $ext_dir_canonicalized = $ext_dir;
$ext_dir_canonicalized =~ s!\\!/!g if $windows_os;

# Use the correct separator and escape \ when running on Windows.
my $sep = $windows_os ? ";" : ":";
$node->append_conf(
	'postgresql.conf', qq{
extension_control_path = '\$system$sep@{[ $windows_os ? ($ext_dir =~ s/\\/\\\\/gr) : $ext_dir ]}$sep@{[ $windows_os ? ($ext_dir2 =~ s/\\/\\\\/gr) : $ext_dir2 ]}'
});

# Start node
$node->start;

# Create a user to test permissions to read extension locations.
my $user = "user01";
$node->safe_psql('postgres', "CREATE USER $user");

my $ecp = $node->safe_psql('postgres', 'show extension_control_path;');

is( $ecp,
	"\$system$sep$ext_dir$sep$ext_dir2",
	"custom extension control directory path configured");

$node->safe_psql('postgres', "CREATE EXTENSION $ext_name");
$node->safe_psql('postgres', "CREATE EXTENSION $ext_name2");

my $ret = $node->safe_psql('postgres',
	"select * from pg_available_extensions where name = '$ext_name'");
is( $ret,
	"test_custom_ext_paths|1.0|1.0|$ext_dir_canonicalized/extension|Test extension_control_path",
	"extension is shown correctly in pg_available_extensions");

$ret = $node->safe_psql('postgres',
	"select * from pg_available_extension_versions where name = '$ext_name'");
is( $ret,
	"test_custom_ext_paths|1.0|t|t|f|t|||$ext_dir_canonicalized/extension|Test extension_control_path",
	"extension is shown correctly in pg_available_extension_versions");

$ret = $node->safe_psql('postgres',
	"select * from pg_available_extensions where name = '$ext_name2'");
is( $ret,
	"test_custom_ext_paths_using_directory|1.0|1.0|$ext_dir_canonicalized/extension|Test extension_control_path",
	"extension is shown correctly in pg_available_extensions");

$ret = $node->safe_psql('postgres',
	"select * from pg_available_extension_versions where name = '$ext_name2'"
);
is( $ret,
	"test_custom_ext_paths_using_directory|1.0|t|t|f|t|||$ext_dir_canonicalized/extension|Test extension_control_path",
	"extension is shown correctly in pg_available_extension_versions");

# Test that a non-superuser is not able to read the extension location in
# pg_available_extensions
$ret = $node->safe_psql(
	'postgres',
	"select location from pg_available_extensions where name = '$ext_name2'",
	connstr => "user=$user");
is( $ret,
	"<insufficient privilege>",
	"extension location is hidden in pg_available_extensions for users with insufficient privilege"
);

# Test that a non-superuser is not able to read the extension location in
# pg_available_extension_versions
$ret = $node->safe_psql(
	'postgres',
	"select location from pg_available_extension_versions where name = '$ext_name2'",
	connstr => "user=$user");
is( $ret,
	"<insufficient privilege>",
	"extension location is hidden in pg_available_extension_versions for users with insufficient privilege"
);

# Ensure that extensions installed in $system are still visible when used with
# custom extension control path.
$ret = $node->safe_psql('postgres',
	"select count(*) > 0 as ok from pg_available_extensions where name = 'plpgsql'"
);
is($ret, "t",
	"\$system extension is shown correctly in pg_available_extensions");

$ret = $node->safe_psql('postgres',
	"set extension_control_path = ''; select location from pg_available_extensions where name = 'plpgsql'"
);
is($ret, "\$system",
	"\$system location is shown correctly in pg_available_extensions with empty extension_control_path"
);

# Empty path elements do not prevent searching the remaining directories.
for my $path (
	"$sep$ext_dir_canonicalized",
	"$ext_dir_canonicalized$sep")
{
	$ret = $node->safe_psql(
		'postgres',
		"set extension_control_path = '$path';
		 select location from pg_available_extensions where name = '$ext_name'");
	is($ret, "$ext_dir_canonicalized/extension",
		"extension is found with an empty element in path '$path'");
}

# A separator-only value provides no directories, unlike an empty value.
$ret = $node->safe_psql(
	'postgres',
	"set extension_control_path = '$sep';
	 select count(*) from pg_available_extensions");
is($ret, '0', 'separator-only path lists no extensions');

# Invalid elements must not replace an unknown-language error or prevent a hint
# from being found using the remaining directories.
for my $path (
	"$sep$ext_dir_canonicalized$sep",
	"relative$sep$ext_dir_canonicalized${sep}relative")
{
	for my $sql (
		"create function test_invalid_language() returns int language $ext_name as 'x'",
		"do language $ext_name 'x'")
	{
		my ($code, $stdout, $stderr) = $node->psql(
			'postgres',
			"set extension_control_path = '$path'; $sql");
		is($code, 3, "unknown language is rejected by $sql");
		like($stderr, qr/ERROR:  language "\Q$ext_name\E" does not exist/,
			"invalid path does not replace the unknown-language error");
		like($stderr,
			qr/HINT:  Use CREATE EXTENSION to load the language into the database\./,
			"remaining directory provides the extension hint");
	}
}

# An extension found only through a relative path must not be listed or loaded.
my $relative_dir = $node->data_dir . '/relative';
mkpath("$relative_dir/extension");
my $relative_ext = 'test_relative_path';
create_extension($relative_ext, $relative_dir);
$ret = $node->safe_psql(
	'postgres',
	"set extension_control_path = '\$system${sep}relative';
	 select count(*) from pg_available_extensions where name = '$relative_ext'");
is($ret, '0', 'extension in a relative directory is not listed');

{
	my ($code, $stdout, $stderr) = $node->psql(
		'postgres',
		"set extension_control_path = '\$system${sep}relative';
		 create extension $relative_ext");
	is($code, 3, 'extension in a relative directory cannot be loaded');
	like($stderr,
		qr/ERROR:  extension "\Q$relative_ext\E" is not available/,
		'relative directory is excluded from the extension search');
}

# The same extension can be found and loaded through an absolute path, even
# with relative elements before or after that path.
create_extension($relative_ext, $ext_dir);
for my $path (
	"relative$sep$ext_dir_canonicalized",
	"$ext_dir_canonicalized${sep}relative")
{
	$ret = $node->safe_psql(
		'postgres',
		"set extension_control_path = '$path';
		 select location from pg_available_extensions where name = '$relative_ext'");
	is($ret, "$ext_dir_canonicalized/extension",
		"extension is listed from the absolute directory in path '$path'");
	$ret = $node->safe_psql(
		'postgres',
		"set extension_control_path = '$path';
		 create extension $relative_ext;
		 select count(*) from pg_extension where extname = '$relative_ext';
		 drop extension $relative_ext");
	is($ret, '1', "extension can be loaded using path '$path'");
}

# Test with an extension that does not exists
my ($code, $stdout, $stderr) =
  $node->psql('postgres', "CREATE EXTENSION invalid");
is($code, 3, 'error creating an extension that does not exist');
like($stderr, qr/ERROR:  extension "invalid" is not available/);

sub create_extension
{
	my ($ext_name, $ext_dir, $directory) = @_;

	my $control_file = "$ext_dir/extension/$ext_name.control";
	my $sql_file;

	if (defined $directory)
	{
		$sql_file = "$ext_dir/$directory/$ext_name--1.0.sql";
	}
	else
	{
		$sql_file = "$ext_dir/extension/$ext_name--1.0.sql";
	}

	# Create .control .sql file
	open my $cf, '>', $control_file
	  or die "Could not create control file: $!";
	print $cf "comment = 'Test extension_control_path'\n";
	print $cf "default_version = '1.0'\n";
	print $cf "relocatable = true\n";
	print $cf "directory = $directory" if defined $directory;
	close $cf;

	# Create --1.0.sql file
	open my $sqlf, '>', $sql_file or die "Could not create sql file: $!";
	print $sqlf "/* $sql_file */\n";
	print $sqlf
	  "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n";
	print $sqlf
	  qq'\\echo Use "CREATE EXTENSION $ext_name" to load this file. \\quit\n';
	close $sqlf;
}

done_testing();
