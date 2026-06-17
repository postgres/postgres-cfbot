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

extern void TrackGlobalTempRelationStorage(Oid relid, RelFileLocator rlocator,
										   ProcNumber backend, bool create,
										   bool register_delete);
extern void ReassignGlobalTempRelationStorage(RelFileLocator rlocator,
											  Oid newRelid);
extern void InitGlobalTempRelation(Relation relation);
extern void TrackGlobalTempRelation(Relation relation);
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

#endif							/* GLOBAL_TEMP_H */
