-- bitmap_and_mixed: quantifies the cost of the per-block
-- VISIBILITYMAP_LOCATOR_SPLIT BitmapAnd signal specifically, not the SIU
-- write-path win the other workloads measure.
--
-- 80% of transactions force a BitmapAnd across siu_b and siu_d (both never
-- updated by this workload) with a wide-enough predicate range to make the
-- planner pick a bitmap plan over a plain index scan.  20% of transactions
-- HOT-indexed-update column c (a third, separately-indexed column), which is
-- what plants a may-be-stale-flagged fresh entry in siu_c.  siu_c is not
-- part of the BitmapAnd predicate here on purpose: the split fix's cost
-- shows up in whether flagged pages elsewhere on the SAME heap page as an
-- update also lose exact-mode precision on OTHER indexes' bitmap
-- contributions for unrelated rows sharing that page, which is exactly what
-- reset_state's freshly-clustered layout (a and b both increasing with row
-- order) puts multiple rows per heap page under.
\set aid random(1, :scale * 100000)
\set cid random(1, :scale * 100000)
\set lo random(1, :scale * 100000 - 1000)
\set hi_off random(1, 1000)
\set which random(1, 100)
\if :which > 80
  UPDATE siu_table SET c = :cid WHERE a = :aid;
\else
  SELECT count(*) FROM siu_table WHERE b BETWEEN :lo::int AND :lo::int + :hi_off::int AND d BETWEEN :lo::int AND :lo::int + :hi_off::int;
\endif
