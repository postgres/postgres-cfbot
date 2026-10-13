--
-- FLUX: a DELETE (plain or MERGE) must stay deleted when a BEFORE STATEMENT
-- trigger advanced the command counter before the row was deleted.
--
-- The trigger's PERFORM makes plpgsql run CommandCounterIncrement, so the
-- current command id is past the statement's output cid when the row is
-- deleted.  The undo record of the delete must carry the output cid, as
-- heap's cmax does; otherwise the next command treats the delete as a later
-- command of its own transaction, reconstructs the deleted row from undo
-- and serves it again.  Heap is the control.
--
CREATE FUNCTION fmd_trig() RETURNS trigger LANGUAGE plpgsql AS
$$ BEGIN PERFORM 1; RETURN NULL; END $$;

CREATE TABLE fmd_heap (tid int, balance int) USING heap;
CREATE TABLE fmd_flux (tid int, balance int) USING flux;
CREATE TABLE fmd_src (sid int, delta int);
INSERT INTO fmd_src VALUES (2, 5), (3, 20), (4, 40);

DO $$
DECLARE
	t text;
BEGIN
	FOREACH t IN ARRAY ARRAY['fmd_heap', 'fmd_flux'] LOOP
		EXECUTE format('INSERT INTO %I VALUES (1, 10), (2, 20), (3, 30)', t);
		EXECUTE format('CREATE TRIGGER fmd_bsd BEFORE DELETE ON %I
						FOR EACH STATEMENT EXECUTE FUNCTION fmd_trig()', t);
		EXECUTE format('CREATE TRIGGER fmd_bsu BEFORE UPDATE ON %I
						FOR EACH STATEMENT EXECUTE FUNCTION fmd_trig()', t);
	END LOOP;
END $$;

-- plain DELETE of a row not yet modified by this transaction
BEGIN;
DELETE FROM fmd_heap WHERE tid = 3;
DELETE FROM fmd_flux WHERE tid = 3;
SELECT tid, balance FROM fmd_heap ORDER BY tid;
SELECT tid, balance FROM fmd_flux ORDER BY tid;
ROLLBACK;

-- plain DELETE of a row this transaction updated in an earlier command
BEGIN;
UPDATE fmd_heap SET balance = 0 WHERE tid = 3;
UPDATE fmd_flux SET balance = 0 WHERE tid = 3;
DELETE FROM fmd_heap WHERE tid = 3;
DELETE FROM fmd_flux WHERE tid = 3;
SELECT tid, balance FROM fmd_heap ORDER BY tid;
SELECT tid, balance FROM fmd_flux ORDER BY tid;
ROLLBACK;

-- the merge.sql "classic UPSERT, with a DELETE"
BEGIN;
UPDATE fmd_heap SET balance = 0 WHERE tid = 3;
UPDATE fmd_flux SET balance = 0 WHERE tid = 3;
MERGE INTO fmd_heap t USING fmd_src s ON t.tid = s.sid
WHEN MATCHED AND t.balance > s.delta THEN
	UPDATE SET balance = t.balance - s.delta
WHEN MATCHED THEN
	DELETE
WHEN NOT MATCHED THEN
	INSERT VALUES (s.sid, s.delta);
MERGE INTO fmd_flux t USING fmd_src s ON t.tid = s.sid
WHEN MATCHED AND t.balance > s.delta THEN
	UPDATE SET balance = t.balance - s.delta
WHEN MATCHED THEN
	DELETE
WHEN NOT MATCHED THEN
	INSERT VALUES (s.sid, s.delta);
SELECT tid, balance FROM fmd_heap ORDER BY tid;
SELECT tid, balance FROM fmd_flux ORDER BY tid;
ROLLBACK;

-- an UPDATE behind the same trigger must keep its new value, too
BEGIN;
UPDATE fmd_flux SET balance = 0 WHERE tid = 3;
UPDATE fmd_flux SET balance = 7 WHERE tid = 3;
SELECT tid, balance FROM fmd_flux ORDER BY tid;
ROLLBACK;

-- after ROLLBACK both tables are back to the original rows
SELECT tid, balance FROM fmd_heap ORDER BY tid;
SELECT tid, balance FROM fmd_flux ORDER BY tid;

DROP TABLE fmd_heap, fmd_flux, fmd_src;
DROP FUNCTION fmd_trig();
