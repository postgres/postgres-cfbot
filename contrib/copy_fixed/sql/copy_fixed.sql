LOAD 'copy_fixed';

-- directory paths are passed to us in environment variables
\getenv abs_builddir PG_ABS_BUILDDIR

CREATE TABLE fixed_data (a int, b text, c numeric);
INSERT INTO fixed_data VALUES (1, 'foo', 1.5),
                              (22, 'barbar', -2.25),
                              (333, NULL, 0);

--
-- COPY TO
--
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7');
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', align 'right');
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', pad '.');

-- a pad character that can also occur in the data is accepted on output;
-- writing such a file for another system to read is what it is for
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', pad '0',
                                align 'right');
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', null 'NULL');

-- record_delimiter: 'none' produces one unbroken stream
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7',
                                record_delimiter 'none');

-- a column list picks the widths in the order the columns are written
COPY fixed_data (c, a) TO stdout WITH (format 'fixed', widths '7,5');

-- a value that does not fit is an error, never silently truncated
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,2,7');

-- a NULL marker that does not fit is a distinct error from an oversized
-- value, and blames the marker rather than the (empty) value
COPY (SELECT * FROM fixed_data WHERE b IS NULL)
    TO stdout WITH (format 'fixed', widths '5,2,7', null 'ABC');

-- a value containing a newline or carriage return would split the record
-- across physical lines, so it is rejected unless record_delimiter is
-- 'none', where there is no framing to break
COPY (SELECT E'a\nb'::text) TO stdout WITH (format 'fixed', widths '5');
COPY (SELECT E'a\nb'::text) TO stdout WITH (format 'fixed', widths '5',
                                            record_delimiter 'none');

--
-- COPY FROM
--
CREATE TABLE fixed_in (a int, b text, c numeric);

-- an unpadded field reads back as NULL, so the output has to tell a NULL
-- apart from an empty string
\pset null '(null)'

COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7');
1    foo     1.5    
22   barbar  -2.25  
333          0      
\.
SELECT * FROM fixed_in ORDER BY a;
TRUNCATE fixed_in;

-- right alignment strips the leading pad instead of the trailing one
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7', align 'right');
    1     foo    1.5
   22  barbar  -2.25
  333              0
\.
SELECT * FROM fixed_in ORDER BY a;
TRUNCATE fixed_in;

-- reading back a file padded with anything but a space is refused: the
-- padding could not be told from the data.  A space may still be given
-- explicitly.
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7', pad '0');
\.
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7', pad ' ');
1    foo     1.5    
\.
SELECT * FROM fixed_in;
TRUNCATE fixed_in;

-- a round trip through a file must give back what went in, whichever
-- record delimiter the file uses
\set filename :abs_builddir '/results/copy_fixed_lf.dat'
COPY fixed_data TO :'filename' WITH (format 'fixed', widths '5,8,7');
COPY fixed_in FROM :'filename' WITH (format 'fixed', widths '5,8,7');
SELECT * FROM fixed_in ORDER BY a;
TRUNCATE fixed_in;

\set filename :abs_builddir '/results/copy_fixed_none.dat'
COPY fixed_data TO :'filename' WITH (format 'fixed', widths '5,8,7',
                                     record_delimiter 'none');
COPY fixed_in FROM :'filename' WITH (format 'fixed', widths '5,8,7',
                                     record_delimiter 'none');
SELECT * FROM fixed_in ORDER BY a;
TRUNCATE fixed_in;

\set filename :abs_builddir '/results/copy_fixed_crlf.dat'
COPY fixed_data TO :'filename' WITH (format 'fixed', widths '5,8,7',
                                     record_delimiter 'crlf');
COPY fixed_in FROM :'filename' WITH (format 'fixed', widths '5,8,7',
                                     record_delimiter 'crlf');
SELECT * FROM fixed_in ORDER BY a;
TRUNCATE fixed_in;

-- reading a crlf file as lf must not silently misparse
COPY fixed_in FROM :'filename' WITH (format 'fixed', widths '5,8,7');
TRUNCATE fixed_in;

-- a record delimiter inside the record's own bytes means the record was
-- short, whether that is a record cut off early or an embedded newline
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7');
1    foo     1.5    
22   short
333          0      
\.
TRUNCATE fixed_in;

-- a final record that stops in the middle is reported as such
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7');
1    foo     1.5    
22   bar
\.
TRUNCATE fixed_in;

-- Bytes that are not valid in the server encoding must never be quoted back
-- to the user raw: they would go into the server log as they stand, and a
-- client using another encoding would be told about the invalid byte instead
-- of about the real error.  Write the file through a large object so that
-- this script itself stays pure ASCII and its expected output does not depend
-- on the encoding the regression database happens to use.  A null byte is
-- rejected by pg_verify_mbstr() in every server encoding, single-byte ones
-- included, so these records fail to validate wherever the test runs.
CREATE FUNCTION fixed_write_file(bytes bytea, path text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
	loid oid;
BEGIN
	loid := lo_from_bytea(0, bytes);
	PERFORM lo_export(loid, path);
	PERFORM lo_unlink(loid);
END;
$$;

\set filename :abs_builddir '/results/copy_fixed_badenc.dat'

-- a complete record: the null byte is refused, naming the column, and the
-- error context falls back to the line number alone
SELECT fixed_write_file('\x31202020206162ff0020202020312e3520202020'::bytea,
                        :'filename');
COPY fixed_in FROM :'filename' WITH (format 'fixed', widths '5,8,7');
TRUNCATE fixed_in;

-- a short record: the error is the one that really happened, not an encoding
-- complaint from trying to put the record into the message
SELECT fixed_write_file('\x31202020206162ff0020'::bytea, :'filename');
COPY fixed_in FROM :'filename' WITH (format 'fixed', widths '5,8,7');
TRUNCATE fixed_in;

-- an unpadded field is NULL by default; the marker is matched after the
-- padding has been stripped
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7', null 'NULL');
1    NULL    1.5    
\.
SELECT * FROM fixed_in;
TRUNCATE fixed_in;

-- DEFAULT marker
CREATE TABLE fixed_def (a int, b text DEFAULT 'defaulted', c numeric);
COPY fixed_def FROM stdin WITH (format 'fixed', widths '5,8,7', default 'D');
1    D       1.5    
\.
SELECT * FROM fixed_def;

-- a DEFAULT marker on a column that has no default is an error
COPY fixed_def (a, c) FROM stdin WITH (format 'fixed', widths '5,7', default 'D');
1    D      
\.

--
-- ON_ERROR, which core implements for the fields it converts
--
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7',
                               on_error 'ignore');
1    foo     1.5    
bad  bar     2.5    
3    baz     3.5    
\.
SELECT * FROM fixed_in ORDER BY a;
TRUNCATE fixed_in;

COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7',
                               on_error 'ignore', log_verbosity 'verbose');
1    foo     1.5    
bad  bar     2.5    
\.
TRUNCATE fixed_in;

COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7',
                               on_error 'ignore', reject_limit 1);
bad1 foo     1.5    
bad2 bar     2.5    
\.
TRUNCATE fixed_in;

COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7',
                               on_error 'set_null');
bad  foo     1.5    
\.
SELECT * FROM fixed_in;
TRUNCATE fixed_in;
\pset null ''

-- the error context must name the right line and column
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7');
1    foo     1.5    
bad  bar     2.5    
\.

--
-- Option errors
--
COPY fixed_data TO stdout WITH (format 'fixed');                    -- widths required
COPY fixed_data TO stdout WITH (format 'fixed', widths '');         -- ERROR
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,0,7');    -- ERROR: not positive
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,x,7');    -- ERROR: not a number
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,,7');     -- ERROR
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8', widths '1,2'); -- ERROR: redundant
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7',
                                pad '.', pad '#');                  -- ERROR: redundant
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7',
                                align 'left', align 'right');       -- ERROR: redundant
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7',
                                record_delimiter 'lf',
                                record_delimiter 'crlf');           -- ERROR: redundant
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8');      -- ERROR: 2 widths, 3 columns
COPY fixed_data TO stdout WITH (format 'fixed', widths '5');        -- ERROR: 1 width, 3 columns
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', pad '');     -- ERROR
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', pad 'ab');   -- ERROR
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', pad 'é');    -- ERROR: multi-byte
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7',
                                pad E'\n');                         -- ERROR: newline, with detail
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', align 'up'); -- ERROR
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7',
                                record_delimiter 'cr');             -- ERROR
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', bogus 1); -- ERROR

-- Core options this format does not implement
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', delimiter ',');
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', quote '"');
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', header true);
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', force_quote *);

-- A NULL marker must not contain a newline
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7',
                                null E'a\nb');

-- Nor a DEFAULT marker
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7', default E'a\nb');
\.

-- NULL and DEFAULT specifications must not collide.  Only COPY FROM can
-- reach the format's own check, because core rejects DEFAULT for any
-- COPY TO before the format is asked to validate anything.
COPY fixed_data TO stdout WITH (format 'fixed', widths '5,8,7', default '');
COPY fixed_in FROM stdin WITH (format 'fixed', widths '5,8,7', default '');
\.

DROP FUNCTION fixed_write_file(bytea, text);
DROP TABLE fixed_def;
DROP TABLE fixed_in;
DROP TABLE fixed_data;
