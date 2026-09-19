/*-------------------------------------------------------------------------
 *
 * undorecord.h
 *	  UNDO record format and insertion API
 *
 * This file defines the generic UNDO record format that can be used by
 * any access method or subsystem.  UNDO records are AM-agnostic: each
 * record carries an RM ID (urec_rmid) that identifies the resource
 * manager responsible for interpreting and applying the record.
 *
 * Design principles:
 * - Physical: UNDO stores opaque payload data for direct restore
 * - Generic: Usable by any AM or subsystem
 * - Compact: Variable-length format to minimize space
 * - Chained: Batches form backward chains via xl_undo_batch.chain_prev
 *   (see access/undo/README; the urec_prev field below is not a reader)
 * - Batch-oriented: API encourages batching for performance
 * - AM-agnostic: No AM-specific types in the generic header or API
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undorecord.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDORECORD_H
#define UNDORECORD_H

#include "access/undodefs.h"
#include "access/undolog.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/itemptr.h"

/*
 * UNDO record info flags
 *
 * These flags provide additional metadata about the UNDO record.
 * The lower byte is reserved for generic flags; the upper byte is
 * available for RM-specific use.
 */
#define UNDO_INFO_HAS_PAYLOAD	0x01	/* Record contains opaque payload */
#define UNDO_INFO_XID_VALID		0x08	/* urec_xid is valid */

/*
 * UndoRecordHeader - Fixed header for all UNDO records
 *
 * Every UNDO record starts with this header, followed by an optional
 * opaque payload whose interpretation is RM-specific.
 *
 * The urec_rmid field identifies which resource manager owns this record.
 * The urec_info field carries RM-specific subtype/flags (e.g., an in-place
 * update AM uses it to distinguish INSERT vs DELETE vs UPDATE).
 *
 * Size: 40 bytes (optimized for alignment)
 */
typedef struct UndoRecordHeader
{
	uint8		urec_rmid;		/* UNDO RM ID */
	uint8		urec_flags;		/* Generic flags (UNDO_INFO_*) */
	uint16		urec_info;		/* RM-specific subtype and flags */
	uint32		urec_len;		/* Total length including header + payload */

	TransactionId urec_xid;		/* Transaction that created this */

	/*
	 * Intended as a per-record back-link, but NOT read by the apply path: the
	 * real chain is between BATCHES, via xl_undo_batch.chain_prev, and order
	 * within a batch is positional (undoapply.c collects record offsets, then
	 * walks them in reverse).  Written by the record builders for format
	 * stability; do not add a reader without first reading the chain model in
	 * access/undo/README.
	 */
	UndoRecPtr	urec_prev;		/* Previous UNDO for same xact (chain) */

	Oid			urec_reloid;	/* Relation OID (InvalidOid if N/A) */

	/*
	 * Payload length: size of the RM-specific opaque data that follows the
	 * header.  Interpretation is entirely RM-specific.
	 */
	uint32		urec_payload_len;

	/* Followed by variable-length RM-specific payload */
} UndoRecordHeader;

#define SizeOfUndoRecordHeader	(offsetof(UndoRecordHeader, urec_payload_len) + sizeof(uint32))

/*
 * Access macros for payload data following the header
 *
 * The payload immediately follows the fixed header in the serialized
 * record.  Its interpretation is entirely RM-specific.
 */
#define UndoRecGetPayload(header) \
	((char *)(header) + SizeOfUndoRecordHeader)

/*
 * UndoRecordSetChunkHeader - Header at the start of each chunk.
 *
 * When an UndoRecordSet spans multiple undo logs (rare, since each log
 * is up to 1TB), the data is organized into chunks, each with a header
 * that records the chunk size and a back-pointer to the previous chunk.
 * This design follows the EDB undo-record-set branch architecture.
 */
typedef struct UndoRecordSetChunkHeader
{
	UndoLogOffset size;
	UndoRecPtr	previous_chunk;
	uint8		type;
} UndoRecordSetChunkHeader;

#define SizeOfUndoRecordSetChunkHeader \
	(offsetof(UndoRecordSetChunkHeader, type) + sizeof(uint8))

/*
 * Possible undo record set types.
 */
typedef enum UndoRecordSetType
{
	URST_INVALID = 0,			/* Placeholder when there's no record set. */
	URST_TRANSACTION = 'T',		/* Normal xact undo; apply on abort. */
	URST_MULTI = 'M',			/* Informational undo. */
	URST_EPHEMERAL = 'E'		/* Ephemeral data for testing purposes. */
} UndoRecordSetType;

/*
 * UndoRecordSet - Batch container for UNDO records
 *
 * This structure accumulates multiple UNDO records before writing them
 * to the UNDO log in a single operation. This improves performance by
 * reducing the number of I/O operations and lock acquisitions.
 *
 * The records are serialized into a contiguous buffer that grows
 * dynamically. The design follows the EDB undo-record-set branch
 * architecture with chunk-based organization and per-persistence-level
 * separation.
 */
typedef struct UndoRecordSet
{
	TransactionId xid;			/* Transaction ID for all records */
	UndoRecPtr	prev_undo_ptr;	/* Previous UNDO pointer in chain (legacy) */
	UndoPersistenceLevel persistence;	/* Persistence level of this set */
	UndoRecordSetType type;		/* Record set type */

	int			nrecords;		/* Number of records in set */

	/*
	 * Dynamic buffer for serialized records. Grows as needed; no fixed
	 * maximum. This replaces the old fixed-capacity max_records array.
	 */
	char	   *buffer;			/* Serialized record buffer */
	Size		buffer_size;	/* Current buffer size */
	Size		buffer_capacity;	/* Allocated buffer capacity */

	/*
	 * WAL-based UNDO chain tracking.  When UNDO records are written to WAL
	 * via XLOG_UNDO_BATCH, last_batch_lsn tracks the LSN of the most recent
	 * batch for this record set.  This is used as the chain_prev link when
	 * the next batch is written.
	 */
	XLogRecPtr	last_batch_lsn; /* LSN of last XLOG_UNDO_BATCH record */

	MemoryContext mctx;			/* Memory context for allocations */
} UndoRecordSet;

/*
 * Public API for UNDO record management
 */

/* Create/destroy/reset UNDO record sets */
extern UndoRecordSet *UndoRecordSetCreate(TransactionId xid,
										  UndoRecPtr prev_undo_ptr);
extern void UndoRecordSetFree(UndoRecordSet *uset);
extern void UndoRecordSetReset(UndoRecordSet *uset);
extern void UndoRecordSetResetCache(void);

/* Add records to a set - generic payload API */
extern void UndoRecordAddPayload(UndoRecordSet *uset,
								 uint8 rmid,
								 uint16 info,
								 Oid reloid,
								 const char *payload,
								 Size payload_len);

/* Add records with scatter-gather payload (avoids intermediate buffer) */
extern void UndoRecordAddPayloadParts(UndoRecordSet *uset,
									  uint8 rmid,
									  uint16 info,
									  Oid reloid,
									  const char *part1,
									  Size part1_len,
									  const char *part2,
									  Size part2_len);

/* Insert the accumulated records into UNDO log */
extern UndoRecPtr UndoRecordSetInsert(UndoRecordSet *uset);

/* WAL batch management for deferred UNDO allocation logging */
extern void UndoWalBatchFlush(void);
extern void UndoWalBatchReset(void);

/* Utility functions for record manipulation */
extern Size UndoRecordGetPayloadSize(Size payload_len);
extern void UndoRecordSerialize(char *dest, UndoRecordHeader *header,
								const char *payload, Size payload_len);
extern bool UndoRecordDeserialize(const char *src, UndoRecordHeader *header,
								  char **payload);

/* Statistics and debugging */
extern Size UndoRecordSetGetSize(UndoRecordSet *uset);

/* Records currently accumulated in the set (0 after a flush/reset). */
extern int	UndoRecordSetGetNumRecords(UndoRecordSet *uset);

/* UNDO application during rollback */

/*
 * Outcome of walking an UNDO chain.
 *
 * UNDO_CHAIN_DONE: every batch was read and no record failed.
 * UNDO_CHAIN_RECORD_ERROR: every batch was read, but some record returned
 *		UNDO_APPLY_ERROR (or belongs to an unknown rmgr).  Walking the chain
 *		again would fail the same way.
 * UNDO_CHAIN_INCOMPLETE: a batch could not be read, so the records it and
 *		all older batches hold were not applied at all.  The read may succeed
 *		later.
 * UNDO_CHAIN_MALFORMED: a batch was read but is not a well-formed part of
 *		the chain (not an UNDO batch, a truncated record or bad record
 *		length, or a chain_prev that does not point backward).  The older
 *		batches were not applied, and walking the chain again cannot help.
 */
typedef enum UndoChainOutcome
{
	UNDO_CHAIN_DONE,
	UNDO_CHAIN_RECORD_ERROR,
	UNDO_CHAIN_INCOMPLETE,
	UNDO_CHAIN_MALFORMED,
} UndoChainOutcome;

typedef struct UndoChainWalkResult
{
	int			batches;		/* batches read */
	int			applied;		/* records returning UNDO_APPLY_SUCCESS */
	int			skipped;		/* records returning UNDO_APPLY_SKIPPED */
	int			failed;			/* UNDO_APPLY_ERROR, or unknown rmgr */
	bool		fileops_failed; /* a FILEOPS inverse was not applied */
	bool		fileops_applied;	/* at least one FILEOPS inverse succeeded */
	bool		complete;		/* walked to the end (or stop) of the chain */
	bool		malformed;		/* if !complete, because of a malformed batch */
	XLogRecPtr	oldest_batch;	/* LSN of the oldest batch read */
	XLogRecPtr	unread_batch;	/* if !complete, the batch that stopped it */
} UndoChainWalkResult;

extern UndoChainOutcome UndoWalkChain(XLogRecPtr last_batch_lsn,
									  XLogRecPtr stop_at_lsn,
									  UndoChainWalkResult *res);
extern bool ApplyUndoChainFromWAL(XLogRecPtr last_batch_lsn);
extern bool ApplyUndoChainFromWALBounded(XLogRecPtr last_batch_lsn,
										 XLogRecPtr stop_at_lsn);
extern UndoChainOutcome ApplyUndoChain(XLogRecPtr last_batch_lsn,
									   XLogRecPtr stop_at_lsn,
									   UndoChainWalkResult *res);

#endif							/* UNDORECORD_H */
