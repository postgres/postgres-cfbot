/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "descriptor.pgc"
#include "port/pg_threads.h"

#define THREADS		16
#define REPEATS		50000


#line 1 "sqlca.h"
#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifndef PGDLLIMPORT
#if  defined(WIN32) || defined(__CYGWIN__)
#define PGDLLIMPORT __declspec (dllimport)
#else
#define PGDLLIMPORT
#endif							/* __CYGWIN__ */
#endif							/* PGDLLIMPORT */

#define SQLERRMC_LEN	150

#ifdef __cplusplus
extern "C"
{
#endif

struct sqlca_t
{
	char		sqlcaid[8];
	long		sqlabc;
	long		sqlcode;
	struct
	{
		int			sqlerrml;
		char		sqlerrmc[SQLERRMC_LEN];
	}			sqlerrm;
	char		sqlerrp[8];
	long		sqlerrd[6];
	/* Element 0: empty						*/
	/* 1: OID of processed tuple if applicable			*/
	/* 2: number of rows processed				*/
	/* after an INSERT, UPDATE or				*/
	/* DELETE statement					*/
	/* 3: empty						*/
	/* 4: empty						*/
	/* 5: empty						*/
	char		sqlwarn[8];
	/* Element 0: set to 'W' if at least one other is 'W'	*/
	/* 1: if 'W' at least one character string		*/
	/* value was truncated when it was			*/
	/* stored into a host variable.             */

	/*
	 * 2: if 'W' a (hopefully) non-fatal notice occurred
	 */	/* 3: empty */
	/* 4: empty						*/
	/* 5: empty						*/
	/* 6: empty						*/
	/* 7: empty						*/

	char		sqlstate[5];
};

struct sqlca_t *ECPGget_sqlca(void);

#ifndef POSTGRES_ECPG_INTERNAL
#define sqlca (*ECPGget_sqlca())
#endif

#ifdef __cplusplus
}
#endif

#endif

#line 6 "descriptor.pgc"

/* exec sql whenever sqlerror  sqlprint ; */
#line 7 "descriptor.pgc"

/* exec sql whenever not found  sqlprint ; */
#line 8 "descriptor.pgc"


static int fn(void* arg)
{
	int i;

	for (i = 1; i <= REPEATS; ++i)
	{
		ECPGallocate_desc(__LINE__, "mydesc");
#line 16 "descriptor.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 16 "descriptor.pgc"

		ECPGdeallocate_desc(__LINE__, "mydesc");
#line 17 "descriptor.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 17 "descriptor.pgc"

	}

	return 0;
}

int main(void)
{
	int i;
	pg_thrd_t threads[THREADS];

	for (i = 0; i < THREADS; ++i)
		pg_thrd_create(&threads[i], fn, NULL);
	for (i = 0; i < THREADS; ++i)
		pg_thrd_join(threads[i], NULL);

	return 0;
}
