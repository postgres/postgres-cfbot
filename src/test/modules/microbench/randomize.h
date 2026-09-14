#ifndef MICROBENCH_RANDOMIZE_H
#define MICROBENCH_RANDOMIZE_H

#include "common/pg_prng.h"

/*
 * Fisher-Yates shuffle of a pointer array.
 * https://en.wikipedia.org/wiki/Fisher-Yates_shuffle
 *
 * Integer indexes (e.g. BlockNumber) can be stored in the array as intptr_t
 * and shuffled with the same function.
 */
static inline void
shuffle_pointers(pg_prng_state *rng, void **ptrs, int count)
{
	for (int i = count - 1; i > 0; i--)
	{
		int			k = (int) pg_prng_int64_range(rng, 0, i);
		void	   *tmp = ptrs[i];

		ptrs[i] = ptrs[k];
		ptrs[k] = tmp;
	}
}

#endif
