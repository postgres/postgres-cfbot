-- An UPDATE that FLUX performs out of place (the row moves to a new TID)
-- must decode as one UPDATE, as heap's cross-page UPDATE does, not as a
-- DELETE plus an INSERT.  Each case runs on heap and on FLUX.
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

-- A changed exclusion-constraint key moves the row.
CREATE TABLE xt_heap (id int PRIMARY KEY, r int4range, v int,
	EXCLUDE USING gist (r WITH &&)) USING heap;
CREATE TABLE xt_flux (id int PRIMARY KEY, r int4range, v int,
	EXCLUDE USING gist (r WITH &&)) USING flux;
INSERT INTO xt_heap VALUES (1, '[1,10)', 0);
INSERT INTO xt_flux VALUES (1, '[1,10)', 0);
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

UPDATE xt_heap SET r = '[1,11)', v = v + 1 WHERE id = 1;
UPDATE xt_flux SET r = '[1,11)', v = v + 1 WHERE id = 1;
-- the primary key changes too: the old key is logged
UPDATE xt_heap SET id = 2, r = '[1,12)' WHERE id = 1;
UPDATE xt_flux SET id = 2, r = '[1,12)' WHERE id = 1;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

ALTER TABLE xt_heap REPLICA IDENTITY FULL;
ALTER TABLE xt_flux REPLICA IDENTITY FULL;
UPDATE xt_heap SET r = '[1,13)', v = v + 1 WHERE id = 2;
UPDATE xt_flux SET r = '[1,13)', v = v + 1 WHERE id = 2;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Rolled back: nothing is decoded.
BEGIN;
UPDATE xt_flux SET r = '[1,14)' WHERE id = 2;
ROLLBACK;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- A row that cannot grow on its full page moves.
CREATE TABLE gt_heap (id int PRIMARY KEY, t text) USING heap WITH (fillfactor = 100);
CREATE TABLE gt_flux (id int PRIMARY KEY, t text) USING flux WITH (fillfactor = 100);
ALTER TABLE gt_heap ALTER COLUMN t SET STORAGE PLAIN;
ALTER TABLE gt_flux ALTER COLUMN t SET STORAGE PLAIN;
INSERT INTO gt_heap SELECT g, repeat('a', 100) FROM generate_series(1, 200) g;
INSERT INTO gt_flux SELECT g, repeat('a', 100) FROM generate_series(1, 200) g;
SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'skip-empty-xacts', '1');

UPDATE gt_heap SET t = repeat('b', 2000) WHERE id = 1;
UPDATE gt_flux SET t = repeat('b', 2000) WHERE id = 1;
SELECT (ctid::text::point)[0] > 0 AS moved FROM gt_heap WHERE id = 1;
SELECT (ctid::text::point)[0] > 0 AS moved FROM gt_flux WHERE id = 1;
SELECT regexp_replace(data, '''b+''', '''b...''') FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- The same, with REPLICA IDENTITY FULL and a TOASTed new value.
ALTER TABLE gt_heap REPLICA IDENTITY FULL;
ALTER TABLE gt_flux REPLICA IDENTITY FULL;
ALTER TABLE gt_heap ALTER COLUMN t SET STORAGE EXTERNAL;
ALTER TABLE gt_flux ALTER COLUMN t SET STORAGE EXTERNAL;
UPDATE gt_heap SET t = repeat('c', 10000) WHERE id = 2;
UPDATE gt_flux SET t = repeat('c', 10000) WHERE id = 2;
SELECT regexp_replace(data, '''c+''', '''c...''') FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT length(t) FROM gt_flux WHERE id = 2;

-- A moved row whose new value is TOASTed, and one whose TOASTed value is
-- unchanged.
CREATE TABLE tt_heap (id int PRIMARY KEY, r int4range, t text,
	EXCLUDE USING gist (r WITH &&)) USING heap;
CREATE TABLE tt_flux (id int PRIMARY KEY, r int4range, t text,
	EXCLUDE USING gist (r WITH &&)) USING flux;
ALTER TABLE tt_heap ALTER COLUMN t SET STORAGE EXTERNAL;
ALTER TABLE tt_flux ALTER COLUMN t SET STORAGE EXTERNAL;
INSERT INTO tt_heap VALUES (1, '[1,10)', 'x');
INSERT INTO tt_flux VALUES (1, '[1,10)', 'x');
UPDATE tt_heap SET r = '[1,11)', t = repeat('d', 10000) WHERE id = 1;
UPDATE tt_flux SET r = '[1,11)', t = repeat('d', 10000) WHERE id = 1;
UPDATE tt_heap SET r = '[1,12)' WHERE id = 1;
UPDATE tt_flux SET r = '[1,12)' WHERE id = 1;
SELECT regexp_replace(data, '''d+''', '''d...''') FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT r, length(t) FROM tt_flux;

DROP TABLE xt_heap, xt_flux, gt_heap, gt_flux, tt_heap, tt_flux;
SELECT pg_drop_replication_slot('regression_slot');
