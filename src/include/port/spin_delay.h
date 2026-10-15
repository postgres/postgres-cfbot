/*-------------------------------------------------------------------------
 *
 * spin_delay.h
 *	  Platform-specific spin delay for busy-wait loops
 *
 * This file provides pg_spin_delay(), a platform-optimized delay instruction
 * for use in spinlock contention loops. Different architectures have different
 * optimal instructions for indicating to the CPU that we're in a busy-wait.
 *
 * Key optimizations:
 * - x86/x86_64: PAUSE instruction (rep nop) reduces power and helps hyperthreads
 * - ARM64: ISB instruction, per the discussion thread below, backs off better
 *   than YIELD under heavy spinlock contention on high-core-count parts
 * - Windows ARM64: __isb() intrinsic (same rationale as ARM64 above)
 *
 * Discussion: https://postgr.es/m/1c2a29b8-5b1e-44f7-a871-71ec5fefc120%40app.fastmail.com
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/spin_delay.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPIN_DELAY_H
#define SPIN_DELAY_H

#ifdef _MSC_VER
/*
 * MSVC intrinsics used below (__isb, _mm_pause, _ReadWriteBarrier) are declared
 * in <intrin.h>, which also provides <emmintrin.h>'s _mm_pause.  Include it at
 * file scope rather than relying on another header having pulled it in first.
 */
#include <intrin.h>
#endif

/*
 * pg_spin_delay_impl - Execute a platform-specific CPU hint for spin-wait loops
 *
 * This function should be called inside busy-wait loops to:
 * 1. Reduce CPU power consumption during contention
 * 2. Improve performance of sibling hyperthreads
 * 3. Signal to the CPU that we're in a spin loop
 *
 * The implementation varies by platform to use the most efficient instruction.
 *
 * Note: This defines pg_spin_delay_impl() directly, matching the pattern used
 * by the traditional atomics implementation (arch-*.h files).
 */
#ifndef PG_HAVE_SPIN_DELAY
#define PG_HAVE_SPIN_DELAY
static inline void
pg_spin_delay_impl(void)
{
#if defined(__GNUC__) || defined(__INTEL_COMPILER)

	/*
	 * GCC and Intel compiler: use inline assembly for optimal instructions
	 */

#if defined(__i386__) || defined(__i386)
	/* x86 32-bit: PAUSE instruction (encoded as rep nop) */
	__asm__ __volatile__(" rep; nop \n");
#elif defined(__x86_64__)
	/* x86-64: PAUSE instruction */
	__asm__ __volatile__(" rep; nop \n");
#elif defined(__aarch64__)
	/*
	 * ARM64: ISB (Instruction Synchronization Barrier).  Per the discussion
	 * thread above, ISB backs off better than YIELD in spinlock loops on
	 * high-core-count ARM64 parts; it forces a pipeline flush that YIELD does
	 * not.
	 */
	__asm__ __volatile__(" isb; \n");
#elif defined(__arm__) || defined(__arm)
	/* ARM 32-bit: YIELD hint */
	__asm__ __volatile__(" yield; \n");
#else
	/*
	 * Other architectures: compiler barrier only
	 *
	 * A compiler barrier prevents the compiler from optimizing away the loop,
	 * even if we don't have an architecture-specific delay instruction.
	 */
	__asm__ __volatile__("":::"memory");
#endif

#elif defined(_MSC_VER)

	/*
	 * Microsoft Visual C++: use intrinsics (declared in <intrin.h>, included
	 * at file scope above).
	 */

#if defined(_M_ARM64) || defined(_M_ARM64EC)
	/*
	 * Windows ARM64: ISB via intrinsic, matching the GCC/Clang ARM64 path
	 * above.  _ARM64_BARRIER_SY is a full system barrier.
	 */
	__isb(_ARM64_BARRIER_SY);
#elif defined(_M_AMD64)
	/* Windows x86-64: _mm_pause() maps to the PAUSE instruction. */
	_mm_pause();
#elif defined(_M_IX86)
	/* Windows x86 32-bit: MASM syntax for PAUSE (rep nop). */
	__asm		rep nop;
#else
	/*
	 * Other Windows architectures: compiler barrier only, matching the
	 * unknown-compiler fallback below.
	 */
	_ReadWriteBarrier();
#endif

#else
	/*
	 * Unknown compiler: no-op with compiler barrier
	 *
	 * At minimum, we need to prevent the compiler from optimizing away the
	 * spin loop.
	 */
	(void) 0;
#endif
}
#endif							/* PG_HAVE_SPIN_DELAY */

/*
 * Public spin-delay macro for the stdatomic path.
 *
 * This is the public pg_spin_delay() entry point used by the stdatomic
 * spinlock path; it maps to the pg_spin_delay_impl() defined above.
 */
#ifndef pg_spin_delay
#define pg_spin_delay() pg_spin_delay_impl()
#endif

/*
 * Architectures where a plain load before the atomic exchange reduces
 * cache-coherency traffic under spinlock contention (test-and-test-and-set).
 *
 * Restricted to x86/x86_64: these are TSO and pg_atomic_read_u32() (seq_cst)
 * compiles to an ordinary load there, so the pre-read is genuinely cheap.  On
 * weakly-ordered targets a seq_cst load emits fence instructions, which would
 * defeat the point of a cheap pre-check, so they are intentionally omitted.
 */
#if defined(__i386__) || defined(__x86_64__) || \
	defined(_M_IX86) || defined(_M_AMD64)
#define PG_SPIN_TRY_RELAXED
#endif

#endif							/* SPIN_DELAY_H */
