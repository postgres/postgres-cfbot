/*-------------------------------------------------------------------------
 *
 * procarrayfuncs.c
 *	  SQL-callable functions exposing ProcArray-derived diagnostics.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procarrayfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "replication/slot.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

/*
 * One gathered row, with the per-class xmin values and the columns that
 * identify it.  Fields that may be SQL NULL use a sentinel: 0 for pid,
 * empty NameData for slot_name, InvalidOid for datid, and
 * InvalidTransactionId for shared/catalog/data.
 */
typedef struct XidHorizonRow
{
	Datum		kind;			/* text Datum (never NULL) */
	int			pid;			/* backend / walsender PID; 0 = NULL */
	NameData	slot_name;		/* replication slot name; empty = NULL */
	Oid			datid;			/* InvalidOid = NULL */
	TransactionId shared;		/* per-class xmin values, InvalidTransactionId
								 * = NULL */
	TransactionId catalog;
	TransactionId data;
} XidHorizonRow;

/*
 * pg_get_xmin_horizon - SQL SRF reporting, per row, the raw per-class xmin
 * contributions to the cluster's xmin horizons.
 *
 * Emits one row per in-use replication slot, prepared transaction, slot-less
 * hot-standby-feedback walsender, and backend that holds a snapshot.
 */
Datum
pg_get_xmin_horizon(PG_FUNCTION_ARGS)
{
#define PG_GET_XMIN_HORIZON_COLS 7
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	XidHorizonProc *procs;
	int			nprocs;
	XidHorizonRow *rows;
	int			nrows;
	int			maxrows;

	/*
	 * On a hot standby, KnownAssignedXids carries the primary's running xacts
	 * and is folded into the standby's data horizon by GetSnapshotData() but
	 * is not surfaced as procarray rows.  Error instead of reporting
	 * incomplete data, which might be mistakenly interpreted as complete.
	 */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("pg_get_xmin_horizon() cannot be executed during recovery.")));

	InitMaterializedSRF(fcinfo, 0);

	/* MaxBackends covers walsenders (standby_feedback rows) too. */
	maxrows = MaxBackends + max_prepared_xacts +
		max_replication_slots + max_repack_replication_slots;
	rows = palloc_array(XidHorizonRow, maxrows);
	nrows = 0;

	procs = GetXidHorizonProcs(&nprocs);

	for (int i = 0; i < nprocs; i++)
	{
		XidHorizonProc *p = &procs[i];
		TransactionId effective_xmin;
		TransactionId shared;
		TransactionId catalog;
		TransactionId data;
		int			pid;
		XidHorizonRow *r;

		/*
		 * Within a backend xmin is set before xid is assigned, so the only
		 * xmin > xid window is the cross-backend read race; see
		 * ComputeXidHorizons().  A backend with neither set holds no snapshot
		 * and does not influence the horizon, so skip it.
		 */
		effective_xmin = TransactionIdOlder(p->xid, p->xmin);
		if (!TransactionIdIsValid(effective_xmin))
			continue;

		/*
		 * Classify this proc's per-class xmin.  VACUUM backends do not pin
		 * xmin, and logical decoding backends pin xmin indirectly through
		 * slots.  Emit them with NULLs so the view lists them without
		 * claiming they pin.  Every other proc pins all three classes.
		 * VISHORIZON_TEMP is omitted because the temp horizon is per-backend
		 * and has no cross-backend diagnostic value.  Keep in sync with
		 * ComputeXidHorizons() and GetSnapshotData().
		 */
		if (p->statusFlags & (PROC_IN_VACUUM | PROC_IN_LOGICAL_DECODING))
			shared = catalog = data = InvalidTransactionId;
		else
			shared = catalog = data = effective_xmin;

		Assert(nrows < maxrows);
		r = &rows[nrows++];
		memset(r, 0, sizeof(*r));
		r->datid = p->databaseId;

		pid = p->pid;
		if (pid == 0)
		{
			/*
			 * A prepared xact's dummy PGPROC never carries an xmin (see
			 * MarkAsPreparingGuts()), so this row's per-class values are
			 * always its xid.  The pg_xmin_horizon view's join to
			 * pg_prepared_xacts relies on that to attach the gid.
			 */
			r->kind = CStringGetTextDatum("prepared_xact");
		}
		else if (p->statusFlags & PROC_AFFECTS_ALL_HORIZONS)
		{
			r->kind = CStringGetTextDatum("standby_feedback");
			r->pid = pid;
			r->datid = InvalidOid;
		}
		else
		{
			r->kind = CStringGetTextDatum("backend");
			r->pid = pid;
		}

		r->shared = shared;
		r->catalog = catalog;
		r->data = data;
	}

	pfree(procs);

	/*
	 * Two-phase gather: ProcArrayLock has been released inside
	 * GetXidHorizonProcs() before we acquire ReplicationSlotControlLock here.
	 * Do not "fix" this by holding both locks at once.  The lock-ordering
	 * convention in this subsystem takes ReplicationSlotControlLock without
	 * ProcArrayLock held; pg_get_replication_slots() and
	 * ReplicationSlotsComputeRequiredXmin() both follow it.  Holding
	 * ProcArrayLock across that acquisition would establish a new
	 * lock-ordering and risk deadlocks.
	 *
	 * This view does not guarantee cross-phase consistency: each row reports
	 * a per-source xmin observed under its own lock.
	 *
	 * The effective aggregate contribution of all slots to the xmin used by
	 * vacuum may be temporarily older than any individual slot xmin reported
	 * by this view.  See the comment in
	 * ReplicationSlotsComputeRequiredXmin().
	 */

	/*
	 * Gather replication slots.  Unlike a backend, which is only listed while
	 * it holds a snapshot, a slot is reported as soon as it exists, even when
	 * it currently pins no xmin, e.g. a freshly created physical slot with no
	 * standby attached, or an invalidated slot.  Such a slot appears with all
	 * per-class columns NULL.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots + max_repack_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		TransactionId xmin;
		TransactionId catalog_xmin;
		ReplicationSlotInvalidationCause invalidated;
		XidHorizonRow *r;

		if (!s->in_use)
			continue;

		/* r->kind is palloc'd here so no allocation happens under the mutex */
		Assert(nrows < maxrows);
		r = &rows[nrows++];
		memset(r, 0, sizeof(*r));
		r->kind = CStringGetTextDatum("replication_slot");

		SpinLockAcquire(&s->mutex);
		xmin = s->effective_xmin;
		catalog_xmin = s->effective_catalog_xmin;
		invalidated = s->data.invalidated;
		r->datid = s->data.database;
		namestrcpy(&r->slot_name, NameStr(s->data.name));
		SpinLockRelease(&s->mutex);

		/* Invalidated slots do not affect the horizon */
		if (invalidated != RS_INVAL_NONE)
			xmin = catalog_xmin = InvalidTransactionId;

		r->shared = TransactionIdOlder(xmin, catalog_xmin);
		r->catalog = r->shared;
		r->data = xmin;
	}
	LWLockRelease(ReplicationSlotControlLock);

	for (int i = 0; i < nrows; i++)
	{
		XidHorizonRow *r = &rows[i];
		Datum		values[PG_GET_XMIN_HORIZON_COLS];
		bool		nulls[PG_GET_XMIN_HORIZON_COLS];
		int			col;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		col = 0;

		values[col++] = r->kind;

		if (r->pid == 0)
			nulls[col++] = true;
		else
			values[col++] = Int32GetDatum(r->pid);

		if (r->slot_name.data[0] == '\0')
			nulls[col++] = true;
		else
			values[col++] = NameGetDatum(&r->slot_name);

		if (r->datid == InvalidOid)
			nulls[col++] = true;
		else
			values[col++] = ObjectIdGetDatum(r->datid);

		if (TransactionIdIsValid(r->shared))
			values[col++] = TransactionIdGetDatum(r->shared);
		else
			nulls[col++] = true;

		if (TransactionIdIsValid(r->catalog))
			values[col++] = TransactionIdGetDatum(r->catalog);
		else
			nulls[col++] = true;

		if (TransactionIdIsValid(r->data))
			values[col++] = TransactionIdGetDatum(r->data);
		else
			nulls[col++] = true;

		Assert(col == PG_GET_XMIN_HORIZON_COLS);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}
