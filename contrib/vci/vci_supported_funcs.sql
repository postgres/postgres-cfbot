-- sys_func_table A table that registers the OID of system-related functions from multiple system catalogs

CREATE TEMPORARY TABLE test (funcoid oid);
INSERT INTO test (funcoid) SELECT unnest(ARRAY[aggfnoid, aggtransfn, aggfinalfn, aggmtransfn, aggminvtransfn, aggmfinalfn]) FROM pg_aggregate;
INSERT INTO test (funcoid) SELECT amhandler FROM pg_am;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[amproc]) FROM pg_amproc;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[castfunc]) FROM pg_cast;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[conproc]) FROM pg_conversion;
INSERT INTO test (funcoid) SELECT evtfoid FROM pg_event_trigger;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[fdwhandler, fdwvalidator]) FROM pg_foreign_data_wrapper;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[lanplcallfoid, laninline, lanvalidator]) FROM pg_language;
INSERT INTO test (funcoid) SELECT tgfoid FROM pg_trigger;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[oprcode, oprrest, oprjoin]) FROM pg_operator;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[rngcanonical, rngsubdiff]) FROM pg_range;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[prsstart, prstoken, prsend, prsheadline, prslextype]) FROM pg_ts_parser;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[tmplinit, tmpllexize]) FROM pg_ts_template;
INSERT INTO test (funcoid) SELECT unnest(ARRAY[typinput, typoutput, typreceive, typsend, typmodin, typmodout, typanalyze]) FROM pg_type;

DROP TABLE IF EXISTS sys_func_table;
CREATE TABLE sys_func_table (funcoid oid UNIQUE);
INSERT INTO sys_func_table SELECT distinct funcoid FROM test WHERE funcoid > 0 ORDER BY funcoid;

DROP TABLE IF EXISTS safe_types;
CREATE TABLE safe_types (typeoid oid UNIQUE);
INSERT INTO safe_types (typeoid) VALUES
       (16), -- bool
       (17), -- bytea
       (18), -- char
       (19), -- name
       (20), -- int8
       (21), -- int2
       (23), -- int4
--     (24) -- regproc
       (25), -- text
--     (26) -- oid
--     (27) -- tid
--     (28) -- xid
--     (30) -- oidvector
--     (71) -- pg_type
--     (75) -- pg_attribute
--     (114) -- json [not supported]
--     (142) -- xml [not supported]
--     (143) -- _xml [not supported]
--     (194) -- pg_node_tree
--     (210) -- smgr
--     (600), -- point [not supported]
--     (601), -- lseg [not supported]
--     (602), -- path [not supported]
--     (603), -- box [not supported]
--     (604), -- polygon [not supported]
--     (628), -- line [not supported]
       (700), -- float4
       (701), -- float8
--     (718), -- circle [not supported]
       (790), -- money
--     (829), -- macaddr [not supported]
--     (869), -- inet [not supported]
--     (650), -- cidr [not supported]
       (1003), -- _name
       (1005), -- _int2
       (1007), -- _int4
       (1009), -- _text
       (1021), -- _float4
--     (1033) -- aclitem
--     (1034) -- _aclitem
       (1042), -- bpchar
       (1082), -- date
       (1083), -- time
       (1114), -- timestamp
       (1184), -- timestamptz
       (1186), -- interval
       (1266), -- timetz
       (1560), -- bit
       (1700), -- numeric
--     (1790), -- refcursor
--     (2202), -- regprocedure
--     (2203), -- regoper
--     (2204), -- regoperator
--     (2205), -- regclass
--     (2206), -- regtype
--     (3220), -- pg_lsn
--     (3614), -- tsvector [not supported]
--     (3615), -- tsquery [not supported]
--     (3734), -- regconfig
--     (3769), -- regdictionary
--     (3802), -- jsonb [not supported]
--     (2970), -- txid_snapshot
--     (3904), -- int4range [not supported]
--     (3906), -- numrange [not supported]
--     (3908), -- tsrange [not supported]
--     (3910), -- tstzrange [not supported]
--     (3912), -- daterange [not supported]
--     (3926), -- int8range [not supported]
       (2249), -- record
--     (2275) -- cstring
       (2276), -- any
       (2277), -- anyarray
       (2278), -- void
--     (2279) -- trigger
--     (2281) -- internal
--     (2282) -- opaque
       (2283), -- anyelement
       (3500); -- anyenum
--     (3831)  -- anyrange

DROP FUNCTION IF EXISTS print_typename;
CREATE FUNCTION print_typename(IN oids _oid) RETURNS _name AS $$
	   SELECT array_agg(pg_type.typname) FROM unnest(oids) AS t(i), pg_type WHERE i = pg_type.oid;
$$ LANGUAGE SQL;

SELECT oid, proname, provolatile, prolang, print_typename(array_prepend(prorettype, proargtypes)) FROM pg_proc WHERE prokind = 'f' AND NOT proretset
    AND NOT EXISTS (SELECT funcoid FROM sys_func_table WHERE pg_proc.oid = sys_func_table.funcoid)
    AND (SELECT bool_and(i IN (SELECT typeoid FROM safe_types)) FROM unnest(array_prepend(prorettype, proargtypes)) AS t(i))
    AND oid < 16384 ORDER BY oid;
