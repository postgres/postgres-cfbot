
# Copyright (c) 2026, PostgreSQL Global Development Group

# A dictionary-compressed datum names its dictionary by OID.  If such a datum
# could outlive its dictionary, a later dictionary given the same OID would
# decode it with the wrong bytes, which is wrong data rather than an error.
#
# Force that case.  In an ordinary (atomic) function, which keeps record
# fields as they come from the table, train a dictionary inside an EXCEPTION
# block, compress a value with it, hold the value in a record, and roll the
# block back.  While the record is still held, and before anything reads it,
# install a different dictionary, trained on other data for another table, at
# exactly the OID of the one that was rolled back.  Binary-upgrade mode is
# used only to choose that OID.  The record must still read back the original
# value, which it does only if it was decompressed before the rollback.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

if ($node->safe_psql('postgres',
		"SELECT NOT (enumvals @> '{zstd}') FROM pg_settings WHERE name = 'default_toast_compression'"
	) eq 't')
{
	plan skip_all => 'server built without zstd';
}

# Two tables with unlike content, so their dictionaries differ.
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION doc(i int, tag text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT format('{"id": %s, "tag": "%s", "status": "active", "notes": "%s"}',
                i, tag, repeat(tag || ' lorem ipsum dolor sit amet ', 60))
$$;
CREATE TABLE a (id int, f text COMPRESSION zstd_dict);
INSERT INTO a SELECT g, doc(g, 'alpha') FROM generate_series(1, 400) g;
CREATE TABLE b (id int, f text COMPRESSION zstd_dict);
INSERT INTO b SELECT g, doc(g, 'omega ' || md5(g::text)) FROM generate_series(1, 400) g;
SELECT pg_train_toast_dictionary('b', 'f', 4096);
CREATE TABLE b_dict AS
  SELECT tddict FROM pg_toast_dictionary WHERE tdrelid = 'b'::regclass;
});

# Restart in binary-upgrade mode so binary_upgrade_add_toast_dictionary() is
# callable from the function while it still holds the record.  Objects are
# created before the restart, since binary-upgrade mode wants preassigned
# OIDs for new relations.
$node->stop;
$node->start(options => '-b');

my $out = $node->safe_psql(
	'postgres', q{
CREATE FUNCTION reuse_test() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
  rec record;
  dead oid;
BEGIN
  BEGIN
    PERFORM pg_train_toast_dictionary('a', 'f', 4096);
    SELECT oid INTO dead FROM pg_toast_dictionary
      WHERE tdrelid = 'a'::regclass AND tdcurrent;
    UPDATE a SET f = f || '' WHERE id = 7;
    SELECT * INTO rec FROM a WHERE id = 7;
    RAISE EXCEPTION 'roll back';
  EXCEPTION WHEN raise_exception THEN NULL;
  END;
  -- Read nothing from rec yet: the dangerous case is a value first read
  -- after its dictionary's OID has gone to another dictionary.

  -- b's dictionary bytes, installed for b at the dead OID
  PERFORM binary_upgrade_add_toast_dictionary('b'::regclass, 2::int2, dead,
            false, 0::int8, 0::int8, (SELECT tddict FROM b_dict));

  RETURN format('%s|%s',
    EXISTS (SELECT 1 FROM pg_toast_dictionary
             WHERE oid = dead AND tdrelid = 'b'::regclass),
    rec.f IS NOT DISTINCT FROM doc(7, 'alpha'));
END $$;
SELECT reuse_test();
});

is($out, 't|t',
	'record first read after its rolled-back dictionary OID was reused reads back exactly'
);

# Table a holds no value that names a dictionary any more.
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM a WHERE pg_column_compression(f) = 'zstd_dict'"
	),
	'0',
	'no value of a names a dictionary after the rollback');
is( $node->safe_psql(
		'postgres', "SELECT bool_and(f = doc(id, 'alpha')) FROM a"),
	't',
	'table a reads back exactly');

$node->stop;
done_testing();
