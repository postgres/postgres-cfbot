# Timeouts in the REPACK (CONCURRENTLY) decoding worker.
#
# The worker connects as the table owner in a new session, so the timeouts
# set for that role (or for the database) would apply to it while it waits
# for older transactions to finish.  Instead, the worker adopts the values in
# effect in the session running REPACK, so that the user can control them
# with SET.

setup
{
	CREATE ROLE regress_repack_timeout;
	ALTER ROLE regress_repack_timeout SET lock_timeout = '100ms';

	CREATE TABLE repack_timeout_tab (a int PRIMARY KEY);
	INSERT INTO repack_timeout_tab VALUES (1), (2);
	ALTER TABLE repack_timeout_tab OWNER TO regress_repack_timeout;

	CREATE TABLE repack_timeout_other (a int);
}

teardown
{
	DROP TABLE repack_timeout_tab, repack_timeout_other;
	DROP ROLE regress_repack_timeout;
}

# Hold an XID that the decoding worker has to wait for.
session s1
step s1_begin	{ BEGIN; INSERT INTO repack_timeout_other VALUES (1); }
# Keep the worker waiting for longer than the role's lock_timeout.
step s1_sleep	{ SELECT pg_sleep(0.5); }
step s1_commit	{ COMMIT; }
teardown	{ ABORT; }

session s2
step s2_lto	{ SET lock_timeout = '100ms'; }
step s2_repack	{ REPACK (CONCURRENTLY) repack_timeout_tab; }

# The role-level lock_timeout does not reach the worker: REPACK waits until
# s1 finishes.  With lock_timeout set in the REPACK session, the worker's
# wait is cancelled by that value.
permutation s1_begin s2_repack s1_sleep s1_commit s1_begin s2_lto s2_repack(*)
