/*-------------------------------------------------------------------------
 *
 * spin_delay_status.h
 *	  SpinDelayStatus type and the spin-delay/backoff helpers
 *
 * This header holds the delay/backoff bookkeeping used by the spinlock slow
 * path (perform_spin_delay / finish_spin_delay in s_lock.c) and by other
 * spinlock-like busy-wait loops.  It is included by storage/spin.h so the
 * definitions live in exactly one place.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/spin_delay_status.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_DELAY_STATUS_H
#define SPIN_DELAY_STATUS_H

#define DEFAULT_SPINS_PER_DELAY  100

typedef struct
{
	int			spins;
	int			delays;
	int			cur_delay;
	const char *file;
	int			line;
	const char *func;
} SpinDelayStatus;

static inline void
init_spin_delay(SpinDelayStatus *status,
				const char *file, int line, const char *func)
{
	status->spins = 0;
	status->delays = 0;
	status->cur_delay = 0;
	status->file = file;
	status->line = line;
	status->func = func;
}

#define init_local_spin_delay(status) init_spin_delay(status, __FILE__, __LINE__, __func__)

extern void perform_spin_delay(SpinDelayStatus *status);
extern void finish_spin_delay(SpinDelayStatus *status);
extern void set_spins_per_delay(int shared_spins_per_delay);
extern int	update_spins_per_delay(int shared_spins_per_delay);

#endif							/* SPIN_DELAY_STATUS_H */
