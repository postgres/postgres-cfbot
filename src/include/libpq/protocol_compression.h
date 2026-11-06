/*-------------------------------------------------------------------------
 *
 * protocol_compression.h
 *    Size limits for compressed frontend/backend protocol messages.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/libpq/protocol_compression.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROTOCOL_COMPRESSION_H
#define PROTOCOL_COMPRESSION_H

#ifdef USE_ZSTD
#include <zstd.h>
#endif

/* Limits of the negotiated profile, not buffer allocation targets. */
#define PQ_COMPRESSION_MAX_SEGMENT_SIZE (16 * 1024 * 1024)
#define PQ_COMPRESSION_WINDOW_LOG 16

#ifdef USE_ZSTD
/*
 * Bound a single segment, not the entire stream.  Each segment starts after
 * the preceding flush has completed and has no intermediate flushes.
 * Otherwise, repeated small blocks could exceed the single-pass bound.
 */
#define PQ_COMPRESSION_MAX_WRAPPER_SIZE \
	ZSTD_COMPRESSBOUND(PQ_COMPRESSION_MAX_SEGMENT_SIZE)
#endif

#endif							/* PROTOCOL_COMPRESSION_H */
