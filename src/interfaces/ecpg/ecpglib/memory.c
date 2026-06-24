/* src/interfaces/ecpg/ecpglib/memory.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include "ecpgerrno.h"
#include "ecpglib.h"
#include "ecpglib_extern.h"
#include "ecpgtype.h"
#include "port/pg_threads.h"

void
ecpg_free(void *ptr)
{
	free(ptr);
}

char *
ecpg_alloc(long size, int lineno)
{
	char	   *new = (char *) calloc(1L, size);

	if (!new)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return new;
}

char *
ecpg_realloc(void *ptr, long size, int lineno)
{
	char	   *new = (char *) realloc(ptr, size);

	if (!new)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return new;
}

/*
 * Wrapper for strdup(), with NULL in input treated as a correct case.
 *
 * "alloc_failed" can be optionally specified by the caller to check for
 * allocation failures.  The caller is responsible for its initialization,
 * as ecpg_strdup() may be called repeatedly across multiple allocations.
 */
char *
ecpg_strdup(const char *string, int lineno, bool *alloc_failed)
{
	char	   *new;

	if (string == NULL)
		return NULL;

	new = strdup(string);
	if (!new)
	{
		if (alloc_failed)
			*alloc_failed = true;
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return new;
}

/* keep a list of memory we allocated for the user */
struct auto_mem
{
	void	   *pointer;
	struct auto_mem *next;
};

static thread_local struct auto_mem *auto_mem_thread_local;

char *
ecpg_auto_alloc(long size, int lineno)
{
	void	   *ptr = ecpg_alloc(size, lineno);

	if (!ptr)
		return NULL;

	if (!ecpg_add_mem(ptr, lineno))
	{
		ecpg_free(ptr);
		return NULL;
	}
	return ptr;
}

bool
ecpg_add_mem(void *ptr, int lineno)
{
	struct auto_mem *am = (struct auto_mem *) ecpg_alloc(sizeof(struct auto_mem), lineno);

	if (!am)
		return false;

	am->pointer = ptr;
	am->next = auto_mem_thread_local;
	auto_mem_thread_local = am;
	return true;
}

void
ECPGfree_auto_mem(void)
{
	/* free all memory we have allocated for the user */
	while (auto_mem_thread_local)
	{
		struct auto_mem *act = auto_mem_thread_local;
		struct auto_mem *next = act->next;

		ecpg_free(act->pointer);
		ecpg_free(act);
		auto_mem_thread_local = next;
	}
}

void
ecpg_clear_auto_mem(void)
{
	/* only free our own structure */
	while (auto_mem_thread_local)
	{
		struct auto_mem *act = auto_mem_thread_local;
		struct auto_mem *next = act->next;

		ecpg_free(act);
		auto_mem_thread_local = next;
	}
}
