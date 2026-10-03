/*-------------------------------------------------------------------------
 *
 * toast_internals.h
 *	  Internal definitions for the TOAST system.
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * src/include/access/toast_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOAST_INTERNALS_H
#define TOAST_INTERNALS_H

#include "access/htup_details.h"
#include "access/skey.h"
#include "storage/bufpage.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
#include "utils/rel.h"

#define RelationGetToastFlavour(relation) \
	((relation)->rd_options ? \
	 ((StdRdOptions *) (relation)->rd_options)->toast_flavour : TOAST_FLAVOUR_PLAIN)

#define RelationGetDirectToastSelfPrune(relation) \
	((relation)->rd_options ? \
	 (((StdRdOptions *) (relation)->rd_options)->direct_toast_self_prune != PG_TERNARY_UNSET ? \
	  (((StdRdOptions *) (relation)->rd_options)->direct_toast_self_prune == PG_TERNARY_TRUE) : direct_toast_self_prune) \
	 : direct_toast_self_prune)

/*
 * Return true if a TOAST page contains at least one deleted unindexed Direct
 * TOAST tuple (chunk_id IS NULL).  Callers must hold at least a shared buffer
 * lock on the page.
 */
static inline bool
toast_page_has_deleted_direct_tuple(Page page)
{
	OffsetNumber offnum,
				maxoff;

	if (!TransactionIdIsValid(PageGetPruneXid(page)))
		return false;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		HeapTupleHeader htup;

		if (!ItemIdIsNormal(itemid))
			continue;

		htup = (HeapTupleHeader) PageGetItem(page, itemid);
		if ((htup->t_infomask & HEAP_XMAX_INVALID) == 0 &&
			(htup->t_infomask & HEAP_HASNULL) != 0 &&
			att_isnull(0, htup->t_bits))
			return true;
	}

	return false;
}

typedef struct DirectToastPruneState
{
	Oid			relid;
	BlockNumber targblock;
	BlockNumber prune_hand;
	BlockNumber unprunable_probes;
	TransactionId last_probe_xid;
	uint64		delete_count;
	uint64		last_delete_count;
	bool		prune_exhausted;
	bool		saw_inprogress_delete;
} DirectToastPruneState;

extern PGDLLIMPORT int toast_default_flavour;
extern PGDLLIMPORT bool direct_toast_self_prune;

extern DirectToastPruneState *toast_get_prune_state(Oid relid);

extern Datum toast_compress_datum(Datum value, char cmethod);
extern Oid	toast_get_valid_index(Oid toastoid, LOCKMODE lock);

extern void toast_delete_datum(Relation rel, Datum value, bool is_speculative);
extern Datum toast_save_datum(Relation rel, Datum value,
							  varlena *oldexternal, uint32 options);

extern void toast_valueid_scankey_init(ScanKey entry, Oid toast_typid,
									   Oid8 valueid);

extern int	toast_open_indexes(Relation toastrel,
							   LOCKMODE lock,
							   Relation **toastidxs,
							   int *num_indexes);
extern void toast_close_indexes(Relation *toastidxs, int num_indexes,
								LOCKMODE lock);
extern Snapshot get_toast_snapshot(void);

#endif							/* TOAST_INTERNALS_H */
