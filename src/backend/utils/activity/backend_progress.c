/* ----------
 * backend_progress.c
 *
 *	Command progress reporting infrastructure.
 *
 *	Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 *	src/backend/utils/activity/backend_progress.c
 * ----------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "access/xact.h"
#include "libpq/pqformat.h"
#include "storage/proc.h"
#include "utils/backend_progress.h"
#include "utils/backend_status.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

/*
 * Set this variable to false when progress monitoring would not work. The
 * obvious case is file_fdw, where a merge join can run two COPY commands at
 * the same time - in such a case, the concept of parent commands does not
 * help.
 */
bool		pgstat_track_progress = true;

/*
 * nestingLevel of the current transaction.
 */
static int xact_level = 0;

/*
 * Element of the parent command stack.
 *
 * XXX Is "outer" command more descriptive than "parent"?
 */
typedef struct PgBackendProgresElt
{
	PgBackendProgress progress;

	struct PgBackendProgresElt *parent;
} PgBackendProgresElt;

/*
 * Stack of outer commands.
 *
 * In the simple case, we only monitor the progress of a single command at a
 * time. However, some monitored commands may launch other commands
 * (sub-commands), which report their progress too. If we didn't care, the
 * sub-command would override the output values of the parent command with
 * values of its own, unrelated parameters.
 *
 * To avoid this, when the sub-command starts its monitoring, the progress of
 * the parent command is pushed onto this stack and only the sub-command is
 * monitored. Once the sub-command is done, monitoring of the parent command
 * is resumed.
 */
static PgBackendProgresElt *parent_commands = NULL;

/*-----------
 * pgstat_progress_start_command() -
 *
 * Set st_progress_command (and st_progress_command_target) in own backend
 * entry.  Also, zero-initialize st_progress_param array.
 *-----------
 */
void
pgstat_progress_start_command(ProgressCommandType cmdtype, Oid relid)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	MemoryContext oldcxt;
	PgBackendProgresElt *parent_new = NULL;
	bool	is_dummy = false;

	if (!beentry || !pgstat_track_activities || !pgstat_track_progress)
		return;

	/*
	 * Allocate the new stack element regardless we need it, as we cannot do
	 * that in a critical section below.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	parent_new = palloc_object(PgBackendProgresElt);
	MemoryContextSwitchTo(oldcxt);

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	/*
	 * If progress monitoring is already in progress, save the status of the
	 * parent command so that we can restore it later.
	 */
	if (beentry->st_progress.command != PROGRESS_COMMAND_INVALID)
	{
		parent_new->progress = beentry->st_progress;
		parent_new->parent = parent_commands;
		parent_commands = parent_new;

		/* Do not free this element below. */
		parent_new = NULL;

		/* Report the top-level command if this is the first subcommand. */
		if (beentry->st_progress_top.command == PROGRESS_COMMAND_INVALID)
			beentry->st_progress_top = beentry->st_progress;
	}

	/*
	 * Now check the stack to see if a command of the same type is already
	 * being monitored.
	 */
	for (PgBackendProgresElt *stack_elt = parent_commands; stack_elt != NULL;
		 stack_elt = stack_elt->parent)
	{
		if (stack_elt->progress.command == cmdtype)
		{
			/* Cannot really monitor the new command. */
			is_dummy = true;
			break;
		}
	}

	beentry->st_progress.command = cmdtype;
	beentry->st_progress.command_target = relid;
	for (int i = 0; i < PGSTAT_NUM_PROGRESS_PARAM; i++)
		beentry->st_progress.param[i] = 0;
	beentry->st_progress.xact_level = xact_level;
	beentry->st_progress.is_dummy = is_dummy;
	PGSTAT_END_WRITE_ACTIVITY(beentry);
	if (parent_new)
		pfree(parent_new);
}

/*-----------
 * pgstat_progress_update_param() -
 *
 * Update index'th member in st_progress_param[] of own backend entry.
 *-----------
 */
void
pgstat_progress_update_param(int index, int64 val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	Assert(index >= 0 && index < PGSTAT_NUM_PROGRESS_PARAM);

	if (!beentry || beentry->st_progress.is_dummy ||
		!pgstat_track_activities || !pgstat_track_progress)
		return;

	/* Sub-commands introduce special cases that need to be tested. */
	if (parent_commands)
		INJECTION_POINT("progress-update-param-nested", NULL);

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress.param[index] = val;
	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/*-----------
 * pgstat_progress_incr_param() -
 *
 * Increment index'th member in st_progress_param[] of own backend entry.
 *-----------
 */
void
pgstat_progress_incr_param(int index, int64 incr)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	Assert(index >= 0 && index < PGSTAT_NUM_PROGRESS_PARAM);

	if (!beentry || beentry->st_progress.is_dummy ||
		!pgstat_track_activities || !pgstat_track_progress)
		return;

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress.param[index] += incr;
	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/*-----------
 * pgstat_progress_parallel_incr_param() -
 *
 * A variant of pgstat_progress_incr_param to allow a worker to poke at
 * a leader to do an incremental progress update.
 *-----------
 */
void
pgstat_progress_parallel_incr_param(int index, int64 incr)
{
	/*
	 * Parallel workers notify a leader through a PqParallelMsg_Progress
	 * message to update progress, passing the progress index and incremented
	 * value. Leaders can just call pgstat_progress_incr_param directly.
	 */
	if (IsParallelWorker())
	{
		static StringInfoData progress_message;

		pq_beginmessage(&progress_message, PqParallelMsg_Progress);
		pq_sendint32(&progress_message, index);
		pq_sendint64(&progress_message, incr);
		pq_endmessage(&progress_message);
	}
	else
		pgstat_progress_incr_param(index, incr);
}

/*-----------
 * pgstat_progress_update_multi_param() -
 *
 * Update multiple members in st_progress_param[] of own backend entry.
 * This is atomic; readers won't see intermediate states.
 *-----------
 */
void
pgstat_progress_update_multi_param(int nparam, const int *index,
								   const int64 *val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	int			i;

	if (!beentry || beentry->st_progress.is_dummy ||
		!pgstat_track_activities || !pgstat_track_progress || nparam == 0)
		return;

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	for (i = 0; i < nparam; ++i)
	{
		Assert(index[i] >= 0 && index[i] < PGSTAT_NUM_PROGRESS_PARAM);

		beentry->st_progress.param[index[i]] = val[i];
	}

	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/*-----------
 * pgstat_progress_end_command() -
 *
 * Reset st_progress_command (and st_progress_command_target) in own backend
 * entry.  This signals the end of the command.
 *
 * If 'xact' is true, end reporting for all the commands being reported by the
 * current transaction.
 *-----------
 */
void
pgstat_progress_end_command(bool xact)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!beentry || !pgstat_track_activities || !pgstat_track_progress)
		return;

	if (beentry->st_progress.command == PROGRESS_COMMAND_INVALID)
		return;

	/* Sub-commands introduce special cases that need to be tested. */
	if (parent_commands)
		INJECTION_POINT("progress-end-command-nested", NULL);

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	while (true)
	{
		if (parent_commands)
		{
			PgBackendProgresElt *parent;

			/*
			 * If dealing with transaction end, do not touch commands started
			 * in parent transaction.
			 */
			if (xact && beentry->st_progress.xact_level < xact_level)
				break;

			parent = parent_commands;

			/* Restore the status of the parent command. */
			beentry->st_progress = parent->progress;

			parent_commands = parent->parent;
			pfree(parent);

			/*
			 * If only the top-level command remains in progress, clear the
			 * top command info to indicate that there is no command above it.
			 */
			if (parent_commands == NULL)
			{
				beentry->st_progress_top.command = PROGRESS_COMMAND_INVALID;
				beentry->st_progress_top.command_target = InvalidOid;
				beentry->st_progress_top.xact_level = 0;
				/* The top command should never become dummy, but be tidy. */
				beentry->st_progress_top.is_dummy = false;
			}
		}
		else
		{
			/*
			 * End the top-level command. (Unless it belongs to parent
			 * transaction.)
			 */
			if (!xact || beentry->st_progress.xact_level >= xact_level)
			{
				beentry->st_progress.command = PROGRESS_COMMAND_INVALID;
				beentry->st_progress.command_target = InvalidOid;
				beentry->st_progress.xact_level = 0;
				/* See above. */
				beentry->st_progress.is_dummy = false;
			}

			break;
		}

		/*
		 * If this is not the end of transaction, the user is only interested
		 * in ending the inner-most command.
		 */
		if (!xact)
			break;
	}
	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/* XXX Define single function and a macro for each kind of callback? */
void
AtStart_BackendProgress(int nestingLevel)
{
	xact_level = nestingLevel;
}

void
AtEOXact_BackendProgress(int nestingLevel)
{
	xact_level = nestingLevel;
}

void
AtSubStart_BackendProgress(int nestingLevel)
{
	xact_level = nestingLevel;
}

void
AtEOSubXact_BackendProgress(int nestingLevel)
{
	xact_level = nestingLevel;
}
