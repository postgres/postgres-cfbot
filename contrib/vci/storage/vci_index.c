/*-------------------------------------------------------------------------
 *
 * vci_index.c
 *	  Index Access Method
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_index.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "access/relation.h"
#include "port.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "rewrite/rewriteSupport.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#include "vci.h"
#include "vci_columns.h"
#include "vci_columns_data.h"

#include "vci_mem.h"
#include "vci_ros.h"
#include "vci_ros_command.h"
#include "vci_ros_daemon.h"
#include "vci_supported_oid.h"
#include "vci_tidcrid.h"
#include "vci_wos.h"
#include "vci_xact.h"

#ifdef WIN32
#define	__func__	__FUNCTION__
#endif

#ifdef HAVE_DESIGNATED_INITIALIZERS
#define SFINIT(f, ...) f = __VA_ARGS__
#else
#define SFINIT(f, ...) __VA_ARGS__
#endif

/**
 * Data Relation
 */
#define VCI_RELTYPE_DATA     ('d')

/**
 * Meta Relation
 */
#define VCI_RELTYPE_META     ('m')

/**
 * WOS Relation
 */
#define VCI_RELTYPE_WOS      ('W')

/**
 * ROS Relation
 */
#define VCI_RELTYPE_ROS      ('R')

/**
 * TIDCRID Relation
 */
#define VCI_RELTYPE_TIDCRID  ('T')

/* local functions */
static IndexBuildResult *vci_inner_build(Relation, Relation, IndexInfo *);
static void vci_inner_buildempty(Relation indexRelation);
static bool vci_inner_insert(Relation, ItemPointer);
static bool vci_inner_insert_in_copy(Relation, ItemPointer);
static IndexBulkDeleteResult *vci_inner_vacuumcleanup(IndexVacuumInfo *, IndexBulkDeleteResult *);

IndexBuildResult *vci_build(Relation heap, Relation index, IndexInfo *indexInfo);
void		vci_buildempty(Relation index);
bool		vci_insert(Relation indexRel, Datum *values, bool *isnull,
					   ItemPointer heap_tid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
IndexBulkDeleteResult *vci_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
									  IndexBulkDeleteCallback callback, void *callback_state);
IndexBulkDeleteResult *vci_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
void		vci_costestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages);
int			vci_gettreeheight(Relation rel);
bytea	   *vci_options(Datum reloptions, bool validate);
IndexScanDesc vci_beginscan(Relation rel, int nkeys, int norderbys);
void		vci_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);
bool		vci_validate(Oid opclassoid);
void		vci_endscan(IndexScanDesc scan);
void		vci_markpos(IndexScanDesc scan);
void		vci_restrpos(IndexScanDesc scan);

static char relNameBuf[NAMEDATALEN];

static bool copy_with_freeze_option;

bool
vci_isVciAdditionalRelation(Relation rel)
{
	return vci_isVciAdditionalRelationTuple(rel->rd_id, rel->rd_rel);
}

bool
vci_isVciAdditionalRelationTuple(Oid reloid, Form_pg_class reltuple)
{
	if (reltuple->relkind == RELKIND_MATVIEW)
	{
		int			ret;
		int			dummy1;
		int			dummy2;
		char		dummy3;

		ret = sscanf(NameStr(reltuple->relname), VCI_INTERNAL_RELATION_TEMPLATE,
					 &dummy1, &dummy2, &dummy3);

		return (ret == 3);
	}

	return false;
}

/* custom index */

IndexBuildResult *
vci_build(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	vci_id_t	vciid;

	if (!fullPageWrites)
	{
		if (vci_rebuild_command == vcirc_invalid)
			/* CREATE INDEX */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("access method \"%s\" does not work under full_page_writes=off", VCI_STRING)));
		else
			/* TRUNCATE, VACUUM FULL */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("access method \"%s\" does not work under full_page_writes=off", VCI_STRING),
					 errhint("Use DROP INDEX \"%s\"", RelationGetRelationName(indexRel))));
	}

	result = vci_inner_build(heapRel, indexRel, indexInfo);

	vciid.oid = RelationGetRelid(indexRel);
	vciid.dbid = MyDatabaseId;

	vci_TouchMemoryEntry(&vciid,
						 get_rel_tablespace(indexRel->rd_id));

	return result;
}

void
vci_buildempty(Relation indexRel)
{
	vci_inner_buildempty(indexRel);

	return;
}

/* for COPY command */
#define EXTENT_LIST_UNIT_EXTENSION  (1024)

typedef struct CopyCommandInfo
{
	TransactionId xid;
	CommandId	cid;
	uint64		numAppendedRows;
	uint32	   *extentList;
	uint32		numFilledExtent;
	uint32		numAllocatedExtent;
} CopyCommandInfo;

static CopyCommandInfo copyInfo = {
	SFINIT(xid, InvalidTransactionId),
	SFINIT(cid, InvalidCommandId),
	SFINIT(numAppendedRows, 0),
	SFINIT(extentList, NULL),
	SFINIT(numFilledExtent, 0),
	SFINIT(numAllocatedExtent, 0)
};
static vci_RosCommandContext copyConvContext;

bool
vci_insert(Relation indexRel, Datum *values, bool *isnull,
		   ItemPointer heap_tid, Relation heapRel,
		   IndexUniqueCheck checkUnique,
		   bool indexUnchanged,
		   struct IndexInfo *indexInfo)
{
	bool		result;
	TransactionId xid = GetCurrentTransactionId();
	CommandId	cid = GetCurrentCommandId(false);

	Assert(TransactionIdIsValid(xid));
	Assert(InvalidCommandId != cid);

	if (!fullPageWrites)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not work under full_page_writes=off", VCI_STRING),
				 errhint("Use DROP INDEX \"%s\"", RelationGetRelationName(indexRel))));

	if (ItemPointerGetOffsetNumber(heap_tid) == FirstOffsetNumber)
	{
		vci_id_t	vciid;

		vciid.oid = RelationGetRelid(indexRel);
		vciid.dbid = MyDatabaseId;

		vci_TouchMemoryEntry(&vciid,
							 get_rel_tablespace(indexRel->rd_id));
	}

	if (TransactionIdEquals(xid, copyInfo.xid) && (cid == copyInfo.cid))
		result = vci_inner_insert_in_copy(indexRel, heap_tid);	/* LCOV_EXCL_LINE */
	else
		result = vci_inner_insert(indexRel, heap_tid);

	return result;
}

/**
 * vci_bulkdelete
 */
IndexBulkDeleteResult *
vci_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	return stats;
}

/**
 * vci_vacuumcleanup
 */
IndexBulkDeleteResult *
vci_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (info->analyze_only)
		return stats;

	vci_inner_vacuumcleanup(info, stats);

	return stats;
}

/**
 * vci_costestimate
 */
void
vci_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	/*
	 * PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0); IndexPath
	 * *path = (IndexPath *) PG_GETARG_POINTER(1); double		loop_count =
	 * PG_GETARG_FLOAT8(2);
	 */

	/* always return worst cost value */
	*indexStartupCost = DBL_MAX;
	*indexTotalCost = DBL_MAX;
	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;
	*indexPages = ((BlockNumber) 0xFFFFFFFE);	/* MaxBlockNumber */

	/**
	 * Disabled nodes are also a cost metric (see Commit e222534), so set a
	 * high value to ensure an Index Scan will not be chosen.
	 */
	path->path.disabled_nodes = INT_MAX;

	return;
}

int
vci_gettreeheight(Relation rel)
{
	int			result;

	result = 0;
	return result;
}

bytea *
vci_options(Datum reloptions, bool validate)
{
	return NULL;
}

bool
vci_validate(Oid opclassoid)
{
	/* pass */
	return true;
}

/* LCOV_EXCL_START */
IndexScanDesc
vci_beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc result;

	/*
	 * Relation indexRel = (Relation) PG_GETARG_POINTER(0); int		 nkeys =
	 * PG_GETARG_INT32(1); int		 norderbys = PG_GETARG_INT32(2);
	 */

	result = NULL;

	ereport(PANIC,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected index access method call : \"%s\"  ", __func__)));

	return result;
}

void
vci_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		   ScanKey orderbys, int norderbys)
{
	/*
	 * IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0); ScanKey keys
	 * = (ScanKey) PG_GETARG_POINTER(1); int		 nkeys = PG_GETARG_INT32(2);
	 * ScanKey orderbys = (ScanKey) PG_GETARG_POINTER(3); int		norderbys =
	 * PG_GETARG_INT32(4);
	 */

	/* pass */
	return;
}

void
vci_endscan(IndexScanDesc scan)
{
	/*
	 * IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	 */

	ereport(PANIC,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected index access method call : \"%s\"  ", __func__)));

	/* pass */
	return;
}

void
vci_markpos(IndexScanDesc scan)
{
	/*
	 * IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	 */
	ereport(PANIC,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected index access method call : \"%s\"  ", __func__)));

	/* pass */
	return;
}

void
vci_restrpos(IndexScanDesc scan)
{
	/*
	 * IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	 */
	ereport(PANIC,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected index access method call : \"%s\"  ", __func__)));

	/* pass */
	return;
}

/* LCOV_EXCL_STOP */

/* --body-- */

static Oid
vci_create_relation(const char *rel_identifier, Relation indexRel, IndexInfo *indexInfo, char vci_reltype)
{
	int			natts;

	/* system catalog relation id */
	Relation	pg_class;
	Relation	pg_attr;

	/* new rel, oid, tupdesc */
	Relation	new_rel;
	Oid			new_oid;
	TupleDesc	new_tupdesc;

	/* attributes */
	Oid			ownerid = GetUserId();

	char		relkind = RELKIND_MATVIEW;

	Oid			new_type_oid = InvalidOid;
	Oid			reloftypeid = InvalidOid;
	CatalogIndexState indstate;

	char		relname[NAMEDATALEN];	/* max 64 characters */
	Oid			reltablespace;
	Oid			relnamespace;
	char		relpersistence;
	Oid			accessmtd;

	/* variables for pg_class */
	Form_pg_class new_rel_reltup;

	RangeVar   *relrv;

	/* Insert pg_depend table */
	ObjectAddress oaIndex;
	ObjectAddress oaNewRel;

	relnamespace = indexRel->rd_rel->relnamespace;
	reltablespace = indexRel->rd_rel->reltablespace;
	relpersistence = indexRel->rd_rel->relpersistence;
	accessmtd = HEAP_TABLE_AM_OID;

	/* function start */
	memset(relname, 0, sizeof(relname));
	strncpy(relname, rel_identifier, sizeof(relname));

	relrv = makeRangeVar(get_namespace_name(relnamespace), relname, -1);
	new_oid = RangeVarGetRelid(relrv, AccessShareLock, true);

	if (OidIsValid(new_oid))
	{
		new_rel = relation_open(new_oid, AccessExclusiveLock);
		RelationSetNewRelfilenumber(new_rel, new_rel->rd_rel->relpersistence);

		/*
		 * if (new_rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
		 * heap_create_init_fork(new_rel);
		 */

		relation_close(new_rel, NoLock);	/* do not unlock till end of xact */

		return new_oid;
	}

	/* Generate Data WOS */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	/* 4.6.1 get new Oid for new relation */

	new_oid = GetNewRelFileNumber(reltablespace, pg_class, relpersistence);

	/* TODO */

	/*
	 * The following line is meaningful? Or shoud we remove it?
	 */
	get_user_default_acl(OBJECT_TABLE, ownerid, relnamespace);

	/* 4.6.1.2 create new relation cache entry */

	/* new tuple descriptor has TID column */

	switch (vci_reltype)
	{
			/* WOS */
		case VCI_RELTYPE_WOS:
			natts = 2;
			new_tupdesc = CreateTemplateTupleDesc(natts);	/* no Oid */
			TupleDescInitEntry(new_tupdesc, (AttrNumber) 1, "original_tid", TIDOID, -1, 0);
			TupleDescInitEntry(new_tupdesc, (AttrNumber) 2, "xid", INT8OID, -1, 0);
			break;

			/* ROS */
		case VCI_RELTYPE_ROS:
			natts = 1;
			new_tupdesc = CreateTemplateTupleDesc(natts);	/* no Oid */
			TupleDescInitEntry(new_tupdesc, (AttrNumber) 1, "bindata", BYTEAOID, -1, 0);	/* */
			break;

			/* TID-CRID  */
		case VCI_RELTYPE_TIDCRID:
			natts = 1;
			new_tupdesc = CreateTemplateTupleDesc(natts);	/* no Oid */
			TupleDescInitEntry(new_tupdesc, (AttrNumber) 1, "bindata", BYTEAOID, -1, 0);	/* */
			break;

			/* LCOV_EXCL_START */
		default:
			elog(ERROR, "unexpected vci_reltype");
			break;
			/* LCOV_EXCL_STOP */
	}

	/*
	 * Create the relcache entry (mostly dummy at this point) and the physical
	 * disk file.  (If we fail further down, it's the smgr's responsibility to
	 * remove the disk file again.)
	 */
	new_rel = RelationBuildLocalRelation(relname,
										 relnamespace,
										 new_tupdesc,
										 new_oid,
										 accessmtd,
										 new_oid,	/* relfilenumber */
										 reltablespace,
										 false, /* shared_relation */
										 false, /* mapped_relation */
										 relpersistence,
										 relkind);

	/* 4.6.1.3 create new starge for new relation */
	RelationCreateStorage(new_rel->rd_locator, relpersistence, true);

	Assert(new_oid == RelationGetRelid(new_rel));

	/* 4.6.1.4 add new entry into pg_class */
	new_rel_reltup = new_rel->rd_rel;
	new_rel_reltup->relpages = 0;
	new_rel_reltup->reltuples = -1;
	new_rel_reltup->relallvisible = 0;
	new_rel_reltup->relfrozenxid = RecentXmin;
	new_rel_reltup->relminmxid = GetOldestMultiXactId();
	new_rel_reltup->relowner = ownerid;
	new_rel_reltup->reltype = new_type_oid;
	new_rel_reltup->reloftype = reloftypeid;

	/*
	 * Flag the VCI internal relation MATVIEW as already populated.
	 *
	 * Users are not supposed to be querying these internal relations, but
	 * just in case they do, setting 'relispopulated' prevents an error saying
	 * the view has not been populated, hinting a "REFRESH MATERIALIZED VIEW"
	 * is needed. That hint only causes confusion, since the REFRESH is
	 * disallowed for VCI internal relations.
	 */
	new_rel_reltup->relispopulated = true;

	/*
	 * @see
	 * https://www.postgresql.jp/document/9.4/html/catalog-pg-rewrite.html
	 */
	new_rel_reltup->relhasrules = true;

	new_rel->rd_att->tdtypeid = new_type_oid;

	InsertPgClassTuple(pg_class, new_rel, new_oid, (Datum) 0, (Datum) 0);

	/*
	 * 4.6.1.5 -now add tuples to pg_attribute for the attributes in our new
	 * relation.
	 */

	/*
	 * open pg_attribute and its indexes.
	 */
	pg_attr = table_open(AttributeRelationId, RowExclusiveLock);
	indstate = CatalogOpenIndexes(pg_attr);

	/*
	 * First we add the user attributes.  This is also a convenient place to
	 * add dependencies on their datatypes and collations.
	 */
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute attrs;

		/* [TODO] Make sure these are OK? */
		new_tupdesc->compact_attrs[i].attcacheoff = -1;
		attrs = TupleDescAttr(new_tupdesc, i);
		attrs->attstorage = TYPSTORAGE_PLAIN;
		attrs->attcompression = InvalidCompressionMethod;
	}
	InsertPgAttributeTuples(pg_attr, new_tupdesc, new_oid, NULL, indstate);

	/*
	 * clean up pg_attribute
	 */
	CatalogCloseIndexes(indstate);
	table_close(pg_attr, RowExclusiveLock);

	/*
	 * VCI internal relations are dependent on the parent index.
	 */
	ObjectAddressSet(oaIndex, RelationRelationId, indexRel->rd_id);
	ObjectAddressSet(oaNewRel, RelationRelationId, new_oid);
	recordDependencyOn(&oaNewRel, &oaIndex, DEPENDENCY_INTERNAL);

	table_close(new_rel, NoLock);	/* do not unlock till end of xact */
	table_close(pg_class, RowExclusiveLock);

	return new_oid;
}

static char *
GenRelName(Relation rel, int16 columnId, char suffix)
{
	snprintf(relNameBuf, NAMEDATALEN, VCI_INTERNAL_RELATION_TEMPLATE, RelationGetRelid(rel),
			 (0xFFFF & columnId), suffix);

	return relNameBuf;
}

static void
CheckIndexedRelationKind(Relation rel)
{
	if (rel->rd_rel->relkind == RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support index on materialized view", VCI_STRING)));

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support index on temporary table", VCI_STRING)));
}

static void
CheckIndexInfo(IndexInfo *indexInfo, Relation indexRel)
{
	/* check Concurrent option first. */
	if (indexInfo->ii_Concurrent)
		/* LCOV_EXCL_START */
		elog(PANIC, "should not reach here");
	/* LCOV_EXCL_STOP */

	if (indexInfo->ii_Predicate != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support partial-index", VCI_STRING)));

	if (indexInfo->ii_Expressions != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support to CREATE INDEX on the expression", VCI_STRING)));

	if (indexInfo->ii_ExclusionOps != NULL ||
		indexInfo->ii_ExclusionProcs != NULL ||
		indexInfo->ii_ExclusionStrats != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" does not support EXCLUDE clause", VCI_STRING)));

	for (int i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		AttrNumber	an = indexInfo->ii_IndexAttrNumbers[i];

		for (int j = i + 1; j < indexInfo->ii_NumIndexAttrs; j++)
		{
			TupleDesc	tupdesc = RelationGetDescr(indexRel);

			if (an == indexInfo->ii_IndexAttrNumbers[j])
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("duplicated columns in vci index creation: %s",
								NameStr(TupleDescAttr(tupdesc, an - 1)->attname)),
						 errhint("duplicated columns are specified")));
		}
	}
}

static void
CheckIndexColumnTypes(TupleDesc tupdesc)
{
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Oid			typeoid = TupleDescAttr(tupdesc, i)->atttypid;

		if (!vci_is_supported_type(typeoid))
		{
			HeapTuple	tuple;
			Form_pg_type typetuple;

			tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeoid));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for type %u", typeoid);

			typetuple = (Form_pg_type) GETSTRUCT(tuple);

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("data type %s is not supported for access method \"%s\"",
							NameStr(typetuple->typname), VCI_STRING)));

			ReleaseSysCache(tuple);
		}
	}
}

static IndexBuildResult *
vci_inner_build(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	Oid			oid;

	vci_MainRelHeaderInfo *vmr_info;

	TupleDesc	tupdesc;

	uint32		offsetToExtentInfo;

	double		reltuples = -1;

	CheckIndexedRelationKind(heapRel);
	CheckIndexInfo(indexInfo, indexRel);
	CheckIndexColumnTypes(RelationGetDescr(indexRel));

	/* create VCI main relation */
	vmr_info = palloc0_object(vci_MainRelHeaderInfo);
	vci_InitMainRelHeaderInfo(vmr_info, indexRel, vci_rc_wos_ros_conv_build);

	if (RelationGetNumberOfBlocks(indexRel) != 0)
		elog(ERROR, "index \"%s\" already contains data", RelationGetRelationName(indexRel));

	/* create blank page * VCI_NUM_MAIN_REL_HEADER_PAGES */
	vci_PreparePagesWithOneItemIfNecessary(indexRel,
										   lengthof(vmr_info->buffer) - 1);

	vci_KeepMainRelHeaderWithoutVersionCheck(vmr_info);

	/* write ROS format version */
	vci_SetMainRelVar(vmr_info, vcimrv_ros_version_major, 0,
					  VCI_ROS_VERSION_MAJOR);
	vci_SetMainRelVar(vmr_info, vcimrv_ros_version_minor, 0,
					  VCI_ROS_VERSION_MINOR);

	/* create WOS relations */
	/* register WOS relation's OID to VCI Main relation */

	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_DATA_WOS, VCI_RELTYPE_DATA), indexRel, indexInfo, VCI_RELTYPE_WOS);
	vci_SetMainRelVar(vmr_info, vcimrv_data_wos_oid, 0, oid);

	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_WHITEOUT_WOS, VCI_RELTYPE_DATA), indexRel, indexInfo, VCI_RELTYPE_WOS);
	vci_SetMainRelVar(vmr_info, vcimrv_whiteout_wos_oid, 0, oid);

	/* create ROS relations */

	/* TID */
	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_TID, VCI_RELTYPE_DATA), indexRel, indexInfo, VCI_RELTYPE_ROS);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_data_oid, 0, oid);
	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_TID, VCI_RELTYPE_META), indexRel, indexInfo, VCI_RELTYPE_ROS);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_meta_oid, 0, oid);

	/* NUll */
	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_NULL, VCI_RELTYPE_DATA), indexRel, indexInfo, VCI_RELTYPE_ROS);
	vci_SetMainRelVar(vmr_info, vcimrv_null_data_oid, 0, oid);
	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_NULL, VCI_RELTYPE_META), indexRel, indexInfo, VCI_RELTYPE_ROS);
	vci_SetMainRelVar(vmr_info, vcimrv_null_meta_oid, 0, oid);

	/* Delete Vector */
	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_DELETE, VCI_RELTYPE_DATA), indexRel, indexInfo, VCI_RELTYPE_ROS);
	vci_SetMainRelVar(vmr_info, vcimrv_delete_data_oid, 0, oid);
	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_DELETE, VCI_RELTYPE_META), indexRel, indexInfo, VCI_RELTYPE_ROS);
	vci_SetMainRelVar(vmr_info, vcimrv_delete_meta_oid, 0, oid);

	/* Column Stores */
	tupdesc = RelationGetDescr(indexRel);
	CheckIndexColumnTypes(tupdesc);

	vci_SetMainRelVar(vmr_info, vcimrv_num_columns, 0, tupdesc->natts);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Oid			column_store_oid;
		Oid			column_meta_oid;
		vcis_m_column_t *columnPointer;

		column_store_oid = vci_create_relation(GenRelName(indexRel, i, VCI_RELTYPE_DATA), indexRel, indexInfo, VCI_RELTYPE_ROS);
		column_meta_oid = vci_create_relation(GenRelName(indexRel, i, VCI_RELTYPE_META), indexRel, indexInfo, VCI_RELTYPE_ROS);

		/* set ROS column pointer, */
		columnPointer = vci_GetMColumn(vmr_info, i);

		columnPointer->meta_oid = column_meta_oid;
		columnPointer->data_oid = column_store_oid;
		columnPointer->max_columns_size = vci_GetColumnWorstSize(TupleDescAttr(tupdesc, i));
		if (TupleDescAttr(tupdesc, i)->attlen == -1)
		{
			columnPointer->comp_type = vcis_compression_type_variable_raw;
		}
		else if (TupleDescAttr(tupdesc, i)->attlen > 0)
		{
			columnPointer->comp_type = vcis_compression_type_fixed_raw;
		}
		else
		{
			Assert(false);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("unexpected attribute length")));
		}
		/* put default extent(free_page) to each columns  */
	}
	vci_SetMainRelVar(vmr_info, vcimrv_num_nullable_columns, 0,
					  vci_GetNumberOfNullableColumn(tupdesc));
	vci_SetMainRelVar(vmr_info, vcimrv_null_width_in_byte, 0,
					  (vci_GetNumberOfNullableColumn(tupdesc) + BITS_PER_BYTE - 1) /
					  BITS_PER_BYTE);

	/* create TID-CRID relations */
	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_TID_CRID, VCI_RELTYPE_META), indexRel, indexInfo, VCI_RELTYPE_TIDCRID);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_crid_meta_oid, 0, oid);

	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_TID_CRID, VCI_RELTYPE_DATA), indexRel, indexInfo, VCI_RELTYPE_TIDCRID);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_crid_data_oid, 0, oid);

	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_TID_CRID_UPDATE, '0'), indexRel, indexInfo, VCI_RELTYPE_TIDCRID);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_crid_update_oid_0, 0, oid);

	oid = vci_create_relation(GenRelName(indexRel, VCI_COLUMN_ID_TID_CRID_UPDATE, '1'), indexRel, indexInfo, VCI_RELTYPE_TIDCRID);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_crid_update_oid_1, 0, oid);

	/* other variables */
	vci_SetMainRelVar(vmr_info, vcimrv_column_info_offset, 0, vcimrv_column_info - VCI_MIN_PAGE_HEADER);

	offsetToExtentInfo = (vci_MRVGetBlockNumber(vcimrv_extent_info) * VCI_MAX_PAGE_SPACE) +
		vci_MRVGetOffset(vcimrv_extent_info) - VCI_MIN_PAGE_HEADER;

	vci_SetMainRelVar(vmr_info, vcimrv_extent_info_offset, 0, offsetToExtentInfo);
	vci_SetMainRelVar(vmr_info, vcimrv_size_mr, 0, offsetToExtentInfo);
	vci_SetMainRelVar(vmr_info, vcimrv_size_mr_old, 0, offsetToExtentInfo);

	vci_SetMainRelVar(vmr_info, vcimrv_current_ros_version, 0, FrozenTransactionId);
	vci_SetMainRelVar(vmr_info, vcimrv_last_ros_version, 0, FrozenTransactionId);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_crid_diff_sel, 0, 0);
	vci_SetMainRelVar(vmr_info, vcimrv_tid_crid_diff_sel_old, 0, 0);

	vci_SetMainRelVar(vmr_info, vcimrv_xid_generation, 0, 1);	/* xid generation starts
																 * from 1 */
	vci_SetMainRelVar(vmr_info, vcimrv_xid_gen_update_xid, 0, GetCurrentTransactionId());

	vci_SetMainRelVar(vmr_info, vcimrv_ros_command, 0, vci_rc_invalid);
	vci_SetMainRelVar(vmr_info, vcimrv_num_unterminated_copy_cmd, 0, 0);

	vci_SetMainRelVar(vmr_info, vcimrv_num_extents, 0, 0);
	vci_SetMainRelVar(vmr_info, vcimrv_num_extents_old, 0, 0);

	/* flush */
	vci_WriteMainRelVar(vmr_info, vci_wmrv_all);

	/* initialize meta data relations and data relations */
	vci_InitializeColumnRelations(vmr_info, tupdesc, heapRel);

	/* initialize meta data relations and data relations */
	vci_InitializeTidCridUpdateLists(vmr_info);
	vci_InitializeTidCridTree(vmr_info);

	/* unlock */
	vci_ReleaseMainRelHeader(vmr_info);
	pfree(vmr_info);

	/* convert data in the relations */
	if (vcirc_truncate != vci_rebuild_command &&
		indexRel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED)
		reltuples = vci_ConvertWos2RosForBuild(indexRel,
											   VciGuc.maintenance_work_mem * (Size) 1024, indexInfo);

	/*
	 * create statistics for return to caller
	 */
	result = palloc0_object(IndexBuildResult);
	result->heap_tuples = reltuples;
	result->index_tuples = -1;

	return result;
}

/*
 * Put or Copy page into INIT_FORK.
 * If valid page is given, that page will be put into INIT_FORK.
 * If invalid page (NULL pointer) is given, MAIN_FORK page will be copied.
 */
static void
vci_putInitPage(Oid oid, Page page, BlockNumber blkno)
{
	Relation	rel;
	Page		pageCopyFrom;
	Buffer		buffer = InvalidBuffer;

	rel = relation_open(oid, AccessExclusiveLock);

	/*
	 * If there is no INIT_FORK, create it. VCI Main Relation may have, but
	 * others may not have.
	 */

	if (!smgrexists(RelationGetSmgr(rel), INIT_FORKNUM))
		smgrcreate(RelationGetSmgr(rel), INIT_FORKNUM, false);

	pageCopyFrom = page;

	if (pageCopyFrom == NULL)
	{
		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		pageCopyFrom = BufferGetPage(buffer);
	}

	PageSetChecksumInplace(pageCopyFrom, blkno);
	smgrwrite(RelationGetSmgr(rel), INIT_FORKNUM, blkno,
			  (char *) pageCopyFrom, true);

	if (XLogIsNeeded())
		log_newpage(&rel->rd_smgr->smgr_rlocator.locator, INIT_FORKNUM,
					blkno, pageCopyFrom, false);

	smgrimmedsync(RelationGetSmgr(rel), INIT_FORKNUM);

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessExclusiveLock);
}

static void
vci_inner_buildempty(Relation indexRel)
{
	Oid			oid;
	Page		tmpPage;
	TupleDesc	itupDesc;

	IndexInfo  *indexInfo;

	vci_MainRelHeaderInfo vmr_infoData;
	vci_MainRelHeaderInfo *vmr_info = &vmr_infoData;

	Relation	heapRel;

	CheckIndexColumnTypes(RelationGetDescr(indexRel));

	/* create VCI main relation */
	vci_InitMainRelHeaderInfo(vmr_info, indexRel, vci_rc_wos_ros_conv_build);
	vci_KeepMainRelHeader(vmr_info);

	/*
	 * WOS relation : a blank page is put again, because the ambuild data
	 * might been inserted in WOS. (it may be OK, WOS can be assumed heap
	 * relation.)
	 */

	tmpPage = (Page) palloc(BLCKSZ);
	PageInit(tmpPage, BLCKSZ, 0);

	oid = vci_GetMainRelVar(vmr_info, vcimrv_data_wos_oid, 0);
	vci_putInitPage(oid, tmpPage, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_whiteout_wos_oid, 0);
	vci_putInitPage(oid, tmpPage, 0);

	pfree(tmpPage);

	oid = vci_GetMainRelVar(vmr_info, vcimrv_tid_meta_oid, 0);
	vci_putInitPage(oid, NULL, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_null_meta_oid, 0);
	vci_putInitPage(oid, NULL, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_delete_meta_oid, 0);
	vci_putInitPage(oid, NULL, 0);

	oid = vci_GetMainRelVar(vmr_info, vcimrv_tid_data_oid, 0);
	vci_putInitPage(oid, NULL, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_null_data_oid, 0);
	vci_putInitPage(oid, NULL, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_delete_data_oid, 0);
	vci_putInitPage(oid, NULL, 0);

	/* column store */
	heapRel = table_open(indexRel->rd_index->indrelid, AccessShareLock);
	itupDesc = RelationGetDescr(indexRel);
	table_close(heapRel, AccessShareLock);

	CheckIndexColumnTypes(itupDesc);

	for (int attn = 0; attn < itupDesc->natts; attn++)
	{
		/* get ROS column pointer, */
		vcis_m_column_t *columnPointer;

		columnPointer = vci_GetMColumn(vmr_info, attn);

		vci_putInitPage(columnPointer->meta_oid, NULL, 0);
		vci_putInitPage(columnPointer->data_oid, NULL, 0);
	}

	oid = vci_GetMainRelVar(vmr_info, vcimrv_tid_crid_meta_oid, 0);
	vci_putInitPage(oid, NULL, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_tid_crid_data_oid, 0);
	vci_putInitPage(oid, NULL, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_tid_crid_update_oid_0, 0);
	vci_putInitPage(oid, NULL, 0);
	oid = vci_GetMainRelVar(vmr_info, vcimrv_tid_crid_update_oid_1, 0);
	vci_putInitPage(oid, NULL, 0);
	/* Copy default content into VCI Main rel INIT_FORK  */
	oid = indexRel->rd_id;
	for (BlockNumber blkno = 0; blkno < lengthof(vmr_info->buffer); blkno++)
	{
		vci_putInitPage(oid, NULL, blkno);
	}

	vci_ReleaseMainRelHeader(vmr_info);

	if (vcirc_truncate != vci_rebuild_command)
	{
		/* extract index key information from the index's pg_index info */
		indexInfo = BuildIndexInfo(indexRel);
		vci_ConvertWos2RosForBuild(indexRel,
								   VciGuc.maintenance_work_mem * (Size) 1024, indexInfo);
	}
}

/* LCOV_EXCL_START */
void
vci_set_copy_transaction_and_command_id(TransactionId xid, CommandId cid)
{
	Assert(NULL == copyInfo.extentList);
	Assert(0 == copyInfo.numAllocatedExtent);
	copyInfo.xid = xid;
	copyInfo.cid = cid;
	copyInfo.numAppendedRows = 0;
	copyInfo.extentList = NULL;
	copyInfo.numFilledExtent = 0;
	copyInfo.numAllocatedExtent = 0;
}

/* LCOV_EXCL_STOP */

static bool
vci_inner_insert(Relation indexRel, ItemPointer heap_tid)
{
	TransactionId xid = GetCurrentTransactionId();
	TupleDesc	tdesc;
	HeapTuple	htup;
	int			options = 0;

	Oid			data_wos_oid;
	Relation	data_wos_rel;

	Datum		new_values[2];
	bool		new_isnull[2];

	vci_MainRelHeaderInfo *vmr_info;

	/* get Data WOS relation from vci main rel */
	vmr_info = palloc0_object(vci_MainRelHeaderInfo);
	vci_InitMainRelHeaderInfo(vmr_info, indexRel, vci_rc_wos_insert);
	vci_KeepMainRelHeader(vmr_info);
	data_wos_oid = (Oid) vci_GetMainRelVar(vmr_info, vcimrv_data_wos_oid, 0);

	data_wos_rel = table_open(data_wos_oid, RowExclusiveLock);

	/* get tuple desc */
	tdesc = RelationGetDescr(data_wos_rel); /* */

	/* create new tuple for insert */
	new_values[0] = ItemPointerGetDatum(heap_tid);
	new_values[1] = Int64GetDatum(vci_GenerateXid64(xid, vmr_info));
	new_isnull[0] = false;
	new_isnull[1] = false;
	htup = heap_form_tuple(tdesc, new_values, new_isnull);

	/* insert (+ WAL) */

	if (copy_with_freeze_option)
		options |= HEAP_INSERT_FROZEN;

	heap_insert(data_wos_rel, htup, GetCurrentCommandId(true), options, NULL);

	heap_freetuple(htup);
	table_close(data_wos_rel, RowExclusiveLock);

	/* unlock */
	vci_ReleaseMainRelHeader(vmr_info);

	return false;
}

/* LCOV_EXCL_START */
static void
WriteOneExtentForCopy(Relation indexRel)
{
	const LOCKMODE lockmode = ShareUpdateExclusiveLock;

	LockRelation(indexRel, lockmode);
	vci_InitMainRelHeaderInfo(&(copyConvContext.info),
							  indexRel, vci_rc_copy_command);
	vci_KeepMainRelHeader(&(copyConvContext.info));
	/* obtain target extent ID */
	copyConvContext.extentId = vci_GetFreeExtentId(&(copyConvContext.info));
	if (copyInfo.numAllocatedExtent <= copyInfo.numFilledExtent)
	{
		copyInfo.numAllocatedExtent += EXTENT_LIST_UNIT_EXTENSION;
		copyInfo.extentList = repalloc_array(copyInfo.extentList, uint32, copyInfo.numAllocatedExtent);
	}
	copyInfo.extentList[++(copyInfo.numFilledExtent)] =
		copyConvContext.extentId;

	/* write one extent into ROS */
	vci_AddTidCridUpdateList(&(copyConvContext.info),
							 &(copyConvContext.storage),
							 copyConvContext.extentId);
	vci_WriteOneExtent(&(copyConvContext.info),
					   &(copyConvContext.storage),
					   copyConvContext.extentId,
					   InvalidTransactionId,
					   copyConvContext.xid,
					   copyConvContext.xid);
	/* write header of the main relation */
	vci_WriteMainRelVar(&(copyConvContext.info),
						vci_wmrv_update);
	UnlockRelation(indexRel, lockmode);
	vci_ReleaseMainRelInCommandContext(&copyConvContext);

	vci_ResetRosChunkStorage(&(copyConvContext.storage));
}

static bool
vci_inner_insert_in_copy(Relation indexRel, ItemPointer heap_tid)
{
	vci_MainRelHeaderInfo *vmr_info = &(copyConvContext.info);

	if (0 == copyInfo.numAppendedRows)
	{
		uint32		val;

		vci_InitRosCommandContext0(&copyConvContext, indexRel,
								   vci_rc_copy_command);
		vci_RecoverOneVCIIfNecessary(vmr_info);

		vci_InitRosCommandContext1(&copyConvContext,
								   VciGuc.maintenance_work_mem * INT64CONST(1024),
								   VCI_NUM_ROWS_IN_EXTENT, 0,
								   false);
		vci_ResetRosChunkStorage(&(copyConvContext.storage));

		vci_WriteExtentInfoInMainRosForCopyInit(vmr_info,
												copyConvContext.extentId,
												copyConvContext.xid);

		/* increment number of copy commands */
		val = vci_GetMainRelVar(vmr_info, vcimrv_num_unterminated_copy_cmd, 0);
		++val;
		vci_SetMainRelVar(vmr_info, vcimrv_num_unterminated_copy_cmd, 0, val);

		vci_SetMainRelVar(vmr_info, vcimrv_ros_command, 0, vci_rc_copy_command);

		/* flush */
		vci_WriteMainRelVar(vmr_info, vci_wmrv_update);

		/* unlock */
		vci_ReleaseMainRelInCommandContext(&copyConvContext);

		/* close heap relation */
		vci_CloseHeapRelInCommandContext(&copyConvContext);
	}

	{
		Relation	rel = table_open(copyConvContext.heapOid, AccessShareLock);
		Buffer		buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(heap_tid));
		Page		page = BufferGetPage(buffer);
		ItemId		lp = PageGetItemId(page, ItemPointerGetOffsetNumber(heap_tid));
		HeapTupleData tupleData;
		HeapTuple	tuple = &tupleData;

		Assert(ItemIdIsNormal(lp));

		tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tuple->t_len = ItemIdGetLength(lp);
		tuple->t_tableOid = RelationGetRelid(rel);
		tuple->t_self = *heap_tid;

		vci_FillOneRowInRosChunkBuffer(&(copyConvContext.buffer),
									   &(copyConvContext.info),
									   &tuple->t_self,
									   tuple,
									   copyConvContext.indxColumnIdList,
									   copyConvContext.heapAttrNumList,
									   vci_GetTupleDescr(vmr_info));

		if (copyConvContext.buffer.numRowsAtOnce <= copyConvContext.buffer.numFilled)
			vci_RegisterChunkBuffer(&(copyConvContext.storage),
									&(copyConvContext.buffer));

		if (copyConvContext.numRowsToConvert <= copyConvContext.storage.numTotalRows)
		{
			Assert(copyConvContext.numRowsToConvert == copyConvContext.storage.numTotalRows);
			WriteOneExtentForCopy(indexRel);
		}

		table_close(rel, AccessShareLock);
	}

	return false;
}

void
vci_FinalizeCopyCommand(void)
{
	if (0 < copyConvContext.storage.numTotalRows)
	{
		Relation	rel = table_open(copyConvContext.indexOid, RowExclusiveLock);

		WriteOneExtentForCopy(rel);
		table_close(rel, RowExclusiveLock);
	}

	vci_FinRosCommandContext(&copyConvContext, false);
}

static IndexBulkDeleteResult *
vci_inner_vacuumcleanup(IndexVacuumInfo *info,
						IndexBulkDeleteResult *stats)
{
	elog(DEBUG2, "%s is called.", __func__);

	LockRelation(info->index, ShareUpdateExclusiveLock);

	vci_VacuumRos(info->index, info);

	UnlockRelation(info->index, ShareUpdateExclusiveLock);

	return NULL;
}

/* LCOV_EXCL_STOP */

/**
 * vci_add_index_delete
 */
void
vci_add_index_delete(Relation heapRel, const ItemPointerData *heap_tid, TransactionId xmin)
{
	List	   *indexoidlist;
	ListCell   *l;

	/* Fast path if definitely no indexes */
	if (!RelationGetForm(heapRel)->relhasindex)
		return;

	/*
	 * Get cached list of index OIDs
	 */
	indexoidlist = RelationGetIndexList(heapRel);

	/* Iterate for indexes */
	foreach(l, indexoidlist)
	{
		Oid			indexOid = lfirst_oid(l);
		Relation	indexRel;

		Oid			whiteoutWosOid;
		Relation	whiteoutWOSRel;
		Datum		new_values[2];
		bool		new_isnull[2];
		HeapTuple	htup;
		TupleDesc	tdesc;

		vci_MainRelHeaderInfo vmr_info_data;
		vci_MainRelHeaderInfo *vmr_info = &vmr_info_data;

		TransactionId xid;

		/* Skip if Index is NOT VCI index */
		indexRel = index_open(indexOid, RowExclusiveLock);
		if (!isVciIndexRelation(indexRel))
		{
			index_close(indexRel, RowExclusiveLock);
			continue;
		}

		if (!fullPageWrites)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("access method \"%s\" does not work under full_page_writes=off", VCI_STRING),
					 errhint("Use DROP INDEX \"%s\"", RelationGetRelationName(indexRel))));

		vci_InitMainRelHeaderInfo(vmr_info, indexRel, vci_rc_wos_delete);
		vci_KeepMainRelHeader(vmr_info);

		/* Open Whiteout WOS */
		whiteoutWosOid = (Oid) vci_GetMainRelVar(vmr_info, vcimrv_whiteout_wos_oid, 0);
		whiteoutWOSRel = table_open(whiteoutWosOid, RowExclusiveLock);

		tdesc = RelationGetDescr(whiteoutWOSRel);

		/* @see generateXidDiff() in vci_ros_command.c */
		if (!TransactionIdEquals(xmin, FrozenTransactionId))
			xid = xmin;
		else
			xid = GetCurrentTransactionId();

		/* create new tuple for insert */
		new_values[0] = ItemPointerGetDatum(heap_tid);
		new_values[1] = Int64GetDatum(vci_GenerateXid64(xid, vmr_info));
		new_isnull[0] = false;
		new_isnull[1] = false;

		htup = heap_form_tuple(tdesc, new_values, new_isnull);

		/* insert TID into Whiteout WOS */
		simple_heap_insert(whiteoutWOSRel, htup);
		heap_freetuple(htup);
		table_close(whiteoutWOSRel, RowExclusiveLock);

		/* flush & unlock */
		vci_ReleaseMainRelHeader(vmr_info);

		index_close(indexRel, RowExclusiveLock);
	}

	list_free(indexoidlist);
}

List *
vci_add_should_index_insert(ResultRelInfo *resultRelInfo,
							TupleTableSlot *slot,
							ItemPointer tupleid,
							EState *estate)
{
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/*
	 * Get information from the result relation info structure.
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (int i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;

		if (indexRelation == NULL)
			continue;

		/* Skip if Index is NOT VCI index */
		if (!isVciIndexRelation(indexRelation))
			continue;

		indexInfo = indexInfoArray[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * If predicate state not set up yet, create it (in the estate's
			 * per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		index_insert(indexRelation, /* index relation */
					 values,	/* array of index Datums */
					 isnull,	/* null flags */
					 tupleid,	/* tid of heap tuple */
					 heapRelation,	/* heap relation */
					 UNIQUE_CHECK_NO,	/* it is ignored in VCI */
					 false,		/* 'logically unchanged index' hint */
					 indexInfo);	/* index AM may need this */
	}

	return NIL;
}

static bool
vci_add_drop_column(const ObjectAddress *object, int flags)
{
	Relation	tableRel;

	if (vci_rebuild_command != vcirc_alter_table)
		return false;

	Assert(object->objectSubId != 0);

	/*
	 * If object->objectSubId < 0, it means that the column is a system
	 * column. Such case occurs only when OID column is modified, but this is
	 * checked in other places. So simply skip in this place.
	 */
	if (object->objectSubId < 0)
		return false;

	tableRel = relation_open(object->objectId, AccessExclusiveLock);

	if (tableRel->rd_rel->relkind != RELKIND_RELATION)
	{
		relation_close(tableRel, AccessExclusiveLock);
		return false;
	}

	relation_close(tableRel, AccessExclusiveLock);

	return false;
}

bool
vci_add_drop_relation(const ObjectAddress *object, int flags)
{
	Relation	rel;
	Oid			ruleId;
	Oid			oid = object->objectId;
	char		relKind = get_rel_relkind(oid);
	bool		concurrent = ((flags & PERFORM_DELETION_CONCURRENTLY)
							  == PERFORM_DELETION_CONCURRENTLY);
	bool		concurrent_lock_mode = ((flags & PERFORM_DELETION_CONCURRENT_LOCK) != 0);
	vci_id_t	vciid;

	if (object->objectSubId != 0)
		return vci_add_drop_column(object, flags);

	if (relKind == RELKIND_INDEX)
	{
		rel = relation_open(oid, AccessExclusiveLock);

		if (!isVciIndexRelation(rel))
		{
			relation_close(rel, NoLock);
			return false;
		}
		relation_close(rel, NoLock);

		/*
		 * Deletion of VCI index by ALTER TABLE command is not supported
		 *
		 * Ereport only if the relation is vci main relation so that it does
		 * not give unnecessary messages.
		 *
		 * Return true when so that the post-processing does not continue.
		 */
		if (vci_rebuild_command == vcirc_alter_table)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter table because the table is indexed by VCI"),
					 errhint("You must drop index \"%s\" before using this command.", RelationGetRelationName(rel))));
		}

		if (concurrent)
			elog(PANIC, "should not reach here");

		index_drop(oid, concurrent, concurrent_lock_mode);

		vciid.oid = oid;
		vciid.dbid = MyDatabaseId;
		vci_freeMemoryEntry(&vciid);
	}
	else
	{
		rel = relation_open(oid, AccessExclusiveLock);

		if (!vci_isVciAdditionalRelation(rel))
		{
			relation_close(rel, NoLock);
			return false;
		}

		/*
		 * Deletion of VCI index by ALTER TABLE command is not supported
		 *
		 * Ereport only if the relation is vci main relation so that it does
		 * not give unneccesary messages.
		 *
		 * Return true when so that the post-processing does not continue.
		 */
		if (vci_rebuild_command == vcirc_alter_table)
		{
			relation_close(rel, NoLock);
			return true;
		}

		if (concurrent)
			elog(PANIC, "should not reach here");

		/* 2.1 Is relation used? */
		CheckTableNotInUse(rel, "DROP TABLE");
		CheckTableForSerializableConflictIn(rel);

		ruleId = get_rewrite_oid(oid, rel->rd_rel->relname.data, true);

		/* 2.2 Drop relation storage */
		RelationDropStorage(rel);

		relation_close(rel, NoLock);
		remove_on_commit_action(oid);

		/* 2.3 release relation cache */
		RelationForgetRelation(oid);

		/* 2.4 remove statistic info */
		RemoveStatistics(oid, 0);

		/* 2.5 remove pg_rewrite entry */
		if (OidIsValid(ruleId))
			RemoveRewriteRuleById(ruleId);

		/* 2.6 remove pg_attributes entry */
		DeleteAttributeTuples(oid);

		/* 2.7 remove pg_system entry */
		DeleteRelationTuple(oid);

	}

	return true;
}

bool
vci_add_reindex_index(Relation indexRel)
{
	bool		continue_after_return;

	/* if it is not VCI relation */
	if (!isVciIndexRelation(indexRel))
		continue_after_return = true;

	/* it is the VCI indexed relation */
	else
	{
		switch (vci_rebuild_command)
		{
			case vcirc_reindex:
				/* called by the command REINDEX except REINDEX INDEX */
				continue_after_return = false;
				break;

			case vcirc_alter_table:

				/*
				 * alter table for columns indexed by vci index, it is not
				 * work
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter table because the table is indexed by VCI"),
						 errhint("You must drop index \"%s\" before using this command.", RelationGetRelationName(indexRel))));
				/* remaining work of reindex_index() must be cancelled */
				continue_after_return = false;
				break;

			case vcirc_truncate:

				/*
				 * This is reindex_index called in truncation Command. In this
				 * case, before RelationSetNewRelfilenumber(indexRel,...) we
				 * must drop other relations for VCI.
				 */
				/* vci_add_drop_index(indexRel->rd_id); */
				continue_after_return = true;
				break;

			case vcirc_cluster:
			case vcirc_vacuum_full:
				/* called by the command CLUSTER or VACUUM FULL */
				continue_after_return = true;
				break;

			default:
				elog(ERROR, "unexpected vci_RebuildCommand");
				break;
		}
	}

	return continue_after_return;
}

bool
vci_add_skip_vci_index(Relation indexRel)
{
	return isVciIndexRelation(indexRel);
}

bool
vci_add_alter_tablespace(Relation indexRel)
{
	if (isVciIndexRelation(indexRel))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER INDEX SET TABLESPACE is not supported for VCI"),
				 errhint("DROP INDEX and CREATE INDEX instead")));
		return true;
	}
	else
		return false;
}

static uint32
GetNumberOfBlocksFromOid(Oid oid)
{
	uint32		result;
	Relation	rel = relation_open(oid, AccessShareLock);

	result = RelationGetNumberOfBlocks(rel);
	relation_close(rel, AccessShareLock);

	return result;
}

static int64
GetNumDBPagesOfVCIElement(vcis_attribute_type_t attrType,
						  int index,
						  vci_MainRelHeaderInfo *info)
{
#ifdef USE_ASSERT_CHECKING
	int			numColumns = vci_GetMainRelVar(info, vcimrv_num_columns, 0);
#endif							/* #ifdef USE_ASSERT_CHECKING */
	Oid			dataOid = InvalidOid;
	Oid			metaOid = InvalidOid;
	int64		result = 0;

	Assert((0 <= attrType) && (attrType < num_vcis_attribute_type));
	Assert((0 <= index) && (index < vci_GetNumIndexForAttributeType(attrType, numColumns)));
	switch (attrType)
	{
		case vcis_attribute_type_main:
			return RelationGetNumberOfBlocks(info->rel);
		case vcis_attribute_type_data_wos:
			dataOid = vci_GetMainRelVar(info, vcimrv_data_wos_oid, 0);
			break;
		case vcis_attribute_type_whiteout_wos:
			dataOid = vci_GetMainRelVar(info, vcimrv_whiteout_wos_oid, 0);
			break;
		case vcis_attribute_type_tid_crid:
			dataOid = vci_GetMainRelVar(info, vcimrv_tid_crid_data_oid, 0);
			metaOid = vci_GetMainRelVar(info, vcimrv_tid_crid_meta_oid, 0);
			break;
		case vcis_attribute_type_tid_crid_update:
			dataOid = vci_GetMainRelVar(info, vcimrv_tid_crid_update_oid_0, index);
			break;
		case vcis_attribute_type_delete_vec:
			dataOid = vci_GetMainRelVar(info, vcimrv_delete_data_oid, 0);
			metaOid = vci_GetMainRelVar(info, vcimrv_delete_meta_oid, 0);
			break;
		case vcis_attribute_type_null_vec:
			dataOid = vci_GetMainRelVar(info, vcimrv_null_data_oid, 0);
			metaOid = vci_GetMainRelVar(info, vcimrv_null_meta_oid, 0);
			break;
		case vcis_attribute_type_tid:
			dataOid = vci_GetMainRelVar(info, vcimrv_tid_data_oid, 0);
			metaOid = vci_GetMainRelVar(info, vcimrv_tid_meta_oid, 0);
			break;
		case vcis_attribute_type_pgsql:
			{
				vcis_m_column_t *mColumn;

				mColumn = vci_GetMColumn(info, index);
				dataOid = mColumn->data_oid;
				metaOid = mColumn->meta_oid;
				break;
			}
		default:
			elog(ERROR, "internal error.  invalid attribute type");
	}

	if (OidIsValid(dataOid))
		result += GetNumberOfBlocksFromOid(dataOid);
	if (OidIsValid(metaOid))
		result += GetNumberOfBlocksFromOid(metaOid);

	return result;
}

PG_FUNCTION_INFO_V1(vci_index_size);
Datum
vci_index_size(PG_FUNCTION_ARGS)
{
	Relation	rel;
	uint32		numColumns;
	uint32		numEntries;
	int64		result = 0;
	vci_MainRelHeaderInfo infoData;
	vci_MainRelHeaderInfo *info = &infoData;
	LOCKMODE	lockmode = AccessShareLock;

	text	   *relname = PG_GETARG_TEXT_P(0);

	if (PG_NARGS() != 1)
		ereport(ERROR,
				(errmsg("vci_index_size requires 1 argument")));

	{
		RangeVar   *relrv;

		relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
		rel = relation_openrv(relrv, lockmode);
		if (!isVciIndexRelation(rel))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("only VCI index is supported")));
	}

	vci_InitMainRelHeaderInfo(info, rel, vci_rc_probe);
	vci_KeepMainRelHeader(info);
	numColumns = vci_GetMainRelVar(info, vcimrv_num_columns, 0);
	numEntries = vci_GetSumOfAttributeIndices(numColumns);

	for (uint32 aId = 0; aId < numEntries; ++aId)
	{
		vcis_attribute_type_t attrType;
		int			index;

		vci_GetAttrTypeAndIndexFromSumOfIndices(&attrType,
												&index,
												numColumns,
												aId);
		result += GetNumDBPagesOfVCIElement(attrType, index, info);
	}

	vci_ReleaseMainRelHeader(info);
	relation_close(rel, lockmode);

	PG_RETURN_INT64(result * BLCKSZ);
}

/*
 * Process Utility Hook
 */

void
vci_process_utility(PlannedStmt *pstmt,
					const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params,
					QueryEnvironment *queryEnv,
					DestReceiver *dest,
					QueryCompletion *qc)
{
	bool		creating_vci_extension = false;
	volatile bool saved_vci_is_in_vci_create_extension;

	Node	   *parseTree = pstmt->utilityStmt;

	vci_check_prohibited_operation(parseTree, &creating_vci_extension);

	saved_vci_is_in_vci_create_extension = vci_is_in_vci_create_extension;

	if (creating_vci_extension)
		vci_is_in_vci_create_extension = true;

	vci_rebuild_command = vcirc_invalid;
	copy_with_freeze_option = false;

#define UNUSE_COPY_INSERT

	switch (nodeTag(parseTree))
	{
			/* check if the statement is a "COPY table FROM ..." statement */
		case T_CopyStmt:
			{
				CopyStmt   *stmt;
				ListCell   *lc;

#ifndef UNUSE_COPY_INSERT
				TransactionId xid = GetCurrentTransactionId();
				CommandId	cid = GetCurrentCommandId(false);

				Assert(TransactionIdIsValid(xid));
				Assert(InvalidCommandId != cid);
				vci_set_copy_transaction_and_command_id(xid, cid);
#endif							/* #ifndef UNUSE_COPY_INSERT  */

				stmt = (CopyStmt *) parseTree;

				foreach(lc, stmt->options)
				{
					DefElem    *defel = (DefElem *) lfirst(lc);

					if (strcmp(defel->defname, "freeze") == 0)
					{
						if (defGetBoolean(defel))
						{
							copy_with_freeze_option = true;
							break;
						}
					}
				}
			}
			break;

			/* check if the statement is a TRUNCATE for VCI Indexed table */
		case T_TruncateStmt:
			vci_rebuild_command = vcirc_truncate;
			break;

			/* check if the statement is a REINDEX for VCI Indexed table */
		case T_ReindexStmt:
			vci_rebuild_command = vcirc_reindex;
			break;

			/* check if the statement is a REINDEX for VCI Indexed table */
		case T_AlterTableStmt:
			vci_rebuild_command = vcirc_alter_table;
			break;

			/* check if the statement is a VACUUM for VCI Indexed table */
		case T_VacuumStmt:
			vci_rebuild_command = vcirc_vacuum_full;
			break;

			/* check if the statement is a CLUSTER for VCI Indexed table */
		case T_ClusterStmt:
			vci_rebuild_command = vcirc_cluster;
			break;

		default:
			break;
	}

	if (creating_vci_extension)
	{
		PG_TRY();
		{
			if (process_utility_prev != NULL)
				process_utility_prev(pstmt, queryString, readOnlyTree,
									 context, params, queryEnv,
									 dest, qc);
			else
				standard_ProcessUtility(pstmt, queryString, readOnlyTree,
										context, params, queryEnv,
										dest, qc);
		}
		PG_CATCH();
		{
			vci_is_in_vci_create_extension = saved_vci_is_in_vci_create_extension;

			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
	{
		if (process_utility_prev != NULL)
			process_utility_prev(pstmt, queryString, readOnlyTree,
								 context, params, queryEnv,
								 dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
	}

	vci_rebuild_command = vcirc_invalid;

	vci_is_in_vci_create_extension = saved_vci_is_in_vci_create_extension;

#ifndef UNUSE_COPY_INSERT
	/* check if the statement is a "COPY table FROM ..." statement */
	if (nodeTag(parseTree) == T_CopyStmt)
		vci_FinalizeCopyCommand();
#endif							/* #ifndef UNUSE_COPY_INSERT */
}

/*
 * VCI handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
PG_FUNCTION_INFO_V1(vci_handler);

Datum
vci_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 1;
	amroutine->amsupport = 0;
	amroutine->amoptsprocnum = 0;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanhash = false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = false;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = vci_build;
	amroutine->ambuildempty = vci_buildempty;
	amroutine->aminsert = vci_insert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = vci_bulkdelete;
	amroutine->amvacuumcleanup = vci_vacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = vci_costestimate;
	amroutine->amgettreeheight = vci_gettreeheight;
	amroutine->amoptions = vci_options;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = vci_validate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = vci_beginscan;
	amroutine->amrescan = vci_rescan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = NULL;
	amroutine->amendscan = vci_endscan;
	amroutine->ammarkpos = vci_markpos;
	amroutine->amrestrpos = vci_restrpos;

	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	amroutine->amtranslatestrategy = NULL;
	amroutine->amtranslatecmptype = NULL;

	PG_RETURN_POINTER(amroutine);
}
