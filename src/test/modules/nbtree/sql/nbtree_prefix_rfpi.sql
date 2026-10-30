--
-- Test the forced full-page-image fallback for prefix-compressed leaves.
--
-- A prefix-compressed leaf can, in principle, hold more logical (decoded)
-- data than fits on a plain page.  When a split's right half or a
-- deduplication rewrite would have to log more than one page of plain
-- items, the WAL record instead carries the coded page as a forced
-- full-page image (XLH_SPLIT_RFPI / XLH_DEDUP_RFPI).  Realistic data never
-- reaches that threshold -- the split-point balance keeps a half well under
-- a page -- so the only way to exercise the path deterministically is the
-- nbtree-prefix-force-rfpi injection point, which forces the fallback for
-- every prefix split and deduplication regardless of size.
--
-- This verifies the primary produces a valid index on that path; byte-for-
-- byte redo of the forced image is covered by the recovery/streaming tests
-- run with wal_consistency_checking.
--
set client_min_messages TO 'warning';
create extension if not exists injection_points;
create extension if not exists amcheck;
reset client_min_messages;

-- Local to this process, so a concurrent backend is unaffected.
SELECT injection_points_set_local();
SELECT injection_points_attach('nbtree-prefix-force-rfpi', 'notice');

-- Keys with a long shared leading prefix so the leaves compress and split.
create table nbtree_prefix_rfpi(u text collate "C") with (autovacuum_enabled = off);
create index nbtree_prefix_rfpi_u_idx on nbtree_prefix_rfpi (u)
  with (prefix_compression = on);

-- Distinct keys: many splits, each right half carried as a forced image.
insert into nbtree_prefix_rfpi
  select 'https://example.com/path/to/resource/' || lpad(g::text, 8, '0')
  from generate_series(1, 20000) g;

-- Duplicate-heavy keys: runtime deduplication, re-coded page as a forced image.
insert into nbtree_prefix_rfpi
  select 'https://example.com/path/to/resource/' || lpad((g % 200)::text, 8, '0')
  from generate_series(1, 20000) g;

-- The index must be structurally valid and agree with the heap.
SELECT bt_index_parent_check('nbtree_prefix_rfpi_u_idx'::regclass, true, true);

-- An index-only scan of the key column must equal the independent oracle.
set enable_seqscan = off;
SELECT (SELECT md5(string_agg(u, '|' ORDER BY u)) FROM nbtree_prefix_rfpi) =
       md5(string_agg(x, '|' ORDER BY x)) AS forced_rfpi_ok
  FROM (SELECT 'https://example.com/path/to/resource/' || lpad(g::text, 8, '0') AS x
          FROM generate_series(1, 20000) g
        UNION ALL
        SELECT 'https://example.com/path/to/resource/' || lpad((g % 200)::text, 8, '0')
          FROM generate_series(1, 20000) g) oracle;
reset enable_seqscan;

SELECT injection_points_detach('nbtree-prefix-force-rfpi');
drop table nbtree_prefix_rfpi;
drop extension amcheck;
drop extension injection_points;
