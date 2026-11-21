/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "prepareas.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pgtypes_date.h>

#line 1 "regression.h"






#line 5 "prepareas.pgc"

/* exec sql whenever sqlerror  sqlprint ; */
#line 6 "prepareas.pgc"


static void
check_result_of_insert(void)
{
	/* exec sql begin declare section */
	      
	
#line 12 "prepareas.pgc"
 int ivar1 = 0 , ivar2 = 0 ;
/* exec sql end declare section */
#line 13 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select c1 , c2 from test", ECPGt_EOIT, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 15 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 15 "prepareas.pgc"

	printf("%d %d\n", ivar1, ivar2);
}

int main(void)
{
	/* exec sql begin declare section */
	      
	   
	    
	
#line 22 "prepareas.pgc"
 int ivar1 = 1 , ivar2 = 2 ;
 
#line 23 "prepareas.pgc"
 short v_ind = - 1 ;
 
#line 24 "prepareas.pgc"
 char v_include_dq_name [ 16 ] , v_include_ws_name [ 16 ] , v_normal_name [ 16 ] , v_query [ 64 ] ;
/* exec sql end declare section */
#line 25 "prepareas.pgc"


	strcpy(v_normal_name, "normal_name");
	strcpy(v_include_dq_name, "include_\"_name");
	strcpy(v_include_ws_name, "include_ _name");
	strcpy(v_query, "insert into test values(?,?)");

	/*
	 * preparing for test
	 */
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 35 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "begin");
#line 36 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "prepareas.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test ( c1 int , c2 int )", ECPGt_EOIT, ECPGt_EORT);
#line 37 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");
#line 38 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "begin");
#line 39 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "prepareas.pgc"


	/*
	 * Non dynamic statement
	 */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 44 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 44 "prepareas.pgc"

	printf("+++++ Test for prepnormal +++++\n");
	printf("insert into test values(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 47 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 47 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 50 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 50 "prepareas.pgc"

	printf("+++++ Test for execute immediate +++++\n");
	printf("execute immediate \"insert into test values(1,2)\"\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_immediate, "insert into test values(1,2)", ECPGt_EOIT, ECPGt_EORT);
#line 53 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 53 "prepareas.pgc"

	check_result_of_insert();

	/*
	 * PREPARE FROM
	 */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 59 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 59 "prepareas.pgc"

	printf("+++++ Test for PREPARE ident FROM CString +++++\n");
	printf("prepare ident_name from \"insert into test values(?,?)\"\n");
	{ ECPGprepare(__LINE__, NULL, 0, "ident_name", "insert into test values(?,?)");
#line 62 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 62 "prepareas.pgc"

	printf("execute ident_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "ident_name", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 64 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 64 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 67 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 67 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_normal_name FROM char_variable +++++\n");
	printf("prepare :v_normal_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_normal_name, v_query);
#line 70 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 70 "prepareas.pgc"

	printf("execute :v_normal_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, v_normal_name, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 72 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 72 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 75 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 75 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_inc_dq_name FROM char_variable +++++\n");
	printf("prepare :v_include_dq_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_include_dq_name, v_query);
#line 78 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 78 "prepareas.pgc"

	printf("execute :v_include_dq_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, v_include_dq_name, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 80 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 80 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 83 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 83 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_inc_ws_name FROM char_variable +++++\n");
	printf("prepare :v_include_ws_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_include_ws_name, v_query);
#line 86 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 86 "prepareas.pgc"

	printf("execute :v_include_ws_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, v_include_ws_name, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 88 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 88 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 91 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 91 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_inc_ws_name FROM char_variable +++++\n");
	printf("prepare \"include_ _name\" from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "include_ _name", v_query);
#line 94 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 94 "prepareas.pgc"

	printf("exec sql execute \"include_ _name\" using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "include_ _name", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 96 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 96 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 99 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 99 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_normal_name FROM char_variable +++++\n");
	printf("prepare \"norma_name\" from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "normal_name", v_query);
#line 102 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 102 "prepareas.pgc"

	printf("exec sql execute \"normal_name\" using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "normal_name", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 104 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 104 "prepareas.pgc"

	check_result_of_insert();

	/*
	 * PREPARE AS
	 */
	{ ECPGdeallocate(__LINE__, 0, NULL, "ident_name");
#line 110 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 110 "prepareas.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 111 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 111 "prepareas.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 112 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 112 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 114 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 114 "prepareas.pgc"

	printf("+++++ Test for PREPARE ident(typelist) AS +++++\n");
	printf("prepare ident_name(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_const,"ident_name",(long)10,(long)1,strlen("ident_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 117 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 117 "prepareas.pgc"

	printf("execute ident_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"ident_name",(long)10,(long)1,strlen("ident_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 119 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 119 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "ident_name");
#line 121 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 121 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 123 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 123 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_normal_name(typelist) AS +++++\n");
	printf("prepare \"normal_name\"(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_const,"normal_name",(long)11,(long)1,strlen("normal_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 126 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 126 "prepareas.pgc"

	printf("execute \"normal_name\"(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"normal_name",(long)11,(long)1,strlen("normal_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 128 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 128 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 130 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 130 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 132 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 132 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_include_ws_name(typelist) AS +++++\n");
	printf("prepare \"include_ _name\"(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_const,"include_ _name",(long)14,(long)1,strlen("include_ _name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 135 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 135 "prepareas.pgc"

	printf("execute \"include_ _name\"(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"include_ _name",(long)14,(long)1,strlen("include_ _name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 137 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 137 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 139 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 139 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 141 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 141 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_normal_name(typelist) AS +++++\n");
	printf("prepare :v_normal_name(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 144 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 144 "prepareas.pgc"

	printf("execute :v_normal_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 146 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 146 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 148 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 148 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 150 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 150 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_include_ws_name(typelist) AS +++++\n");
	printf("prepare :v_include_ws_name(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_char,(v_include_ws_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 153 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 153 "prepareas.pgc"

	printf("execute :v_include_ws_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_char,(v_include_ws_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 155 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 155 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 157 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 157 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 159 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 159 "prepareas.pgc"

	printf("+++++ Test for EXECUTE :v_normal_name(const,const) +++++\n");
	printf("prepare :v_normal_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_normal_name, v_query);
#line 162 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 162 "prepareas.pgc"

	printf("execute :v_normal_name(1,2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( 1 , 2 )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 164 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 164 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 166 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 166 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 168 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 168 "prepareas.pgc"

	printf("+++++ Test for EXECUTE :v_normal_name(expr,expr) +++++\n");
	printf("prepare :v_normal_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_normal_name, v_query);
#line 171 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 171 "prepareas.pgc"

	printf("execute :v_normal_name(0+1,1+1)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( 0 + 1 , 1 + 1 )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 173 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 173 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 175 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 175 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 177 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 177 "prepareas.pgc"

	printf("+++++ Test for combination PREPARE FROM and EXECUTE ident(typelist) +++++\n");
	printf("prepare ident_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "ident_name", v_query);
#line 180 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 180 "prepareas.pgc"

	printf("execute ident_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"ident_name",(long)10,(long)1,strlen("ident_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 182 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 182 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "ident_name");
#line 184 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 184 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 186 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 186 "prepareas.pgc"

	printf("+++++ Test for combination PREPARE FROM and EXECUTE CString_include_ws_name(typelist) +++++\n");
	printf("prepare \"include_ _name\" from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "include_ _name", v_query);
#line 189 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 189 "prepareas.pgc"

	printf("execute \"include_ _name\"(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"include_ _name",(long)14,(long)1,strlen("include_ _name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 191 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 191 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 193 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 193 "prepareas.pgc"


	/*
	 * A NULL host-variable value (negative indicator) passed to
	 * EXECUTE(exprlist) must be inserted as the SQL keyword NULL, not
	 * dereferenced as a null pointer.
	 */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 200 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 200 "prepareas.pgc"

	printf("+++++ Test for a NULL value in EXECUTE(exprlist) +++++\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_const,"stmt_null",(long)9,(long)1,strlen("stmt_null"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 202 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 202 "prepareas.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"stmt_null",(long)9,(long)1,strlen("stmt_null"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_short,&(v_ind),(long)1,(long)1,sizeof(short), 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 203 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 203 "prepareas.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from test where c1 is null", ECPGt_EOIT, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 204 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 204 "prepareas.pgc"

	printf("rows with NULL c1 (must be 1) = %d\n", ivar1);
	{ ECPGdeallocate(__LINE__, 0, NULL, "stmt_null");
#line 206 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 206 "prepareas.pgc"


	/*
	 * A value that contains a quote or $n (or ?, with -r questionmarks) must
	 * not hide the placeholder after it or be taken for one.  Every value is
	 * inserted as a quoted literal.  Placeholders from $10 up must be
	 * replaced whole.  All rows must be stored as given.
	 */
	{
		/* exec sql begin declare section */
		 
		 
		
#line 216 "prepareas.pgc"
  struct bytea_1  { int len; char arr[ 4 ]; }  v_bytea ;
 
#line 217 "prepareas.pgc"
 char v_literal [ 16 ] ;
/* exec sql end declare section */
#line 218 "prepareas.pgc"


		printf("+++++ Test for a quoted value before another value in EXECUTE(exprlist) +++++\n");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test_pos ( a text , b int )", ECPGt_EOIT, ECPGt_EORT);
#line 221 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 221 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( bytea , int ) as insert into test_pos values ( encode ( $1 , 'escape' ) , $2 )", 
	ECPGt_const,"stmt_pos_b",(long)10,(long)1,strlen("stmt_pos_b"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 222 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 222 "prepareas.pgc"

		v_bytea.len = 2;
		memcpy(v_bytea.arr, "ab", 2);
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"stmt_pos_b",(long)10,(long)1,strlen("stmt_pos_b"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(v_bytea),(long)4,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 225 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 225 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( text , int ) as insert into test_pos values ( $1 , $2 )", 
	ECPGt_const,"stmt_pos_t",(long)10,(long)1,strlen("stmt_pos_t"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 226 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 226 "prepareas.pgc"

		strcpy(v_literal, "cd");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"stmt_pos_t",(long)10,(long)1,strlen("stmt_pos_t"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(v_literal),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 228 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 228 "prepareas.pgc"

		strcpy(v_literal, "x$2");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"stmt_pos_t",(long)10,(long)1,strlen("stmt_pos_t"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(v_literal),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 230 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 230 "prepareas.pgc"

		strcpy(v_literal, "a?b");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"stmt_pos_t",(long)10,(long)1,strlen("stmt_pos_t"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(v_literal),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 232 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 232 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from test_pos where a in ( 'ab' , 'cd' , 'x$2' , 'a?b' ) and b = 2", ECPGt_EOIT, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 233 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 233 "prepareas.pgc"

		printf("rows stored (must be 4) = %d\n", ivar1);
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int , int , int , int , int , int , int , int , int , int ) as insert into test_pos values ( concat_ws ( ',' , $1 , $2 , $3 , $4 , $5 , $6 , $7 , $8 , $9 , $10 ) , $11 )", 
	ECPGt_const,"stmt_pos_11",(long)11,(long)1,strlen("stmt_pos_11"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 236 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 236 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  , $3  , $4  , $5  , $6  , $7  , $8  , $9  , $10  , $11  )", 
	ECPGt_const,"stmt_pos_11",(long)11,(long)1,strlen("stmt_pos_11"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 238 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 238 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from test_pos where a = '2,2,2,2,2,2,2,2,2,2' and b = 2", ECPGt_EOIT, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 239 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 239 "prepareas.pgc"

		printf("eleven values stored (must be 1) = %d\n", ivar1);
		{ ECPGdeallocate(__LINE__, 0, NULL, "stmt_pos_b");
#line 241 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 241 "prepareas.pgc"

		{ ECPGdeallocate(__LINE__, 0, NULL, "stmt_pos_t");
#line 242 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 242 "prepareas.pgc"

		{ ECPGdeallocate(__LINE__, 0, NULL, "stmt_pos_11");
#line 243 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 243 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test_pos", ECPGt_EOIT, ECPGt_EORT);
#line 244 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 244 "prepareas.pgc"

	}

	/*
	 * A character value containing SQL text must be stored as is, and the
	 * INSERT into test_marker in it must not run.  Boolean, date and NaN
	 * values, which can't be written as bare words, must be stored too.
	 */
	{
		/* exec sql begin declare section */
		 
		   
		 
		 
		
#line 254 "prepareas.pgc"
 char v_text [ 64 ] ;
 
#line 255 "prepareas.pgc"
 bool v_bool = true ;
 
#line 256 "prepareas.pgc"
 date v_date ;
 
#line 257 "prepareas.pgc"
 double v_double ;
/* exec sql end declare section */
#line 258 "prepareas.pgc"


		printf("+++++ Test for quoting values in EXECUTE(exprlist) +++++\n");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test_quote ( t text , b bool , d date , f float8 )", ECPGt_EOIT, ECPGt_EORT);
#line 261 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 261 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test_marker ( c int )", ECPGt_EOIT, ECPGt_EORT);
#line 262 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 262 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( text , bool , date , float8 ) as insert into test_quote values ( $1 , $2 , $3 , $4 )", 
	ECPGt_const,"stmt_q",(long)6,(long)1,strlen("stmt_q"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 263 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 263 "prepareas.pgc"

		v_date = PGTYPESdate_from_asc("2026-11-18", NULL);
		v_double = strtod("NaN", NULL);
		strcpy(v_text, "x'); insert into test_marker values(1); --");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  , $3  , $4  )", 
	ECPGt_const,"stmt_q",(long)6,(long)1,strlen("stmt_q"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(v_text),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(v_bool),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(v_date),(long)1,(long)1,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(v_double),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 267 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 267 "prepareas.pgc"

		strcpy(v_text, "O'Brien");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  , $3  , $4  )", 
	ECPGt_const,"stmt_q",(long)6,(long)1,strlen("stmt_q"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(v_text),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(v_bool),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(v_date),(long)1,(long)1,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(v_double),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 269 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 269 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from test_marker", ECPGt_EOIT, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 270 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 270 "prepareas.pgc"

		printf("rows in test_marker (must be 0) = %d\n", ivar1);
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select count ( * ) from test_quote where ( t = 'O''Brien' or t like 'x''); insert%' ) and b and d = '2026-11-18' and f = 'NaN'", ECPGt_EOIT, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 273 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 273 "prepareas.pgc"

		printf("rows stored (must be 2) = %d\n", ivar1);
		{ ECPGdeallocate(__LINE__, 0, NULL, "stmt_q");
#line 275 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 275 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test_quote", ECPGt_EOIT, ECPGt_EORT);
#line 276 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 276 "prepareas.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test_marker", ECPGt_EOIT, ECPGt_EORT);
#line 277 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 277 "prepareas.pgc"

	}

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 280 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 280 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");
#line 281 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 281 "prepareas.pgc"


	return 0;
}
