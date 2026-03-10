# UPDATEs whose target row is updated concurrently, with a BEFORE ROW UPDATE
# trigger.  The trigger code locks the latest version and rebuilds the new
# row from it, so deciding which indexes the UPDATE changes must compare the
# new row with that latest version, not with the one the UPDATE first read.
#
# The row starts with status a, inside the partial index.  s2 changes it and
# holds the lock; s1 then sets it back and waits.  Compared with the version
# s1 first read, s1 would change nothing, and every index would keep the
# entry for the version s2 wrote.

setup
{
	CREATE TABLE hot_epq (id int PRIMARY KEY, status text, doc jsonb, vip bool)
		WITH (fillfactor = 50);
	CREATE INDEX hot_epq_status ON hot_epq (status);
	CREATE INDEX hot_epq_doc ON hot_epq ((doc->>'status'));
	CREATE INDEX hot_epq_vip ON hot_epq (id) WHERE vip;
	INSERT INTO hot_epq VALUES (1, 'a', jsonb_build_object('status', 'a'), true);
	CREATE FUNCTION hot_epq_trig() RETURNS trigger LANGUAGE plpgsql AS
		$$BEGIN RETURN NEW; END$$;
	CREATE TRIGGER hot_epq_before BEFORE UPDATE ON hot_epq
		FOR EACH ROW EXECUTE FUNCTION hot_epq_trig();
}

teardown
{
	DROP TABLE hot_epq;
	DROP FUNCTION hot_epq_trig();
}

session s1
step s1_status	{ UPDATE hot_epq SET status = 'a' WHERE id = 1; }
step s1_doc		{ UPDATE hot_epq SET doc = jsonb_set(doc, ARRAY['status'], to_jsonb('a'::text)) WHERE id = 1; }
step s1_vip		{ UPDATE hot_epq SET vip = true WHERE id = 1; }
step s1_check
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*) AS status_a FROM hot_epq WHERE status = 'a';
	SELECT count(*) AS doc_a FROM hot_epq WHERE doc->>'status' = 'a';
	SELECT count(*) AS vip FROM hot_epq WHERE vip AND id = 1;
	RESET enable_seqscan;
	RESET enable_bitmapscan;
}

session s2
step s2_begin	{ BEGIN; }
step s2_status	{ UPDATE hot_epq SET status = 'b' WHERE id = 1; }
step s2_doc		{ UPDATE hot_epq SET doc = jsonb_set(doc, ARRAY['status'], to_jsonb('b'::text)) WHERE id = 1; }
step s2_vip		{ UPDATE hot_epq SET vip = false WHERE id = 1; }
step s2_commit	{ COMMIT; }

permutation s2_begin s2_status s1_status s2_commit s1_check
permutation s2_begin s2_doc s1_doc s2_commit s1_check
permutation s2_begin s2_vip s1_vip s2_commit s1_check
