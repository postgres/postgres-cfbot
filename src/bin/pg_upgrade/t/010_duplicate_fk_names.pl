# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that pg_upgrade --check reports NOT VALID foreign keys on partitions
# that share their name with a foreign key on the parent partitioned table.
#
# Such constraints can only be created before PostgreSQL 18, so this test
# needs "oldinstall" to point to an installation of version 11 through 17.
# Otherwise it is skipped.

use strict;
use warnings FATAL => 'all';

use File::Glob ':bsd_glob';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'oldinstall must point to a PostgreSQL 11-17 installation'
  unless defined $ENV{oldinstall};

my $old =
  PostgreSQL::Test::Cluster->new('old', install_path => $ENV{oldinstall});
my $new = PostgreSQL::Test::Cluster->new('new');

plan skip_all => 'old cluster must be between versions 11 and 17'
  if $old->pg_version->major < 11 || $old->pg_version->major > 17;

# Checksums are now enabled by default, but weren't before 18.
$old->init(extra => ['-k']);
$new->init();

$old->start;
$old->safe_psql(
	'postgres', q{
	CREATE SCHEMA xxi;
	CREATE TABLE xxi.acc (id int NOT NULL, CONSTRAINT acc_pkey PRIMARY KEY (id));
	CREATE TABLE xxi.trn (acc_id int NOT NULL) PARTITION BY RANGE (acc_id);
	CREATE TABLE xxi.trn_part1 (acc_id int NOT NULL);
	ALTER TABLE ONLY xxi.trn ATTACH PARTITION xxi.trn_part1
	  FOR VALUES FROM (1) TO (100);
	-- unlinked NOT VALID FK on the partition, same name as the parent's FK
	ALTER TABLE ONLY xxi.trn_part1 ADD CONSTRAINT fk_trn_acc
	  FOREIGN KEY (acc_id) REFERENCES xxi.acc (id) NOT VALID;
	ALTER TABLE xxi.trn ADD CONSTRAINT fk_trn_acc
	  FOREIGN KEY (acc_id) REFERENCES xxi.acc (id);

	-- same problem with names containing dots, which must be quoted in the
	-- report, and with the partition in another schema
	CREATE SCHEMA c;
	CREATE SCHEMA "a.b";
	CREATE TABLE "a.b"."t.x" (acc_id int NOT NULL) PARTITION BY RANGE (acc_id);
	CREATE TABLE c."p.y" (acc_id int NOT NULL);
	ALTER TABLE ONLY "a.b"."t.x" ATTACH PARTITION c."p.y"
	  FOR VALUES FROM (1) TO (100);
	ALTER TABLE ONLY c."p.y" ADD CONSTRAINT "fk.z"
	  FOREIGN KEY (acc_id) REFERENCES xxi.acc (id) NOT VALID;
	ALTER TABLE "a.b"."t.x" ADD CONSTRAINT "fk.z"
	  FOREIGN KEY (acc_id) REFERENCES xxi.acc (id);

	-- these must not be reported: a VALID FK is linked to the parent's FK
	CREATE TABLE xxi.trn2 (acc_id int NOT NULL) PARTITION BY RANGE (acc_id);
	CREATE TABLE xxi.trn2_part1 (acc_id int NOT NULL);
	ALTER TABLE ONLY xxi.trn2 ATTACH PARTITION xxi.trn2_part1
	  FOR VALUES FROM (1) TO (100);
	ALTER TABLE ONLY xxi.trn2_part1 ADD CONSTRAINT fk_trn2_acc
	  FOREIGN KEY (acc_id) REFERENCES xxi.acc (id);
	ALTER TABLE xxi.trn2 ADD CONSTRAINT fk_trn2_acc
	  FOREIGN KEY (acc_id) REFERENCES xxi.acc (id);
	-- a NOT VALID FK that exists only on the partition
	CREATE TABLE xxi.trn3 (acc_id int NOT NULL) PARTITION BY RANGE (acc_id);
	CREATE TABLE xxi.trn3_part1 (acc_id int NOT NULL);
	ALTER TABLE ONLY xxi.trn3 ATTACH PARTITION xxi.trn3_part1
	  FOR VALUES FROM (1) TO (100);
	ALTER TABLE ONLY xxi.trn3_part1 ADD CONSTRAINT fk_trn3_acc
	  FOREIGN KEY (acc_id) REFERENCES xxi.acc (id) NOT VALID;
});
$old->stop;

my @pg_upgrade = (
	'pg_upgrade', '--no-sync',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $old->config_data('--bindir'),
	'--new-bindir' => $new->config_data('--bindir'),
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port,
);

command_fails([ @pg_upgrade, '--check' ],
	'pg_upgrade --check fails on duplicate foreign key name');

my ($report) =
  bsd_glob($new->data_dir . '/pg_upgrade_output.d/*/duplicate_fk_names.txt');
ok(defined $report && -e $report, 'report file was written')
  or BAIL_OUT('no report file');
my $contents = slurp_file($report);
like(
	$contents,
	qr/^  table xxi\.trn_part1, constraint fk_trn_acc$/m,
	'offending constraint listed');
like(
	$contents,
	qr/^  table c\."p\.y", constraint "fk\.z"$/m,
	'names with dots are quoted in the report');
unlike($contents, qr/trn2_part1|trn3_part1/,
	'valid and partition-only constraints not listed');
rmtree($new->data_dir . '/pg_upgrade_output.d');

# Apply the fixes suggested by the error message and retry
$old->start;
$old->safe_psql(
	'postgres', q{
	ALTER TABLE ONLY xxi.trn_part1 DROP CONSTRAINT fk_trn_acc;
	ALTER TABLE ONLY c."p.y" RENAME CONSTRAINT "fk.z" TO fk_z_part;
});
$old->stop;

command_ok([ @pg_upgrade, '--check' ],
	'pg_upgrade --check passes after fixing the partition constraints');
command_ok([@pg_upgrade], 'pg_upgrade succeeds');

done_testing();
