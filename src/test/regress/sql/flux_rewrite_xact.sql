--
-- FLUX: commit, abort and PREPARE work after the relation's storage was
-- replaced in the same transaction (SET ACCESS METHOD, ALTER COLUMN TYPE,
-- CLUSTER, TRUNCATE).
--
-- FLUX remembers the tuples a transaction wrote by relation OID and TID, and
-- at commit it clears a hint bit on each of them, or marks rows a rolled-back
-- savepoint inserted as deleted.  A rewrite keeps the OID but moves the rows
-- to new storage, where the same TIDs name other tuples (or are past the end
-- of the file, or belong to a heap page).  Every case runs on a heap twin and
-- on FLUX, with COMMIT and with ROLLBACK; the rows written before and after
-- the rewrite have equal widths, so their TIDs collide.  The two columns of
-- the result must agree.
--
-- amcheck's verify_heapam checks every table that ends up heap when the
-- extension is available; the result is the same either way.  It goes into a
-- schema of its own, off the search path of the tests running alongside.
CREATE SCHEMA rx_amcheck;
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'amcheck') THEN
    CREATE EXTENSION amcheck SCHEMA rx_amcheck;
  END IF;
EXCEPTION WHEN OTHERS THEN
  NULL;
END $$;

-- A wide value that compresses inline, as heap and FLUX both store it.
CREATE FUNCTION rx_val(k int) RETURNS text LANGUAGE sql IMMUTABLE
  RETURN repeat(md5(k::text), 150);

-- Rows, their ids, the rows whose value is intact, then FOR UPDATE, UPDATE
-- and DELETE counts in the same transaction, and amcheck problems.
CREATE FUNCTION rx_check(t text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
  rel regclass := to_regclass(t);
  vh text := (SELECT format('%I.verify_heapam', n.nspname)
               FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace
               WHERE e.extname = 'amcheck');
  n bigint; ids bigint; ks bigint; good bigint; lck bigint; upd bigint;
  del bigint; bad bigint := 0;
BEGIN
  IF rel IS NULL THEN
    RETURN 'no table';
  END IF;
  IF (SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
      WHERE c.oid = rel) = 'heap'
     AND vh IS NOT NULL THEN
    EXECUTE format('SELECT count(*) FROM %s(%L)', vh, rel) INTO bad;
  END IF;
  EXECUTE format('SELECT count(*), sum(id), sum(k),
                         count(*) FILTER (WHERE w = rx_val(k))
                  FROM %s', rel) INTO n, ids, ks, good;
  EXECUTE format('SELECT count(*) FROM (SELECT 1 FROM %s FOR UPDATE) x', rel)
    INTO lck;
  EXECUTE format('WITH x AS (UPDATE %s SET w = w RETURNING 1)
                  SELECT count(*) FROM x', rel) INTO upd;
  EXECUTE format('WITH x AS (DELETE FROM %s RETURNING 1)
                  SELECT count(*) FROM x', rel) INTO del;
  RETURN format('rows=%s ids=%s ks=%s good=%s lock=%s upd=%s del=%s amcheck=%s',
                n, ids, ks, good, lck, upd, del, bad);
EXCEPTION WHEN OTHERS THEN
  RETURN 'ERROR: ' || regexp_replace(SQLERRM, '(pglz|lz4)', 'lzN');
END $$;

-- Each case: statements before BEGIN, then the transaction body.  $T is the
-- table, $AM its access method.  The base is 20 committed rows, inserted in
-- descending id order so that CLUSTER moves them.
CREATE TABLE rx_case (n int, name text, setup text[], body text[]);
INSERT INTO rx_case VALUES
-- flux -> heap
(1, 'fh', NULL, ARRAY[
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g',
  'ALTER TABLE $T SET ACCESS METHOD heap']),
-- flux -> heap -> flux
(2, 'fhf', NULL, ARRAY[
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g',
  'ALTER TABLE $T SET ACCESS METHOD heap',
  'ALTER TABLE $T SET ACCESS METHOD $AM']),
-- flux -> heap, after rows rolled back to a savepoint
(3, 'sp_fh', NULL, ARRAY[
  'SAVEPOINT a',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g',
  'ROLLBACK TO a',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(41, 60) g',
  'ALTER TABLE $T SET ACCESS METHOD heap']),
-- TRUNCATE, then new rows on the same TIDs
(4, 'trunc', NULL, ARRAY[
  'TRUNCATE $T',
  'INSERT INTO $T SELECT g, g + 100, rx_val(g + 100) FROM generate_series(1, 20) g']),
-- rows rolled back to a savepoint, then TRUNCATE and new rows on their TIDs
(5, 'sp_trunc', NULL, ARRAY[
  'SAVEPOINT a',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g',
  'ROLLBACK TO a',
  'TRUNCATE $T',
  'INSERT INTO $T SELECT g, g + 100, rx_val(g + 100) FROM generate_series(1, 40) g']),
-- ALTER COLUMN TYPE
(6, 'act', NULL, ARRAY[
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g',
  'ALTER TABLE $T ALTER COLUMN id TYPE bigint']),
-- rows rolled back to a savepoint, then a rewrite moves live rows onto their
-- TIDs
(7, 'sp_act', NULL, ARRAY[
  'SAVEPOINT a',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g',
  'ROLLBACK TO a',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(41, 60) g',
  'ALTER TABLE $T ALTER COLUMN id TYPE bigint']),
-- CLUSTER after updates and deletes in place (no rows rolled back)
(8, 'cluster', NULL, ARRAY[
  'UPDATE $T SET k = k + 100, w = rx_val(k + 100) WHERE id % 2 = 0',
  'DELETE FROM $T WHERE id % 5 = 0',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(40, 21, -1) g',
  'CLUSTER $T USING $T_pkey']),
-- TRUNCATE + ALTER TYPE: the rows before TRUNCATE fill blocks the rewritten
-- relation does not have
(9, 'trunc_act', NULL, ARRAY[
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 400) g',
  'TRUNCATE $T',
  'INSERT INTO $T SELECT g, g + 100, rx_val(g + 100) FROM generate_series(1, 20) g',
  'ALTER TABLE $T ALTER COLUMN id TYPE bigint']),
-- the same with those rows rolled back to a savepoint; the new rows go in
-- after the rewrite
(10, 'sp_trunc_act', NULL, ARRAY[
  'SAVEPOINT a',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 400) g',
  'ROLLBACK TO a',
  'TRUNCATE $T',
  'ALTER TABLE $T ALTER COLUMN id TYPE bigint',
  'INSERT INTO $T SELECT g, g + 100, rx_val(g + 100) FROM generate_series(1, 20) g']),
-- a table created in this transaction is truncated in place, keeping its
-- storage: rows rolled back to a savepoint, then new rows on their TIDs
(11, 'inplace', ARRAY[]::text[], ARRAY[
  'CREATE TABLE $T (id int PRIMARY KEY, k int, w text) USING $AM',
  'SAVEPOINT a',
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(1, 20) g',
  'ROLLBACK TO a',
  'TRUNCATE $T',
  'INSERT INTO $T SELECT g, g + 100, rx_val(g + 100) FROM generate_series(1, 20) g']),
-- a rewrite inside a savepoint that rolls back: the old storage is current
-- again, and the rows written to it before the savepoint still commit
(12, 'sp_rewrite_rb', NULL, ARRAY[
  'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(21, 40) g',
  'UPDATE $T SET k = k + 100, w = rx_val(k + 100) WHERE id % 2 = 0',
  'SAVEPOINT a',
  'ALTER TABLE $T SET ACCESS METHOD heap',
  'ROLLBACK TO a',
  'SAVEPOINT b',
  'TRUNCATE $T',
  'ROLLBACK TO b']);

CREATE FUNCTION rx_script() RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
  c rx_case;
  fin text;
  am text;
  t text;
  s text;
BEGIN
  FOR c IN SELECT * FROM rx_case ORDER BY n LOOP
    FOREACH fin IN ARRAY ARRAY['COMMIT', 'ROLLBACK'] LOOP
      FOREACH am IN ARRAY ARRAY['heap', 'flux'] LOOP
        t := format('rx_%s_%s_%s', c.name, lower(fin), am);
        FOREACH s IN ARRAY coalesce(c.setup, ARRAY[
            'CREATE TABLE $T (id int PRIMARY KEY, k int, w text) USING $AM',
            'INSERT INTO $T SELECT g, g, rx_val(g) FROM generate_series(20, 1, -1) g'])
            || ARRAY['BEGIN'] || c.body || fin LOOP
          RETURN NEXT replace(replace(s, '$T', t), '$AM', am);
        END LOOP;
      END LOOP;
    END LOOP;
  END LOOP;
END $$;

SELECT rx_script() \gexec

\pset format unaligned
SELECT name, fin, heap, CASE WHEN flux = heap THEN 'same' ELSE flux END AS flux
FROM (SELECT c.n, c.name, f.fin,
             rx_check(format('rx_%s_%s_heap', c.name, lower(f.fin))) AS heap,
             rx_check(format('rx_%s_%s_flux', c.name, lower(f.fin))) AS flux
      FROM rx_case c, (VALUES ('COMMIT'), ('ROLLBACK')) f(fin)) r
ORDER BY n, fin;
\pset format aligned

-- Clean up.
SELECT format('DROP TABLE %I', relname) FROM pg_class
WHERE relname ~ '^rx_.*_(heap|flux)$' AND relkind = 'r' ORDER BY 1 \gexec
DROP FUNCTION rx_script(), rx_check(text), rx_val(int);
DROP TABLE rx_case;
SET client_min_messages = warning;
DROP SCHEMA rx_amcheck CASCADE;
RESET client_min_messages;
