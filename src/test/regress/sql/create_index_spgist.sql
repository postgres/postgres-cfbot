--
-- SP-GiST index tests
--

CREATE TABLE quad_point_tbl AS
    SELECT point(unique1,unique2) AS p FROM tenk1;

INSERT INTO quad_point_tbl
    SELECT '(333.0,400.0)'::point FROM generate_series(1,1000);

INSERT INTO quad_point_tbl VALUES (NULL), (NULL), (NULL);

CREATE INDEX sp_quad_ind ON quad_point_tbl USING spgist (p);

CREATE TABLE kd_point_tbl AS SELECT * FROM quad_point_tbl;

CREATE INDEX sp_kd_ind ON kd_point_tbl USING spgist (p kd_point_ops);

CREATE TABLE radix_text_tbl AS
    SELECT name AS t FROM road WHERE name !~ '^[0-9]';

INSERT INTO radix_text_tbl
    SELECT 'P0123456789abcdef' FROM generate_series(1,1000);
INSERT INTO radix_text_tbl VALUES ('P0123456789abcde');
INSERT INTO radix_text_tbl VALUES ('P0123456789abcdefF');

CREATE INDEX sp_radix_ind ON radix_text_tbl USING spgist (t);

-- get non-indexed results for comparison purposes

SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SET enable_bitmapscan = OFF;

SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

SELECT count(*) FROM quad_point_tbl;

SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

CREATE TEMP TABLE quad_point_tbl_ord_seq1 AS
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM quad_point_tbl;

CREATE TEMP TABLE quad_point_tbl_ord_seq2 AS
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

CREATE TEMP TABLE quad_point_tbl_ord_seq3 AS
SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
FROM quad_point_tbl WHERE p IS NOT NULL;

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ^@  'Worth';

-- Now check the results from plain indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl;
SELECT count(*) FROM quad_point_tbl;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM quad_point_tbl;
CREATE TEMP TABLE quad_point_tbl_ord_idx1 AS
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM quad_point_tbl;
SELECT * FROM quad_point_tbl_ord_seq1 seq FULL JOIN quad_point_tbl_ord_idx1 idx
ON seq.n = idx.n
WHERE seq.dist IS DISTINCT FROM idx.dist;

EXPLAIN (COSTS OFF)
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
CREATE TEMP TABLE quad_point_tbl_ord_idx2 AS
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT * FROM quad_point_tbl_ord_seq2 seq FULL JOIN quad_point_tbl_ord_idx2 idx
ON seq.n = idx.n
WHERE seq.dist IS DISTINCT FROM idx.dist;

EXPLAIN (COSTS OFF)
SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
FROM quad_point_tbl WHERE p IS NOT NULL;
CREATE TEMP TABLE quad_point_tbl_ord_idx3 AS
SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
FROM quad_point_tbl WHERE p IS NOT NULL;
SELECT * FROM quad_point_tbl_ord_seq3 seq FULL JOIN quad_point_tbl_ord_idx3 idx
ON seq.n = idx.n
WHERE seq.dist IS DISTINCT FROM idx.dist;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM kd_point_tbl;
CREATE TEMP TABLE kd_point_tbl_ord_idx1 AS
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM kd_point_tbl;
SELECT * FROM quad_point_tbl_ord_seq1 seq FULL JOIN kd_point_tbl_ord_idx1 idx
ON seq.n = idx.n
WHERE seq.dist IS DISTINCT FROM idx.dist;

EXPLAIN (COSTS OFF)
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
CREATE TEMP TABLE kd_point_tbl_ord_idx2 AS
SELECT row_number() OVER (ORDER BY p <-> '0,0') n, p <-> '0,0' dist, p
FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT * FROM quad_point_tbl_ord_seq2 seq FULL JOIN kd_point_tbl_ord_idx2 idx
ON seq.n = idx.n
WHERE seq.dist IS DISTINCT FROM idx.dist;

EXPLAIN (COSTS OFF)
SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
FROM kd_point_tbl WHERE p IS NOT NULL;
CREATE TEMP TABLE kd_point_tbl_ord_idx3 AS
SELECT row_number() OVER (ORDER BY p <-> '333,400') n, p <-> '333,400' dist, p
FROM kd_point_tbl WHERE p IS NOT NULL;
SELECT * FROM quad_point_tbl_ord_seq3 seq FULL JOIN kd_point_tbl_ord_idx3 idx
ON seq.n = idx.n
WHERE seq.dist IS DISTINCT FROM idx.dist;

-- test KNN scan with included columns
-- the distance numbers are not exactly the same across platforms
SET extra_float_digits = 0;
CREATE INDEX ON quad_point_tbl_ord_seq1 USING spgist(p) INCLUDE(dist);
EXPLAIN (COSTS OFF)
SELECT p, dist FROM quad_point_tbl_ord_seq1 ORDER BY p <-> '0,0' LIMIT 10;
SELECT p, dist FROM quad_point_tbl_ord_seq1 ORDER BY p <-> '0,0' LIMIT 10;
RESET extra_float_digits;

-- test an index-only scan that returns only an included column
CREATE TEMP TABLE spgist_poly_include (id int, p polygon);
-- make a stored BOX look like a large varlena value if treated as a POLYGON
INSERT INTO spgist_poly_include VALUES
  (7, polygon(box(point(0,0), point(1.0000000009313226,1))));
CREATE INDEX spgist_poly_include_idx ON spgist_poly_include
  USING spgist (p) INCLUDE (id);
VACUUM (FREEZE, ANALYZE) spgist_poly_include;
EXPLAIN (COSTS OFF)
SELECT id FROM spgist_poly_include;
SELECT id FROM spgist_poly_include;

-- the same with integral coordinates, where the stored box would be read
-- as a zero-length polygon if it were returned as the key
CREATE TEMP TABLE spgist_poly_include2 (id int, p polygon);
INSERT INTO spgist_poly_include2
  SELECT i, polygon(box(point(i, i), point(i + 1, i + 1)))
  FROM generate_series(1, 10) i;
CREATE INDEX spgist_poly_include2_idx ON spgist_poly_include2
  USING spgist (p) INCLUDE (id);
VACUUM (FREEZE, ANALYZE) spgist_poly_include2;
EXPLAIN (COSTS OFF)
SELECT id FROM spgist_poly_include2 ORDER BY id;
SELECT id FROM spgist_poly_include2 ORDER BY id;

-- check ORDER BY distance to NULL
SELECT (SELECT p FROM kd_point_tbl ORDER BY p <-> pt, p <-> '0,0' LIMIT 1)
FROM (VALUES (point '1,2'), (NULL), ('1234,5678')) pts(pt);


EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');

-- Now check the results from bitmap indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl;
SELECT count(*) FROM quad_point_tbl;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p <<| '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p |>> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p <<| '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p |>> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';
SELECT count(*) FROM radix_text_tbl WHERE t ^@	 'Worth';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');
SELECT count(*) FROM radix_text_tbl WHERE starts_with(t, 'Worth');

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
