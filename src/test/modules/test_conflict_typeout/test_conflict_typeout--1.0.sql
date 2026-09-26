/* src/test/modules/test_conflict_typeout/test_conflict_typeout--1.0.sql */

\echo Use "CREATE EXTENSION test_conflict_typeout" to load this file. \quit

CREATE TYPE boomtype;

CREATE FUNCTION boomtype_in(cstring) RETURNS boomtype
    AS 'MODULE_PATHNAME', 'boomtype_in' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION boomtype_out(boomtype) RETURNS cstring
    AS 'MODULE_PATHNAME', 'boomtype_out' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION boomtype_send(boomtype) RETURNS bytea
    AS 'MODULE_PATHNAME', 'boomtype_send' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION boomtype_recv(internal) RETURNS boomtype
    AS 'MODULE_PATHNAME', 'boomtype_recv' LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE boomtype (
    INPUT = boomtype_in,
    OUTPUT = boomtype_out,
    SEND = boomtype_send,
    RECEIVE = boomtype_recv,
    INTERNALLENGTH = VARIABLE,
    STORAGE = plain
);

CREATE FUNCTION boomtype_cmp(boomtype, boomtype) RETURNS int4
    AS 'MODULE_PATHNAME', 'boomtype_cmp' LANGUAGE C IMMUTABLE STRICT;

/*
 * Written directly in C, not as SQL-language wrappers around boomtype_cmp:
 * the apply worker runs with search_path = '', and an unqualified name
 * inside a SQL function body would fail to resolve there.
 */
CREATE FUNCTION boomtype_lt(boomtype, boomtype) RETURNS bool
    AS 'MODULE_PATHNAME', 'boomtype_lt' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION boomtype_eq(boomtype, boomtype) RETURNS bool
    AS 'MODULE_PATHNAME', 'boomtype_eq' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION boomtype_gt(boomtype, boomtype) RETURNS bool
    AS 'MODULE_PATHNAME', 'boomtype_gt' LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR < (LEFTARG = boomtype, RIGHTARG = boomtype, PROCEDURE = boomtype_lt);
CREATE OPERATOR = (LEFTARG = boomtype, RIGHTARG = boomtype, PROCEDURE = boomtype_eq,
    COMMUTATOR = =);
CREATE OPERATOR > (LEFTARG = boomtype, RIGHTARG = boomtype, PROCEDURE = boomtype_gt);

CREATE OPERATOR CLASS boomtype_ops DEFAULT FOR TYPE boomtype USING btree AS
    OPERATOR 1 <,
    OPERATOR 3 =,
    OPERATOR 5 >,
    FUNCTION 1 boomtype_cmp(boomtype, boomtype);
