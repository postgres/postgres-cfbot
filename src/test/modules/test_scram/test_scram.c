/*--------------------------------------------------------------------------
 *
 * test_scram.c
 *		Test harness for SCRAM helpers in src/common/scram-common.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_scram/test_scram.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/scram-common.h"
#include "fmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/*
 * Wrapper around scram_parse_iterations() for direct testing.
 *
 * Returns 'invalid' when the parser rejects the input, otherwise the
 * parsed integer value as text.  Returning text rather than a record
 * keeps the regression output one column wide and easy to read.
 */
PG_FUNCTION_INFO_V1(test_scram_parse_iterations);
Datum
test_scram_parse_iterations(PG_FUNCTION_ARGS)
{
	text	   *input = PG_GETARG_TEXT_PP(0);
	char	   *cstr = text_to_cstring(input);
	int			iter = -1;
	char		buf[32];

	if (scram_parse_iterations(cstr, &iter))
		snprintf(buf, sizeof(buf), "%d", iter);
	else
		snprintf(buf, sizeof(buf), "invalid");

	pfree(cstr);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}
