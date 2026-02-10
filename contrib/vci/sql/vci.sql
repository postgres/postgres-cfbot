CREATE EXTENSION vci;
SELECT amname, amhandler, amtype FROM pg_am WHERE amname = 'vci';

SET vci.table_rows_threshold = 0;

CREATE TABLE testtable (
       key  int,
       cond int,
       c01a bool,
       c01b bool,
       c02 bytea,
       c03 "char",
       c05 int8,
       c06 int2,
       c07 int4,
       c08 text,
       c09 float4,
       c10 float8,
       c13 interval,
       c15 money,
       c16 bpchar,
       c17 varchar,
       c18 date,
       c19 time,
       c20 timetz,
       c21 timestamp,
       c22 timestamptz,
       c23a bit,
       c23b bit,
       c24a varbit,
       c24b varbit,
       c25 numeric,
       c26 uuid);

-- Input data
INSERT INTO testtable (key, cond, c01a, c01b, c02, c03, c05, c06, c07, c08, c09, c10, c13, c15, c16, c17, c18, c19, c20, c21, c22, c23a, c23b, c24a, c24b, c25, c26)
SELECT
        i % 10, -- key int
        i % 21.000000000000, -- cond int
        (i % 3.000000000000) > 0,  -- c01a bool
        (i % 11.000000000000) > 0, -- c01b bool
        CAST(to_char((i % 1001.000000000000), '9999') AS bytea), -- c02 bytea
        CAST(CAST((i % 249.000000000000) AS character varying) AS "char"), -- c03 "char"
        i % 651.000000000000 + i % 350, -- c05 int8
        i % 1001.000000000000, -- c06 in2
        i % 1001.000000000000, -- c07 int4
        i % 1001.000000000000, -- c08 text
        i % 1001.000000000000, -- c09 float4
        i % 1001.000000000000, -- c10 float8
        i % 1001.000000000000 * interval '1h', -- c13 interval
        (i % 1001.000000000000)::integer::money, -- c15 money
        i % 1001.000000000000, -- c16 bpchar
        i % 1001.000000000000, -- c17 varchar
        date '2015-12-21' +  1 % 1001.000000000000 * interval '1d', -- c18 date
        TIMESTAMP '2015-12-21' + (i % 1001.000000000000) * interval '1h', -- c19 time
        TIMESTAMP WITH TIME ZONE '2015-12-21 10:00:00+09' + (i % 1001.000000000000) * interval '1h', -- c20 timetz
        TIMESTAMP '2015-12-21' + (i % 1001.000000000000) * interval '1h', -- c21 timestamp
        TIMESTAMP WITH TIME ZONE '2015-12-21 10:00:00+09' + (i % 1001.000000000000) * interval '1h', -- c22 timestamptz
        ((i % 3.000000000000)>0)::integer::bit(1),  -- c23a bit
        ((i % 11.000000000000)>0)::integer::bit(1), -- c23b bit
        (i % 1001.000000000000)::integer::bit(10),     -- c24a varbit
        (i % 999.000000000000)::integer::bit(10), -- c24b varbit
        i % 1001.000000000000, -- c25 numeric
        'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::uuid -- c26 uuid
FROM generate_series(1, 10000) AS i;

-- Testcase: insert with some NULL values
INSERT INTO testtable (key, cond, c01a, c01b, c02, c03, c05, c06, c07, c08, c09, c10, c13, c15, c16, c17, c18, c19, c20, c21, c22, c23a, c23b, c24a, c24b, c25, c26) SELECT i, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL FROM generate_series(1, 9) AS i;

-- Testcase: insert with some special values
INSERT INTO testtable (key, cond, c09, c10, c18, c21, c22) VALUES (7, 1, 'Infinity', 'Infinity', 'Infinity', 'infinity', 'infinity');
INSERT INTO testtable (key, cond, c09, c10, c18, c21, c22) VALUES (8, 1, 'Infinity', 'Infinity', 'Infinity', 'infinity', 'infinity');
INSERT INTO testtable (key, cond, c09, c10, c18, c21, c22) VALUES (8, 1, '-Infinity', '-Infinity', '-Infinity', '-infinity', '-infinity');
INSERT INTO testtable (key, cond, c09, c10) VALUES (9, 1, 'NaN', 'NaN');

-- Testcase: NaN only
INSERT INTO testtable (key, cond, c09, c10) VALUES (10, 1, 'NaN', 'NaN');
INSERT INTO testtable (key, cond, c09, c10) VALUES (10, 1, 'NaN', 'NaN');
INSERT INTO testtable (key, cond, c05)           VALUES (10, 1, 1);

-- Testcase: Timestamp with timezone
INSERT INTO testtable (key, cond, c18, c22) VALUES (11, 1, TIMESTAMP WITH TIME ZONE '2004-10-19 01:00:00+01', TIMESTAMP WITH TIME ZONE '2004-10-19 02:00:00+01');
INSERT INTO testtable (key, cond, c18, c22) VALUES (11, 1, TIMESTAMP WITH TIME ZONE '2004-10-19 02:00:00+02', TIMESTAMP WITH TIME ZONE '2004-10-19 02:00:00+02');
INSERT INTO testtable (key, cond, c18, c22) VALUES (11, 1, TIMESTAMP WITH TIME ZONE '2004-10-19 01:00:00+02', TIMESTAMP WITH TIME ZONE '2004-10-19 01:00:00+02');
INSERT INTO testtable (key, cond, c05) VALUES (11, 1, 1);

-- Testcase: few attributes are valid
INSERT INTO testtable (key, cond, c01a, c01b, c02, c03, c05, c06, c07, c08, c09, c10, c13, c15, c16, c17, c18, c19, c20, c21, c22, c23a, c23b, c24a, c24b, c25, c26) SELECT 98, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL FROM generate_series(1, 50) AS i;

INSERT INTO testtable (key, cond, c01a, c01b, c02, c03, c05, c06, c07, c08, c09, c10, c15, c16, c17, c18, c19, c20, c21, c22, c23a, c23b, c24a, c24b, c25) VALUES (98, 1, true, true, 'text', 1::char, 1, 1, 1, 'text', 1.0, 1.0, 1, 'text', 'text', timestamp '2015-12-22', timestamp '2015-12-22', timestamp with time zone '2015-12-22 10:23:54+02', timestamp '2015-12-22', timestamp with time zone '2015-12-22 10:23:54+02', 1::bit(1), 1::bit(1), 1::bit(10), 1::bit(10), 1);

INSERT INTO testtable (key, cond, c01a, c01b, c02, c03, c05, c06, c07, c08, c09, c10, c13, c15, c16, c17, c18, c19, c20, c21, c22, c23a, c23b, c24a, c24b, c25, c26) SELECT 98, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL FROM generate_series(1, 50) AS i;

INSERT INTO testtable (key, cond, c01a, c01b, c02, c03, c05, c06, c07, c08, c09, c10, c13, c15, c16, c17, c18, c19, c20, c21, c22, c23a, c23b, c24a, c24b, c25, c26) SELECT 99, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL FROM generate_series(1, 100) AS i;

-- Create an index which uses VCI index access method
CREATE INDEX testindex ON testtable USING vci (key, cond, c01a, c01b, c02, c03, c05, c06, c07, c08, c09, c10, c13, c15, c16, c17, c18, c19, c20, c21, c22, c23a, c23b, c24a, c24b, c25, c26);

-- We expect VCI plans are chosen here
EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, SUMMARY OFF, BUFFERS OFF)
SELECT key, count(*) AS count_star, count(c05) AS count_c05 FROM testtable WHERE NOT cond = 0 GROUP BY key ORDER BY key;

-- Confirms the aggregation can work. The first column indicates whether the
-- VCI scan was used.
SELECT vci_runs_in_query() AS vci_runs_in_query, key, count(*) AS count_star, count(c05) AS count_c05 FROM testtable WHERE NOT cond = 0 GROUP BY key ORDER BY key;

-- cleanup
DROP TABLE testtable;
