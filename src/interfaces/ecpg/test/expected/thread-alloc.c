/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "alloc.pgc"
#include <stdint.h>
#include <stdlib.h>
#include "ecpg_config.h"
#include "port/pg_threads.h"

#define THREADS		16
#define REPEATS		50


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

#line 9 "alloc.pgc"


#line 1 "regression.h"






#line 10 "alloc.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 12 "alloc.pgc"

/* exec sql whenever not found  sqlprint ; */
#line 13 "alloc.pgc"


static int fn(void* arg)
{
	int i;

	/* exec sql begin declare section */
	  
	 
	   
	
#line 20 "alloc.pgc"
 int value ;
 
#line 21 "alloc.pgc"
 char name [ 100 ] ;
 
#line 22 "alloc.pgc"
 char ** r = NULL ;
/* exec sql end declare section */
#line 23 "alloc.pgc"


	value = (intptr_t) arg;
	sprintf(name, "Connection: %d", value);

	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , name, 0); 
#line 28 "alloc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 28 "alloc.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 29 "alloc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "alloc.pgc"

	for (i = 1; i <= REPEATS; ++i)
	{
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select relname from pg_class where relname = 'pg_class'", ECPGt_EOIT, 
	ECPGt_char,&(r),(long)0,(long)0,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 32 "alloc.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 32 "alloc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "alloc.pgc"

		free(r);
		r = NULL;
	}
	{ ECPGdisconnect(__LINE__, name);
#line 36 "alloc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "alloc.pgc"


	return 0;
}

int main(void)
{
	intptr_t i;
	pg_thrd_t threads[THREADS];

	for (i = 0; i < THREADS; ++i)
		pg_thrd_create(&threads[i], fn, (void *) i);
	for (i = 0; i < THREADS; ++i)
		pg_thrd_join(threads[i], NULL);

	return 0;
}
