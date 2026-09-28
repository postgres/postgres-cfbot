# Test that ALTER TYPE ... ALTER ATTRIBUTE ... TYPE re-adds a dependent
# domain constraint (and its comment) to the domain it belonged to, even if
# that domain is renamed and another one takes its name while the ALTER is
# waiting for a lock.

setup
{
	CREATE TYPE ct AS (i int);
	CREATE FUNCTION ct_mk(int) RETURNS ct LANGUAGE sql AS 'select row($1)::ct';
	CREATE TABLE t2 (x int CHECK ((ct_mk(x)).i > 0));
	INSERT INTO t2 VALUES (1);
	CREATE DOMAIN d AS int CONSTRAINT d_check CHECK ((ct_mk(value)).i > 0);
	COMMENT ON CONSTRAINT d_check ON DOMAIN d IS 'd comment';
}

teardown
{
	DROP TABLE t2;
	DROP DOMAIN IF EXISTS d;
	DROP DOMAIN IF EXISTS d_old;
	DROP FUNCTION ct_mk(int);
	DROP TYPE ct;
}

session s1
step s1_begin	{ BEGIN; SELECT count(*) FROM t2; }
step s1_commit	{ COMMIT; }

session s2
step s2_alter	{ ALTER TYPE ct ALTER ATTRIBUTE i TYPE bigint; }

session s3
step s3_swap	{ ALTER DOMAIN d RENAME TO d_old; CREATE DOMAIN d AS int; }
step s3_check	{
	SELECT t.typname, c.conname, obj_description(c.oid, 'pg_constraint')
	  FROM pg_constraint c JOIN pg_type t ON t.oid = c.contypid
	 WHERE t.typname IN ('d', 'd_old');
}
step s3_cast	{ SELECT (-1)::d_old; }

permutation s1_begin s2_alter s3_swap s1_commit s3_check s3_cast
