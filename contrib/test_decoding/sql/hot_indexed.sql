-- Logical decoding of HOT-indexed UPDATEs.  A HOT-indexed update is an
-- ordinary heap update at the WAL level (the new version is logged in full),
-- so it must decode exactly like any other update.  Exercise a chain of
-- HOT-indexed updates under REPLICA IDENTITY FULL so the decoded old tuple and
-- new tuple can both be checked.
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

CREATE TABLE hi_decode (id int PRIMARY KEY, a int, b int) WITH (fillfactor = 50);
CREATE INDEX hi_decode_a ON hi_decode (a);
CREATE INDEX hi_decode_b ON hi_decode (b);
ALTER TABLE hi_decode REPLICA IDENTITY FULL;

INSERT INTO hi_decode VALUES (1, 10, 100);
-- each update changes one indexed column, so each stays HOT-indexed
UPDATE hi_decode SET a = 11 WHERE id = 1;
UPDATE hi_decode SET b = 101 WHERE id = 1;
UPDATE hi_decode SET a = 12 WHERE id = 1;
-- cycle a away and back (ABA) and then delete
UPDATE hi_decode SET a = 99 WHERE id = 1;
UPDATE hi_decode SET a = 12 WHERE id = 1;
DELETE FROM hi_decode WHERE id = 1;

SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL,
       'include-xids', '0', 'skip-empty-xacts', '1');

SELECT pg_drop_replication_slot('regression_slot');
DROP TABLE hi_decode;
