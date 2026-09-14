# heap control for flux_ssi_conflict_out.spec, which runs the same steps on
# FLUX; the two expected outputs are identical.
#
# A serializable reader must report a conflict out to every writer of a
# version it examines that its snapshot cannot see, whether that writer is
# still running or committed after the snapshot was taken.  FLUX keeps only
# the newest version on the page and reaches older ones through the UNDO
# version chain, so the writers the walk steps over must be reported too.
#
# Permutations 1-2 are update-conflict-out.spec.  Permutations 3-4 are a write
# skew whose read goes through an index and is served a version rebuilt from
# UNDO, past one committed in-place UPDATE (3) and past a committed and a
# running one (4).  Permutations 5-6 are receipt-report.spec's schedules
# where a read-only transaction sees a state no serial order produces.

setup
{
  CREATE TABLE txn0 (id int4 PRIMARY KEY, val text) USING heap;
  CREATE TABLE txn1 (id int4 PRIMARY KEY, val text) USING heap;
  CREATE TABLE ws (id int PRIMARY KEY, v int) USING heap;
  INSERT INTO ws VALUES (1, 0), (2, 0), (3, 0);
  CREATE TABLE ctl (k text NOT NULL PRIMARY KEY, deposit_date date NOT NULL) USING heap;
  INSERT INTO ctl VALUES ('receipt', DATE '2008-12-22');
  CREATE TABLE receipt (receipt_no int NOT NULL PRIMARY KEY, deposit_date date NOT NULL, amount numeric(13,2)) USING heap;
  INSERT INTO receipt VALUES (1, DATE '2008-12-22', 1.00), (2, DATE '2008-12-22', 2.00);
}

teardown
{
  DROP TABLE txn0, txn1, ws, ctl, receipt;
}

session foo
step foo_begin		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step foo_select		{ SELECT * FROM txn0 WHERE id = 42; }
step foo_insert		{ INSERT INTO txn1 SELECT 7, 'foo_insert'; }
step foo_commit		{ COMMIT; }

session bar
step bar_begin		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step bar_select		{ SELECT * FROM txn1 WHERE id = 7; }
step bar_insert		{ INSERT INTO txn0 SELECT 42, 'bar_insert'; }
step bar_commit		{ COMMIT; }

session trouble
step trouble_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step trouble_update	{ UPDATE txn1 SET val = 'add physical version for "bar_select"' WHERE id = 7; }
step trouble_delete	{ DELETE FROM txn1 WHERE id = 7; }
step trouble_abort	{ ABORT; }

session w1
step w1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step w1snap	{ SELECT v FROM ws WHERE id = 3; }
step w1r1	{ SELECT v FROM ws WHERE id = 1; }
step w1w2	{ UPDATE ws SET v = v + 1 WHERE id = 2; }
step w1c	{ COMMIT; }

session w2
step w2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step w2r2	{ SELECT v FROM ws WHERE id = 2; }
step w2w1	{ UPDATE ws SET v = v + 10 WHERE id = 1; }
step w2c	{ COMMIT; }

session w3
step w3b	{ BEGIN; }
step w3w1	{ UPDATE ws SET v = v + 100 WHERE id = 1; }
step w3a	{ ABORT; }

session s1
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rxwy1	{ INSERT INTO receipt VALUES (3, (SELECT deposit_date FROM ctl WHERE k = 'receipt'), 4.00); }
step c1		{ COMMIT; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wx2	{ UPDATE ctl SET deposit_date = DATE '2008-12-23' WHERE k = 'receipt'; }
step c2		{ COMMIT; }

session s3
step s3b	{ BEGIN ISOLATION LEVEL SERIALIZABLE, READ ONLY; }
step rx3	{ SELECT * FROM ctl WHERE k = 'receipt'; }
step ry3	{ SELECT * FROM receipt WHERE deposit_date = DATE '2008-12-22'; }
step c3		{ COMMIT; }

permutation foo_begin bar_begin trouble_begin
	foo_select bar_insert foo_insert foo_commit
	trouble_update bar_select bar_commit trouble_abort

permutation foo_begin bar_begin trouble_begin
	foo_select bar_insert foo_insert foo_commit
	trouble_delete bar_select bar_commit trouble_abort

permutation w1b w1snap w2b w2r2 w2w1 w2c w1r1 w1w2 w1c

permutation w1b w1snap w2b w2r2 w2w1 w2c w3b w3w1 w1r1 w1w2 w1c w3a

permutation s1b s2b s3b rxwy1 wx2 c2 rx3 c1 ry3 c3

permutation s1b s2b s3b wx2 rxwy1 c2 rx3 c1 ry3 c3
