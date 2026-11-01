/*-------------------------------------------------------------------------
 *
 * poller.h
 *	  Socket set polling abstraction for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/poller.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLLER_H
#define POLLER_H

/* For testing, PGBENCH_USE_SELECT can be defined to force use of that code */
#if defined(HAVE_PPOLL) && !defined(PGBENCH_USE_SELECT)
#define POLL_USING_PPOLL
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#else							/* no ppoll(), so use select() */
#define POLL_USING_SELECT
#include <sys/select.h>
#endif

/*
 * Multi-platform socket set implementations
 */

#ifdef POLL_USING_PPOLL
#define SOCKET_WAIT_METHOD "ppoll"

typedef struct socket_set
{
	int			maxfds;			/* allocated length of pollfds[] array */
	int			curfds;			/* number currently in use */
	struct pollfd pollfds[FLEXIBLE_ARRAY_MEMBER];
} socket_set;

#endif							/* POLL_USING_PPOLL */

#ifdef POLL_USING_SELECT
#define SOCKET_WAIT_METHOD "select"

typedef struct socket_set
{
	int			maxfd;			/* largest FD currently set in fds */
	fd_set		fds;
} socket_set;

#endif							/* POLL_USING_SELECT */

extern socket_set *alloc_socket_set(int count);
extern void free_socket_set(socket_set *sa);
extern void clear_socket_set(socket_set *sa);
extern void add_socket_to_set(socket_set *sa, int fd, int idx);
extern int	wait_on_socket_set(socket_set *sa, int64 usecs);
extern bool socket_has_input(socket_set *sa, int fd, int idx);

#endif							/* POLLER_H */
