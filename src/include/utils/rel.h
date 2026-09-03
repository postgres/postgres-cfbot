/*-------------------------------------------------------------------------
 *
 * rel.h
 *	  POSTGRES relation descriptor (a/k/a relcache entry) definitions.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REL_H
#define REL_H

#include "access/tupdesc.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_publication.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "partitioning/partdefs.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "utils/relcache.h"
#include "utils/reltrigger.h"
#include "utils/rel_internal.h"

/*
 * ForeignKeyCacheInfo
 *		Information the relcache can cache about foreign key constraints
 *
 * This is basically just an image of relevant columns from pg_constraint.
 * We make it a subclass of Node so that copyObject() can be used on a list
 * of these, but we also ensure it is a "flat" object without substructure,
 * so that list_free_deep() is sufficient to free such a list.
 * The per-FK-column arrays can be fixed-size because we allow at most
 * INDEX_MAX_KEYS columns in a foreign key constraint.
 *
 * Currently, we mostly cache fields of interest to the planner, but the set
 * of fields has already grown the constraint OID for other uses.
 */
typedef struct ForeignKeyCacheInfo
{
	pg_node_attr(no_equal, no_read, no_query_jumble)

	NodeTag		type;
	/* oid of the constraint itself */
	Oid			conoid;
	/* relation constrained by the foreign key */
	Oid			conrelid;
	/* relation referenced by the foreign key */
	Oid			confrelid;
	/* number of columns in the foreign key */
	int			nkeys;

	/* Is enforced ? */
	bool		conenforced;

	/*
	 * these arrays each have nkeys valid entries:
	 */
	/* cols in referencing table */
	AttrNumber	conkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* cols in referenced table */
	AttrNumber	confkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* PK = FK operator OIDs */
	Oid			conpfeqop[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
} ForeignKeyCacheInfo;


/*
 * StdRdOptions
 *		Standard contents of rd_options for heaps.
 *
 * RelationGetFillFactor() and RelationGetTargetPageFreeSpace() can only
 * be applied to relations that use this format or a superset for
 * private options data.
 */
 /* autovacuum-related reloptions. */
typedef struct AutoVacOpts
{
	pg_ternary	enabled;

	int			autovacuum_parallel_workers;
	int			vacuum_threshold;
	int			vacuum_max_threshold;
	int			vacuum_ins_threshold;
	int			analyze_threshold;
	int			vacuum_cost_limit;
	int			freeze_min_age;
	int			freeze_max_age;
	int			freeze_table_age;
	int			multixact_freeze_min_age;
	int			multixact_freeze_max_age;
	int			multixact_freeze_table_age;
	int			log_vacuum_min_duration;
	int			log_analyze_min_duration;
	float8		vacuum_cost_delay;
	float8		vacuum_scale_factor;
	float8		vacuum_ins_scale_factor;
	float8		analyze_scale_factor;
} AutoVacOpts;

/* StdRdOptions->vacuum_index_cleanup values */
typedef enum StdRdOptIndexCleanup
{
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO = 0,
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF,
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON,
	STDRD_OPTION_VACUUM_INDEX_CLEANUP_NOT_SET,
} StdRdOptIndexCleanup;

typedef struct StdRdOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
	int			toast_tuple_target; /* target for tuple toasting */
	AutoVacOpts autovacuum;		/* autovacuum-related options */
	bool		user_catalog_table; /* use as an additional catalog relation */
	int			parallel_workers;	/* max number of parallel workers */
	StdRdOptIndexCleanup vacuum_index_cleanup;	/* controls index vacuuming */
	pg_ternary	vacuum_truncate;	/* enables vacuum to truncate a relation */

	/*
	 * Fraction of pages in a relation that vacuum can eagerly scan and fail
	 * to freeze. 0 if disabled, -1 if unspecified.
	 */
	double		vacuum_max_eager_freeze_failure_rate;
} StdRdOptions;

#define HEAP_MIN_FILLFACTOR			10
#define HEAP_DEFAULT_FILLFACTOR		100

/*
 * RelationGetToastTupleTarget
 *		Returns the relation's toast_tuple_target.  Note multiple eval of argument!
 */
#define RelationGetToastTupleTarget(relation, defaulttarg) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->toast_tuple_target : (defaulttarg))

/*
 * RelationGetFillFactor
 *		Returns the relation's fillfactor.  Note multiple eval of argument!
 */
#define RelationGetFillFactor(relation, defaultff) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->fillfactor : (defaultff))

/*
 * RelationGetTargetPageUsage
 *		Returns the relation's desired space usage per page in bytes.
 */
#define RelationGetTargetPageUsage(relation, defaultff) \
	(BLCKSZ * RelationGetFillFactor(relation, defaultff) / 100)

/*
 * RelationGetTargetPageFreeSpace
 *		Returns the relation's desired freespace per page in bytes.
 */
#define RelationGetTargetPageFreeSpace(relation, defaultff) \
	(BLCKSZ * (100 - RelationGetFillFactor(relation, defaultff)) / 100)

/*
 * RelationIsUsedAsCatalogTable
 *		Returns whether the relation should be treated as a catalog table
 *		from the pov of logical decoding.  Note multiple eval of argument!
 */
#define RelationIsUsedAsCatalogTable(relation)	\
	((relation)->rd_options && \
	 ((relation)->rd_rel->relkind == RELKIND_RELATION || \
	  (relation)->rd_rel->relkind == RELKIND_MATVIEW) ? \
	 ((StdRdOptions *) (relation)->rd_options)->user_catalog_table : false)

/*
 * RelationGetParallelWorkers
 *		Returns the relation's parallel_workers reloption setting.
 *		Note multiple eval of argument!
 */
#define RelationGetParallelWorkers(relation, defaultpw) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->parallel_workers : (defaultpw))

/* ViewOptions->check_option values */
typedef enum ViewOptCheckOption
{
	VIEW_OPTION_CHECK_OPTION_NOT_SET,
	VIEW_OPTION_CHECK_OPTION_LOCAL,
	VIEW_OPTION_CHECK_OPTION_CASCADED,
} ViewOptCheckOption;

/*
 * ViewOptions
 *		Contents of rd_options for views
 */
typedef struct ViewOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	bool		security_barrier;
	bool		security_invoker;
	ViewOptCheckOption check_option;
} ViewOptions;

/*
 * RelationIsSecurityView
 *		Returns whether the relation is security view, or not.  Note multiple
 *		eval of argument!
 */
#define RelationIsSecurityView(relation)									\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options ?												\
	  ((ViewOptions *) (relation)->rd_options)->security_barrier : false)

/*
 * RelationHasSecurityInvoker
 *		Returns true if the relation has the security_invoker property set.
 *		Note multiple eval of argument!
 */
#define RelationHasSecurityInvoker(relation)								\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options ?												\
	  ((ViewOptions *) (relation)->rd_options)->security_invoker : false)

/*
 * RelationHasCheckOption
 *		Returns true if the relation is a view defined with either the local
 *		or the cascaded check option.  Note multiple eval of argument!
 */
#define RelationHasCheckOption(relation)									\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option !=				\
	 VIEW_OPTION_CHECK_OPTION_NOT_SET)

/*
 * RelationHasLocalCheckOption
 *		Returns true if the relation is a view defined with the local check
 *		option.  Note multiple eval of argument!
 */
#define RelationHasLocalCheckOption(relation)								\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option ==				\
	 VIEW_OPTION_CHECK_OPTION_LOCAL)

/*
 * RelationHasCascadedCheckOption
 *		Returns true if the relation is a view defined with the cascaded check
 *		option.  Note multiple eval of argument!
 */
#define RelationHasCascadedCheckOption(relation)							\
	(AssertMacro(relation->rd_rel->relkind == RELKIND_VIEW),				\
	 (relation)->rd_options &&												\
	 ((ViewOptions *) (relation)->rd_options)->check_option ==				\
	  VIEW_OPTION_CHECK_OPTION_CASCADED)

/*
 * RelationIsValid
 *		True iff relation descriptor is valid.
 */
#define RelationIsValid(relation) ((relation) != NULL)

/*
 * RelationHasReferenceCountZero
 *		True iff relation reference count is zero.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationHasReferenceCountZero(relation) \
		((bool)((relation)->rd_refcnt == 0))

/*
 * RelationGetForm
 *		Returns pg_class tuple for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationGetForm(relation) ((relation)->rd_rel)

/*
 * RelationGetRelid
 *		Returns the OID of the relation
 */
#define RelationGetRelid(relation) ((relation)->rd_id)

/*
 * RelationGetLockRelId
 *		Returns the LockRelId to use for locking the relation.
 *
 * This used to be a field cached in the relation descriptor
 * (rd_lockInfo.lockRelId), populated once by RelationInitLockInfo().  It's
 * cheap enough to compute on the fly instead, which lets us avoid storing
 * it.  Note that we can't derive dbId from rd_locator.dbOid: relation kinds
 * without storage (e.g. partitioned tables/indexes, views) never have
 * rd_locator populated, so we replicate the original relisshared-based
 * computation instead.
 */
static inline LockRelId
RelationGetLockRelId(Relation relation)
{
	LockRelId	lockRelId;

	lockRelId.relId = RelationGetRelid(relation);
	lockRelId.dbId = relation->rd_rel->relisshared ? InvalidOid : MyDatabaseId;
	return lockRelId;
}

/*
 * RelationGetNumberOfAttributes
 *		Returns the total number of attributes in a relation.
 */
#define RelationGetNumberOfAttributes(relation) ((relation)->rd_rel->relnatts)

/*
 * IndexRelationGetNumberOfAttributes
 *		Returns the number of attributes in an index.
 */
#define IndexRelationGetNumberOfAttributes(relation) \
		((relation)->rd_index->indnatts)

/*
 * IndexRelationGetNumberOfKeyAttributes
 *		Returns the number of key attributes in an index.
 */
#define IndexRelationGetNumberOfKeyAttributes(relation) \
		((relation)->rd_index->indnkeyatts)

/*
 * RelationGetDescr
 *		Returns tuple descriptor for a relation.
 */
#define RelationGetDescr(relation) ((relation)->rd_att)

/*
 * RelationGetRelationName
 *		Returns the rel's name.
 *
 * Note that the name is only unique within the containing namespace.
 */
#define RelationGetRelationName(relation) \
	(NameStr((relation)->rd_rel->relname))

/*
 * RelationGetNamespace
 *		Returns the rel's namespace OID.
 */
#define RelationGetNamespace(relation) \
	((relation)->rd_rel->relnamespace)

/*
 * RelationIsMapped
 *		True if the relation uses the relfilenumber map.  Note multiple eval
 *		of argument!
 */
#define RelationIsMapped(relation) \
	(RELKIND_HAS_STORAGE((relation)->rd_rel->relkind) && \
	 ((relation)->rd_rel->relfilenode == InvalidRelFileNumber))

#ifndef FRONTEND
/*
 * RelationGetSmgr
 *		Returns smgr file handle for a relation, opening it if needed.
 *
 * Very little code is authorized to touch rel->rd_smgr directly.  Instead
 * use this function to fetch its value.
 */
static inline SMgrRelation
RelationGetSmgr(Relation rel)
{
	if (unlikely(rel->rd_smgr == NULL))
	{
		rel->rd_smgr = smgropen(rel->rd_locator, rel->rd_backend);
		smgrpin(rel->rd_smgr);
	}
	return rel->rd_smgr;
}

/*
 * RelationCloseSmgr
 *		Close the relation at the smgr level, if not already done.
 */
static inline void
RelationCloseSmgr(Relation relation)
{
	if (relation->rd_smgr != NULL)
	{
		smgrunpin(relation->rd_smgr);
		smgrclose(relation->rd_smgr);
		relation->rd_smgr = NULL;
	}
}
#endif							/* !FRONTEND */

/*
 * RelationGetTargetBlock
 *		Fetch relation's current insertion target block.
 *
 * Returns InvalidBlockNumber if there is no current target block.  Note
 * that the target block status is discarded on any smgr-level invalidation,
 * so there's no need to re-open the smgr handle if it's not currently open.
 */
#define RelationGetTargetBlock(relation) \
	( (relation)->rd_smgr != NULL ? (relation)->rd_smgr->smgr_targblock : InvalidBlockNumber )

/*
 * RelationSetTargetBlock
 *		Set relation's current insertion target block.
 */
#define RelationSetTargetBlock(relation, targblock) \
	do { \
		RelationGetSmgr(relation)->smgr_targblock = (targblock); \
	} while (0)

/*
 * RelationIsPermanent
 *		True if relation is permanent.
 */
#define RelationIsPermanent(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT)

/*
 * RelationNeedsWAL
 *		True if relation needs WAL.
 *
 * Returns false if wal_level = minimal and this relation is created or
 * truncated in the current transaction.  See "Skipping WAL for New
 * RelFileLocator" in src/backend/access/transam/README.
 */
#define RelationNeedsWAL(relation)										\
	(RelationIsPermanent(relation) && (XLogIsNeeded() ||				\
	  (relation->rd_createSubid == InvalidSubTransactionId &&			\
	   relation->rd_firstRelfilelocatorSubid == InvalidSubTransactionId)))

/*
 * RelationUsesLocalBuffers
 *		True if relation's pages are stored in local buffers.
 */
#define RelationUsesLocalBuffers(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP)

/*
 * RELATION_IS_LOCAL
 *		If a rel is either temp or newly created in the current transaction,
 *		it can be assumed to be accessible only to the current backend.
 *		This is typically used to decide that we can skip acquiring locks.
 *
 * Beware of multiple eval of argument
 */
#define RELATION_IS_LOCAL(relation) \
	((relation)->rd_islocaltemp || \
	 (relation)->rd_createSubid != InvalidSubTransactionId)

/*
 * RELATION_IS_OTHER_TEMP
 *		Test for a temporary relation that belongs to some other session.
 *
 * Reading another session's temp-table data through never works right:
 * the owning session keeps the data in its private local buffer pool,
 * which we cannot access.  Existing buffer-manager entry points
 * (ReadBuffer_common(), StartReadBuffersImpl(), read_stream_begin_impl(),
 * PrefetchBuffer() and ExtendBufferedRelCommon()) already enforce this; any
 * new buffer-access entry points must do the same.  Command-level code
 * (TRUNCATE, ALTER TABLE, VACUUM, CLUSTER, REINDEX, ...) additionally uses
 * this macro for command-specific error messages.
 *
 * Beware of multiple eval of argument
 */
#define RELATION_IS_OTHER_TEMP(relation) \
	((relation)->rd_rel->relpersistence == RELPERSISTENCE_TEMP && \
	 !(relation)->rd_islocaltemp)


/*
 * RelationIsScannable
 *		Currently can only be false for a materialized view which has not been
 *		populated by its query.  This is likely to get more complicated later,
 *		so use a macro which looks like a function.
 */
#define RelationIsScannable(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsPopulated
 *		Currently, we don't physically distinguish the "populated" and
 *		"scannable" properties of matviews, but that may change later.
 *		Hence, use the appropriate one of these macros in code tests.
 */
#define RelationIsPopulated(relation) ((relation)->rd_rel->relispopulated)

/*
 * RelationIsAccessibleInLogicalDecoding
 *		True if we need to log enough information to have access via
 *		decoding snapshot.
 */
#define RelationIsAccessibleInLogicalDecoding(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 (IsCatalogRelation(relation) || RelationIsUsedAsCatalogTable(relation)))

/*
 * RelationIsLogicallyLogged
 *		True if we need to log enough information to extract the data from the
 *		WAL stream.
 *
 * We don't log information for unlogged tables (since they don't WAL log
 * anyway), for foreign tables (since they don't WAL log, either),
 * and for system tables (their content is hard to make sense of, and
 * it would complicate decoding slightly for little gain). Note that we *do*
 * log information for user defined catalog tables since they presumably are
 * interesting to the user...
 */
#define RelationIsLogicallyLogged(relation) \
	(XLogLogicalInfoActive() && \
	 RelationNeedsWAL(relation) && \
	 (relation)->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&	\
	 !IsCatalogRelation(relation))

/* routines in utils/cache/relcache.c */
extern void RelationIncrementReferenceCount(Relation rel);
extern void RelationDecrementReferenceCount(Relation rel);

#endif							/* REL_H */
