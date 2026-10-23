# An ordering index scan that returns its ORDER BY values (amcanreturnorderby)
# must not hand back the value it computed for the old row version when
# EvalPlanQual re-checks a concurrently updated row: the value is recomputed
# from the new version.

setup
{
    CREATE TABLE epq_knn (id int, p point);
    INSERT INTO epq_knn SELECT i, point(i, i) FROM generate_series(1, 20) i;
    CREATE INDEX epq_knn_p ON epq_knn USING gist (p);
}

teardown
{
    DROP TABLE epq_knn;
}

session s1
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1b	{ BEGIN; }
step s1u	{ UPDATE epq_knn SET p = point(3.5, 3.5) WHERE id = 3; }
step s1c	{ COMMIT; }

session s2
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s2e
{
    EXPLAIN (VERBOSE, COSTS OFF)
    SELECT id, p <-> point(0, 0) AS d FROM epq_knn
      WHERE p <@ box(point(0, 0), point(10, 10))
      ORDER BY p <-> point(0, 0) LIMIT 4 FOR UPDATE;
}
step s2s
{
    SELECT id, p <-> point(0, 0) AS d, (p <-> point(0, 0)) = d AS same
      FROM (SELECT id, p, p <-> point(0, 0) AS d FROM epq_knn
              WHERE p <@ box(point(0, 0), point(10, 10))
              ORDER BY p <-> point(0, 0) LIMIT 4 FOR UPDATE) s;
}

# s2's scan computes id 3's distance from (3,3), then waits on s1's lock;
# after s1 commits, EvalPlanQual re-checks id 3 with p = (3.5,3.5), and the
# value returned must be that row's distance.
permutation s2e s1b s1u s2s s1c
