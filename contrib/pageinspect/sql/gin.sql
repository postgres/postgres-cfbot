CREATE TABLE test1 (x int, y int[]);
INSERT INTO test1 VALUES (1, ARRAY[11, 111]);
CREATE INDEX test1_y_idx ON test1 USING gin (y) WITH (fastupdate = off);

\x

SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 0));
SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_page_opaque_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_leafpage_items(get_raw_page('test1_y_idx', 1));

INSERT INTO test1 SELECT x, ARRAY[1,10] FROM generate_series(2,10000) x;

SELECT COUNT(*) > 0
FROM gin_leafpage_items(get_raw_page('test1_y_idx',
                        (pg_relation_size('test1_y_idx') /
                         current_setting('block_size')::bigint)::int - 1));

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
-- corrupt posting list on the last leaf page.  The first GinPostingList
-- begins 32 bytes into the page, so on any block size its first item's
-- offset is at bytes 36-37 and its varbyte stream starts at byte 40
-- (set_byte is 0-based, overlay 1-based).  Each of these must be reported,
-- not decoded past.
SELECT (pg_relation_size('test1_y_idx') /
        current_setting('block_size')::bigint)::int - 1 AS ln \gset
-- a varbyte stream that never terminates (seven continuation bytes)
SELECT gin_leafpage_items(overlay(get_raw_page('test1_y_idx', :ln)
                                  PLACING '\x80808080808080'::bytea FROM 41));
-- an item that decodes to offset 0, with the first item's offset set to 1
-- and the first delta to 2047, so the running value reaches 2048
SELECT gin_leafpage_items(set_byte(set_byte(set_byte(set_byte(
                                  get_raw_page('test1_y_idx', :ln),
                                  36, 1), 37, 0), 40, 255), 41, 15));
-- a segment whose first item has an invalid offset of 0
SELECT gin_leafpage_items(set_byte(set_byte(
                                  get_raw_page('test1_y_idx', :ln),
                                  36, 0), 37, 0));
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT gin_leafpage_items(decode(repeat('00', :block_size), 'hex'));
SELECT gin_metapage_info(decode(repeat('00', :block_size), 'hex'));
SELECT gin_page_opaque_info(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test1;
