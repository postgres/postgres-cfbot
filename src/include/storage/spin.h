/*-------------------------------------------------------------------------
 *
 * spin.h
 *	   API for spinlocks.
 *
 *
 *	The interface to spinlocks is defined by the typedef "slock_t" and
 *	these functions:
 *
 *	void SpinLockInit(volatile slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	void SpinLockAcquire(volatile slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *
 *	void SpinLockRelease(volatile slock_t *lock)
 *		Unlock a previously acquired lock.
 *
 *	Load and store operations in calling code are guaranteed not to be
 *	reordered with respect to these operations, because they include a
 *	compiler barrier.  (Before PostgreSQL 9.5, callers needed to use a
 *	volatile qualifier to access data protected by spinlocks.)
 *
 *	Keep in mind the coding rule that spinlocks must not be held for more
 *	than a few instructions.  In particular, we assume it is not possible
 *	for a CHECK_FOR_INTERRUPTS() to occur while holding a spinlock, and so
 *	it is not necessary to do HOLD/RESUME_INTERRUPTS() in these functions.
 *
 *	These functions are implemented in terms of hardware-dependent macros
 *	supplied by s_lock.h.  There is not currently any extra functionality
 *	added by this header, but there has been in the past and may someday
 *	be again.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/spin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_H
#define SPIN_H

#include "storage/s_lock.h"
#include "port/atomics.h"
#include "c.h"

static inline void
SpinLockInit(volatile slock_t *lock)
{
	S_INIT_LOCK(lock);
}

static inline void
SpinLockAcquire(volatile slock_t *lock)
{
	S_LOCK(lock);
}

static inline void
SpinLockRelease(volatile slock_t *lock)
{
	S_UNLOCK(lock);
}


/*
 * Load/store a field also guarded by *lock. when the platform can access *p
 * with the correct width atomically, the lock is not used.
 * On platforms with PG_HAVE_ATOMIC_U64_SIMULATION, 64-bit accesses use *lock.
 */

#define SLOCK_DEFINE_SCALAR_IMPL(bits) \
static inline uint##bits \
slock_read_uint##bits##_impl(volatile slock_t *lock, volatile uint##bits *p) \
{ \
	(void) lock; \
	AssertPointerAlignment(p, alignof(uint##bits)); \
	return pg_atomic_read_membarrier_u##bits((pg_atomic_uint##bits *) (p)); \
} \
static inline void \
slock_write_uint##bits##_impl(volatile slock_t *lock, volatile uint##bits *p, uint##bits v) \
{ \
	(void) lock; \
	AssertPointerAlignment(p, alignof(uint##bits)); \
	pg_atomic_write_membarrier_u##bits((pg_atomic_uint##bits *) (p), v); \
}

#define SLOCK_DEFINE_LOCKED_IMPL(bits) \
static inline uint##bits \
slock_read_uint##bits##_impl(volatile slock_t *lock, volatile uint##bits *p) \
{ \
	uint##bits	val; \
\
	SpinLockAcquire(lock); \
	val = *p; /* lock takes care of memory ordering */ \
	SpinLockRelease(lock); \
	return val; \
} \
static inline void \
slock_write_uint##bits##_impl(volatile slock_t *lock, volatile uint##bits *p, uint##bits v) \
{ \
	SpinLockAcquire(lock); \
	*p = v; /* lock takes care of memory ordering */ \
	SpinLockRelease(lock); \
}

#define SLOCK_SCALAR_READ(type, lock, p) \
	( \
		StaticAssertExpr(sizeof(*(p)) == sizeof(type), \
						 "slock_read_" #type " size mismatch"), \
		slock_read_##type##_impl((lock), (volatile type *) (p)))

#define SLOCK_SCALAR_WRITE(type, lock, p, v) \
	((void) ( \
		StaticAssertExpr(sizeof(*(p)) == sizeof(type), \
						 "slock_write_" #type " size mismatch"), \
		StaticAssertExpr(sizeof(v) == sizeof(type), \
						 "slock_write_" #type " size mismatch"), \
		slock_write_##type##_impl((lock), (volatile type *) (p), (type) (v))))

StaticAssertDecl(sizeof(pg_atomic_uint32) == sizeof(uint32),
				 "pg_atomic_uint32 must match uint32");
StaticAssertDecl(sizeof(Pointer) == SIZEOF_VOID_P,
				 "Pointer must match pointer size");
SLOCK_DEFINE_SCALAR_IMPL(32)

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
SLOCK_DEFINE_SCALAR_IMPL(64)
#else
SLOCK_DEFINE_LOCKED_IMPL(64)
#endif
/* Not attempting atomic operations for 8-bit and 16-bit types for now */
SLOCK_DEFINE_LOCKED_IMPL(8)
SLOCK_DEFINE_LOCKED_IMPL(16)

#define slock_read_uint8(lock, p) \
	SLOCK_SCALAR_READ(uint8, lock, p)
#define slock_write_uint8(lock, p, v) \
	SLOCK_SCALAR_WRITE(uint8, lock, p, v)
#define slock_read_uint16(lock, p) \
	SLOCK_SCALAR_READ(uint16, lock, p)
#define slock_write_uint16(lock, p, v) \
	SLOCK_SCALAR_WRITE(uint16, lock, p, v)
#define slock_read_uint32(lock, p) \
	SLOCK_SCALAR_READ(uint32, lock, p)
#define slock_write_uint32(lock, p, v) \
	SLOCK_SCALAR_WRITE(uint32, lock, p, v)
#define slock_read_uint64(lock, p) \
	SLOCK_SCALAR_READ(uint64, lock, p)
#define slock_write_uint64(lock, p, v) \
	SLOCK_SCALAR_WRITE(uint64, lock, p, v)

#if SIZEOF_VOID_P == 8
#define slock_read_ptr(lock, p) \
	((Pointer) (uintptr_t) SLOCK_SCALAR_READ(uint64, lock, (volatile uint64 *) (p)))
#define slock_write_ptr(lock, p, v) \
	SLOCK_SCALAR_WRITE(uint64, lock, (volatile uint64 *) (p), (uint64) (uintptr_t) (v))
#elif SIZEOF_VOID_P == 4
#define slock_read_ptr(lock, p) \
	((Pointer) (uintptr_t) SLOCK_SCALAR_READ(uint32, lock, (volatile uint32 *) (p)))
#define slock_write_ptr(lock, p, v) \
	SLOCK_SCALAR_WRITE(uint32, lock, (volatile uint32 *) (p), (uint32) (uintptr_t) (v))
#else
#error unsupported pointer size for slock_*_ptr
#endif

#undef SLOCK_DEFINE_SCALAR_IMPL
#undef SLOCK_DEFINE_LOCKED_IMPL
#endif							/* SPIN_H */
