# Deadlock behavior of FOR PORTION OF when sessions lock rows first, with 3
# sessions.
#
# Locking the portion of history you intend to change with SELECT ... FOR
# UPDATE before running FOR PORTION OF used to be the documented way around
# the READ COMMITTED leftover-visibility anomaly.  It is no longer recommended
# -- it isn't a dependable protection, see for-portion-of.spec, and the
# anomaly now raises a serialization failure instead -- but clients may still
# lock rows first, and doing so should carry no deadlock risk beyond the usual
# one: as long as everyone takes the locks in the same order, three or more
# sessions are no different from two.
#
# This spec exercises that with three sessions:
#  - If the sessions lock history portions in an inconsistent (cyclic) order,
#    they deadlock; PostgreSQL detects it, rolls one session back, and the other
#    two complete their FOR PORTION OF updates normally.
#  - If every session locks the same portion first (a consistent policy), they
#    serialize with no deadlock.
#
# The table holds three disjoint validity periods for one id, so there are three
# separate rows to lock.  deadlock_timeout is tiny in s3 and huge in s1/s2, so
# s3 is always the session that runs the deadlock detector and is rolled back.
# datestyle and deadlock_timeout are set at session level (not inside a
# transaction) so they survive s3's rollback.

setup
{
  CREATE TABLE products (
	id int4range NOT NULL,
	valid_at daterange NOT NULL,
	price decimal NOT NULL,
	PRIMARY KEY (id, valid_at WITHOUT OVERLAPS));
  INSERT INTO products VALUES
	('[1,2)', '[2020-01-01,2021-01-01)', 5.00),
	('[1,2)', '[2021-01-01,2022-01-01)', 5.00),
	('[1,2)', '[2022-01-01,2023-01-01)', 5.00);
}

teardown { DROP TABLE products; }

session s1
setup	{ SET datestyle TO ISO, YMD; SET deadlock_timeout = '100s'; }
step s1b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1lock2020	{ SELECT price FROM products WHERE id = '[1,2)' AND valid_at && '[2020-01-01,2021-01-01)' FOR UPDATE; }
step s1lock2021	{ SELECT price FROM products WHERE id = '[1,2)' AND valid_at && '[2021-01-01,2022-01-01)' FOR UPDATE; }
step s1upd2021	{ UPDATE products FOR PORTION OF valid_at FROM '2021-03-01' TO '2021-04-01' SET price = 8.00 WHERE id = '[1,2)'; }
step s1c	{ COMMIT; }
step s1q	{ SELECT id, valid_at, price FROM products ORDER BY valid_at; }

session s2
setup	{ SET datestyle TO ISO, YMD; SET deadlock_timeout = '100s'; }
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2lock2020	{ SELECT price FROM products WHERE id = '[1,2)' AND valid_at && '[2020-01-01,2021-01-01)' FOR UPDATE; }
step s2lock2021	{ SELECT price FROM products WHERE id = '[1,2)' AND valid_at && '[2021-01-01,2022-01-01)' FOR UPDATE; }
step s2lock2022	{ SELECT price FROM products WHERE id = '[1,2)' AND valid_at && '[2022-01-01,2023-01-01)' FOR UPDATE; }
step s2upd2022	{ UPDATE products FOR PORTION OF valid_at FROM '2022-03-01' TO '2022-04-01' SET price = 8.00 WHERE id = '[1,2)'; }
step s2c	{ COMMIT; }

session s3
setup	{ SET datestyle TO ISO, YMD; SET deadlock_timeout = '10ms'; }
step s3b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s3lock2020	{ SELECT price FROM products WHERE id = '[1,2)' AND valid_at && '[2020-01-01,2021-01-01)' FOR UPDATE; }
step s3lock2022	{ SELECT price FROM products WHERE id = '[1,2)' AND valid_at && '[2022-01-01,2023-01-01)' FOR UPDATE; }
step s3c	{ COMMIT; }

# Cyclic lock order: s1 holds 2020 wants 2021; s2 holds 2021 wants 2022;
# s3 holds 2022 wants 2020.  s3 (tiny deadlock_timeout) detects the deadlock and
# is rolled back; s2 then s1 finish their FOR PORTION OF updates and commit.
# The blocking marks force s3's deadlock error to be reported before s2lock2022
# unblocks (the same timing fix used in deadlock-hard.spec).
permutation s1b s2b s3b s1lock2020 s2lock2021 s3lock2022 s1lock2021 s2lock2022(s3lock2020) s3lock2020(*) s2upd2022 s2c s1upd2021 s1c s3c s1q

# Consistent lock order: all three lock the 2020 row first, so they serialize
# with no deadlock.
permutation s1b s2b s3b s1lock2020 s2lock2020 s3lock2020 s1c s2c s3c
