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
 *	the information stored in pg_class and pg_index, allowing it to vary
 * between backends.
 */
typedef struct GtrInfo
{
	/* pg_class info */
	Oid			relfilenode;	/* the relation's physical storage file */
	Oid			reltablespace;	/* the relation's tablespace identifier */
	int32		relpages;		/* rel stats: number of blocks */
	float4		reltuples;		/* rel stats: number of tuples */
	int32		relallvisible;	/* rel stats: number of all-visible blocks */
	int32		relallfrozen;	/* rel stats: number of all-frozen blocks */
	TransactionId relfrozenxid; /* vac stats: all Xids < this are frozen */
	MultiXactId relminmxid;		/* vac stats: all multixacts are >= this */

	/* pg_index info */
	bool		indisvalid;		/* is the index valid in this session? */
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
		(target)->relpages = (source)->relpages; \
		(target)->reltuples = (source)->reltuples; \
		(target)->relallvisible = (source)->relallvisible; \
		(target)->relallfrozen = (source)->relallfrozen; \
		(target)->relfrozenxid = (source)->relfrozenxid; \
		(target)->relminmxid = (source)->relminmxid; \
	} while (0)

extern void TrackGlobalTempRelationStorage(Oid relid, RelFileLocator rlocator,
										   ProcNumber backend, bool create,
										   bool register_delete);
extern void ReassignGlobalTempRelationStorage(RelFileLocator rlocator,
											  Oid newRelid);
extern void InitGlobalTempRelation(Relation relation);
extern void TrackGlobalTempRelation(Relation relation, bool isNew);
extern void ForgetGlobalTempRelation(Oid relid);
extern void InvalidateGlobalTempRelation(Oid relid);
extern void ProcessInvalidatedGlobalTempRelations(void);
extern void UpdateTempFrozenXids(void);
extern void AtEOXact_GlobalTempRelation(bool isCommit);
extern void AtEOSubXact_GlobalTempRelation(bool isCommit,
										   SubTransactionId mySubid,
										   SubTransactionId parentSubid);
extern bool IsGlobalTempRelationInUse(Oid relid);
extern bool IsOtherUsingGlobalTempRelation(Oid relid);
extern List *GetAllGlobalTempRelationsInUse(Oid dbId);
extern GtrInfo *GetGlobalTempRelationInfo(Oid relid);
extern GtrInfo *GetGlobalTempRelationInfoForUpdate(Oid relid);
extern GtrInfo *GetGlobalTempRelationInfoForInPlaceUpdate(Oid relid);
extern HeapTuple GetEffectivePgClassTuple(Oid relid);
extern HeapTuple GetEffectivePgIndexTuple(Oid indexrelid);
extern void DiscardGlobalTempRelations(void);

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
 * Get the effective value of relpages for a relation.  For a global temporary
 * relation, the value from gtr_info (if present) takes precedence.
 */
static inline int32
GetEffective_relpages(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->relpages : class_form->relpages;
}

/*
 * Get the effective value of reltuples for a relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline float4
GetEffective_reltuples(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->reltuples : class_form->reltuples;
}

/*
 * Get the effective value of relallvisible for a relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline int32
GetEffective_relallvisible(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->relallvisible : class_form->relallvisible;
}

/*
 * Get the effective value of relallfrozen for a relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline int32
GetEffective_relallfrozen(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->relallfrozen : class_form->relallfrozen;
}

/*
 * Get the effective value of relfrozenxid for a relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline TransactionId
GetEffective_relfrozenxid(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->relfrozenxid : class_form->relfrozenxid;
}

/*
 * Get the effective value of relminmxid for a relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline MultiXactId
GetEffective_relminmxid(Form_pg_class class_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->relminmxid : class_form->relminmxid;
}

/*
 * Get the effective value of indisvalid for an index relation.  For a global
 * temporary relation, the value from gtr_info (if present) takes precedence.
 */
static inline bool
GetEffective_indisvalid(Form_pg_index index_form, GtrInfo *gtr_info)
{
	return gtr_info != NULL ? gtr_info->indisvalid : index_form->indisvalid;
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

/*
 * Set the effective value of relpages for a relation.  For a global temporary
 * relation, GetGlobalTempRelationInfoFor[InPlace]Update() should have been
 * used to obtain gtr_info, and it will be updated instead of the pg_class
 * entry.  Otherwise, the value is set in the pg_class entry.
 *
 * If non-NULL, the class_dirty or gtr_dirty flag is set to true, if the value
 * in pg_class or gtr_info actually changes.
 */
static inline void
SetEffective_relpages(Form_pg_class class_form, GtrInfo *gtr_info,
					  int32 newval, bool *class_dirty, bool *gtr_dirty)
{
	if (gtr_info != NULL)
	{
		if (newval != gtr_info->relpages)
		{
			gtr_info->relpages = newval;
			if (gtr_dirty != NULL)
				*gtr_dirty = true;
		}
	}
	else if (newval != class_form->relpages)
	{
		class_form->relpages = newval;
		if (class_dirty != NULL)
			*class_dirty = true;
	}
}

/*
 * Set the effective value of reltuples for a relation.  For a global
 * temporary relation, GetGlobalTempRelationInfoFor[InPlace]Update() should
 * have been used to obtain gtr_info, and it will be updated instead of the
 * pg_class entry.  Otherwise, the value is set in the pg_class entry.
 *
 * If non-NULL, the class_dirty or gtr_dirty flag is set to true, if the value
 * in pg_class or gtr_info actually changes.
 */
static inline void
SetEffective_reltuples(Form_pg_class class_form, GtrInfo *gtr_info,
					   float4 newval, bool *class_dirty, bool *gtr_dirty)
{
	if (gtr_info != NULL)
	{
		if (newval != gtr_info->reltuples)
		{
			gtr_info->reltuples = newval;
			if (gtr_dirty != NULL)
				*gtr_dirty = true;
		}
	}
	else if (newval != class_form->reltuples)
	{
		class_form->reltuples = newval;
		if (class_dirty != NULL)
			*class_dirty = true;
	}
}

/*
 * Set the effective value of relallvisible for a relation.  For a global
 * temporary relation, GetGlobalTempRelationInfoFor[InPlace]Update() should
 * have been used to obtain gtr_info, and it will be updated instead of the
 * pg_class entry.  Otherwise, the value is set in the pg_class entry.
 *
 * If non-NULL, the class_dirty or gtr_dirty flag is set to true, if the value
 * in pg_class or gtr_info actually changes.
 */
static inline void
SetEffective_relallvisible(Form_pg_class class_form, GtrInfo *gtr_info,
						   int32 newval, bool *class_dirty, bool *gtr_dirty)
{
	if (gtr_info != NULL)
	{
		if (newval != gtr_info->relallvisible)
		{
			gtr_info->relallvisible = newval;
			if (gtr_dirty != NULL)
				*gtr_dirty = true;
		}
	}
	else if (newval != class_form->relallvisible)
	{
		class_form->relallvisible = newval;
		if (class_dirty != NULL)
			*class_dirty = true;
	}
}

/*
 * Set the effective value of relallfrozen for a relation.  For a global
 * temporary relation, GetGlobalTempRelationInfoFor[InPlace]Update() should
 * have been used to obtain gtr_info, and it will be updated instead of the
 * pg_class entry.  Otherwise, the value is set in the pg_class entry.
 *
 * If non-NULL, the class_dirty or gtr_dirty flag is set to true, if the value
 * in pg_class or gtr_info actually changes.
 */
static inline void
SetEffective_relallfrozen(Form_pg_class class_form, GtrInfo *gtr_info,
						  int32 newval, bool *class_dirty, bool *gtr_dirty)
{
	if (gtr_info != NULL)
	{
		if (newval != gtr_info->relallfrozen)
		{
			gtr_info->relallfrozen = newval;
			if (gtr_dirty != NULL)
				*gtr_dirty = true;
		}
	}
	else if (newval != class_form->relallfrozen)
	{
		class_form->relallfrozen = newval;
		if (class_dirty != NULL)
			*class_dirty = true;
	}
}

/*
 * Set the effective value of relfrozenxid for a relation.  For a global
 * temporary relation, GetGlobalTempRelationInfoFor[InPlace]Update() should
 * have been used to obtain gtr_info, and it will be updated instead of the
 * pg_class entry.  Otherwise, the value is set in the pg_class entry.
 *
 * If non-NULL, the class_dirty or gtr_dirty flag is set to true, if the value
 * in pg_class or gtr_info actually changes.
 */
static inline void
SetEffective_relfrozenxid(Form_pg_class class_form, GtrInfo *gtr_info,
						  TransactionId newval, bool *class_dirty, bool *gtr_dirty)
{
	if (gtr_info != NULL)
	{
		if (newval != gtr_info->relfrozenxid)
		{
			gtr_info->relfrozenxid = newval;
			if (gtr_dirty != NULL)
				*gtr_dirty = true;
		}
	}
	else if (newval != class_form->relfrozenxid)
	{
		class_form->relfrozenxid = newval;
		if (class_dirty != NULL)
			*class_dirty = true;
	}
}

/*
 * Set the effective value of relminmxid for a relation.  For a global
 * temporary relation, GetGlobalTempRelationInfoFor[InPlace]Update() should
 * have been used to obtain gtr_info, and it will be updated instead of the
 * pg_class entry.  Otherwise, the value is set in the pg_class entry.
 *
 * If non-NULL, the class_dirty or gtr_dirty flag is set to true, if the value
 * in pg_class or gtr_info actually changes.
 */
static inline void
SetEffective_relminmxid(Form_pg_class class_form, GtrInfo *gtr_info,
						MultiXactId newval, bool *class_dirty, bool *gtr_dirty)
{
	if (gtr_info != NULL)
	{
		if (newval != gtr_info->relminmxid)
		{
			gtr_info->relminmxid = newval;
			if (gtr_dirty != NULL)
				*gtr_dirty = true;
		}
	}
	else if (newval != class_form->relminmxid)
	{
		class_form->relminmxid = newval;
		if (class_dirty != NULL)
			*class_dirty = true;
	}
}

#endif							/* GLOBAL_TEMP_H */
