/* src/test/modules/test_idxundo/test_idxundo--1.0.sql */
\echo Use "CREATE EXTENSION test_idxundo" to load this file. \quit

-- Total LP_DEAD line pointers across every block of an index.  Used by the
-- end-to-end tests, where the aborted entries' block is not known in advance.
CREATE FUNCTION idxundo_count_dead_all(index regclass)
RETURNS int AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Does the current transaction hold published index UNDO?  Reports
-- XactUndo.has_undo together with whether a permanent chain-head batch LSN was
-- recorded -- the two pieces AtAbort_XactUndo() requires before it will walk a
-- chain.  An index insert that published neither would make ROLLBACK a
-- silent no-op; this guards against that.
CREATE FUNCTION idxundo_undo_chain_published()
RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Every Aborted Transaction Map entry: which aborts were handed to the logical
-- revert worker, for which database, whether the chain is bounded (a
-- subtransaction's; stop_lsn is NULL for a whole chain), and whether they were
-- reverted.
CREATE FUNCTION idxundo_atm_entries(OUT xid xid, OUT dboid oid,
	OUT last_batch_lsn pg_lsn, OUT stop_lsn pg_lsn, OUT revert_complete bool)
RETURNS SETOF record AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Queue xid in the ATM with an UNDO chain at lsn (which need not be one).
CREATE FUNCTION idxundo_atm_add(xid xid, lsn pg_lsn)
RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Write an UNDO record that fails to apply, in the current (sub)transaction,
-- into the chain of the given relpersistence ('p', 'u' or 't').
CREATE FUNCTION idxundo_write_failing_undo(persistence "char" DEFAULT 'p')
RETURNS void AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Write UNDO records 1..n of an order-logging resource manager in one batch
-- (needs shared_preload_libraries = 'test_idxundo').
CREATE FUNCTION idxundo_write_ordered_undo(n int)
RETURNS void AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- FLUX's count of active transactions (oldest-active timestamp slots).
CREATE FUNCTION idxundo_flux_active_xacts()
RETURNS int AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- table_tuple_delete of t with wait = false: the result and the xid it
-- reports.
CREATE FUNCTION idxundo_delete_nowait(rel regclass, t tid)
RETURNS text AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
