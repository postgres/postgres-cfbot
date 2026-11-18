/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/dest.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - initialize the destination at start of command
 *		CreateDestReceiver - create tuple receiver object for destination
 *		EndCommand - clean up the destination at end of command
 *		NullCommand - tell dest that an empty query string was recognized
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *	 NOTES
 *		These routines do the appropriate work before and after
 *		tuples are returned by a query to keep the backend and the
 *		"destination" portals synchronized.
 */

#include "postgres.h"

#include "access/printsimple.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/explain_dr.h"
#include "commands/matview.h"
#include "commands/prepare.h"
#include "executor/functions.h"
#include "executor/tqueue.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "utils/guc_hooks.h"
#include "utils/portal.h"

/* GUC variable */
int			ready_for_query_message = READY_FOR_QUERY_PLAIN;

/* Hook for ReadyForQuery */
ready_for_query_hook_type ready_for_query_hook = NULL;


/* ----------------
 *		dummy DestReceiver functions
 * ----------------
 */
static bool
donothingReceive(TupleTableSlot *slot, DestReceiver *self)
{
	return true;
}

static void
donothingStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

static void
donothingCleanup(DestReceiver *self)
{
	/* this is used for both shutdown and destroy methods */
}

/* ----------------
 *		static DestReceiver structs for dest types needing no local state
 * ----------------
 */
static const DestReceiver donothingDR = {
	donothingReceive, donothingStartup, donothingCleanup, donothingCleanup,
	DestNone
};

static const DestReceiver debugtupDR = {
	debugtup, debugStartup, donothingCleanup, donothingCleanup,
	DestDebug
};

static const DestReceiver printsimpleDR = {
	printsimple, printsimple_startup, donothingCleanup, donothingCleanup,
	DestRemoteSimple
};

static const DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
	DestSPI
};

/*
 * Globally available receiver for DestNone.
 *
 * It's ok to cast the constness away as any modification of the none receiver
 * would be a bug (which gets easier to catch this way).
 */
DestReceiver *None_Receiver = unconstify_constexpr(DestReceiver *, &donothingDR);

/* ----------------
 *		BeginCommand - initialize the destination at start of command
 * ----------------
 */
void
BeginCommand(CommandTag commandTag, CommandDest dest)
{
	/* Nothing to do at present */
}

/* ----------------
 *		CreateDestReceiver - return appropriate receiver function set for dest
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest)
{
	/*
	 * It's ok to cast the constness away as any modification of the none
	 * receiver would be a bug (which gets easier to catch this way).
	 */

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			return printtup_create_DR(dest);

		case DestRemoteSimple:
			return unconstify(DestReceiver *, &printsimpleDR);

		case DestNone:
			return unconstify(DestReceiver *, &donothingDR);

		case DestDebug:
			return unconstify(DestReceiver *, &debugtupDR);

		case DestSPI:
			return unconstify(DestReceiver *, &spi_printtupDR);

		case DestTuplestore:
			return CreateTuplestoreDestReceiver();

		case DestIntoRel:
			return CreateIntoRelDestReceiver(NULL);

		case DestCopyOut:
			return CreateCopyDestReceiver();

		case DestSQLFunction:
			return CreateSQLFunctionDestReceiver();

		case DestTransientRel:
			return CreateTransientRelDestReceiver(InvalidOid);

		case DestTupleQueue:
			return CreateTupleQueueDestReceiver(NULL);

		case DestExplainSerialize:
			return CreateExplainSerializeDestReceiver(NULL);
	}

	/* should never get here */
	pg_unreachable();
}

/* ----------------
 *		EndCommand - clean up the destination at end of command
 * ----------------
 */

void
EndCommandExtended(const QueryCompletion *qc, CommandDest dest,
				   bool force_undecorated_output, bool noblock)
{
	char		completionTag[COMPLETION_TAG_BUFSIZE];
	Size		len;

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			len = BuildQueryCompletionString(completionTag, qc,
											 force_undecorated_output);
			if (noblock)
				pq_putmessage_noblock(PqMsg_CommandComplete, completionTag, len + 1);
			else
				pq_putmessage(PqMsg_CommandComplete, completionTag, len + 1);
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

void
EndCommand(const QueryCompletion *qc, CommandDest dest, bool force_undecorated_output)
{
	EndCommandExtended(qc, dest, force_undecorated_output, false);
}

/* ----------------
 *		EndReplicationCommand - stripped down version of EndCommand
 *
 *		For use by replication commands.
 * ----------------
 */
void
EndReplicationCommand(const char *commandTag)
{
	pq_putmessage(PqMsg_CommandComplete, commandTag, strlen(commandTag) + 1);
}

/* ----------------
 *		NullCommand - tell dest that an empty query string was recognized
 *
 *		This ensures that there will be a recognizable end to the response
 *		to an Execute message in the extended query protocol.
 * ----------------
 */
void
NullCommand(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			/* Tell the FE that we saw an empty query string */
			pq_putemptymessage(PqMsg_EmptyQueryResponse);
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

/*
 * ready_for_query_has_key
 *		Check if the specified key is already present in the ReadyForQuery message buffer.
 */
bool
ready_for_query_has_key(StringInfo buf, const char *key, uint8 key_len)
{
	int			offset = 1;		/* Skip the initial 1-byte TxStatus indicator */

	while (offset < buf->len)
	{
		uint8		klen = (uint8) buf->data[offset++];
		uint8		vlen;

		if (offset + klen > buf->len)
			break;

		if (klen == key_len && memcmp(&buf->data[offset], key, key_len) == 0)
			return true;

		offset += klen;
		if (offset >= buf->len)
			break;

		vlen = (uint8) buf->data[offset++];

		offset += vlen;
	}

	return false;
}

/*
 * ready_for_query_append_kv
 *		Append a key-value pair to the ReadyForQuery message buffer.
 */
void
ready_for_query_append_kv(StringInfo buf,
						  const char *key, uint8 key_len,
						  const char *val, uint8 val_len)
{
	pq_sendbyte(buf, key_len);
	pq_sendbytes(buf, key, key_len);
	pq_sendbyte(buf, val_len);
	if (val_len > 0)
		pq_sendbytes(buf, val, val_len);
}

/*
 * append_builtin_ready_for_query_status
 *		Append built-in single-character session status indicators to ReadyForQuery buffer.
 */
static void
append_builtin_ready_for_query_status(StringInfo buf)
{
	bool		has_temp;
	bool		has_cursors;
	bool		has_prepared;
	uint64		lsn_nbo;

	/* 1. Temporary tables ('T') */
	has_temp = HasSessionTempTables();
	ready_for_query_append_kv(buf, "T", 1, has_temp ? "1" : "0", 1);

	/* 2. With-hold cursors ('H') */
	has_cursors = HasActiveWithHoldCursors();
	ready_for_query_append_kv(buf, "H", 1, has_cursors ? "1" : "0", 1);

	/* 3. Prepared statements ('P') */
	has_prepared = HasActivePreparedStatements();
	ready_for_query_append_kv(buf, "P", 1, has_prepared ? "1" : "0", 1);

	/* 4. Last Commit LSN ('l') as 8-byte uint64 in network byte order */
	lsn_nbo = pg_hton64((uint64) XactLastCommitEnd);
	ready_for_query_append_kv(buf, "l", 1, (char *) &lsn_nbo, sizeof(lsn_nbo));
}

/*
 * assign_ready_for_query_message - GUC assign hook for ready_for_query_message
 */
void
assign_ready_for_query_message(int newval, void *extra)
{
	if (newval == READY_FOR_QUERY_RICH && IsTransactionState())
		CheckSessionTempTables();
}

/* ----------------
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *		The ReadyForQuery message is sent so that the FE can tell when
 *		we are done processing a query string.
 *		In versions 3.0 and up, it also carries a transaction state indicator.
 *
 *		Note that by flushing the stdio buffer here, we can avoid doing it
 *		most other places and thus reduce the number of separate packets sent.
 * ----------------
 */
void
ReadyForQuery(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:
			{
				StringInfoData buf;

				pq_beginmessage(&buf, PqMsg_ReadyForQuery);
				pq_sendbyte(&buf, TransactionBlockStatusCode());

				if (ready_for_query_message == READY_FOR_QUERY_RICH)
				{
					append_builtin_ready_for_query_status(&buf);

					if (ready_for_query_hook != NULL)
						ready_for_query_hook(&buf);
				}

				pq_endmessage(&buf);
			}
			/* Flush output at end of cycle in any case. */
			pq_flush();
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}
