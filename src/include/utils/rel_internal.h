#ifndef REL_INTERNAL_H
#define REL_INTERNAL_H

#include "access/tupdesc.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_publication.h"
#include "nodes/bitmapset.h"
#include "partitioning/partdefs.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "utils/relcache.h"
#include "utils/reltrigger.h"

/*
 * LockRelId and LockInfo really belong to lmgr.h, but it's more convenient
 * to declare them here so we can have a LockInfoData field in a Relation.
 */

typedef struct LockRelId
{
	Oid			relId;			/* a relation identifier */
	Oid			dbId;			/* a database identifier */
} LockRelId;

typedef struct LockInfoData
{
	LockRelId	lockRelId;
} LockInfoData;

typedef LockInfoData *LockInfo;

/*
 * Here are the contents of a relation cache entry.
 */

/*
 * Field order within RelationData (and within each branch of the union
 * further down) is chosen to minimize struct padding: pointer-sized
 * (8-byte) fields are grouped first, followed by 4-byte fields, followed
 * by 1-byte bool fields.  Don't reorder fields without re-checking that
 * this packing is preserved.
 */
typedef struct RelationData
{
	SMgrRelation rd_smgr;		/* cached file handle, or NULL */
	Form_pg_class rd_rel;		/* RELATION tuple */
	TupleDesc	rd_att;			/* tuple descriptor */

	RelFileLocator rd_locator;	/* relation physical identifier */
	int			rd_refcnt;		/* reference count */
	ProcNumber	rd_backend;		/* owning backend's proc number, if temp rel */

	/*----------
	 * rd_createSubid is the ID of the highest subtransaction the rel has
	 * survived into or zero if the rel or its storage was created before the
	 * current top transaction.  (IndexStmt.oldNumber leads to the case of a new
	 * rel with an old rd_locator.)  rd_firstRelfilelocatorSubid is the ID of the
	 * highest subtransaction an rd_locator change has survived into or zero if
	 * rd_locator matches the value it had at the start of the current top
	 * transaction.  (Rolling back the subtransaction that
	 * rd_firstRelfilelocatorSubid denotes would restore rd_locator to the value it
	 * had at the start of the current top transaction.  Rolling back any
	 * lower subtransaction would not.)  Their accuracy is critical to
	 * RelationNeedsWAL().
	 *
	 * rd_newRelfilelocatorSubid is the ID of the highest subtransaction the
	 * most-recent relfilenumber change has survived into or zero if not changed
	 * in the current transaction (or we have forgotten changing it).  This
	 * field is accurate when non-zero, but it can be zero when a relation has
	 * multiple new relfilenumbers within a single transaction, with one of them
	 * occurring in a subsequently aborted subtransaction, e.g.
	 *		BEGIN;
	 *		TRUNCATE t;
	 *		SAVEPOINT save;
	 *		TRUNCATE t;
	 *		ROLLBACK TO save;
	 *		-- rd_newRelfilelocatorSubid is now forgotten
	 *
	 * If every rd_*Subid field is zero, they are read-only outside
	 * relcache.c.  Files that trigger rd_locator changes by updating
	 * pg_class.reltablespace and/or pg_class.relfilenode call
	 * RelationAssumeNewRelfilelocator() to update rd_*Subid.
	 *
	 * rd_droppedSubid is the ID of the highest subtransaction that a drop of
	 * the rel has survived into.  In entries visible outside relcache.c, this
	 * is always zero.
	 */
	SubTransactionId rd_createSubid;	/* rel was created in current xact */
	SubTransactionId rd_newRelfilelocatorSubid; /* highest subxact changing
												 * rd_locator to current value */
	SubTransactionId rd_firstRelfilelocatorSubid;	/* highest subxact
													 * changing rd_locator to
													 * any value */
	SubTransactionId rd_droppedSubid;	/* dropped with another Subid set */

	Oid			rd_id;			/* relation's object id */
	LockInfoData rd_lockInfo;	/* lock mgr's info for locking relation */

	bool		rd_islocaltemp; /* rel is a temp rel of this session */
	bool		rd_isnailed;	/* rel is nailed in cache */
	bool		rd_isvalid;		/* relcache entry is valid */
	bool		rd_indexvalid;	/* is rd_indexlist valid? (also rd_pkindex and
								 * rd_replidindex) */
	bool		rd_statvalid;	/* is rd_statlist valid? */
	bool		rd_ispkdeferrable;	/* is rd_pkindex a deferrable PK? */
	bool		rd_attrsvalid;	/* are bitmaps of attrs valid? (see
								 * RelationGetIndexAttrBitmap) */
	bool		pgstat_enabled; /* should relation stats be counted */

	/*
	 * The following two blocks of fields are mutually exclusive: a
	 * relation is never both an index and a non-index (table-like)
	 * relation at the same time.  Overlapping them in a union avoids
	 * paying for both sets of fields in every relcache entry, which adds
	 * up across installations with large numbers of tables and indexes.
	 * Anonymous struct/union members are used so that every field below
	 * remains directly accessible as "relation->rd_xxx", exactly as if it
	 * were not inside a union.
	 *
	 * NOTE: RelationDestroyRelation() in relcache.c must only clean up the
	 * branch of this union that matches the relation's actual relkind; the
	 * fields of the other branch alias this one's memory and must not be
	 * dereferenced.
	 */
	union
	{
		/* fields used only for a non-index (table-like) relation */
		struct
		{
			RuleLock   *rd_rules;		/* rewrite rules */
			MemoryContext rd_rulescxt;	/* private memory cxt for rd_rules, if any */
			TriggerDesc *trigdesc;		/* Trigger info, or NULL if rel has none */
			/* use "struct" here to avoid needing to include rowsecurity.h: */
			struct RowSecurityDesc *rd_rsdesc;	/* row security policies, or NULL */

			/* data managed by RelationGetFKeyList: */
			List	   *rd_fkeylist;	/* list of ForeignKeyCacheInfo (see below) */

			/* data managed by RelationGetPartitionKey: */
			PartitionKey rd_partkey;	/* partition key, or NULL */
			MemoryContext rd_partkeycxt;	/* private context for rd_partkey, if any */

			/* data managed by RelationGetPartitionDesc: */
			PartitionDesc rd_partdesc;	/* partition descriptor, or NULL */
			MemoryContext rd_pdcxt;		/* private context for rd_partdesc, if any */

			/* Same as above, for partdescs that omit detached partitions */
			PartitionDesc rd_partdesc_nodetached;	/* partdesc w/o detached parts */
			MemoryContext rd_pddcxt;	/* for rd_partdesc_nodetached, if any */

			/* data managed by RelationGetPartitionQual: */
			List	   *rd_partcheck;	/* partition CHECK quals */
			MemoryContext rd_partcheckcxt;	/* private cxt for rd_partcheck, if any */

			/* data managed by RelationGetIndexAttrBitmap: */
			Bitmapset  *rd_keyattr;		/* cols that can be ref'd by foreign keys */
			Bitmapset  *rd_pkattr;		/* cols included in primary key */
			Bitmapset  *rd_idattr;		/* included in replica identity index */
			Bitmapset  *rd_hotblockingattr; /* cols blocking HOT update */
			Bitmapset  *rd_summarizedattr;	/* cols indexed by summarizing indexes */

			PublicationDesc *rd_pubdesc;	/* publication descriptor, or NULL */

			/*
			 * foreign-table support
			 *
			 * rd_fdwroutine must point to a single memory chunk palloc'd in
			 * CacheMemoryContext.  It will be freed and reset to NULL on a
			 * relcache reset.
			 */
			/* use "struct" here to avoid needing to include fdwapi.h: */
			struct FdwRoutine *rd_fdwroutine;	/* cached function pointers, or NULL */

			/*
			 * pg_inherits.xmin of the partition that was excluded in
			 * rd_partdesc_nodetached.  This informs a future user of that partdesc:
			 * if this value is not in progress for the active snapshot, then the
			 * partdesc can be used, otherwise they have to build a new one.  (This
			 * matches what find_inheritance_children_extended would do).
			 */
			TransactionId rd_partdesc_nodetached_xmin;

			bool		rd_fkeyvalid;	/* true if rd_fkeylist has been computed */
			bool		rd_partcheckvalid;	/* true if rd_partcheck has been computed */
		};

		/* fields used only for an index relation */
		struct
		{
			Form_pg_index rd_index;		/* pg_index tuple describing this index */
			/* use "struct" here to avoid needing to include htup.h: */
			struct HeapTupleData *rd_indextuple;	/* all of pg_index tuple */

			/*
			 * index access support info (used only for an index relation)
			 *
			 * Note: only default support procs for each opclass are cached, namely
			 * those with lefttype and righttype equal to the opclass's opcintype. The
			 * arrays are indexed by support function number, which is a sufficient
			 * identifier given that restriction.
			 */
			MemoryContext rd_indexcxt;	/* private memory cxt for this stuff */
			/* use "struct" here to avoid needing to include amapi.h: */
			const struct IndexAmRoutine *rd_indam;	/* index AM's API struct */
			Oid		   *rd_opfamily;	/* OIDs of op families for each index col */
			Oid		   *rd_opcintype;	/* OIDs of opclass declared input data types */
			RegProcedure *rd_support;	/* OIDs of support procedures */
			struct FmgrInfo *rd_supportinfo;	/* lookup info for support procedures */
			int16	   *rd_indoption;	/* per-column AM-specific flags */
			List	   *rd_indexprs;	/* index expression trees, if any */
			List	   *rd_indpred;		/* index predicate tree, if any */
			Oid		   *rd_exclops;		/* OIDs of exclusion operators, if any */
			Oid		   *rd_exclprocs;	/* OIDs of exclusion ops' procs, if any */
			uint16	   *rd_exclstrats;	/* exclusion ops' strategy numbers, if any */
			Oid		   *rd_indcollation;	/* OIDs of index collations */
			bytea	  **rd_opcoptions;	/* parsed opclass-specific options */
		};
	};

	/* data managed by RelationGetIndexList: */
	List	   *rd_indexlist;	/* list of OIDs of indexes on relation */

	/* data managed by RelationGetStatExtList: */
	List	   *rd_statlist;	/* list of OIDs of extended stats */

	/*
	 * rd_options is set whenever rd_rel is loaded into the relcache entry.
	 * Note that you can NOT look into rd_rel for this data.  NULL means "use
	 * defaults".
	 */
	bytea	   *rd_options;		/* parsed pg_class.reloptions */

	/*
	 * Table access method.
	 */
	const struct TableAmRoutine *rd_tableam;

	/*
	 * rd_amcache is available for index and table AMs to cache private data
	 * about the relation.  This must be just a cache since it may get reset
	 * at any time (in particular, it will get reset by a relcache inval
	 * message for the relation).  If used, it must point to a single memory
	 * chunk palloc'd in CacheMemoryContext, or in rd_indexcxt for an index
	 * relation.  A relcache reset will include freeing that chunk and setting
	 * rd_amcache = NULL.
	 */
	void	   *rd_amcache;		/* available for use by index/table AM */

	/* use "struct" here to avoid needing to include pgstat.h: */
	struct PgStat_RelationStatus *pgstat_info;	/* statistics collection area */

	Oid			rd_pkindex;		/* OID of (deferrable?) primary key, if any */
	Oid			rd_replidindex; /* OID of replica identity index, if any */

	/*
	 * Oid of the handler for this relation. For an index this is a function
	 * returning IndexAmRoutine, for table like relations a function returning
	 * TableAmRoutine.  This is stored separately from rd_indam, rd_tableam as
	 * its lookup requires syscache access, but during relcache bootstrap we
	 * need to be able to initialize rd_tableam without syscache lookups.
	 */
	Oid			rd_amhandler;	/* OID of index AM's handler function */

	/*
	 * Hack for CLUSTER, rewriting ALTER TABLE, etc: when writing a new
	 * version of a table, we need to make any toast pointers inserted into it
	 * have the existing toast table's OID, not the OID of the transient toast
	 * table.  If rd_toastoid isn't InvalidOid, it is the OID to place in
	 * toast pointers inserted into this rel.  (Note it's set on the new
	 * version of the main heap, not the toast table itself.)  This also
	 * causes toast_save_datum() to try to preserve toast value OIDs.
	 */
	Oid			rd_toastoid;	/* Real TOAST table's OID, or InvalidOid */
} RelationData;

#endif							/* REL_INTERNAL_H */
