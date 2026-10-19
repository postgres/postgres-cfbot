# Verify that ALTER INDEX ... ATTACH PARTITION locks ancestor tables and
# indexes from the root down before locking the target index.  Validating
# the target can recurse up the index hierarchy, taking AccessExclusiveLock
# on each ancestor index and its table.

setup
{
  CREATE TABLE index_attach_root (id int) PARTITION BY RANGE (id);
  CREATE TABLE index_attach_mid1 PARTITION OF index_attach_root FOR VALUES FROM (0) TO (1000) PARTITION BY RANGE (id);
  CREATE TABLE index_attach_mid2 PARTITION OF index_attach_mid1 FOR VALUES FROM (0) TO (500) PARTITION BY RANGE (id);
  CREATE TABLE index_attach_leaf PARTITION OF index_attach_mid2 FOR VALUES FROM (0) TO (100);
  CREATE INDEX index_attach_root_idx ON ONLY index_attach_root (id);
  CREATE INDEX index_attach_mid1_idx ON ONLY index_attach_mid1 (id);
  CREATE INDEX index_attach_mid2_idx ON ONLY index_attach_mid2 (id);
  CREATE INDEX index_attach_leaf_idx ON index_attach_leaf (id);
  ALTER INDEX index_attach_root_idx ATTACH PARTITION index_attach_mid1_idx;
  ALTER INDEX index_attach_mid1_idx ATTACH PARTITION index_attach_mid2_idx;
}

teardown
{
  DROP TABLE index_attach_root;
}

session s1
step s1begin      { BEGIN; }
step s1lock       { LOCK TABLE ONLY index_attach_root IN ACCESS EXCLUSIVE MODE; }
step s1alter      { ALTER TABLE index_attach_root ALTER COLUMN id SET DEFAULT 0; }
step s1attachroot { ALTER INDEX index_attach_root_idx ATTACH PARTITION index_attach_mid1_idx; }
step s1attachmid  { ALTER INDEX index_attach_mid1_idx ATTACH PARTITION index_attach_mid2_idx; }
step s1commit     { COMMIT; }

session s2
step s2attach     { ALTER INDEX index_attach_mid2_idx ATTACH PARTITION index_attach_leaf_idx; }
step s2check
{
  SELECT bool_and(indisvalid) AS all_valid FROM pg_index
    WHERE indexrelid IN ('index_attach_root_idx'::regclass,
                        'index_attach_mid1_idx'::regclass,
                        'index_attach_mid2_idx'::regclass,
                        'index_attach_leaf_idx'::regclass);
}

# Simulate a recursive ALTER TABLE paused after locking only the root.
# ATTACH must wait without locking any descendant table.  Two ancestor
# levels ensure that pre-locking only the immediate parent is insufficient.
permutation s1begin s1lock s2attach s1alter s1commit s2check

# ATTACH must pre-lock ancestors before even locking its target index, and
# must use AccessExclusiveLock on ancestor tables from the outset.  Otherwise
# s2attach could hold the target index (or AccessShareLock on the root table)
# while waiting for s1, preventing s1attachmid from completing.
permutation s1begin s1attachroot s2attach s1attachmid s1commit s2check
