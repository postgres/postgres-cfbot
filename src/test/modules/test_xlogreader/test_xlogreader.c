/*-------------------------------------------------------------------------
 *
 * test_xlogreader.c
 *		Frontend harness for DecodeXLogRecord() hole-geometry checks
 *
 * Builds in-memory WAL records with full-page images and feeds them to
 * DecodeXLogRecord().  Used to check that malformed hole geometry is
 * rejected during decode, before RestoreBlockImage() can touch the page
 * buffer.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_xlogreader/test_xlogreader.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * We have to use postgres.h not postgres_fe.h here, because there's so much
 * backend-only stuff in the XLOG include files we need.  But we need a
 * frontend-ish environment otherwise.  Hence this ugly hack.
 */
#define FRONTEND 1
#include "postgres.h"

#include <stdio.h>
#include <string.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "common/relpath.h"
#include "port/pg_crc32c.h"

static void
append_bytes(char **dst, const void *src, Size len)
{
	memcpy(*dst, src, len);
	*dst += len;
}

/*
 * Build a one-block WAL record containing a full-page image with a hole.
 *
 * If compressed is true, hole_length is stored on the wire (as in a
 * corrupt or hand-built record).  Otherwise the decoder derives hole_length as
 * BLCKSZ - bimg_len.
 *
 * A valid CRC is filled in, matching ValidXLogRecord(), even though
 * DecodeXLogRecord() itself does not verify the CRC.
 */
static XLogRecord *
build_hole_image_record(char *buf, Size buflen,
						uint16 hole_offset, uint16 hole_length,
						uint16 bimg_len, bool compressed)
{
	XLogRecord *record;
	char	   *p;
	uint8		id;
	uint8		fork_flags;
	uint8		bimg_info;
	uint16		data_len;
	RelFileLocator rlocator;
	BlockNumber blkno;
	pg_crc32c	crc;
	char		image[BLCKSZ];

	if (bimg_len > BLCKSZ ||
		buflen < SizeOfXLogRecord + MaxSizeOfXLogRecordBlockHeader + bimg_len)
	{
		fprintf(stderr, "internal error: cannot build WAL image record\n");
		exit(1);
	}

	memset(buf, 0, buflen);
	memset(image, 0xA5, sizeof(image));

	record = (XLogRecord *) buf;
	p = buf + SizeOfXLogRecord;

	id = 0;
	fork_flags = MAIN_FORKNUM | BKPBLOCK_HAS_IMAGE;
	data_len = 0;
	append_bytes(&p, &id, sizeof(id));
	append_bytes(&p, &fork_flags, sizeof(fork_flags));
	append_bytes(&p, &data_len, sizeof(data_len));

	bimg_info = BKPIMAGE_HAS_HOLE | BKPIMAGE_APPLY;
	if (compressed)
		bimg_info |= BKPIMAGE_COMPRESS_PGLZ;

	append_bytes(&p, &bimg_len, sizeof(bimg_len));
	append_bytes(&p, &hole_offset, sizeof(hole_offset));
	append_bytes(&p, &bimg_info, sizeof(bimg_info));
	if (compressed)
		append_bytes(&p, &hole_length, sizeof(hole_length));

	rlocator.spcOid = 1663;
	rlocator.dbOid = 5;
	rlocator.relNumber = 16384;
	blkno = 0;
	append_bytes(&p, &rlocator, sizeof(rlocator));
	append_bytes(&p, &blkno, sizeof(blkno));
	append_bytes(&p, image, bimg_len);

	record->xl_tot_len = p - buf;
	record->xl_xid = InvalidTransactionId;
	record->xl_prev = InvalidXLogRecPtr;
	record->xl_info = 0;
	record->xl_rmid = RM_XLOG_ID;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf + SizeOfXLogRecord,
				record->xl_tot_len - SizeOfXLogRecord);
	COMP_CRC32C(crc, buf, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);
	record->xl_crc = crc;

	return record;
}

static bool
try_decode(XLogReaderState *state, XLogRecord *record, char **errormsg)
{
	DecodedXLogRecord *decoded;
	bool		ok;

	decoded = (DecodedXLogRecord *)
		palloc(DecodeXLogRecordRequiredSpace(record->xl_tot_len));
	decoded->oversized = true;
	state->ReadRecPtr = 0x28;

	ok = DecodeXLogRecord(state, decoded, record, state->ReadRecPtr, errormsg);
	pfree(decoded);
	return ok;
}

static int
run_case(XLogReaderState *state, const char *name,
		 uint16 hole_offset, uint16 hole_length, uint16 bimg_len,
		 bool compressed, bool expect_ok)
{
	char		buf[BLCKSZ + 512];
	XLogRecord *record;
	char	   *errormsg = NULL;
	bool		ok;

	record = build_hole_image_record(buf, sizeof(buf),
									 hole_offset, hole_length,
									 bimg_len, compressed);
	ok = try_decode(state, record, &errormsg);

	if (ok != expect_ok)
	{
		fprintf(stderr,
				"FAIL: %s: hole_offset=%u hole_length=%u bimg_len=%u compressed=%d: expected %s, got %s%s%s\n",
				name,
				(unsigned int) hole_offset,
				(unsigned int) hole_length,
				(unsigned int) bimg_len,
				(int) compressed,
				expect_ok ? "accept" : "reject",
				ok ? "accept" : "reject",
				errormsg ? ": " : "",
				errormsg ? errormsg : "");
		return 1;
	}

	if (!expect_ok &&
		(errormsg == NULL || strstr(errormsg, "BKPIMAGE_HAS_HOLE set") == NULL))
	{
		fprintf(stderr,
				"FAIL: %s: rejected, but error did not mention hole geometry: %s\n",
				name,
				errormsg ? errormsg : "(null)");
		return 1;
	}

	printf("ok %s\n", name);
	return 0;
}

int
main(int argc, char *argv[])
{
	XLogReaderState *state;
	int			failed = 0;
	uint16		near_end;

	(void) argc;
	(void) argv;

	state = XLogReaderAllocate(DEFAULT_XLOG_SEG_SIZE, NULL,
							   XL_ROUTINE(.page_read = NULL,
										  .segment_open = NULL,
										  .segment_close = NULL),
							   NULL);
	if (state == NULL)
	{
		fprintf(stderr, "out of memory while allocating XLogReader\n");
		return 1;
	}

	/* Use values near the end of the page, scaled to the build's BLCKSZ. */
	near_end = (uint16) (BLCKSZ - 192);

	/* Valid compressed hole entirely inside the page. */
	failed += run_case(state, "valid compressed hole",
					   100, 100, 16, true, true);

	/* Hole ends exactly at BLCKSZ. */
	failed += run_case(state, "valid compressed hole to end of page",
					   near_end, 192, 16, true, true);

	/* Both fields large, sum past BLCKSZ. */
	failed += run_case(state, "oversized compressed hole",
					   near_end, near_end, 16, true, false);

	/* Just one byte past the end of the page. */
	failed += run_case(state, "compressed hole one byte past page",
					   near_end, 193, 16, true, false);

	/* hole_offset itself past the page. */
	if (BLCKSZ < PG_UINT16_MAX)
		failed += run_case(state, "hole_offset past page",
						   (uint16) (BLCKSZ + 1), 1, 16, true, false);

	/*
	 * Uncompressed images store hole_offset on the wire and derive
	 * hole_length as BLCKSZ - bimg_len.  A large hole_offset still has to
	 * fit in the remaining image bytes.
	 */
	failed += run_case(state, "valid uncompressed hole",
					   24, 0, 100, false, true);
	failed += run_case(state, "uncompressed hole_offset past image",
					   near_end, 0, 16, false, false);

	XLogReaderFree(state);

	if (failed)
	{
		fprintf(stderr, "%d test(s) failed\n", failed);
		return 1;
	}

	printf("All tests passed\n");
	return 0;
}
