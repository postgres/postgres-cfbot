/* contrib/btree_gist/btree_gist--1.9--1.10.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.10'" to load this file. \quit

-- Add cross-type operator support for the integer trio (int2, int4, int8)
-- to the existing GiST operator families.
--
-- GiST's amvalidate requires support functions in a family to have matching
-- left/right input types, so the catalog additions below are deliberately
-- pg_amop-only. The existing consistent/distance support functions dispatch
-- on the subtype OID: same-type queries take the normal path, while mixed-width
-- integer queries select a cross-type comparison callback that reads the query
-- and key sides at their own widths (see btree_int{2,4,8}.c).

CREATE FUNCTION int2_int4_dist(int2, int4)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int4_int2_dist(int4, int2)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int2_int8_dist(int2, int8)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8_int2_dist(int8, int2)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int4_int8_dist(int4, int8)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION int8_int4_dist(int8, int4)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = int2,
	RIGHTARG = int4,
	PROCEDURE = int2_int4_dist,
	COMMUTATOR = '<->'
);

CREATE OPERATOR <-> (
	LEFTARG = int4,
	RIGHTARG = int2,
	PROCEDURE = int4_int2_dist,
	COMMUTATOR = '<->'
);

CREATE OPERATOR <-> (
	LEFTARG = int2,
	RIGHTARG = int8,
	PROCEDURE = int2_int8_dist,
	COMMUTATOR = '<->'
);

CREATE OPERATOR <-> (
	LEFTARG = int8,
	RIGHTARG = int2,
	PROCEDURE = int8_int2_dist,
	COMMUTATOR = '<->'
);

CREATE OPERATOR <-> (
	LEFTARG = int4,
	RIGHTARG = int8,
	PROCEDURE = int4_int8_dist,
	COMMUTATOR = '<->'
);

CREATE OPERATOR <-> (
	LEFTARG = int8,
	RIGHTARG = int4,
	PROCEDURE = int8_int4_dist,
	COMMUTATOR = '<->'
);

ALTER OPERATOR FAMILY gist_int2_ops USING gist ADD
	OPERATOR	1	<  (int2, int4),
	OPERATOR	2	<= (int2, int4),
	OPERATOR	3	=  (int2, int4),
	OPERATOR	4	>= (int2, int4),
	OPERATOR	5	>  (int2, int4),
	OPERATOR	6	<> (int2, int4),
	OPERATOR	15	<-> (int2, int4) FOR ORDER BY pg_catalog.integer_ops,
	OPERATOR	1	<  (int2, int8),
	OPERATOR	2	<= (int2, int8),
	OPERATOR	3	=  (int2, int8),
	OPERATOR	4	>= (int2, int8),
	OPERATOR	5	>  (int2, int8),
	OPERATOR	6	<> (int2, int8),
	OPERATOR	15	<-> (int2, int8) FOR ORDER BY pg_catalog.integer_ops;

ALTER OPERATOR FAMILY gist_int4_ops USING gist ADD
	OPERATOR	1	<  (int4, int2),
	OPERATOR	2	<= (int4, int2),
	OPERATOR	3	=  (int4, int2),
	OPERATOR	4	>= (int4, int2),
	OPERATOR	5	>  (int4, int2),
	OPERATOR	6	<> (int4, int2),
	OPERATOR	15	<-> (int4, int2) FOR ORDER BY pg_catalog.integer_ops,
	OPERATOR	1	<  (int4, int8),
	OPERATOR	2	<= (int4, int8),
	OPERATOR	3	=  (int4, int8),
	OPERATOR	4	>= (int4, int8),
	OPERATOR	5	>  (int4, int8),
	OPERATOR	6	<> (int4, int8),
	OPERATOR	15	<-> (int4, int8) FOR ORDER BY pg_catalog.integer_ops;

ALTER OPERATOR FAMILY gist_int8_ops USING gist ADD
	OPERATOR	1	<  (int8, int2),
	OPERATOR	2	<= (int8, int2),
	OPERATOR	3	=  (int8, int2),
	OPERATOR	4	>= (int8, int2),
	OPERATOR	5	>  (int8, int2),
	OPERATOR	6	<> (int8, int2),
	OPERATOR	15	<-> (int8, int2) FOR ORDER BY pg_catalog.integer_ops,
	OPERATOR	1	<  (int8, int4),
	OPERATOR	2	<= (int8, int4),
	OPERATOR	3	=  (int8, int4),
	OPERATOR	4	>= (int8, int4),
	OPERATOR	5	>  (int8, int4),
	OPERATOR	6	<> (int8, int4),
	OPERATOR	15	<-> (int8, int4) FOR ORDER BY pg_catalog.integer_ops;
