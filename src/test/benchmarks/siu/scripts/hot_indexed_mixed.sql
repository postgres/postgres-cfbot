-- Mixed workload: 80% selects, 20% indexed-column updates.
-- Exercises both the hot-indexed writer and the crossed-attribute-bitmap reader.
\set aid random(1, :scale * 100000)
\set bid random(1, 1000000)
\set which random(1, 100)
BEGIN;
SELECT * FROM siu_table WHERE a = :aid;
\if :which > 80
  UPDATE siu_table SET b = :bid WHERE a = :aid;
\endif
COMMIT;
