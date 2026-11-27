-- Tests for automatic TOAST dictionary evolution during ANALYZE: an opted-in
-- column retrains its dictionary from ANALYZE's sample, adopts a candidate
-- only when a held-out size test shows a margin win, rejects churn when no
-- candidate wins, and stops at the retained-dictionary cap.  See
-- toast_dictionary_maybe_evolve().

SELECT NOT(enumvals @> '{zstd}') AS skip_test FROM pg_settings WHERE
  name = 'default_toast_compression' \gset
\if :skip_test
   \echo '*** skipping TOAST dictionary evolution tests (not supported) ***'
   \quit
\endif

CREATE SCHEMA dict_evolve;
SET search_path TO dict_evolve, public;

-- No cooldown, so that each ANALYZE below may retrain; the per-column options
-- set later accept any growth and lower the row floor.  The 5% margin keeps
-- its default so the held-out size test is the real adoption decision.
SET dictionary_evolution_min_interval = 0;

-- Two visibly different value distributions, each wide enough to be TOASTed
-- and compressed on disk (a few kB), so the test exercises real
-- dictionary compression and not just catalog-row churn.  A dictionary
-- trained on one compresses it well and the other poorly, so switching the
-- stored distribution produces a candidate that wins the held-out test.
CREATE FUNCTION docA(i int) RETURNS text LANGUAGE SQL IMMUTABLE AS $$
  SELECT repeat(
    format('{"customer_id": %s, "status": "active", "region": "us-east", '
           '"plan": "enterprise", "features": ["sso", "audit", "backup"], '
           '"env": "production", "tier": "gold", "shard": "a-%s"}' || chr(10),
           i, i % 7), 40)
$$;
CREATE FUNCTION docB(i int) RETURNS text LANGUAGE SQL IMMUTABLE AS $$
  SELECT repeat(
    format('<record seq="%s"><owner>warehouse</owner><kind>pallet</kind>'
           '<zone>cold-storage</zone><lane>%s</lane><status>in-transit</status>'
           '<carrier>freightways</carrier><priority>standard</priority></record>' || chr(10),
           i, i % 7), 40)
$$;

CREATE TABLE evo (id int, f1 text COMPRESSION zstd_dict)
  WITH (autovacuum_enabled = off);
ALTER TABLE evo ALTER COLUMN f1 SET (dictionary_evolution = on,
                                     dictionary_evolution_growth = 1.0,
                                     dictionary_evolution_min_rows = 50);
SELECT 'evo'::regclass::oid AS evo_oid \gset

-- no dictionary yet
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = :evo_oid;

-- A fixed set of rows, distribution A.  ANALYZE adopts the first dictionary
-- unconditionally once the row floor is cleared.
INSERT INTO evo SELECT g, docA(g) FROM generate_series(1, 4000) g;
ANALYZE evo;
SELECT count(*) AS ndicts, count(*) FILTER (WHERE tdcurrent) AS ncurrent
  FROM pg_toast_dictionary WHERE tdrelid = :evo_oid;
SELECT oid AS dicta FROM pg_toast_dictionary
  WHERE tdrelid = :evo_oid AND tdcurrent \gset

-- The rows were inserted before there was a dictionary, so they are plain
-- zstd.  Rewrite them, so that they are compressed with dictionary A, and
-- confirm they are wide enough to be compressed on disk (stored smaller than
-- raw), so the test exercises real on-disk compression and not just
-- catalog-row churn on sub-threshold values.
UPDATE evo SET f1 = f1 || '';
SELECT pg_column_compression(f1) AS cm,
       bool_and(pg_column_size(f1) < octet_length(f1)) AS compressed_on_disk,
       count(*)
  FROM evo GROUP BY 1;

-- Same distribution again: a re-trained candidate does not beat the current
-- dictionary by the margin, so nothing is adopted.  The gates restart all the
-- same, so that the next ANALYZE does not train again on the same data.
SELECT tdtraintime AS traintime_a FROM pg_toast_dictionary
  WHERE tdrelid = :evo_oid AND tdcurrent \gset
ANALYZE evo;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = :evo_oid;
SELECT oid = :dicta AS still_same_current, tdtraintime > :traintime_a AS gates_restarted
  FROM pg_toast_dictionary WHERE tdrelid = :evo_oid AND tdcurrent;

-- Drift: add a batch of distribution-B rows (the A rows stay, so values
-- compressed with the A dictionary coexist with B rows on disk).  ANALYZE
-- should find a candidate that beats the A-trained dictionary on the held-out
-- B-dominated sample and adopt it: the count rises and the current dictionary
-- changes.
INSERT INTO evo SELECT g, docB(g) FROM generate_series(4001, 12000) g;
ANALYZE evo;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = :evo_oid;
SELECT oid <> :dicta AS current_changed FROM pg_toast_dictionary
  WHERE tdrelid = :evo_oid AND tdcurrent;
SELECT oid AS dictb FROM pg_toast_dictionary
  WHERE tdrelid = :evo_oid AND tdcurrent \gset

-- Both generations read back correctly: the A rows, still compressed with
-- the now non-current dictionary A, and the B rows, which were inserted
-- while A was current and so are compressed with it too.  New rows use B.
INSERT INTO evo SELECT g, docB(g) FROM generate_series(12001, 12100) g;
SELECT id <= 4000 AS a_rows, pg_column_compression(f1) AS cm, count(*)
  FROM evo GROUP BY 1, 2 ORDER BY 1, 2;
SELECT bool_and(f1 = docA(id)) AS a_ok FROM evo WHERE id <= 4000;
SELECT bool_and(f1 = docB(id)) AS b_ok FROM evo WHERE id > 4000;
SELECT count(*) FILTER (WHERE NOT tdcurrent) AS retired_kept,
       count(*) FILTER (WHERE tdcurrent) AS ncurrent
  FROM pg_toast_dictionary WHERE tdrelid = :evo_oid;
-- A rewrite recompresses every value with B and drops A.
VACUUM FULL evo;
SELECT count(*) AS ndicts_after_rewrite, bool_and(oid = :dictb) AS only_b
  FROM pg_toast_dictionary WHERE tdrelid = :evo_oid;
SELECT bool_and(f1 = CASE WHEN id <= 4000 THEN docA(id) ELSE docB(id) END) AS all_ok
  FROM evo;

-- Stable again on B: ANALYZE must not churn.
ANALYZE evo;
SELECT oid = :dictb AS still_same_current FROM pg_toast_dictionary
  WHERE tdrelid = :evo_oid AND tdcurrent;

-- Cap: with a low cap, repeated drift keeps adopting only until the cap is
-- reached, after which evolution declines and the catalog stops growing.  The
-- cap message goes to the server log; here we just check the bound holds.
SET dictionary_evolution_max_retained = 3;
DO $$
DECLARE
  i int;
BEGIN
  FOR i IN 1..6 LOOP
    IF i % 2 = 0 THEN
      UPDATE evo SET f1 = docA(id);
    ELSE
      UPDATE evo SET f1 = docB(id);
    END IF;
    ANALYZE evo;
  END LOOP;
END $$;
SELECT count(*) AS ndicts_at_cap,
       count(*) FILTER (WHERE tdcurrent) AS ncurrent
  FROM pg_toast_dictionary WHERE tdrelid = :evo_oid;
SELECT bool_and(f1 = docA(id)) AS all_equal FROM evo;

-- Reaching the cap restarts only the cooldown, not the growth baseline, so
-- once a rewrite has removed the superseded dictionaries, evolution resumes
-- without waiting for the table to double again: the next ANALYZE considers
-- a candidate, and records the row count it was considered at.
SET dictionary_evolution_max_retained = 2;
CREATE TABLE evocapr (id int, f1 text COMPRESSION zstd_dict)
  WITH (autovacuum_enabled = off);
ALTER TABLE evocapr ALTER COLUMN f1 SET (dictionary_evolution = on,
                                         dictionary_evolution_growth = 2.0,
                                         dictionary_evolution_min_rows = 100);
INSERT INTO evocapr SELECT g, docA(g) FROM generate_series(1, 500) g;
ANALYZE evocapr;
INSERT INTO evocapr SELECT g, docB(g) FROM generate_series(501, 2000) g;
ANALYZE evocapr;
-- the table has doubled, but the column is at the cap
UPDATE evocapr SET f1 = docA(id);
INSERT INTO evocapr SELECT g, docA(g) FROM generate_series(2001, 4000) g;
ANALYZE evocapr;
SELECT count(*) AS ndicts_capped, max(tdtrainrows) AS baseline_kept
  FROM pg_toast_dictionary WHERE tdrelid = 'evocapr'::regclass;
VACUUM FULL evocapr;
ANALYZE evocapr;
SELECT count(*) AS ndicts_after_rewrite, max(tdtrainrows) AS considered_at
  FROM pg_toast_dictionary WHERE tdrelid = 'evocapr'::regclass;
SELECT bool_and(f1 = docA(id)) AS values_ok FROM evocapr;
RESET dictionary_evolution_max_retained;
DROP TABLE evocapr;

-- A dictionary trained by hand before the table was ever analyzed records no
-- row count.  The first ANALYZE takes its count as the baseline for growth,
-- instead of seeing growth from zero, and keeps the hand-trained dictionary.
-- It does so even while the table is below the row floor.
CREATE TABLE evo2 (id int, f1 text COMPRESSION zstd_dict)
  WITH (autovacuum_enabled = off);
ALTER TABLE evo2 ALTER COLUMN f1 SET (dictionary_evolution = on,
                                      dictionary_evolution_min_rows = 5000);
INSERT INTO evo2 SELECT g, docB(g) FROM generate_series(1, 2000) g;
SELECT pg_train_toast_dictionary('evo2', 'f1', 4096) AS dict2 \gset
SELECT tdtrainrows FROM pg_toast_dictionary WHERE oid = :dict2;
ANALYZE evo2;
SELECT oid = :dict2 AS kept, tdtrainrows > 0 AS baseline_set
  FROM pg_toast_dictionary WHERE tdrelid = 'evo2'::regclass AND tdcurrent;

-- pg_toast_dictionaries shows the training time as a timestamptz
SELECT trained_rows > 0 AS rows_known,
       trained_at BETWEEN now() - interval '1 hour' AND now() + interval '1 hour' AS recent
  FROM pg_toast_dictionaries
  WHERE schemaname = 'dict_evolve' AND tablename = 'evo2' AND is_current;

-- TRUNCATE resets the recorded row count, so that the table's new contents,
-- not its size before the truncation, are the baseline for growth.
SELECT tdtrainrows > 0 AS has_baseline FROM pg_toast_dictionary
  WHERE tdrelid = 'evo2'::regclass AND tdcurrent;
TRUNCATE evo2;
SELECT tdtrainrows AS baseline_after_truncate FROM pg_toast_dictionary
  WHERE tdrelid = 'evo2'::regclass AND tdcurrent;

-- A populated REFRESH MATERIALIZED VIEW keeps the recorded row count: a
-- matview grows only across refreshes, so clearing the baseline on every
-- refresh would strand evolution at the first dictionary.  A refresh that
-- leaves the matview empty (WITH NO DATA) resets it as TRUNCATE does.
CREATE MATERIALIZED VIEW evomv AS SELECT id, docB(id)::text AS f1 FROM generate_series(1, 300) id;
ALTER MATERIALIZED VIEW evomv ALTER COLUMN f1 SET COMPRESSION zstd_dict;
REFRESH MATERIALIZED VIEW evomv;
ANALYZE evomv;
SELECT pg_train_toast_dictionary('evomv', 'f1', 4096) IS NOT NULL AS mv_trained;
SELECT tdtrainrows AS mv_baseline FROM pg_toast_dictionary
  WHERE tdrelid = 'evomv'::regclass AND tdcurrent;
REFRESH MATERIALIZED VIEW evomv;
SELECT tdtrainrows AS mv_baseline_kept_on_refresh FROM pg_toast_dictionary
  WHERE tdrelid = 'evomv'::regclass AND tdcurrent;
REFRESH MATERIALIZED VIEW evomv WITH NO DATA;
SELECT tdtrainrows AS mv_baseline_reset_on_no_data FROM pg_toast_dictionary
  WHERE tdrelid = 'evomv'::regclass AND tdcurrent;
DROP MATERIALIZED VIEW evomv;

-- The retained-dictionary cap bounds manual training by non-superusers too.
CREATE ROLE regress_evo_maint;
GRANT USAGE ON SCHEMA dict_evolve TO regress_evo_maint;
CREATE TABLE evocap (id int, f1 text COMPRESSION zstd_dict);
INSERT INTO evocap SELECT g, docA(g) FROM generate_series(1, 300) g;
GRANT MAINTAIN ON evocap TO regress_evo_maint;
SET dictionary_evolution_max_retained = 2;
SET ROLE regress_evo_maint;
SELECT pg_train_toast_dictionary('dict_evolve.evocap', 'f1', 4096) IS NOT NULL AS t1;
SELECT pg_train_toast_dictionary('dict_evolve.evocap', 'f1', 4096) IS NOT NULL AS t2;
SELECT pg_train_toast_dictionary('dict_evolve.evocap', 'f1', 4096) IS NOT NULL AS t3;
RESET ROLE;
SELECT pg_train_toast_dictionary('evocap', 'f1', 4096) IS NOT NULL AS superuser_t3;
RESET dictionary_evolution_max_retained;
DROP TABLE evocap;
DROP OWNED BY regress_evo_maint;
DROP ROLE regress_evo_maint;

-- Nothing is installed in a read-only transaction.
CREATE TABLE evo3 (id int, f1 text COMPRESSION zstd_dict)
  WITH (autovacuum_enabled = off);
ALTER TABLE evo3 ALTER COLUMN f1 SET (dictionary_evolution = on,
                                      dictionary_evolution_min_rows = 50);
INSERT INTO evo3 SELECT g, docA(g) FROM generate_series(1, 500) g;
BEGIN TRANSACTION READ ONLY;
ANALYZE evo3;
COMMIT;
SELECT count(*) AS ndicts_read_only FROM pg_toast_dictionary
  WHERE tdrelid = 'evo3'::regclass;
ANALYZE evo3;
SELECT count(*) AS ndicts_read_write FROM pg_toast_dictionary
  WHERE tdrelid = 'evo3'::regclass;

RESET dictionary_evolution_max_retained;
RESET dictionary_evolution_min_interval;
RESET search_path;
DROP SCHEMA dict_evolve CASCADE;
