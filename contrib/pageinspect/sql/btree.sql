CREATE TABLE test1 (a int8, b int4range);
INSERT INTO test1 VALUES (72057594037927937, '[0,1)');
CREATE INDEX test1_a_idx ON test1 USING btree (a);

\x

SELECT * FROM bt_metap('test1_a_idx');

SELECT * FROM bt_page_stats('test1_a_idx', -1);
SELECT * FROM bt_page_stats('test1_a_idx', 0);
SELECT * FROM bt_page_stats('test1_a_idx', 1);
SELECT * FROM bt_page_stats('test1_a_idx', 2);

-- bt_multi_page_stats() function returns a set of records of page statistics.
CREATE TABLE test2 AS (SELECT generate_series(1, 1000)::int8 AS col1);
CREATE INDEX test2_col1_idx ON test2(col1);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 0, 1);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 1, -1);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 1, 0);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 1, 2);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 3, 2);
SELECT * FROM bt_multi_page_stats('test2_col1_idx', 7, 2);
DROP TABLE test2;

SELECT * FROM bt_page_items('test1_a_idx', -1);
SELECT * FROM bt_page_items('test1_a_idx', 0);
SELECT * FROM bt_page_items('test1_a_idx', 1);
SELECT * FROM bt_page_items('test1_a_idx', 2);

SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', -1));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 0));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 1));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 2));

-- Failure when using a non-btree index.
CREATE INDEX test1_a_hash ON test1 USING hash(a);
SELECT bt_metap('test1_a_hash');
SELECT bt_page_stats('test1_a_hash', 0);
SELECT bt_page_items('test1_a_hash', 0);
SELECT bt_page_items(get_raw_page('test1_a_hash', 0));
CREATE INDEX test1_b_gist ON test1 USING gist(b);
-- Special area of GiST is the same as btree, this complains about inconsistent
-- leaf data on the page.
SELECT bt_page_items(get_raw_page('test1_b_gist', 0));

-- Several failure modes.
-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes and architectures.
\set VERBOSITY terse
-- invalid page size
SELECT bt_page_items('aaa'::bytea);
-- invalid special area size
CREATE INDEX test1_a_brin ON test1 USING brin(a);
SELECT bt_page_items(get_raw_page('test1', 0));
SELECT bt_page_items(get_raw_page('test1_a_brin', 0));
-- A corrupt line pointer must be reported, not read out of bounds.  All-ones is
-- an invalid (out-of-range, unaligned) line pointer on any architecture.
SELECT bt_page_items(set_byte(set_byte(set_byte(set_byte(get_raw_page('test1_a_idx', 1), 24, 255), 25, 255), 26, 255), 27, 255));
-- LP_DEAD is never set on a pivot tuple, so a page that has it is corrupt.
-- Block 1 is the first leaf of a multi-page index, and its first item is the
-- high key.  Byte 26 holds the lp_flags of that line pointer.
CREATE TABLE test2 (a int);
INSERT INTO test2 SELECT g FROM generate_series(1, 1000) g;
CREATE INDEX test2_a_idx ON test2 (a);
SELECT bt_page_items(set_byte(get_raw_page('test2_a_idx', 1), 26, 245));
-- A posting list must lie within its tuple.  Equal keys are deduplicated into
-- posting list tuples, and block 1 is the only leaf, so its first line pointer
-- is such a tuple.  First set both bytes of the posting list count to 0xFF,
-- then both bytes of the low half of the posting list offset to 0x1F.  Both
-- values are the same on little and big endian machines.
CREATE TABLE test3 (a int);
INSERT INTO test3 SELECT 1 FROM generate_series(1, 300);
CREATE INDEX test3_a_idx ON test3 (a);
SELECT bt_page_items(set_byte(set_byte(get_raw_page('test3_a_idx', 1),
       (SELECT lp_off FROM heap_page_items(get_raw_page('test3_a_idx', 1))
        WHERE lp = 1) + 4, 255),
       (SELECT lp_off FROM heap_page_items(get_raw_page('test3_a_idx', 1))
        WHERE lp = 1) + 5, 255));
SELECT bt_page_items(set_byte(set_byte(get_raw_page('test3_a_idx', 1),
       (SELECT lp_off FROM heap_page_items(get_raw_page('test3_a_idx', 1))
        WHERE lp = 1) + 2, 31),
       (SELECT lp_off FROM heap_page_items(get_raw_page('test3_a_idx', 1))
        WHERE lp = 1) + 3, 31));
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT bt_page_items(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test1;
DROP TABLE test2;
DROP TABLE test3;
