# A row that grows on a full page cannot stay at its TID in FLUX, so the
# UPDATE moves it to a new TID even though no indexed column changed.  A
# READ COMMITTED writer that waited for it must follow the row to its new
# version, as with heap: no update is lost.
# heap_outofplace_grow.spec runs the same steps on heap; the two expected
# outputs are identical.
#
# FLUX used to report the moved row as deleted, so the waiting UPDATE
# silently did nothing.
setup
{
  CREATE TABLE xg (id int, n int, p1 bigint, p2 bigint, p3 bigint, p4 bigint,
                   p5 bigint, p6 bigint, p7 bigint, p8 bigint)
    USING flux WITH (fillfactor = 100);
  INSERT INTO xg (id, n) SELECT g, 0 FROM generate_series(1, 400) g;
}
teardown
{
  DROP TABLE xg;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1g	{ UPDATE xg SET p1 = 1, p2 = 2, p3 = 3, p4 = 4, p5 = 5, p6 = 6,
			  p7 = 7, p8 = 8 WHERE id = 1; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }
step s1sel	{ SELECT id, n, p8, (SELECT count(*) FROM xg) AS nrows
			  FROM xg WHERE id = 1; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2u	{ UPDATE xg SET n = n + 1 WHERE id = 1; }
step s2c	{ COMMIT; }

permutation s1b s2b s1g s2u s1c s2c s1sel
permutation s1b s2b s1g s2u s1r s2c s1sel
