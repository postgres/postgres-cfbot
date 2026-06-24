/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "prep.pgc"
#include <stdint.h>
#include <stdlib.h>
#include "ecpg_config.h"
#include "port/pg_threads.h"

#include <stdio.h>

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

#line 11 "prep.pgc"


#line 1 "regression.h"






#line 12 "prep.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 14 "prep.pgc"

/* exec sql whenever not found  sqlprint ; */
#line 15 "prep.pgc"


static int fn(void* arg)
{
	int i;

	/* exec sql begin declare section */
	  
	 
	   
	
#line 22 "prep.pgc"
 int value ;
 
#line 23 "prep.pgc"
 char name [ 100 ] ;
 
#line 24 "prep.pgc"
 char query [ 256 ] = "INSERT INTO T VALUES ( ? )" ;
/* exec sql end declare section */
#line 25 "prep.pgc"


	value = (intptr_t) arg;
	sprintf(name, "Connection: %d", value);

	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , name, 0); 
#line 30 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "prep.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 31 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 31 "prep.pgc"

	for (i = 1; i <= REPEATS; ++i)
	{
		{ ECPGprepare(__LINE__, NULL, 0, "i", query);
#line 34 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 34 "prep.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "i", 
	ECPGt_int,&(value),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 35 "prep.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 35 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "prep.pgc"

	}
	{ ECPGdeallocate(__LINE__, 0, NULL, "i");
#line 37 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "prep.pgc"

	{ ECPGdisconnect(__LINE__, name);
#line 38 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "prep.pgc"


	return 0;
}

int main(void)
{
	intptr_t i;
	pg_thrd_t threads[THREADS];

	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 48 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 48 "prep.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 49 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 49 "prep.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table if exists T", ECPGt_EOIT, ECPGt_EORT);
#line 50 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 50 "prep.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table T ( i int )", ECPGt_EOIT, ECPGt_EORT);
#line 51 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 51 "prep.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 52 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 52 "prep.pgc"


	for (i = 0; i < THREADS; ++i)
		pg_thrd_create(&threads[i], fn, (void *) i);
	for (i = 0; i < THREADS; ++i)
		pg_thrd_join(threads[i], NULL);

	return 0;
}
