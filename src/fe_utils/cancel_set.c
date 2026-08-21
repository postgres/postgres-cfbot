#include "c.h"

#include "fe_utils/cancel_set.h"
#include "lib/ilist.h"
#include "port/pg_threads.h"

typedef struct cancel_set_entry
{
	dlist_node	node;
	PGconn	   *connection;
	PGcancelConn *cancel;
} cancel_set_entry;

struct cancel_set
{
	pg_mtx_t	mutex;
	dlist_head	entries;
};

cancel_set *
cancel_set_alloc(void)
{
	cancel_set *cs;

	if (!(cs = malloc(sizeof(*cs))))
		return NULL;

	pg_mtx_init(&cs->mutex, pg_mtx_plain);
	dlist_init(&cs->entries);

	return cs;
}

void
cancel_set_free(cancel_set *cs)
{
	cancel_set_remove_all(cs);
	pg_mtx_destroy(&cs->mutex);
	free(cs);
}

/*
 * Add a connection to the set.  Returns false on failure.
 */
bool
cancel_set_add(cancel_set *cs, PGconn *connection)
{
	bool		result;
	cancel_set_entry *entry;
	PGcancelConn *cancel;

	cancel = PQcancelCreate(connection);
	if (cancel == NULL)
		return false;

	pg_mtx_lock(&cs->mutex);
	entry = malloc(sizeof(*entry));
	if (entry)
	{
		entry->connection = connection;
		entry->cancel = cancel;
		dlist_push_tail(&cs->entries, &entry->node);
		result = true;
	}
	else
	{
		PQcancelFinish(cancel);
		result = false;
	}
	pg_mtx_unlock(&cs->mutex);

	return result;
}

static int
cancel_set_scan(cancel_set *cs, PGconn *connection, bool cancel, bool remove)
{
	dlist_mutable_iter iter;
	int			count = 0;

	pg_mtx_lock(&cs->mutex);
	dlist_foreach_modify(iter, &cs->entries)
	{
		cancel_set_entry *entry;

		entry = dlist_container(cancel_set_entry, node, iter.cur);
		if (connection == NULL || entry->connection == connection)
		{
			count++;
			if (cancel)
			{
				/*
				 * XXX Pin these so they can't be removed yet, and then
				 * perform the network request without holding the lock, and
				 * then go back and unpin them?
				 *
				 * XXX Would it be possible to detect connections that match,
				 * and collect multiple cancel packets that we send together
				 * in one network request?
				 */
				PQcancelBlocking(entry->cancel);
			}
			if (remove)
			{
				dlist_delete(&entry->node);
				PQcancelFinish(entry->cancel);
				free(entry);
			}
			if (connection)
				break;
		}
	}
	pg_mtx_unlock(&cs->mutex);

	return count;
}

/* Cancel a connection, without removing it. */
bool
cancel_set_cancel(cancel_set *cs, PGconn *connection)
{
	return cancel_set_scan(cs, connection, true, false) != 0;
}

/* Cancel and remove a connection. */
bool
cancel_set_cancel_and_remove(cancel_set *cs, PGconn *connection)
{
	return cancel_set_scan(cs, connection, true, true) != 0;
}

/* Remove a connection from the set. */
bool
cancel_set_remove(cancel_set *cs, PGconn *connection)
{
	return cancel_set_scan(cs, connection, false, true) != 0;
}

/* Cancel all connections in the set, without removing them. */
int
cancel_set_cancel_all(cancel_set *cs)
{
	return cancel_set_scan(cs, NULL, true, false);
}

/* Cancel and remove all connections in the set. */
int
cancel_set_cancel_and_remove_all(cancel_set *cs)
{
	return cancel_set_scan(cs, NULL, true, true);
}

/* Remove all connections from the set. */
int
cancel_set_remove_all(cancel_set *cs)
{
	return cancel_set_scan(cs, NULL, false, true);
}
