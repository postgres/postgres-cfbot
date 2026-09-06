/*-------------------------------------------------------------------------
 *
 * poller.h
 *		Socket multiplexing and event poller abstraction for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/poller.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_POLLER_H
#define PGBENCH_POLLER_H

#include "c.h"

/*
 * Opaque socket set / poller handle
 */
typedef struct socket_set socket_set;
typedef struct socket_set PgBenchPoller;

/*
 * Lifecycle & polling operations
 */
extern socket_set *alloc_socket_set(int count);
extern void free_socket_set(socket_set *sa);
extern void clear_socket_set(socket_set *sa);
extern void add_socket_to_set(socket_set *sa, int fd, int idx);
extern int wait_on_socket_set(socket_set *sa, int64 usecs);
extern bool socket_has_input(socket_set *sa, int fd, int idx);
extern const char *socket_wait_method_name(void);

#define SOCKET_WAIT_METHOD (socket_wait_method_name())

/*
 * Object-style aliases for PgBenchPoller
 */
#define poller_create(max_sockets) alloc_socket_set(max_sockets)
#define poller_destroy(p) free_socket_set(p)
#define poller_clear(p) clear_socket_set(p)
#define poller_add(p, fd, idx) add_socket_to_set(p, fd, idx)
#define poller_wait(p, usecs) wait_on_socket_set(p, usecs)
#define poller_has_input(p, fd, idx) socket_has_input(p, fd, idx)

#endif							/* PGBENCH_POLLER_H */
