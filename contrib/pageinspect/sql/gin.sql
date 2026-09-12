CREATE TABLE test1 (x int, y int[], z text[]);
INSERT INTO test1 VALUES (1, ARRAY[11, 111], ARRAY['a', 'b', 'c']);
INSERT INTO test1 VALUES (2, ARRAY[NULL, 222], ARRAY['d', NULL]);
CREATE INDEX test1_y_idx ON test1 USING gin (y) WITH (fastupdate = off);
CREATE INDEX test2_y_z_idx ON test1 USING gin (y, z) WITH (fastupdate = off);
CREATE INDEX test3_y_z_idx ON test1 USING gin (y, z) WITH (fastupdate = on);

\x

SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 0));
SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_page_opaque_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_leafpage_items(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_entrypage_items(get_raw_page('test1_y_idx', 1), 'test1_y_idx'::regclass);

SELECT * FROM gin_entrypage_items(get_raw_page('test2_y_z_idx', 1), 'test2_y_z_idx'::regclass);

INSERT INTO test1 SELECT x, ARRAY[1,10] FROM generate_series(2,10000) x;

SELECT COUNT(*) > 0
FROM gin_leafpage_items(get_raw_page('test1_y_idx',
                        (pg_relation_size('test1_y_idx') /
                         current_setting('block_size')::bigint)::int - 1));

-- Now test posting tree non-leaf page.
-- This requires inserting many tuples on a single leaf page to trigger page split.

CREATE TABLE test_data_page(i INT[]);
CREATE INDEX test_data_page_i_idx ON test_data_page USING gin(i) WITH (fastupdate = off);

INSERT INTO test_data_page SELECT ARRAY[1] FROM generate_series(1, 10000);

-- For this index, block 0 is metapage, block 1 is entry tree, block 2 is
-- posting tree non-leaf page and block 3 & 4 are compressed data leaf pages.
SELECT * FROM gin_datapage_items(get_raw_page('test_data_page_i_idx', 2));

-- Failure with various modes.
-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes and architectures.
\set VERBOSITY terse
-- invalid page size
SELECT gin_leafpage_items('aaa'::bytea);
SELECT gin_metapage_info('bbb'::bytea);
SELECT gin_page_opaque_info('ccc'::bytea);
-- invalid special area size
SELECT * FROM gin_metapage_info(get_raw_page('test1', 0));
SELECT * FROM gin_page_opaque_info(get_raw_page('test1', 0));
SELECT * FROM gin_leafpage_items(get_raw_page('test1', 0));
\set VERBOSITY default

-- Reject unsupported page types in gin_entrypage_items.
SELECT * FROM gin_entrypage_items(get_raw_page('test2_y_z_idx', 0), 'test2_y_z_idx'::regclass);
-- Check the error message for the internal posting tree page.
SELECT * FROM gin_entrypage_items(get_raw_page('test_data_page_i_idx', 2), 'test_data_page_i_idx'::regclass);
-- insert new row to trigger new (fast-list) page allocation.
INSERT INTO test1 VALUES (1, ARRAY[11, 111], ARRAY['a', 'b', 'c']);
-- double check that the new page is fast-list.
SELECT * FROM gin_page_opaque_info(get_raw_page('test3_y_z_idx', 2));
-- reject fast-list pages.
SELECT * FROM gin_entrypage_items(get_raw_page('test3_y_z_idx', 3), 'test3_y_z_idx'::regclass);

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT gin_leafpage_items(decode(repeat('00', :block_size), 'hex'));
SELECT gin_datapage_items(decode(repeat('00', :block_size), 'hex'));
SELECT gin_metapage_info(decode(repeat('00', :block_size), 'hex'));
SELECT gin_page_opaque_info(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test1;
