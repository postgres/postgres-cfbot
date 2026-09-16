/*-------------------------------------------------------------------------
 *
 * repack.h
 *	  header file for the REPACK command
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/repack.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPACK_H
#define REPACK_H

#include <signal.h>

#include "access/hio.h"
#include "access/skey.h"
#include "access/xlogdefs.h"
#include "catalog/index.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "storage/block.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/* flag bits for ClusterParams->options */
#define CLUOPT_VERBOSE 0x01		/* print progress info */
#define CLUOPT_RECHECK 0x02		/* recheck relation state */
#define CLUOPT_RECHECK_ISCLUSTERED 0x04 /* recheck relation state for
										 * indisclustered */
#define CLUOPT_ANALYZE 0x08		/* do an ANALYZE */
#define CLUOPT_CONCURRENT 0x10	/* allow concurrent data changes */

/* options for CLUSTER */
typedef struct ClusterParams
{
	uint32		options;		/* bitmask of CLUOPT_* */
} ClusterParams;

extern PGDLLIMPORT volatile sig_atomic_t RepackMessagePending;

/*
 * Table to apply concurrent data changes to.
 */
typedef struct RepackDest
{
	/* The relation the changes are applied to. */
	Relation	rel;

	BulkInsertStateData *bistate;

	/* Needed to update indexes of cc_rel. */
	ResultRelInfo *rri;
	EState	   *estate;

	/*
	 * Existing tuples to UPDATE and DELETE are located via this index. We
	 * keep the scankey in partially initialized state to avoid repeated work.
	 * sk_argument is completed on the fly.
	 */
	Relation	ident_index;
	ScanKey		ident_key;

	int			ident_key_nentries;

	/* The latest column we need to deform to have the tuple identity */
	AttrNumber	last_key_attno;

	/*
	 * Range of blocks in the old table the contents of this table comes from.
	 * Note that range_end is the first block of the next range.
	 */
	BlockNumber range_start;
	BlockNumber range_end;
} RepackDest;

/*
 * Information needed to apply concurrent data changes.
 *
 * XXX Now that it's in *.h file, rename to RepackChangeContext?
 */
typedef struct ChangeContext
{
	/* The destination table. */
	RepackDest	cc_dest;

	/* Sequential number of the file containing snapshot. */
	int			cc_file_seq_snapshot;
	/* Sequential number of the file containing data changes. */
	int			cc_file_seq_changes;

	/*
	 * Auxiliary table to store ordered tuples temporarily.
	 *
	 * When the new relation needs to be clustered, we use this table instead
	 * of tuplesort. The problem with a tuplesort is that data changes need to
	 * be applied at range boundary (see heapam_relation_copy_for_cluster()
	 * for more information), however it's not possible to look-up and change
	 * tuples in tuplestore.
	 *
	 * Once the contents of the REPACKed table has been copied into the
	 * auxiliary table, we build the clustering index (unless it's the same as
	 * the identity index) and scan it to get the tuple in the desired order.
	 * XXX Is it worth putting the contents into a tuplestore and sorting it?
	 * Not sure, it'd require disk space for one more copy and the copying
	 * itself is not free.
	 *
	 * TODO 1) make the tables unlogged, 2) if REPACK locks the TOAST relation
	 * too (not sure it does) try to preserve TOAST pointers, instead of
	 * storing them to TOAST relations of these tables, 3) Check that the
	 * tables are dropped on transaction abort.
	 */
	RepackDest *cc_dest_aux;

	/*
	 * The index that defines ordering of the old table.
	 */
	Oid			cc_clustering_index;

	/*
	 * Information needed to disable / enable security restrictions on index
	 * functions. This is needed when starting a new transaction.
	 */
	IndexBuildSecurity cc_ind_build_sec;
} ChangeContext;

extern PGDLLIMPORT int repack_pages_per_snapshot;

extern void ExecRepack(ParseState *pstate, RepackStmt *stmt, bool isTopLevel);

extern void cluster_rel(RepackCommand cmd, Relation OldHeap, Oid indexOid,
						ClusterParams *params, bool isTopLevel);
extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid,
									   LOCKMODE lockmode);
extern void mark_index_clustered(Relation rel, Oid indexOid, bool is_internal);

extern Oid	make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
						  char relpersistence, LOCKMODE lockmode,
						  bool auxiliary);
extern void heap_insert_for_repack(ChangeContext *chgcxt, TupleTableSlot *src,
								   TupleTableSlot *reform);
extern bool tuple_needs_reform(HeapTuple tuple, TupleDesc tupDesc);
extern void clear_dropped_attributes(HeapTuple tuple, TupleTableSlot *reform);
extern void finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
							 bool is_system_catalog,
							 bool swap_toast_by_content,
							 bool check_constraints,
							 bool is_internal,
							 bool reindex,
							 TransactionId frozenXid,
							 MultiXactId cutoffMulti,
							 char newrelpersistence);
extern Snapshot repack_get_snapshot(ChangeContext *chgcxt);
extern void repack_process_concurrent_changes(ChangeContext *chgcxt,
											  XLogRecPtr end_of_wal,
											  BlockNumber range_start,
											  BlockNumber range_end,
											  bool request_snapshot, bool done);
extern void HandleRepackMessageInterrupt(void);
extern void ProcessRepackMessages(void);

/* in repack_worker.c */
extern void RepackWorkerMain(Datum main_arg);
extern bool AmRepackWorker(void);

#endif							/* REPACK_H */
