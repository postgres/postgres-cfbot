/*--------------------------------------------------------------------------
 *
 * test_logicalmsg_hooks.c
 *		Code for testing LogicalRepMessageHandle_hook
 *
 * The handler records every logical decoding message it receives into the
 * table public.test_logicalmsg_log, so that tests can assert on the contents
 * of a table rather than on the server log. Recording the messages this way
 * also exercises the transactional behavior of the hook: a transactional
 * message is recorded as part of the remote transaction that emitted it, and
 * therefore disappears together with it if that transaction is rolled back or
 * skipped, while a non-transactional message is recorded independently.
 *
 * The handler creates that table itself on the first message, so a test does
 * not have to set it up. For a transactional message the creation is part of
 * the remote transaction too, and is rolled back with it, so the table does
 * not exist until some message has been applied successfully; a test must not
 * query it before then.
 *
 * The messages are also logged, which lets a test tell "the handler ran but
 * its work was rolled back" apart from "the handler was never called".
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_logicalmsg_hooks/test_logicalmsg_hooks.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "replication/logicalproto.h"
#include "replication/logicalworker.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

#define LOG_SCHEMA	"public"
#define LOG_TABLE	"test_logicalmsg_log"

static LogicalRepMessageHandle_hook_type prev_logical_message_handler = NULL;
static void test_logical_message_handler(const LogicalRepMessageData *msg);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	prev_logical_message_handler = LogicalRepMessageHandle_hook;
	LogicalRepMessageHandle_hook = &test_logical_message_handler;
}

/*
 * Create the log table if not exists.
 */
static void
ensure_log_table_exists(void)
{
	Oid			relid;
	int			ret;

	relid = get_relname_relid(LOG_TABLE, get_namespace_oid(LOG_SCHEMA, true));
	if (OidIsValid(relid))
		return;

	SPI_connect();

	ret = SPI_execute("CREATE TABLE " LOG_SCHEMA "." LOG_TABLE
					  "(lsn pg_lsn, transactional boolean, prefix text, message_size int, message text)",
					  false, 0);
	if (ret != SPI_OK_UTILITY)
		elog(ERROR, "could not create " LOG_SCHEMA "." LOG_TABLE);

	SPI_finish();
}

static void
test_logical_message_handler(const LogicalRepMessageData *msg)
{
	Oid			argtypes[5] = {LSNOID, BOOLOID, TEXTOID, INT4OID, TEXTOID};
	Datum		values[5];
	int			ret;

	/*
	 * Give a handler installed before ours its turn, so that several
	 * extensions can act on the same message.
	 */
	if (prev_logical_message_handler)
		(*prev_logical_message_handler) (msg);

	ereport(LOG,
			(errmsg("received message: LSN %X/%08X, prefix: %s, size: %zu, transactional: %d",
					LSN_FORMAT_ARGS(msg->lsn), msg->prefix,
					msg->message_size, msg->transactional)));

	ensure_log_table_exists();

	/*
	 * Pass the message as query parameters rather than interpolating it into
	 * the query text. Message contents come from the publisher and are not to
	 * be trusted, and the payload may contain characters that would need
	 * quoting. Note also that message_size, not strlen(), is authoritative
	 * for the payload length.
	 *
	 * The payload is recorded as text because the accompanying test only ever
	 * emits text messages. A handler must not assume that in general: a
	 * payload is an arbitrary string of bytes, which may contain embedded
	 * nulls and bytes that are not valid in the subscriber's encoding. See
	 * the comments on LogicalRepMessageHandle_hook_type.
	 */
	values[0] = LSNGetDatum(msg->lsn);
	values[1] = BoolGetDatum(msg->transactional);
	values[2] = CStringGetTextDatum(msg->prefix);
	values[3] = Int32GetDatum((int32) msg->message_size);
	values[4] = PointerGetDatum(cstring_to_text_with_len(msg->message,
														 msg->message_size));

	SPI_connect();

	/*
	 * The apply worker runs with an empty search_path, so the table name must
	 * be schema-qualified.
	 */
	ret = SPI_execute_with_args("INSERT INTO " LOG_SCHEMA "." LOG_TABLE
								" (lsn, transactional, prefix, message_size, message)"
								" VALUES ($1, $2, $3, $4, $5)",
								5, argtypes, values, NULL, false, 0);
	if (ret != SPI_OK_INSERT)
		elog(ERROR, "could not record logical message: SPI returned %d", ret);

	SPI_finish();
}
