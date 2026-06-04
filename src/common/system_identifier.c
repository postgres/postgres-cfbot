/*-------------------------------------------------------------------------
 *
 * system_identifier.c
 *		Implementation of system identifier generation
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/common/system_identifier.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <sys/time.h>
#include <unistd.h>

#include "common/system_identifier.h"

/*
 * GenerateSystemIdentifier
 *
 * Creates a reasonably unique 64-bit identifier using:
 *  - current time (seconds + microseconds)
 *  - process ID
 */
uint64
GenerateSystemIdentifier(void)
{
	struct	timeval tv;
	uint64	sysidentifier;

	/*
	 * Select a hopefully-unique system identifier code for this installation.
	 * We use the result of gettimeofday(), including the fractional seconds
	 * field, as being about as unique as we can easily get.  (Think not to
	 * use random(), since it hasn't been seeded and there's no portable way
	 * to seed it other than the system clock value...)  The upper half of the
	 * uint64 value is just the tv_sec part, while the lower half contains the
	 * tv_usec part (which must fit in 20 bits), plus 12 bits from our current
	 * PID for a little extra uniqueness.  A person knowing this encoding can
	 * determine the initialization time of the installation, which could
	 * perhaps be useful sometimes.
	 */
	gettimeofday(&tv, NULL);
	sysidentifier = ((uint64) tv.tv_sec) << 32;
	sysidentifier |= ((uint64) tv.tv_usec) << 12;
	sysidentifier |= getpid() & 0xFFF;

	return sysidentifier;
}