/*-------------------------------------------------------------------------
 *
 * vci_internal_view.c
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_internal_view.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"	/* for RangeVarGetRelid */
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_namespace.h"
#include "commands/tablecmds.h"
#include "commands/defrem.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"		/* for F_OIDEQ */
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "vci.h"

#include "vci_ros.h"

bool		vci_is_in_vci_create_extension;

static List *make_dependent_view_list(Oid relOid);
static void change_owner_or_schema_of_internal_view_list(List *internal_view_oid_list, Oid newOid, bool is_owner);
static void check_prohibited_operation_for_extension(const char *extname);
static void check_prohibited_operation_for_access_method(const char *amname);
static void check_prohibited_operation_for_range_var(RangeVar *rel);
static void check_prohibited_operation_for_object(ObjectType objtype, Node *object);
static void check_prohibited_operation_for_relation(Relation rel);
static bool is_vci_access_method(Oid accessMethodObjectId);

void
vci_alter_table_change_owner(Oid relOid, char relKind, Oid newOwnerId)
{
	List	   *view_oid_list = NIL;

	if (relKind != RELKIND_INDEX)
		return;

	view_oid_list = make_dependent_view_list(relOid);

	if (view_oid_list == NIL)
		return;

	change_owner_or_schema_of_internal_view_list(view_oid_list, newOwnerId, true);

	list_free(view_oid_list);
}

void
vci_alter_table_change_schema(Oid relOid, char relKind, Oid newNspOid)
{
	List	   *view_oid_list = NIL;

	if (relKind != RELKIND_INDEX)
		return;

	view_oid_list = make_dependent_view_list(relOid);

	if (view_oid_list == NIL)
		return;

	change_owner_or_schema_of_internal_view_list(view_oid_list, newNspOid, false);

	list_free(view_oid_list);
}

static List *
make_dependent_view_list(Oid relOid)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;
	List	   *view_oid_list = NIL;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);

		Assert(pg_depend->refclassid == RelationRelationId);
		Assert(pg_depend->refobjid == relOid);

		/* Ignore dependees that aren't user columns of relations */
		/* (we assume system columns are never of rowtypes) */
		if (pg_depend->classid != RelationRelationId ||
			pg_depend->refobjsubid != 0)
			continue;

		view_oid_list = lappend_oid(view_oid_list, pg_depend->objid);
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	return view_oid_list;
}

static void
change_owner_or_schema_of_internal_view_list(List *view_oid_list, Oid newOid, bool is_owner)
{
	ListCell   *lc;

	foreach(lc, view_oid_list)
	{
		Oid			childRelOid = lfirst_oid(lc);
		Relation	class_rel;
		HeapTuple	tuple;
		Form_pg_class tuple_class;

		class_rel = table_open(RelationRelationId, RowExclusiveLock);

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(childRelOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", childRelOid);

		tuple_class = (Form_pg_class) GETSTRUCT(tuple);

		if (vci_isVciAdditionalRelationTuple(childRelOid, tuple_class))
		{
			Datum		repl_val[Natts_pg_class];
			bool		repl_null[Natts_pg_class];
			bool		repl_repl[Natts_pg_class];
			Acl		   *newAcl;
			Datum		aclDatum;
			bool		isNull;
			HeapTuple	newtuple;

			memset(repl_null, false, sizeof(repl_null));
			memset(repl_repl, false, sizeof(repl_repl));

			if (is_owner)
			{
				repl_repl[Anum_pg_class_relowner - 1] = true;
				repl_val[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(newOid);

				aclDatum = SysCacheGetAttr(RELOID, tuple,
										   Anum_pg_class_relacl,
										   &isNull);
				if (!isNull)
				{
					newAcl = aclnewowner(DatumGetAclP(aclDatum),
										 tuple_class->relowner, newOid);
					repl_repl[Anum_pg_class_relacl - 1] = true;
					repl_val[Anum_pg_class_relacl - 1] = PointerGetDatum(newAcl);
				}
			}
			else
			{
				repl_repl[Anum_pg_class_relnamespace - 1] = true;
				repl_val[Anum_pg_class_relnamespace - 1] = ObjectIdGetDatum(newOid);
			}

			newtuple = heap_modify_tuple(tuple, RelationGetDescr(class_rel), repl_val, repl_null, repl_repl);

			CatalogTupleUpdate(class_rel, &newtuple->t_self, newtuple);

			heap_freetuple(newtuple);
		}

		ReleaseSysCache(tuple);
		table_close(class_rel, RowExclusiveLock);
	}
}

void
vci_check_prohibited_operation(Node *parseTree, bool *creating_vci_extension)
{
	switch (nodeTag(parseTree))
	{
		case T_CreateExtensionStmt:
			{
				CreateExtensionStmt *stmt = (CreateExtensionStmt *) parseTree;

				if (strcmp(stmt->extname, VCI_STRING) == 0)
				{
					ListCell   *lc;

					foreach(lc, stmt->options)
					{
						DefElem    *defel = (DefElem *) lfirst(lc);

						if (strcmp(defel->defname, "schema") == 0
							&& get_namespace_oid(defGetString(defel), false) != PG_PUBLIC_NAMESPACE)
						{
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("extension \"%s\" cannot specify a schema name", VCI_STRING)));
						}
					}

					*creating_vci_extension = true;
				}
			}
			break;

		case T_AlterExtensionStmt:
			check_prohibited_operation_for_extension(((AlterExtensionStmt *) parseTree)->extname);
			break;

		case T_AlterExtensionContentsStmt:
			check_prohibited_operation_for_extension(((AlterExtensionContentsStmt *) parseTree)->extname);
			break;

		case T_ViewStmt:		/* CREATE (OR REPLACE) VIEW */
			check_prohibited_operation_for_range_var(((ViewStmt *) parseTree)->view);
			break;

		case T_AlterTableStmt:	/* ALTER VIEW */
			check_prohibited_operation_for_range_var(((AlterTableStmt *) parseTree)->relation);
			break;

		case T_RuleStmt:		/* CREATE RULE */
			check_prohibited_operation_for_range_var(((RuleStmt *) parseTree)->relation);
			break;

		case T_CreateTrigStmt:	/* CREATE TRIGGER */
			check_prohibited_operation_for_range_var(((CreateTrigStmt *) parseTree)->relation);
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parseTree;

				if ((stmt->targtype == ACL_TARGET_OBJECT) && (stmt->objtype == OBJECT_TABLE))
				{
					ListCell   *lc;

					foreach(lc, stmt->objects)
						check_prohibited_operation_for_range_var((RangeVar *) lfirst(lc));
				}
			}
			break;

		case T_GrantRoleStmt:
			break;

		case T_CreateOpClassStmt:
			if (!vci_is_in_vci_create_extension)
				check_prohibited_operation_for_access_method(((CreateOpClassStmt *) parseTree)->amname);
			break;

		case T_CreateOpFamilyStmt:
			if (!vci_is_in_vci_create_extension)
				check_prohibited_operation_for_access_method(((CreateOpFamilyStmt *) parseTree)->amname);
			break;

		case T_AlterOpFamilyStmt:
			if (!vci_is_in_vci_create_extension)
				check_prohibited_operation_for_access_method(((AlterOpFamilyStmt *) parseTree)->amname);
			break;

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parseTree;
				Relation	rel;

				if (stmt->kind != REINDEX_OBJECT_INDEX)
					break;

				rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);

				if (rel == NULL)
					break;

				if (isVciIndexRelation(rel))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("REINDEX is not supported for VCI"),
							 errhint("DROP INDEX and CREATE INDEX instead")));

				relation_close(rel, AccessShareLock);
			}
			break;

		case T_ClusterStmt:
			{
				ClusterStmt *stmt = (ClusterStmt *) parseTree;
				Relation	rel;

				/*
				 * Do nothing, if CLUSTER command issued without relation
				 * name. As this command will only cluster previously
				 * clustered tables, VCI indexed tables will not be clustered
				 * anyways
				 */
				if (stmt->relation == NULL)
					break;

				rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);

				if (rel == NULL)
					break;

				if (RelationGetForm(rel)->relhasindex)
				{
					List	   *indexoidlist;
					ListCell   *lc;

					indexoidlist = RelationGetIndexList(rel);

					foreach(lc, indexoidlist)
					{
						Oid			indexOid = lfirst_oid(lc);
						Relation	indexRel;

						indexRel = index_open(indexOid, AccessShareLock);

						if (isVciIndexRelation(indexRel))
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("cannot cluster tables including %s index(es)", VCI_STRING),
									 errhint("Use DROP INDEX %s first", RelationGetRelationName(indexRel))));

						index_close(indexRel, AccessShareLock);
					}
				}

				relation_close(rel, AccessShareLock);
			}
			break;

		case T_CommentStmt:		/* COMMENT */
			{
				CommentStmt *stmt = (CommentStmt *) parseTree;

				if (stmt->objtype == OBJECT_MATVIEW)
					check_prohibited_operation_for_object(stmt->objtype, stmt->object);
			}
			break;

		case T_SecLabelStmt:	/* SECURITY LABEL */
			{
				SecLabelStmt *stmt = (SecLabelStmt *) parseTree;

				if (stmt->objtype == OBJECT_MATVIEW)
					check_prohibited_operation_for_object(stmt->objtype, stmt->object);
			}
			break;

		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parseTree;

				switch (stmt->renameType)
				{
					case OBJECT_MATVIEW:
						check_prohibited_operation_for_range_var(stmt->relation);
						break;

					case OBJECT_OPCLASS:
					case OBJECT_OPFAMILY:
						check_prohibited_operation_for_object(stmt->renameType, stmt->object);
						break;
					default:
						break;
				}
			}
			break;

		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parseTree;

				switch (stmt->objectType)
				{
					case OBJECT_MATVIEW:
						check_prohibited_operation_for_range_var(stmt->relation);
						break;

					case OBJECT_EXTENSION:
					case OBJECT_OPCLASS:
					case OBJECT_OPFAMILY:
						check_prohibited_operation_for_object(stmt->objectType, stmt->object);
						break;

					default:
						break;
				}
			}
			break;

		case T_AlterOwnerStmt:
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) parseTree;

				switch (stmt->objectType)
				{
					case OBJECT_OPCLASS:
					case OBJECT_OPFAMILY:
						check_prohibited_operation_for_object(stmt->objectType, stmt->object);
						break;

					default:
						break;
				}
			}
			break;

		case T_IndexStmt:
			{
				IndexStmt  *stmt = (IndexStmt *) parseTree;

				if (strcmp(stmt->accessMethod, VCI_STRING) == 0)
				{
					if (stmt->concurrent)
					{
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("access method \"%s\" does not support concurrent index build", VCI_STRING),
								 errhint("Use DROP INDEX to remove an vci index and try again without CONCURRENTLY option")));
					}
				}
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parseTree;

				if (stmt->removeType == OBJECT_INDEX)
				{
					ListCell   *lc;

					if (stmt->concurrent)
					{
						foreach(lc, stmt->objects)
						{
							RangeVar   *range_var = makeRangeVarFromNameList((List *) lfirst(lc));
							Relation	relation;

							relation = relation_openrv_extended(range_var, AccessShareLock, true);

							if (relation == NULL)
								break;

							if (isVciIndexRelation(relation))
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("access method \"%s\" does not support concurrent index drop", VCI_STRING),
										 errhint("Try again without CONCURRENTLY option")));

							relation_close(relation, AccessShareLock);
						}
					}
				}
			}
			break;

			/*
			 * REFRESH MATERIALIZED VIEW on a VCI internal materialized view
			 * is prohibited.
			 */
		case T_RefreshMatViewStmt:
			check_prohibited_operation_for_range_var(((RefreshMatViewStmt *) parseTree)->relation);
			break;

		default:
			break;
	}
}

static void
check_prohibited_operation_for_extension(const char *extname)
{
	if (strcmp(extname, VCI_STRING) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" prohibits this operation", VCI_STRING)));
}

static void
check_prohibited_operation_for_access_method(const char *amname)
{
	if (strcmp(amname, VCI_STRING) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" prohibits this operation on access method \"%s\"",
						VCI_STRING, VCI_STRING)));
}

static void
check_prohibited_operation_for_range_var(RangeVar *range_var)
{
	Relation	rel;

	rel = relation_openrv_extended(range_var, AccessShareLock, true);

	if (rel == NULL)
		return;

	check_prohibited_operation_for_relation(rel);

	relation_close(rel, AccessShareLock);
}

static void
check_prohibited_operation_for_object(ObjectType objtype, Node *object)
{
	switch (objtype)
	{
		case OBJECT_EXTENSION:
			check_prohibited_operation_for_extension(strVal(object));
			break;

		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
			{
				ObjectAddress address;
				Relation	relation = NULL;

				address = get_object_address(objtype, object, &relation, AccessShareLock, true);

				if (!OidIsValid(address.objectId))
					goto done;

				switch (objtype)
				{
					case OBJECT_MATVIEW:
						check_prohibited_operation_for_relation(relation);
						break;

					case OBJECT_OPCLASS:
						{
							Relation	opclass_rel;
							HeapTuple	opclass_tuple;
							Form_pg_opclass opclass_form;

							opclass_rel = table_open(OperatorClassRelationId, AccessShareLock);

							opclass_tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(address.objectId));
							if (!HeapTupleIsValid(opclass_tuple))	/* should not happen */
								elog(ERROR, "cache lookup failed for opclass %u", address.objectId);

							opclass_form = (Form_pg_opclass) GETSTRUCT(opclass_tuple);

							if (is_vci_access_method(opclass_form->opcmethod))
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("extension \"%s\" prohibits this operation on operation class \"%s\"",
												VCI_STRING, NameStr(opclass_form->opcname))));

							ReleaseSysCache(opclass_tuple);
							table_close(opclass_rel, AccessShareLock);
						}
						break;

					case OBJECT_OPFAMILY:
						{
							Relation	opfamily_rel;
							HeapTuple	opfamily_tuple;
							Form_pg_opfamily opfamily_form;

							opfamily_rel = table_open(OperatorFamilyRelationId, AccessShareLock);

							opfamily_tuple = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(address.objectId));
							if (!HeapTupleIsValid(opfamily_tuple))	/* should not happen */
								elog(ERROR, "cache lookup failed for opfamily %u", address.objectId);

							opfamily_form = (Form_pg_opfamily) GETSTRUCT(opfamily_tuple);

							if (is_vci_access_method(opfamily_form->opfmethod))
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("extension \"%s\" prohibits this operation on operation family \"%s\"",
												VCI_STRING, NameStr(opfamily_form->opfname))));

							ReleaseSysCache(opfamily_tuple);
							table_close(opfamily_rel, AccessShareLock);
						}
						break;

					default:
						break;
				}

		done:
				if (relation != NULL)
					relation_close(relation, AccessShareLock);
			}
			break;

		default:
			break;
	}
}

static void
check_prohibited_operation_for_relation(Relation rel)
{
	if (vci_isVciAdditionalRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" prohibits this operation on view \"%s\"",
						VCI_STRING, NameStr(rel->rd_rel->relname))));
}

static bool
is_vci_access_method(Oid accessMethodObjectId)
{
	HeapTuple	amtuple;
	bool		result = false;
	Form_pg_am	amform;

	amtuple = SearchSysCache1(AMOID,
							  ObjectIdGetDatum(accessMethodObjectId));

	if (!HeapTupleIsValid(amtuple))
	{
		elog(WARNING,
			 "cache lookup failed for access method %u", accessMethodObjectId);

		return false;
	}

	amform = (Form_pg_am) GETSTRUCT(amtuple);

	if (strcmp(NameStr(amform->amname), VCI_STRING) == 0)
		result = true;

	ReleaseSysCache(amtuple);

	return result;
}
