--
-- CLUSTER orders a FLUX table by the index, whichever plan CLUSTER picks:
-- an index scan, or a sequential scan plus sort (an expression index, or a
-- big table with little maintenance_work_mem, makes it sort).  Heap is the
-- control; both halves must give the same answer.
--
CREATE FUNCTION flux_cluster_misordered(t regclass, key text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM (SELECT %s AS k, lag(%s) OVER (ORDER BY ctid) AS lk FROM %s) s WHERE lk > k',
				   key, key, t) INTO n;
	RETURN n;
END $$;

-- Expression index: CLUSTER always sorts.
CREATE TABLE heap_clx (id serial PRIMARY KEY, a int, b text COLLATE "C") USING heap;
CREATE TABLE flux_clx (id serial PRIMARY KEY, a int, b text COLLATE "C") USING flux;
INSERT INTO heap_clx (a, b) SELECT g % 42, 'prefix' || g FROM generate_series(1, 133) g;
INSERT INTO flux_clx (a, b) SELECT g % 42, 'prefix' || g FROM generate_series(1, 133) g;
CREATE INDEX heap_clx_minus_a ON heap_clx ((-a), b);
CREATE INDEX flux_clx_minus_a ON flux_clx ((-a), b);
CLUSTER heap_clx USING heap_clx_minus_a;
CLUSTER flux_clx USING flux_clx_minus_a;
SELECT flux_cluster_misordered('heap_clx', '-a') AS heap_misordered,
	   flux_cluster_misordered('flux_clx', '-a') AS flux_misordered;
SELECT (SELECT count(*) FROM heap_clx) AS heap_rows, (SELECT count(*) FROM flux_clx) AS flux_rows;

-- Plain index, sort forced by planner settings.
CREATE TABLE heap_cls (k int, pad text) USING heap;
CREATE TABLE flux_cls (k int, pad text) USING flux;
INSERT INTO heap_cls SELECT (g * 7919) % 5000, 'p' FROM generate_series(1, 5000) g;
INSERT INTO flux_cls SELECT (g * 7919) % 5000, 'p' FROM generate_series(1, 5000) g;
CREATE INDEX heap_cls_k ON heap_cls (k);
CREATE INDEX flux_cls_k ON flux_cls (k);
ANALYZE heap_cls, flux_cls;
SET enable_indexscan = off;
CLUSTER heap_cls USING heap_cls_k;
CLUSTER flux_cls USING flux_cls_k;
RESET enable_indexscan;
SELECT flux_cluster_misordered('heap_cls', 'k') AS heap_misordered,
	   flux_cluster_misordered('flux_cls', 'k') AS flux_misordered;

-- Rows deleted but still visible to an open snapshot survive the sort.
CREATE TABLE flux_clr (k int) USING flux;
INSERT INTO flux_clr SELECT g FROM generate_series(1, 20) g;
CREATE INDEX flux_clr_e ON flux_clr ((-k));
DELETE FROM flux_clr WHERE k > 10;
CLUSTER flux_clr USING flux_clr_e;
SELECT count(*), min(k), max(k) FROM flux_clr;
SELECT flux_cluster_misordered('flux_clr', '-k') AS flux_misordered;

DROP TABLE heap_clx, flux_clx, heap_cls, flux_cls, flux_clr;
DROP FUNCTION flux_cluster_misordered(regclass, text);
