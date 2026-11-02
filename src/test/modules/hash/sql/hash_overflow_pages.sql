-- Test allocation and freeing of hash index overflow pages.
--
-- Searching for a free overflow page and freeing one don't lock the
-- metapage.  When there is no free page, the index is extended by a batch
-- of 64 overflow pages, all but the first left free in the bitmap.
CREATE EXTENSION pageinspect;

-- Size the index for 6600 rows, so the rows added below don't split it.
-- That gives 32 buckets with either 4- or 8-byte MAXALIGN.
CREATE TABLE hash_ovfl_test (v int4) WITH (autovacuum_enabled = false);
INSERT INTO hash_ovfl_test SELECT g FROM generate_series(1, 6600) g;
ANALYZE hash_ovfl_test;
CREATE INDEX hash_ovfl_index ON hash_ovfl_test USING hash (v);

-- The pages after the bitmap page are all overflow pages: their type,
-- whether their bit is set, and whether there is more than one such page
-- (the exact number depends on the tuple width)
CREATE FUNCTION hash_ovfl_pages(OUT type text, OUT bit_set bool,
                                OUT several bool)
RETURNS SETOF record LANGUAGE sql AS $$
  SELECT hash_page_type(get_raw_page('hash_ovfl_index', b::int)),
         (hash_bitmap_info('hash_ovfl_index', b)).bitstatus,
         count(*) > 1
    FROM generate_series(
      (SELECT mapp[1] FROM hash_metapage_info(get_raw_page('hash_ovfl_index', 0))) + 1,
      pg_relation_size('hash_ovfl_index') / current_setting('block_size')::int - 1) b
   GROUP BY 1, 2 ORDER BY 1, 2
$$;
CREATE FUNCTION hash_ovfl_meta(OUT maxbucket int8, OUT firstfree int8,
                               OUT ovflpages int8, OUT index_pages int8)
LANGUAGE sql AS $$
  SELECT maxbucket, firstfree, spares[ovflpoint + 1],
         pg_relation_size('hash_ovfl_index') / current_setting('block_size')::int
    FROM hash_metapage_info(get_raw_page('hash_ovfl_index', 0))
$$;

-- Only the bitmap page counts as an overflow page so far
SELECT * FROM hash_ovfl_meta();
SELECT * FROM hash_ovfl_pages();

-- Fill one bucket past its primary page.  The first overflow page extends
-- the index by 64 pages, and the others come from that batch.
INSERT INTO hash_ovfl_test SELECT 1 FROM generate_series(1, 2000);
SELECT * FROM hash_ovfl_meta();
SELECT * FROM hash_ovfl_pages();

-- Freeing overflow pages clears their bits, and leaves firstfree alone
DELETE FROM hash_ovfl_test WHERE v = 1;
VACUUM hash_ovfl_test;
SELECT * FROM hash_ovfl_meta();
SELECT * FROM hash_ovfl_pages();

-- New overflow pages reuse free ones, so the index does not grow
INSERT INTO hash_ovfl_test SELECT 1 FROM generate_series(1, 1500);
SELECT * FROM hash_ovfl_meta();
SELECT * FROM hash_ovfl_pages();

-- The index still finds every row
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM hash_ovfl_test WHERE v = 1;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*) FROM hash_ovfl_test WHERE v = 1;

DROP TABLE hash_ovfl_test;
DROP FUNCTION hash_ovfl_pages, hash_ovfl_meta;
DROP EXTENSION pageinspect;
