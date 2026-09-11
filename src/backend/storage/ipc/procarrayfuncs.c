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
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

/*
 * One gathered row, with the per-class xmin values and the columns that
 * identify it.  Rows are initialized to zero, and each field left at zero
 * becomes SQL NULL.
 */
typedef struct XidHorizonRow
{
	Datum		kind;			/* text Datum (never NULL) */
	int			pid;			/* backend / walsender PID */
	NameData	slot_name;		/* replication slot name */
	Oid			datid;			/* database ID */
	TransactionId xid;			/* top-level xid */
	TransactionId shared;		/* per-class xmin values */
	TransactionId catalog;
	TransactionId data;
	TimestampTz xact_start;		/* backend rows only */
} XidHorizonRow;

/*
 * pg_get_xmin_horizon - SQL SRF reporting, per row, the raw per-class xmin
 * contributions to the cluster's xmin horizons.
 *
 * Emits one row per in-use replication slot, prepared transaction, slot-less
 * hot-standby-feedback walsender, and backend that holds a snapshot.
 *
 * This function collects data from multiple sources and does not guarantee
 * consistency across them.  In particular, if this function scans a backend
 * in the procarray, that backend's transaction ends, and the same backend or
 * a backend with the same pid and procNumber starts another transaction
 * before the backend status is read, then the xact_start field will be
 * incorrect.
 *
 * The effective aggregate contribution of all slots to the xmin used by
 * vacuum may be temporarily older than any individual slot xmin reported
 * by this function.  See the comment in
 * ReplicationSlotsComputeRequiredXmin().
 */
Datum
pg_get_xmin_horizon(PG_FUNCTION_ARGS)
{
#define PG_GET_XMIN_HORIZON_COLS 9
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
				 errhint("%s cannot be executed during recovery.",
						 "pg_get_xmin_horizon()")));

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
		 * claiming they pin.  Non-walsender backends without databases pin
		 * only the shared class and are handled later.  Every other proc pins
		 * all three classes.  VISHORIZON_TEMP is omitted because the temp
		 * horizon is per-backend and has no cross-backend diagnostic value.
		 * Keep this skip filter in sync with ComputeXidHorizons() and
		 * GetSnapshotData().
		 */
		if (p->statusFlags & (PROC_IN_VACUUM | PROC_IN_LOGICAL_DECODING))
			shared = catalog = data = InvalidTransactionId;
		else
			shared = catalog = data = effective_xmin;

		Assert(nrows < maxrows);
		r = &rows[nrows++];
		memset(r, 0, sizeof(*r));
		r->datid = p->databaseId;
		r->xid = p->xid;

		pid = p->pid;
		if (pid == 0)
		{
			/*
			 * A prepared xact's dummy PGPROC never carries an xmin (see
			 * MarkAsPreparingGuts()), so this row's per-class values are
			 * always its xid.  The pg_xmin_horizon view joins
			 * pg_prepared_xacts on the xid column to attach the gid.
			 */
			r->kind = CStringGetTextDatum("prepared_xact");
		}
		else if (p->statusFlags & PROC_AFFECTS_ALL_HORIZONS)
		{
			r->kind = CStringGetTextDatum("standby_feedback");
			r->pid = pid;
			r->datid = InvalidOid;
			r->xid = InvalidTransactionId;
		}
		else
		{
			r->kind = CStringGetTextDatum("backend");
			r->pid = pid;
			r->xact_start = pgstat_get_xact_start_by_proc_number(p->procNumber,
																 pid);

			/*
			 * A non-walsender backend not connected to a database, such as
			 * the autovacuum launcher or a backend still starting up, has no
			 * practical effect on data or catalog horizons.  See
			 * ComputeXidHorizons().
			 */
			if (p->databaseId == InvalidOid)
				catalog = data = InvalidTransactionId;
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
	 * ProcArrayLock held, and pg_get_replication_slots() and
	 * ReplicationSlotsComputeRequiredXmin() both follow it.  Holding
	 * ProcArrayLock across that acquisition would establish a new
	 * lock-ordering and risk deadlocks.
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

		if (TransactionIdIsValid(r->xid))
			values[col++] = TransactionIdGetDatum(r->xid);
		else
			nulls[col++] = true;

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

		if (r->xact_start != 0)
			values[col++] = TimestampTzGetDatum(r->xact_start);
		else
			nulls[col++] = true;

		Assert(col == PG_GET_XMIN_HORIZON_COLS);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}
