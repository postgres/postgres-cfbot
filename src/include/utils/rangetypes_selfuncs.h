/*-------------------------------------------------------------------------
 *
 * rangetypes_selfuncs.h
 *	  Shared helper functions for range and multirange selectivity estimation.
 *
 * These functions are defined in rangetypes_selfuncs.c and used by both
 * rangetypes_selfuncs.c and multirangetypes_selfuncs.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/rangetypes_selfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RANGETYPES_SELFUNCS_H
#define RANGETYPES_SELFUNCS_H

#include "utils/rangetypes.h"

extern double calc_hist_selectivity_scalar(TypeCacheEntry *typcache,
										   const RangeBound *constbound,
										   const RangeBound *hist, int hist_nvalues,
										   bool equal);
extern int	rbound_bsearch(TypeCacheEntry *typcache,
						   const RangeBound *value, const RangeBound *hist,
						   int hist_length, bool equal);
extern int	length_hist_bsearch(const Datum *length_hist_values,
								int length_hist_nvalues,
								double value, bool equal);
extern float8 get_position(TypeCacheEntry *typcache,
						   const RangeBound *value,
						   const RangeBound *hist1, const RangeBound *hist2);
extern double get_len_position(double value, double hist1, double hist2);
extern float8 get_distance(TypeCacheEntry *typcache,
						   const RangeBound *bound1, const RangeBound *bound2);
extern double calc_length_hist_frac(const Datum *length_hist_values,
									int length_hist_nvalues,
									double length1, double length2, bool equal);
extern double calc_hist_selectivity_contained(TypeCacheEntry *typcache,
											  const RangeBound *lower, RangeBound *upper,
											  const RangeBound *hist_lower, int hist_nvalues,
											  const Datum *length_hist_values,
											  int length_hist_nvalues);
extern double calc_hist_selectivity_contains(TypeCacheEntry *typcache,
											 const RangeBound *lower, const RangeBound *upper,
											 const RangeBound *hist_lower, int hist_nvalues,
											 const Datum *length_hist_values,
											 int length_hist_nvalues);
extern double calc_hist_join_selectivity(TypeCacheEntry *typcache,
										 const RangeBound *hist1, int nhist1,
										 const RangeBound *hist2, int nhist2);

#endif							/* RANGETYPES_SELFUNCS_H */
