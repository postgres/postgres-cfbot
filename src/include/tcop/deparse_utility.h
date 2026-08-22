/*-------------------------------------------------------------------------
 *
 * deparse_utility.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/deparse_utility.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEPARSE_UTILITY_H
#define DEPARSE_UTILITY_H

#include "access/attnum.h"
#include "catalog/objectaddress.h"
#include "nodes/nodes.h"
#include "utils/aclchk_internal.h"


/*
 * Support for keeping track of collected commands.
 */
typedef enum CollectedCommandType
{
	SCT_Simple,
	SCT_AlterTable,
	SCT_Grant,
	SCT_AlterOpFamily,
	SCT_AlterDefaultPrivileges,
	SCT_CreateOpClass,
	SCT_AlterTSConfig,
} CollectedCommandType;

/*
 * A partition's schema-qualified name, captured for the deparser before the
 * partition is dropped.  See CollectedATSubcmd.partition_sources.
 */
typedef struct CollectedPartitionName
{
	char	   *schemaname;
	char	   *objname;
} CollectedPartitionName;

/*
 * For ALTER TABLE commands, we keep a list of the subcommands therein.
 */
typedef struct CollectedATSubcmd
{
	ObjectAddress address;		/* affected column, constraint, index, ... */
	Node	   *parsetree;

	/*
	 * For ALTER COLUMN TYPE, the USING expression rendered to text at prep
	 * time (before any column it references can be dropped by a sibling
	 * subcommand), or NULL.  See EventTriggerCollectAlterColumnTypeUsing().
	 */
	char	   *using_text;

	/*
	 * For MERGE PARTITIONS / SPLIT PARTITION, the schema-qualified names of
	 * the source partition(s) the command drops, captured before the drop
	 * (they are gone by deparse time).  A list of CollectedPartitionName, or
	 * NIL.  See EventTriggerCollectMergeSplitSources().
	 */
	List	   *partition_sources;

	/*
	 * For MERGE PARTITIONS / SPLIT PARTITION, the OIDs of the partition(s)
	 * the command creates (the merged-into partition, or the split-off ones),
	 * captured at execution time.  These still exist at deparse time, so
	 * their name and bound are read from the catalog by OID -- unlike the raw
	 * parse node's names, that needs no search_path.  A list of Oid, or NIL.
	 * See EventTriggerCollectMergeSplitCreated().
	 */
	List	   *partition_created;
} CollectedATSubcmd;

typedef struct CollectedCommand
{
	CollectedCommandType type;

	bool		in_extension;
	Node	   *parsetree;

	union
	{
		/* most commands */
		struct
		{
			ObjectAddress address;
			ObjectAddress secondaryObject;
		}			simple;

		/* ALTER TABLE, and internal uses thereof */
		struct
		{
			Oid			objectId;
			Oid			classId;
			List	   *subcmds;
		}			alterTable;

		/* GRANT / REVOKE */
		struct
		{
			InternalGrant *istmt;
		}			grant;

		/* ALTER OPERATOR FAMILY */
		struct
		{
			ObjectAddress address;
			List	   *operators;
			List	   *procedures;
		}			opfam;

		/* CREATE OPERATOR CLASS */
		struct
		{
			ObjectAddress address;
			List	   *operators;
			List	   *procedures;
		}			createopc;

		/* ALTER TEXT SEARCH CONFIGURATION ADD/ALTER/DROP MAPPING */
		struct
		{
			ObjectAddress address;
			Oid		   *dictIds;
			int			ndicts;
		}			atscfg;

		/* ALTER DEFAULT PRIVILEGES */
		struct
		{
			ObjectType	objtype;
		}			defprivs;
	}			d;

	struct CollectedCommand *parent;	/* when nested */
} CollectedCommand;

#endif							/* DEPARSE_UTILITY_H */
