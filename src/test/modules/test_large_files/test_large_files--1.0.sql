CREATE FUNCTION test_large_files_offset_size()
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION test_large_files_test_4gb_boundary()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
