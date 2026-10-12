/*-------------------------------------------------------------------------
*
* pthread-win32.c
*	 partial pthread implementation for win32
*
* Copyright (c) 2004-2026, PostgreSQL Global Development Group
* IDENTIFICATION
*	src/interfaces/libpq/pthread-win32.c
*
*-------------------------------------------------------------------------
*/

#include "postgres_fe.h"

#include "pthread-win32.h"

DWORD
pthread_self(void)
{
	return GetCurrentThreadId();
}

void
pthread_setspecific(pthread_key_t key, void *val)
{
}

void *
pthread_getspecific(pthread_key_t key)
{
	return NULL;
}

int
pthread_mutex_init(pthread_mutex_t *mp, void *attr)
{
	mp->initstate = 0;
	return 0;
}

int
pthread_mutex_lock(pthread_mutex_t *mp)
{
	/* Acquire the initialization state without a read-modify-write. */
#ifdef _MSC_VER
	if (ReadAcquire(&mp->initstate) != 1)
#else
	if (__atomic_load_n(&mp->initstate, __ATOMIC_ACQUIRE) != 1)
#endif
	{
		LONG		istate;

		while ((istate = InterlockedCompareExchange(&mp->initstate, 2, 0)) == 2)
			Sleep(0);			/* wait, another thread is doing this */
		if (istate == 0)
		{
			InitializeCriticalSection(&mp->csection);
			InterlockedExchange(&mp->initstate, 1);
		}
	}
	EnterCriticalSection(&mp->csection);
	return 0;
}

int
pthread_mutex_unlock(pthread_mutex_t *mp)
{
	/* The successful lock already acquired the initialized critical section. */
#ifdef _MSC_VER
	if (ReadNoFence(&mp->initstate) != 1)
#else
	if (__atomic_load_n(&mp->initstate, __ATOMIC_RELAXED) != 1)
#endif
		return EINVAL;
	LeaveCriticalSection(&mp->csection);
	return 0;
}
