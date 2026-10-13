# heap control for flux_outofplace_update.spec, which runs the same steps
# on FLUX; the two expected outputs are identical.
setup
{
  CREATE TABLE xt (id int, r int4range, v int,
                   EXCLUDE USING gist (r WITH &&)) USING heap;
  INSERT INTO xt VALUES (1, '[1,10)', 0);
  CREATE TABLE xp (id int4range NOT NULL, valid_at daterange NOT NULL,
                   PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)) USING heap;
  CREATE TABLE xc (id int4range NOT NULL, valid_at daterange NOT NULL,
                   parent_id int4range,
                   FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES xp)
                   USING heap;
  INSERT INTO xp VALUES ('[1,2)', '[2020-01-01,2030-01-01)');
}
teardown
{
  DROP TABLE xc, xp, xt;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1u	{ UPDATE xt SET v = v + 1 WHERE id = 1; }
step s1uk	{ UPDATE xt SET v = v + 1, r = int4range(lower(r), upper(r) + 1) WHERE id = 1; }
step s1up	{ UPDATE xp SET valid_at = '[2020-01-01,2026-01-01)' WHERE id = '[1,2)'; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }
step s1sel	{ SELECT id, v, r, count(*) OVER () AS nrows FROM xt; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2sel	{ SELECT id, v, r FROM xt; }
step s2u	{ UPDATE xt SET v = v + 1 WHERE id = 1; }
step s2uk	{ UPDATE xt SET v = v + 1, r = int4range(lower(r), upper(r) + 1) WHERE id = 1; }
step s2d	{ DELETE FROM xt WHERE id = 1; }
step s2fu	{ SELECT id, v, r FROM xt WHERE id = 1 FOR UPDATE; }
step s2ins	{ INSERT INTO xc VALUES ('[1,2)', '[2020-01-01,2025-01-01)', '[1,2)'); }
step s2c	{ COMMIT; }

# (a) READ COMMITTED: the waiter acts on the new version; no update is lost.
permutation s1b s2b s1u s2u s1c s2c s1sel
permutation s1b s2b s1uk s2uk s1c s2c s1sel
permutation s1b s2b s1uk s2u s1c s2c s1sel
permutation s1b s2b s1u s2uk s1c s2c s1sel
permutation s1b s2b s1uk s2d s1c s2c s1sel
# The move rolls back: the waiter updates the old version.
permutation s1b s2b s1uk s2u s1r s2c s1sel
permutation s1b s2b s1uk s2uk s1r s2c s1sel
# (b) REPEATABLE READ: a serialization failure for a concurrent update.
permutation s1b s2brr s1uk s2u s1c s2c s1sel
permutation s1b s2brr s2sel s1uk s1c s2d s2c s1sel
# (c) The FK check follows the parent row to its new version.
permutation s1b s2b s1up s2ins s1c s2c
permutation s1b s2brr s1up s2ins s1c s2c
# (d) FOR UPDATE locks the new version, or fails under REPEATABLE READ.
permutation s1b s2b s1uk s2fu s1c s2c s1sel
permutation s1b s2brr s2sel s1uk s1c s2fu s2c
