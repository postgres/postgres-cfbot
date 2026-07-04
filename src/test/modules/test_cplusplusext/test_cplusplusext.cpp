/*--------------------------------------------------------------------------
 *
 * test_cplusplusext.cpp
 *		Test that PostgreSQL headers compile with a C++ compiler.
 *
 * This file is compiled with a C++ compiler to verify that PostgreSQL
 * headers remain compatible with C++ extensions.
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_cplusplusext/test_cplusplusext.cpp
 *
 * -------------------------------------------------------------------------
 */

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/hsearch.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_cplusplus_add);
}

StaticAssertDecl(sizeof(int32) == 4, "int32 should be 4 bytes");

/*
 * Simple function that returns the sum of two integers.  This verifies that
 * C++ extension modules can be loaded and called correctly at runtime.
 */
extern "C" Datum
test_cplusplus_add(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		b = PG_GETARG_INT32(1);
	RangeTblRef *node = makeNode(RangeTblRef);
	const RangeTblRef *nodec = node;
	RangeTblRef *copy = copyObject(nodec);
	List	   *list = list_make1(node);

	foreach_ptr(RangeTblRef, rtr, list)
	{
		(void) rtr;
	}

	foreach_node(RangeTblRef, rtr, list)
	{
		(void) rtr;
	}

	StaticAssertStmt(sizeof(int32) == 4, "int32 should be 4 bytes");
	(void) StaticAssertExpr(sizeof(int64) == 8, "int64 should be 8 bytes");

	list_free(list);
	pfree(node);
	pfree(copy);

	/* Test hash macros compile under C++ */
	{
		typedef struct
		{
			Oid			oid;
			int			data;
		} OidEntry;

		typedef struct
		{
			char		name[NAMEDATALEN];
			int			value;
		} NameEntry;

		HTAB	   *htab;

		StaticAssertStmt(!HASH_KEY_AS_STRING(OidEntry, oid),
						 "Oid key should use HASH_BLOBS");
		StaticAssertStmt(!HASH_TYPE_AS_STRING(Oid),
						 "Oid hashset should use HASH_BLOBS");
		StaticAssertStmt(HASH_KEY_AS_STRING(NameEntry, name),
						 "char[] key should use HASH_STRINGS");
		StaticAssertStmt(HASH_TYPE_AS_STRING(NameData),
						 "NameData should use HASH_STRINGS");

		htab = hash_make(OidEntry, oid, "C++ oid hash", 8);
		foreach_hash(OidEntry, myOidEntry, htab)
		{
			(void) myOidEntry;
		}
		hash_destroy(htab);

		htab = hash_make(NameEntry, name, "C++ name hash", 8);
		hash_destroy(htab);

		htab = hash_make(OidEntry, oid, "C++ cxt hash", 8,
						 .mcxt = CurrentMemoryContext);
		hash_destroy(htab);

		htab = hash_make(OidEntry, oid, "C++ fn hash", 8,
						 .hash = NULL, .match = NULL);
		hash_destroy(htab);

		htab = hashset_make(Oid, "C++ oid hashset", 8);
		hash_destroy(htab);
	}

	switch (a)
	{
		case 1:
			elog(DEBUG1, "1");
			pg_fallthrough;
		case 2:
			elog(DEBUG1, "2");
			break;
	}

	PG_RETURN_INT32(a + b);
}
