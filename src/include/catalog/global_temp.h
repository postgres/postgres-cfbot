/*-------------------------------------------------------------------------
 *
 * global_temp.h
 *	  Global temporary relation management.
 *
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/catalog/global_temp.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GLOBAL_TEMP_H
#define GLOBAL_TEMP_H

#include "storage/relfilelocator.h"
#include "utils/rel.h"

/*
 * GtrInfo
 *
 *	Structure holding information about a global temporary relation that is
 *	local to the current session.  These properties act as local overrides to
 *	the information stored in pg_class, allowing it to vary between backends.
 */
typedef struct GtrInfo
{
	Oid			relfilenode;	/* the relation's physical storage file */
	Oid			reltablespace;	/* the relation's tablespace identifier */
} GtrInfo;

/*
 * Copy all pg_class attributes that may be session-local for a global
 * temporary relation from "source" to "target", where the source and target
 * may be of type Form_pg_class or GtrInfo *.
 *
 * Beware of multiple evaluations of arguments!
 */
#define COPY_PG_CLASS_GTR_INFO(source, target) \
	do { \
		(target)->relfilenode = (source)->relfilenode; \
		(target)->reltablespace = (source)->reltablespace; \
	} while (0)

extern void TrackGlobalTempRelationStorage(Oid relid, RelFileLocator rlocator,
										   ProcNumber backend, bool create);
extern void ReassignGlobalTempRelationStorage(RelFileLocator rlocator,
											  Oid newRelid);
extern void InitGlobalTempRelation(Relation relation);
extern void TrackGlobalTempRelation(Relation relation);
extern void ForgetGlobalTempRelation(Oid relid);
extern void InvalidateGlobalTempRelation(Oid relid);
extern void ProcessInvalidatedGlobalTempRelations(void);
extern void AtEOXact_GlobalTempRelation(bool isCommit);
extern void AtEOSubXact_GlobalTempRelation(bool isCommit,
										   SubTransactionId mySubid,
										   SubTransactionId parentSubid);
extern bool IsGlobalTempRelationInUse(Oid relid);
extern bool IsOtherUsingGlobalTempRelation(Oid relid);
extern List *GetAllGlobalTempRelationsInUse(Oid dbId);
extern GtrInfo *GetGlobalTempRelationInfo(Oid relid);
extern GtrInfo *GetGlobalTempRelationInfoForUpdate(Oid relid);
extern HeapTuple GetEffectivePgClassTuple(Oid relid);

/*
 * Get the effective value of relfilenode for a relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline Oid
GetEffective_relfilenode(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->relfilenode : class_form->relfilenode;
}

/*
 * Get the effective value of reltablespace for a relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline Oid
GetEffective_reltablespace(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->reltablespace : class_form->reltablespace;
}

/*
 * Set the effective value of relfilenode for a relation.  For a global
 * temporary relation, GetGlobalTempRelationInfoForUpdate() should have been
 * used to obtain gtr_info, and it will be updated instead of the pg_class
 * entry.  Otherwise, the value is set in the pg_class entry.
 */
static inline void
SetEffective_relfilenode(Form_pg_class class_form, GtrInfo *gtr_info, Oid newval)
{
	if (gtr_info != NULL)
		gtr_info->relfilenode = newval;
	else
		class_form->relfilenode = newval;
}

/*
 * Set the effective value of reltablespace for a relation.  For a global
 * temporary relation, GetGlobalTempRelationInfoForUpdate() should have been
 * used to obtain gtr_info, and it will be updated *in addition to* updating
 * the pg_class entry, since we want a tablespace change to apply to both the
 * current session and all future sessions.
 */
static inline void
SetEffective_reltablespace(Form_pg_class class_form, GtrInfo *gtr_info, Oid newval)
{
	/* NB: newval is set *both* locally and globally */
	if (gtr_info != NULL)
		gtr_info->reltablespace = newval;
	class_form->reltablespace = newval;
}

#endif							/* GLOBAL_TEMP_H */
