# Tests for amnotifydelete with INSERT ... ON CONFLICT.
#
# A speculative insertion that runs into a concurrent insert removes its
# tuple again, after every index was given it through aminsert, so the
# indexes must hear of the removal.  s1 is made to wait while it inserts
# index entries, after its pre-check found no conflict but before the
# unique index sees one: notify_wait() in the expression of spec_idx, which
# comes first because its OID is lower, waits for the controller's lock.
#
# Each session loads the library quietly first: its string reloption
# validator prints notices when it is loaded.  Parallel query is off, as a
# parallel worker would load the library again and print them.

setup
{
	CREATE EXTENSION dummy_index_am;
	CREATE FUNCTION notify_wait(int4) RETURNS int4
		IMMUTABLE LANGUAGE plpgsql AS $$
	BEGIN
		IF current_setting('notify.wait', true) = 'on' THEN
			PERFORM pg_advisory_xact_lock_shared(1);
		END IF;
		RETURN $1;
	END $$;
	CREATE TABLE spec_tab (k int4, v int4);
	CREATE INDEX spec_idx ON spec_tab USING dummy_index_am (notify_wait(v));
	CREATE UNIQUE INDEX spec_key ON spec_tab (k);
}

teardown
{
	DROP TABLE spec_tab;
	DROP FUNCTION notify_wait(int4);
	DROP EXTENSION dummy_index_am;
}

session c
setup
{
	SET client_min_messages = warning;
	LOAD 'dummy_index_am';
	RESET client_min_messages;
	SET debug_parallel_query = off;
}
step c_lock		{ SELECT pg_advisory_lock(1); }
step c_order	{ SELECT 'spec_idx'::regclass::oid < 'spec_key'::regclass::oid; }
step c_unlock	{ SELECT pg_advisory_unlock(1); }
step c_show		{ SELECT * FROM spec_tab ORDER BY k; }

session s1
setup
{
	SET client_min_messages = warning;
	LOAD 'dummy_index_am';
	RESET client_min_messages;
	SET debug_parallel_query = off;
	SET notify.wait = on;
}
step s1_nothing	{ INSERT INTO spec_tab VALUES (1, 10) ON CONFLICT DO NOTHING; }
step s1_update	{ INSERT INTO spec_tab VALUES (1, 10) ON CONFLICT (k) DO UPDATE SET v = excluded.v; }

session s2
setup
{
	SET client_min_messages = warning;
	LOAD 'dummy_index_am';
	RESET client_min_messages;
	SET debug_parallel_query = off;
}
step s2_begin	{ BEGIN; }
step s2_insert	{ INSERT INTO spec_tab VALUES (1, 20); }
step s2_other	{ INSERT INTO spec_tab VALUES (2, 20); }
step s2_abort	{ ROLLBACK; }

# s2 inserts the same key while s1 waits: s1 removes its tuple, which spec_idx
# hears of, and then does nothing
permutation c_lock c_order s1_nothing s2_insert c_unlock c_show

# The same, but s1 then updates s2's row, which spec_idx hears of too
permutation c_lock s1_update s2_insert c_unlock c_show

# s2 inserts another key: no conflict, so nothing is removed
permutation c_lock s1_nothing s2_other c_unlock c_show

# s2's insert of the same key aborts first: no conflict either
permutation c_lock s1_nothing s2_begin s2_insert s2_abort c_unlock c_show
