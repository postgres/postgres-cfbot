/*-------------------------------------------------------------------------
 *
 * pg_numa.c
 * 		Basic NUMA portability routines
 *
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_numa.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include <unistd.h>

#include "miscadmin.h"
#include "port/pg_numa.h"
#include "storage/pg_shmem.h"

int	numa_flags;

/*
 * At this point we provide support only for Linux thanks to libnuma, but in
 * future support for other platforms e.g. Win32 or FreeBSD might be possible
 * too. For Win32 NUMA APIs see
 * https://learn.microsoft.com/en-us/windows/win32/procthread/numa-support
 */
#ifdef USE_LIBNUMA

#include <numa.h>
#include <numaif.h>

/*
 * numa_move_pages() chunk size, has to be <= 16 to work around a kernel bug
 * in do_pages_stat() (chunked by DO_PAGES_STAT_CHUNK_NR). By using the same
 * chunk size, we make it work even on unfixed kernels.
 *
 * 64-bit system are not affected by the bug, and so use much larger chunks.
 */
#if SIZEOF_SIZE_T == 4
#define NUMA_QUERY_CHUNK_SIZE 16
#else
#define NUMA_QUERY_CHUNK_SIZE 1024
#endif

/* libnuma requires initialization as per numa(3) on Linux */
int
pg_numa_init(void)
{
	int			r;

	/*
	 * XXX libnuma versions before 2.0.19 don't handle EPERM by disabling
	 * NUMA, which then leads to unexpected failures later. This affects
	 * containers that disable get_mempolicy by a seccomp profile.
	 */
	if (get_mempolicy(NULL, NULL, 0, 0, 0) < 0 && (errno == EPERM))
		r = -1;
	else
		r = numa_available();

	return r;
}

/*
 * We use move_pages(2) syscall here - instead of get_mempolicy(2) - as the
 * first one allows us to batch and query about many memory pages in one single
 * giant system call that is way faster.
 *
 * We call numa_move_pages() for smaller chunks of the whole array. The first
 * reason is to work around a kernel bug, but also to allow interrupting the
 * query between the calls (for many pointers processing the whole array can
 * take a lot of time).
 */
int
pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status)
{
	unsigned long next = 0;
	int			ret = 0;

	/*
	 * Chunk pointers passed to numa_move_pages to NUMA_QUERY_CHUNK_SIZE
	 * items, to work around a kernel bug in do_pages_stat().
	 */
	while (next < count)
	{
		unsigned long count_chunk = Min(count - next,
										NUMA_QUERY_CHUNK_SIZE);

#ifndef FRONTEND
		CHECK_FOR_INTERRUPTS();
#endif

		/*
		 * Bail out if any of the chunks errors out (ret<0). We ignore (ret>0)
		 * which is used to return number of nonmigrated pages, but we're not
		 * migrating any pages here.
		 */
		ret = numa_move_pages(pid, count_chunk, &pages[next], NULL, &status[next], 0);
		if (ret < 0)
		{
			/* plain error, return as is */
			return ret;
		}

		next += count_chunk;
	}

	/* should have consumed the input array exactly */
	Assert(next == count);

	return 0;
}

int
pg_numa_get_max_node(void)
{
	return numa_max_node();
}

/*
 * pg_numa_move_to_node
 *		move memory to different NUMA nodes in larger chunks
 *
 * startptr - start of the region (should be aligned to page size)
 * endptr - end of the region (doesn't need to be aligned)
 * node - node to move the memory to
 *
 * The "startptr" is expected to be a multiple of system memory page size, as
 * determined by pg_numa_page_size.
 *
 * XXX We only expect to do this during startup, when the shared memory is
 * still being setup.
 */
void
pg_numa_move_to_node(char *startptr, char *endptr, int node)
{
	Size		sz = (endptr - startptr);

	Assert((int64) startptr % pg_numa_page_size() == 0);

	/*
	 * numa_tonode_memory does not actually cause a page fault, and thus does
	 * not locate the memory on the node. So it's fast, at least compared to
	 * pg_numa_query_pages, and does not make startup longer. But it also
	 * means the expensive part happen later, on the first access.
	 */
	numa_tonode_memory(startptr, sz, node);
}

int
pg_numa_bind_to_node(char *startptr, char *endptr, int node)
{
	int				ret;
	struct bitmask *nodemask;

	if (node < 0)
	{
		errno = EINVAL;
		return -1;
	}

	nodemask = numa_allocate_nodemask();
	if (nodemask == NULL)
	{
		errno = ENOMEM;
		return -1;
	}

	numa_bitmask_setbit(nodemask, node);

	/*
	 * MPOL_BIND places the pages strictly on the node, and MPOL_MF_MOVE migrates
	 * pages already faulted in to that node. If mbind() fails, leave the default
	 * placement in effect, and report the failure.
	 */
	ret = mbind(startptr, (endptr - startptr),
				MPOL_BIND, nodemask->maskp, node, MPOL_MF_MOVE);

	numa_free_nodemask(nodemask);

	return ret;
}

Size
pg_numa_page_size(void)
{
	Size		os_page_size;
	Size		huge_page_size;

#ifdef WIN32
	SYSTEM_INFO sysinfo;

	GetSystemInfo(&sysinfo);
	os_page_size = sysinfo.dwPageSize;
#else
	os_page_size = sysconf(_SC_PAGESIZE);
#endif

	/* assume huge pages get used, unless HUGE_PAGES_OFF */
	if (huge_pages_status != HUGE_PAGES_OFF)
		GetHugePageSize(&huge_page_size, NULL);
	else
		huge_page_size = 0;

	return Max(os_page_size, huge_page_size);
}

#else

/* Empty wrappers */
int
pg_numa_init(void)
{
	/* We state that NUMA is not available */
	return -1;
}

int
pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status)
{
	return 0;
}

int
pg_numa_get_max_node(void)
{
	return 0;
}

void
pg_numa_move_to_node(char *startptr, char *endptr, int node)
{
	/* we don't expect to ever get here in builds without libnuma */
	Assert(false);
}

int
pg_numa_bind_to_node(char *startptr, char *endptr, int node)
{
	/* we don't expect to ever get here in builds without libnuma */
	Assert(false);
}

Size
pg_numa_page_size(void)
{
	/* we don't expect to ever get here in builds without libnuma */
	Assert(false);
}

#endif
