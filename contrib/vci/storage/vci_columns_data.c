/*-------------------------------------------------------------------------
 *
 * vci_columns_data.c
 *	  Definitions of functions to check which columns are indexed.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_columns_data.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#include "vci.h"
#include "vci_columns.h"
#include "vci_columns_data.h"
#include "vci_ros.h"

static Bitmapset *parseVciColumnsIds(const char *vci_column_ids);

/* Convert comma-separated column ids to Bitmapset */
static Bitmapset *
parseVciColumnsIds(const char *vci_column_ids)
{
	List	   *columnlist;
	ListCell   *l;

	/* SplitIdentifierString can destroy the first argument. */
	char	   *copied_ids = pstrdup(vci_column_ids);
	Bitmapset  *indexedAttids = NULL;
	int			attid = 0;

	if (!SplitIdentifierString(copied_ids, ',', &columnlist))
		ereport(ERROR, (errmsg("internal error. failed to split")));

	foreach(l, columnlist)
	{
		char	   *number_str = (char *) lfirst(l);

		/* The max id is '1600' -> 4 digits. */
		int			attid_diff = pg_strtoint32(number_str);

		attid += attid_diff;

		if (attid >= MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("invalid attribute number %d", attid + 1)));

		if (bms_is_member(attid, indexedAttids))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 (errmsg("duplicated columns in vci index creation")),
					 errhint("duplicated columns are specified")));

		indexedAttids = bms_add_member(indexedAttids, attid);
	}

	pfree(copied_ids);

	return indexedAttids;
}

/*
 * vci_ConvertAttidBitmap2String -- Convert a Bitmapset that represents which
									attids are targets to comma separated string
 */
char *
vci_ConvertAttidBitmap2String(Bitmapset *attid_bitmap)
{
	int			attid;
	int			preAttid = 0;
	StringInfo	buf = makeStringInfo();

	attid = -1;
	while ((attid = bms_next_member(attid_bitmap, attid)) >= 0)
	{
		if (attid >= MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("invalid attribute number %d", attid + 1)));

		if (buf->len == 0)
			appendStringInfo(buf, "%d", attid - preAttid);
		else
			appendStringInfo(buf, ",%d", attid - preAttid);

		preAttid = attid;
	}
	return buf->data;
}

/*
 * vci_ExtractColumnDataUsingIds -- returns TupleDesc that contains indexed columns
 *							        information.
 *
 * The vci_GetTupleDescr() requires a prebuilt vci_MainRelHeaderInfo. So please use
 * this when building a VCI because the structure is in the process of building.
 */
TupleDesc
vci_ExtractColumnDataUsingIds(const char *vci_column_ids, Relation heapRel)
{
	int			i;
	int			attid;
	TupleDesc	heapTupDesc;
	TupleDesc	result;
	Bitmapset  *indexedAttids = NULL;	/* for duplication check */

	heapTupDesc = RelationGetDescr(heapRel);
	indexedAttids = parseVciColumnsIds(vci_column_ids);
	result = CreateTemplateTupleDesc(bms_num_members(indexedAttids));

	attid = -1;
	i = 0;
	while ((attid = bms_next_member(indexedAttids, attid)) >= 0)
	{
		if (attid >= heapTupDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid attribute number %d", attid + 1)));

		TupleDescCopyEntry(result, i + 1, heapTupDesc, attid + 1);
		i++;
	}

	bms_free(indexedAttids);

	return result;
}

/*
 * vci_GetTupleDescr -- returns TupleDesc that contains indexed columns
 *                      information from vci_MainRelHeaderInfo.
 */
TupleDesc
vci_GetTupleDescr(vci_MainRelHeaderInfo *info)
{
	MemoryContext oldcontext;

	if (info->cached_tupledesc)
		return info->cached_tupledesc;

	oldcontext = MemoryContextSwitchTo(info->initctx);

	info->cached_tupledesc = RelationGetDescr(info->rel);

	MemoryContextSwitchTo(oldcontext);

	return info->cached_tupledesc;
}

Bitmapset *
vci_MakeIndexedColumnBitmap(Oid mainRelationOid,
							MemoryContext sharedMemCtx,
							LOCKMODE lockmode)
{
	Relation	main_rel;
	Bitmapset  *result = NULL;
	vci_MainRelHeaderInfo *info;

	info = MemoryContextAllocZero(sharedMemCtx,
								  sizeof(vci_MainRelHeaderInfo));
	main_rel = relation_open(mainRelationOid, lockmode);
	vci_InitMainRelHeaderInfo(info, main_rel, vci_rc_query);
	vci_KeepMainRelHeader(info);

	{
		int32		indexNumColumns = vci_GetMainRelVar(info,
														vcimrv_num_columns, 0);

		for (int aId = 0; aId < indexNumColumns; ++aId)
		{
			vcis_m_column_t *mColumn = vci_GetMColumn(info, aId);
			LOCKMODE	lockmode_for_meta = AccessShareLock;
			Relation	column_meta_rel = table_open(mColumn->meta_oid, lockmode_for_meta);
			Buffer		buffer;
			vcis_column_meta_t *metaHeader = vci_GetColumnMeta(&buffer, column_meta_rel);

			Assert(metaHeader->pgsql_attnum > InvalidAttrNumber);
			result = bms_add_member(result, metaHeader->pgsql_attnum);
			ReleaseBuffer(buffer);
			table_close(column_meta_rel, lockmode_for_meta);
		}
	}

	vci_ReleaseMainRelHeader(info);
	relation_close(main_rel, lockmode);

	return result;
}

/**
 * @brief Get attribute number from the name.
 *
 * @param[in] desc The tuple descriptor of the relation.
 * @param[in] name The name of attribute.
 * @return The attribute number.
 * If the name is not found in the descriptor, InvalidAttrNumber is returned.
 */
AttrNumber
vci_GetAttNum(TupleDesc desc, const char *name)
{
	for (int aId = 0; aId < desc->natts; ++aId)
	{
		if (strcmp(name, NameStr(TupleDescAttr(desc, aId)->attname)) == 0)
			return aId + 1;
	}

	return InvalidAttrNumber;
}
