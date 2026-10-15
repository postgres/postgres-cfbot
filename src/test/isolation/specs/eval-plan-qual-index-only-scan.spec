# Run a correlated index-only scan in EPQ using the updated outer keys.
# The three rows respectively need projection, fail the scan qual, and use
# a NULL runtime key.  All are rechecked in the same EPQ state tree.
setup
{
  CREATE TABLE epq_ios_target (id int PRIMARY KEY, k int, v int);
  CREATE TABLE epq_ios_lookup (k int PRIMARY KEY);
  INSERT INTO epq_ios_target VALUES (1, 1, 0), (2, 1, 0), (3, 1, 0);
  -- Enough rows to choose an index-only scan without planner overrides.
  INSERT INTO epq_ios_lookup SELECT generate_series(0, 999);
}
setup { VACUUM ANALYZE epq_ios_lookup; }

teardown { DROP TABLE epq_ios_target, epq_ios_lookup; }

session s1
setup { BEGIN; }
step change_keys
{
  UPDATE epq_ios_target
    SET k = CASE id WHEN 1 THEN 3 WHEN 2 THEN 2 ELSE NULL END;
}
step commit_keys { COMMIT; }

session s2
step update_values
{
  UPDATE epq_ios_target t
    SET v = (SELECT k + 10 FROM epq_ios_lookup
             WHERE k = t.k AND k % 3 <> 2)
    WHERE id >= 1;
}
step check_values { SELECT * FROM epq_ios_target ORDER BY id; }

permutation change_keys update_values(commit_keys) commit_keys check_values
