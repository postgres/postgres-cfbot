-- Tests for ALTER TABLE
CREATE EXTENSION injection_points;

SELECT injection_points_set_local();

SELECT injection_points_attach('alter-table-phase-3-rewrite', 'notice');
SELECT injection_points_attach('alter-table-phase-3-verify', 'notice');

CREATE SCHEMA testgen_inj;

-- Check that the table isn't being rewritten nor scanned during phase 3,
-- even if other objects depend on the column we are changing.
CREATE TABLE testgen_inj.t1 (a INT, b INT NOT NULL);
INSERT INTO testgen_inj.t1 (a, b) VALUES (1, 2);
ALTER TABLE testgen_inj.t1 ADD CONSTRAINT c1 CHECK (b > 0);
ALTER TABLE testgen_inj.t1 ADD CONSTRAINT c2 CHECK (b IS NOT DISTINCT FROM a * 2);
CREATE INDEX ON testgen_inj.t1 (b);
ALTER TABLE testgen_inj.t1 ALTER b
    ADD GENERATED USING CONSTRAINT c2 STORED;
-- we expect to *not* see a "alter-table-phase-3-*" notice here
DROP TABLE testgen_inj.t1;
DROP SCHEMA testgen_inj;

DROP EXTENSION injection_points;
