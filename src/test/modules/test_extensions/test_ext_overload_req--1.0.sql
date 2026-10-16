/* src/test/modules/test_extensions/test_ext_overload_req--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_req" to load this file. \quit

-- Every reference below is to a required extension's object.  A same-named
-- plant in @extschema@ must not capture any of them.
CREATE TABLE @extschema@.captured AS
    SELECT reqcall(1) AS who,
           reqplpgsql() AS who_cached,
           pg_catalog.pg_typeof('abc'::reqdom) AS dom;

INSERT INTO reqtab VALUES ('from script');

CREATE OPERATOR @extschema@.### (leftarg = integer, rightarg = integer,
                                 function = reqeq);

CREATE OPERATOR FAMILY @extschema@.reqfam USING btree;
ALTER OPERATOR FAMILY @extschema@.reqfam USING btree ADD
    OPERATOR 3 === (integer, integer);
