/*-------------------------------------------------------------------------
 *
 * gzip.c
 *
 * Shared frontend routines for gzip compression and decompression.
 * These routines use zlib's low-level streaming API, without relying on
 * its gzip file I/O functions such as gzread() and gzclose().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/fe_utils/gzip.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <limits.h>

#include "fe_utils/gzip.h"

#ifdef HAVE_LIBZ
bool
gzip_decompressor_init(GzipDecompressor *decoder)
{
	int			res;

	memset(decoder, 0, sizeof(GzipDecompressor));
	decoder->state = GZIP_STREAM_NEW;

	/*
	 * Data compression was initialized using deflateInit2 to request a gzip
	 * header. Similarly, we are using inflateInit2 to initialize data
	 * decompression.
	 *
	 * Per the documentation for inflateInit2, the second argument is
	 * "windowBits" and its value must be greater than or equal to the value
	 * provided while compressing the data, so we are using the maximum
	 * possible value for safety.
	 */
	res = inflateInit2(&decoder->stream, 15 + 16);
	if (res != Z_OK)
	{
		decoder->error = decoder->stream.msg ?
			decoder->stream.msg : zError(res);
		return false;
	}
	return true;
}

/*
 * Consume input and produce at most output_size bytes.  MORE means that the
 * caller must provide more input or output space; it is not an error.  DONE
 * is returned only when eof is true and input ends at a member boundary.
 * Plain input, trailing non-member bytes, and physically empty streams are
 * rejected, while valid empty members are accepted.
 *
 * On return, *consumed is the number of bytes read from input, and *produced
 * is the number of decompressed bytes written to output.  The caller must
 * advance the input pointer by *consumed and reduce input_size accordingly
 * before passing the remaining input to the next call.
 */
GzipDecompressResult
gzip_decompressor_read(GzipDecompressor *decoder,
					   const void *input, size_t input_size,
					   void *output, size_t output_size,
					   bool eof, size_t *consumed, size_t *produced)
{
	z_stream   *zs = &decoder->stream;
	GzipDecompressResult result = GZIP_DECOMPRESS_MORE;

	/*
	 * zlib's input and output sizes use uInt, which may be narrower than
	 * size_t. Limit each call to UINT_MAX bytes.
	 */
	uInt		zs_in_size = Min(input_size, UINT_MAX);
	uInt		zs_out_size = Min(output_size, UINT_MAX);

	Assert(input != NULL || input_size == 0);
	Assert(output_size > 0);
	*consumed = *produced = 0;
	zs->next_in = (const Bytef *) input;
	zs->avail_in = zs_in_size;
	zs->next_out = output;
	zs->avail_out = zs_out_size;

	for (;;)
	{
		int			res;
		uInt		old_in = zs->avail_in;
		uInt		old_out = zs->avail_out;

		if (zs->avail_in == 0 && decoder->state != GZIP_MEMBER_INCOMPLETE)
		{
			/*
			 * No input remains in the current zlib span. We can only be done
			 * if the caller indicated EOF and all supplied input was consumed
			 * at a member boundary.
			 */
			if (eof && input_size == zs_in_size)
			{
				/* At EOF, and no more data from the caller's input */
				if (decoder->state == GZIP_STREAM_NEW)
				{
					decoder->error = _("compressed stream is empty");
					result = GZIP_DECOMPRESS_ERROR;
				}
				else
					result = GZIP_DECOMPRESS_DONE;
			}
			break;
		}

		/* Check if we have no more output space */
		if (zs->avail_out == 0)
			break;

		/*
		 * This call decompresses data starting at zs->next_in and updates
		 * zs->next_in and zs->avail_in. It generates output data starting at
		 * zs->next_out and updates zs->next_out and zs->avail_out
		 * accordingly.
		 */
		res = inflate(zs, Z_NO_FLUSH);
		if (res == Z_STREAM_END)
		{
			decoder->state = GZIP_MEMBER_COMPLETE;
			if (inflateReset(zs) != Z_OK)
			{
				decoder->error = _("could not reset compression stream");
				result = GZIP_DECOMPRESS_ERROR;
				break;
			}
		}
		else if (res == Z_OK || res == Z_BUF_ERROR)
		{
			decoder->state = GZIP_MEMBER_INCOMPLETE;
			if (old_in == zs->avail_in && old_out == zs->avail_out)
			{
				/* No progress was made */
				if (zs->avail_in > 0)
				{
					/* Input data is still available. */
					decoder->error = _("compression library made no progress");
					result = GZIP_DECOMPRESS_ERROR;
				}
				else if (eof && input_size == zs_in_size)
				{
					/*
					 * At EOF, inflate made no further progress without
					 * completing a member.
					 */
					decoder->error = _("compressed stream is incomplete");
					result = GZIP_DECOMPRESS_ERROR;
				}
				break;
			}
		}
		else
		{
			/* An error occurred during decompression */
			decoder->error = zs->msg ? zs->msg : zError(res);
			result = GZIP_DECOMPRESS_ERROR;
			break;
		}
	}

	*consumed = zs_in_size - zs->avail_in;
	*produced = zs_out_size - zs->avail_out;
	zs->next_in = NULL;
	zs->next_out = NULL;
	return result;
}

bool
gzip_decompressor_end(GzipDecompressor *decoder)
{
	int			res;

	res = inflateEnd(&decoder->stream);
	if (res != Z_OK)
	{
		decoder->error = zError(res);
		return false;
	}
	return true;
}
#endif							/* HAVE_LIBZ */
