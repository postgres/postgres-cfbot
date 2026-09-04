# Test SSI conflict detection when a unique check observes a committed
# deletion through SnapshotDirty.

setup
{
  CREATE TABLE test (k integer PRIMARY KEY, j integer);
  INSERT INTO test VALUES (1, 1000000);
  INSERT INTO test SELECT g, g FROM generate_series(100, 2000) g;
  CREATE INDEX test_j_idx ON test (j);
  CREATE TABLE other (k integer PRIMARY KEY, v integer);
  INSERT INTO other VALUES (1, 1);
  ANALYZE test;
}

teardown
{
  DROP TABLE other;
  DROP TABLE test;
}

session s1
step b1 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r1 { SELECT * FROM test WHERE k = 1; }
step rother1 { SELECT * FROM other WHERE k = 1; }
step sp1 { SAVEPOINT s; }
step w1 { INSERT INTO test VALUES (1, 2); }
step w1conflict { INSERT INTO test VALUES (1, 2) ON CONFLICT DO NOTHING; }
step rb1 { ROLLBACK TO SAVEPOINT s; }
step r1again { SELECT * FROM test WHERE k = 1 ORDER BY j; }
step c1 { COMMIT; }

session s2
setup { SET enable_seqscan = off; }
step b2 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step d2 { DELETE FROM test WHERE j = 1000000; }
step c2 { COMMIT; }

session s3
step b3 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step u3 { UPDATE other SET v = 2 WHERE k = 1; }
step c3 { COMMIT; }

# s1's initial read must precede s2, while its INSERT relies on s2's deletion.
# There is no serial order in which both observations are possible.
permutation b1 r1 b2 d2 c2 w1 r1again c1

# ON CONFLICT uses a partial unique check, but must detect the same anomaly.
permutation b1 r1 b2 d2 c2 w1conflict r1again c1

# The serialization failure must remain effective after rolling back the
# statement's subtransaction.
permutation b1 r1 b2 d2 c2 sp1 w1 rb1 c1

# A deletion committed before s1 takes its snapshot is visible normally and
# permits the key to be reused.
permutation b2 d2 c2 b1 w1 r1again c1

# An rw-conflict out to an unrelated transaction does not make relying on the
# deletion unsafe: s2 can be ordered before s1, and s1 before s3.
permutation b1 rother1 b3 u3 c3 b2 d2 c2 w1 c1
