/*-------------------------------------------------------------------------
 *
 * poller.c
 *		Socket multiplexing and event poller abstraction for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/poller.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>
#include <sys/time.h>

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

#include "common/fe_memutils.h"
#include "common/logging.h"
#include "poller.h"

#ifdef POLL_USING_PPOLL

#define SOCKET_WAIT_METHOD_NAME "ppoll"

struct socket_set
{
	int			maxfds;			/* allocated length of pollfds[] array */
	int			curfds;			/* number currently in use */
	struct pollfd pollfds[FLEXIBLE_ARRAY_MEMBER];
};

const char *
socket_wait_method_name(void)
{
	return SOCKET_WAIT_METHOD_NAME;
}

socket_set *
alloc_socket_set(int count)
{
	socket_set *sa;

	sa = (socket_set *) pg_malloc0(offsetof(socket_set, pollfds) +
								   sizeof(struct pollfd) * count);
	sa->maxfds = count;
	sa->curfds = 0;
	return sa;
}

void
free_socket_set(socket_set *sa)
{
	pg_free(sa);
}

void
clear_socket_set(socket_set *sa)
{
	sa->curfds = 0;
}

void
add_socket_to_set(socket_set *sa, int fd, int idx)
{
	Assert(idx < sa->maxfds && idx == sa->curfds);
	sa->pollfds[idx].fd = fd;
	sa->pollfds[idx].events = POLLIN;
	sa->pollfds[idx].revents = 0;
	sa->curfds++;
}

int
wait_on_socket_set(socket_set *sa, int64 usecs)
{
	if (usecs > 0)
	{
		struct timespec timeout;

		timeout.tv_sec = usecs / 1000000;
		timeout.tv_nsec = (usecs % 1000000) * 1000;
		return ppoll(sa->pollfds, sa->curfds, &timeout, NULL);
	}
	else
	{
		return ppoll(sa->pollfds, sa->curfds, NULL, NULL);
	}
}

bool
socket_has_input(socket_set *sa, int fd, int idx)
{
	if (sa->curfds == 0)
		return false;

	Assert(idx < sa->curfds && sa->pollfds[idx].fd == fd);
	return (sa->pollfds[idx].revents & POLLIN) != 0;
}

#endif							/* POLL_USING_PPOLL */

#ifdef POLL_USING_SELECT

#define SOCKET_WAIT_METHOD_NAME "select"

struct socket_set
{
	int			maxfd;			/* largest FD currently set in fds */
	fd_set		fds;
};

const char *
socket_wait_method_name(void)
{
	return SOCKET_WAIT_METHOD_NAME;
}

socket_set *
alloc_socket_set(int count)
{
	return pg_malloc0_object(socket_set);
}

void
free_socket_set(socket_set *sa)
{
	pg_free(sa);
}

void
clear_socket_set(socket_set *sa)
{
	FD_ZERO(&sa->fds);
	sa->maxfd = -1;
}

void
add_socket_to_set(socket_set *sa, int fd, int idx)
{
#ifdef WIN32
	if (sa->fds.fd_count + 1 >= FD_SETSIZE)
	{
		pg_log_error("too many concurrent database clients for this platform: %d",
					 sa->fds.fd_count + 1);
		exit(1);
	}
#else
	if (fd < 0 || fd >= FD_SETSIZE)
	{
		pg_log_error("socket file descriptor out of range for select(): %d",
					 fd);
		pg_log_error_hint("Try fewer concurrent database clients.");
		exit(1);
	}
#endif
	FD_SET(fd, &sa->fds);
	if (fd > sa->maxfd)
		sa->maxfd = fd;
}

int
wait_on_socket_set(socket_set *sa, int64 usecs)
{
	if (usecs > 0)
	{
		struct timeval timeout;

		timeout.tv_sec = usecs / 1000000;
		timeout.tv_usec = usecs % 1000000;
		return select(sa->maxfd + 1, &sa->fds, NULL, NULL, &timeout);
	}
	else
	{
		return select(sa->maxfd + 1, &sa->fds, NULL, NULL, NULL);
	}
}

bool
socket_has_input(socket_set *sa, int fd, int idx)
{
	return (FD_ISSET(fd, &sa->fds) != 0);
}

#endif							/* POLL_USING_SELECT */
