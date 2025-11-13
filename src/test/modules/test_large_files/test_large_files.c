/*-------------------------------------------------------------------------
 *
 * test_large_files.c
 *		Test module for large file I/O operations
 *
 * This module tests PostgreSQL's ability to handle file offsets larger
 * than 2GB (2^31 bytes), validating that pgoff_t is correctly used
 * throughout the file I/O layer.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_large_files_offset_size);
Datum
test_large_files_offset_size(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(sizeof(pgoff_t));
}

PG_FUNCTION_INFO_V1(test_large_files_test_4gb_boundary);
Datum
test_large_files_test_4gb_boundary(PG_FUNCTION_ARGS)
{
	File		file;
	pgoff_t		large_offset = ((pgoff_t) 4294967296LL) + 1;
	pgoff_t		expected_size = large_offset + 8;
	pgoff_t		actual_size;
	char		write_buf_0[8] = "OFFSET_0";
	char		write_buf_large[8] = "TESTDATA";
	char		read_buf[8];
	int			nbytes;

	file = OpenTemporaryFile(false);
	if (file < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create temporary file")));

	nbytes = FileWrite(file, write_buf_0, 8, 0, WAIT_EVENT_DATA_FILE_WRITE);
	if (nbytes != 8)
	{
		FileClose(file);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write at offset 0")));
	}

	nbytes = FileWrite(file, write_buf_large, 8, large_offset,
					   WAIT_EVENT_DATA_FILE_WRITE);
	if (nbytes != 8)
	{
		FileClose(file);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write at large offset")));
	}

	actual_size = FileSize(file);
	if (actual_size < expected_size)
	{
		FileClose(file);
		ereport(ERROR,
				(errmsg("file size is %lld bytes, expected at least %lld bytes - offset truncated",
						(long long) actual_size,
						(long long) expected_size)));
	}

	memset(read_buf, 0, 8);
	nbytes = FileRead(file, read_buf, 8, 0, WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != 8)
	{
		FileClose(file);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from offset 0")));
	}

	if (memcmp(read_buf, write_buf_0, 8) != 0)
	{
		FileClose(file);
		ereport(ERROR,
				(errmsg("data at offset 0 was corrupted - write wrapped around")));
	}

	memset(read_buf, 0, 8);
	nbytes = FileRead(file, read_buf, 8, large_offset,
					  WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != 8)
	{
		FileClose(file);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read at large offset")));
	}

	if (memcmp(write_buf_large, read_buf, 8) != 0)
	{
		FileClose(file);
		ereport(ERROR,
				(errmsg("data mismatch at large offset")));
	}

	FileClose(file);

	PG_RETURN_TEXT_P(cstring_to_text("4GB boundary test passed"));
}
