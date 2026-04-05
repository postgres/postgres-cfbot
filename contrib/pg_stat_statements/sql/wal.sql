--
-- Validate WAL generation metrics
--

SET pg_stat_statements.track_utility = FALSE;

CREATE TABLE pgss_wal_tab (a int, b char(20));

INSERT INTO pgss_wal_tab VALUES(generate_series(1, 10), 'aaa');
UPDATE pgss_wal_tab SET b = 'bbb' WHERE a > 7;
DELETE FROM pgss_wal_tab WHERE a > 9;
DROP TABLE pgss_wal_tab;

-- Check WAL is generated for the above statements
SELECT query, calls, rows,
wal_bytes > 0 as wal_bytes_generated,
wal_records > 0 as wal_records_generated,
wal_records >= rows as wal_records_ge_rows
FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- Validate buffer/WAL counting with caught exception in PL/pgSQL
--
CREATE TEMP TABLE pgss_error_tab (a int, b char(20));
INSERT INTO pgss_error_tab VALUES (0, 'zzz');

CREATE FUNCTION pgss_error_func() RETURNS void AS $$
DECLARE
    v int;
BEGIN
    WITH ins AS (INSERT INTO pgss_error_tab VALUES (1, 'aaa') RETURNING a)
    SELECT a / 0 INTO v FROM ins;
EXCEPTION WHEN division_by_zero THEN
    NULL;
END;
$$ LANGUAGE plpgsql;

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT pgss_error_func();

-- Buffer/WAL usage from the wCTE INSERT should survive the exception
SELECT query, calls,
local_blks_hit + local_blks_read > 0 as local_hitread,
wal_bytes > 0 as wal_bytes_generated,
wal_records > 0 as wal_records_generated
FROM pg_stat_statements
WHERE query LIKE '%pgss_error_func%'
ORDER BY query COLLATE "C";

DROP TABLE pgss_error_tab;
DROP FUNCTION pgss_error_func;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
