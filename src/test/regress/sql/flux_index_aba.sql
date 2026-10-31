--
-- FLUX: an indexed column that returns to an earlier value (A -> B -> A)
-- inside one transaction, then is rolled back.
--
-- An in-place key UPDATE keeps the row at its TID: it inserts (k_new, TID),
-- or revives a delete-marked (k_new, TID) left by an earlier UPDATE of the
-- same row, and delete-marks (k_old, TID).  Rolling the UPDATEs back must
-- put every index back as it was: one live entry for the row's key, and
-- nothing that an index scan or amcheck could tell from a heap table that
-- had done the same.  Each FLUX table here has a heap twin that receives the
-- same statements; heap never reuses a TID, so it is the control.
--
-- aba_check() reports 'ok' or what is wrong: for both tables, an index scan
-- through the index under test must return what a sequential scan returns,
-- for every key the row has held; bt_index_check(heapallindexed) must pass
-- on every btree index (when amcheck is available); and the FLUX table must
-- hold what its heap twin holds.
--
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_available_extensions WHERE name = 'amcheck') AND
       NOT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'amcheck') THEN
        CREATE EXTENSION amcheck;
        CREATE TABLE aba_created_amcheck ();
    END IF;
END $$;

-- One FLUX/heap pair per index kind.  k is the column that cycles; j stays 0.
CREATE FUNCTION aba_make(kind text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    am text;
    t text;
BEGIN
    FOREACH am IN ARRAY ARRAY['flux', 'heap'] LOOP
        t := am || '_' || kind;
        EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, j int, k int) USING %s', t, am);
        EXECUTE format('INSERT INTO %I SELECT g, 0, g FROM generate_series(1, 40) g', t);
        EXECUTE format(CASE kind
            WHEN 'plain'   THEN 'CREATE INDEX %I ON %I (k)'
            WHEN 'uniq'    THEN 'CREATE UNIQUE INDEX %I ON %I (k)'
            WHEN 'multi'   THEN 'CREATE INDEX %I ON %I (j, k)'
            WHEN 'expr'    THEN 'CREATE INDEX %I ON %I ((k * 2))'
            WHEN 'partial' THEN 'CREATE INDEX %I ON %I (k) WHERE k < 1000'
            WHEN 'hash'    THEN 'CREATE INDEX %I ON %I USING hash (k)'
            END, t || '_ix', t);
    END LOOP;
END $$;

CREATE FUNCTION aba_drop(kind text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE format('DROP TABLE %I, %I', 'flux_' || kind, 'heap_' || kind);
END $$;

-- Set row id's k on both tables of the pair.
CREATE FUNCTION aba_set(kind text, rid int, newk int) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE format('UPDATE %I SET k = $1 WHERE id = $2', 'flux_' || kind) USING newk, rid;
    EXECUTE format('UPDATE %I SET k = $1 WHERE id = $2', 'heap_' || kind) USING newk, rid;
END $$;

-- Try an UPDATE on one table; report whether it raised unique_violation.
CREATE FUNCTION aba_try(t text, rid int, newk int) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE format('UPDATE %I SET k = $1 WHERE id = $2', t) USING newk, rid;
    RETURN 'updated';
EXCEPTION WHEN unique_violation THEN
    RETURN 'unique_violation';
END $$;

CREATE FUNCTION aba_count(q text, idxscan bool) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    n bigint;
BEGIN
    PERFORM set_config('enable_seqscan', CASE WHEN idxscan THEN 'off' ELSE 'on' END, true);
    PERFORM set_config('enable_indexscan', CASE WHEN idxscan THEN 'on' ELSE 'off' END, true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    PERFORM set_config('enable_indexonlyscan', 'off', true);
    EXECUTE 'SELECT count(*) FROM ' || q INTO n;
    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_bitmapscan', 'on', true);
    PERFORM set_config('enable_indexonlyscan', 'on', true);
    RETURN n;
END $$;

CREATE FUNCTION aba_check(kind text, keys int[]) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    am text;
    t text;
    key int;
    q text;
    ni bigint;
    ns bigint;
    i regclass;
    res text := '';
    fx text;
    hp text;
BEGIN
    FOREACH am IN ARRAY ARRAY['flux', 'heap'] LOOP
        t := am || '_' || kind;
        FOREACH key IN ARRAY keys LOOP
            -- a partial index holds only keys < 1000
            CONTINUE WHEN kind = 'partial' AND key >= 1000;
            q := format('%I WHERE ', t) || CASE kind
                WHEN 'multi' THEN format('j = 0 AND k = %s', key)
                WHEN 'expr'  THEN format('k * 2 = 2 * %s', key)
                ELSE format('k = %s', key) END;
            ni := aba_count(q, true);
            ns := aba_count(q, false);
            IF ni <> ns THEN
                res := res || format('%s k=%s: idxscan %s seqscan %s; ', t, key, ni, ns);
            END IF;
        END LOOP;
        FOR i IN SELECT indexrelid FROM pg_index x JOIN pg_class c ON c.oid = x.indexrelid
                 JOIN pg_am a ON a.oid = c.relam
                 WHERE x.indrelid = t::regclass AND a.amname = 'btree' ORDER BY 1 LOOP
            IF to_regprocedure('bt_index_check(regclass,boolean)') IS NOT NULL THEN
                BEGIN
                    PERFORM bt_index_check(i, true);
                EXCEPTION WHEN OTHERS THEN
                    res := res || format('%s: %s; ', i, SQLERRM);
                END;
            END IF;
        END LOOP;
    END LOOP;
    EXECUTE format('SELECT string_agg(format(''%%s:%%s:%%s'', id, j, k), '','' ORDER BY id) FROM %I', 'flux_' || kind) INTO fx;
    EXECUTE format('SELECT string_agg(format(''%%s:%%s:%%s'', id, j, k), '','' ORDER BY id) FROM %I', 'heap_' || kind) INTO hp;
    IF fx IS DISTINCT FROM hp THEN
        res := res || 'flux and heap rows differ; ';
    END IF;
    RETURN CASE WHEN res = '' THEN 'ok' ELSE res END;
END $$;

-- The plan aba_count(q, true) gets: a plain index scan of the index under
-- test.  Index-only scans are off, as a heap table could otherwise answer
-- from the index alone, and a FLUX table never does.
CREATE FUNCTION aba_plan(kind text, key int) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
    q text;
    l text;
BEGIN
    q := format('%I WHERE ', 'flux_' || kind) || CASE kind
        WHEN 'multi' THEN format('j = 0 AND k = %s', key)
        WHEN 'expr'  THEN format('k * 2 = 2 * %s', key)
        ELSE format('k = %s', key) END;
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    PERFORM set_config('enable_indexonlyscan', 'off', true);
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT count(*) FROM ' || q LOOP
        RETURN NEXT l;
    END LOOP;
    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_bitmapscan', 'on', true);
    PERFORM set_config('enable_indexonlyscan', 'on', true);
END $$;

CREATE VIEW aba_kinds AS SELECT unnest(ARRAY['plain', 'uniq', 'multi', 'expr', 'partial', 'hash']) AS kind;

SELECT aba_make(kind) FROM aba_kinds;
SELECT kind, aba_plan(kind, 12) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- Every case starts from fresh tables (amcheck reports only the first
-- missing entry) and checks the indexes before and after a VACUUM, which
-- reclaims the tombstones a case leaves and must keep the indexes intact.
-- B = A + 3000000 and C = A + 4000000 lie outside the partial index.

-- 1. A -> B -> A, ROLLBACK
SELECT aba_make(kind) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 12, 3000012), aba_set(kind, 12, 12)
  FROM aba_kinds;
ROLLBACK;
SELECT kind, aba_check(kind, ARRAY[12, 3000012]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[12, 3000012]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 2. A -> B -> C -> A, ROLLBACK
SELECT aba_make(kind) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 13, 3000013), aba_set(kind, 13, 4000013), aba_set(kind, 13, 13)
  FROM aba_kinds;
ROLLBACK;
SELECT kind, aba_check(kind, ARRAY[13, 3000013, 4000013]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[13, 3000013, 4000013]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 3. A -> B, then B -> A in a savepoint that is rolled back, COMMIT: B
SELECT aba_make(kind) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 14, 3000014)
  FROM aba_kinds;
SAVEPOINT s;
SELECT aba_set(kind, 14, 14)
  FROM aba_kinds;
ROLLBACK TO s;
COMMIT;
SELECT kind, aba_check(kind, ARRAY[14, 3000014]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[14, 3000014]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 4. A -> B -> C, then C -> A in a savepoint that is rolled back, COMMIT: C
SELECT aba_make(kind) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 15, 3000015), aba_set(kind, 15, 4000015)
  FROM aba_kinds;
SAVEPOINT s;
SELECT aba_set(kind, 15, 15)
  FROM aba_kinds;
ROLLBACK TO s;
COMMIT;
SELECT kind, aba_check(kind, ARRAY[15, 3000015, 4000015]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[15, 3000015, 4000015]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 5. A -> B, then B -> C -> A in a savepoint that is rolled back, COMMIT: B
SELECT aba_make(kind) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 16, 3000016)
  FROM aba_kinds;
SAVEPOINT s;
SELECT aba_set(kind, 16, 4000016), aba_set(kind, 16, 16)
  FROM aba_kinds;
ROLLBACK TO s;
COMMIT;
SELECT kind, aba_check(kind, ARRAY[16, 3000016, 4000016]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[16, 3000016, 4000016]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 6. COMMIT controls: A -> B -> A and A -> B -> C -> A
SELECT aba_make(kind) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 17, 3000017), aba_set(kind, 17, 17),
       aba_set(kind, 18, 3000018), aba_set(kind, 18, 4000018), aba_set(kind, 18, 18)
  FROM aba_kinds;
COMMIT;
SELECT kind, aba_check(kind, ARRAY[17, 3000017, 18, 3000018, 4000018]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[17, 3000017, 18, 3000018, 4000018]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 7. Committed A -> C, then C -> A rolled back: C
SELECT aba_make(kind) FROM aba_kinds;
SELECT aba_set(kind, 19, 4000019)
  FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 19, 19)
  FROM aba_kinds;
ROLLBACK;
SELECT kind, aba_check(kind, ARRAY[19, 4000019]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[19, 4000019]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 8. The same row cycled and rolled back round after round, then committed
-- back to A: no round may lose an entry or leave a live one behind.
SELECT aba_make(kind) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 20, 3000020), aba_set(kind, 20, 20)
  FROM aba_kinds;
ROLLBACK;
SELECT kind, aba_check(kind, ARRAY[20, 3000020]) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 20, 3000020), aba_set(kind, 20, 20), aba_set(kind, 20, 3000020)
  FROM aba_kinds;
ROLLBACK;
SELECT kind, aba_check(kind, ARRAY[20, 3000020]) FROM aba_kinds;
BEGIN;
SELECT aba_set(kind, 20, 3000020), aba_set(kind, 20, 20)
  FROM aba_kinds;
COMMIT;
SELECT kind, aba_check(kind, ARRAY[20, 3000020]) FROM aba_kinds;
VACUUM flux_plain, flux_uniq, flux_multi, flux_expr, flux_partial, flux_hash,
       heap_plain, heap_uniq, heap_multi, heap_expr, heap_partial, heap_hash;
SELECT kind, aba_check(kind, ARRAY[20, 3000020]) FROM aba_kinds;
SELECT aba_drop(kind) FROM aba_kinds;

-- 9. Unique index.  A key the row has left is free for another row, and the
-- row cannot take it back while that row holds it.  An aborted A -> B -> A
-- keeps A taken: no other row may take it.
SELECT aba_make('uniq');
SELECT aba_set('uniq', 21, 3000021), aba_set('uniq', 22, 21);
SELECT aba_try('flux_uniq', 21, 21) AS flux, aba_try('heap_uniq', 21, 21) AS heap;
BEGIN;
SELECT aba_set('uniq', 23, 3000023), aba_set('uniq', 23, 23);
ROLLBACK;
SELECT aba_try('flux_uniq', 24, 23) AS flux, aba_try('heap_uniq', 24, 23) AS heap;
-- once the row has moved off it again (committed), the key is free
SELECT aba_set('uniq', 23, 3000023);
SELECT aba_try('flux_uniq', 24, 23) AS flux, aba_try('heap_uniq', 24, 23) AS heap;
SELECT aba_check('uniq', ARRAY[21, 22, 3000021, 23, 24, 3000023]);
VACUUM flux_uniq, heap_uniq;
SELECT aba_check('uniq', ARRAY[21, 22, 3000021, 23, 24, 3000023]);
SELECT aba_drop('uniq');

DROP VIEW aba_kinds;
DROP FUNCTION aba_make, aba_drop, aba_set, aba_try, aba_count, aba_check, aba_plan;
DO $$
BEGIN
    IF to_regclass('aba_created_amcheck') IS NOT NULL THEN
        DROP EXTENSION amcheck;
        DROP TABLE aba_created_amcheck;
    END IF;
END $$;
