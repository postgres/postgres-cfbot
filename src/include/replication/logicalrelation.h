/*-------------------------------------------------------------------------
 *
 * logicalrelation.h
 *	  Relation definitions for logical replication relation mapping.
 *
 * Portions Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalrelation.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALRELATION_H
#define LOGICALRELATION_H

#include "access/attmap.h"
#include "catalog/index.h"
#include "replication/logicalproto.h"

typedef enum LogicalRepParallelAction
{
	LRPA_INSERT,
	LRPA_UPDATE,
	LRPA_DELETE,
	LRPA_TRUNCATE
} LogicalRepParallelAction;

#define LRPA_ACTION_COUNT (LRPA_TRUNCATE + 1)

typedef struct LogicalRepRelMapEntry
{
	LogicalRepRelation remoterel;	/* key is remoterel.remoteid */

	/*
	 * Validity flag -- when false, revalidate all derived info at next
	 * logicalrep_rel_open.  (While the localrel is open, we assume our lock
	 * on that rel ensures the info remains good.)
	 */
	bool		localrelvalid;

	/* Mapping to local relation. */
	Oid			localreloid;	/* local relation id */
	Relation	localrel;		/* relcache entry (NULL when closed) */
	AttrMap    *attrmap;		/* map of local attributes to remote ones */
	bool		updatable;		/* Can apply updates/deletes? */
	Oid			localindexoid;	/* which index to use, or InvalidOid if none */

	/* Sync state. */
	char		state;
	XLogRecPtr	statelsn;

	/*
	 * The last remote transaction that modified the relation's schema or
	 * truncated the relation. Used in dependency tracking to ensure subsequent
	 * transactions modifying the same table wait for this transaction to finish
	 * applying (see check_dependency_on_rel).
	 */
	TransactionId last_depended_xid;

	/*
	 * Per-operation safety cache for parallel apply. If
	 * parallel_global_unsafe[action] is true, that action cannot be applied in
	 * parallel for this relation.
	 *
	 * This cache must be computed by the worker that actually applies changes
	 * (via logicalrep_rel_check_parallel_safety), rather than solely by the
	 * leader.
	 *
	 * Relying on the leader alone is unsafe because the table could be altered
	 * before changes are dispatched to a parallel worker. Without re-validation
	 * by the parallel worker, it might incorrectly assume that parallel apply
	 * is safe for this relation.
	 */
	bool		parallel_safety_valid;
	bool		parallel_global_unsafe[LRPA_ACTION_COUNT];
} LogicalRepRelMapEntry;

extern void logicalrep_relmap_update(LogicalRepRelation *remoterel);
extern void logicalrep_partmap_reset_relmap(LogicalRepRelation *remoterel);

extern LogicalRepRelMapEntry *logicalrep_rel_open(LogicalRepRelId remoteid,
												  LOCKMODE lockmode);
extern LogicalRepRelMapEntry *logicalrep_partition_open(LogicalRepRelMapEntry *root,
														Relation partrel, AttrMap *map);
extern void logicalrep_rel_close(LogicalRepRelMapEntry *rel,
								 LOCKMODE lockmode);
extern void logicalrep_rel_check_parallel_safety(LogicalRepRelMapEntry *entry);
extern bool IsIndexUsableForReplicaIdentityFull(Relation idxrel, AttrMap *attrmap);
extern Oid	GetRelationIdentityOrPK(Relation rel);
extern void logicalrep_write_all_internal_rels(StringInfo out, int *num_rels);
extern void logicalrep_write_one_internal_rel(StringInfo out, LogicalRepRelation *rel);
extern LogicalRepRelMapEntry *logicalrep_get_relentry(LogicalRepRelId remoteid);

#endif							/* LOGICALRELATION_H */
