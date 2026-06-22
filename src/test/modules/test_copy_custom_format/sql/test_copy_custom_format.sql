LOAD 'test_copy_custom_format';

CREATE TABLE copy_data (a smallint, b integer, c bigint);
INSERT INTO copy_data VALUES (1,2,3),(12,34,56),(123,456,789);

COPY copy_data TO stdout WITH (format 'test_format');          -- Start, OutFunc x3, OneRow x3, End
COPY copy_data FROM stdin WITH (format 'test_format');         -- InFunc x3, Start, OneRow, End
\.

COPY copy_data (a, b) TO stdout WITH (format 'test_format');   -- Start: natts 2
COPY (SELECT a FROM copy_data) TO stdout WITH (format 'test_format'); -- Start: natts 1

COPY copy_data TO stdout WITH (format 'nonexistent');          -- ERROR: not recognized
COPY copy_data TO stdout WITH (format 'text', format 'csv');   -- ERROR: conflicting

COPY copy_data TO stdout WITH (format 'test_format', bogus 1); -- ERROR

COPY copy_data TO stdout WITH (format 'test_format', max_attributes 5); -- OK
COPY copy_data TO stdout WITH (format 'test_format', max_attributes 3); -- OK
COPY copy_data TO stdout WITH (format 'test_format', max_attributes 2); -- ERROR: 3 columns exceeds 2
COPY copy_data TO stdout WITH (format 'test_format', max_attributes 0);   -- ERROR: positive
COPY copy_data TO stdout WITH (format 'test_format', max_attributes -1);  -- ERROR
COPY copy_data TO stdout WITH (format 'test_format', max_attributes 'x'); -- ERROR: integer required

COPY copy_data FROM stdin WITH (format 'test_format', freeze true, disallow_freeze true); -- ERROR (validate)
COPY copy_data FROM stdin WITH (format 'test_format', disallow_freeze true); -- OK
\.

-- The built-in options are handled in the same way of built-in formats.
COPY copy_data TO stdout WITH (format 'test_format', delimiter ',');
COPY copy_data TO stdout WITH (format 'test_format', quote '"');
COPY copy_data TO stdout WITH (format 'test_format', freeze true);     -- ERROR: FREEZE with COPY TO
