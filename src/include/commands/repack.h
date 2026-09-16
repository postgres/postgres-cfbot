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
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"


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
} RepackDest;

/*
 * The first file exported by the decoding worker must contain a snapshot, the
 * following ones contain the data changes.
 */
#define WORKER_FILE_SNAPSHOT	0

/*
 * Information needed to apply concurrent data changes.
 *
 * XXX Now that it's in *.h file, rename to RepackChangeContext?
 */
typedef struct ChangeContext
{
	/* The destination table. */
	RepackDest	cc_dest;

	/* Sequential number of the file containing the changes. */
	int			cc_file_seq;
} ChangeContext;

extern void ExecRepack(ParseState *pstate, RepackStmt *stmt, bool isTopLevel);

extern void cluster_rel(RepackCommand cmd, Relation OldHeap, Oid indexOid,
						ClusterParams *params, bool isTopLevel);
extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid,
									   LOCKMODE lockmode);
extern void mark_index_clustered(Relation rel, Oid indexOid, bool is_internal);

extern Oid	make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
						  char relpersistence, LOCKMODE lockmode);
extern void heap_insert_for_repack(Relation rel, TupleTableSlot *src,
								   TupleTableSlot *reform,
								   BulkInsertStateData *bistate);
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

extern void HandleRepackMessageInterrupt(void);
extern void ProcessRepackMessages(void);

/* in repack_worker.c */
extern void RepackWorkerMain(Datum main_arg);
extern bool AmRepackWorker(void);

#endif							/* REPACK_H */
