/*-------------------------------------------------------------------------
 *
 * dummy_table_am.c
 *		Table AMs for testing the amoptions callback.
 *
 * This module provides two table access methods that use heap's callbacks
 * for everything except reloption parsing.  dummy_table_am's options struct
 * begins with a StdRdOptions; dummy_custom_table_am's does not.
 *
 * Indexes cannot be built on tables of either AM, since that uses
 * heap_getnext(), which accepts only heap's own TableAmRoutine.
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
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/* Kind of relation options for dummy_table_am */
static relopt_kind dt_relopt_kind;

/* Parse table and kind of relation options for dummy_custom_table_am */
static relopt_parse_elt dct_relopt_tab[3];
static relopt_kind dct_relopt_kind;

typedef enum DummyTableEnum
{
	DUMMY_TABLE_ENUM_ONE,
	DUMMY_TABLE_ENUM_TWO,
}			DummyTableEnum;

/*
 * dummy_table_am options.  Since dummy_table_am sets has_std_options_prefix,
 * "std" must be first, and each of its fields must be in the parse table so
 * that it gets its default when not set.
 */
typedef struct DummyTableOptions
{
	StdRdOptions std;			/* must be first, see above */
	int			option_int;
	double		option_real;
	bool		option_bool;
	DummyTableEnum option_enum;
}			DummyTableOptions;

/* The standard options, all of which dummy_table_am accepts */
static const relopt_parse_elt dt_std_options[] = {
	{"fillfactor", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.fillfactor)},
	{"toast_tuple_target", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.toast_tuple_target)},
	{"toast_value_type", RELOPT_TYPE_ENUM,
	offsetof(DummyTableOptions, std.toast_value_type)},
	{"user_catalog_table", RELOPT_TYPE_BOOL,
	offsetof(DummyTableOptions, std.user_catalog_table)},
	{"parallel_workers", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.parallel_workers)},
	{"vacuum_index_cleanup", RELOPT_TYPE_ENUM,
	offsetof(DummyTableOptions, std.vacuum_index_cleanup)},
	{"vacuum_truncate", RELOPT_TYPE_TERNARY,
	offsetof(DummyTableOptions, std.vacuum_truncate)},
	{"vacuum_max_eager_freeze_failure_rate", RELOPT_TYPE_REAL,
	offsetof(DummyTableOptions, std.vacuum_max_eager_freeze_failure_rate)},
	{"autovacuum_enabled", RELOPT_TYPE_TERNARY,
	offsetof(DummyTableOptions, std.autovacuum.enabled)},
	{"autovacuum_parallel_workers", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.autovacuum_parallel_workers)},
	{"autovacuum_vacuum_threshold", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.vacuum_threshold)},
	{"autovacuum_vacuum_max_threshold", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.vacuum_max_threshold)},
	{"autovacuum_vacuum_insert_threshold", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.vacuum_ins_threshold)},
	{"autovacuum_analyze_threshold", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.analyze_threshold)},
	{"autovacuum_vacuum_cost_limit", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.vacuum_cost_limit)},
	{"autovacuum_freeze_min_age", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.freeze_min_age)},
	{"autovacuum_freeze_max_age", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.freeze_max_age)},
	{"autovacuum_freeze_table_age", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.freeze_table_age)},
	{"autovacuum_multixact_freeze_min_age", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.multixact_freeze_min_age)},
	{"autovacuum_multixact_freeze_max_age", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.multixact_freeze_max_age)},
	{"autovacuum_multixact_freeze_table_age", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.multixact_freeze_table_age)},
	{"log_autovacuum_min_duration", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.log_vacuum_min_duration)},
	{"log_autoanalyze_min_duration", RELOPT_TYPE_INT,
	offsetof(DummyTableOptions, std.autovacuum.log_analyze_min_duration)},
	{"autovacuum_vacuum_cost_delay", RELOPT_TYPE_REAL,
	offsetof(DummyTableOptions, std.autovacuum.vacuum_cost_delay)},
	{"autovacuum_vacuum_scale_factor", RELOPT_TYPE_REAL,
	offsetof(DummyTableOptions, std.autovacuum.vacuum_scale_factor)},
	{"autovacuum_vacuum_insert_scale_factor", RELOPT_TYPE_REAL,
	offsetof(DummyTableOptions, std.autovacuum.vacuum_ins_scale_factor)},
	{"autovacuum_analyze_scale_factor", RELOPT_TYPE_REAL,
	offsetof(DummyTableOptions, std.autovacuum.analyze_scale_factor)},
};

/* Parse table for dummy_table_am: the standard options and 4 of its own */
static relopt_parse_elt dt_relopt_tab[lengthof(dt_std_options) + 4];

/*
 * dummy_custom_table_am options.  This is smaller than StdRdOptions, and its
 * fields are at the offsets of StdRdOptions' fillfactor, toast_tuple_target
 * and toast_value_type, so that core code reading a StdRdOptions field
 * without checking RelationHasStdRdOptions() gets a visibly wrong value.
 */
typedef struct DummyCustomTableOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			option_a;
	int			option_b;
	int			option_c;
}			DummyCustomTableOptions;

static relopt_enum_elt_def dummyTableEnumValues[] =
{
	{"one", DUMMY_TABLE_ENUM_ONE},
	{"two", DUMMY_TABLE_ENUM_TWO},
	{(const char *) NULL}		/* list terminator */
};

static void create_reloptions_table(void);
static void create_custom_reloptions_table(void);
static bytea *dtoptions(Datum reloptions, bool validate);
static bytea *dctoptions(Datum reloptions, bool validate);
static Oid	dummy_table_relation_toast_am(Relation rel);

PG_FUNCTION_INFO_V1(dthandler);
PG_FUNCTION_INFO_V1(dcthandler);

/*
 * Register dummy_table_am's options and populate its parse table.
 */
static void
create_reloptions_table(void)
{
	int			i = 0;

	dt_relopt_kind = add_reloption_kind();

	for (int j = 0; j < lengthof(dt_std_options); j++)
	{
		add_reloption_to_kind(dt_std_options[j].optname, dt_relopt_kind);
		dt_relopt_tab[i++] = dt_std_options[j];
	}

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
 * Register dummy_custom_table_am's options and populate its parse table.
 */
static void
create_custom_reloptions_table(void)
{
	static const char *const names[] = {"option_a", "option_b", "option_c"};
	static const int offsets[] = {
		offsetof(DummyCustomTableOptions, option_a),
		offsetof(DummyCustomTableOptions, option_b),
		offsetof(DummyCustomTableOptions, option_c),
	};

	StaticAssertDecl(lengthof(names) == lengthof(dct_relopt_tab),
					 "names and dct_relopt_tab must have the same length");
	StaticAssertDecl(lengthof(offsets) == lengthof(dct_relopt_tab),
					 "offsets and dct_relopt_tab must have the same length");

	dct_relopt_kind = add_reloption_kind();

	for (int i = 0; i < lengthof(dct_relopt_tab); i++)
	{
		add_int_reloption(dct_relopt_kind, names[i],
						  "Integer option for dummy_custom_table_am",
						  0, 0, 100, AccessExclusiveLock);
		dct_relopt_tab[i].optname = names[i];
		dct_relopt_tab[i].opttype = RELOPT_TYPE_INT;
		dct_relopt_tab[i].offset = offsets[i];
	}
}

/*
 * Parse reloptions for dummy_table_am.
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
 * Parse reloptions for dummy_custom_table_am.
 */
static bytea *
dctoptions(Datum reloptions, bool validate)
{
	return (bytea *) build_reloptions(reloptions, validate,
									  dct_relopt_kind,
									  sizeof(DummyCustomTableOptions),
									  dct_relopt_tab, lengthof(dct_relopt_tab));
}

/*
 * Make TOAST tables plain heap tables.  Heap's own callback returns the
 * relation's AM, and building the TOAST table's index would then fail in
 * heap_getnext().
 */
static Oid
dummy_table_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

/*
 * Handler for dummy_table_am.
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
		routine.has_std_options_prefix = true;
		routine.relation_toast_am = dummy_table_relation_toast_am;
		initialized = true;
	}

	PG_RETURN_POINTER(&routine);
}

/*
 * Handler for dummy_custom_table_am.
 */
Datum
dcthandler(PG_FUNCTION_ARGS)
{
	static TableAmRoutine routine;
	static bool initialized = false;

	if (!initialized)
	{
		memcpy(&routine, GetHeapamTableAmRoutine(), sizeof(routine));
		routine.amoptions = dctoptions;
		routine.has_std_options_prefix = false;
		routine.relation_toast_am = dummy_table_relation_toast_am;
		initialized = true;
	}

	PG_RETURN_POINTER(&routine);
}

void
_PG_init(void)
{
	create_reloptions_table();
	create_custom_reloptions_table();
}
