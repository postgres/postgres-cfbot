# UPDATE/DELETE FOR PORTION OF test
#
# Test inserting temporal leftovers from a FOR PORTION OF update/delete.
#
# The interesting case is READ COMMITTED.  When a statement finds a row that a
# concurrent transaction has already changed, it waits, then re-checks the
# latest version with EvalPlanQual.  That is enough for an ordinary update,
# but a FOR PORTION OF update or delete narrows the row's application time and
# re-inserts the rest of its history as new rows.  Those leftovers are not
# chained off the row we found and are not visible to our snapshot, so the
# portion of history we were asked to change may no longer be reachable.
# Rather than silently skip it, we raise a serialization failure; see
# ExecForPortionOfCheckSplit().  The client can retry.
#
# The check is precise: it fires only when part of the range the row used to
# have, and no longer has, overlaps the portion we target.  A concurrent
# update that leaves the application time alone, or one whose target contains
# ours, does not raise anything.  Both cases are tested below.
#
# A concurrently deleted row is reported the same way, because a partial
# FOR PORTION OF delete removes the row and re-inserts the history it did not
# target, and we cannot tell that apart from a plain delete.
#
# Locking the rows first with SELECT FOR UPDATE is not a dependable
# alternative to retrying, even though many of the permutations here do it,
# and the last few permutations in this file show two ways it fails.  Locking
# the targeted portion can come away with a lock on a row that a concurrent
# transaction has already narrowed out of that portion.  Locking the whole key
# instead does not help either: after a DELETE FOR PORTION OF there is no
# surviving row for the waiting lock to follow, so it takes no lock at all.
#
# We run the same tests under REPEATABLE READ, where a concurrent
# update/delete failure is reported before we get that far.  In general they
# do what you'd want with no explicit locking required.  If there is a prior
# read by s1, concurrent update/delete failures are more common.
#
# To save on test time, we only run a couple SERIALIZABLE tests (for the more
# problematic permutations).
#
# We test updates where s2 updates history that is:
#
# - non-overlapping with s1,
# - contained entirely in s1,
# - partly contained in s1,
# - containing s1 entirely.
#
# We don't need to test where s2 entirely contains s1 in both orderings
# because of symmetry: we test both when s1 precedes s2 and when s2 precedes
# s1, so that scenario is covered.
#
# We test various orderings of the update/delete/commit from s1 and s2.
# Note that `s1lock s2lock s1change` is boring because it's the same as
# `s1lock s1change s2lock`. In other words it doesn't matter if something
# interposes between the lock and its change (as long as everyone is following
# the same policy).

setup
{
  CREATE TABLE products (
	id int4range NOT NULL,
	valid_at daterange NOT NULL,
	price decimal NOT NULL,
	PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));
  INSERT INTO products VALUES
	('[1,2)', '[2020-01-01,2030-01-01)', 5.00);

  CREATE TABLE fpo_lockall_delete_gap (
	id int4range NOT NULL,
	valid_at int4range NOT NULL,
	price integer NOT NULL,
	PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));
  INSERT INTO fpo_lockall_delete_gap VALUES ('[1,2)', '[0,10)', 0);

  CREATE FUNCTION fpo_noop_trig() RETURNS trigger LANGUAGE plpgsql AS $$
  BEGIN
	IF TG_OP = 'DELETE' THEN
	  RETURN OLD;
	END IF;
	RETURN NEW;
  END;
  $$;
}

teardown
{
  DROP TABLE products, fpo_lockall_delete_gap;
  DROP FUNCTION fpo_noop_trig();
}

session s1
setup		{ SET datestyle TO ISO, YMD; }
step s1rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1lock2025 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2025-01-01,2026-01-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s1upd2025	{
  UPDATE products
  FOR PORTION OF valid_at FROM '2025-01-01' TO '2026-01-01'
  SET price = 8.00
  WHERE id = '[1,2)';
}
step s1del2025 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2025-01-01' TO '2026-01-01'
  WHERE id = '[1,2)';
}
step s1q	{ SELECT * FROM products ORDER BY id, valid_at; }
step s1c	{ COMMIT; }
step s1lockgap {
  SELECT * FROM fpo_lockall_delete_gap WHERE id = '[1,2)' FOR UPDATE;
}
step s1delgap {
  DELETE FROM fpo_lockall_delete_gap
  FOR PORTION OF valid_at FROM 0 TO 1
  WHERE id = '[1,2)'
  RETURNING *;
}
step s1qgap	{ SELECT * FROM fpo_lockall_delete_gap ORDER BY id, valid_at; }
step s1trig {
  CREATE TRIGGER products_noop BEFORE UPDATE OR DELETE ON products
  FOR EACH ROW EXECUTE FUNCTION fpo_noop_trig();
}
step s1trigoff	{ ALTER TABLE products DISABLE TRIGGER products_noop; }

session s2
setup		{ SET datestyle TO ISO, YMD; }
step s2rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2ser	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2lock202503 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2025-03-01,2025-04-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s2lock20252026 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2025-06-01,2026-06-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s2lock2027 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2027-01-01,2028-01-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s2lock2021 {
  SELECT * FROM products
  WHERE id = '[1,2)' AND valid_at && '[2021-01-01,2022-01-01)'
  ORDER BY valid_at FOR UPDATE;
}
step s2upd2021 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2021-01-01' TO '2022-01-01'
  SET price = 20.00
  WHERE id = '[1,2)';
}
step s2upd202503 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2025-03-01' TO '2025-04-01'
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2upd20252026 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2025-06-01' TO '2026-06-01'
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2upd2027 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2027-01-01' TO '2028-01-01'
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2del202503 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2025-03-01' TO '2025-04-01'
  WHERE id = '[1,2)';
}
step s2del20252026 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2025-06-01' TO '2026-06-01'
  WHERE id = '[1,2)';
}
step s2del2027 {
  DELETE FROM products
  FOR PORTION OF valid_at FROM '2027-01-01' TO '2028-01-01'
  WHERE id = '[1,2)';
}
step s2upd20242027 {
  UPDATE products
  FOR PORTION OF valid_at FROM '2024-01-01' TO '2027-01-01'
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2updprice {
  UPDATE products
  SET price = 10.00
  WHERE id = '[1,2)';
}
step s2c	{ COMMIT; }
step s2rc2	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2c2	{ COMMIT; }
step s2lockgap {
  SELECT * FROM fpo_lockall_delete_gap WHERE id = '[1,2)' FOR UPDATE;
}
step s2updgap {
  UPDATE fpo_lockall_delete_gap
  FOR PORTION OF valid_at FROM 4 TO 5
  SET price = 2
  WHERE id = '[1,2)'
  RETURNING *;
}

session s3
setup		{ SET datestyle TO ISO, YMD; }
step s3rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s3lockgap {
  SELECT * FROM fpo_lockall_delete_gap WHERE id = '[1,2)' FOR UPDATE;
}
step s3updgap {
  UPDATE fpo_lockall_delete_gap
  FOR PORTION OF valid_at FROM 2 TO 3
  SET price = 3
  WHERE id = '[1,2)'
  RETURNING *;
}
step s3c	{ COMMIT; }

# ########################################
# READ COMMITTED tests, UPDATE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2upd2027 s2c s1lock2025 s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2upd202503 s2c s1lock2025 s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2upd20252026 s2c s1lock2025 s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock2027 s2upd2027 s2c s1q

# s2 loads the updated row
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock202503 s2upd202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock20252026 s2upd20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2upd2027 s1upd2025 s2c s1c s1q

# Workaround:
# s1 updates the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2upd2027 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be updated.
permutation s1rc s2rc s2upd202503 s1upd2025 s2c s1c s1q

# Workaround:
# s1 overwrites the row from s2 and sees its leftovers
permutation s1rc s2rc s2lock202503 s2upd202503 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be updated,
# and s1's leftovers don't conflict with s2's.
permutation s1rc s2rc s2upd20252026 s1upd2025 s2c s1c s1q

# Workaround:
# s1 overwrites the row from s2 and sees its leftovers
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2upd20252026 s1lock2025 s2c s1upd2025 s1c s1q

# Not a problem:
# s2's update leaves the application time alone, so nothing s1 targets was
# moved out of the row and s1 can go ahead.
permutation s1rc s2rc s2updprice s1upd2025 s2c s1c s1q

# Not a problem:
# s2's target contains s1's, so the portion s1 wants is still in the row s1
# finds, even though s2 did leave leftovers behind.
permutation s1rc s2rc s2upd20242027 s1upd2025 s2c s1c s1q

# ########################################
# READ COMMITTED tests, UPDATE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2del2027 s2c s1lock2025 s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2del202503 s2c s1lock2025 s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2del20252026 s2c s1lock2025 s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock2027 s2del2027 s2c s1q

# s2 loads the updated row
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock202503 s2del202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1upd2025 s1c s2lock20252026 s2del20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2del2027 s1upd2025 s2c s1c s1q

# Workaround:
# s1 updates the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2del2027 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2del202503 s1upd2025 s2c s1c s1q

# Workaround:
# s1 sees the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock202503 s2del202503 s1lock2025 s2c s1upd2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be updated either.
permutation s1rc s2rc s2del20252026 s1upd2025 s2c s1c s1q

# Workaround:
# s1 sees the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2del20252026 s1lock2025 s2c s1upd2025 s1c s1q

# ########################################
# READ COMMITTED tests, DELETE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2upd2027 s2c s1lock2025 s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2upd202503 s2c s1lock2025 s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2upd20252026 s2c s1lock2025 s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock2027 s2upd2027 s2c s1q

# s2 ignores the deleted row
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock202503 s2upd202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock20252026 s2upd20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2upd2027 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2upd2027 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be deleted.
permutation s1rc s2rc s2upd202503 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the new row from s2 and its leftovers
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock202503 s2upd202503 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# but EvalPlanQual still matches the row to be deleted,
# and s1 leaves leftovers from the row created by s2.
permutation s1rc s2rc s2upd20252026 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the new row from s2 and its leftovers
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2upd20252026 s1lock2025 s2c s1del2025 s1c s1q

# ########################################
# READ COMMITTED tests, DELETE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rc s2rc s2lock2027 s2del2027 s2c s1lock2025 s1del2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock202503 s2del202503 s2c s1lock2025 s1del2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s2lock20252026 s2del20252026 s2c s1lock2025 s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock2027 s2del2027 s2c s1q

# s2 ignores the deleted row
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock202503 s2del202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rc s2rc s1lock2025 s1del2025 s1c s2lock20252026 s2del20252026 s2c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2del2027 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock2027 s2del2027 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2del202503 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock202503 s2del202503 s1lock2025 s2c s1del2025 s1c s1q

# Problem:
# s1 (without locking) overlooks the leftovers from s2
# and EvalPlanQual no longer matches the row to be deleted either.
permutation s1rc s2rc s2del20252026 s1del2025 s2c s1c s1q

# Workaround:
# s1 deletes the leftovers from s2
# Locking is required or s1 won't see the leftovers.
permutation s1rc s2rc s2lock20252026 s2del20252026 s1lock2025 s2c s1del2025 s1c s1q

# ########################################
# REPEATABLE READ tests, UPDATE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2upd2027 s2c s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd202503 s2c s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1upd2025 s1c s2upd2027 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1upd2025 s1c s2upd202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1upd2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd20252026 s1upd2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s2c s1upd2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s2c s1upd2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2upd2027 s2c s1q

# s2 loads the updated row
permutation s1rr s2rr s1q s1upd2025 s1c s2upd202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s1upd2025 s2c s1c s1q

# ########################################
# REPEATABLE READ tests, UPDATE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2del2027 s2c s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s2del202503 s2c s1upd2025 s1c s1q

# s1 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s2del20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1upd2025 s1c s2del2027 s2c s1q

# s2 loads the updated row
permutation s1rr s2rr s1upd2025 s1c s2del202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1upd2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del20252026 s1upd2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s2c s1upd2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s2c s1upd2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s2c s1upd2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2del2027 s2c s1q

# s2 loads the updated row
permutation s1rr s2rr s1q s1upd2025 s1c s2del202503 s2c s1q

# s2 loads the updated row and sees its leftovers
permutation s1rr s2rr s1q s1upd2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s1upd2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s1upd2025 s2c s1c s1q

# ########################################
# REPEATABLE READ tests, DELETE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2upd2027 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd202503 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2upd20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1del2025 s1c s2upd2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1del2025 s1c s2upd202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1del2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s2upd20252026 s1del2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s2c s1del2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s2c s1del2025 s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2upd2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1q s1del2025 s1c s2upd202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2upd20252026 s2c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent update
permutation s1rr s2rr s1q s2upd20252026 s1del2025 s2c s1c s1q

# ########################################
# REPEATABLE READ tests, DELETE+DELETE:
# ########################################

# s1 sees the leftovers
permutation s1rr s2rr s2del2027 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2del202503 s2c s1del2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1rr s2rr s2del20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1del2025 s1c s2del2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1del2025 s1c s2del202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1del2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s2del20252026 s1del2025 s2c s1c s1q

## with prior read by s1:

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s2c s1del2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s2c s1del2025 s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s2c s1del2025 s1c s1q

# s2 sees the leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2del2027 s2c s1q

# s2 ignores the deleted row
permutation s1rr s2rr s1q s1del2025 s1c s2del202503 s2c s1q

# s2 ignores the deleted row and sees its leftovers
permutation s1rr s2rr s1q s1del2025 s1c s2del20252026 s2c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del2027 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del202503 s1del2025 s2c s1c s1q

# s1 fails from concurrent delete
permutation s1rr s2rr s1q s2del20252026 s1del2025 s2c s1c s1q

# ########################################
# SERIALIZABLE tests, UPDATE+UPDATE:
# ########################################

# s1 sees the leftovers
permutation s1ser s2ser s2upd2027 s2c s1upd2025 s1c s1q

# s1 reloads the updated row and sees its leftovers
permutation s1ser s2ser s2upd20252026 s2c s1upd2025 s1c s1q

# ########################################
# Locking the targeted portion first does not help either:
# ########################################

# Every writer here locks the portion it is about to change, in the same
# transaction as the change, so the protocol is being followed by everyone.
# It still fails.  s1's lock waits for s2, and by the time EvalPlanQual
# re-checks, s2 has narrowed the row out of the portion s1 asked for: the lock
# lands on that narrowed row and s1 gets nothing back.  The history s1 wants
# has moved into a leftover, which s1 never locked and no ctid chain leads to.
# So s2's next transaction locks that leftover without conflicting with s1,
# and the two run concurrently after all.  Reported by Andres Freund.
permutation s1rc s2rc s2lock2027 s2upd2027 s1lock2025 s2c s2rc2 s2lock20252026 s2upd20252026 s1upd2025 s2c2 s1c s1q

# Andres Freund's original form of the same failure: s2's second transaction
# changes a disjoint portion (2021), so s1's lock and EvalPlanQual find the row
# s2 narrowed and nothing else.  Without the check, s1's update of 2025 is
# silently lost.
permutation s1rc s2rc s2lock2027 s2upd2027 s1lock2025 s2c s2rc2 s2lock2021 s2upd2021 s1upd2025 s2c2 s1c s1q

# The same with s2's second transaction committing before s1 updates: then s1's
# UPDATE takes a new snapshot that sees everything, and nothing is lost or
# reported.  This shows the failure above is a concurrency artifact.
permutation s1rc s2rc s2lock2027 s2upd2027 s1lock2025 s2c s2rc2 s2lock2021 s2upd2021 s2c2 s1upd2025 s1c s1q

# ########################################
# Locking the whole key does not serialize across DELETE FOR PORTION OF:
# ########################################

# All three writers lock every row of the key in the same transaction as their
# DML, which is as careful as a client can be.  It still isn't enough.
#
# s1 deletes the original tuple and inserts [1,10) as an independent leftover.
# s2's locking scan waits for s1, then skips the deleted tuple.  Its snapshot
# cannot see the leftover, so it finishes without locking any live row.  s3 can
# therefore lock the whole key and update [2,3).  s2's subsequent UPDATE sees
# [1,10), but waits for s3 and follows its update chain only to [2,3).  That
# tuple fails s2's overlap qual, and s3's [3,10) leftover is invisible to s2's
# UPDATE snapshot, so without the check s2 would change no rows and leave
# [4,5) at price 0.  Reported by Andres Freund.
permutation s1rc s2rc s3rc s1lockgap s1delgap s2lockgap s1c s3lockgap s3updgap s2updgap s3c s2c s1qgap
permutation s1rc s2rc s3rc s1lockgap s1delgap s2lockgap s3lockgap s1c s3updgap s2updgap s3c s2c s1qgap

# ########################################
# READ COMMITTED with a BEFORE row trigger:
# ########################################

# A BEFORE row trigger makes the executor lock the row and run EvalPlanQual
# before calling the trigger, rather than when the update or delete finds the
# row changed.  The check has to happen there too, or s1's change is lost.
permutation s1trig s1rc s2rc s2upd2027 s1upd2025 s2c s1c s1q
permutation s1trig s1rc s2rc s2del2027 s1upd2025 s2c s1c s1q
permutation s1trig s1rc s2rc s2upd2027 s1del2025 s2c s1c s1q
permutation s1trig s1rc s2rc s2del2027 s1del2025 s2c s1c s1q

# A concurrent update that doesn't move s1's target out of the row is still
# followed without complaint.
permutation s1trig s1rc s2rc s2updprice s1upd2025 s2c s1c s1q

# A disabled trigger still takes that path.
permutation s1trig s1trigoff s1rc s2rc s2upd2027 s1upd2025 s2c s1c s1q
