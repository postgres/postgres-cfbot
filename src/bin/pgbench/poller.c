/*-------------------------------------------------------------------------
 *
 * poller.c
 *	  Multi-platform socket set implementations for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/poller.c
 *
 *-------------------------------------------------------------------------
 */
#if defined(WIN32) && FD_SETSIZE < 1024
#error FD_SETSIZE needs to have been increased
#endif

#include "postgres_fe.h"

#include <signal.h>
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

#include "common/logging.h"
#include "poller.h"

/*
 * These functions provide an abstraction layer that hides the syscall
 * we use to wait for input on a set of sockets.
 *
 * Currently there are two implementations, based on ppoll(2) and select(2).
 * ppoll() is preferred where available due to its typically higher ceiling
 * on the number of usable sockets.  We do not use the more-widely-available
 * poll(2) because it only offers millisecond timeout resolution, which could
 * be problematic with high --rate settings.
 *
 * Function APIs:
 *
 * alloc_socket_set: allocate an empty socket set with room for up to
 *		"count" sockets.
 *
 * free_socket_set: deallocate a socket set.
 *
 * clear_socket_set: reset a socket set to empty.
 *
 * add_socket_to_set: add socket with indicated FD to slot "idx" in the
 *		socket set.  Slots must be filled in order, starting with 0.
 *
 * wait_on_socket_set: wait for input on any socket in set, or for timeout
 *		to expire.  timeout is measured in microseconds; 0 means wait forever.
 *		Returns result code of underlying syscall (>=0 if OK, else see errno).
 *
 * socket_has_input: after waiting, call this to see if given socket has
 *		input.  fd and idx parameters should match some previous call to
 *		add_socket_to_set.
 *
 * Note that wait_on_socket_set destructively modifies the state of the
 * socket set.  After checking for input, caller must apply clear_socket_set
 * and add_socket_to_set again before waiting again.
 */

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
	/*
	 * In some cases, threadRun will apply clear_socket_set and then try to
	 * apply socket_has_input anyway with arguments that it used before that,
	 * or might've used before that except that it exited its setup loop
	 * early.  Hence, if the socket set is empty, silently return false
	 * regardless of the parameters.  If it's not empty, we can Assert that
	 * the parameters match a previous call.
	 */
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
	/* See connect_slot() for background on this code. */
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
