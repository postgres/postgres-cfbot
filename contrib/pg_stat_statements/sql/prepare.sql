-- Tests for PREPARE

SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Test that prepared statements in a multi-query string behaves as expected
SELECT 1\;PREPARE p1 AS SELECT 1\; PREPARE p2(int) AS SELECT 2 * $1\; SELECT 1, 1;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

SELECT pg_stat_statements_reset() IS NOT NULL AS t;

EXECUTE p1;
EXECUTE p2(0);

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
