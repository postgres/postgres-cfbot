--
-- HOT_UPDATES
-- Test classic Heap-Only Tuple (HOT) update decisions
--
-- This file covers HOT decisions that apply identically on a pre-hot-indexed
-- server: every UPDATE here either leaves all indexed attributes
-- unchanged or touches only summarizing-index (BRIN) attributes, so the
-- HOT vs non-HOT choice does not depend on whether Selective Index
-- Update (hot-indexed) is enabled.  hot-indexed-specific behaviour (UPDATEs that modify
-- a non-summarizing indexed attribute) is covered in
-- hot_indexed_updates.sql.
--
-- Validation methods:
--   1. Statistics (pg_stat_get_tuples_hot_updated)
--   2. pageinspect for HOT chain structure
--   3. EXPLAIN to confirm the planner still picks the index
--

-- Load required extensions
CREATE EXTENSION IF NOT EXISTS pageinspect;

-- Sum of committed and in-progress (non-HOT, HOT) update counters.
CREATE OR REPLACE FUNCTION get_hot_count(rel_name text)
RETURNS TABLE (
    updates BIGINT,
    hot BIGINT
) AS $$
DECLARE
    rel_oid oid;
BEGIN
    rel_oid := rel_name::regclass::oid;
    updates := COALESCE(pg_stat_get_tuples_updated(rel_oid), 0) +
               COALESCE(pg_stat_get_xact_tuples_updated(rel_oid), 0);
    hot := COALESCE(pg_stat_get_tuples_hot_updated(rel_oid), 0) +
           COALESCE(pg_stat_get_xact_tuples_hot_updated(rel_oid), 0);
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- True iff target_ctid is the TAIL of a HOT chain on the same page.
CREATE OR REPLACE FUNCTION has_hot_chain(rel_name text, target_ctid tid)
RETURNS boolean AS $$
DECLARE
    block_num int;
    page_item record;
BEGIN
    block_num := (target_ctid::text::point)[0]::int;
    FOR page_item IN
        SELECT lp, lp_flags, t_ctid
        FROM heap_page_items(get_raw_page(rel_name, block_num))
        WHERE lp_flags = 1
          AND t_ctid IS NOT NULL
          AND t_ctid = target_ctid
          AND ('(' || block_num::text || ',' || lp::text || ')')::tid != target_ctid
    LOOP
        RETURN true;
    END LOOP;
    RETURN false;
END;
$$ LANGUAGE plpgsql;

-- Emit the HOT chain rooted at start_ctid.
CREATE OR REPLACE FUNCTION print_hot_chain(rel_name text, start_ctid tid)
RETURNS TABLE(chain_position int, ctid tid, lp_flags text, t_ctid tid, chain_end boolean) AS
$$
#variable_conflict use_column
DECLARE
    block_num int;
    line_ptr int;
    current_ctid tid := start_ctid;
    next_ctid tid;
    position int := 0;
    max_iterations int := 100;
    page_item record;
    found_predecessor boolean := false;
    flags_name text;
BEGIN
    block_num := (start_ctid::text::point)[0]::int;

    FOR page_item IN
        SELECT lp, lp_flags, t_ctid
        FROM heap_page_items(get_raw_page(rel_name, block_num))
        WHERE lp_flags = 1
          AND t_ctid = start_ctid
    LOOP
        current_ctid := ('(' || block_num::text || ',' || page_item.lp::text || ')')::tid;
        found_predecessor := true;
        EXIT;
    END LOOP;
    IF NOT found_predecessor THEN
        current_ctid := start_ctid;
    END IF;

    WHILE position < max_iterations LOOP
        line_ptr := (current_ctid::text::point)[1]::int;
        FOR page_item IN
            SELECT lp, lp_flags, t_ctid
            FROM heap_page_items(get_raw_page(rel_name, block_num))
            WHERE lp = line_ptr
        LOOP
            flags_name := CASE page_item.lp_flags
                WHEN 0 THEN 'unused (0)'
                WHEN 1 THEN 'normal (1)'
                WHEN 2 THEN 'redirect (2)'
                WHEN 3 THEN 'dead (3)'
                ELSE 'unknown (' || page_item.lp_flags::text || ')'
            END;
            RETURN QUERY SELECT
                position,
                current_ctid,
                flags_name,
                page_item.t_ctid,
                (page_item.t_ctid IS NULL OR page_item.t_ctid = current_ctid)::boolean;

            IF page_item.t_ctid IS NULL OR page_item.t_ctid = current_ctid THEN
                RETURN;
            END IF;
            next_ctid := page_item.t_ctid;
            IF (next_ctid::text::point)[0]::int != block_num THEN
                RETURN;
            END IF;
            current_ctid := next_ctid;
            position := position + 1;
        END LOOP;
        IF position = 0 THEN
            RETURN;
        END IF;
    END LOOP;
END;
$$ LANGUAGE plpgsql;


-- ---------------------------------------------------------------------------
-- Basic HOT: update of a non-indexed column
-- ---------------------------------------------------------------------------
CREATE TABLE hot_test (
    id int PRIMARY KEY,
    indexed_col int,
    non_indexed_col text
) WITH (fillfactor = 50);
CREATE INDEX hot_test_indexed_idx ON hot_test(indexed_col);

INSERT INTO hot_test VALUES (1, 100, 'initial');
INSERT INTO hot_test VALUES (2, 200, 'initial');
INSERT INTO hot_test VALUES (3, 300, 'initial');

SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

-- Three classic HOT updates (non-indexed col).
UPDATE hot_test SET non_indexed_col = 'updated1' WHERE id = 1;
UPDATE hot_test SET non_indexed_col = 'updated2' WHERE id = 2;
UPDATE hot_test SET non_indexed_col = 'updated3' WHERE id = 3;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

-- Chain-of-1 on id=1 still has a predecessor line pointer.
WITH current_tuple AS (SELECT ctid FROM hot_test WHERE id = 1)
SELECT has_hot_chain('hot_test', current_tuple.ctid) AS has_chain,
       chain_position, print_hot_chain.ctid, lp_flags, t_ctid
FROM current_tuple, LATERAL print_hot_chain('hot_test', current_tuple.ctid);

-- VACUUM collapses the chain.
VACUUM hot_test;

WITH current_tuple AS (SELECT ctid FROM hot_test WHERE id = 1)
SELECT has_hot_chain('hot_test', current_tuple.ctid) AS has_chain,
       chain_position, print_hot_chain.ctid, lp_flags, t_ctid
FROM current_tuple, LATERAL print_hot_chain('hot_test', current_tuple.ctid);

DROP TABLE hot_test;

-- ---------------------------------------------------------------------------
-- Summarizing indexes (BRIN) do not block HOT
-- ---------------------------------------------------------------------------
CREATE TABLE hot_test (
    id int PRIMARY KEY,
    ts timestamp,
    value int,
    brin_col int
) WITH (fillfactor = 50);
CREATE INDEX hot_test_ts_brin ON hot_test USING brin(ts);
CREATE INDEX hot_test_brin_col_brin ON hot_test USING brin(brin_col);

INSERT INTO hot_test VALUES (1, '2024-01-01', 100, 1000);

-- BRIN columns are summarizing; updating them stays classic HOT even
-- though their values change.
UPDATE hot_test SET ts = '2024-01-02', brin_col = 2000 WHERE id = 1;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

-- Non-indexed column: also HOT.
UPDATE hot_test SET value = 200 WHERE id = 1;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

DROP TABLE hot_test;

-- ---------------------------------------------------------------------------
-- TOAST participates in HOT (non-indexed column paths only)
-- ---------------------------------------------------------------------------
CREATE TABLE hot_test (
    id int PRIMARY KEY,
    indexed_col int,
    large_text text,
    small_text text
) WITH (fillfactor = 50);
CREATE INDEX hot_test_idx ON hot_test(indexed_col);

INSERT INTO hot_test VALUES (1, 100, repeat('x', 3000), 'small');

-- Non-indexed, non-TOAST column: HOT.
UPDATE hot_test SET small_text = 'updated';
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

-- TOAST column, indexed_col unchanged: HOT.
UPDATE hot_test SET large_text = repeat('y', 3000);
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

DROP TABLE hot_test;

-- ---------------------------------------------------------------------------
-- Partial index where update leaves indexed attrs unchanged
-- ---------------------------------------------------------------------------
CREATE TABLE hot_test (
    id int PRIMARY KEY,
    status text,
    data text
) WITH (fillfactor = 50);
CREATE INDEX hot_test_active_idx ON hot_test(status) WHERE status = 'active';

INSERT INTO hot_test VALUES (1, 'active', 'data1');
INSERT INTO hot_test VALUES (2, 'inactive', 'data2');
INSERT INTO hot_test VALUES (3, 'deleted', 'data3');

-- Update data on a row whose status matches the partial predicate: HOT.
UPDATE hot_test SET data = 'updated1' WHERE id = 1;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

-- Update data on a row outside the predicate: HOT.
UPDATE hot_test SET data = 'updated2' WHERE id = 2;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

SELECT id, status FROM hot_test WHERE status = 'active';

DROP TABLE hot_test;

-- ---------------------------------------------------------------------------
-- Multi-column btree: update of non-indexed column
-- ---------------------------------------------------------------------------
CREATE TABLE hot_test (
    id int PRIMARY KEY,
    col_a int,
    col_b int,
    col_c int,
    data text
) WITH (fillfactor = 50);
CREATE INDEX hot_test_ab_idx ON hot_test(col_a, col_b);

INSERT INTO hot_test VALUES (1, 10, 20, 30, 'data');

-- col_c not in any index: HOT.
UPDATE hot_test SET col_c = 35;
-- data not in any index: HOT.
UPDATE hot_test SET data = 'updated';
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

DROP TABLE hot_test;

-- ---------------------------------------------------------------------------
-- Unique index: update of non-indexed column + uniqueness enforcement
-- ---------------------------------------------------------------------------
CREATE TABLE hot_test (
    id int PRIMARY KEY,
    unique_col int UNIQUE,
    data text
) WITH (fillfactor = 50);

INSERT INTO hot_test VALUES (1, 100, 'data1');
INSERT INTO hot_test VALUES (2, 200, 'data2');

UPDATE hot_test SET data = 'updated';
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test');

SELECT id, unique_col, data FROM hot_test ORDER BY id;

-- Unique constraint still enforced on any path.
UPDATE hot_test SET unique_col = 100 WHERE id = 2;

DROP TABLE hot_test;

-- ---------------------------------------------------------------------------
-- Partitioned tables: HOT within a partition
-- ---------------------------------------------------------------------------
CREATE TABLE hot_test_partitioned (
    id int,
    partition_key int,
    indexed_col int,
    data text,
    PRIMARY KEY (id, partition_key)
) PARTITION BY RANGE (partition_key);

CREATE TABLE hot_test_part1 PARTITION OF hot_test_partitioned
    FOR VALUES FROM (1) TO (100) WITH (fillfactor = 50);
CREATE TABLE hot_test_part2 PARTITION OF hot_test_partitioned
    FOR VALUES FROM (100) TO (200) WITH (fillfactor = 50);

CREATE INDEX hot_test_part_idx ON hot_test_partitioned(indexed_col);

INSERT INTO hot_test_partitioned VALUES (1, 50, 100, 'initial1');
INSERT INTO hot_test_partitioned VALUES (2, 150, 200, 'initial2');

UPDATE hot_test_partitioned SET data = 'updated1' WHERE id = 1;
UPDATE hot_test_partitioned SET data = 'updated2' WHERE id = 2;

SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test_part1');
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_test_part2');

SELECT id FROM hot_test_partitioned WHERE indexed_col = 100;
SELECT id FROM hot_test_partitioned WHERE indexed_col = 200;

DROP TABLE hot_test_partitioned CASCADE;

-- ---------------------------------------------------------------------------
-- JSONB expression index: any change to the indexed column is non-HOT
-- ---------------------------------------------------------------------------
CREATE TABLE hot_jsonb_test (
    id int PRIMARY KEY,
    data jsonb
) WITH (fillfactor = 50);
CREATE INDEX hot_jsonb_name_idx ON hot_jsonb_test ((data->>'name'));

INSERT INTO hot_jsonb_test VALUES
    (1, '{"name":"Alice","age":30,"city":"NYC"}'),
    (2, '{"name":"Bob","age":25,"city":"LA"}');

-- The jsonb column is the expression index's input, so HOT-indexed is
-- disqualified (expression indexes are not yet supported) and the jsonb
-- change blocks classic HOT: non-HOT update.
UPDATE hot_jsonb_test SET data = jsonb_set(data, '{age}', '31') WHERE id = 1;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_jsonb_test');

-- Likewise non-HOT: expression index disqualifies HOT-indexed.
UPDATE hot_jsonb_test SET data = data - 'city' WHERE id = 2;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_jsonb_test');

-- Likewise non-HOT: expression index disqualifies HOT-indexed.
UPDATE hot_jsonb_test SET data = jsonb_insert(data, '{country}', '"USA"') WHERE id = 2;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_jsonb_test');

DROP TABLE hot_jsonb_test;


-- ---------------------------------------------------------------------------
-- A change to a GIN-indexed column is HOT-indexed
--
-- The read side filters a stale leaf via the crossed-attribute bitmap, which
-- is access-method agnostic, so a GIN-covered column is HOT-indexed like any
-- other: only the GIN index is maintained, and a GIN scan (which rechecks on
-- the heap) returns correct results across the chain.
-- ---------------------------------------------------------------------------
CREATE TABLE hot_gin_test (
    id int PRIMARY KEY,
    tags text[],
    properties jsonb
) WITH (fillfactor = 50);
CREATE INDEX hot_gin_tags_idx ON hot_gin_test USING gin (tags);
CREATE INDEX hot_gin_props_idx ON hot_gin_test USING gin (properties);

INSERT INTO hot_gin_test VALUES
    (1, ARRAY['tag1', 'tag2'], '{"key1":"val1","key2":"val2"}'),
    (2, ARRAY['tag3', 'tag4'], '{"key3":"val3","key4":"val4"}');

-- Reorder tags: a GIN-covered column changes, so this is HOT-indexed.
UPDATE hot_gin_test SET tags = ARRAY['tag2', 'tag1'] WHERE id = 1;
SELECT pg_stat_force_next_flush();
SELECT * FROM get_hot_count('hot_gin_test');

DROP TABLE hot_gin_test;

-- ---------------------------------------------------------------------------
-- Cleanup
-- ---------------------------------------------------------------------------
DROP FUNCTION has_hot_chain(text, tid);
DROP FUNCTION print_hot_chain(text, tid);
DROP FUNCTION get_hot_count(text);
-- Leave pageinspect installed: this runs inside the pageinspect regress suite
-- (page.sql created it), and other tests in the schedule still need it.
