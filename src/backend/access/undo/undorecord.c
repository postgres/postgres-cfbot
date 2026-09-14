/*-------------------------------------------------------------------------
 *
 * undorecord.c
 *	  UNDO record assembly and serialization
 *
 * This file implements the AM-agnostic UNDO record format and provides
 * functions for creating, serializing, and deserializing UNDO records.
 * All AM-specific knowledge is kept out of this module; records carry
 * opaque payloads whose interpretation is delegated to the owning RM.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undorecord.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/undorecord.h"
#include "utils/memutils.h"

/*
 * Per-backend recycled memory context for UndoRecordSet.
 *
 * Instead of creating and destroying a MemoryContext for every
 * UndoRecordSet, we recycle one context across operations within a
 * transaction.  This avoids the overhead of repeated
 * AllocSetContextCreate/MemoryContextDelete for high-frequency
 * operations (e.g., 1000-row INSERTs).  The cached context is cleaned
 * up at transaction end by UndoRecordSetResetCache().
 */
static MemoryContext UndoRecordReusableContext = NULL;

/*
 * UndoRecordGetPayloadSize - Calculate size needed for an UNDO record
 *
 * This includes the fixed header plus the RM-specific payload, rounded up to
 * MAXALIGN so that each record in a packed stream begins at a MAXALIGN'd
 * offset.  UndoRecordHeader contains 8-byte members (urec_xid, urec_prev),
 * and the writers build the header in place through an (UndoRecordHeader *)
 * cast into the packed buffer; without this rounding a record whose length is
 * not a multiple of MAXIMUM_ALIGNOF would leave the next record's header at a
 * mis-aligned address, which is undefined behavior (and traps under
 * -fsanitize=alignment / crashes on alignment-strict hardware).  The padded
 * size is stored in urec_len, so readers advance by the same stride; the true
 * payload length travels separately in urec_payload_len, so the padding never
 * affects payload extraction.
 */
Size
UndoRecordGetPayloadSize(Size payload_len)
{
	return MAXALIGN(SizeOfUndoRecordHeader + payload_len);
}

/*
 * UndoRecordSerialize - Serialize an UNDO record into a buffer
 *
 * The destination buffer must be large enough to hold the entire record.
 * Use UndoRecordGetPayloadSize() to determine the required size.
 */
void
UndoRecordSerialize(char *dest, UndoRecordHeader *header,
					const char *payload, Size payload_len)
{
	/* Copy header */
	memcpy(dest, header, SizeOfUndoRecordHeader);

	/* Copy payload if present */
	if (payload_len > 0 && payload != NULL)
	{
		memcpy(dest + SizeOfUndoRecordHeader, payload, payload_len);
	}
}

/*
 * UndoRecordDeserialize - Deserialize an UNDO record from a buffer
 *
 * Reads the header and sets the payload pointer into the source buffer
 * (zero-copy).  Returns true on success, false on failure.
 */
bool
UndoRecordDeserialize(const char *src, UndoRecordHeader *header,
					  char **payload)
{
	if (src == NULL || header == NULL)
		return false;

	/* Copy header */
	memcpy(header, src, SizeOfUndoRecordHeader);

	/* Set payload pointer if there is payload data */
	if (header->urec_payload_len > 0)
	{
		if (payload != NULL)
			*payload = (char *) (src + SizeOfUndoRecordHeader);
	}
	else
	{
		if (payload != NULL)
			*payload = NULL;
	}

	return true;
}

/*
 * UndoRecordSetCreate - Create a new UNDO record set
 *
 * A record set accumulates multiple UNDO records before writing them
 * to the UNDO log in a batch. This improves performance by reducing
 * I/O operations.
 */
UndoRecordSet *
UndoRecordSetCreate(TransactionId xid, UndoRecPtr prev_undo_ptr)
{
	UndoRecordSet *uset;
	MemoryContext mctx;
	MemoryContext parent;

	/*
	 * Use TopTransactionContext as the parent so the record set survives
	 * across SPI statement boundaries.  When called from PL/pgSQL DO blocks,
	 * CurrentMemoryContext is the executor's per-query context
	 * (es_query_cxt), which is destroyed in FreeExecutorState() after each
	 * SPI_execute call.  Since xactundo.c stores the uset pointer in the
	 * static XactUndo.record_set[] and reuses it across multiple statements
	 * within a transaction, the context must outlive any single query.
	 * TopTransactionContext is ideal: it survives until transaction
	 * commit/abort, and AtAbort cleanup will free the uset via
	 * UndoRecordSetFree().
	 */
	parent = TopTransactionContext;

	/*
	 * Reuse a previously recycled memory context if available. This avoids
	 * the overhead of AllocSetContextCreate/MemoryContextDelete for every
	 * UndoRecordSet within a transaction.  MemoryContextReset clears all
	 * allocations but keeps the context's memory blocks for reuse.
	 */
	if (UndoRecordReusableContext != NULL)
	{
		mctx = UndoRecordReusableContext;
		UndoRecordReusableContext = NULL;	/* take ownership */
		MemoryContextReset(mctx);
		MemoryContextSetParent(mctx, parent);
	}
	else
	{
		mctx = AllocSetContextCreate(parent,
									 "UNDO record set",
									 ALLOCSET_DEFAULT_SIZES);
	}

	/*
	 * Allocate everything in the uset's memory context using direct
	 * MemoryContextAlloc to avoid MemoryContextSwitchTo overhead.
	 */
	uset = (UndoRecordSet *) MemoryContextAllocZero(mctx, sizeof(UndoRecordSet));
	uset->xid = xid;
	uset->prev_undo_ptr = prev_undo_ptr;
	uset->persistence = UNDOPERSISTENCE_PERMANENT;
	uset->type = URST_TRANSACTION;

	/*
	 * Allocate initial buffer.  512 bytes is enough for a single UNDO record
	 * (48-byte header + typical heap payload).  For bulk mode the buffer
	 * grows dynamically via UndoRecordEnsureCapacity.
	 */
	uset->buffer_capacity = 512;
	uset->buffer = (char *) MemoryContextAlloc(mctx, uset->buffer_capacity);
	uset->buffer_size = 0;

	uset->last_batch_lsn = InvalidXLogRecPtr;
	uset->mctx = mctx;

	return uset;
}

/*
 * UndoRecordSetFree - Free an UNDO record set
 *
 * Recycles the memory context for later reuse if possible, otherwise
 * destroys it.  We keep at most one recycled context to bound memory.
 */
void
UndoRecordSetFree(UndoRecordSet *uset)
{
	MemoryContext mctx;

	if (uset == NULL || uset->mctx == NULL)
		return;

	mctx = uset->mctx;

	if (UndoRecordReusableContext == NULL)
	{
		/*
		 * Recycle this context for the next UndoRecordSetCreate call.
		 *
		 * Re-parent to TopMemoryContext so the cached context is not
		 * destroyed if its original parent is cleaned up before
		 * UndoRecordSetResetCache() runs.  This can happen when the UNDO
		 * record set was created inside an SPI execution context (e.g., DO $$
		 * ... $$ blocks): SPI_finish() deletes its procCxt/execCxt, which
		 * would recursively destroy this child context, leaving
		 * UndoRecordReusableContext as a dangling pointer.
		 * UndoRecordSetCreate() will re-parent it to the caller's
		 * CurrentMemoryContext on reuse.
		 */
		MemoryContextSetParent(mctx, TopMemoryContext);
		UndoRecordReusableContext = mctx;
	}
	else
	{
		/* Already have one recycled context; destroy this one */
		MemoryContextDelete(mctx);
	}
}

/*
 * UndoRecordEnsureCapacity - Ensure the uset buffer can hold additional bytes
 *
 * Grows the buffer (using the uset's memory context) if needed.
 * Avoids MemoryContextSwitchTo overhead by using MemoryContextAlloc directly.
 */
static void
UndoRecordEnsureCapacity(UndoRecordSet *uset, Size additional)
{
	if (uset->buffer_size + additional > uset->buffer_capacity)
	{
		Size		new_capacity = uset->buffer_capacity * 2;
		char	   *newbuf;

		while (new_capacity < uset->buffer_size + additional)
			new_capacity *= 2;

		newbuf = (char *) MemoryContextAlloc(uset->mctx, new_capacity);
		if (uset->buffer_size > 0)
			memcpy(newbuf, uset->buffer, uset->buffer_size);
		pfree(uset->buffer);
		uset->buffer = newbuf;
		uset->buffer_capacity = new_capacity;
	}
}

/*
 * UndoRecordSetReset - Reset a record set for reuse
 *
 * Resets the buffer position and record count without freeing the memory
 * context or reallocating the buffer.  This is much cheaper than
 * UndoRecordSetCreate/Free (~5 cycles vs ~300 cycles) because it avoids
 * MemoryContextReset/AllocSetContextCreate overhead entirely.
 *
 * The prev_undo_ptr and other metadata are preserved so the record set
 * can continue chaining records correctly across multiple insertions
 * within the same transaction.
 */
void
UndoRecordSetReset(UndoRecordSet *uset)
{
	if (uset == NULL)
		return;

	uset->buffer_size = 0;
	uset->nrecords = 0;
}

/*
 * UndoRecordSetResetCache - Release the recycled memory context.
 *
 * Called at transaction end (commit or abort) to ensure the cached
 * context does not outlive the transaction.
 */
void
UndoRecordSetResetCache(void)
{
	if (UndoRecordReusableContext != NULL)
	{
		MemoryContextDelete(UndoRecordReusableContext);
		UndoRecordReusableContext = NULL;
	}
}

/*
 * UndoRecordAddPayload - Add an UNDO record with opaque payload to the set
 *
 * This is the main API for adding UNDO records.  The caller provides an
 * RM ID, RM-specific info flags, a relation OID, and an opaque payload.
 * The payload's interpretation is entirely RM-specific.
 */
void
UndoRecordAddPayload(UndoRecordSet *uset,
					 uint8 rmid,
					 uint16 info,
					 Oid reloid,
					 const char *payload,
					 Size payload_len)
{
	UndoRecordHeader *header;
	Size		record_size;
	char	   *dest;

	if (uset == NULL)
		elog(ERROR, "cannot add UNDO record to NULL set");

	record_size = UndoRecordGetPayloadSize(payload_len);

	/* Expand buffer if needed (allocate in the uset's memory context) */
	UndoRecordEnsureCapacity(uset, record_size);

	/*
	 * Build the header directly in the buffer, avoiding a separate stack
	 * variable, memset, and memcpy.  We zero the header in-place to avoid
	 * uninitialized padding bytes in the on-disk format.
	 */
	dest = uset->buffer + uset->buffer_size;
	header = (UndoRecordHeader *) dest;
	memset(header, 0, SizeOfUndoRecordHeader);
	header->urec_rmid = rmid;
	header->urec_flags = UNDO_INFO_XID_VALID;
	if (payload_len > 0)
		header->urec_flags |= UNDO_INFO_HAS_PAYLOAD;
	header->urec_info = info;
	header->urec_len = (uint32) record_size;
	header->urec_xid = uset->xid;
	header->urec_prev = uset->prev_undo_ptr;
	header->urec_reloid = reloid;
	header->urec_payload_len = (uint32) payload_len;

	/* Copy payload directly after header */
	if (payload_len > 0 && payload != NULL)
		memcpy(dest + SizeOfUndoRecordHeader, payload, payload_len);

	/*
	 * Zero the alignment padding between the payload and the next record so
	 * the serialized stream is deterministic (the padding is skipped by
	 * readers, which advance by urec_len and read urec_payload_len bytes of
	 * payload, but deterministic bytes keep WAL consistency checking happy).
	 */
	if (record_size > SizeOfUndoRecordHeader + payload_len)
		memset(dest + SizeOfUndoRecordHeader + payload_len, 0,
			   record_size - SizeOfUndoRecordHeader - payload_len);

	uset->buffer_size += record_size;
	uset->nrecords++;
}

/*
 * UndoRecordAddPayloadParts - Add an UNDO record with scatter-gather payload
 *
 * Like UndoRecordAddPayload, but takes the payload as two parts that are
 * concatenated directly into the uset buffer.  This avoids allocating an
 * intermediate payload buffer when the caller has the data in separate
 * pieces (e.g., a fixed header struct + variable-length tuple data).
 */
void
UndoRecordAddPayloadParts(UndoRecordSet *uset,
						  uint8 rmid,
						  uint16 info,
						  Oid reloid,
						  const char *part1,
						  Size part1_len,
						  const char *part2,
						  Size part2_len)
{
	UndoRecordHeader *header;
	Size		payload_len = part1_len + part2_len;
	Size		record_size;
	char	   *dest;

	if (uset == NULL)
		elog(ERROR, "cannot add UNDO record to NULL set");

	record_size = UndoRecordGetPayloadSize(payload_len);

	UndoRecordEnsureCapacity(uset, record_size);

	/* Build header directly in the buffer */
	dest = uset->buffer + uset->buffer_size;
	header = (UndoRecordHeader *) dest;
	memset(header, 0, SizeOfUndoRecordHeader);
	header->urec_rmid = rmid;
	header->urec_flags = UNDO_INFO_XID_VALID;
	if (payload_len > 0)
		header->urec_flags |= UNDO_INFO_HAS_PAYLOAD;
	header->urec_info = info;
	header->urec_len = (uint32) record_size;
	header->urec_xid = uset->xid;
	header->urec_prev = uset->prev_undo_ptr;
	header->urec_reloid = reloid;
	header->urec_payload_len = (uint32) payload_len;

	/* Copy payload parts directly after header */
	dest += SizeOfUndoRecordHeader;
	if (part1_len > 0 && part1 != NULL)
	{
		memcpy(dest, part1, part1_len);
		dest += part1_len;
	}
	if (part2_len > 0 && part2 != NULL)
	{
		memcpy(dest, part2, part2_len);
		dest += part2_len;
	}

	/*
	 * Zero the alignment padding between the payload and the next record (see
	 * UndoRecordAddPayload); dest now points just past the payload.
	 */
	{
		char	   *rec_start = uset->buffer + uset->buffer_size;

		if ((Size) (dest - rec_start) < record_size)
			memset(dest, 0, record_size - (Size) (dest - rec_start));
	}

	uset->buffer_size += record_size;
	uset->nrecords++;
}

/*
 * UndoRecordSetGetSize - Get total size of all records in set
 */
Size
UndoRecordSetGetSize(UndoRecordSet *uset)
{
	if (uset == NULL)
		return 0;

	return uset->buffer_size;
}

/*
 * UndoRecordSetGetNumRecords - How many records are accumulated in the set?
 *
 * Used by the deferred-batch path (DeferXactUndoData) to enforce the
 * undo_batch_record_limit bound and to tell whether a flush would write
 * anything.
 */
int
UndoRecordSetGetNumRecords(UndoRecordSet *uset)
{
	if (uset == NULL)
		return 0;

	return uset->nrecords;
}
