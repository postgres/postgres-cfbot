/*-------------------------------------------------------------------------
 *
 * visibilitymapdefs.h
 *		macros for accessing contents of visibility map pages
 *
 *
 * Copyright (c) 2021-2026, PostgreSQL Global Development Group
 *
 * src/include/access/visibilitymapdefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VISIBILITYMAPDEFS_H
#define VISIBILITYMAPDEFS_H

/* Number of bits for one heap page */
/*
 * Bits per heap block in the visibility map.  Widened from 2 to 4 to add
 * VISIBILITYMAP_LOCATOR_SPLIT (below).  3 bits would suffice, but
 * HEAPBLOCKS_PER_BYTE = BITS_PER_BYTE / BITS_PER_HEAPBLOCK is integer division,
 * so 3 and 4 both pack 2 blocks per byte; 4 keeps HEAPBLK_TO_OFFSET a shift and
 * leaves the top bit for the next user.  A cluster upgraded from a 2-bit VM has
 * its _vm forks rewritten by pg_upgrade.
 */
#define BITS_PER_HEAPBLOCK 4

/* Flags for bit map */
#define VISIBILITYMAP_ALL_VISIBLE	0x01
#define VISIBILITYMAP_ALL_FROZEN	0x02
/*
 * VISIBILITYMAP_LOCATOR_SPLIT: some live row on this heap block is reachable
 * through index entries that name different line pointers (a selective-indexed
 * update planted a fresh entry mid-chain while unchanged indexes still point at
 * the root).  Read lock-free by BitmapAnd to decide, per block, whether two
 * indexes' offsets may disagree; see tidbitmap.c and heapam.c.  Set/cleared by
 * the heap AM only; it is not part of visibility and is excluded from
 * VISIBILITYMAP_VALID_BITS (which gates the all-visible/all-frozen VM API).
 */
#define VISIBILITYMAP_LOCATOR_SPLIT	0x04
#define VISIBILITYMAP_VALID_BITS	0x03	/* OR of all valid visibilitymap
											 * flags bits */

#endif							/* VISIBILITYMAPDEFS_H */
