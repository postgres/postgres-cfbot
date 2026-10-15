# Adversarial correctness tests for HOT-indexed (SIU) updates.
#
# Each permutation pins a case that the mid-chain-pointing invariant must
# satisfy: an index entry points at the heap-only version whose key it
# matched, and a chain walk that crosses a HOT-indexed hop drops the arriving
# entry when the crossed-attribute bitmap overlaps the index's key columns
# (no key comparison).  These are
# exactly the cases that historically broke write-amplification-reduction
# designs.

setup
{
    CREATE TABLE hia (id int PRIMARY KEY, k int, pad text) WITH (fillfactor = 40);
    CREATE INDEX hia_k ON hia(k);
    CREATE TABLE hiu (id int PRIMARY KEY, k int, pad text) WITH (fillfactor = 40);
    CREATE UNIQUE INDEX hiu_k ON hiu(k);
    INSERT INTO hia VALUES (1, 10, repeat('x', 40));
    INSERT INTO hiu VALUES (1, 10, repeat('x', 40));

    -- Table for the BitmapAnd-vs-SIU cases: two btree indexes on
    -- separate columns.  An SIU update changes c2 (planting a fresh c2 entry
    -- mid-chain) and leaves c1 unchanged (its entry still points at the root),
    -- so a BitmapAnd on c1 AND c2 must union the disagreeing offsets.
    CREATE TABLE hband (id int PRIMARY KEY, c1 int, c2 int, pad text)
        WITH (fillfactor = 50);
    CREATE INDEX hband_c1 ON hband(c1);
    CREATE INDEX hband_c2 ON hband(c2);
    INSERT INTO hband VALUES (1, 11, 21, repeat('x', 40));

    -- Table for the concurrent-collapse reader-consistency case.
    CREATE TABLE hib (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 50);
    CREATE INDEX hib_v_idx ON hib(v);
    INSERT INTO hib SELECT g, g * 10, repeat('x', 50) FROM generate_series(1, 5) g;
    UPDATE hib SET v = 100 WHERE id = 1;
    UPDATE hib SET v = 200 WHERE id = 1;
    UPDATE hib SET v = 300 WHERE id = 1;
    UPDATE hib SET v = 400 WHERE id = 1;
}

teardown
{
    DROP TABLE hia;
    DROP TABLE hiu;
    DROP TABLE hband;
    DROP TABLE hib;
}

session s1
step s1_begin   { BEGIN; }
# Cycle the indexed key away and back: 10 -> 20 -> 10.  The original 10 leaf
# and the freshly-inserted 10 leaf both resolve to the live tuple; the chain
# walk must drop the stale one so a lookup returns the row exactly once.
step s1_cycle   { UPDATE hia SET k = 20 WHERE id = 1; UPDATE hia SET k = 10 WHERE id = 1; }
# A single HOT-indexed update on hia, used by the abort and reader cases.
step s1_upd20   { UPDATE hia SET k = 20 WHERE id = 1; }
# A HOT-indexed update on the UNIQUE-indexed table, freeing key 10 for k=20.
step s1_uupd20  { UPDATE hiu SET k = 20 WHERE id = 1; }
step s1_commit  { COMMIT; }
step s1_abort   { ROLLBACK; }
# SIU update on hband: change c2 (fresh mid-chain entry), leave c1 unchanged.
step s1_bupd    { UPDATE hband SET c2 = 22 WHERE id = 1; }

session s2
# Index-only lookups (forced) that exercise the stale-leaf recheck.
step s2_noseq   { SET enable_seqscan = off; }
step s2_eq10    { SELECT id, k FROM hia WHERE k = 10; }
step s2_eq20    { SELECT id, k FROM hia WHERE k = 20; }
step s2_range   { SELECT id, k FROM hia WHERE k >= 5 ORDER BY k; }
# Concurrent unique insert of the key s1 is freeing (10) and of the key s1 is
# taking (20); _bt_check_unique must filter the stale 10 leaf but still
# conflict on the live key.
step s2_ins10   { INSERT INTO hiu VALUES (2, 10, repeat('y', 40)); }
step s2_ins20   { INSERT INTO hiu VALUES (3, 20, repeat('z', 40)); }
# Move the indexed key well away from 10 (two HOT-indexed hops) and force an
# index scan on the new key.  That scan reaches the live tuple through the
# stale 10 leaf and may try to kill it for bloat reclaim.
step s2_to30    { UPDATE hia SET k = 20 WHERE id = 1; UPDATE hia SET k = 30 WHERE id = 1; }
step s2_scan30  { SET enable_seqscan = off; SELECT id, k FROM hia WHERE k = 30; }
# Forced BitmapAnd over the unchanged (c1) and changed (c2) indexes.  Must
# return the row: after the SIU update the two indexes name different offsets
# on the one block, and the block's LOCATOR_SPLIT signal makes tbm_intersect
# union them.  new key -> 1 row; the pre-update pair (c1=11,c2=21) -> 0.
step s2_bnoseq  { SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = on; }
step s2_band_new { SELECT count(*) AS band FROM hband WHERE c1 = 11 AND c2 = 22; }
step s2_band_old { SELECT count(*) AS band FROM hband WHERE c1 = 11 AND c2 = 21; }

# Reader holding an older REPEATABLE READ snapshot that still sees k=10.
session s3
step s3_begin   { BEGIN ISOLATION LEVEL REPEATABLE READ; SET enable_seqscan = off; }
step s3_eq10    { SELECT id, k FROM hia WHERE k = 10; }
step s3_commit  { COMMIT; }

session b1
step b1_begin   { BEGIN; }
# Reader takes a snapshot and reads the chain via the secondary index.
step b1_snap    { SELECT id, v FROM hib WHERE v = 400; }
step b1_commit  { COMMIT; }

session b2
# Force prune/collapse: another HOT-indexed update plus a VACUUM that
# collapses the dead chain members to LP_REDIRECT forwarders.
step b2_update  { UPDATE hib SET v = 500 WHERE id = 1; }
step b2_vacuum  { VACUUM (INDEX_CLEANUP off) hib; }

session b3
step b3_seq     { SELECT id, v FROM hib ORDER BY id; }

# a->b->a cycle: exactly one row for the cycled-back key, none for the
#    transient key.
permutation s2_noseq s1_cycle s2_eq10 s2_eq20

# Range scan returns the live row exactly once across the stale+fresh
#    leaves left by the cycle.
permutation s2_noseq s1_cycle s2_range

# Abort of a HOT-indexed update: the new key must not surface and the old
#    key must still resolve to the (restored) live tuple.
permutation s2_noseq s1_begin s1_upd20 s1_abort s2_eq20 s2_eq10

# Concurrent unique insert while a HOT-indexed update is in flight.  An
#    insert of the key s1 is freeing (10) must wait on the uncommitted updater,
#    then succeed once it commits (the stale 10 leaf is filtered).
permutation s1_begin s1_uupd20 s2_ins10 s1_commit

# After the update commits, the freed key (10) inserts cleanly and the
#    taken key (20) conflicts; the live leaf is honoured, the stale one is not.
permutation s1_begin s1_uupd20 s1_commit s2_ins10
permutation s1_begin s1_uupd20 s1_commit s2_ins20

# Snapshot safety of stale-leaf reclaim.  An older REPEATABLE READ reader
#    still sees k=10; a concurrent session then moves the key to 30 and runs an
#    index scan that reaches the live tuple through the stale 10 leaf and may
#    try to reclaim it.  The reclaim is gated on the skipped versions being
#    dead to all transactions, which s3's held snapshot prevents, so s3 must
#    still find the row by k=10 after the scan.
permutation s3_begin s3_eq10 s2_to30 s2_scan30 s3_eq10 s3_commit

# Reader consistency across a concurrent prune/collapse.  s1's index scan,
#    crossing the collapsed chain after the VACUUM, must still return the row
#    via the crossed-attribute bitmap; the query must not error and the row count
#    must be consistent.
permutation b1_begin b1_snap b2_update b2_vacuum b1_snap b1_commit b3_seq
permutation b1_begin b2_update b1_snap b2_vacuum b1_snap b1_commit b3_seq

# BitmapAnd after a committed SIU update: the changed (c2) and unchanged
#    (c1) index entries disagree on offset within the one block, and the
#    per-block LOCATOR_SPLIT signal must make BitmapAnd union them so the row
#    is found by the new key (and not by the stale pair).
permutation s2_bnoseq s1_bupd s2_band_new s2_band_old

# BitmapAnd concurrent with an in-flight SIU update.  The reader takes its
#    bitmap before the updater commits (sees the pre-update pair), then again
#    after (sees the new key).  Exercises the lock-free VM split-bit read
#    against a concurrent setter; neither result may drop or double-count.
permutation s2_bnoseq s1_begin s1_bupd s2_band_new s2_band_old s1_commit s2_band_new s2_band_old
