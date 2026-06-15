-- hot-indexed-friendly workload: narrow table with a few non-PK indexes.
-- Each UPDATE changes a non-summarizing indexed column on a random row.
-- With hot-indexed this is HOT-indexed; without hot-indexed it is non-HOT.
\set aid random(1, :scale * 100000)
\set new_b random(1, 1000000)
UPDATE siu_table SET b = :new_b WHERE a = :aid;
