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
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT bt_page_items(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test1;

-- A prefix-compressed leaf: data items are shown decoded, so data and htid
-- read as on a plain page, while itemlen shows the stored (coded) size.
CREATE TABLE test_prefix (u text COLLATE "C");
INSERT INTO test_prefix
  SELECT 'https://example.com/path/' || lpad(g::text, 4, '0')
  FROM generate_series(1, 20) g;
CREATE INDEX test_prefix_idx ON test_prefix (u) WITH (prefix_compression = on);
CREATE INDEX test_plain_idx ON test_prefix (u);
SELECT btpo_flags & 512 <> 0 AS coded FROM bt_page_stats('test_prefix_idx', 1);
-- same decoded items as the plain index (offset 1 is the prefix directory)
SELECT count(*) AS mismatches
  FROM bt_page_items('test_prefix_idx', 1) c
  FULL JOIN bt_page_items('test_plain_idx', 1) p ON p.itemoffset = c.itemoffset - 1
  WHERE c.itemoffset > 1 AND (c.data IS DISTINCT FROM p.data OR c.htid IS DISTINCT FROM p.htid);
-- and the same from a raw page image
SELECT count(*) AS mismatches
  FROM bt_page_items(get_raw_page('test_prefix_idx', 1)) c
  JOIN bt_page_items('test_prefix_idx', 1) r USING (itemoffset)
  WHERE c.data IS DISTINCT FROM r.data;
-- coded items are smaller than the plain ones
SELECT (SELECT sum(itemlen) FROM bt_page_items('test_prefix_idx', 1) WHERE itemoffset > 1) <
       (SELECT sum(itemlen) FROM bt_page_items('test_plain_idx', 1)) AS smaller;
DROP TABLE test_prefix;
