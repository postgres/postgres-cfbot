/*--------------------------------------------------------------------------
 *
 * test_tablespace_options.c
 *		Test module defining tablespace options.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_tablespace_options/test_tablespace_options.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "fmgr.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* name of an extra, invalid option to register, for testing the checks */
static char *bad_option_name = NULL;

static relopt_enum_elt_def mode_values[] =
{
	{"fast", 0},
	{"safe", 1},
	{NULL, 0}
};

void
_PG_init(void)
{
	DefineCustomStringVariable("test_tablespace_options.bad_option_name",
							   "Name of an invalid tablespace option to register.",
							   NULL,
							   &bad_option_name,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("test_tablespace_options");

	add_int_reloption(RELOPT_KIND_TABLESPACE,
					  "test_tablespace_options.level",
					  "Integer tablespace option for testing",
					  1, 0, 10, AccessExclusiveLock);
	add_string_reloption(RELOPT_KIND_TABLESPACE,
						 "test_tablespace_options.label",
						 "String tablespace option for testing",
						 NULL, NULL, AccessExclusiveLock);
	add_enum_reloption(RELOPT_KIND_TABLESPACE,
					   "test_tablespace_options.mode",
					   "Enum tablespace option for testing",
					   mode_values, 0,
					   "Valid values are \"fast\" and \"safe\".",
					   AccessExclusiveLock);
	add_bool_reloption(RELOPT_KIND_TABLESPACE,
					   "test_tablespace_options.flag",
					   NULL, false, AccessExclusiveLock);

	if (bad_option_name[0] != '\0')
		add_bool_reloption(RELOPT_KIND_TABLESPACE, bad_option_name,
						   NULL, false, AccessExclusiveLock);
}
