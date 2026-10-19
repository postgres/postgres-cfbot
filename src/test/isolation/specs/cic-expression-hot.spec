# UPDATEs during CREATE INDEX CONCURRENTLY of an expression index.
#
# The build waits for an advisory lock that s1 holds, inside the index
# expression, so s1's UPDATE runs while the index is not yet ready for
# inserts.  The update leaves the expression's value alone; the finished
# index must find every row under its current key.

setup
{
	CREATE TABLE cic_expr (id int PRIMARY KEY, s text, x int)
		WITH (fillfactor = 50);
	INSERT INTO cic_expr SELECT g, 'abc' || g, g FROM generate_series(1, 100) g;
	CREATE FUNCTION cic_lower(text) RETURNS text IMMUTABLE LANGUAGE plpgsql AS $$
		BEGIN PERFORM pg_advisory_lock_shared(4242); PERFORM pg_advisory_unlock_shared(4242); RETURN lower($1); END;
	$$;
}

teardown
{
	DROP TABLE cic_expr;
	DROP FUNCTION cic_lower(text);
}

session s1
step s1_lock	{ SELECT pg_advisory_lock(4242); }
step s1_upd_s	{ UPDATE cic_expr SET s = upper(s); }
step s1_unlock	{ SELECT pg_advisory_unlock(4242); }
step s1_upd2	{ UPDATE cic_expr SET s = lower(s) || ''; }
step s1_check
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*) FROM cic_expr WHERE cic_lower(s) LIKE 'abc%';
	SELECT count(*) FROM cic_expr WHERE cic_lower(s) = 'abc42';
	RESET enable_seqscan;
	RESET enable_bitmapscan;
	SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_expr_idx'::regclass;
}

session s2
step s2_cic_s	{ CREATE INDEX CONCURRENTLY cic_expr_idx ON cic_expr (cic_lower(s)); }

permutation s1_lock s2_cic_s s1_upd_s s1_unlock s1_upd2 s1_check
