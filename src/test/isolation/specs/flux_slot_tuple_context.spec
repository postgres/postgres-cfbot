# FLUX: a scan that reconstructs an older version of a row from UNDO, run
# in a per-tuple memory context that is reset after every row.
#
# ALTER TABLE ... VALIDATE CONSTRAINT (validateCheckConstraint), like the
# table rewrite of ATRewriteTable, scans in the executor's per-tuple context
# and resets it for every row.  A row that another session updated in place
# is served as its committed version, rebuilt from UNDO.  The slot must own
# that image in its own context: it used to adopt the reconstruction's
# allocation, made in the per-tuple context, so the context reset freed it
# under the slot and the next clear of the slot freed it again ("pfree
# called with invalid pointer").

setup
{
  CREATE TABLE fv (id int PRIMARY KEY, v int, pad text) USING flux;
  INSERT INTO fv SELECT g, g, 'p' || g FROM generate_series(1, 30) g;
  ALTER TABLE fv ADD CONSTRAINT fv_v CHECK (v > 0) NOT VALID;
}

teardown
{
  DROP TABLE fv;
}

session u
step upd	{ BEGIN; UPDATE fv SET v = v + 100, pad = pad || 'x' WHERE id IN (3, 17); }
step abort	{ ROLLBACK; }

session a
step validate	{ ALTER TABLE fv VALIDATE CONSTRAINT fv_v; }
step check	{ SELECT convalidated FROM pg_constraint WHERE conname = 'fv_v';
		  SELECT count(*), sum(v) FROM fv; }

permutation upd validate check abort
