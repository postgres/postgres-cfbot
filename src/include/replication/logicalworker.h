/*-------------------------------------------------------------------------
 *
 * logicalworker.h
 *	  Exports for logical replication workers.
 *
 * Portions Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALWORKER_H
#define LOGICALWORKER_H

#include <signal.h>

extern PGDLLIMPORT volatile sig_atomic_t ParallelApplyMessagePending;

/*
 * Forward-declared so that this header, which is included by several files
 * unrelated to logical replication, does not have to include logicalproto.h.
 * A handler that dereferences the struct must include that header itself.
 */
struct LogicalRepMessageData;

/*
 * Hook for extensions to handle logical decoding messages (see
 * pg_logical_emit_message()) received from the publisher. Called only when
 * the subscription has the "messages" option enabled.
 *
 * The handler runs in the apply worker, inside a transaction, with an active
 * snapshot pushed. For a transactional message the handler's work is part of
 * the remote transaction and commits with it; a non-transactional message is
 * committed as an independent unit. The same message may be delivered more
 * than once, so the handler must be idempotent.
 *
 * The payload is an arbitrary string of bytes. The WAL record makes no
 * distinction between the text and the bytea form of
 * pg_logical_emit_message(), and the bytes are transmitted without character
 * set conversion, so the payload may contain embedded nulls and bytes that
 * are not valid in the subscriber's encoding. Its length is given by
 * msg->message_size, not by strlen(); the buffer is null-terminated only for
 * convenience, and that null is not part of the payload.
 *
 * The handler always runs with the privileges of the subscription owner.
 * The payload is chosen by whoever called pg_logical_emit_message() on the
 * publisher, which by default any role may do, so a handler that acts on the
 * payload gives every role on the publisher the subscription owner's
 * privileges. A handler that needs less than that can lower them itself
 * with SwitchToUntrustedUser().
 *
 * Note that the apply worker runs with an empty search_path. An ERROR thrown
 * by the handler restarts the apply worker, or disables the subscription if
 * disable_on_error is set. Because the message is delivered again after the
 * restart, a handler cannot reject a message by throwing an error; that only
 * loops. A message the handler does not want must simply be ignored.
 */
typedef void (*LogicalRepMessageHandle_hook_type) (const struct LogicalRepMessageData *msg);
extern PGDLLIMPORT LogicalRepMessageHandle_hook_type LogicalRepMessageHandle_hook;

extern void ApplyWorkerMain(Datum main_arg);
extern void ParallelApplyWorkerMain(Datum main_arg);
extern void TableSyncWorkerMain(Datum main_arg);
extern void SequenceSyncWorkerMain(Datum main_arg);

extern bool IsLogicalWorker(void);
extern bool IsLogicalParallelApplyWorker(void);

extern void HandleParallelApplyMessageInterrupt(void);
extern void ProcessParallelApplyMessages(void);

extern void LogicalRepWorkersWakeupAtCommit(Oid subid);

extern void AtEOXact_LogicalRepWorkers(bool isCommit);

#endif							/* LOGICALWORKER_H */
