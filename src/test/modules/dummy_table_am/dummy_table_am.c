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
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/* Parse table for build_reloptions: 8 inherited standard options + 4 of our own */
static relopt_parse_elt dt_relopt_tab[12];

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
 * This AM sets TableAmRoutine.has_std_options_prefix (see dthandler()
 * below), which promises core code that rd_options begins with a complete,
 * valid StdRdOptions it may read directly -- RelationGetFillFactor(), the
 * autovacuum option readers, and so on.  "std" is that StdRdOptions, and
 * must be the first member.
 *
 * The promise only holds if every field of "std" carries a sensible value
 * even when the user set nothing.  build_reloptions() fills exactly the
 * fields listed in the parse table (with the option's default when unset)
 * and leaves the rest zeroed -- and zero is the wrong "unset" value for
 * several of them (parallel_workers and
 * vacuum_max_eager_freeze_failure_rate both use -1; fillfactor's default
 * is HEAP_DEFAULT_FILLFACTOR).  That is why create_reloptions_table()
 * inherits and registers every option heap's default_reloptions()
 * understands, not just the ones this module is interesting for.
 *
 * The remaining four are AM-specific options that only dummy_table_am
 * knows about.
 */
typedef struct DummyTableOptions
{
	StdRdOptions std;			/* must be first, see above */
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
	 * Accept every standard option that core's default_reloptions()
	 * understands (registered for RELOPT_KIND_HEAP and/or RELOPT_KIND_TOAST)
	 * under our own kind.  This is the canonical use of
	 * add_reloption_to_kind(): an AM that wants to honour existing
	 * core-registered options without duplicating their definitions.  All of
	 * them, not just the interesting ones, must be both inherited and listed
	 * in the parse table, or the corresponding DummyTableOptions.std fields
	 * would stay zeroed rather than get their defaults -- and core code reads
	 * those fields directly because of has_std_options_prefix (see the
	 * comment on DummyTableOptions).
	 */
	add_reloption_to_kind("fillfactor", dt_relopt_kind);
	dt_relopt_tab[i].optname = "fillfactor";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_INT;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.fillfactor);
	i++;

	add_reloption_to_kind("toast_tuple_target", dt_relopt_kind);
	dt_relopt_tab[i].optname = "toast_tuple_target";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_INT;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.toast_tuple_target);
	i++;

	add_reloption_to_kind("parallel_workers", dt_relopt_kind);
	dt_relopt_tab[i].optname = "parallel_workers";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_INT;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.parallel_workers);
	i++;

	add_reloption_to_kind("vacuum_index_cleanup", dt_relopt_kind);
	dt_relopt_tab[i].optname = "vacuum_index_cleanup";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_ENUM;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.vacuum_index_cleanup);
	i++;

	add_reloption_to_kind("vacuum_truncate", dt_relopt_kind);
	dt_relopt_tab[i].optname = "vacuum_truncate";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_TERNARY;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.vacuum_truncate);
	i++;

	add_reloption_to_kind("vacuum_max_eager_freeze_failure_rate", dt_relopt_kind);
	dt_relopt_tab[i].optname = "vacuum_max_eager_freeze_failure_rate";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_REAL;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.vacuum_max_eager_freeze_failure_rate);
	i++;

	add_reloption_to_kind("autovacuum_enabled", dt_relopt_kind);
	dt_relopt_tab[i].optname = "autovacuum_enabled";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_TERNARY;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.autovacuum.enabled);
	i++;

	add_reloption_to_kind("user_catalog_table", dt_relopt_kind);
	dt_relopt_tab[i].optname = "user_catalog_table";
	dt_relopt_tab[i].opttype = RELOPT_TYPE_BOOL;
	dt_relopt_tab[i].offset = offsetof(DummyTableOptions, std.user_catalog_table);
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
 * heapam_relation_toast_am() (heap's own relation_toast_am callback, which
 * we would otherwise inherit unchanged along with the rest of heap's
 * routine) returns rel->rd_rel->relam -- correct for a real heap table, but
 * for dummy_table_am that's dummy_table_am's own oid, not heap's.  That
 * would make this AM's TOAST tables dummy_table_am relations too, and
 * building their chunk_id/chunk_seq index fails as soon as it's scanned,
 * since that scan goes through heap_getnext() directly.  Override it to
 * return the literal heap AM oid: this AM's TOAST tables are always plain
 * heap, regardless of what created the owning table.
 */
static Oid
dummy_table_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

/*
 * Handler for table AM.
 *
 * All storage-side callbacks are inherited from heap; we swap in our own
 * amoptions so that the AM owns its reloption set, and our own
 * relation_toast_am (see dummy_table_relation_toast_am() above).  This
 * keeps the example focused on the new API without duplicating the heap
 * AM.
 *
 * has_std_options_prefix is set because DummyTableOptions embeds a full
 * StdRdOptions as its first member with every field populated (see the
 * comment on DummyTableOptions): that makes it safe for core code to keep
 * reading fillfactor and friends directly out of rd_options, exactly as
 * it would for a plain heap table.
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

void
_PG_init(void)
{
	create_reloptions_table();
}
