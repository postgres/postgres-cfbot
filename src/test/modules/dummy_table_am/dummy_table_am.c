/*-------------------------------------------------------------------------
 *
 * dummy_table_am.c
 *		Table AM template main file.
 *
 * This module exists primarily to demonstrate and exercise the table AM
 * amoptions callback and the add_reloption_to_kind() helper.  Storage
 * and scan callbacks are delegated to the heap AM, so a relation
 * created with USING dummy_table_am behaves like a heap table; only the
 * reloption surface differs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/dummy_table_am/dummy_table_am.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/tableam.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

/* Parse table for build_reloptions */
static relopt_parse_elt dt_relopt_tab[5];

/* Kind of relation options for dummy table */
static relopt_kind dt_relopt_kind;

typedef enum DummyTableEnum
{
	DUMMY_TABLE_ENUM_ONE,
	DUMMY_TABLE_ENUM_TWO,
}			DummyTableEnum;

/*
 * Dummy table options.
 *
 * The first two fields are the standard heap options (fillfactor +
 * autovacuum_enabled) that we inherit by calling add_reloption_to_kind()
 * on the matching names; the remaining ones are AM-specific options.
 */
typedef struct DummyTableOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			fillfactor;
	int			option_int;
	double		option_real;
	bool		option_bool;
	DummyTableEnum option_enum;
}			DummyTableOptions;

static relopt_enum_elt_def dummyTableEnumValues[] =
{
	{"one", DUMMY_TABLE_ENUM_ONE},
	{"two", DUMMY_TABLE_ENUM_TWO},
	{(const char *) NULL}		/* list terminator */
};

PG_FUNCTION_INFO_V1(dthandler);

/*
 * Register a relopt_kind for this AM and populate the parse table.
 */
static void
create_reloptions_table(void)
{
	int			i = 0;

	dt_relopt_kind = add_reloption_kind();

	/*
	 * Accept the standard "fillfactor" option (registered by core for
	 * RELOPT_KIND_HEAP only) under our own kind.  This is the canonical use
	 * of add_reloption_to_kind(): an AM that wants to honour an existing
	 * core-registered option without duplicating its definition.
	 */
	add_reloption_to_kind("fillfactor", dt_relopt_kind);
	dt_relopt_tab[i].optname = "fillfactor";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_INT;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, fillfactor);
	i++;

	add_int_reloption(dt_relopt_kind, "option_int",
					  "Integer option for dummy_table_am",
					  10, -10, 100, AccessExclusiveLock);
	dt_relopt_tab[i].optname = "option_int";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_INT;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, option_int);
	i++;

	add_real_reloption(dt_relopt_kind, "option_real",
					   "Real option for dummy_table_am",
					   3.1415, -10, 100, AccessExclusiveLock);
	dt_relopt_tab[i].optname = "option_real";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_REAL;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, option_real);
	i++;

	add_bool_reloption(dt_relopt_kind, "option_bool",
					   "Boolean option for dummy_table_am",
					   true, AccessExclusiveLock);
	dt_relopt_tab[i].optname = "option_bool";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_BOOL;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, option_bool);
	i++;

	add_enum_reloption(dt_relopt_kind, "option_enum",
					   "Enum option for dummy_table_am",
					   dummyTableEnumValues,
					   DUMMY_TABLE_ENUM_ONE,
					   "Valid values are \"one\" and \"two\".",
					   AccessExclusiveLock);
	dt_relopt_tab[i].optname = "option_enum";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_ENUM;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, option_enum);
	i++;
}

/*
 * Parse reloptions for dummy_table_am.
 *
 * Returning DummyTableOptions tells the caller (relcache.c) to store
 * exactly that layout in Relation->rd_options.
 */
static bytea *
dtoptions(Datum reloptions, bool validate)
{
	return (bytea *) build_reloptions(reloptions, validate,
									  dt_relopt_kind,
									  sizeof(DummyTableOptions),
									  dt_relopt_tab, lengthof(dt_relopt_tab));
}

/*
 * Handler for table AM.
 *
 * All storage-side callbacks are inherited from heap; we only swap in
 * our own amoptions so that the AM owns its reloption set.  This keeps
 * the example focused on the new API without duplicating the heap AM.
 */
Datum
dthandler(PG_FUNCTION_ARGS)
{
	static TableAmRoutine routine;
	static bool initialized = false;

	if (!initialized)
	{
		memcpy(&routine, GetHeapamTableAmRoutine(), sizeof(routine));
		routine.amoptions = dtoptions;
		initialized = true;
	}

	PG_RETURN_POINTER(&routine);
}

void
_PG_init(void)
{
	create_reloptions_table();
}
