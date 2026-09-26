/*-------------------------------------------------------------------------
 *
 * amlocator.h
 *	  The locator contract between table and index access methods.
 *
 * An index stores, for every entry, a locator: the value it hands back to
 * the table access method to identify the row the entry describes.  Heap's
 * locator is an ItemPointerData, and several parts of the system rely on
 * properties of that particular locator.  Bitmap scans split it into a block
 * and an offset.  TID range scans expose its ordering to SQL.  The
 * after-trigger queue records a row's locator during an UPDATE and fetches the
 * pre-update row through it later.  A table AM whose rows are located
 * differently, or that overwrites rows in place, would break those assumptions
 * without any way to say so.
 *
 * The locator descriptor lets a table AM state the properties in which its
 * locator may differ from heap's, and lets the code that depends on one test
 * it.  A table AM returns a descriptor for each relation from its
 * relation_locator callback, reached through RelationGetLocatorDesc().
 *
 * Core code also relies on these properties of every locator, which no
 * descriptor field states:
 *
 * - Index AMs and the executor keep a locator in an ItemPointerData: a block
 *   number and an offset.  validate_index() packs it into an int8 with
 *   itemptr_encode() for CREATE INDEX CONCURRENTLY.  An index can therefore
 *   hold only a locator that fits in sizeof(ItemPointerData) bytes.
 *
 * - The locator is the row's ctid, a value of type tid.  The executor carries
 *   it as a junk column through UPDATE, DELETE, MERGE and row locking
 *   (preptlist.c, appendinfo.c), WHERE CURRENT OF finds a row by it
 *   (execCurrent.c), and TID scans fetch rows by it.  This holds whether or
 *   not the table has indexes.
 *
 * - Some values mean something else to core code, so no locator uses them:
 *   offset InvalidOffsetNumber, offsets above MaxOffsetNumber (such as
 *   SpecTokenOffsetNumber and MovedPartitionsOffsetNumber in itemptr.h, and
 *   GIN's lossy-page marker), and block InvalidBlockNumber.
 *
 * - Locators sort as unsigned (block, offset) pairs, and rows near each other
 *   in that order should be near each other in storage.  nbtree orders equal
 *   keys by locator and relies on that order for deduplication and suffix
 *   truncation; ANALYZE sorts sampled rows by it (analyze.c); TID range scans
 *   expose it.  BRIN summarizes rows by ranges of block numbers, read through
 *   table_index_build_range_scan().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/amlocator.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMLOCATOR_H
#define AMLOCATOR_H

#include "storage/off.h"

/* avoid including rel.h here; rel.h and tableam.h include this header */
struct RelationData;

/*
 * A description of the locator a relation's table AM hands to its indexes.
 *
 * NB: the table AM returns this from its relation_locator callback, and the
 * relcache keeps the pointer for the life of the relcache entry, so it must
 * remain valid that long.  A static descriptor, as heap uses, satisfies this.
 */
typedef struct LocatorDesc
{
	/*
	 * The locator's size in bytes.  A positive value is an exact size, the
	 * same for every row; a negative value is the largest a variable-width
	 * locator can be, so its magnitude bounds the width either way.
	 */
	int16		width;

	/*
	 * The largest offset any locator of the relation carries, at most
	 * MaxOffsetNumber; for heap, MaxHeapTuplesPerPage.  A TID bitmap holds
	 * offsets only up to TBM_MAX_TUPLES_PER_PAGE, so the planner considers no
	 * bitmap scan of a relation whose max_offset is larger, and GIN, whose
	 * posting lists and partial-match scans assume the same bound, refuses to
	 * index it.
	 */
	OffsetNumber max_offset;

	const char *name;			/* for error messages */

	/*
	 * Does a row keep its locator for its whole life?  Heap moves a row to a
	 * new TID on every UPDATE, so it says false: a lock that follows the
	 * update chain may change *tid.  An AM that updates rows in place, or
	 * names rows by a key that UPDATE cannot change, says true, and
	 * table_tuple_lock, table_tuple_update and table_tuple_delete never
	 * change the caller's locator.
	 *
	 * This says nothing about whether the row's contents changed; a caller
	 * that must re-evaluate a concurrently updated row learns that from
	 * TM_FailureData.retargeted, whatever this says.
	 */
	bool		stable;

	/*
	 * Is the pre-update version of a row still fetchable through its locator
	 * after the UPDATE that replaced it?  Heap keeps the old version on its
	 * page until VACUUM, so it is.  An AM that overwrites rows in place says
	 * false; see RelationUpdatesInPlace().
	 */
	bool		old_version_retained;
} LocatorDesc;

#endif							/* AMLOCATOR_H */
