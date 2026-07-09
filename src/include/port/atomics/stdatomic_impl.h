/*-------------------------------------------------------------------------
 *
 * stdatomic_impl.h
 *	  Atomic operations implementation using C11 stdatomic.h
 *
 * This file provides PostgreSQL atomic operations using the C11 standard
 * <stdatomic.h>. It is only included when USE_STDATOMIC_H is defined.
 *
 * The traditional platform-specific implementation (arch-*.h, generic-*.h,
 * fallback.h) remains available and is used when stdatomic.h is not
 * available or when explicitly requested via -Duse_stdatomic=no.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/atomics/stdatomic_impl.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef STDATOMIC_IMPL_H
#define STDATOMIC_IMPL_H

/*
 * Only include this file when USE_STDATOMIC_H is defined at build time.
 * This ensures the traditional implementation remains available as fallback.
 */
#ifdef USE_STDATOMIC_H

/*
 * C++ Compatibility
 *
 * C++11 through C++20 cannot include C's <stdatomic.h> directly. Instead,
 * we use C++11's <atomic> header and map to std::atomic types. C++23 allows
 * <stdatomic.h> but we handle older versions.
 *
 * This follows the approach from C++23 standard section 33.5.12 which
 * allows C and C++ atomic types to interoperate.
 */
#if defined(__cplusplus) && __cplusplus < 202302L
extern "C++"
{
#include <atomic>

	/* Map to C++ atomic types */
#define pg_atomic(T) std::atomic<T>

	/* Import memory order constants into global namespace */
	using std::memory_order_relaxed;
	using std::memory_order_acquire;
	using std::memory_order_release;
	using std::memory_order_acq_rel;
	using std::memory_order_seq_cst;
}
#else
/* C11 or C++23+: use standard <stdatomic.h> */
#include <stdatomic.h>
#define pg_atomic(T) _Atomic(T)
#endif

/*
 * Type Definitions
 *
 * We use C11 _Atomic qualifier (or C++ std::atomic) with PostgreSQL's
 * standard integer types. These types are compatible with atomic operations
 * and provide sequential consistency guarantees.
 *
 * Note: pg_atomic(T) is defined above and maps to either _Atomic(T) for C
 * or std::atomic<T> for C++.
 */
/*
 * pg_atomic_flag is backed by a full 32-bit word, not uint8.  On
 * architectures without byte-granular atomics (notably RISC-V, whose base
 * 'A' extension has only word/doubleword AMOs), a sub-word _Atomic(uint8)
 * RMW is emulated by the compiler as a load-reserved/store-conditional of
 * the *entire containing 32-bit word*.  Because slock_t (a pg_atomic_flag)
 * is frequently packed next to other fields, that word-wide RMW can clobber
 * the neighbouring bytes, corrupting adjacent shared state under contention.
 * Using a 32-bit word makes every flag operation a clean word-aligned AMO
 * that never touches neighbouring memory, matching the traditional
 * implementation, whose pg_atomic_flag is also 32 bits (generic.h).
 */
typedef pg_atomic(uint32) pg_atomic_flag;
typedef pg_atomic(uint8)  pg_atomic_uint8;
typedef pg_atomic(uint16) pg_atomic_uint16;
typedef pg_atomic(uint32) pg_atomic_uint32;
typedef pg_atomic(uint64) pg_atomic_uint64;

/*
 * PostgreSQL's atomics live in shared memory and are accessed from multiple
 * *processes*, not just threads.  A non-lock-free atomic implemented by the
 * compiler/runtime with a lock table (e.g. libatomic) uses process-local
 * locks, which would silently fail to provide atomicity across processes and
 * corrupt shared state.  Require genuine, always-lock-free support at compile
 * time for every width we use.  (ATOMIC_*_LOCK_FREE is 0 = never lock-free,
 * 1 = sometimes / not known at compile time, 2 = always lock-free.)
 *
 * The == 1 case is a real hazard, not just conservatism: e.g. 64-bit atomics
 * on a 32-bit target may be emulated by libatomic with a process-local lock,
 * which corrupts cross-process shared memory exactly as == 0 would.  So we
 * require == 2 everywhere the macro is trustworthy.
 *
 * The one exception is MSVC compiling as C: its <stdatomic.h> reports
 * ATOMIC_*_LOCK_FREE == 1 for every width even though the atomics are in fact
 * always lock-free on the hardware we target (in C++ mode the same MSVC
 * reports 2, and atomic_is_lock_free() returns true at run time).  We skip the
 * assertion only in that specific configuration rather than weakening it for
 * everyone.
 */
#if !(defined(_MSC_VER) && !defined(__cplusplus))
StaticAssertDecl(ATOMIC_CHAR_LOCK_FREE == 2, "8-bit atomics are not lock-free");
StaticAssertDecl(ATOMIC_SHORT_LOCK_FREE == 2, "16-bit atomics are not lock-free");
StaticAssertDecl(ATOMIC_INT_LOCK_FREE == 2, "32-bit atomics are not lock-free");
StaticAssertDecl(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics are not lock-free");
#endif

/*
 * Signal atomic operation support to atomics.h
 *
 * stdatomic.h provides native support for all atomic types on all platforms
 * it compiles on, so we unconditionally define these.  PG_HAVE_ATOMIC_U64_SIMULATION
 * is intentionally NOT defined -- stdatomic provides real 64-bit atomics.
 */
#define PG_HAVE_ATOMIC_U32_SUPPORT
#define PG_HAVE_ATOMIC_U64_SUPPORT

/*
 * 8-byte single-copy atomicity.
 *
 * All 64-bit architectures that support C11 stdatomic.h guarantee 8-byte
 * single-copy atomicity for aligned loads/stores.  This is used by bufmgr.c
 * and other performance-critical paths to avoid unnecessary atomic operations.
 */
#if SIZEOF_VOID_P >= 8
#define PG_HAVE_8BYTE_SINGLE_COPY_ATOMICITY
#endif

/*
 * Memory Barrier Implementation
 *
 * PostgreSQL's memory barriers must order *non-atomic, non-volatile* accesses
 * too, which a bare C11 atomic_thread_fence() does NOT guarantee against
 * compiler reordering: atomic_thread_fence only establishes ordering with
 * respect to atomic operations, so the compiler is still free to move plain
 * loads/stores across it.  We therefore pair each thread fence with an
 * explicit compiler barrier (atomic_signal_fence), matching the semantics of
 * the traditional implementation whose barriers also constrain plain memory
 * accesses.  Omitting the compiler barrier corrupts lock-free algorithms on
 * weak-memory architectures (observed as lost tuples in parallel hash join on
 * RISC-V, where the compiler reordered non-atomic hash-table stores across a
 * bare acquire/release fence).
 *
 * These are implementation functions (_impl suffix) mapped to the public API
 * by atomics.h.
 */
#define pg_compiler_barrier_impl()	atomic_signal_fence(memory_order_seq_cst)

static inline void
pg_memory_barrier_impl(void)
{
	atomic_signal_fence(memory_order_seq_cst);
	atomic_thread_fence(memory_order_seq_cst);
}

static inline void
pg_read_barrier_impl(void)
{
	atomic_signal_fence(memory_order_seq_cst);
	atomic_thread_fence(memory_order_acquire);
}

static inline void
pg_write_barrier_impl(void)
{
	atomic_signal_fence(memory_order_seq_cst);
	atomic_thread_fence(memory_order_release);
}

/*
 * Spin Delay Implementation
 *
 * On x86, a spinloop without a "pause" instruction can waste a lot of power
 * and slow down the adjacent hyperthread. On ARM64, ISB provides better
 * backoff than YIELD. This provides platform-specific spin delay hints.
 *
 * The implementation is in spin_delay.h which directly defines pg_spin_delay_impl(),
 * matching the pattern used by the traditional atomics implementation.
 */
#include "port/spin_delay.h"

/*
 * =================================================================
 * pg_atomic_flag Operations
 * =================================================================
 *
 * Atomic flag type used primarily for spinlocks.
 *
 * CRITICAL FLAG CONVENTION DIFFERENCE FROM TRADITIONAL IMPLEMENTATION:
 *
 * stdatomic.h implementation (this file):
 *   1 = unlocked, 0 = locked
 *
 * Traditional implementation (generic.h):
 *   0 = unlocked, 1 = locked
 *
 * This polarity difference is INTENTIONAL and internal to the implementation.
 * The public API behavior is identical in both implementations:
 * - pg_atomic_init_flag() initializes to unlocked state
 * - pg_atomic_test_set_flag() returns true if lock was successfully acquired
 * - pg_atomic_clear_flag() releases the lock
 *
 * The stdatomic.h convention (1=unlocked) is more natural for C11 atomics
 * and avoids double-negation logic in the implementation.
 */

/*
 * pg_atomic_init_flag_impl - Initialize atomic flag to unlocked state
 *
 * In the stdatomic.h implementation, we initialize to 1 (unlocked).
 */
static inline void
pg_atomic_init_flag_impl(volatile pg_atomic_flag *ptr)
{
	atomic_init(ptr, 1);  /* 1 = unlocked */
}

/*
 * pg_atomic_test_set_flag_impl - Try to acquire lock
 *
 * Atomically clears the flag (AND with 0) and returns previous value.
 * Returns true if we successfully acquired the lock (old value was 1,
 * meaning unlocked), false if already locked (old value was 0).
 *
 * Memory ordering: acquire (synchronizes-with prior release)
 */
static inline bool
pg_atomic_test_set_flag_impl(volatile pg_atomic_flag *ptr)
{
	/*
	 * AND flag with 0 to clear it (locked). If previous value was 1
	 * (unlocked), we succeeded in acquiring. If previous value was 0
	 * (already locked), we failed.
	 *
	 * Return true if we SUCCEEDED in acquiring the lock.
	 */
	return atomic_fetch_and_explicit(ptr, 0, memory_order_acquire) != 0;
}

/*
 * pg_atomic_unlocked_test_flag_impl - Test if flag is unlocked without acquiring
 *
 * Returns true if the flag is currently unlocked (value == 1).
 * Uses relaxed ordering since this is typically used for optimization hints.
 */
static inline bool
pg_atomic_unlocked_test_flag_impl(volatile pg_atomic_flag *ptr)
{
	return atomic_load_explicit(ptr, memory_order_relaxed) != 0;
}

/*
 * pg_atomic_clear_flag_impl - Release lock
 *
 * Sets flag to 1 (unlocked).
 * Memory ordering: release (makes prior writes visible)
 */
static inline void
pg_atomic_clear_flag_impl(volatile pg_atomic_flag *ptr)
{
	atomic_store_explicit(ptr, 1, memory_order_release);
}

/*
 * =================================================================
 * pg_atomic_uint32 Operations
 * =================================================================
 *
 * 32-bit atomic unsigned integer operations.
 * All operations use sequential consistency unless otherwise noted.
 */

/*
 * pg_atomic_init_u32_impl - Initialize atomic uint32
 *
 * This is not an atomic operation - must only be called during initialization
 * when no concurrent access is possible.
 */
static inline void
pg_atomic_init_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 val)
{
	atomic_init(ptr, val);
}

/*
 * pg_atomic_read_u32_impl - Atomically read uint32 value
 *
 * Memory ordering: sequentially consistent.  This is stronger than the
 * documented "no barrier semantics" of pg_atomic_read_u32(), and it is
 * deliberate: a bare memory_order_relaxed load reintroduces a real,
 * reproducible data-loss bug on weak-memory hardware.
 *
 * The traditional implementation reads through a volatile-qualified pointer.
 * A volatile access cannot be reordered *by the compiler* relative to the
 * surrounding code, so consumers that publish a pointer with a
 * barrier-carrying store and then read+dereference it get the dependent load
 * ordered for free.  A C11 memory_order_relaxed atomic load does not carry
 * that compiler-ordering property, and the parallel hash join relies on it:
 * ExecParallelHash reads a freshly published bucket pointer via
 * dsa_pointer_atomic_read() and then dereferences the tuple it points to.
 * With a relaxed load this was observed to lose a tuple on RISC-V --
 * join_hash "extremely_skewed" returns 19999 instead of 20000, intermittently
 * (reproduced here at roughly 1 run in 4 on real rv64 hardware).  seq_cst
 * restores correctness.
 *
 * The cost of the seq_cst *load* is not the ARM regression that was measured
 * for an earlier revision of this work: microbenchmarking on Graviton showed
 * an uncontended ldar is not measurably costlier than a plain ldr, and the
 * ~2% aarch64 read-only regression seen previously was traced to the seq_cst
 * *store* (STLR) in pg_atomic_write_u32, which is now relaxed (see below).
 * So the ordered read is kept for correctness while the write, which was the
 * actual measured cost, is not paid.
 *
 * A cleaner-looking alternative would be to leave this primitive relaxed
 * (matching the documented contract) and add the ordering only at the
 * specific hash-join consumer -- e.g. a pg_read_barrier() between the bucket
 * read and the tuple dereference in ExecParallelHashFirstTuple().  That was
 * tried and is NOT sufficient: on RISC-V join_hash still lost a tuple at the
 * same ~1-in-4 rate.  The ordering the volatile load provided is not local to
 * that one read -- the parallel hash join also reaches shared state through
 * the Barrier / condition-variable / LWLock machinery, all of which read via
 * pg_atomic_read_*() and relied on the same compiler-ordering.  Relaxing the
 * primitive loses it everywhere at once; re-establishing it consumer by
 * consumer would mean auditing many call sites across lmgr/ipc/executor.  So
 * the read stays seq_cst: it is the single change that restores, tree-wide,
 * the property the volatile load provided.
 */
static inline uint32
pg_atomic_read_u32_impl(volatile pg_atomic_uint32 *ptr)
{
	return atomic_load_explicit(ptr, memory_order_seq_cst);
}

/*
 * pg_atomic_write_u32_impl - Atomically write uint32 value
 *
 * Memory ordering: relaxed.  A plain atomic (non-torn) store matches the
 * traditional implementation's "no barrier" contract for pg_atomic_write_u32
 * and, on ARM64, compiles to a plain STR rather than STLR.  This matters:
 * benchmarking traced the ~2% aarch64 read-only regression of an earlier
 * seq_cst-everywhere revision to STLR store-side serialization on hot,
 * contended shared lines (LWLock state, buffer headers), not to the reads.
 * Making the write relaxed removes that cost.  Callers needing an ordered
 * write use pg_atomic_write_membarrier_u32() or an explicit barrier.
 */
static inline void
pg_atomic_write_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 val)
{
	atomic_store_explicit(ptr, val, memory_order_relaxed);
}

/*
 * pg_atomic_exchange_u32_impl - Atomically exchange uint32 value
 *
 * Atomically replaces the value with newval and returns the old value.
 * Memory ordering: seq_cst
 */
static inline uint32
pg_atomic_exchange_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 newval)
{
	return atomic_exchange_explicit(ptr, newval, memory_order_seq_cst);
}

/*
 * pg_atomic_compare_exchange_u32_impl - Atomic compare-and-swap
 *
 * Compares *ptr with *expected. If equal, replaces *ptr with newval and
 * returns true. If not equal, updates *expected with current *ptr value
 * and returns false.
 *
 * This is the strong version (no spurious failures).
 * Memory ordering: seq_cst for both success and failure
 */
static inline bool
pg_atomic_compare_exchange_u32_impl(volatile pg_atomic_uint32 *ptr,
									uint32 *expected, uint32 newval)
{
	return atomic_compare_exchange_strong_explicit(ptr, expected, newval,
												   memory_order_seq_cst,
												   memory_order_seq_cst);
}

/*
 * pg_atomic_fetch_add_u32_impl - Atomic fetch-and-add
 *
 * Atomically adds add_ to *ptr and returns the old value.
 * Memory ordering: seq_cst
 */
static inline uint32
pg_atomic_fetch_add_u32_impl(volatile pg_atomic_uint32 *ptr, int32 add_)
{
	return atomic_fetch_add_explicit(ptr, add_, memory_order_seq_cst);
}

/*
 * pg_atomic_fetch_sub_u32_impl - Atomic fetch-and-subtract
 *
 * Atomically subtracts sub_ from *ptr and returns the old value.
 * Memory ordering: seq_cst
 */
static inline uint32
pg_atomic_fetch_sub_u32_impl(volatile pg_atomic_uint32 *ptr, int32 sub_)
{
	return atomic_fetch_sub_explicit(ptr, sub_, memory_order_seq_cst);
}

/*
 * pg_atomic_fetch_and_u32_impl - Atomic fetch-and-AND
 *
 * Atomically performs *ptr &= and_ and returns the old value.
 * Memory ordering: seq_cst
 */
static inline uint32
pg_atomic_fetch_and_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 and_)
{
	return atomic_fetch_and_explicit(ptr, and_, memory_order_seq_cst);
}

/*
 * pg_atomic_fetch_or_u32_impl - Atomic fetch-and-OR
 *
 * Atomically performs *ptr |= or_ and returns the old value.
 * Memory ordering: seq_cst
 */
static inline uint32
pg_atomic_fetch_or_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 or_)
{
	return atomic_fetch_or_explicit(ptr, or_, memory_order_seq_cst);
}

/*
 * pg_atomic_add_fetch_u32_impl - Atomic add-and-fetch
 *
 * Atomically adds add_ to *ptr and returns the NEW value.
 * Implemented using fetch_add + add_ since C11 doesn't have add_fetch.
 */
static inline uint32
pg_atomic_add_fetch_u32_impl(volatile pg_atomic_uint32 *ptr, int32 add_)
{
	return atomic_fetch_add_explicit(ptr, add_, memory_order_seq_cst) + add_;
}

/*
 * pg_atomic_sub_fetch_u32_impl - Atomic subtract-and-fetch
 *
 * Atomically subtracts sub_ from *ptr and returns the NEW value.
 */
static inline uint32
pg_atomic_sub_fetch_u32_impl(volatile pg_atomic_uint32 *ptr, int32 sub_)
{
	return atomic_fetch_sub_explicit(ptr, sub_, memory_order_seq_cst) - sub_;
}

/*
 * pg_atomic_read_membarrier_u32_impl - Read with full memory barrier
 *
 * Atomic read with sequential consistency. Useful for cases where
 * correctness is more important than performance.
 */
static inline uint32
pg_atomic_read_membarrier_u32_impl(volatile pg_atomic_uint32 *ptr)
{
	return atomic_load_explicit(ptr, memory_order_seq_cst);
}

/*
 * pg_atomic_unlocked_write_u32_impl - Non-atomic write
 *
 * Write without atomicity guarantees. Only safe when exclusive access
 * is guaranteed externally. Uses relaxed ordering.
 */
static inline void
pg_atomic_unlocked_write_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 val)
{
	atomic_store_explicit(ptr, val, memory_order_relaxed);
}

/*
 * pg_atomic_write_membarrier_u32_impl - Write with full memory barrier
 *
 * Atomic write with sequential consistency.
 */
static inline void
pg_atomic_write_membarrier_u32_impl(volatile pg_atomic_uint32 *ptr, uint32 val)
{
	atomic_store_explicit(ptr, val, memory_order_seq_cst);
}

/*
 * =================================================================
 * pg_atomic_uint64 Operations
 * =================================================================
 *
 * 64-bit equivalents of the u32 operations above.  See the u32 comments
 * for API documentation; semantics and memory ordering are identical.
 */

static inline void
pg_atomic_init_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 val)
{
	atomic_init(ptr, val);
}

static inline uint64
pg_atomic_read_u64_impl(volatile pg_atomic_uint64 *ptr)
{
	return atomic_load_explicit(ptr, memory_order_seq_cst);
}

static inline void
pg_atomic_write_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 val)
{
	atomic_store_explicit(ptr, val, memory_order_relaxed);
}

static inline uint64
pg_atomic_exchange_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 newval)
{
	return atomic_exchange_explicit(ptr, newval, memory_order_seq_cst);
}

static inline bool
pg_atomic_compare_exchange_u64_impl(volatile pg_atomic_uint64 *ptr,
									uint64 *expected, uint64 newval)
{
	return atomic_compare_exchange_strong_explicit(ptr, expected, newval,
												   memory_order_seq_cst,
												   memory_order_seq_cst);
}

static inline uint64
pg_atomic_fetch_add_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	return atomic_fetch_add_explicit(ptr, add_, memory_order_seq_cst);
}

static inline uint64
pg_atomic_fetch_sub_u64_impl(volatile pg_atomic_uint64 *ptr, int64 sub_)
{
	return atomic_fetch_sub_explicit(ptr, sub_, memory_order_seq_cst);
}

static inline uint64
pg_atomic_fetch_and_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 and_)
{
	return atomic_fetch_and_explicit(ptr, and_, memory_order_seq_cst);
}

static inline uint64
pg_atomic_fetch_or_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 or_)
{
	return atomic_fetch_or_explicit(ptr, or_, memory_order_seq_cst);
}

static inline uint64
pg_atomic_add_fetch_u64_impl(volatile pg_atomic_uint64 *ptr, int64 add_)
{
	return atomic_fetch_add_explicit(ptr, add_, memory_order_seq_cst) + add_;
}

static inline uint64
pg_atomic_sub_fetch_u64_impl(volatile pg_atomic_uint64 *ptr, int64 sub_)
{
	return atomic_fetch_sub_explicit(ptr, sub_, memory_order_seq_cst) - sub_;
}

static inline uint64
pg_atomic_read_membarrier_u64_impl(volatile pg_atomic_uint64 *ptr)
{
	return atomic_load_explicit(ptr, memory_order_seq_cst);
}

static inline void
pg_atomic_unlocked_write_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 val)
{
	atomic_store_explicit(ptr, val, memory_order_relaxed);
}

static inline void
pg_atomic_write_membarrier_u64_impl(volatile pg_atomic_uint64 *ptr, uint64 val)
{
	atomic_store_explicit(ptr, val, memory_order_seq_cst);
}

#endif /* USE_STDATOMIC_H */

#endif /* STDATOMIC_IMPL_H */
