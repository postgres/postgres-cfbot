/* contrib/pg_trgm/pg_trgm--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.7'" to load this file. \quit

-- A GiST opclass identical to gist_trgm_ops, minus the '=' operator.
CREATE OPERATOR FAMILY gist_trgm_ops_noeq USING gist;

CREATE OPERATOR CLASS gist_trgm_ops_noeq
FOR TYPE text USING gist FAMILY gist_trgm_ops_noeq AS
	OPERATOR	1	%  (text, text),
	OPERATOR	2	<-> (text, text) FOR ORDER BY pg_catalog.float_ops,
	OPERATOR	3	pg_catalog.~~ (text, text),
	OPERATOR	4	pg_catalog.~~* (text, text),
	OPERATOR	5	pg_catalog.~ (text, text),
	OPERATOR	6	pg_catalog.~* (text, text),
	OPERATOR	7	%> (text, text),
	OPERATOR	8	<->> (text, text) FOR ORDER BY pg_catalog.float_ops,
	OPERATOR	9	%>> (text, text),
	OPERATOR	10	<->>> (text, text) FOR ORDER BY pg_catalog.float_ops,
	-- deliberately no OPERATOR 11 ( pg_catalog.= )
	FUNCTION	1	gtrgm_consistent (internal, text, smallint, oid, internal),
	FUNCTION	2	gtrgm_union (internal, internal),
	FUNCTION	3	gtrgm_compress (internal),
	FUNCTION	4	gtrgm_decompress (internal),
	FUNCTION	5	gtrgm_penalty (internal, internal, internal),
	FUNCTION	6	gtrgm_picksplit (internal, internal),
	FUNCTION	7	gtrgm_same (gtrgm, gtrgm, internal),
	FUNCTION	8	gtrgm_distance (internal, text, smallint, oid, internal),
	FUNCTION	10	gtrgm_options (internal),
	STORAGE		gtrgm;
