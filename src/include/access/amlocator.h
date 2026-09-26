/*-------------------------------------------------------------------------
 *
 * amlocator.h
 *	  The locator contract between table and index access methods.
 *
 * An index stores, for every entry, a locator: the value it hands back to
 * the table access method to identify the row the entry describes.  Heap's
 * locator is an ItemPointerData, and several parts of the system rely on
 * properties of that particular locator.  Bitmap scans split it into a block
 * and an offset.  TID range scans expose its ordering to SQL.  A table AM
 * whose rows are located differently would break those assumptions without
 * any way to say so.
 *
 * The locator descriptor lets a table AM state these properties, and lets the
 * code that depends on one test it.  A table AM returns a descriptor for each
 * relation from its relation_locator callback, reached through
 * RelationGetLocatorDesc().
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

#include "storage/buf.h"

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
	const char *name;			/* for error messages */

	/*
	 * Does a row keep its locator across an UPDATE?  If not, as for heap, an
	 * update may store the new version at a different locator, and a caller
	 * that locks the latest version of a row may be handed a different one;
	 * see TM_FailureData.traversed.
	 */
	bool		stable;

	/*
	 * A bitmap scan splits every locator into a group and a slot within it;
	 * for heap the group is a block and the slot is a line pointer offset.
	 * The two callbacks below let a table AM warn the bitmap code that a
	 * given group's slots cannot be combined exactly, because two index
	 * entries for one row version may name different slots in it.
	 *
	 * A combining bitmap operation reads a group only through these hooks; it
	 * never touches the table.  Each takes the group's identifier and a
	 * "cache" the function may use to keep a pinned buffer across calls on the
	 * same scan, which the caller releases.  A NULL callback means "always
	 * exact" for that operation, which is the common case and the behavior for
	 * a table AM that leaves both unset.
	 *
	 * bitmap_and_inexact: may set intersection (BitmapAnd) lose a row in this
	 * group?  It keeps a slot only when every input has it, so two entries
	 * that name different slots for one row would drop the row.  When this
	 * returns true, BitmapAnd unions the inputs' slots for the group instead
	 * and marks it for recheck.
	 *
	 * bitmap_or_inexact: may set union (BitmapOr) mishandle this group?  Union
	 * keeps a slot when any input has it, so it cannot lose a row this way and
	 * no in-core AM needs this; it exists so a future AM whose union is not
	 * exact can force a recheck of the group.  When it returns true, BitmapOr
	 * marks the group for recheck.
	 */
	bool		(*bitmap_and_inexact) (struct RelationData *rel, uint64 group,
									   Buffer *cache);
	bool		(*bitmap_or_inexact) (struct RelationData *rel, uint64 group,
									  Buffer *cache);
} LocatorDesc;

#endif							/* AMLOCATOR_H */
