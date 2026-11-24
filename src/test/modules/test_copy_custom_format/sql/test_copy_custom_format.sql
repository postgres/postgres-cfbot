LOAD 'test_copy_custom_format';

CREATE TABLE copy_data (a smallint, b integer, c bigint);
INSERT INTO copy_data VALUES (1,2,3),(12,34,56),(123,456,789),(NULL,NULL,NULL);

COPY copy_data TO stdout WITH (format 'test_format');          -- Start, OneRow x3, End

COPY copy_data (a, b) TO stdout WITH (format 'test_format');   -- Start: natts 2
COPY (SELECT a FROM copy_data) TO stdout WITH (format 'test_format'); -- Start: natts 1

-- Write to a file.
\getenv abs_builddir PG_ABS_BUILDDIR
\set filename :abs_builddir '/results/test_copy_custom_format.data'
COPY copy_data TO :'filename' WITH (format 'test_format');

COPY copy_data TO stdout WITH (format 'nonexistent');          -- ERROR: not recognized
COPY copy_data TO stdout WITH (format 'text', format 'csv');   -- ERROR: conflicting

COPY copy_data TO stdout WITH (format 'test_format', freeze true);     -- ERROR: FREEZE with COPY TO
COPY copy_data FROM stdin WITH (format 'test_format');         -- ERROR: no COPY FROM
