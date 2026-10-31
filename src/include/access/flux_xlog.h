/*-------------------------------------------------------------------------
 *
 * flux_xlog.h
 *	  FLUX table access method WAL definitions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/flux_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FLUX_XLOG_H
#define FLUX_XLOG_H

#include "postgres.h"

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "storage/off.h"

/* Forward declarations */
typedef struct FluxTupleData *FluxTuple;
typedef struct RelationData *Relation;
typedef struct FluxOverflowBuffers FluxOverflowBuffers;

/*
 * Heap-format tuple image for logical decoding.
 *
 * Logical decoding consumes a heap-format image of the FLUX tuple that the
 * write path appends to the end of the main WAL data channel as
 * "[heap bytes][uint32 heap_len]" (see decode.c).  Forming that image calls
 * heap_form_tuple()/palloc(), which are forbidden inside a WAL critical
 * section.  Callers therefore prepare the image with
 * FluxXLogPrepareLogicalImage() BEFORE entering the critical section; the
 * WAL functions only register the prebuilt bytes (an allocation-free
 * operation) inside the section.  The image must stay alive until after
 * XLogInsert() returns; release it with FluxXLogReleaseLogicalImage() after
 * END_CRIT_SECTION().
 *
 * When the relation is not logically logged, prepare leaves data == NULL and
 * the WAL functions skip the append.
 */
typedef struct FluxLogicalImage
{
	char	   *data;			/* palloc'd heap tuple body, or NULL */
	uint32		len;			/* heap tuple t_len (also written trailing) */
} FluxLogicalImage;

/*
 * WAL record types for FLUX
 */
/*
 * WAL record types for FLUX.
 *
 * Each opcode must be unique.  The info byte uses bits 0-7 with
 * XLR_INFO_MASK occupying the upper bits, so we have the lower
 * nibble(s) available for opcodes.
 */
#define XLOG_FLUX_INSERT			0x00
#define XLOG_FLUX_UPDATE_INPLACE	0x10
#define XLOG_FLUX_DELETE			0x20
#define XLOG_FLUX_DEFRAG			0x30	/* single-page defrag */
#define XLOG_FLUX_OVERFLOW_WRITE	0x40
#define XLOG_FLUX_COMPRESS			0x50
#define XLOG_FLUX_FREEZE_PAGE		0x70
#define XLOG_FLUX_INIT_PAGE		0x60
#define XLOG_FLUX_VM_SET			0x80	/* Set visibility map bits */
#define XLOG_FLUX_VM_CLEAR			0x90	/* Clear visibility map bits */
#define XLOG_FLUX_LOCK				0xA0	/* Tuple lock */
#define XLOG_FLUX_CAS_UPDATE		0xB0	/* Same-size CAS in-place update */
#define XLOG_FLUX_WRITE_DICT		0xC0	/* Compression-dictionary fork
											 * write */
#define XLOG_FLUX_MULTI_INSERT		0xD0	/* Batched multi-tuple insert */
#define XLOG_FLUX_CAS_UPDATE_UNDO	0xE0	/* CAS update folded with UNDO
											 * before-image */
#define XLOG_FLUX_SUPERSEDE		0xF0	/* Link a moved row's old version to
										 * its new TID */
#define XLOG_FLUX_OPMASK			0xF0

/* Aliases for backward compatibility / clarity */
#define XLOG_FLUX_VACUUM			XLOG_FLUX_DEFRAG
#define XLOG_FLUX_UPDATE			XLOG_FLUX_UPDATE_INPLACE

/* Flags for xl_flux_overflow_write */
#define FLUX_OVERFLOW_WAL_NEW_RECORD	0x0000	/* New overflow record */

/*
 * Common WAL record flags.
 *
 * These appear in the 'flags' field of the DML WAL record structures
 * (xl_flux_insert, xl_flux_update, xl_flux_delete).
 *
 * FLUX_WAL_NO_LOGICAL marks an UPDATE the caller asked not to be decoded
 * (TABLE_UPDATE_NO_LOGICAL), so that decoding skips it, as it skips heap's
 * UPDATE without a new tuple, instead of failing on the missing images.
 */
#define FLUX_WAL_NO_LOGICAL	0x0001
#define FLUX_WAL_CROSS_PAGE	0x0002	/* Cross-page out-of-place update */
/*
 * 0x0004 reserved (was FLUX_WAL_HAS_OVERFLOW_BLK0; overflow chunks are now
 * self-logged, never collected onto a DML record)
 */
#define FLUX_WAL_PREFIX_SUFFIX	0x0008	/* Update uses prefix/suffix
										 * compression */
/*
 * Heap-format tuple image is appended to the WAL record for the benefit of
 * logical decoding.  Set when RelationIsLogicallyLogged(rel) at WAL-emit
 * time.  The layout of the appended region is:
 *
 *		uint32		logical_len				-- bytes of the heap-tuple payload
 *		bytes[logical_len]	HeapTuple t_data bytes
 *
 * For INSERT / DELETE the record contains exactly one heap-tuple payload.
 * For UPDATE it contains two back-to-back payloads (old, then new).
 */
#define FLUX_WAL_LOGICAL_TUPLE	0x0010
#define FLUX_WAL_SPECULATIVE		0x0020	/* Speculative insert or abort */
#define FLUX_WAL_MOVED_PARTITION	0x0040	/* DELETE moved the row to another
											 * partition: set
											 * FLUX_TUPLE_MOVED_PARTITION */
#define FLUX_WAL_SPEC_CONFIRM	0x0080	/* INSERT confirms a speculative
										 * insertion; no tuple body */
#define FLUX_WAL_LOGICAL_REL	0x0100	/* CAS UPDATE of a relation that was
										 * logically logged: not decodable */
#define FLUX_WAL_ALL_VISIBLE_CLEARED	0x0200	/* INSERT, UPDATE or DELETE
												 * cleared PD_ALL_VISIBLE and
												 * the visibility map bits */

/*
 * Block id of the visibility map page an INSERT, UPDATE or DELETE record
 * clears bits on (with FLUX_WAL_ALL_VISIBLE_CLEARED).  The highest one, so
 * that it never takes the place of a destination or undo block, which follow
 * block 0 without gaps.
 */
#define FLUX_WAL_KEY_INTACT 0x0400	/* SUPERSEDE did not change key columns */

#define FLUX_VM_BLOCK_ID	XLR_MAX_BLOCK_ID

/* Freeze plans are WAL state, never hint bits. */
#define FLUX_FREEZE_XMIN 0x01
#define FLUX_FREEZE_XMAX 0x02
#define FLUX_FREEZE_DETACH 0x04

typedef struct xl_flux_freeze_page
{
	TransactionId conflict_xid;
	uint16		nplans;
	bool		is_catalog_rel;
} xl_flux_freeze_page;

typedef struct xl_flux_freeze_tuple
{
	uint16		offset;
	uint8		actions;
} xl_flux_freeze_tuple;

#ifndef FRONTEND
/*
 * WAL record data structures
 */
typedef struct xl_flux_insert
{
	Oid			relid;			/* logical identity for tuple sLog replay */
	OffsetNumber offnum;		/* Offset number */
	uint16		flags;			/* Flags (FLUX_WAL_* bits) */
	uint32		tuple_len;		/* Length of tuple data that follows */
	uint64		commit_ts;		/* Commit timestamp */
	/* Tuple data follows */
} xl_flux_insert;

typedef struct xl_flux_update
{
	OffsetNumber offnum;		/* Offset number on source page (block 0) */
	uint16		flags;			/* Flags (FLUX_WAL_* bits) */
	uint64		old_commit_ts;	/* Old commit timestamp */
	uint64		new_commit_ts;	/* New commit timestamp */
	uint16		old_tuple_len;	/* Length of old tuple */
	uint16		new_tuple_len;	/* Length of new tuple data that follows */
	uint8		dst_block_id;	/* Block ID of destination page for cross-page
								 * updates (only valid when
								 * FLUX_WAL_CROSS_PAGE is set in flags) */
	uint8		pad[3];			/* Padding for alignment */
	/* New tuple data follows (old tuple data is in UNDO log only) */
} xl_flux_update;

/*
 * Prefix/suffix compression header for in-place updates.
 *
 * When FLUX_WAL_PREFIX_SUFFIX is set in xl_flux_update.flags, this header
 * immediately follows the xl_flux_update struct and precedes the diff data.
 * Only the changed bytes (between prefixlen and len-suffixlen) are logged.
 *
 * The redo handler reconstructs the full new tuple by:
 *   1. Reading the existing tuple from the page (old data)
 *   2. Keeping old[0..prefixlen-1] as-is
 *   3. Copying the diff data into old[prefixlen..len-suffixlen-1]
 *   4. Keeping old[len-suffixlen..len-1] as-is
 */
typedef struct xl_flux_prefix_suffix
{
	uint16		prefixlen;		/* Bytes of common prefix */
	uint16		suffixlen;		/* Bytes of common suffix */
} xl_flux_prefix_suffix;

typedef struct xl_flux_delete
{
	OffsetNumber offnum;		/* Offset number */
	uint16		flags;			/* Flags (FLUX_WAL_* bits) */
	uint32		tuple_len;		/* Length of old tuple (for logical decoding) */
	uint64		commit_ts;		/* Commit timestamp */
	/* Old tuple data is in UNDO log only */
} xl_flux_delete;

/*
 * WAL record for a batched multi-tuple insert (XLOG_FLUX_MULTI_INSERT).
 *
 * flux_multi_insert packs many tuples onto a single page in one critical
 * section.  Rather than emit one xl_flux_insert per tuple (and force a
 * full-page image to stay crash-safe), we log every tuple body once in a
 * single record modelled on heap's xl_heap_multi_insert.
 *
 * Layout of the record's main data:
 *
 *		xl_flux_multi_insert						-- header (ntuples, flags, ts)
 *		repeated ntuples times:
 *			xl_flux_multi_insert_tuple				-- per-tuple header
 *			char body[datalen]						-- tuple t_data bytes
 *
 * If flags & FLUX_WAL_LOGICAL_TUPLE, ntuples heap-format logical images
 * follow (one per tuple) after the per-tuple region, each as [body][uint32 len].
 */
typedef struct xl_flux_multi_insert
{
	Oid			relid;			/* logical identity for tuple sLog replay */
	uint16		ntuples;		/* Number of tuples in this batch */
	uint16		flags;			/* Flags (FLUX_WAL_* bits) */
	uint64		commit_ts;		/* Shared commit timestamp */
	/* xl_flux_multi_insert_tuple entries follow */
} xl_flux_multi_insert;

#define SizeOfFluxMultiInsert	sizeof(xl_flux_multi_insert)

typedef struct xl_flux_multi_insert_tuple
{
	OffsetNumber offnum;		/* Target offset on the page */
	uint16		datalen;		/* Length of tuple t_data bytes that follow */
	/* char body[datalen] follows */
} xl_flux_multi_insert_tuple;

#define SizeOfFluxMultiInsertTuple	sizeof(xl_flux_multi_insert_tuple)

typedef struct xl_flux_lock
{
	OffsetNumber offnum;		/* Offset number */
	uint16		flags;			/* Flags */
	uint8		infomask;		/* Infomask bits (uint8) */
	uint8		lock_mode;		/* LockTupleMode */
} xl_flux_lock;

/*
 * XLOG_FLUX_SUPERSEDE: set FLUX_TUPLE_SUPERSEDED on the old version at offnum
 * of block 0 and store new_tid in its t_ctid.  See flux_link_superseded.
 * With FLUX_WAL_LOGICAL_TUPLE in flags, the old replica identity and the new
 * row follow as in XLOG_FLUX_UPDATE_INPLACE, and the record decodes as the
 * UPDATE that moved the row.
 */
typedef struct xl_flux_supersede
{
	OffsetNumber offnum;		/* Old version, on block 0 */
	ItemPointerData new_tid;	/* The new version */
	uint16		flags;			/* FLUX_WAL_LOGICAL_TUPLE or 0 */
} xl_flux_supersede;

/*
 * Per-backend UNDO record carried on a FLUX forward-op WAL record, so that
 * flux_redo() can re-run the undo insert instead of restoring the undo page
 * from a forced full-page image.
 *
 * This header plus its trailing payload is registered as BLOCK DATA on the
 * FIRST undo block of the record (see FluxPbuRegisterUndoBuffers), not on the
 * main data channel.  That keeps every opcode's main-data layout -- and hence
 * both the redo parsers and the tail-anchored logical-decoding images -- exactly
 * as they were, and it makes the region self-locating: redo finds the undo
 * blocks by their relfilenode (undo pages live in the reserved undo database
 * OID) and reads the region with XLogRecGetBlockData().  The undo buffers are
 * registered with REGBUF_KEEP_DATA so this region survives even when the undo
 * page happens to carry a full-page image.
 *
 * Fields:
 *   urec_ptr  - the undo location the DO-time PrepareUndoInsert() returned.
 *               Redo re-derives the location independently and PANICs on a
 *               mismatch, so a divergence between the recovered undo-log
 *               metadata and the WAL cannot silently misplace the record.
 *   reloid    - the undo record's uur_reloid.
 *   subtype   - the FLUX_UNDO_* code (uur_type).
 *   payload_len / trailing bytes - the undo record's uur_payload verbatim
 *               (FluxUndoPayloadHeader followed by the before-image tuple
 *               bytes).  Carried unmodified so the replayed record is
 *               byte-identical to the original, which preserves the target TID,
 *               the producing writer_xid, and the before-image -- hence the
 *               t_verptr version chain -- exactly as the primary wrote them.
 *   flags, dbid, trailing last_xact_start - the undo log state DO placed the
 *               record with, besides urec_ptr: FLUX_UNDO_WAL_FIRST_REC if it
 *               starts its transaction in the log (carries the transaction
 *               header), and then the log's last_xact_start just before it,
 *               8 bytes after the payload; and the writer's database.  Redo
 *               installs them with UndoLogRecoverySetPlacement() before it
 *               re-runs the insert: a checkpoint between DO's undo reservation
 *               and this record leaves the undo log snapshot recovery starts
 *               from already past the record.  flags and dbid occupy what was
 *               alignment padding, so the header did not grow.
 */
typedef struct xl_flux_undo
{
	TransactionId freeze_conflict;	/* DML before-image freeze horizon */
	bool		freeze_catalog_rel; /* relation is accessible in logical
									 * decoding, see
									 * RelationIsAccessibleInLogicalDecoding */
	uint16		flags;			/* FLUX_UNDO_WAL_* */
	uint64		urec_ptr;		/* DO-time undo record location */
	Oid			reloid;			/* relation OID of the undo record */
	uint16		subtype;		/* FLUX_UNDO_* code */
	uint16		payload_len;	/* bytes of payload that follow */
	TransactionId top_xid;		/* top-level xid that owns the undo log */
	uint32		top_xid_epoch;	/* epoch of top_xid */
	CommandId	cid;			/* command id of the writer (uur_cid) */
	Oid			dbid;			/* database of the writer */
	/* char payload[payload_len] follows */
	/* uint64 last_xact_start follows, if FLUX_UNDO_WAL_FIRST_REC */
} xl_flux_undo;

/*
 * The undo record starts its transaction in its log (carries the transaction
 * header).  The log's last_xact_start before it follows the payload.
 */
#define FLUX_UNDO_WAL_FIRST_REC		0x0001

#define SizeOfFluxUndo	sizeof(xl_flux_undo)

/*
 * WAL record for same-size CAS in-place update (XLOG_FLUX_CAS_UPDATE).
 *
 * This is a lightweight record logged by the tuple-level CAS update fast
 * path.  Only the changed portion of the tuple data is logged (the region
 * between data_offset and data_offset+data_len within the on-page tuple).
 * The redo handler patches these bytes directly into the tuple on the page.
 */
typedef struct xl_flux_cas_update
{
	OffsetNumber offnum;		/* Tuple offset on page */
	uint16		flags;			/* FLUX_WAL_* bits */
	uint16		data_offset;	/* Byte offset within tuple for patch start */
	uint16		data_len;		/* Length of replacement data */
	uint64		new_commit_ts;	/* New commit timestamp for the tuple */
	/* char data[data_len] follows */
} xl_flux_cas_update;

/*
 * WAL record for a CAS in-place update folded with its UNDO before-image
 * (XLOG_FLUX_CAS_UPDATE_UNDO).
 *
 * This is the FOLD variant of the CAS fast path: instead of emitting a
 * separate RM_FLUX_ID CAS-update record and a separate per-backend UNDO
 * before-image record, one record carries both.  Block 0 is the main-fork
 * page (same redo byte-diff as xl_flux_cas_update).  Block 1+ are the
 * per-backend UNDO pages, registered (without a forced image) by
 * FluxPbuRegisterUndoBuffers() so they are covered by this record; the undo
 * record's own bytes ride in the trailing xl_flux_undo region.
 *
 * The redo handler replays block 0 like flux_xlog_cas_update_redo, then re-runs
 * the undo insert from that region (FluxPbuRedoUndo).
 */
typedef struct xl_flux_cas_update_undo
{
	/* --- redo half (block 0): mirrors xl_flux_cas_update --- */
	OffsetNumber offnum;		/* Tuple offset on main-fork page */
	uint16		flags;			/* FLUX_WAL_* bits */
	uint16		data_offset;	/* Byte offset within tuple for patch start */
	uint16		data_len;		/* Length of replacement data */
	uint64		new_commit_ts;	/* New commit timestamp for the tuple */
	/* char redo_data[data_len] follows (block 0 byte-diff) */
	/* per-backend undo pages ride as full-page images at block 1+ */
} xl_flux_cas_update_undo;

#define SizeOfFluxCasUpdateUndo	sizeof(xl_flux_cas_update_undo)

/*
 * A defrag record removes tuples (the page image is always logged), so like
 * heap's prune record it carries the newest XID the removal conflicts with on
 * a hot standby, see FluxTupleAdvanceConflictHorizon.
 */
typedef struct xl_flux_defrag
{
	uint16		ntuples;		/* Number of tuples moved */
	uint64		commit_ts;		/* Commit timestamp */
	TransactionId snapshotConflictHorizon;	/* or InvalidTransactionId */
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */
	/* Array of offset mappings follows */
} xl_flux_defrag;

typedef struct xl_flux_overflow_write
{
	OffsetNumber offnum;		/* Offset of overflow record on page */
	uint16		flags;			/* Reserved; always FLUX_OVERFLOW_WAL_NEW_RECORD */
	uint32		data_len;		/* Length of overflow data chunk */
	uint64		commit_ts;		/* Commit timestamp */
	/* FluxOverflowRecordHeader + data follows */
} xl_flux_overflow_write;

typedef struct xl_flux_compress
{
	OffsetNumber offnum;		/* Offset number */
	uint16		attr_num;		/* Attribute number */
	uint8		comp_type;		/* Compression type */
	uint8		comp_level;		/* Compression level */
	uint32		orig_size;		/* Original size */
	uint32		comp_size;		/* Compressed size */
	uint64		commit_ts;		/* Commit timestamp */
	/* Compressed data follows */
} xl_flux_compress;

typedef struct xl_flux_vacuum
{
	uint32		ntuples;		/* Number of removed tuples */
} xl_flux_vacuum;

typedef struct xl_flux_init_page
{
	uint32		flags;			/* Page flags */
	uint64		commit_ts;		/* Initial commit timestamp */
} xl_flux_init_page;

/*
 * Compression-dictionary fork write (XLOG_FLUX_WRITE_DICT).
 *
 * The dictionary fork (FLUX_DICT_FORKNUM) stores trained ZSTD/LZ4 dictionary
 * blobs append-only.  Its pages use a non-standard layout (the directory
 * metapage keeps FluxDictMeta above pd_lower; data pages keep payload above
 * pd_lower), so the redo path cannot reconstruct them from logical deltas.
 * Instead each dirtied dict-fork page is logged as a full-page image with
 * REGBUF_FORCE_IMAGE and the redo handler simply restores the registered
 * block.  No record-specific payload is required beyond the block image.
 */
typedef struct xl_flux_write_dict
{
	BlockNumber blkno;			/* Dictionary-fork block being written */
} xl_flux_write_dict;

/*
 * Visibility Map WAL records
 */
typedef struct xl_flux_vm_set
{
	TransactionId conflict_xid;
	BlockNumber heapBlk;		/* Heap block number */
	uint8		flags;			/* VM flags being set */
} xl_flux_vm_set;

typedef struct xl_flux_vm_clear
{
	BlockNumber heapBlk;		/* Heap block number */
	uint8		flags;			/* VM flags being cleared */
} xl_flux_vm_clear;

/*
 * Offset mapping for defragmentation
 */
typedef struct FluxOffsetMapping
{
	OffsetNumber old_offnum;
	OffsetNumber new_offnum;
} FluxOffsetMapping;
#else							/* FRONTEND */

/* Frontend-safe versions of WAL record structures */
typedef struct xl_flux_insert
{
	Oid			relid;			/* logical identity for tuple sLog replay */
	uint16		offnum;			/* Offset number */
	uint16		flags;			/* Flags */
	uint32		tuple_len;		/* Length of tuple data */
	uint64		commit_ts;		/* Commit timestamp */
} xl_flux_insert;

typedef struct xl_flux_delete
{
	uint16		offnum;			/* Offset number */
	uint16		flags;			/* Flags */
	uint32		tuple_len;		/* Length of old tuple */
	uint64		commit_ts;		/* Commit timestamp */
} xl_flux_delete;

typedef struct xl_flux_multi_insert
{
	Oid			relid;			/* logical identity for tuple sLog replay */
	uint16		ntuples;		/* Number of tuples in this batch */
	uint16		flags;			/* Flags */
	uint64		commit_ts;		/* Shared commit timestamp */
} xl_flux_multi_insert;

typedef struct xl_flux_multi_insert_tuple
{
	uint16		offnum;			/* Target offset on the page */
	uint16		datalen;		/* Length of tuple data that follows */
} xl_flux_multi_insert_tuple;

typedef struct xl_flux_update
{
	uint16		offnum;			/* Offset number */
	uint16		flags;			/* Flags */
	uint64		old_commit_ts;	/* Old commit timestamp */
	uint64		new_commit_ts;	/* New commit timestamp */
	uint16		old_tuple_len;	/* Length of old tuple */
	uint16		new_tuple_len;	/* Length of new tuple */
} xl_flux_update;

typedef struct xl_flux_vacuum
{
	uint32		ntuples;		/* Number of removed tuples */
} xl_flux_vacuum;

typedef struct xl_flux_compress
{
	uint16		offnum;			/* Offset number */
	uint16		attr_num;		/* Attribute number */
	uint8		comp_type;		/* Compression type */
	uint8		comp_level;		/* Compression level */
	uint32		orig_size;		/* Original size */
	uint32		comp_size;		/* Compressed size */
	uint64		commit_ts;		/* Commit timestamp */
} xl_flux_compress;

#endif							/* !FRONTEND */

/*
 * Function prototypes
 */

/* Frontend-safe function prototypes (pg_waldump, etc.) */
extern void flux_desc(StringInfo buf, XLogReaderState *record);
extern const char *flux_identify(uint8 info);

#ifndef FRONTEND
/* WAL replay and logging functions - backend only */
extern void flux_redo(XLogReaderState *record);
extern void flux_mask(char *page, BlockNumber blkno);

/*
 * Prepare/release the heap-format logical-decoding image.  Call prepare
 * BEFORE START_CRIT_SECTION() and release AFTER END_CRIT_SECTION().  When rel
 * is not logically logged, prepare sets img->data = NULL and the WAL
 * functions emit no logical image.
 */
extern void FluxXLogPrepareLogicalImage(Relation rel, FluxTuple rtup,
										FluxLogicalImage *img);
extern void FluxXLogPrepareReplicaIdentity(Relation rel, FluxTuple oldtup,
										   FluxTuple newtup, FluxLogicalImage *img);
extern void FluxXLogReleaseLogicalImage(FluxLogicalImage *img);
extern void FluxXLogRegisterLogicalImage(FluxLogicalImage *img, uint16 *flags);

extern XLogRecPtr FluxXLogInsert(Relation rel, Buffer buffer, OffsetNumber offnum,
								 FluxTuple tuple, uint64 commit_ts,
								 FluxLogicalImage *logical_img,
								 bool force_page_image);
extern XLogRecPtr FluxXLogUpdate(Relation rel, Buffer buffer, OffsetNumber offnum,
								 FluxTuple old_tuple, FluxTuple new_tuple,
								 uint64 old_commit_ts, uint64 new_commit_ts,
								 Buffer new_buffer,
								 FluxLogicalImage *old_img,
								 FluxLogicalImage *new_img,
								 bool no_logical);
extern XLogRecPtr FluxXLogDelete(Relation rel, Buffer buffer, OffsetNumber offnum,
								 FluxTuple tuple, uint64 commit_ts,
								 FluxLogicalImage *logical_img);

/*
 * Log a batch of tuples inserted onto a single page by flux_multi_insert.
 * Every tuple body is logged once (no forced full-page image), modelled on
 * heap's xl_heap_multi_insert.  offnums[i] is the on-page offset assigned to
 * tuples[i]; logical_imgs may be NULL (no logical decoding) or an array of
 * ntuples prepared images.
 */
extern XLogRecPtr FluxXLogMultiInsert(Relation rel, Buffer buffer,
									  OffsetNumber *offnums, FluxTuple *tuples,
									  int ntuples, uint64 commit_ts,
									  FluxLogicalImage *logical_imgs);

extern void FluxExecuteFreeze(Buffer buffer, xl_flux_freeze_tuple *plans, int nplans);
extern XLogRecPtr FluxXLogFreeze(Relation rel, Buffer buffer,
								 xl_flux_freeze_tuple *plans, int nplans,
								 TransactionId conflict_xid);
extern XLogRecPtr FluxXLogDefrag(Relation rel, Buffer buffer,
								 FluxOffsetMapping *mappings, int nmappings, uint64 commit_ts,
								 TransactionId snapshotConflictHorizon);
extern XLogRecPtr FluxXLogOverflowWrite(Relation rel, Buffer buffer,
										OffsetNumber offnum, char *record_data,
										uint32 record_len, uint16 flags,
										uint64 commit_ts);
extern XLogRecPtr FluxXLogCompress(Relation rel, Buffer buffer, OffsetNumber offnum,
								   uint16 attr_num, uint8 comp_type,
								   uint8 comp_level, char *comp_data, uint32 orig_size, uint32 comp_size,
								   uint64 commit_ts);
extern XLogRecPtr FluxXLogInitPage(Relation rel, Buffer buffer, uint32 flags, uint64 commit_ts);
extern XLogRecPtr FluxXLogCasUpdate(Relation rel, Buffer buffer,
									OffsetNumber offnum,
									uint16 data_offset, uint16 data_len,
									const char *new_data,
									uint64 new_commit_ts);
extern XLogRecPtr FluxXLogCasUpdateUndo(Relation rel, Buffer buffer,
										OffsetNumber offnum,
										uint16 data_offset, uint16 data_len,
										const char *new_data,
										uint64 new_commit_ts);

/*
 * Log a compression-dictionary fork page as a full-page image so crash
 * recovery and physical replicas reproduce the append-only dict fork.  The
 * caller must hold the buffer locked and have already modified it inside a
 * critical section; this registers the page with REGBUF_FORCE_IMAGE and sets
 * the page LSN.
 */
extern XLogRecPtr FluxXLogWriteDict(Relation rel, Buffer buffer);

/*
 * Logical replication decode entry point for FLUX WAL records.
 */
struct LogicalDecodingContext;
struct XLogRecordBuffer;
extern void flux_decode(struct LogicalDecodingContext *ctx,
						struct XLogRecordBuffer *buf);
#endif							/* !FRONTEND */
#endif							/* FLUX_XLOG_H */
