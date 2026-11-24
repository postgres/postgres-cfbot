LOAD 'test_copy_custom_format';

CREATE TABLE copy_data (a smallint, b integer, c bigint);
INSERT INTO copy_data VALUES (1,2,3),(12,34,56),(123,456,789),(NULL,NULL,NULL);
CREATE TABLE copy_data_in (LIKE copy_data);

COPY copy_data TO stdout WITH (format 'test_format');          -- Start, OneRow x3, End
COPY copy_data_in FROM stdin WITH (format 'test_format');      -- Start, End
1,2,3
4,5,6
\.
SELECT * FROM copy_data_in;

COPY copy_data (a, b) TO stdout WITH (format 'test_format');   -- Start: natts 2
COPY (SELECT a FROM copy_data) TO stdout WITH (format 'test_format'); -- Start: natts 1

-- Write to and read from a file.
\getenv abs_builddir PG_ABS_BUILDDIR
\set filename :abs_builddir '/results/test_copy_custom_format.data'
COPY copy_data TO :'filename' WITH (format 'test_format');
COPY copy_data_in FROM :'filename' WITH (format 'test_format');
SELECT * FROM copy_data_in;

COPY copy_data TO stdout WITH (format 'nonexistent');          -- ERROR: not recognized
COPY copy_data TO stdout WITH (format 'text', format 'csv');   -- ERROR: conflicting

COPY copy_data TO stdout WITH (format 'test_format', freeze true);     -- ERROR: FREEZE with COPY TO

-- The format has core convert the fields, so ON_ERROR, LOG_VERBOSITY and
-- REJECT_LIMIT work as for the built-in formats.  Column c is filled with its
-- default value, which is not evaluated for the rows that are dropped.
CREATE TABLE copy_onerr (a int, b int, c serial);
COPY copy_onerr (a, b) FROM stdin WITH (format 'test_format', on_error ignore, log_verbosity verbose);
1,1
x,2
3,y
4,4
\.
COPY copy_onerr (b, a) FROM stdin WITH (format 'test_format', on_error set_null, log_verbosity verbose);
5,x
y,z
\.
SELECT * FROM copy_onerr ORDER BY c;
COPY copy_onerr (a, b) FROM stdin WITH (format 'test_format', on_error ignore, reject_limit 1); -- ERROR
x,1
y,2
\.
COPY copy_onerr (a, b) FROM stdin WITH (format 'test_format', on_error ignore); -- ERROR: too few fields
1
\.

-- The DEFAULT marker.  The dropped column makes the format fill arrays of its
-- own, which core moves into the row.
CREATE TABLE copy_dflt (a int DEFAULT 42, x int, b text);
ALTER TABLE copy_dflt DROP COLUMN x;
COPY copy_dflt (b, a) FROM stdin WITH (format 'test_format', default '\D');
foo,\D
bar,7
null,null
\.
SELECT * FROM copy_dflt;
COPY copy_dflt (b) FROM stdin WITH (format 'test_format', default '\D'); -- ERROR: no default
\D
\.

-- A default value is not evaluated for a row that is dropped either.
CREATE TABLE copy_dflt_seq (id serial, v int);
COPY copy_dflt_seq FROM stdin WITH (format 'test_format', default '\D', on_error ignore);
\D,1
\D,x
\D,3
\.
SELECT * FROM copy_dflt_seq;

-- The format's data ends at a line "END".  Core complains about data that
-- follows, beyond what the format has read ahead into its 64-byte buffer.
COPY copy_data_in FROM stdin WITH (format 'test_format');
7,8,9
END
\.
COPY copy_data_in FROM stdin WITH (format 'test_format'); -- ERROR
10,11,12
END
xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
\.
SELECT * FROM copy_data_in WHERE a IN (7, 10);
