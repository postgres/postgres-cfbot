# An UPDATE whose SET clause cannot change what an index reads, waiting on
# a concurrent UPDATE that does change it.  The waiting UPDATE recomputes
# its new row from the version the other one wrote, and must leave the
# index entry for that version in place.  The waiting row is the second
# row of the UPDATE, so that the conclusion drawn from the SET clause is
# used for it.

setup
{
	CREATE TABLE subattr_epq (id int PRIMARY KEY, doc jsonb)
		WITH (fillfactor = 50);
	CREATE INDEX subattr_epq_status ON subattr_epq ((doc->>'status'));
	CREATE INDEX subattr_epq_vip ON subattr_epq (id) WHERE (doc->>'vip') = 'yes';
	INSERT INTO subattr_epq
		SELECT g, jsonb_build_object('status', 'a', 'vip', 'yes', 'n', 0)
		FROM generate_series(1, 2) g;
}

teardown
{
	DROP TABLE subattr_epq;
}

session s1
step s1_n		{ UPDATE subattr_epq SET doc = jsonb_set(doc, ARRAY['n'], '1'); }
step s1_check
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT id FROM subattr_epq WHERE doc->>'status' = 'b';
	SELECT id FROM subattr_epq WHERE (doc->>'vip') = 'yes' AND id IN (1, 2);
	RESET enable_seqscan;
	RESET enable_bitmapscan;
	SELECT id, doc FROM subattr_epq ORDER BY id;
}

session s2
step s2_begin	{ BEGIN; }
step s2_status	{ UPDATE subattr_epq SET doc = jsonb_set(doc, ARRAY['status'], to_jsonb('b'::text)) WHERE id = 2; }
step s2_vip		{ UPDATE subattr_epq SET doc = jsonb_set(doc, ARRAY['vip'], to_jsonb('no'::text)) WHERE id = 2; }
step s2_commit	{ COMMIT; }

permutation s2_begin s2_status s1_n s2_commit s1_check
permutation s2_begin s2_vip s1_n s2_commit s1_check
