--
-- Tests for OR-expansion to UNION ALL (Append)
--

CREATE TABLE orexp_a (id int, val int);
CREATE TABLE orexp_b (x int, y int, val int);

INSERT INTO orexp_a VALUES
  (1, 10),
  (2, 20),
  (3, 30),
  (4, NULL),
  (5, 50);

INSERT INTO orexp_b VALUES
  (1, 99, 100),
  (99, 2, 200),
  (3, 3, 300),
  (4, 99, 400),
  (99, 99, 500),
  (NULL, NULL, 600);

-- Verify GUC exists and can be toggled
SHOW or_expansion_limit;

-- Case C3: Join condition OR (disjunction of join keys)
-- With default or_expansion_limit = 8, planner should expand to Append of joins
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

-- Result must match or_expansion_limit = 0 (disabled)
SET or_expansion_limit = 0;
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

-- Case C4: Cross-table filter with separate join key
-- Tests null-safety, 3-valued logic, and duplicate exclusion
SET or_expansion_limit = 8;
EXPLAIN (COSTS OFF)
SELECT a.id, a.val, b.val FROM orexp_a a JOIN orexp_b b ON a.id = b.x
WHERE a.val = 10 OR b.val = 300
ORDER BY a.id, a.val, b.val;

SELECT a.id, a.val, b.val FROM orexp_a a JOIN orexp_b b ON a.id = b.x
WHERE a.val = 10 OR b.val = 300
ORDER BY a.id, a.val, b.val;

SET or_expansion_limit = 0;
SELECT a.id, a.val, b.val FROM orexp_a a JOIN orexp_b b ON a.id = b.x
WHERE a.val = 10 OR b.val = 300
ORDER BY a.id, a.val, b.val;

-- Case C5: Non-flattened subquery over ordinary table with cross-table OR
-- Verifies expression_tree_walker is not called on RangeTblEntry nodes
CREATE TABLE orexp_c (id int, tier text);
CREATE TABLE orexp_d (cid int, val int);
INSERT INTO orexp_c VALUES (1, 'vip'), (2, 'std');
INSERT INTO orexp_d VALUES (1, 10), (1, 20), (2, 5);

SET or_expansion_limit = 8;
EXPLAIN (COSTS OFF)
SELECT c.id, s.n FROM orexp_c c
  JOIN (SELECT cid, count(*) n FROM orexp_d GROUP BY cid) s ON s.cid = c.id
WHERE c.tier = 'vip' OR s.n > 20
ORDER BY c.id;

SELECT c.id, s.n FROM orexp_c c
  JOIN (SELECT cid, count(*) n FROM orexp_d GROUP BY cid) s ON s.cid = c.id
WHERE c.tier = 'vip' OR s.n > 20
ORDER BY c.id;

SET or_expansion_limit = 0;
SELECT c.id, s.n FROM orexp_c c
  JOIN (SELECT cid, count(*) n FROM orexp_d GROUP BY cid) s ON s.cid = c.id
WHERE c.tier = 'vip' OR s.n > 20
ORDER BY c.id;

DROP TABLE orexp_c;
DROP TABLE orexp_d;

-- Case C6: Subquery over partitioned table (gate must reject OR expansion)
CREATE TABLE orexp_part (id int, d int) PARTITION BY RANGE (d);
CREATE TABLE orexp_part1 PARTITION OF orexp_part FOR VALUES FROM (0) TO (10);
CREATE TABLE orexp_part2 PARTITION OF orexp_part FOR VALUES FROM (10) TO (20);
INSERT INTO orexp_part VALUES (1, 5), (2, 15);

SET or_expansion_limit = 8;
EXPLAIN (COSTS OFF)
SELECT a.id, s.d FROM orexp_a a,
  (SELECT id, d FROM orexp_part GROUP BY id, d) s
WHERE a.id = s.id OR s.d = 15
ORDER BY a.id, s.d;

SELECT a.id, s.d FROM orexp_a a,
  (SELECT id, d FROM orexp_part GROUP BY id, d) s
WHERE a.id = s.id OR s.d = 15
ORDER BY a.id, s.d;

SET or_expansion_limit = 0;
SELECT a.id, s.d FROM orexp_a a,
  (SELECT id, d FROM orexp_part GROUP BY id, d) s
WHERE a.id = s.id OR s.d = 15
ORDER BY a.id, s.d;

DROP TABLE orexp_part;

-- Clean up
RESET or_expansion_limit;
DROP TABLE orexp_a;
DROP TABLE orexp_b;
