# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# This tests that vacuumlo correctly removes orphaned large objects while
# preserving references stored in oid columns, lo columns, and complex types
# built over either.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $dbname = 'vacuumlo_test';
$node->safe_psql('postgres', "CREATE DATABASE $dbname");

# Install the lo extension (creates the lo type, recognized by name).
$node->safe_psql($dbname, "CREATE EXTENSION lo");

# Create composite types containing lo fields.
$node->safe_psql($dbname, q{
    CREATE TYPE comp_with_lo AS (label text, loid lo);
    CREATE TYPE nested_comp AS (label text, inner_comp comp_with_lo);
});

# Create tables exercising various patterns.
$node->safe_psql($dbname, q{
    CREATE TABLE t_plain_oid (id serial, loid oid);
    CREATE TABLE t_lo (id serial, loid lo);
    CREATE TABLE t_comp (id serial, val comp_with_lo);
    CREATE TABLE t_nested_comp (id serial, val nested_comp);
    CREATE TABLE t_array_oid (id serial, loids oid[]);
    CREATE TABLE t_array_lo (id serial, loids lo[]);
    CREATE TABLE t_array_comp (id serial, vals comp_with_lo[]);
});

# Create large objects: lo1..lo9 will be referenced, lo10..lo14 are orphans.
my @lo_oids;
for my $i (1 .. 14)
{
    my $oid = $node->safe_psql($dbname, "SELECT lo_create(0)");
    push @lo_oids, $oid;
}

# lo1: plain oid column
$node->safe_psql($dbname,
    "INSERT INTO t_plain_oid (loid) VALUES ('$lo_oids[0]')");

# lo2: lo extension type
$node->safe_psql($dbname,
    "INSERT INTO t_lo (loid) VALUES ('$lo_oids[1]')");

# lo3: composite type with lo field
$node->safe_psql($dbname,
    "INSERT INTO t_comp (val) VALUES (ROW('a', '$lo_oids[2]')::comp_with_lo)");

# lo4: nested composite
$node->safe_psql($dbname,
    "INSERT INTO t_nested_comp (val) VALUES (ROW('b', ROW('c', '$lo_oids[3]')::comp_with_lo)::nested_comp)");

# lo5, lo6: array of oid
$node->safe_psql($dbname,
    "INSERT INTO t_array_oid (loids) VALUES (ARRAY['$lo_oids[4]', '$lo_oids[5]']::oid[])");

# lo7: array of lo
$node->safe_psql($dbname,
    "INSERT INTO t_array_lo (loids) VALUES (ARRAY['$lo_oids[6]']::lo[])");

# lo8, lo9: array of composite with lo
$node->safe_psql($dbname,
    "INSERT INTO t_array_comp (vals) VALUES (ARRAY[ROW('d', '$lo_oids[7]'), ROW('e', '$lo_oids[8]')]::comp_with_lo[])");

# Verify all 14 large objects exist before vacuumlo.
my $count_before = $node->safe_psql($dbname,
    "SELECT count(*) FROM pg_largeobject_metadata");
is($count_before, '14', 'all 14 large objects exist before vacuumlo');

# Run vacuumlo — assert success and check removal message.
command_like(
    ['vacuumlo', '-v', $node->connstr($dbname)],
    qr/Successfully removed 5 large objects/,
    'vacuumlo removes orphan large objects');

# lo10..lo14 (indices 9..13) should have been removed.
my $count_after = $node->safe_psql($dbname,
    "SELECT count(*) FROM pg_largeobject_metadata");
is($count_after, '9', 'only 9 referenced large objects remain after vacuumlo');

# Verify each referenced LO still exists.
for my $i (0 .. 8)
{
    my $exists = $node->safe_psql($dbname,
        "SELECT count(*) FROM pg_largeobject_metadata WHERE oid = $lo_oids[$i]");
    is($exists, '1', "referenced lo (index $i, oid $lo_oids[$i]) still exists");
}

# Verify each orphan LO was removed.
for my $i (9 .. 13)
{
    my $exists = $node->safe_psql($dbname,
        "SELECT count(*) FROM pg_largeobject_metadata WHERE oid = $lo_oids[$i]");
    is($exists, '0', "orphan lo (index $i, oid $lo_oids[$i]) was removed");
}

$node->stop;
done_testing();
