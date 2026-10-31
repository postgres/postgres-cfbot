-- Synthetic exerciser for nbtree delete-marking.
CREATE EXTENSION test_deletemark;

-- Basic table + non-unique btree index over an ordinary heap.
-- (No table AM enables delete-marking yet; we drive btdeletemark directly.)
CREATE TABLE dm_t (k int, v int);
INSERT INTO dm_t SELECT g, g FROM generate_series(1, 20) g;
CREATE INDEX dm_idx ON dm_t (k);

-- Pick a concrete heap TID to operate on (k = 10).
SELECT ctid AS tid10 FROM dm_t WHERE k = 10 \gset

-- (a) Before marking: entry classifies as a plain leaf tuple, heap TID matches.
SELECT dm_classify('dm_idx', :'tid10') AS before_class;
SELECT dm_heaptid('dm_idx', :'tid10') = :'tid10' AS before_heaptid_ok;

-- Mark it delete-marked in place (re-descend by key+TID, WAL-logged).
SELECT dm_mark('dm_idx', :'tid10') AS marked;

-- (a) After marking: classifies as delete-marked (NOT pivot, NOT posting),
--     and the heap TID is still retrievable from the trailer == original TID.
SELECT dm_classify('dm_idx', :'tid10') AS after_class;
SELECT dm_heaptid('dm_idx', :'tid10') = :'tid10' AS after_heaptid_ok;

-- Marking an already-marked entry is idempotent (returns false: nothing to do).
SELECT dm_mark('dm_idx', :'tid10') AS remark;

-- (b) A scan returns the delete-marked entry WITH recheck set.
SELECT dm_scan_recheck('dm_idx') AS scan_sets_recheck;

-- (d) amcheck: structural checks must still pass on an index with a
--     delete-marked entry (TID validity, key ordering, alt-TID subtype).
CREATE EXTENSION amcheck;
SELECT bt_index_check('dm_idx');
SELECT bt_index_parent_check('dm_idx');

-- (c) Uniqueness: delete-marked entry must be skipped as a duplicate.
CREATE TABLE dm_u (k int);
INSERT INTO dm_u SELECT g FROM generate_series(1, 20) g;
CREATE UNIQUE INDEX dm_uidx ON dm_u (k);

SELECT ctid AS utid7 FROM dm_u WHERE k = 7 \gset

-- Delete-mark the (k=7) entry, then a NEW insert of k=7 must SUCCEED
-- (the tombstone is not a live duplicate).
SELECT dm_mark('dm_uidx', :'utid7') AS u_marked;
INSERT INTO dm_u VALUES (7);   -- accepted: delete-marked dup skipped

-- A true duplicate of a still-live key must still be REJECTED.
INSERT INTO dm_u VALUES (8);   -- should fail: k=8 is live

-- amcheck the unique index too (structural).
SELECT bt_index_check('dm_uidx');

DROP EXTENSION test_deletemark CASCADE;
DROP EXTENSION amcheck CASCADE;
DROP TABLE dm_t;
DROP TABLE dm_u;
